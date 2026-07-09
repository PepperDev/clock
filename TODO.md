# TODO

- [x] add `biased` flag to battery samples: first sample after state change is biased=true, next is biased=false. On sample completion (rollover or state change), if unbiased sample totals ≥60s, prune biased samples and skip future biased samples. Independent per direction.
- [x] visually indicate when a local IP matches its WAN counterpart: text mode `IP`/`IP6` → `WIP`/`WIP6`, ascii/sixel 📡 → 🌍; redundant WAN/WAN6 line omitted. Line order preserved (IPv4, IPv6, WAN, WAN6). IPv4 and IPv6 independent.
- [ ] `try_refetch_weather` must call `io_cancel_all(…, CANCEL_WEATHER)` before closing the weather connection fd, so the I/O thread does not poll on a recycled file descriptor.
- [ ] add assertion `slot->state == DNS_IDLE` in `start_dns` before overwriting slot state and thread handle.
- [x] guard CPU temp display so `-1°C` is not shown when the sensor is unavailable.
- [x] aggregate motherboard fans/temps across all hwmon dirs into one fan line and one temp line instead of per-dir blocks.
- [ ] change GPU temp sentinel so `0°C` (a valid reading) is not silently hidden as "no sensor".
- [x] **BUG (container):** echo not restored on exit in continuous ascii mode with TTY.
      **Root cause (trace12):** `struct clock_state c = { 0 }` zero-initializes `c.keep.nlk.fd = 0`. In
      container there is no wireless interface so `wlan_open_nlk` is never called and `nlk_init`
      never runs — `nlk.fd` stays 0. On exit, `cleanup_fds` in `main_term.c:123` calls
      `sys_close(c->keep.nlk.fd)` which closes **stdin (fd 0)**. The subsequent
      `tcsetattr(STDIN_FILENO, …)` at line 138 then gets **EBADF** because fd 0 is already closed.
      Terminal ECHO is never restored. **Fix:** initialise `nlk.fd = -1` in `init_clock` so
      `cleanup_fds` calls `sys_close(-1)` which is harmless.
- [x] **BUG (container):** NIC and local IPs not showing.
      **Root causes:**
      1. Link dump: `rtnl_dump` sent `RTM_GETLINK` with stack garbage and `struct rtgenmsg`
         (1 byte). Container kernel parsed garbage for `ifinfomsg` fields, filtering out
         non-loopback. **Fix:** zero-init buffer, use `struct ifinfomsg`, set
         `NETLINK_GET_STRICT_CHK=1`, and use `AF_UNSPEC` instead of `AF_PACKET`.
      2. Route metrics: Container kernel never emits `RTA_PRIORITY` in netlink route dumps.
         C code's `metric = ~0U` sentinel collided with `a->best = -1` sentinel in
         `update_best` — both are `~0U` as unsigned, so `metric < (unsigned)a->best` was
         always false. **Fix:** `def_route_cb` normalises missing metric to `0` (kernel
         default) before `update_best`.
- [x] **BUG (net_line):** phantom line due to missing null terminator in `fmt_net_line`.
      **Root cause:** `fmt_wired_lines` writes `\n` by `*(*p)++ = '\n'`, overwriting the null
      terminator that `fmt_into` placed. Next frame writes a shorter string, leaving tail
      bytes from the previous frame. The trailing `\n` in those tail bytes creates phantom
      lines via `cat_lines`. **Fix:** `*p = 0` at end of `fmt_net_line`.
- [x] remove flags `-a`/`-g`/`-f`, make GPU+FAN always-on in default widget set
- [ ] remove unused `sto_pct` field from `struct clock_state`
- [ ] remove dead `sto_temp` data flow (written to `clock_state` from disk.c, never consumed by display).
- [ ] add `wc > 0` guard in `sixel_emit_overlay` so the sidebar is not positioned when the widget list is empty (ascii path already has this guard).
- [ ] prepend 🌐 icon (`\xf0\x9f\x8c\x90`) to the first NIC line of the NET widget in TTY modes, matching the spec's icon column for NET.
- [x] truncate sidebar to terminal height `wsrow` (continuous loop mode only): when sidebar rows (after wrapping) exceed terminal rows, drop excess lines silently. `--once` mode is excluded — the sidebar always prints fully below the clock. Non-TTY paths are unaffected — they write to a pipe/file with no terminal rows constraint.
- [x] skip virtual mount types `fusectl`, `nsfs`, `binfmt_misc` in STO widget
- [x] **GHOST on resize (post-RIS scroll):** After shrink resize, `d->sidebar_lines` retained pre-resize count. `update_sidebar_lines` compared new truncated count against the stale large value and emitted `\033[K\n` × diff. These `\n` past the new `wsrow` scrolled the just-rendered frame up. **Fix:** reset `d->sidebar_lines = 0` after RIS in `check_resize` — screen is already blank, no clearing needed.
- [x] **GHOST CLOCK on resize:** SIGWINCH arriving during data gathering (async fetches) was not caught until the next tick, showing a one-frame ghost at the old clock position. **Fix:** move `check_resize` to after data gathering but before `render_and_wait` so a late SIGWINCH is processed in the same tick.
