---
name: performance-audit
description: Profile CPU, memory, and execution with perf, gprof, and valgrind to identify bottlenecks, cache misses, branch mispredictions, and memory leaks.
---

## When to use

When the user asks to profile, benchmark, optimize, or diagnose performance regressions, memory leaks, or excessive syscall overhead. Use the project's `DOMAIN.md`, `CONTRIBUTING.md` and `AGENTS.md` for core principles (lightweight, stack-preferred, deduplication, widget-scoped computation).

## Tools

### perf (Linux)

Collect CPU cycles, cache misses, branch mispredictions, context switches:

```
# build release
make -j$(nproc)

# CPU cycle sample (default)
perf record -g -F 99 bin/clock --all --once
perf report -g graph --sort=comm,dso,symbol

# Cache misses
perf stat -e cache-misses,cache-references,LLC-load-misses,LLC-store-misses bin/clock --all --once

# Branch mispredictions
perf stat -e branch-misses,branch-instructions bin/clock --all --once

# Syscall profile
perf stat -e context-switches,cpu-migrations,page-faults bin/clock --all --once

# S1: All widgets, once
perf stat -e cycles,instructions,cache-misses,branch-misses bin/clock --all --once

# S2: Each individual widget, once
for w in CPU GPU MEM FAN BAT UP STO NET WEATHER; do
  perf stat -e cycles,instructions,cache-misses,branch-misses \
    bin/clock --once --widgets "$w" 2>&1 | tail -1
done

# S3: Random combos, once — catch widget interaction costs
# Generate ten unique 2+ widget combinations, replace COMBO below
for i in 1 2 3 4 5 6 7 8 9 10; do
  perf stat -e cycles,instructions,cache-misses,branch-misses \
    bin/clock --once --widgets "CPU,STO" 2>&1 | tail -1
done

# S4: Live mode — all, each, and combos (timeout 5)
timeout 5 perf stat -e cycles,instructions,cache-misses,branch-misses \
  bin/clock --all
for w in CPU GPU MEM FAN BAT UP STO NET WEATHER; do
  timeout 5 perf stat -e cycles,instructions,cache-misses,branch-misses \
    bin/clock --widgets "$w" 2>&1 | tail -1
done
for i in 1 2 3 4 5 6 7 8 9 10; do
  timeout 5 perf stat -e cycles,instructions,cache-misses,branch-misses \
    bin/clock --widgets "CPU,STO" 2>&1 | tail -1
done
```

### gprof

Compile with `-pg`, run, then read profile:

```
# Build with profiling
make CFLAGS="-Wall -Wextra -pg" -j$(nproc)
bin/clock --all --once && gprof bin/clock gmon.out > /tmp/gprof.txt
less /tmp/gprof.txt
```

For per-widget profiles, instrument the build once then run each widget combo against the same binary (gprof accumulates, so run one at a time or reset `gmon.out` between runs).

### Valgrind / sanitizers

#### Memory leaks (valgrind)
```
valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
  bin/clock --all --once 2>&1 | tee /tmp/valgrind.log
```

#### AddressSanitizer (faster than valgrind, catches buffer overflows)
```
make CFLAGS="-fsanitize=address -fno-omit-frame-pointer" -j$(nproc)
bin/clock --all --once
```

#### UndefinedBehaviourSanitizer
```
make CFLAGS="-fsanitize=undefined -fno-omit-frame-pointer" -j$(nproc)
bin/clock --all --once
```

#### ThreadSanitizer (data races)
```
make CFLAGS="-fsanitize=thread -fno-omit-frame-pointer" -j$(nproc)
bin/clock -w "NET,WEATHER" --once
```

### Live mode profiling

For sustained profiling of the live loop (not `--once`), use a 5-second timeout — repeat the S1–S3 scenario matrix:

```
timeout 5 perf record -g -F 99 bin/clock --all
timeout 5 perf record -g -F 99 bin/clock --once --widgets CPU
timeout 5 valgrind --leak-check=full bin/clock --all
timeout 5 valgrind --leak-check=full bin/clock --once --widgets CPU,STO
```

## Analysis checklist

For each profile result, check:

1. **Hot functions**: which functions consume most CPU? Are they expected? Can they be cached or optimized?
2. **Cache misses**: high LLC misses may indicate poor data locality or oversized structs. Check `struct cpu_info` layout.
3. **Branch mispredictions**: high ratio in hot paths — simplify conditionals, reduce nesting.
4. **Syscall count**: excessive `open`/`close`/`read` per tick may violate §4 (deduplication). Compare against per-widget baselines and cross-reference with syscall-audit logs (same S1–S4 scenario labels).
5. **Memory leaks**: every `malloc`/`calloc`/`realloc` must have a paired `free`. Check valgrind `definitely lost` blocks.
6. **Widget isolation** (§6): S2 single-widget and S3 combo runs should not trigger syscalls for inactive widget data sources. Cross-reference with syscall audit findings.
7. **Stack vs heap**: are there heap allocations that could be replaced with stack buffers? (AGENTS.md: "Worship lightweight resource usage. Prefer stack memory.")
8. **Profile-guided optimization**: consider `-fprofile-generate`/`-fprofile-use` for hot paths if measurable gains justify build complexity.

## Presenting results

Group findings by principle violated. Always include file:line references where code causes the issue. Suggest concrete TODO.md tasks — do not modify code or docs unless asked.
