---
name: syscall-audit
description: Audit CONTRIBUTING.md §Design Principles 4 (deduplication) and 6 (widget-scoped computation) using strace -tt -f.
---

## Procedure

When the user requests a syscall audit:

1. Build release binary: `make -j$(nproc)`
2. Run strace on each scenario below, capturing to a log file
3. Analyse each log for violations of §4 (duplicate syscalls in one tick) and §6 (work outside active widget set)
4. Present violations and suggest TODO.md tasks to fix them — do not modify code or docs unless asked

## Scenarios

### S1: All widgets, once
```
strace -tt -f -o /tmp/strace-all-once.log bin/clock --all --once
```

### S2: Each individual widget, once
For each widget `w` in `DATE CPU GPU MEM FAN BAT UP STO NET WEATHER CAL`:
```
strace -tt -f -o /tmp/strace-{w}-once.log bin/clock --once --widgets {w}
```

### S3: Random combos, once
Generate ten random unique combinations of 2+ widgets (e.g. `CPU,MEM`, `NET,WEATHER,BAT`, etc.):
```
for i in 1 2 3 4 5 6 7 8 9 10; do
  strace -tt -f -o /tmp/strace-combo$i-once.log bin/clock --once --widgets "CPU,STO"
done
```
Replace `"CPU,STO"` with each combination.

### S4: Same as S1–S3, live (no `--once`, timeout 5)
Same combinations as S1–S3, but without `--once` and with a 5-second timeout.

**New in S4**: verify that any sysfs file that returned `ENOENT` on the first tick does not appear again in ticks 2, 3, … n, regardless of the syscall (`access`, `open`, `statx`, etc.). This checks that per-file presence flags (e.g. GPU `gpu_probe_features`) are working correctly — a file not found on first tick must not be probed again on subsequent ticks.
```
strace -tt -f -o /tmp/strace-all-live-5s.log timeout 5 bin/clock --all
```
```
strace -tt -f -o /tmp/strace-{w}-live-5s.log timeout 5 bin/clock --widgets {w}
```
```
strace -tt -f -o /tmp/strace-combo$i-live-5s.log timeout 5 bin/clock --widgets "CPU,STO"
```
**Note**: `timeout 5` goes after `strace` so strace wraps the application and traces the timeout signal delivery. Discard the last ~10 lines of each live log (the signal/exit noise).

## Analysis Checklist

For each log, check:

1. **§4 Duplicate syscalls in one tick**: same `open("/proc/..."`, `ioctl(SIOCETHTOOL)`, `socket(AF_NETLINK)`, `sendmsg` (RTM_GETROUTE/RTM_GETLINK) appearing twice within the same second.
2. **§6 Widget-scoped leakage**: syscalls for data sources of widgets NOT in the active set (e.g. `open("/sys/class/power_supply/...)` when BAT not in `--widgets`, or `open("/sys/class/drm/...")` when GPU not active).

**Suggest improvements**: after spotting duplicate or widget-leaked syscalls, identify opportunities where a once-per-tick result could be cached and reused in subsequent ticks without breaking hotplug or device dynamics (e.g. netlink dump results for stable interfaces, file descriptors kept open across ticks, or `stat`/`access` calls elided when device presence is already confirmed).

Present findings grouped by principle, with file + line references where code causes the violation.
