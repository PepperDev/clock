## Documentation Conventions

### Flow diagrams

When documenting flows and lifecycles, use these consistent conventions:
- Quotes (`"..."`) for user input or command-line arguments
- Backticks (`` `...` ``) for function names, syscalls, file paths, and variable names
- `→` for sequencing (e.g. `parse_input → validate → execute`)
- `⟳` for retry loops; `⏎` for return values cascading through the call chain
- Distinguish syscalls (`open`, `read`) from libc calls (`qsort`, `strlen`) from shell commands; use consistent formatting for each
- For async flows, use `fork`/`join` notation:
  ```
  main ──→ thread_A ──→ join
       └──→ thread_B ──→ join
  ```
- Align multi-line diagrams with consistent indentation (width 4)

### Pseudocode

Prefer pseudocode over real C when describing algorithms, business rules, and flow logic. Pseudocode keeps the spec concise and language-agnostic — it conveys intent without tying the reader to implementation details (variable names, error handling, syntax). Use these conventions:
- `→` for sequencing and data flow
- `if` / `else if` / `else` for conditionals
- `for each` / `while` for iteration
- `//` for inline commentary
- Reference named sections (e.g. "per §Retry rules") for sub-logic that is detailed elsewhere
- Keep it language-agnostic — no C-isms like `++`, `->`, or type declarations

## Design Invariants

- **Zero-leak principle**: every resource acquired must be released before the
  owning function returns.
- **Widget-scoped computation**: no code path outside the active feature set
  may execute. Data sources for widgets not in the active set are never opened
  or read.
- **Single TLS flag for signal handler**: the SIGINT/SIGTERM handler writes
  only one `__thread volatile sig_atomic_t tls_terminated` flag. On a
  second signal it calls `_exit(128+sig)` — no other TLS or global state
  is touched. All cleanup state (terminal attributes, I/O pointers, DNS
  slots) is stored in regular struct fields passed explicitly through the
  call chain. See §Cleanup path under Loop Mode.

  **`tls_terminated` is main-thread-only**: because it is `__thread`, each
  thread has its own copy. The signal handler writes to whichever thread
  happens to receive the signal (per-thread default signal delivery).
  Threads other than the main thread must never read `tls_terminated` —
  they check their own runtime state (e.g. `m->fd < 0`) to detect
  shutdown requests. Background threads that block on `poll()` retry on
  `EINTR` but check their fd-based termination condition before the
  retry, ensuring they exit promptly when the main thread closes the fd.

  Terminal state and cleanup pointers are passed explicitly through the
  call chain (e.g., as fields of `struct clock_state` or local variables
  in `clock_main`). The only `__thread` variable is a single termination
  flag:

  ```
  __thread volatile sig_atomic_t tls_terminated = 0;
  ```

  On a first SIGINT/SIGTERM the signal handler sets `tls_terminated = 1`
  and returns. On a second signal (flag already `1`) it calls
  `_exit(128+sig)` directly without touching any other state. The main
  loop checks `tls_terminated` on each iteration and breaks when set —
  both normal completion and signal-triggered exit converge to the same
  `cleanup_all()` call before `return` in `clock_main`. See §Cleanup
  path under Loop Mode.

  **Terminal setup is single-shot**: `setup_terminal` is called at most once
  per process invocation. It receives a mutable `enum mode *` — when the mode
  is `MODE_AUTO` it runs the DA1 query (temporarily disables ICANON/ECHO under
  its own save/restore cycle) to detect sixel capability and resolves the mode
  to sixel or ascii. Once resolved it saves the original termios (including the
  ECHO flag as-is), disables ECHO for both ascii and sixel, additionally
  disables ICANON for sixel mode, then runs the `\033[14t` pixel query to
  obtain terminal geometry (reuses the already-disabled ICANON — no separate
  termios manipulation needed). Finally it registers the signal handler with
  `sigaction`. The `da1_setup` helper in `sixel.c` is called only from within
  `setup_terminal`'s auto-mode branch, never from startup code. On exit
  `restore_termios` restores the saved `c_lflag` exactly — if the terminal
  had ECHO off when the program started, ECHO stays off. No `|= ECHO` override.

## CLI Arguments

```
clock [OPTIONS] [auto|text|ascii|sixel]
```

| Flag | Short | Value? | Description | Default |
|------|-------|--------|-------------|---------|
| `--help` | `-h` | no | Print usage summary to stdout and exit 0 | — |
| `--once` | `-o` | no | Print output once and exit. No 1-second loop. | — |
| `--sunday-start` | `-S` | no | Calendar week starts on Sunday instead of Monday. | Monday |
| `--widgets` | `-w` | `<list>` | Comma-separated widget names defining sidebar content and order. All widgets always-on in default mode. | *(default set)* |
| `--ip-refresh` | `-I` | `<sec>` | Refresh interval for public IP addresses in seconds. | 86400 (24h) |
| `--weather-refresh` | `-W` | `<sec>` | Refresh interval for weather data in seconds. | 1800 (30min) |

**Value format**: flags requiring a value accept either `=` in the same argument or the following argument:
- Long: `--widgets=NET` or `--widgets NET`
- Short: `-wNET` (immediately following) or `-w NET`

When a value-requiring flag is present, the value must be provided immediately — having a default value does not make the argument optional. The widget list is the **only** flag that accepts an **empty value**: `-w=`, `-w ''`, or bare `-w` when the next argument is another flag (starts with `-`) or a positional mode keyword (`auto`, `text`, `ascii`, `sixel`). An empty list disables the sidebar entirely (full-screen clock). All other value-requiring flags (`-I`, `-W`) must have a non-empty value or the program exits with an error.

Note: `--widgets auto` (long form, space-separated) produces an empty widget list with mode `auto` — the same as `-w auto`. `--widgets=auto` (equals form) fails validation because `auto` is not a valid widget name; the long-form parser does not skip mode keywords in the value position.

**Short grouping**: boolean flags may be combined after a single `-`, e.g. `-oS` ≡ `-o -S`. A value-requiring flag ends the group — the remaining characters are its value (e.g. `-owNET` ≡ `-o -w NET`). `-h` inside a short group terminates the group: prior flags are applied, the program prints usage and exits immediately.

**Positional mode**: `auto` (default), `text`, `ascii`, or `sixel`. If omitted, equivalent to `auto`. Mode and flags are order-independent.

Any unrecognized argument (flag or positional) causes the program to print `"Unknown argument: <arg>"` to stderr and exit with code 1.

## Buffer Strategy Audit

All data reads in this codebase go through the `syscall.c` wrapper layer (`sys_open`/`sys_read`/`sys_close`/`sys_recvmsg`/`sys_socket`) for testability via weak aliases. No file-based stdio (`fopen`/`fread`/`fclose`) or `mmap` is used. `sscanf` for in-memory string parsing is permitted.

| Read pattern | Used by | Strategy | Buffer |
|-------------|---------|----------|--------|
| **read_uint** — single numeric value from sysfs | cpu.c (freq), mem.c (cgroup limits), gpu.c (temp/freq/mem/fan), bat.c (charge_now/energy_now), fan.c (temp/rpm), disk.c (nvme temp) | `read_file` into 64-byte stack, `strtoull` | 64B stack |
| **read_file** — fixed-size string from kernel file | bat.c (type/status), fan.c (hwmon name), cpu.c (governor) | Single `sys_read` into caller buffer, null-terminated | caller-supplied (16–32B stack) |
| **read_file + sscanf** — small kernel files with multiple fields | disk.c (`block/*/size`, `block/*/dev`) | `read_file` into 128B stack buffer, `sscanf` for multi-field extraction | 128B stack |
| **slurp** — full-file dynamic alloc | gpu.c (`pp_dpm_sclk/mclk`), disk.c (`block/*/stat`) | Open + loop `sys_read` doubling `realloc`; free after parse | dynamic (starts 1024B) |
| **batch scan** — chunked filter/reduce | cpu.c (`/proc/stat`), mem.c (`/proc/meminfo`) | Open + loop `sys_read` into fixed buffer; extract with `sscanf`/`memcmp`/`memchr`; partial line carried across reads via `LineCarry` | 1024B stack |
| **chunked line scan** — incremental read + parse | mount.c (`/proc/self/mountinfo`) | Open + loop `sys_read` into fixed buffer, find newlines with `memchr`, parse each line independently into dynamic array of `struct mount`; partial line carried across reads via `LineCarry` | 1024B stack + dynamic struct array |
| **socket recv (netlink)** — rtnetlink/nl80211 dumps | rtnl.c, netlink_util.c, net_route.c | `sys_recvmsg` into fixed chunk buffer; dump loops re-read until `NLMSG_DONE`, kernel leaves overflow in socket buffer for next `recvmsg`. Individual netlink messages are bounded by `NLMSG_GOODSIZE` (~8KB) — 16KB is double the maximum, safe as a chunk size. | 16KB stack (16384B) |
| **socket recv (netlink event)** — async netlink monitor | net_route_mon.c (`rtnl_mon_thread`) | `poll()` → `ensure_buf` to current capacity → `recvmsg`; grows via `realloc` doubling when message exceeds capacity | dynamic (starts NLBUF=4096B) |
| **socket recv (HTTP)** — async HTTP responses | ioserv.c (`io_conn_recv`) | `recv` into dynamically-growing buffer; doubles on overflow | dynamic (starts 4096B) |
| **TTY read** — sixel DA1 and pixel query response | sixel.c (`da1_read`) | `poll` 10ms timeout + `read` into buffer | 64B stack (pixel query) / 128B stack (DA1 detect) |
| **eventfd read** — IO thread wake notification | ioserv.c | `read` from eventfd | 8B stack |
| **syscall** — `sysinfo` for loadavg/uptime | monitor.c, up.c | `sys_sysinfo` fills `struct sysinfo` at `cpu_keep.si` | struct |
| **resolver** — `getaddrinfo` for DNS | dns.c | libc DNS resolver, result stored in `struct addrinfo *` | libc-managed |
| **tcgetattr** — terminal attribute query | main.c (`setup_terminal`), sixel.c | `tcgetattr` reads current `struct termios` | struct |
| **ioctl TIOCGWINSZ** — terminal size | main.c (`check_resize`) | `ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws)` | `struct winsize` |
| **accumulator line buffer** — multi-device display string | `sto_line` / `net_line` in `clock_state` | Widget-specific functions append per-device formatted lines via `snprintf` into a fixed buffer; `cat_lines` / `zcat` copies the final text to the draw buffer | `BIG_BUF` (1024B) |

*Buffer strategies:*
- **Batch scan**: read fixed-size chunk, process, re-use buffer for next chunk; extract only what's needed
- **Chunked line processing**: read chunk, find newlines with `memchr`, process complete lines; partial line stays (via `memmove`) at front of buffer for next iteration
- **Dynamic alloc**: `malloc`/`realloc` loop, doubling capacity; freed after extraction
- **Key-split awareness**: scan byte-by-byte with `memcmp` so keys split at chunk boundaries are still found

> **Accepted limitation — single line exceeds buffer:** The `LineCarry` mechanism (`file_scan.c`) shifts partial leftover to the front of the buffer, but does **not** grow or wrap when a single line exceeds `buf_sz`. If a line fills the entire buffer without a newline, `sys_read` returns 0 (no room), the loop exits, and `drain_line` passes only the first `buf_sz` bytes — the tail is silently lost. This is safe for current consumers (`/proc/stat` lines ≤220B, `/proc/meminfo` lines ≤80B, `/proc/self/mountinfo` lines ≤~400B) but must be reviewed before reuse on files with longer lines.

> **Hard rule — `block/*/stat` must remain `slurp`:** This file was once read with `read_file` into a 128B stack buffer (`STAT_BUF_SZ`). The kernel stat file for an NVMe drive was 156 bytes — the buffer was too small, `read_file` returned -1 on buffer-full, and no block devices were printed. Fixed by switching to `slurp`. The kernel stat format has 17 fields (adds discard/flush over time); worst-case 64-bit values produce ~357 bytes. Do not attempt to revert this to a stack buffer — any fixed size is speculative against future kernel changes. `slurp` is the permanent, safe choice.

All read paths are scoped to the active widget set (see CONTRIBUTING.md §Design Principles). If a widget is off, none of its data source files are opened or read.

## Business Rules

1. The program displays a real-time dashboard on the terminal showing: a large digital clock (center/middle), and a right-side information bar with machine monitoring metrics.
2. Runs indefinitely in a loop (every 1s) until terminated by SIGINT or SIGTERM, unless `--once` is given.
3. On each second boundary, the display refreshes with sub-second precision using `clock_gettime`/`nanosleep` to avoid drift. The clock reads time at the START of each tick: `clock_gettime(CLOCK_REALTIME)` called at the end of the previous tick's wait (`wait_next_tick`), returning `tv_sec + 1` for the next tick. This means the value feeding a given tick was captured just before the tick begins — equivalent to reading at start-of-tick in steady state. The first tick gets its initial time from `time(0)` at startup (step 1).
4. CLI argument selects display mode: `auto` (default), `text`, `ascii`, or `sixel`.
 5. `auto` mode re-evaluates at startup: if stdout is a TTY and stdin is a TTY,
    probe sixel support via DA1 query `\033[c` (10ms timeout). If sixel-capable
    → sixel mode; otherwise → ascii mode. If stdout is not a TTY → text mode.
    The computed mode is then used for all subsequent operations as if it were
    given as a CLI argument — no repeated `isatty()` call, DA1 query, or
    `TIOCGWINSZ` ioctl occurs after startup. When a mode (`text`, `ascii`,
    `sixel`) is given explicitly, `isatty()` is still called once to cache the
    result in `d->is_tty` for downstream cursor/echo/position decisions per Rules
    9–10, but no sixel-detection or DA1 query is performed. TIOCGWINSZ is still
    performed for ascii and sixel modes (they need layout dimensions) but is
    skipped for text mode.
 6. Sixel DA1 query is only sent in `auto` mode when both stdout and stdin are
    TTYs. Response parameter `4` indicates sixel capability. No `$TERM` matching.
    When `sixel` or `ascii` is given explicitly, the DA1 query is skipped
    entirely.
7. Text mode produces minimal machine-parseable output with no ANSI control sequences.
8. Terminal resize triggers full re-render (clear and recompute layout).
9. Keyboard echo is suppressed only when all four conditions hold: the resolved mode is ascii or sixel (not text), stdout is a TTY, **stdin is a TTY**, and the program is in loop mode (not `--once`). In text mode, `--once` mode, or when stdout or stdin is not a TTY, echo is never touched — neither suppressed nor restored.
10. The cursor is hidden during operation and restored on exit (via `cleanup_all`, called from both SIGINT/SIGTERM handler and before main return). Cursor hide (`\033[?25l`) is only emitted when all three conditions hold: ascii or sixel mode, stdout is a TTY, and loop mode (not `--once`). Cursor show (`\033[?25h`) is only emitted on exit if the cursor was previously hidden — if the program never called hide, it must not call show.

11. On terminal resize (`SIGWINCH`), `check_resize` re-reads window size, emits RIS (`\033c`), and recomputes layout. The resize check runs **after** data gathering but **before** rendering, so a SIGWINCH that arrives during async data fetching (WAN IP, weather) is caught in the same tick — no ghost frame at the old layout. After RIS, `d->sidebar_lines` is reset to 0 so the scroll-based clearing logic (`update_sidebar_lines`) does not emit `\033[K\n` past the new terminal height. RIS resets the terminal emulator — including cursor visibility — to its default (visible). After the render, the cursor is re-hidden (`\033[?25l`) if it was hidden before. This preserves the invariant: cursor is always hidden during loop-mode operation, even after resize.
12. All machine monitoring data is gathered internally — no external binaries are invoked.
13. Public IPv4 is fetched via HTTP GET from `api.ipify.org`, IPv6 from `api6.ipify.org`. Weather is fetched from `wttr.in/?format=j1`.
14. HTTP requests (WAN IP, weather) use a **three-tier async architecture**:
    - **Parallel DNS threads** (one per target) run blocking `getaddrinfo`
      and write results to `struct dns_slot` under a mutex.
    - **Single dedicated I/O thread** uses non-blocking `poll()` to handle
      connect, send, recv, and parse for all three connections concurrently.
      See [Thread / Worker / Parallelism Landscape](#thread--worker--parallelism-landscape).
    - **Main loop** never touches sockets. It reads final results from
      `struct http_result`, starts DNS threads on timer/event, and sets
      flags on a shared `struct ioserv_ctl` (mutex-only). Exception: the main
      loop MAY close socket fds directly during cancellation (wan/weather
      restart) and cleanup (`once_net_cleanup`, `weather_cleanup`,
      `cleanup_net`), but never performs I/O (connect/send/recv) on them.
15. While a response is pending in continuous mode: WAN/WAN6 lines are
    **omitted** from the NET widget (not emitted at all); weather shows a
    single dash `-`. In `--once` mode, all three fetch attempts run in
    parallel with a **3-second total deadline** — stragglers are cancelled
    and their results omitted.
16. If a request fails (timeout, connection refused, parse error), the
    previous cycle's data is retained in continuous mode. The failed target
    enters exponential backoff. DNS backoff is shorter (1<<n), connect/HTTP
    backoff is longer (2<<n). See [Retry Landscape](#retry-landscape) for the
    exact per-stage formulas. When all retries are exhausted (3 failures per
     stage): the target is abandoned — no further retries are attempted. The
    last successfully cached value is displayed (WAN shows cached IP, weather
    shows dash if never valid or cached data if available). Only a network-change
     event (address add/del or link add/del from the monitor FIFO) resets the
     try counters and reactivates the target.
17. No kernel module loading is performed. If a hwmon driver is already loaded, its sysfs nodes are used; otherwise the sensor is unavailable.
18. Netlink event monitoring uses a **monitor thread** holding a dedicated
    `NETLINK_ROUTE` socket subscribed to `RTMGRP_IPV4_IFADDR |
    RTMGRP_IPV6_IFADDR | RTMGRP_LINK | RTMGRP_IPV4_ROUTE |
    RTMGRP_IPV6_ROUTE`. The thread blocks on `recvmsg` inside a `poll(100)`
    loop. For each received netlink message it pushes a typed action into a
    mutex-protected FIFO — no conversion, parsing, or decision-making (see
    [Monitor Thread Action FIFO](#monitor-thread-action-fifo)). The main loop
    drains the FIFO each tick via a trylock loop: up to 10 attempts, 10ms
    `nanosleep` between each (no `clock_gettime` — see CONTRIBUTING.md
    §Design Principles for the "exactly three time-source calls" rule).
    If all 10 fail, skip this tick.
    aggregates events, and applies the aggregate per
    [the processing rules](#main-thread-processing-per-tick). After
    `poll_refresh` and `pick_primary`, `addr4_changed`/`addr6_changed` are
    always set to 1 (new primary NIC may have a different IP).
    The `addr4_changed` / `addr6_changed` flags are then consumed by
    `refresh_local_ips` each tick: if any is set, it invalidates the stale
    v6 route cache (`ci->keep.rtnl.v6_cached = 0`) and runs `ld_refresh()`
    (SIOCGIFADDR + rtnl_find_addr6 with a fresh route dump). If a local IP
    changed from its previously cached value, `refetch_wan()` closes each
    connection fd and sets `c->fd = -1` in the I/O thread's `io_conn` array
    (under the I/O ctl mutex), cancels any in-flight DNS threads
    (`pthread_cancel`), sets `ctl.cancel_all=1`, and immediately starts
    fresh DNS resolution for both WAN IPv4 and IPv6.
    The eventfd is exclusively for DNS thread → I/O thread wake; the
    main loop never writes it. The monitor socket and FIFO are active only
    in continuous mode — in `--once` mode the socket is never opened, the
    FIFO is never populated, and WAN IPs are fetched by the parallel DNS +
    I/O thread path with a 3-second deadline.

## Entities

| Entity | Description |
|--------|-------------|
| **Clock display** | Large-font time rendered with Unicode block characters (ASCII mode) or sixel graphics (sixel mode), centered horizontally and vertically. Width adjusts: if the sidebar has widgets, clock uses left/center region; if the sidebar is empty, clock uses full terminal width. |
| **Info bar** | Right-side panel built from the active widget set. Each widget occupies one or more lines. Width per [Display Layout](#display-layout). Absent when no widgets are active. |
| **Widget** | A named info-bar section that displays a specific metric group (CPU, GPU, MEM, etc.). Controlled by `--widgets`. |
| **Dot-matrix digit** | 3 columns × 5 rows bitmap encoding digits 0–9 and colon `:`. Used in ASCII mode for the large clock. |
| **Terminal state** | Original `termios` mode saved for restoration on exit. Cursor visibility flag. Window size for layout calculations. |
| **Network fetcher** | Three-tier architecture: (1) parallel DNS threads (WAN v4, WAN v6, weather) run blocking `getaddrinfo`, each signals the I/O thread on completion via `eventfd`; (2) I/O thread is **created on demand** by the first DNS to complete — uses `poll(100)` on an `eventfd` + socket fds for near-zero CPU wait, exits when no work remains; (3) main loop reads results from `struct http_result` and never touches sockets. |
| **Netlink context** | Three sockets total: (1) `NETLINK_GENERIC` — reused for wireless queries (nl80211); (2) `NETLINK_ROUTE` (dump socket) — reused for route lookups (`RTM_GETROUTE`), link queries (`RTM_GETLINK`), and address queries (`RTM_GETADDR`); (3) `NETLINK_ROUTE` (monitor socket in `rtnl_mon_ctx`) — subscribed to address/link/route multicast groups (`RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR | RTMGRP_LINK | RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE`), held by a dedicated background thread that blocks on `recvmsg`. The thread pushes typed actions into a mutex-protected FIFO (see [Monitor Thread Action FIFO](#monitor-thread-action-fifo)); the main thread drains the FIFO each tick and acts on each action — setting flags, marking caches stale, or triggering re-evaluation. The monitor socket is created only in continuous mode (not `--once`). Bootstrap: `rtnl_open_monitor` sets `addr4_changed=1` and `addr6_changed=1` directly so the first tick always performs an initial IP refresh. The dump socket hosts a **link cache** (`r->links[]`) with two population modes: full dump (first tick, or on route/link event from the FIFO) populating `{ifindex, name, is_virtual, rx_bytes, tx_bytes}` for ALL interfaces; steady-state ticks do targeted `RTM_GETLINK` queries for only the SELECTED interfaces (at most 3) to refresh throughput counters. Both route selection (`pick_primary`) and `rtnl_read_dev` consume this cache — no duplicate dump. |
| **DNS slot** | Shared struct (`lock`, `addr`, `addrlen`, `state`, `cancelled`, `thread`) used by WAN v4, WAN v6, and weather DNS resolution. The DNS background thread calls `getaddrinfo` (a POSIX cancellation point) and writes the result under the mutex. `cancelled` flag and `pthread_cancel` allow the main thread to abort an in-flight DNS on network-change restart. `thread` stores the `pthread_t` (created joinable) so `pthread_cancel` is valid; after cancellation, `pthread_detach` prevents a zombie. Three static slots: `wan4_dns`, `wan6_dns`, `weather_dns`. |
| **HTTP result slot** | Shared struct (`lock`, `state`, `cancelled`, `reason`, `data[96]`) written by the I/O thread after a fetch completes and read by the main loop. `state`: 0=empty, 2=done, -1=error (state 1 is reserved but never written). `reason`: 0=unknown, 'C'=connect fail, 'H'=HTTP fail, 'D'=DNS exhausted. Three static instances: `wan4_result`, `wan6_result`, `weather_result`. |
| **I/O thread** | On-demand single thread created by the first DNS thread to complete. Runs a `poll(100)`-based event loop on an `eventfd` + 3 socket fds. Blocks with near-zero CPU when waiting; eventfd write shortens wait to ≤100ms. Exits when all connections complete and `pending_work == 0`. DNS threads still running create a fresh I/O thread when they finish. |
| **I/O control block** | Shared struct (`pthread_mutex_t lock`, `int efd`, `int io_thread_active`, `int shutdown`, `int cancel_all`, `int wan_cancel`, `int weather_cancel`, `int pending_work`, `struct async_ctx *owner`, `struct io_conn conns[MAX_HTTP_CONNS]`) used for communication between main thread, DNS threads, and I/O thread. The connection array is indexed by `io_setup_conns()`; the main loop cancels via `io_cancel_all()` (closes `conns[i].fd` and sets to -1 under the mutex — the I/O thread detects this via POLLNVAL). `cancel_all`, `wan_cancel`, and `weather_cancel` flags tell the I/O thread which connections to clean up on its next loop iteration. `eventfd` (fd, poll-able counter) replaces condvar — DNS threads `eventfd_write` to wake I/O thread, I/O thread polls it in its `poll()` set. The main loop never writes the eventfd. |
| **Network context** (`net_ctx`) | Stores selected primary NIC ifindices in `c->ifindex[MAX_NET]` (up to 3), plus corresponding `c->link_idx[MAX_NET]` (index into `r->links[]` for throughput). Gateway route cache in `c->gw_idx[]` / `c->gw_metric[]` / `c->gw_n`. Wireless state (`c->wlan_idx`, `dgram_fd`). Flags: `needs_route`, `needs_reprimary`, `wcache_stale`, `has_phys`. Wireless cache (`c->wcache`) with discovered wlan ifindices and SSIDs. Display code resolves name for the sidebar via `name_idx_by_idx(r, c->ifindex[i])`. |
| **System stats collector** | Reads `/proc/stat`, `/proc/meminfo`, `/sys/class/power_supply/*`, `/proc/self/mountinfo`, `/sys/block/*/stat`, `/sys/fs/cgroup/memory.*`, `/sys/class/hwmon/hwmon*`, etc. Plus `sysinfo()` for uptime and load average, ethtool ioctl for link speed, and netlink (nl80211 + rtnetlink) for wireless signal/SSID, routes, addresses, and link stats. |
| **GPU stats collector** | Best-effort reads from DRM sysfs: GPU busy percent, power/rc6 residency, clock frequencies, memory usage, hwmon temperature and fan. Only active when GPU widget is in the active set. |
| **Sensor collector** | Best-effort reads from hwmon sysfs for motherboard fan speeds and temperatures (no kernel module loading). Only active when FAN widget is in the active set. |

## Widgets

Widgets control which sections appear in the info bar and their rendering order. The clock (TIME) is always shown regardless of widget configuration.

### Widget types

| Widget | Content |
|--------|---------|
| `DATE` | Current date line (e.g. `Thu 12 Jun 2026`) |
| `CPU` | CPU usage %, load % (avg / cpus), frequency avg, governor, temperature |
| `GPU` | GPU usage %, temperature, fan RPM, core clock (current/max); VRAM usage %, memory clock (current/max) |
| `MEM` | Memory usage %, used/total |
| `FAN` | Motherboard fan speeds + motherboard temperatures |
| `BAT` | Battery percentage, status with icon |
| `UP` | Uptime |
| `STO` | Per-device throughput, per-mount usage, storage temperature |
| `NET` | Per-interface throughput + link speed, wireless signal + link speed, IPv4/IPv6, public IPs |
| `WEATHER` | Weather icon + current temperature + day max/min + short description |
| `CAL` | Month calendar grid |

Data sources for each widget are detailed in [Machine Monitoring — Data Sources](#machine-monitoring--data-sources).

### Default mode (no `--widgets`)

The sidebar uses this fixed order, with all widgets always-on:

```
DATE → CPU → MEM → GPU → FAN → BAT → UP → STO → NET → WEATHER → CAL
```

### Custom mode (`--widgets=<list>`)

When `--widgets` is given:
1. The comma-separated list defines exactly which widgets appear and in what order.
2. Widget names are case-insensitive.
3. Unknown widget names cause the program to exit with an error.
4. `--widgets ''` (empty string) disables the sidebar entirely. The clock uses the full terminal width.

Examples:
```
--widgets NET,CPU,MEM     — only NET, CPU, MEM in that order
--widgets CAL,DATE        — calendar and date only
--widgets ''              — no sidebar, full-screen clock
```

### Lazy reads

Data is only gathered for widgets in the active set. If no widget depends on a given source, the file is not read and its file descriptor is not opened.

## Display Layout

When the sidebar has at least one widget: right-side info bar occupies ~30% of terminal width, with a minimum of 40 characters and maximum of 60 characters. The clock is centered in the remaining (left) region and scaled to fit.

When the sidebar is empty (widget list is empty): the clock uses the full terminal width and is centered both horizontally and vertically.

In ASCII mode the scale factor `size` is computed from the available width
(terminal width minus sidebar, or full terminal width when no sidebar) and the
terminal height:

```
size = min(available_width / 32, wsrow / 16)
```

The divisor 32 (= 27 dot-matrix columns + 5 columns of implicit margin) keeps
the clock from pressing against the edges, matching the reference tty-clock
behaviour. `wsrow / 16` provides the vertical constraint: one size unit per 16
terminal rows.

The clock is centered vertically within the terminal using its actual rendered
height. When the terminal is too small for any centering (`wsrow ≤ 3`), the
clock is placed at row 0:

```
clock_height = (size × DOT_ROWS + 1) / 2    (terminal lines)
row          = wsrow > 3 ? (wsrow - clock_height) / 2 : 0    (0-indexed from top)
```

Horizontal centering uses the dot-matrix width (without margin):

```
clock_width = size × DOT_COLS               (terminal columns)
col         = max(0, (left_w - clock_width) / 2)    (for left_w > DOT_COLS)
```

Vertical positioning differs by mode:
- **Continuous (loop) mode**: the clock is vertically centered and the sidebar starts at the **top row** (row 0).
- **`--once` mode (TTY, ascii or sixel)**: the clock renders inline at the cursor position — no vertical `position_cursor` CUP, but **horizontally centered** via cursor-forward `\033[<N>C` (`render_line` for ascii, equivalent for sixel). The sidebar is positioned at the **same Y** where the clock started, via relative cursor-up `\033[%dA`. Since cursor-up inherently handles scrolling, no scroll-adjustment formula is needed.
- **`--once` mode (non-TTY)**: no cursor-positioning escapes at all — clock renders at current cursor column, left-aligned.

Sidebar truncation (continuous loop mode only): when the number of sidebar rows (after wrapping long lines) would exceed the terminal height (`wsrow`), excess rows are silently dropped. If the terminal is resized to a smaller height, the sidebar is truncated to the new height on the next tick — no scroll-back or scroll-forward occurs. The sidebar is always a window into the widget content that fits the current terminal height, never longer.

Frame separation between ticks differs by mode: text mode outputs a `\n` between frames (producing a blank line separator); ascii and sixel modes use absolute CUP positioning and emit no `\n` between frames, relying on cursor-position escapes to move to the clock's start row each tick. On exit, a `\n` is emitted before cursor-show to ensure the shell prompt lands on a fresh line rather than at the cursor's last sidebar position.

Screen split applies to ascii and sixel modes only — text mode outputs plain `HH:MM:SS` without layout splitting.

```
 ┌──────────────────────────────────────┬──────────────────────────┐
 │                                      │  Thu 12 Jun 2026         │
 │        ██  ██  ██████                │ CPU 12% 15% 2% 3.2/4.0GHz 48°C schedutil │
 │        ██  ██  ██                    │ GPU 45% 200/1200MHz 72°C 3200RPM auto │
 │        ██████  ██████                │ MEM 45% 2.3/8.0G         │
 │        ██  ██  ██                    │ FAN 3200RPM 2800RPM      │
 │        ██  ██  ██████                │ 48°C 45°C 42°C           │
 │                                      │ ⚡ 78%                   │
 │              12:34:56                │ UP 12d 4h 32m            │
 │                                      │ STO sda ↓0K↑0K           │
 │                                      │ /  62%  120/200G         │
 │                                      │ /home 81%  820/1T        │
 │                                      │ nvme0n1 45°C ↓250↑180M   │
 │                                      │ eth0 ↓1.2M↑3.4M 2.5G     │
 │                                      │ wlan0 ↓0.5M↑1.0M ▂▄▆█ -45dBm │
 │                                      │ SSID MyWiFi 1.7/2.3G     │
 │                                      │ IP 203.0.113.42          │
 │                                      │ IP6 2001:db8::1          │
 │                                      │ ☀️ +15°C                  │
 │                                      │ CAL Jun 2026             │
 │                                      │ Mo Tu We Th Fr Sa Su     │
 │                                      │ 1  2  3  4  5  6  7      │
 │                                      │ 8  9 10 11 12 13 14      │
 │                                      │ 15 16 17 18 19 20 21     │
 │                                      │ 22 23 24 25 26 27 28     │
 │                                      │ 29 30                    │
 └──────────────────────────────────────┴──────────────────────────┘
```

## Format Rules

Single source for all unit and threshold decisions. All other sections reference
this one.

### Data-size thresholds

Compute unit from the **maximum** of used and total (identical for total-only
sources). Unit applies to both values shown.

**MEM and VRAM** (M → G):

Both host memory (from `/proc/meminfo`) and container/guest memory (from cgroup
v2/v1) follow the same unit rules. When container and host are both shown on the
same MEM line, the unit threshold is evaluated independently for each pair — the
host and container may use different units (one M, one G) based on their
individual max values. VRAM follows the same rules: only M and G tiers, no K tier.

| Condition | Unit | Display | Example |
|---|---|---|---|
| `max ≤ 1536` MB | M | `%.1f` | `512.0M`, `1.5M` |
| else | G | `%.1f` | `4.0G`, `16.5G` |

**STO usage** (M → G → T):

| Condition | Unit | Display | Example |
|---|---|---|---|
| `max ≤ 1536` MB | M | `%llu` | `120M` |
| `max / 1024 ≤ 1536` | G | `%.1f` | `120.0G` |
| else | T | `%.1f` | `1.5T` |

### STO throughput

Per-direction read/write, independent unit per direction (b → K → M):

| Condition | Unit | Display |
|---|---|---|
| `v ≤ 1536` b | b | `%llu` (implicit `b` suffix appended) |
| `v / 1024 ≤ 1536` | K | `%.1f` |
| else | M | `%.1f` |

### Network throughput thresholds

| Range (bytes/s) | Unit | Display |
|---|---|---|
| `< 512` | b | `%llu` |
| `≥ 512` and `< 102400` | K | `%.1f` |
| `≥ 102400` and `< 104857600` | M | `%.1f` |
| `≥ 104857600` | G | `%.1f` |

Each direction (rx/tx) has independent unit selection.

The numerical value is truncated (floor) to one decimal place via
integer arithmetic: `(v × 10 ÷ divisor) ÷ 10`. This ensures values
at the upper bound (e.g. `102399`) display as `99.9K` (4 chars)
instead of `100.0K` (5 chars), avoiding an extra character column.

### CPU, GPU, and VRAM clock frequencies

Compute unit from the **max frequency** (`freq_max`) only. Applies to
CPU core clocks, GPU core clocks, and GPU memory clocks. When `freq_max`
is 0 (unavailable, e.g. some Intel GPUs without `gt_max_freq_mhz`),
fall back to using current `freq` for the unit threshold decision.

| Condition | Unit | Display |
|---|---|---|
| `max ≤ 1536` MHz | MHz | `%d` |
| `max > 1536` MHz | GHz | `%.1f` |

Same unit applies to both current and max value.

### Wired link speed

Value comes from `ETHTOOL_GLINKSETTINGS` ioctl in Mb/s:

| Speed | Display |
|---|---|
| `< 1000` Mb/s | `%dM` (e.g. `100M`) |
| `≥ 1000` Mb/s | `%.1fG` (e.g. `1.0G`, `2.5G`, `10.0G`) |

Fallback: when `ETHTOOL_GLINKSETTINGS` returns speed=0 but the two-phase probe
ioctl succeeds, the legacy `ETHTOOL_GSET` ioctl is tried as a second attempt
(during the same tick). If the two-phase probe fails entirely
(`ethtool_need_nwords` ioctl error) or both probes return 0, `speed = 0` is
cached in the link cache entry and no further probing is attempted on subsequent
ticks — the interface is treated as having no known link speed and the speed
field is omitted from the output.

### Wireless link rate

Values from nl80211 in Mb/s. Decision to show G or M:

Show `%.1fG` when **any** rx or tx ≥ 1000, **or** both rx and tx ≥ 500.
Otherwise show `%dM`.

### Wireless signal bars

Signal strength in dBm rendered as Unicode block bars before the dBm value:

| Range | Bars |
|-------|------|
| `≥ -50` dBm | `▂▄▆█` |
| `≥ -60` dBm | `▂▄▆` |
| `≥ -70` dBm | `▂▄` |
| `≥ -80` dBm | `▂` |
| `< -80` dBm | *(none — dBm only)* |

Padding between throughput and bars depends on display mode — see [Mode-specific rendering](#mode-specific-rendering).

### Percentage display

All percentages are integer (`%d%%`). Memory and VRAM percentages are computed
from integer division of used/total; CPU load/usage/iowait, GPU usage, storage
usage, and battery all follow the same integer format.

### Temperature display

Always `%d°C` (integer Celsius).

### Widget rendering details

| Widget | Text label | Ascii/sixel icon | Content lines |
|--------|------------|------------------|---------------|
| `DATE` | *(none)* | *(none)* | `Thu 12 Jun 2026` |
| `CPU` | `CPU` | 💻 (`U+1F4BB`) | Single line: `<text:CPU / icon:💻> NN% NN% NN% {NNNNMHz|N.NGHz} NN°C governor` | Usage %, load % (avg/cpus × 100), iowait %, frequency current/max (unit per [Format Rules](#cpu-gpu-and-vram-clock-frequencies); one format selected by threshold), temperature, governor. All on one line. |
| `GPU` | `GPU` | 🎮 (`U+1F3AE`) | Line 1: `<text:GPU / icon:🎮> NN% {NN/NNNMHz|N.N/N.NGHz} NN°C NNNNRPM governor`; Line 2: `<ascii/sixel icon:🎞️ / text mode:VRAM > NN% {N.N/N.NM|N.N/N.NG} {NNN/NNNMHz|N.N/N.NGHz}` — the only difference across modes is the prefix (icon vs text label); content after the prefix is identical. GPU core clock per [Format Rules](#cpu-gpu-and-vram-clock-frequencies). VRAM usage per [Data-size thresholds](#data-size-thresholds). VRAM clock per same frequency rules. Fan appended as `NNNNRPM` after temp if present. VRAM line is hardware-dependent — omitted entirely when the GPU does not expose memory info (e.g. some integrated GPUs, or when `mem_info_vis_vram_*` files are absent). |
| `MEM` | `MEM` | 🧠 (`U+1F9E0`) | Without container: `<text:MEM / icon:🧠> NN% N.N/N.NM / N.N/N.NG`. With container: `<text:MEM / icon:🧠> NN% N.N/N.NG NN% N.N/N.NG` — first percentage/value pair is container (guest), second is host. Each pair follows unit selection independently per [Data-size thresholds](#data-size-thresholds), using its own max value for the threshold decision. All values stored in **kB** internally; display divides by 1024.0 per data-size thresholds. | Percentage per [Format Rules](#percentage-display). Unit per [Data-size thresholds](#data-size-thresholds). Container values from cgroup v2/v1; when `memory.max` is `"max"` (unlimited), `ctr_max_kb` comes from `MemTotal` (already in kB) — no conversion needed. |
| `FAN` | `FAN` | 💨 (`U+1F4A8`) | Line 1 (ascii/sixel): `<icon:💨> NNNNRPM RRRRPM ...` — text mode: `FAN NNNNRPM RRRRPM ...`; Line 2 (ascii/sixel): `<icon:🌡️> NN°C NN°C ...` — text mode: `NN°C NN°C ...` (no prefix in text mode) | Fans space-separated on one line with a single icon/prefix, temps space-separated on next line. Newline (not blank line) between groups. Output order per [sensor discovery](#hardware-sensor-discovery-order). |
| `BAT` | `BAT` | *(dynamic)* | Text mode: `BAT NN% charging/discharging/full`; Ascii/sixel: `<icon:⚡🔋🪫> NN%` (icon before percentage, no `BAT` label) | After percentage, append space + arrow + time estimate when samples exist: `↓ 2h 15m` (discharging) or `↑ 45m` (charging). When no samples exist, no arrow/time appended. Full/Not charging always shows no estimate. Ascii/sixel mode: same arrow/time appended. |
| `UP` | `UP` | ⏱️ (`U+23F1` + VS16) | `<text:UP / icon:⏱️> Nd Nh Nm` |
| `STO` | `STO` | 🗄️ (`U+1F5C4` + VS16) | `<text:STO / icon:🗄️> device ↓N↑N/s`; `mount NN% N/N`; `device NN°C ↓N↑N/s` | Throughput unit per [Data-size thresholds](#data-size-thresholds) (b→K→M chain). Usage unit per same rules. Each direction (read/write) has independent unit. Arrow: ↓ read, ↑ write. |
| `NET` | *(none)* | 🌐 (`U+1F310`) | Each NIC on its own line (no blank lines): `iface ↓N↑N/s [linkspeed]`; wireless when present: `wlan0 ↓N↑N/s ▂▄▆█ -NNdBm` (bars per [Wireless signal bars](#wireless-signal-bars); padding per [Mode-specific rendering](#mode-specific-rendering)); SSID line (ascii/sixel): `<icon:🛜> <name> N.N/N.NG` / `<icon:🛜> <name> NN/NNM` — text mode: `SSID <name> ...`; then address lines in fixed order: IPv4, IPv6 (local), WAN (public), WAN6 (public). Each line is shown only when its address is non-empty. Text mode labels: `IP`/`IP6`/`WAN`/`WAN6`; ascii/sixel icons: 📡/📡/🌍/🌍. When a local IP matches its WAN counterpart, its label/icon changes and the redundant WAN/WAN6 line is omitted, per [Overlapping IP/WAN](#mode-specific-rendering). | Throughput per [Network throughput thresholds](#network-throughput-thresholds). Wireless rate per [Wireless link rate](#wireless-link-rate). Link speed per [Wired link speed](#wired-link-speed). Arrow: ↓ rx, ↑ tx. |
| `WEATHER` | *(none)* | *(none)* | `[icon/desc] +NN°C [+NN°C..+NN°C]`; while pending: `-` (dash). In `--once` mode the main loop blocks up to 3s awaiting weather; if the deadline expires or the fetch fails, a dash is shown. |
| `CAL` | `Month YYYY` | *(none)* | `   Month YYYY` (3-space indent on the month line only), then day header row, then day grid. The first day is padded with `w*3` spaces to align it below its weekday header column — this is valid positional alignment, not extra indentation. Today's date is highlighted with reverse video (`\033[7m`) in ascii/panel modes when stdout is a TTY; no escape sequences emitted on pipe. |

Widget groups are emitted consecutively — a newline separates each widget from the next. No divider lines or blank lines between widget groups.

Text mode uses the text label (fifth column) as-is — no Unicode icons. Ascii and sixel modes replace the text label with the corresponding Unicode icon. The content after the label/icon is identical across modes.

### Battery icons

Regular (TTY) modes show Unicode icons. Text mode shows short English descriptions in their place.

| Status | TTY icon | Text mode |
|--------|----------|-----------|
| Charging | ⚡ (lightning bolt) | `charging` |
| Full | 🔋 (full battery) | `full` |
| Discharging | 🪫 (draining battery) | `discharging` |
| Unknown or not‑present | Hidden (no line emitted) | Hidden (no line emitted) |

In ascii and sixel modes, the icon appears **before** the percentage and the `BAT` text label is omitted — e.g. `⚡ 78%` instead of `BAT 78% ⚡`.

### Battery time estimation

#### Sample model

Two independent FIFO slot arrays, each holding up to 10 samples:

| Array | Direction | Contents |
|-------|-----------|----------|
| `charge_samples` | Charging (status `Charging`) | Samples recorded while status register is `Charging` |
| `discharge_samples` | Discharging (status `Discharging`) | Samples recorded while status register is `Discharging` |

Each sample:

```
struct bat_sample {
  int       duration_sec; /* seconds elapsed for this sample (~60) */
  long long power_diff;   /* change in raw charge (µAh), signed; positive = charging, negative = discharging */
};
```

#### Power variable — raw charge/energy (µAh)

The estimation uses **raw charge values**, not the capacity percentage. On every tick,
`get_battery` reads `charge_now` (µAh) from sysfs; if that file is absent, it falls back
to `energy_now` (µWh). On read failure (both files return error), the previously cached
`bat_last_raw` value is retained.

**Permanent vs transient failures**: on the **first tick**, file paths that fail are
permanently marked (`BAT_NO_CHARGE_NOW`, `BAT_NO_ENERGY_NOW`) and never retried —
these files genuinely don't exist. On **subsequent ticks**, a read failure is treated
as transient (e.g. recalibration); the file is retried next tick and the permanent flag
must not be set. The display percentage is computed as:

```
bat_pct = (charge_now * 100) / charge_full
```

where `charge_full` (or `energy_full`) is read once at initialization and cached in
`cpu_keep.bat_charge_full_raw`. All `power_diff` fields and the estimation formula
operate on µAh (or µWh), not on percentage points. Using the raw linear value avoids
the non-linearity of lithium‑ion discharge curves near 0% and 100%.

#### Persistent state (held in `cpu_keep`)

| Field | Type | Purpose |
|-------|------|---------|
| `bat_samples_chg[10]` | `struct bat_sample` | Charging samples, FIFO, indexed by `bat_idx_chg`. Each sample has `power_diff` (µAh), `duration_sec`, and `biased` (int: 1 = biased, 0 = unbiased). |
| `bat_samples_dchg[10]` | `struct bat_sample` | Discharging samples, FIFO, indexed by `bat_idx_dchg`. Same structure. |
| `bat_idx_chg` | `int` | Current slot index for charging (−1 when no active slot) |
| `bat_idx_dchg` | `int` | Current slot index for discharging (−1 when no active slot) |
| `bat_charge_count` | `int` | Number of valid samples in charge array (0–10) |
| `bat_discharge_count` | `int` | Number of valid samples in discharge array (0–10) |
| `bat_last_raw` | `int` | Raw charge value (µAh) at last state-transition or last sample-start |
| `bat_tstate` | `int` | 0=discharging, 1=charging, 2=full/not-charging; mirrors `bat_charging` semantics |
| `bat_prev_state` | `int` | Previous tick's `bat_tstate`; used to detect transitions to state 2 |
| `bat_change_ts` | `time_t` | `time(0)` wall-clock value, dual-purpose: (a) state-entry timestamp — set when state switches to discharging or charging; (b) slot-rollover timestamp — updated when a 60-second sample is closed. Used for `duration_sec` computation in both cases. Rollover never fires on a state-transition tick (transition runs first, clearing slots before rollover logic executes). |
| `bat_charge_full_raw` | `int` | Full charge capacity (µAh) — re-read on Full/Not-charging when `bat_pct != 100` |
| `bat_biased_next_chg` | `int` | 1 if the next charging sample created should be marked biased, 0 otherwise |
| `bat_biased_next_dchg` | `int` | 1 if the next discharging sample created should be marked biased, 0 otherwise |
| `bat_unbiased_full_chg` | `int` | 1 when enough unbiased charging time has been accumulated (flag to skip further biased charging samples) |
| `bat_unbiased_full_dchg` | `int` | 1 when enough unbiased discharging time has been accumulated (flag to skip further biased discharging samples) |

#### State machine

**On every tick** (inside `get_battery`):

1. Read `charge_now` (µAh) → `cur_raw`, and `status` → `state`.
   - Fallback: if `charge_now` is absent, read `energy_now` (µWh).
    - Read `charge_full` (fallback `energy_full`) on the first tick and cache in `bat_charge_full_raw`. Re-read it only on the tick where state transitions *to* Full/Not-charging (2) and `bat_pct != 100` — the capacity may drift over time (battery wear, calibration), and this state provides a stable moment to recalibrate. Consecutive ticks in state 2 do not re-read (avoids pointless sysfs reads on batteries with charge limits below 100%).
   - Compute `bat_pct = (cur_raw * 100) / bat_charge_full_raw` for display.
   - `state 0` = Discharging.
   - `state 1` = Charging.
   - `state 2` = Full / Not charging / Unknown → no sampling possible.

2. **State transition detection** — on ANY state change between 0, 1, or 2, the in-flight sample for the direction being left (if one exists) is finalized at the transition moment. The finalization runs the same consolidation logic as a rollover (step 3b below) for consistency. Previously completed samples are kept and remain available for future estimates.

   - If the previous state was discharging (0) or charging (1): finalize its in-flight sample if one exists (`bat_idx_dchg ≥ 0` or `bat_idx_chg ≥ 0`). Finalizing records `duration_sec = (int)(now - bat_change_ts)` on the final slot — `power_diff` is already correct from the last incremental update. Apply the **unbiased consolidation** logic (see below). If no open slot exists (no power change occurred during the period), no slot is created or advanced.
   - If the previous state was 2 (full/not-charging): nothing to finalize.
   - If the new state is discharging (0) or charging (1): reset baselines (`bat_change_ts = now`, `bat_last_raw = cur_raw`), set the new direction's index to -1, and set `bat_biased_next_* = 1` for the new direction. If the old direction's `bat_unbiased_full_*` was 1, it remains 1 — future biased samples in that direction are still skipped when the direction is re-entered.
   - If the new state is 2 (full/not-charging): recalibrate `bat_charge_full_raw` if `bat_pct != 100` (only on the transition tick — subsequent consecutive ticks in state 2 skip the re-read via `bat_prev_state == 2` guard).

   After handling the transition, update `bat_prev_state = bat_tstate`, then `bat_tstate = state`. Consecutive ticks within the same state only execute step 3 below — the transition block is not re-entered.

3. **While state is Discharging or Charging** and `bat_tstate` matches:

   a. Compute `per_tick_delta = cur_raw - bat_last_raw`.
      - If `per_tick_delta == 0`: skip slot storage entirely this tick. No updates to samples, no slot advancement.

   b. If `per_tick_delta != 0`:

      - **Biased flag**: the first sample created after entering a direction is marked biased (`bat_biased_next_* == 1`). Subsequent samples in the same direction (created on rollover, when `idx == -1` and a new slot is allocated) are marked unbiased (`bat_biased_next_* == 0`). Multiple biased samples for the same direction can exist if state changes back and forth faster than the tick interval, each entry resetting `bat_biased_next_* = 1`.

      - If the index for this direction is `−1` (no active slot):
        - If the new slot would be **biased** (`bat_biased_next_* == 1`) AND `bat_unbiased_full_* == 1`: skip slot creation entirely for this tick. Do not create or advance any slot. Proceed to step 3c.
        - Otherwise: advance to the next free slot (wrapping FIFO at 10, evicting oldest). Initialize the new slot's `power_diff = 0`, `biased = bat_biased_next_*` (1 or 0), then set `bat_biased_next_* = 0`. If advancing beyond `count`, increment count (capped at 10).

      - Accumulate into the active slot:
        - `power_diff += per_tick_delta` — running total of raw change since slot creation.
        - `duration_sec = (int)(now - bat_change_ts)` — time since last state entry or rollover.

      - If `duration_sec ≥ 60`, close the active slot (values already stored this tick) and set `idx = -1`. Apply **unbiased consolidation** (see below). Update `bat_change_ts = now` and `bat_last_raw = cur_raw`. The next tick with `per_tick_delta != 0` will allocate a fresh slot starting at `power_diff = 0`, `duration_sec = 0`.

      - If `duration_sec < 60`, leave the slot open. Future ticks with non-zero `per_tick_delta` will accumulate onto the same `power_diff` and refresh `duration_sec`.

      - **Unbiased consolidation** (runs when a slot is finalized, both at rollover and at state-transition finalization, but only if `bat_unbiased_full_*` is not already 1):
        - If the finalized slot is biased: store it normally, no further action.
        - If the finalized slot is unbiased:
          1. Compute `total_unbiased_dur = slot.duration_sec + sum(duration_sec of all previously stored unbiased samples in this direction)`.
          2. If `total_unbiased_dur ≥ 60`:
             - Set `bat_unbiased_full_* = 1`.
             - Remove (prune) all biased samples from this direction's FIFO. Only unbiased samples remain.
             - Any future biased slot creation for this direction is skipped (the `bat_unbiased_full_* == 1` check above).

   c. Always set `bat_last_raw = cur_raw` at the end of the tick regardless of whether storage occurred.

#### Estimation formula

For each direction that has at least one valid sample — including the current open in-flight slot (its `duration_sec > 0` once any power change occurred):
```
total_uah   = sum of all valid samples' |power_diff|   (µAh or µWh)
total_time  = sum of all valid samples' duration_sec     (seconds)
rate_uah_ps = total_uah / total_time                     (µAh per second)
```

- **Discharging**: `estimated_seconds = cur_raw / rate_uah_ps` (remaining raw charge at current drain rate).
- **Charging**: `estimated_seconds = (bat_charge_full_raw - cur_raw) / rate_uah_ps` (raw charge needed to fill at current rate).

The intermediate product `remaining_uah × total_time` is evaluated as `long long` to
avoid overflow with high-capacity batteries (e.g. > 3 000 000 µAh).

#### Display format

- If `count == 0` for the current direction, no estimate is appended.
- If `count > 0` and discharging: append ` ↓ %dh %dm` (or ` ↓ %dm` when less than 1 hour).
- If `count > 0` and charging: append ` ↑ %dh %dm` (or ` ↑ %dm` when less than 1 hour).
- Estimate applies to **text and ascii/sixel modes alike** — the arrow and time always appear after the percentage in the same order.

#### Edge cases

- **Wrap around**: When all 10 slots are full and a new slot is needed, evict the oldest (index wraps, count stays 10).
- **Full / Not charging**: On entering state 2, the current in-flight sample (if any) is finalized at that tick's `cur_raw` and `now`. No new slot is started. Completed samples from prior periods are kept.
- **Power stays flat for minutes**: No slot is created until `power_diff ≠ 0`. When it finally changes, the slot's `duration_sec` already spans from state entry (or last rollover) to the current time, so the flat interval is captured accurately in a single sample.
- **Power reverses direction**: A state change occurs first (Charging→Discharging or Discharging→Charging). On the transition tick, the current in-flight sample for the prior direction is finalized (if it exists); baselines reset; the new direction starts fresh.
- **Fast flips**: If the battery flips state every tick, any in-flight sample is finalized on each flip. If no power change occurred during the period (no open slot), nothing is stored and no slot is wasted. Previously completed samples from prior periods are kept and contribute to the estimate.
- **Fast charging/discharging (>1%/s)**: The open in-flight slot is included in the estimate on every tick, so the rate reflects all available data. The 60-second rollover (closing the slot and starting a fresh one at `idx = -1`) is purely a slot management concern — no data is lost, and the next power change will create a new slot that continues contributing to the estimate.

### Calendar start day

Default: week starts on Monday. With `--sunday-start`, week starts on Sunday (Sunday → Saturday layout).

### Mode-specific rendering

| Field | Text mode | ASCII mode | Sixel mode |
|-------|-----------|------------|------------|
| Clock | `HH:MM:SS` plain text | Unicode block chars (▀▄█) scaled | Sixel bitmapped clock digits |
| Widget labels | Plain text labels (`CPU`, `MEM`, `GPU`, `FAN`, `UP`, `STO`, `BAT`, `SSID`, `IP`, `IP6`, `WIP`, `WIP6`, `WAN`, `WAN6`, `VRAM`) | Unicode icons per [Widget rendering details](#widget-rendering-details) — text labels removed | Same as ASCII |
| Battery | `BAT NN% charging/discharging/full` (time estimate appended when samples exist: `BAT NN% ↓ 2h 15m discharging`) | `<icon> NN%` — icon before percentage, no `BAT` label, no text suffix. Time estimate appended after percentage: `<icon> NN% ↓ 2h 15m` | Same as ASCII |
| FAN temps | No prefix (`NN°C NN°C ...`) | 🌡️ prefix added before temp line | Same as ASCII |
| VRAM label | `VRAM` | 🎞️ (`U+1F39E` + VS16) | Same as ASCII |
| SSID label | `SSID` | 🛜 (`U+1F6DC`) | Same as ASCII |
| IP/IP6 labels | `IP`, `IP6` | 📡 (`U+1F4E1`) — same icon for both; IPv4 and IPv6 format patterns are visibly different, no distinct icon needed | Same as ASCII |
| WAN/WAN6 labels | `WAN`, `WAN6` | 🌍 (`U+1F30D`) — text mode shows `WAN <addr>`, ascii/sixel shows globe icon before address | Same as ASCII |
| Overlapping IP/WAN | When local IP matches WAN, text mode label changes from `IP`/`IP6` to `WIP`/`WIP6`. Ascii/sixel mode replaces 📡 with 🌍. The redundant WAN/WAN6 line is omitted (the relabeled IP line already carries the WAN address). Line order preserved: IPv4 always first, then IPv6, then WAN, then WAN6 — each omitted when empty. Independent per address family. | Same as text (icon swap, no label text) | Same as ASCII |
| Wireless strength | Bars per [Wireless signal bars](#wireless-signal-bars) — fixed 1-space padding, no dynamic `wifi_pad` | Bars per [Wireless signal bars](#wireless-signal-bars) — dynamic `wifi_pad` spacing 1–7 | Same as ASCII |
| NIC icon | *(none — SSID on its own line)* | *(none — SSID on its own line)* | Same |
| Weather | short description + temperature [+max..+min]; `-` while pending (continuous) | Unicode icon + temperature [+max..+min]; `-` while pending (continuous) | Same as ASCII |
| GPU | `GPU 45% 200MHz/1200MHz / 0.2/1.2GHz 72°C 3200RPM auto` `VRAM 25% 2.0/8.0G 933/1200MHz` | Unicode icon + text. Line 2 uses 🎞️ instead of `VRAM`. | Same as ASCII |
| Trailing newline (`--once`) | Ends with `\n` | Must end with `\n` | Must end with `\n` |
| ANSI control sequences | None — no `\033` bytes in output. | **Loop (TTY)**: `\033[%d;0H\033[<N>C` (vertical offset + horizontal centering), reset `\033c`, cursor hide. **Loop (non-TTY)**: no escapes — render output as-is (Unicode block chars, left-aligned). **Once (TTY)**: `\033[<N>C` (horizontal centering only — no vertical CUP), no cursor hide/show, no reset. **Once (non-TTY)**: no escapes at all. | **Loop (TTY)**: same as ASCII — `\033[%d;0H` + horizontal centering, cursor hide, DCS envelope. **Loop (non-TTY)**: sixel DCS envelope emitted (image format, not positioning), all other escapes suppressed. **Once (TTY)**: `\033[<N>C` only (horizontal centering — no vertical CUP, no cursor hide/show), DCS envelope. **Once (non-TTY)**: DCS envelope only, no escapes. On exit (signal handler or normal cleanup) if sixel mode and TTY, emit `\033\\` (ST) to terminate any in-progress sixel sequence. |
| Sidebar panel | *(no sidebar)* | Clear-to-right `\033[K` — each line is positioned at `info_col`+1, then `\033[K` clears to end of line before content is written. No trailing spaces. `\033[K` avoids line-wrapping at the terminal right margin caused by DECAWM (auto-wrap) when trailing spaces reach the last column. | Trailing spaces to `info_w` width — the sixel image occupies the full terminal area, so spaces are consumed as part of the pixel canvas without visible wrapping. |

## Machine Monitoring — Data Sources

All reads are from `/proc` and `/sys` — no external binaries.

| Metric | Source | Conditional |
|--------|--------|-------------|
| **CPU usage** | `/proc/stat` — delta of total vs idle CPU time over 1s interval. Formula: `100 - (idle_delta * 100 / total_delta)` | CPU widget active |
| **CPU load** | `sysinfo().loads[0]` — 1-minute average (scaled by LOAD_INT/65536), divided by num_cpus × 100 for percentage display | CPU widget active |
| **CPU freq / governor** | Iterate `/sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq`, sum and average; same for `scaling_max_freq`. Governor only from cpu0's `scaling_governor`. | CPU widget active |
| **I/O wait** | `/proc/stat` — `iowait` delta over 1s interval | CPU widget active |
| **Memory usage** | `/proc/meminfo` — `MemTotal`, `MemAvailable` | MEM widget active |
| **Container memory** | cgroup v2: `/sys/fs/cgroup/memory.current` + `memory.max`; cgroup v1: `/sys/fs/cgroup/memory/memory.usage_in_bytes` + `memory.limit_in_bytes`. Check existence with `access(file, F_OK)`. Raw cgroup values are in **bytes**; divide by 1024 to store in **kB** (`ctr_used_kb`, `ctr_max_kb`). When `memory.current` exists but `memory.max` is `"max"` (v2 unlimited), set `ctr_max_kb` directly from `MemTotal` (already parsed from `/proc/meminfo` in kB — no multiply/divide needed). When `memory.limit_in_bytes` is `-1` (`0xFFFFFFFFFFFFFFFF` as unsigned, v1 unlimited) or a value >= `CGROUP_UNLIMITED`, treat the same way. Display formatting divides by 1024.0 to convert kB → MB/GB per [Data-size thresholds](#data-size-thresholds).
| **Battery** | First tick: discover via `glob("/sys/class/power_supply/*/type")`, pick first with value `Battery`. Cache the prefix path. If none found, `bat_pct` stays -1 and BAT line is omitted. Subsequent ticks: use the cached prefix directly, no re-discovery. Read `charge_now` (fallback `energy_now`) for raw µAh value each tick. Read `charge_full` (fallback `energy_full`) on first tick and cache in `bat_charge_full_raw`; re-read when state is Full/Not-charging and `bat_pct != 100`. Compute display percentage as `(charge_now * 100) / charge_full`. Read `status`. Icons per [Battery icons](#battery-icons). On each tick, record raw charge and wall-clock timestamp (`time(0)`) into FIFO samples per [Battery time estimation](#battery-time-estimation). | BAT widget active |
| **Network throughput** | rtnetlink `RTM_GETLINK` — first tick (or route-change event) does a full dump of ALL interfaces populating the **link cache** (`r->links[]`). Steady-state ticks send targeted `RTM_GETLINK` queries for only the SELECTED interfaces (at most 3). `rtnl_read_dev()` reads cached `rx_bytes`/`tx_bytes` from `IFLA_STATS64` and computes deltas against the previous tick's raw values. | NET widget active |
| **Wired link speed** | `ETHTOOL_GLINKSETTINGS` ioctl via `SIOCETHTOOL` — `resolve_iface_speed()` issues a two-phase ioctl (query required `link_mode_masks_nwords`, then full query). On speed=0 or probe failure, falls back to `ETHTOOL_GSET`. Speed is lazily fetched on first access (or after link-event cache eviction) and cached in `link_entry.speed`. A probe failure is cached per-entry to skip the two-phase path on subsequent ticks. Returns speed in Mb/s. Format per [Format Rules — Wired link speed](#wired-link-speed). Shown after throughput on the NIC line. | NET widget active |
| **Wireless signal** | netlink nl80211 `NL80211_CMD_GET_STATION` — signal strength in dBm. Only for the selected wlan NIC (at most one, placed last on its own line). | NET widget active |
| **Wireless link speed** | netlink nl80211 — `NL80211_CMD_GET_STATION` for tx_bitrate and rx_bitrate. Queried only for the selected wlan NIC. Requires a persistent `NETLINK_GENERIC` socket held across intervals. Format per [Format Rules — Wireless link rate](#wireless-link-rate). | NET widget active |
  | **Wireless SSID** | netlink nl80211 — `NL80211_CMD_GET_INTERFACE` returns SSID. Shown on a separate line prefixed with `SSID`, after the signal/dBm line, with band rate appended. Fallback: interface name. SSID is re-queried when the main thread processes an address or link action from the monitor FIFO that triggers a local IP refresh (the same events that cause WAN re-fetch). | NET widget active |
| **Storage usage** | Parse `/proc/self/mountinfo` for mounted filesystems. Cache the parsed mount list and skip `statvfs()` for 29 of every 30 ticks; only re-run `statvfs()` every 30th tick. On mountinfo change (re-read every tick), re-filter virtual mounts and re-discover device mappings immediately. Each mount on its own info bar line. | STO widget active |
| **Storage throughput** | Per device (not per partition). Read `/sys/block/*/stat`. Field 3 (read sectors), field 7 (write sectors). Compute delta over 1s, ×512 → bytes/s. `/sys/block/` only lists bare block devices (sdX, nvme0n1) — partitions are never present. Skip virtual (ram, loop, dm-, zram, md). | STO widget active |
| **Uptime** | `sysinfo().uptime` | UP widget active |
| **Date / calendar** | `localtime_r` + precomputed month calendar grid. `--sunday-start` flips week start. | DATE or CAL widget active |
| **Public IPv4** | HTTP GET `api.ipify.org:80` — fully async: DNS resolve, TCP connect, HTTP send, response recv all non-blocking via `poll()` in main loop | NET widget active |
| **Public IPv6** | HTTP GET `api6.ipify.org:80` — same async pattern | NET widget active |
| **Local IP** | IPv4 via `SIOCGIFADDR` ioctl on the selected interface. IPv6 via rtnetlink `RTM_GETADDR` (`rtnl_find_addr6`). Default routes (v4 + v6) via rtnetlink `RTM_GETROUTE` (`rtnl_default_v4`, `rtnl_default_v6`). Refreshed on rtnetlink address/link events via background thread. On change, triggers public IP re-fetch. | NET widget active |
| **Weather** | HTTP GET `wttr.in/?format=j1` — fully async DNS resolve, TCP connect, HTTP send, response recv. JSON parsed for current temp, today's max/min, and condition name | WEATHER widget active |
| **CPU temperature** | `/sys/bus/pci/drivers/k10temp/*/hwmon/hwmon*/temp1_input` or `/sys/devices/platform/coretemp.0/hwmon/hwmon*/temp1_input` | CPU widget active |
| **Storage temperature** | `/sys/block/nvme*n1/device/hwmon*/temp*_input` (NVMe only). Probe via `glob()` — the path contains a wildcard suffix (`hwmon*/temp*_input`) so `access()` on the pattern alone would fail. Cache the resolved path on success; mark absent and skip further probing on failure. NVMe hwmon path may be absent on some controllers. SCSI/SATA storage temperature is not supported (no hwmon). | STO widget active |
| **GPU usage** | `/sys/class/drm/card*/device/gpu_busy_percent` or `/sys/class/drm/card*/power/rc6_residency_ms` (delta). Hot-pluggable eGPUs not supported — probe once at startup, never re-probe. On the first tick, check existence with `access()` (or equivalent probe). Cache per-file presence flags — files that don't exist on first tick are never retried. Subsequent ticks: read only files confirmed present. If absent, GPU usage is omitted. | GPU widget active |
| **GPU core clock** | `/sys/class/drm/card*/device/pp_dpm_sclk` (active + max from `*` marker / last line). Hot-pluggable eGPUs not supported — probe once at startup, never re-probe. First tick: `access()` to detect AMD vs Intel flow (`gt_act_freq_mhz`/`gt_max_freq_mhz`). Cache per-file presence flags — files not found are never retried. Subsequent ticks: read only confirmed present files. If both absent, clock line omitted. | GPU widget active |
| **GPU memory clock** | `/sys/class/drm/card*/device/pp_dpm_mclk` (active + max from `*` marker / last line). Hot-pluggable eGPUs not supported — probe once at startup, never re-probe. First tick: `access()` to check existence. Cache presence flag — never retried on failure. Subsequent ticks: read only if confirmed present. If absent, memory clock omitted. | GPU widget active |
| **GPU memory usage** | `/sys/class/drm/card*/device/mem_info_vis_vram_used`, `mem_info_vis_vram_total`. Hot-pluggable eGPUs not supported — probe once at startup, never re-probe. First tick: `access()`. Cache presence flag — never retried on failure. Subsequent ticks: read only if confirmed present. If absent, VRAM line omitted entirely. | GPU widget active |
| **GPU temperature** | `/sys/class/drm/card*/device/hwmon/hwmon*/temp1_input`. Hot-pluggable eGPUs not supported — probe once at startup, never re-probe. First tick: discover path via `access()` or `glob()`. Cache path + presence flag — never retried on failure. Subsequent ticks: read only if confirmed present. | GPU widget active |
| **GPU fan** | `/sys/class/drm/card*/device/hwmon/hwmon*/fan1_input`. Hot-pluggable eGPUs not supported — probe once at startup, never re-probe. First tick: discover via `access()` or `glob()`. Cache path + presence flag — never retried on failure. Subsequent ticks: read only if confirmed present. | GPU widget active |
| **GPU governor** | `/sys/class/drm/card*/device/power_dpm_force_performance_level`. Hot-pluggable eGPUs not supported — probe once at startup, never re-probe. First tick: `access()`. Cache presence flag — never retried on failure. Subsequent ticks: read only if confirmed present. | GPU widget active |
| **Motherboard fans** | Discovery order below. First tick: locate the hwmon directory via the discovery glob, then enumerate `*_input` files within it and cache the full file paths. Subsequent ticks: only `read_uint()` on each cached path — no `glob()`, no `malloc()`/`free()` of the entry array, no filesystem re-enumeration. Sorted before temperatures — all fan entries emitted first on the fan line, then temperatures on a separate line. | FAN widget active |
| **Motherboard temperatures** | Scanned in the same `*_input` glob pass as fans on the first tick; identical per-path caching. Subsequent ticks: only `read_uint()` on each cached path. Emitted after all fan entries, separated by a newline when both groups are present. | FAN widget active |

### Hardware sensor discovery order

1. **GPU** — check `/sys/class/drm/card*` entries. Cards are sorted numerically
   (`card2` before `card12`, not lexicographic). Only when GPU widget is active.
2. **CPU temperature** — check `/sys/bus/pci/drivers/k10temp` first, fallback to `/sys/devices/platform/coretemp.0`. Only when CPU widget is active.
3. **Motherboard sensors** (fans + temps) — only when FAN widget is active. Check in order:
   - `/sys/bus/wmi/drivers/dell_smm_hwmon` (Dell — look for device subdirectory, then its `hwmon/` child)
   - `/sys/devices/platform/asus-nb-wmi` (ASUS — direct `hwmon/` child)
   - `/sys/bus/platform/drivers/nct6687` (NCT6687 — only if the module is already loaded; do not `modprobe`)
   - `/sys/class/hwmon/hwmon*` (fallback — enumerate all hwmon class devices; read each device's `name` file and skip known non-motherboard drivers: `k10temp`, `coretemp`, `nvme`, `acpitz`, `amdgpu`, `nouveau`, `thinkpad`, `pmbus`, etc.)
4. For each motherboard hwmon directory found, collect `*_input` entries (fan and temperature files discovered in a single glob).
5. **FAN widget output order**: all fan entries **across all motherboard hwmon directories** space-separated on one line with a single `FAN` prefix before the first (`FAN NNNNRPM RRRRPM ...`), then a newline, then all temperature entries **across all directories** space-separated on the next line (`NN°C NN°C ...`). No separator when only fans or only temperatures are present. In other words, the hwmon directories are not rendered as separate blocks — fans are aggregated into one line, temperatures into a second line.

## Network status logic

- If a network request fails (timeout, connection refused): in continuous
  mode, **omit** the WAN/WAN6 line entirely (do not emit the line) and
  schedule a retry with exponential backoff. Each target (wan4, wan6,
  weather) has a single retry counter (`wan{4,6,try}`, `weather_try`) and
  a single retry timestamp (`retry_ts`). On any failure (DNS, connect,
  or HTTP), the counter is incremented and the target is abandoned on the
  third failure. Backoff starts at 4 s and doubles each attempt (4 s, 8 s,
  16 s). In `--once` mode, wait for the response (no
  placeholder needed).
- On a local IP change (detected by the main thread processing an address
  action from the [monitor FIFO](#monitor-thread-action-fifo)): if the new
  local IP differs from the cached value,
  cancel any in-flight requests (DNS and HTTP) per the cancellation rule below,
  reset all try counters, and start fresh DNS for both v4 and v6 immediately.
  The bootstrap DNS (started by `start_net_fetch`) is never cancelled by the
  initial bootstrap actions pushed by `rtnl_open_monitor` — `ips_changed`
  returns 0 when the previous IP was empty, so no WAN refetch is triggered on
  first-time initialization. Genuine IP changes during an in-flight fetch are
  deferred until the current fetch completes — the stale result will be
  overwritten by the next refresh cycle. If the new IP equals the cached value
  (duplicate/spurious event), no action is taken. The cancellation ensures
  that a pending stale result does not overwrite the fresh one — the in-flight
  DNS threads are **cancelled** via `pthread_cancel` + detach,
  `ctl.wan_cancel=1` under mutex closes stale socket fds in the I/O thread's
  next poll iteration, and fresh DNS resolution starts immediately. This
  ensures the displayed WAN IP always reflects the current network attachment,
  even during flapping.
- **Public IP overlap detection**: WAN (public IPv4) is compared against
  local IPv4. WAN6 (public IPv6) is compared against local IPv6. If a
  public IP matches its local counterpart, the local IP line changes its
  prefix/label to indicate the overlap. When they match, the redundant
  WAN/WAN6 line is omitted — the IP line already bears the WAN address.
  Line order is always: IPv4, IPv6, WAN, WAN6 (each omitted when empty).
  IPv4 and IPv6 comparisons are independent; a mismatch on one family
  does not affect the other.
- **Link cache** (`r->links[]`): per-tick storage for interface metadata
  and throughput counters. Each entry:

  ```
  struct link_entry:
    ifindex         // kernel interface index
    name            // IFLA_IFNAME
    is_virtual      // 1 = virtual (IFLA_INFO_KIND or lo)
    speed           // ethtool Mb/s, -1 = not yet fetched (lazy)
    rx              // IFLA_STATS64 rx_bytes
    tx              // IFLA_STATS64 tx_bytes
  ```

  Two population modes:

  **Full dump** (first tick, or route-change / link-add / link-remove
  event): a single `RTM_GETLINK` dump with `NLM_F_DUMP` queries ALL
  interfaces. Each response is parsed into a cache entry holding
  `ifindex`, `name`, `is_virtual`, `rx`, `tx`. The `speed` field is set
  to -1 (lazy) for all entries.
  After the full dump the cache is flagged as `links_stale = 0`.

  **Targeted queries** (steady-state ticks): instead of a full dump,
  send individual `RTM_GETLINK` requests (`NLM_F_REQUEST`, no dump)
  with `ifi_index` set for each SELECTED interface only (the NICs in
  `c->ifindex[0..count-1]`, at most 3). Each response updates that
  entry's `rx` / `tx`. Virtual metadata and `speed` are NOT re-fetched
  — they are static or lazy from the full dump.
  
  **Refresh trigger**: the main thread's FIFO processing (see
  [§Main thread processing](#main-thread-processing-per-tick-in-get_net_info))
  sets per-event flags:
  - `MON_LINK_ADD` → `links_stale = 1` (full link dump needed to discover the
    new NIC)
  - `MON_LINK_REMOVE` → `needs_reprimary = 1` (interface is already removed
    locally — no dump needed, just re-evaluate primaries from existing data)
  - `MON_ROUTE_ADD` / `MON_ROUTE_REMOVE` (default route, `RT_TABLE_MAIN`,
    `dst_len == 0`) → `needs_route = 1` (re-run route dump inside
    `pick_primary` — no link dump needed).

  The next tick checks these flags and dispatches the corresponding work.

  The cache is the single source for the `pick_primary` virtual-
  interface filter, for throughput delta computation in
  `rtnl_read_dev` (which uses cached raw bytes, no second dump), and
  for lazy ethtool link speed (cached in
  `speed`, re-fetched only after link-event cache eviction).
  The wireless station rate (`wlan_rx_rate`, `wlan_tx_rate`) is fetched
  separately via nl80211 `NL80211_CMD_GET_STATION` — not part of the
  link cache.
- **ifindex↔ifname resolution cache** (`name_idx`): a per-tick
  bidirectional lookup table that eliminates redundant
  `ioctl(SIOCGIFNAME)` / `ioctl(SIOCGIFINDEX)` calls. In a single tick
  the same interface pair is resolved from multiple call sites:
  `update_best` (route dump callback for each address family),
  `pickup_gw_name`. Without caching, these
  produce 2× `SIOCGIFNAME` and 2× `SIOCGIFINDEX` for "wlan0" per tick.

  ```
  struct name_idx:
    name       // interface name string
    ifindex    // interface index
  ```

  The cache is a dynamic array grown by a stepped doubling formula:
  `step = cap > 65536 ? 65536 : cap > 64 ? cap : 64; new_cap = cap + step`,
  then 64-byte aligned. Initial allocation on first use is 64 entries
  (from `cap = 0 → step = 64 → new_cap = 64`). This handles systems
  with many distinct interfaces (bonding, containers, virtual NICs)
  without silently dropping entries.

  Resolution rules:

  1. On `ifindex→name`: scan cache for `ifindex`. If found, return
     cached `name` — no ioctl.
  2. On `name→ifindex`: scan cache for `name`. If found, return cached
     `ifindex` — no ioctl.
  3. On first miss: call the ioctl (`SIOCGIFNAME` or `SIOCGIFINDEX`),
     grow the cache array if at capacity, store the pair, return the
     result.
   4. Cache persists across ticks. When a `LINK_REMOVE` action arrives
      from the monitor thread, the matching entry is removed and the
      array is compacted (trailing entries shifted down via `memmove`,
      `name_idx_n` decremented). When the array is sufficiently sparse
      (e.g. `name_idx_n ≤ name_idx_cap / 2` and `name_idx_cap > 64`),
      the heap allocation is shrink-wrapped with `realloc`. No clearing
      on route changes — a removed interface is the only path that
      evicts an entry. Between calls within the same tick, every
      already-resolved pair costs zero syscalls.

  Replaces the following raw syscall sites:
  - `sys_if_indextoname()` in `rtnl_route.c:update_best`
  - `sys_if_nametoindex()` in `net_route.c:pickup_gw_name`

  Verification: after implementation, a single tick must issue at most
  ONE `SIOCGIFNAME` AND ONE `SIOCGIFINDEX` per distinct interface
  (down from 4 total for "wlan0").
- **Default gateway selection**: use rtnetlink `RTM_GETROUTE` dump
  (`rtnl_default_v4`, `rtnl_default_v6`) restricted to the main
  routing table (`RT_TABLE_MAIN`) to find default
  routes for each address family independently. Custom-table or
  local-table routes are never considered.
- **Primary NICs**: at most three interfaces may be shown, with
  deduplication:
  1. Interface of the default IPv4 route — always shown when a v4
     default route exists.
  2. Interface of the default IPv6 route — shown only when it differs
     from the IPv4 route interface.
  3. Wireless interface — shown at the last position when one is
     active and not already listed.
  
  If all three slots refer to the same interface (e.g. a wireless
  NIC carries both v4 and v6 default routes), only that single NIC is
  shown. The maximum of three occurs when v4, v6, and wireless are
  three distinct interfaces.
- **Virtual NIC filtering**: after `gather_routes` fills `gw_idx[]`
  from the main-table route dump, iterate the candidate ifindices
  and look up each one in the link cache (`r->links[].is_virtual`).
  If **at least one physical interface** is found in the candidate
  set, remove all virtual interfaces from `gw_idx[]` and compact the
  array. If **no physical interface** is found (e.g. inside a
  container where all interfaces are virtual), keep all virtual
  interfaces. The filtered set then determines the primary NIC
  slot and serves as the `gw_idx` for wireless route matching.
  Wireless route matching uses the same filtered `gw_idx` — a
  wireless NIC behind a virtual interface is only considered when no
  physical interface exists.
- **Wireless NIC selection**: the RTM link dump alone cannot identify
  wireless interfaces — `IFLA_INFO_KIND` is driver-dependent and often
  absent, and `ifi_type` is `ARPHRD_ETHER` (same as wired). A full
  `NL80211_CMD_GET_INTERFACE` dump is used to enumerate all
  station-mode wireless NICs in one call. The dump result is cached
  with a `fresh` flag.
  
  Events trigger the following refresh strategy:
  - **Initial / NIC attach/detach**: perform a full nl80211 dump
    (`fresh = true`). Match the listed wireless interfaces against the
    route table's `gw_idx[]` to find the one with the lowest-metric
    (highest-priority) route — this is the selected wireless NIC. Its
    SSID is read directly from the dump response.
  - **Route change only** (no NIC add/remove): the cached dump is still
    valid for identifying which interfaces are wireless, so no fresh
    nl80211 dump is needed. Re-match `gw_idx[]` against the cached
    dump to pick the wireless NIC. The SSID from the cached dump is
    reused (may be stale, but SSID only changes when the NIC
    reconnects, which also changes the local IP).
  - **Local IP change**: a fresh SSID is needed. Perform a single
    targeted `NL80211_CMD_GET_INTERFACE` query (`nlk_wlan_ssid`) on
    the already-known wireless NIC to update the SSID.
  
  After the initial selection, `fresh` is set to `false`. A fresh dump
  only occurs again when interfaces are added or removed.
  
  At most **one wireless NIC** is shown, always placed after all
  non-wireless NICs on its own line (no blank line separation).
  The wireless NIC may be the same interface as the IPv4/IPv6 primary,
  or a different one.
- **Link speed (ethtool)**: fetched lazily via `ETHTOOL_GLINKSETTINGS`
  ioctl only for the primary wired NICs. Wireless NIC speed comes from
  nl80211 `NL80211_CMD_GET_STATION`, not ethtool. Cached in
  `link_entry.speed` (starts at -1 = not yet fetched). On first access
  per link lifetime: try `ETHTOOL_GLINKSETTINGS` (two-phase if
  supported), fall back to `ETHTOOL_GSET`. On total failure set
  `speed = 0` — stops retrying. Result is cached until the next link
  event triggers full cache eviction (`speed = -1` for all entries).
  No further ioctl while the link stays up (`speed >= 0` skips
  probe). The display reads speed from the link cache entry and
  stores it in `c->link_mbps[i]` for formatting.
- **Line break**: each NIC appears on its own line. No blank lines
  between NICs.
- **Rendering**: the wired lines loop (skipping the wireless index)
  outputs up to 2 wired NICs. The wireless line output adds 1 more.
  Total output is at most 3 NIC lines.

## Storage

The STO widget shows three kinds of information per device:

**Usage** (per mount): parse `/proc/self/mountinfo` every tick into a **single
retained dynamic array** of `struct mount` entries (capacity kept across ticks —
not freed per tick). The mount array is the authoritative store of both parsed
mountinfo fields and `statvfs()` results.

```
struct mount:
  mnt_id           // mount ID from field 1 (identity key)
  maj, min         // major:minor from field 3
  consumed         // 1 if associated with a block device this tick
  total, free      // total, free bytes (MB) from statvfs
  mntpt            // mount point from field 5
  statvfs_tick     // countdown: ticks remaining until statvfs refresh
```

Each tick applies a **delta** to the retained array:

1. Parse `/proc/self/mountinfo` (filtering out virtual filesystems from the
   skip list) into a temporary line buffer.
2. For each parsed entry, match against the retained array by `mnt_id`
   as the identity key:
   - **Already present**: keep the entry as-is (its `total`/`free` persist).
   - **New** (not in the array): append the entry, set `statvfs_tick = 30`,
     then call `statvfs()` on it immediately.
   - **Removed** (in the array but absent from mountinfo): delete the entry
     and compact the array.
3. After delta processing, iterate the array and decrement every entry's
   `statvfs_tick`. When it reaches 0, run `statvfs()` and reset to 30.
   Each mount gets its own independent refresh window.

The array starts with 64 slots, grows on demand (`realloc` doubling), and
**shrinks** when entries are removed (compaction by `memmove`, followed by
`realloc` to a smaller capacity when utilisation drops below a threshold).
Each tick filters virtual filesystems (`proc`, `sysfs`, `tmpfs`,
`devtmpfs`, `cgroup*`, `debugfs`, `pstore`, `securityfs`,
`hugetlbfs`, `configfs`, `efivarfs`, `bpf`, `autofs`, `overlay`,
`squashfs`, `devpts`, `mqueue`, plus any mount with major number 7
(loop devices)), and re-matches mounts to block devices.

Format per [Data-size thresholds](#data-size-thresholds), suffix on total only.

**Throughput** (per device): read `/sys/block/*/stat` once per tick (filtering
only the selected devices after enumeration), extract field 3 (read sectors),
field 7 (write sectors). Delta over 1s, ×512 → bytes/s. `/sys/block/` only
lists bare block devices (sdX, nvme0n1) — partitions never appear here, so
no partition filtering is needed. Filter virtual devices by checking the
symlink target of `/sys/block/<name>`: skip devices whose link target starts
with `../devices/virtual/block/` (loop, dm-, zram, md).

Block device data is stored as a persistent dynamic array:

```
struct dev_out:
  name             // device name (e.g. "sda")
  major, minor     // from /sys/block/<name>/dev, read once at enumeration
  is_virtual       // cached from link target check in enumeration
  has_temp         // temperature path found
  temp_path        // cached temperature hwmon path
  rp, wp           // read/write sectors for delta computation
  prev_rp, prev_wp // previous tick's raw sector count
  size             // total size in bytes, read from /sys/block/<name>/size at enumeration
```

On each tick, `glob("/sys/block/*")` enumerates the current device set and
compares it against the cached array. Devices present in the glob but absent
from the cache are **added** (full probe: major:minor, virtual check, hwmon
path, size). Devices present in the cache but absent from the glob are
**removed** (entry removed, array compacted). The array order follows the glob
order. Devices present in both are kept intact — no re-probe of major:minor,
virtual status, hwmon path, or size.

`rp/wp` are read every tick for all cached devices from `/sys/block/<name>/stat`.
The previous tick's raw values are kept for delta computation.

Show as `↓read↑write`.
Unit per [Data-size thresholds](#data-size-thresholds) (b→K→M chain).
Throughput counters are only updated for selected physical (non-virtual) devices.
Virtual devices never show throughput.

**Temperature** (per device): probe `/sys/block/<name>/device/hwmon*/temp*_input`
regardless of hwmon name or path prefix. Check existence with `access()`.
Shown as `NN°C` per [Format Rules](#temperature-display) before device size on
the same device line.

**Device size** (per device): read `/sys/block/<name>/size` once at device
enumeration (when `d->cnt = 0` triggers re-discovery on token change). The
file contains total size in 512-byte sectors. Convert to bytes on read and
cache in `dev_out.size`. Do not re-read per tick — only on redump.

Shown after temperature (or after device name when no temp) and before
throughput. Format per [Data-size thresholds](#data-size-thresholds) using
the same M→G→T chain as STO usage: convert sectors×512 to MB (divide by
`BYTES_PER_MB`), then apply the M→G→T unit selection. Devices with
`size == 0` (virtual or probe failure) omit the size field.

Format: `<icon|STO> <device> [temp] <size> ↓read↑write`

Examples:
```
<icon|STO> sda 256.0G ↓2.0K↑1.0K
<icon|STO> nvme0n1 45°C 512.0G ↓250.0↑180.0M
```

Each physical device appears on its own line with the `<icon|STO>` prefix,
followed by its associated mounts (indented lines). A mount is associated
with a block device when its major:minor (field 3) matches the device's
major number and its minor falls within the device's minor range (i.e.
partitions and the device itself share the same major). Mounts that have no
matching physical device (e.g. on dm-crypt, loop, or other virtual backing)
appear before any device line, without icon prefix — just indented mount
lines as a group. Multiple devices are separated by line breaks (not blank
lines) when both have content. Physical block devices are always shown even
without active mounts, since direct I/O may occur.

**Mount deduplication**: When two or more mount entries share the same
major:minor device identifier (one partition mounted at multiple paths, e.g.
bind mounts), only the first occurring mount point for that device pair
is displayed. Subsequent mounts with the same (major, minor) are silently
dropped. Deduplication happens during mountinfo parsing, before device
association — the `struct mount` array contains at most one entry per unique
(major, minor) pair.

Example layout:
```
  /mnt/crypt   65%  200/300G          ← unmatched mount, no icon
<icon|STO> sda 256G ↓2K↑1K           ← physical device with icon, no temp
  /boot        30%  120/400M          ← matched mount, indented
<icon|STO> nvme0n1 45°C 512G ↓250↑180M
  /            62%  120/200G
  /home        81%  820/1T

## Flow / Lifecycle

  1. **Startup** — parse CLI args (mode + flags). If mode is `auto`, the
     `isatty(stdout)` check runs here — if not a TTY → text mode immediately.
     If a TTY, mode stays `auto` and the DA1 query is deferred to step 2 (inside
     `setup_terminal`). If mode was given explicitly (`text`, `ascii`, `sixel`),
     keep it as-is — no DA1 query or sixel probing is ever performed for
     explicitly-chosen modes. `isatty()` is called once and the result cached
     for downstream cursor/echo decisions.
     Build the active widget set per [Widgets — Default mode](#default-mode-no---widgets)
     or [Custom mode](#custom-mode---widgetslist).
     Init `struct ioserv_ctl` (mutex, efd=-1, flags=0). No I/O thread yet.
     Read the initial terminal window size via `ioctl(TIOCGWINSZ)` and compute
     layout for the info bar width. This read happens before the DA1 query
     because it only needs dimensions, not pixel geometry — the result is
     sufficient for the initial layout computation.
 2. **Terminal setup** (TTY only, not `--once` or text mode) —
    receives the mutable `enum mode *`:
    a. If `*mode == MODE_AUTO`: run the DA1 query (`\033[c`) with a temporary
       termios save/restore that disables ICANON + ECHO for the brief query
       window. Parse the terminal response — sixel-capable → set `*mode` to
       `MODE_SIXEL`, otherwise → `MODE_ASCII`.
    b. Emit RIS (`\033c`, full terminal reset), hide cursor.
    c. Save original termios (`d->saved_termios`), disable ECHO on the live
       termios. If resolved mode is `MODE_SIXEL`, additionally disable ICANON
       (the sixel pixel query in the next step needs raw input).
     d. Re-read the terminal window size via `ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws)`
        (the initial read happened during startup). For sixel mode, if the ioctl
        returns zero pixel dimensions
       (`ws_xpixel == 0 || ws_ypixel == 0`), query the terminal via the
       `\033[14t` escape sequence for window pixel geometry
       (response: `\033[4;{height};{width}t`). Character cell dimensions are
       derived as `cw = pixel_width / wscol`, `ch = pixel_height / wsrow`.
       If both TIOCGWINSZ and the escape yield zero dimensions, fall back to
       `cw = 5`, `ch = 9`. In text mode TIOCGWINSZ is **skipped entirely** —
       the hardcoded default (80×24) is used directly. If `ioctl` fails
       (non-TTY or any other error), fall back to 80×24.
    e. Register signal handler with `sigaction`.
  3. **Main loop** (unconditional single pass if `--once`):
     a. The tick target time is already known from the previous iteration
        (initial value from `time(NULL)` at startup). At the end of each tick,
        `wait_next_tick` calls `clock_gettime(CLOCK_REALTIME, ...)`, computes
        `target = tv_sec + 1`, and `nanosleep`s until `CLOCK_REALTIME` reaches
        that target. The returned `target` becomes the `now` for the next tick.
    b. Convert to broken-down time via `localtime_r` (reentrant, thread-safe).
     c. For each active widget, collect its metrics. Skip widgets not in the active set — their data sources are never read.
        d. Drain the monitor action FIFO (trylock loop: up to 10 attempts,
           10ms `nanosleep` between each; no `clock_gettime` — see
           CONTRIBUTING.md §Design Principles for the "exactly three
           time-source calls" rule). If all 10 fail, skip this tick.
           Aggregate events and apply per [the processing
           rules](#main-thread-processing-per-tick). Then run `poll_refresh`
          and `pick_primary`, setting `addr4_changed`/`addr6_changed` after
          `pick_primary` per [the post-pick_primary rule](#main-thread-processing-per-tick).
          If the processing sets `addr4_changed` or `addr6_changed` (address
          add/del actions), subsequently refresh local IPs. If a local IP
         changed from its cached value,
         cancel any in-flight DNS threads (`pthread_cancel` + detach), set
         `ctl.wan_cancel=1` (closes WAN sockets only — weather is NOT cancelled),
         then start both WAN IPv4 and WAN IPv6 DNS resolution immediately.
         A periodic weather refresh cancels weather's pending operations only
          (sets `ctl.weather_cancel=1`, closes weather fd only). The
           `once_cleanup_widgets` function cancels remaining connections
           via per-target calls: `io_cancel_all(ctx, CANCEL_WAN)` and
           `weather_cleanup` (which calls `io_cancel_all(ctx, CANCEL_WEATHER)`).
           The I/O thread checks `ctl.wan_cancel` and
           `ctl.weather_cancel` on every loop iteration;
           both flags tell the I/O thread which fds to close.
           Event-driven restarts bypass the exponential backoff timers.
      e. Per-target lifecycle. Each target (wan4, wan6, weather) has
         independent DNS, connection, and HTTP retry counters and timers.
         WAN v4 and WAN v6 are triggered together but may complete at
         different times.

         Cancellation rule: starting any new request for a target —
         whether triggered by periodic refresh, retry backoff expiry,
         or event-driven restart — MUST first cancel any pending
         operations for that same target without affecting other targets.
         Three targets (wan4, wan6, weather) each have independent lifecycle;
         cancellation scopes are:

           - **WAN refetch** (IP change, periodic WAN refresh): cancels
             wan4 + wan6 only — weather is untouched. Sets `ctl.wan_cancel=1`.

           - **Weather refresh** (periodic timer expiry): cancels weather
             only — WAN is untouched. Sets `ctl.weather_cancel=1`.

           - **Per-target retry** (backoff expiry for a single target):
             cancels only that one target's pending DNS + connection.
             No `ctl.*_cancel` flag — the main loop directly closes the
             individual connection fd and cancels the individual DNS slot.

            - **Once-mode widget cleanup** (`once_cleanup_widgets`): cancels
              only the active widget's connections (NET → WAN only;
              weather → weather only). Sets the appropriate `ctl.*_cancel` flag.

            - **Cleanup** (`cleanup_all` after loop exit): shuts down all
              I/O connections and cancels all DNS threads under normal
              mutex/thread operations, then restores terminal. Runs from
              `clock_main` after the main loop exits, regardless of whether
              exit was triggered by normal completion or by the
              `terminated` flag set by the signal handler.

         Two separate resources must be released per cancelled target:

           - **In-flight DNS**: set the DNS slot's `cancelled` flag
             under its mutex and terminate the running thread (via
             `pthread_cancel` on the slot's stored pthread_t). The DNS
             thread checks the cancelled flag after `getaddrinfo` returns and exits
             without writing a result. Subsequent `start_dns` clears the
             flag for the new thread.

             **Invariant**: `start_dns` unconditionally resets the
             slot (`state=DNS_RUNNING`, `cancelled=0`). Any previous
             result (state==DNS_DONE or DNS_ERROR) is discarded —
             `slot->addr` is a static struct so no leak, and the
             state change makes old data unreachable. Only the most
             recently started thread's result is accepted; a prior
             thread completing before `dns_cancel` sees `state!=1`
             and is simply eclipsed.

           - **In-flight HTTP (I/O thread side)**: close the connection
            socket fd stored in `wan4_fd`/`wan6_fd` (or `weather_fd`)
            and set the variable to -1. The I/O thread's `poll()` detects
            the fd change to -1 and cancels the connection — it closes
            its own copy, discards partial buffers, resets the connection
            phase, and writes an error result. Cancel the target's
            `http_result` slot (`cancelled=1`, `state=0`) so the I/O
            thread's discarded completion is ignored.

            No `eventfd` write is involved — the I/O thread checks
            `ctl.cancel_all` and per-connection fd changes on every
            loop iteration. The eventfd is exclusively for DNS threads
            to signal the I/O thread when a new DNS result is ready
            for consumption.

         Then start fresh DNS via `wan_dns_start()`. The new DNS
         completion's `ensure_io_thread()` naturally creates or wakes
         the I/O thread.

         Retry mechanism (separate from cancellation): on
         `dns_slot.state == -1` increment `dns_try`; on HTTP result
         reason 'C' increment `conn_try`; reason 'H' increment
         `http_try`. Each stage has exponential backoff. When backoff
         expires, the retry path follows the same cancellation-before-
         start rule above. After 3 failures per stage the target is
         abandoned until a network-change reset.

         Re-fetch public IP at `--ip-refresh` interval and weather at
         `--weather-refresh` interval (only if NET/WEATHER widget active).

         Main loop does no socket I/O — it reads `struct http_result`
          slots for completed fetch data. The I/O thread is created on
          demand: the first DNS thread to complete spawns it.
          Exception: the main loop MAY close socket fds directly
          during cancellation/restart and cleanup, but never performs
          connect/send/recv on them.
     f. (TTY only, not `--once`) Re-check terminal size via `ioctl(TIOCGWINSZ)`.
        If changed from the initial size (read at step 1), emit RIS and recompute
        layout. In `--once` mode this step is skipped — the single frame uses the
        initial layout read at startup.
       g. Render clock + sidebar in the selected mode. If sidebar is empty, clock uses
          full terminal width. In continuous mode the clock is vertically centered via
          `position_cursor(d->row)` and the sidebar is positioned at the top row (row 0).
          In `--once` mode the clock renders inline at the cursor position (no CUP);
           the sidebar is positioned at the clock's starting Y via relative cursor-up
           `\033[%dA` (see [`--once` mode](#--once-mode-deviation-from-continuous)).
           Sidebar lines are truncated to `wsrow` (continuous mode only) — any rows
           beyond the terminal height are silently dropped. A terminal resize to a smaller
           height causes the sidebar to shorten on the next tick; a resize to a larger
           height reveals previously truncated lines.
     h. If `tls_terminated` is set, break out of the loop.
     i. If `--once`, break out of the loop.
     j. Compute sleep: `nanosleep(1s - ns_elapsed)`.
    4. **Cleanup** (post-loop — always runs regardless of how the loop ended):
        `cleanup_all()` receives all state explicitly (widget mask,
        I/O control block, DNS slot pointers, terminal state, sixel
        mode, cursor state) as function arguments — no TLS access needed
        because it runs in main-thread context after the loop.

        1. **I/O connections**: acquire `ctl.lock`, set `ctl.shutdown=1`,
           close each I/O connection fd under the lock, close the eventfd.
           The I/O thread detects shutdown via POLLNVAL or the shutdown
           flag on its next poll iteration.
        2. **DNS threads**: call `dns_cancel()` on every DNS slot
           belonging to the active widget set. Each call acquires the
           slot's mutex and calls `pthread_cancel` + `pthread_detach`.
         3. **Terminal restore**: send sixel ST (`\033\\`), restore termios,
            show cursor, in that order. Sending sixel ST while termios is
            still in raw mode ensures the terminal interprets the escape
            sequence correctly.

       After `cleanup_all()` returns, `clock_main` returns `EXIT_SUCCESS`.

       No `atexit` is registered — the only cleanup path is the explicit
       `cleanup_all()` call before `return`.

        **Signal handler** (registered with `sigaction`, no `SA_RESETHAND`):

         - **First SIGINT/SIGTERM**: set `tls_terminated = 1`
           and return. The main loop detects the flag on its next
           iteration, breaks, and runs `cleanup_all()` from the main
           thread (not signal context). The `cleanup_all()` call uses
           normal mutex/thread operations — no async-signal-safety
           constraints apply because the signal handler itself performs
           only a single volatile write.
         - **Second SIGINT/SIGTERM** (while the first signal has already
           set the flag and the main loop is still exiting): call
           `_exit(128+sig)` directly to force termination. This is the
           nuclear option — the kernel reclaims all process resources.
           The flag check before `_exit()` prevents the handler from
           writing `tls_terminated` again when racing with cleanup code
           reading it.

### `--once` mode (deviation from continuous)

Instead of steps 3d–3i above, `--once` mode does:

  1. Start WAN v4, WAN v6, and weather DNS threads **in parallel** (only
     for widgets in the active set). WAN v4 and WAN v6 are always triggered
     regardless of local IP presence. Each target has an independent lifecycle
     — WAN v4 and WAN v6 are triggered at the same time but may resolve,
     connect, and receive responses on different schedules. No I/O thread
     yet — the first DNS to complete will create it.
  2. Enter a **tight poll loop** for up to **3 seconds total**:
     ```
     deadline = now + 3s
     while (now < deadline && !all_results_ready) {
         check each http_result.state
         remaining = deadline - now
         if (remaining > 0 && !all_results_ready)
             poll(NULL, 0, min(remaining_ms, 100))
     }
     ```
     The I/O thread (if created) handles all socket I/O in the background.
     Results are checked per-widget: weather readiness is independent; WAN
     readiness waits for both v4 and v6 when both local IPs exist, or skips
     a family when its local IP is absent. If neither family has a local IP,
     the WAN check passes immediately. The loop exits early when all active
     widgets are ready.
 3. Single `tick()` call renders once with whatever results are available.
       Inside `tick()`, local IP is refreshed via `refresh_local_ip_once`
       (calls `do_get_ip` for IPv4 and `get_local_ip6` for IPv6 directly,
       without `pick_primary`). If no default route was resolved, `rtnl.fd`
       stays 0 and `get_local_ip6` returns early — IPv6 is silently empty.
       This is done in `tick()` rather than before the poll loop because
       local IP uses `SIOCGIFADDR` (ioctl) and `RTM_GETADDR` (rtnetlink
       dump) — local syscalls that complete in microseconds, not network
       operations that benefit from the 3-second async window.
      When the output mode is `ascii` or `sixel` and stdout is a TTY, the clock
      renders inline at the cursor position (no `position_cursor` CUP for the
      clock). After the clock face lines are written the cursor sits below the
      clock. The sidebar is then positioned at the **same Y** as the clock's first
      row via relative cursor-up `\033[%dA`, placing sidebar content on the same
      terminal rows as the clock. Cursor-up inherently handles scrolling — no
      scroll-adjustment formula is needed. This distinguishes from the non-TTY path
      where widgets follow below the clock as plain text.
     Missing results are omitted (WAN IP empty, weather dash).
 4. Exit (skip the 1-second nanosleep loop).

## Retry Landscape

One retry counter per target (wan4, wan6, weather), max 3 attempts each,
with uniform exponential backoff:

- **Any failure** (DNS, connect, or HTTP): increment the target's single
  `try` counter. If `try < 3`: `retry_ts = now + (4 << try)` seconds
  (4s, 8s, 16s). If `try >= 3`: the target is abandoned — no further
  attempts until a network-change event resets the counter.

- **On success**: `try = 0` for that target.

- **Event-driven restart** (IP address or link change detected by main thread
  processing a monitor FIFO action): bypasses all
  backoff timers — resets `try = 0`, cancels in-flight operations, starts
  DNS immediately.

- `--once` mode: single attempt per fetch with a hard **3-second total
   deadline**. Uses the same cancellation mechanism (§3e rule) to release
   resources when the deadline expires or all results arrive — no new
   request follows. Missing results omitted. No retry.

- **Resource release on abandonment**: resources are released naturally
  as part of the failure path — the I/O thread closes the connection fd
  and frees the response buffer before writing the error result; the DNS
  thread has already exited. The main loop resets the result slot
  (`state=0`). No explicit cleanup beyond not retrying is needed. The
  try counter at 3 acts as a static sentinel — the target is permanently
  exhausted.

- System stats: read fresh on every cycle; no retry needed.
- GPU/sensor reads: read fresh every cycle; no retry needed.

## Dynamic Arrays (Grow / Shrink)

The codebase uses a pragmatic mix of fixed-size and dynamic storage:

- **Known fixed bounds**: selected interfaces (at most 3, `MAX_NET`), gateway
  indices (at most 3), DNS slots (3 static), HTTP result slots (3 static),
  battery charge samples (10 per direction × 2), wireless link rate,
  paths with known prefix (shortened from hardcoded `PATH_MAX`).
- **Dynamic (heap, preallocated with growth)**: routes and link dumps returned
  by netlink (variable number, may exceed any sensible static bound); disk
  storage entries (`/sys/block/*` enumeration, initial 16 slots, grows via
  `realloc` doubling, **retained** across ticks);
  mountinfo entries (`/proc/self/mountinfo` — one pass per tick, initial 64
  slots, grows via `realloc` doubling, **retained** across ticks so `statvfs()`
  results persist between refresh ticks — not freed per tick);
  motherboard hwmon entries (fan*_input, temp*_input — discovered once on the
  first tick, allocated exact-fit, persisted across ticks — no reallocation
  after discovery since sensor file set is static);
  any other data set whose count is not known at compile time.

  Retained dynamic arrays keep their capacity across ticks and only shrink
  when the data set is explicitly reset (e.g. mountinfo change, new device
  hotplug). The growth pattern avoids repeated `malloc`/`free` in the steady
  state. Motherboard hwmon entries are a special case: they use a fixed-size
  persistent array allocated exact-fit on the first tick — no growth formula
  is needed because the sensor file set does not change after boot.

  Data that is genuinely ephemeral and computed fresh each tick (disk I/O
  deltas, per-tick throughput) uses a stack-allocated or per-tick-allocated
  temporary that is freed at end of tick — no capacity retained.

  Retained dynamic arrays keep their capacity across ticks and only shrink
  when the data set is explicitly reset (e.g. mountinfo change, new device
  hotplug). The growth pattern avoids repeated `malloc`/`free` in the steady
  state. Motherboard hwmon entries are a special case: they use a fixed-size
  persistent array allocated exact-fit on the first tick — no growth formula
  is needed because the sensor file set does not change after boot.

### Growth algorithm

```
new_cap := cap + max(min(max(cap, 64), 65536), len + need - cap)
new_cap := new_cap + (63 - (new_cap - 1) % 64)   # or: new_cap := (new_cap + 63) & ~63
```

### Shrink trigger

```
if len <= cap / 4 then new_cap := min(cap / 2, floor_or_initial_value)
```

### Hash map capacity triggers

Grow when `len > cap * 0.75`, shrink when `len < cap * 0.25`.

## Performance

- For I/O, prefer epoll over poll; use non-blocking operations, batch processing, lock-free queues, and smart concurrency design.
- Pack structs, align data to cache lines, avoid false sharing. Implement small object pools with predictable allocation cost. Prefer in-place or zero-copy processing over buffered copies.
- For concurrency on hot paths, use lock-free or wait-free algorithms. Favour per-core queues, SPSC/MPSC ring buffers with power-of-two sizing, and single-writer rules.
- Use profiling (cycle counters, perf), ASAN/UBSAN in development, and static analysis for race and UB detection. Inline small functions on hot paths, reduce branches, favour branchless algorithms where they lower jitter.

## Thread / Worker / Parallelism Landscape

Background threads (all on-demand except netlink monitor):

1. **Netlink monitor thread** (`rtnl_mon_thread`): runs `poll(100)` on a
   dedicated `NETLINK_ROUTE` socket subscribed to `RTMGRP_IPV4_IFADDR |
   RTMGRP_IPV6_IFADDR | RTMGRP_LINK | RTMGRP_IPV4_ROUTE |
   RTMGRP_IPV6_ROUTE`. On each `recvmsg` the thread iterates netlink messages
   and appends typed actions to a mutex-protected FIFO (see
   [Monitor Thread Action FIFO](#monitor-thread-action-fifo)). The thread
   performs no conversion or decision-making beyond type detection and raw
   field extraction.
   Shutdown: main thread sets `m->stop = 1`, then closes the monitor socket
   (`m->fd = -1`). The thread's `poll(100)` timeout expires, detects the
    condition and exits → `pthread_join`.

2. **DNS resolver threads** (up to 3, one per target): each runs blocking
   `getaddrinfo()` (a POSIX cancellation point) and writes the result to a
   `struct dns_slot` under a mutex. On successful resolution, signals the
   I/O thread via `eventfd` (or creates it if none exists). Thread lifecycle:
   - Created joinable (default) by main loop in `start_dns`.
   - On **normal exit**: thread writes result under `slot->lock`, then calls
     `pthread_detach(pthread_self())` and returns — kernel reclaims resources.
    - On **cancel** (`dns_cancel`): acquires `slot->lock`, sets `cancelled=1`,
      calls `pthread_cancel(t)`, then `pthread_detach(t)`. If the thread already
      exited normally, `pthread_cancel` returns ESRCH and `pthread_detach` returns
      ESRCH (both harmless on Linux).
   - `slot->cancelled` flag (under `slot->lock`) prevents a stale DNS result
     from overwriting a fresh restart's state.
   - `dns_arg` (heap-allocated) is freed by cleanup handler on cancellation
     (during `getaddrinfo`), or by explicit `free(d)` on normal exit.
   - No `pthread_t` reuse race: `pthread_t` stored in slot before thread
     starts; `start_dns` asserts `slot->state == 0` (previous thread must
     have been consumed/released before starting a new one).

3. **I/O thread** (single, **on-demand**): created by the first DNS thread
   to complete. Created **detached** (`pthread_detach` right after create) —
   the kernel reclaims resources on exit; no `pthread_join` needed.
    Runs a `poll(100)`-based event loop on an `eventfd` + socket fds — blocks
    with near-zero CPU when waiting; eventfd write shortens wait to ≤100ms.
   **Exits** when all connections complete and all DNS completions have been
   consumed (`pending_work == 0`) (under `ctl.lock` — see race analysis below).
   DNS threads still running create a fresh I/O thread when they finish —
   on-demand creation is the normal pattern, not a special case.

   I/O thread loop:
   ```
    loop:
        lock(ctl)
        if ctl.shutdown:           unlock; goto shutdown
        if ctl.cancel_all:         close all fds; reset phases; ctl.cancel_all = 0
        if ctl.wan_cancel:         close WAN fds only; reset WAN phases; ctl.wan_cancel = 0
        if ctl.weather_cancel:     close weather fd only; reset weather phase; ctl.weather_cancel = 0
        ctl.pending_work = 0
        unlock(ctl)

        # Check DNS slots for new addresses
        for each conn where phase == 0:
            lock(conn.dns.lock)
            if conn.dns.state == 2:
                copy addr to stack
                detach DNS thread (conn.dns.thread)
                conn.dns.state = 0              # slot reusable
                unlock(conn.dns.lock)
                socket(), set non-blocking, connect(), phase = 1
            else:
                unlock(conn.dns.lock)

        # Exit if no work and no connections
        lock(ctl)
        if pending_work == 0 and !ctl.cancel_all and !ctl.wan_cancel and !ctl.weather_cancel and all phases == 0:
            ctl.io_thread_active = 0
            efd = ctl.efd; ctl.efd = -1
            unlock(ctl); close(efd); exit thread
        unlock(ctl)

        # poll(100) — block on eventfd + socket fds, near-zero CPU
        pfds = [ (efd, POLLIN) ]
        for each active conn: pfds += [ (conn.fd, POLLIN | POLLOUT) ]
        poll(pfds, 100)             # retry on EINTR; 100ms timeout allows poll-level
                                    # checks like ctl.shutdown or m->fd < 0 on exit

        # Consume eventfd
        if pfds[0] has POLLIN: read eventfd (8 bytes, discard)

        # Handle socket events — detect fd changes to -1
        for each conn:
            if conn.fd < 0:        # main loop closed this fd
                close own copy; phase = 0; discard buffers; continue
            if conn has no event:  continue
            if revents has POLLERR or POLLHUP or POLLNVAL:
                close(fd); phase = 0; http_result_write(error)
            else if revents has POLLOUT:
                if phase == 1:  connect done → send request, phase = 2
                else if phase == 2:  send done → phase = 3
            else if revents has POLLIN and phase == 3:
                recv → complete → parse → http_result_write(data), close, phase = 0

   shutdown:
       close(ctl.efd); ctl.io_thread_active = 0; exit thread
   ```

   Parse callbacks: WAN v4/v6 extract IP body from HTTP response; weather
   parses JSON for temp/conditions.

4. **Main loop** (continuous mode): never blocks on I/O. Each tick:
   - Drains the monitor FIFO via trylock loop (10 attempts × 10ms nanosleep;
     no clock_gettime — see CONTRIBUTING.md §Design Principles).
     aggregates events per [the processing rules](#main-thread-processing-per-tick).
     `addr4_changed`/`addr6_changed`, `r->links_stale`, or `c->needs_route`.
   - Reads `struct http_result` slots (mutex-protected, instantaneous).
   - If `state == 2`: copies data to `cpu_keep`, resets the target's try counter.
   - If `state == -1` (any error reason): increments the target's single `try`
     counter, sets `retry_ts = now + (4 << try)`.
   - Periodic refresh (WAN IP, weather): when timer expired and try
     counter < `RETRY_MAX`, per the cancellation rule above: cancel in-flight
     DNS (cancel thread + set `cancelled` flag), close the connection
     fd (`ctl.wan_cancel` for WAN or `ctl.weather_cancel` for weather),
     cancel the HTTP result slot, then start fresh DNS.
     - Event-driven restart (network change via monitor FIFO action): follows
     the same cancellation rule for WAN only, resets try counters to 0,
     then starts DNS for both v4 and v6. Bypasses all backoff timers.

### Thread creation lifecycle

```
Main loop               DNS thread              I/O thread
──────────              ──────────              ──────────
wan_dns_start()
  ── create thread ──▶ getaddrinfo() ...
                        (parallel ×3)
                                          [doesn't exist yet]
                         First to complete:
                           write slot
                           lock(ctl)
                           pending_work++
                           !io_thread_active
                             → create_thread() ──▶ io_thread_run():
                             → detach thread                lock(ctl)
                           unlock(ctl)                      efd = eventfd()
                                                             ctl.efd = efd
                                                             ctl.io_thread_active = 1
                                                             unlock(ctl)
                                                             check DNS slots
                                                             → found resolved
                                                             → detach DNS thread
                                                             → consume addr, state=0
                                                             → start connect
                                                              poll(100) ◀── blocked
                         Second to complete:        ↑
                           write slot               │ eventfd becomes readable
                           lock(ctl)                │ → wake up
                           pending_work++           │ → check slots
                           eventfd_write(efd) ──────┘ → detach DNS thread
                                                    → consume addr, start connect
                           unlock(ctl)
                                                    ...
                                                    all done, exit
                                                    io_thread_active = 0
                                                      close(efd), exit
Next refresh (main loop cancels pending work for this target per §3e rule):
cancel DNS thread + close(fd) + cancel result ──▶ wan_dns_start()
 ...      DNS completes ──▶ ensure_io_thread()
                                           ├── I/O thread active → eventfd_write (mutex-safe)
                                           └── I/O thread exited → create new thread
```

### Communication mechanisms

| Sender | Receiver | Mechanism | When |
|--------|----------|-----------|------|
| **Monitor thread** | **Main loop** | **`mon_action_fifo` (mutex-protected dynamic array, drained each tick)** | **Netlink event received (link/addr/route add/del)** |
| **Main loop** | **Monitor thread** | **`m->stop = 1` + `sys_close(m->fd)`** | **Shutdown** |
| DNS thread | I/O thread | `eventfd_write` | DNS resolved, new work ready — **exclusive** use of eventfd |
| DNS thread | (none) | `pthread_create` + `pthread_detach` | First DNS to complete creates I/O thread |
| Main loop | I/O thread | `ctl.cancel_all` / `ctl.wan_cancel` / `ctl.weather_cancel` / `ctl.shutdown` (context struct flags) + direct fd close (main closes socket fd, sets var to -1; I/O thread detects -1 on poll) | Cancel all / WAN-only / weather-only / shutdown — checked by I/O thread in `io_cmd()` on every loop iteration; per-connection fd changes trigger connection cancellation |
| Main loop | DNS thread | `pthread_cancel` + `pthread_detach` | Network change, --once timeout |
| I/O thread | DNS thread | `pthread_detach(slot->thread)` | After consuming resolved DNS addr |
| DNS thread (self) | (kernel) | `pthread_detach(pthread_self())` | After writing result + signaling I/O |
| I/O thread | Main loop | `http_result.state` | Fetch completed or failed |
| Main loop | (itself) | `http_result_read` | Each tick reads result |

### Race-free I/O thread exit

Only DNS threads ever write to the eventfd. The main loop communicates
with the I/O thread by closing socket fds (setting `fd = -1` in the
connection struct) and through `ctl.cancel_all` / `ctl.shutdown` flags
(checked by `io_cmd()` on every loop iteration). The race below is
exclusively between DNS threads and the I/O thread exiting.

I/O thread holds `ctl.lock` while checking `pending_work==0` AND setting
`io_thread_active=0` + saving `efd` + setting `ctl.efd=-1`. DNS thread
acquires same lock before checking `io_thread_active` — the two are
mutually exclusive, so there is no TOCTOU window where a DNS completion
is lost between the check and the flag write.

If a DNS thread wins the lock first: it increments `pending_work`, sees
`io_thread_active==1` (I/O thread hasn't exited yet), writes eventfd, and
unlocks. I/O thread then acquires lock, sees `pending_work > 0`, does NOT
exit.

If the I/O thread wins the lock first: it sets `io_thread_active=0`,
saves `efd`, sets `ctl.efd=-1`, and unlocks. DNS thread then acquires
lock, sees `io_thread_active==0`, opens a NEW eventfd, creates a NEW I/O
thread. No eventfd write to a closed fd: `ctl.efd` was set to -1 before
the unlock, so the DNS thread's `eventfd_write` path is unreachable (it
only reaches `eventfd_write` when `io_thread_active==1`).

No double I/O thread create: `pthread_create` for I/O thread is gated
on `!io_thread_active` under `ctl.lock`. Only one DNS thread can be
inside that branch.

### DNS thread lifecycle

```
Main thread                          DNS thread
───────────                          ──────────
dns_cancel(slot):                    dns_thread_run(arg):
  lock(slot->lock)                     pthread_cleanup_push(dns_cleanup, arg)  // on cancel: freeaddrinfo + free(arg)
  if (slot->state != 1)                getaddrinfo(...)                        // ← cancellation point
      { unlock; return; }              pthread_cleanup_pop(0)
  slot->cancelled = 1                  lock(slot->lock)
  t = slot->thread                     if slot->cancelled:
  slot->state = 0                          freeaddrinfo(ai)
  unlock(slot->lock)                       slot->state = -1
  pthread_cancel(t)                       unlock; free(arg); pthread_detach(self); return
  pthread_detach(t)                    else:
                                          slot->addr = ai
                                          slot->state = 2
                                        unlock(slot->lock)

                                       // signal I/O thread (ctl.lock)
                                       // ...

                                        free(arg)
                                        pthread_detach(pthread_self())  // self-cleanup
                                        return NULL
```

> **Design Principle 4 note — resolv.conf deduplication attempt**:
> We investigated using `res_query` (musl) or `res_ninit`/`res_nquery` (glibc)
> to avoid re-reading `/etc/resolv.conf` on every DNS query. This failed:
> on musl, `res_init()` is a no-op and `res_query` is a wrapper around
> `__res_msend` which calls `__get_resolv_conf` on every invocation — every
> DNS query re-parses resolv.conf from the filesystem. On glibc, each
> thread has its own thread-local `_res` state, so each new DNS thread
> spawned on refresh cycles re-reads resolv.conf regardless. Writing our
> own DNS wire-protocol (build query packets + raw UDP sockets) would
> achieve the goal but adds ~100 LOC of complex byte-twiddling for
> marginal real-world savings — resolv.conf is small and reading it a few
> times per refresh cycle is not a bottleneck. We opted for `getaddrinfo`
> (simpler, portable) and accept the per-thread resolv.conf reads.

> **Race note**: between `dns_cancel` unlocking the slot and `start_dns`
> re-locking it, the cancelled thread (which may have unblocked from
> `getaddrinfo` before `pthread_cancel` arrived) can call `store_result` and
> see `cancelled=0` if `start_dns` already cleared it. `store_result` checks
> `slot->cancelled` again after acquiring the lock (as shown in the flow
> above) — if cancelled, the stale result is discarded. Both threads resolve
> the same hostname so the address is identical even without the check, but
> the guard prevents a latent defect if targets ever diverge.

## Glossary

| Term | Definition |
|------|------------|
| **RIS** | Reset to Initial State — ANSI escape `\033c`. |
| **sixel** | Sixel graphics format for terminal images (DEC VT-series). |
| **DA1** | Device Attributes primary query — `\033[c`. Terminal responds with `\033[?<params>c`. Parameter `4` = sixel support. |
| **DCS** | Device Control String — sixel envelope: `\033P` start, `\033\` end (7-bit). Also `\220`/`\234` (8-bit). |
| **Raster Attributes** | Sixel header format after DCS start: `q"1;1;<width>;<height>`. `q` = DECGRA final byte, `"1;1;` = Pan/Pad aspect ratio. |
| **DECGCI** | Select color: `#<index>;2;<R>;<G>;<B>` where R/G/B are 0–100. |
| **DECGRI** | Repeat introducer: `!<count><char>` compresses runs of identical pixels (threshold > 3 consecutive). |
| **dot matrix** | 3-column × 5-row bitmap representing a digit or colon. |
| **scale factor** | Number of terminal cells per dot-matrix cell, computed from terminal size. |
| **Widget** | A named info-bar section showing a group of related metrics. Controlled by `--widgets`. |
| **Default mode** | Operation without `--widgets`: fixed widget set, order, and `--gpu`/`--fan` flag gating. |
| **Custom mode** | Operation with `--widgets`: fully user-controlled inclusion and ordering. |
| **Lazy reads** | Only read data sources needed by the active widget set. |
| **ASCII mode** | Large-font clock using UTF-8 block characters. |
| **headless mode** | Operation when stdout is not a TTY — text-only output. |
| **TTY** | A terminal device (physical or virtual). |
| **iowait** | Time the CPU spent waiting for I/O to complete, from `/proc/stat`. |
| **I/O thread** | Single dedicated thread, created **detached** on demand by the first DNS thread to complete. Runs a non-blocking `poll(-1)`-based event loop with eventfd for all HTTP I/O (connect, send, recv, parse). Exits when work is done; kernel reclaims on exit. |
| **hwmon** | Linux hardware monitoring sysfs subsystem (`/sys/class/hwmon`). |
| **pp_dpm_sclk/mclk** | AMD GPU power/clock control sysfs files listing available core/memory clock levels; `*` marks the active level. |
| **rc6_residency_ms** | Intel GPU RC6 sleep state residency counter (microseconds). Used to derive GPU busy % via delta. |
| **gpu_busy_percent** | AMD GPU busy percentage sysfs file. |

## Sixel Mode Design

### Overview

Sixel mode renders the clock digits as a bitmap using sixel escape sequences. The info bar and all text/icon content are rendered as normal terminal text positioned alongside the sixel graphics.

### Sixel capability detection

The DA1 query runs **inside `setup_terminal`**, not at startup. It is only
executed when the user specified `auto` mode — if the user explicitly passes
`sixel`, `ascii`, or `text` mode no DA1 query is performed.

The query sends `\033[c` to stdout and waits for the terminal's response on
stdin. The terminal responds within a few milliseconds with
`\033[?<param1>;<param2>;...c`. If the response contains parameter `4`, the
terminal supports sixel graphics. Implementation:

1. `setup_terminal`'s auto-mode branch calls `da1_setup` which temporarily
   saves termios, disables ICANON + ECHO (needed because the response
   `\033[?4c` has no newline — canonical mode would not return it), and sets
   VMIN=1, VTIME=0.
2. Set stdin to non-blocking mode.
3. Flush stdout, write `\033[c`.
4. Call `poll()` on stdin with a 10ms timeout.
5. Read available bytes and scan for `\033[?` followed by semicolon-delimited
   parameters ending with `c`.
6. Restore original termios (the temporary save from step 1).
7. If parameter `4` is present, resolve `*mode = MODE_SIXEL`, otherwise
   `*mode = MODE_ASCII`.

After the DA1 query resolves the mode, `setup_terminal` saves a **fresh**
termios snapshot (the real one, with ECHO off — and ICANON off if sixel)
and registers the signal handler. The `da1_setup` restore in step 6 ensures
the DA1 temporary manipulation does not leak into the saved snapshot.

This approach works with xterm, mlterm, foot, WezTerm, and all terminals
implementing the DEC VT-style DA1 response.

### Sixel data format

A complete sixel image follows this structure:

```
<DCS start>     \033P               — Device Control String begin
<raster attr>   q"1;1;<W>;<H>      — Raster attributes: Pan/Pad 1;1, width W, height H
<palette>       #0;2;90;90;90      — Color 0: background #e5e5e5 (R=90,G=90,B=90)
                #1;2;13;13;13      — Color 1: clock digit #202020 (R=13,G=13,B=13)
<pixel data>    ???...             — Sixel character data (? = 0x3F..0x7E = value 0..63)
<next band>     -                  — DECGNL: advance to next 6-row band
...repeat...
<DCS end>       \033\              — Device Control String end
```

**Pixel encoding:** Each sixel character represents 6 vertical pixels. Bit 0 = topmost pixel. Character `?` (0x3F) = all bits clear (empty), `~` (0x7E) = all bits set (fill). The pixel value is `char - 0x3F`.

**Run-length compression:** Four or more consecutive identical pixels use the repeat introducer `!<count><char>`. Example: `!20?` = 20 columns of empty pixels. The threshold of 3 matches libsixel's encoding — below 4, emit individual characters.

**Band structure:** Sixel data is encoded in 6-row bands. A band covers vertical strips of 6 pixels each. After completing one band's data, emit `-` (DECGNL) to advance to the next band. The final band may be shorter than 6 rows when `H % 6 != 0` — the background fill value for the partial band uses `(1 << (H % 6 + 6 * !(H % 6))) - 1` (only the valid pixel rows are filled). When `H % 6 == 0` the final band is a full 6-row band and uses bg_val = 63 (all six bits set).

**Palette:** 2-color palette. `#0` = background (`#e5e5e5`, R=90,G=90,B=90), `#1` = clock digit (`#202020`, R=13,G=13,B=13). Color values are 0–100 (not 0–255). Conversion from 8-bit: `(value * 100 + 127) / 255`.

**Background fill:** Each band starts with `#0!<W>~` filling all W columns with background color. Bands with no foreground data end there — the `#1$` switch is omitted entirely. When foreground pixels exist, `#1$` selects the clock color and the sixel data is emitted as a single row within the band (no per-row `$`).

**Pixel sampling:** Each pixel position `(x, row)` is mapped to the 27 × 5 dot grid via proportional floor mapping:

```
dc = x * 27 / iw           dot column index (0..26)
dr = row * 5 / ih          dot row index (0..4)
```

Where `dc >= DOT_COLS` or `dr >= DOT_ROWS` yields no pixel. This gives each dot a width of either `floor(iw/27)` or `ceil(iw/27)` pixels, maintaining proportional spacing across the clock face.

### Pixel buffer layout

Sixel is pixel-exact — the terminal renders the declared W×H image at the
cursor position without clipping to character cell boundaries.  All canvas
sizing must therefore account for the true pixel dimensions.

### Variables

| Variable | Meaning | Unit |
|----------|---------|------|
| `aw` | available width (terminal width minus sidebar) | characters |
| `ah` | available height (terminal height) | characters |
| `cw` | character cell width | pixels |
| `ch` | character cell height | pixels |
| `dw` | outer canvas width | characters |
| `dh` | outer canvas height | characters |
| `iw` | inner clock width | pixels |
| `ih` | inner clock height | pixels |

### Character cell dimensions

The character cell dimensions are derived from the terminal's pixel geometry
(returned by `TIOCGWINSZ`):

```
cw = ws_xpixel / wscol   (pixels per character cell width)
ch = ws_ypixel / wsrow  (pixels per character cell height)
```

When `TIOCGWINSZ` returns zero pixel dimensions (`ws_xpixel = 0` or
`ws_ypixel = 0`), query the terminal via `\033[14t` escape sequence.
Parse the response `\033[4;{height};{width}t`, compute:

```
cw = pixel_width / wscol
ch = pixel_height / wsrow
```

If the escape sequence also fails (timeout or no response), fall back to
`cw = 5`, `ch = 9`.

### Outer canvas (aspect ratio 29:7 characters)

The available terminal area is obtained from the character grid
(terminal width × height less any sidebar, per
[Display Layout](#display-layout)), clamped to a minimum of 60 × 12
character cells, then capped at 85% of each dimension (floor-rounded).

```
aw = floor(max(info_col, 60) * 85 / 100)
ah = floor(max(wsrow,     12) * 85 / 100)
```

For non‑TTY (piped output) the same formula applies with
`info_col = 80, wsrow = 24` (VT100 default), yielding
`aw = 68, ah = 20`.

Find the largest 29:7 rectangle that fits within `aw × ah`:

```
if aw * cw * 7 <= ah * ch * 29:          # width-constrained
    dw = aw
    dh = ((aw * cw * 7 + ch - 1) / ch) / 29
else:                                     # height-constrained
    dw = ((ah * ch * 29 + cw - 1) / cw) / 7
    dh = ah
```

The outer canvas occupies `dw × dh` character cells.  It is centered within
`aw × ah`:

```
char_offset_x = (aw - dw) / 2            characters from left
char_offset_y = (ah - dh) / 2            characters from top
```

### Inner clock (aspect ratio 27:5 pixels)

The clock face (27 × 5 dot matrix) sits inside the outer canvas.  The inner
box is always width‑constrained — the 29:7 outer aspect ratio guarantees the
inner rectangle fits within `dw × dh`:

```
iw = dw * cw * 27 / 29
ih = iw * 5 / 27
```

All divisions are integer floor.  The inner clock is centered within the outer
canvas:

```
h_margin = (dw * cw - iw) / 2            pixels each side
v_margin = (dh * ch - ih) / 2            pixels top and bottom
```

When `(dh * ch - ih)` is odd, `ih` is increased by 1 before computing
`v_margin` so that top and bottom margins are equal.

No half-character margin guarantee — simple centering is sufficient.

### Sixel encoding parameters

```
pixel_width    = dw * cw
pixel_height   = dh * ch
dot_grid_left  = h_margin
dot_grid_top   = v_margin
```

The sixel image is declared as `pixel_width × pixel_height`.  The sixel
encoding fills all bands with background color (`#0`), then overlays the
27 × 5 digit grid using foreground color (`#1`) wherever the dot grid has
a set pixel.  The dot grid starts at `dot_grid_left` pixels from the left
edge and `dot_grid_top` pixels from the top edge.  Each pixel is sampled
from the dot grid via proportional floor mapping (see [Sixel data format](#sixel-data-format)).

### Positioning

Positioning escapes follow the same rules as [Mode-specific rendering](#mode-specific-rendering):
with TTY and loop mode (not `--once`), the sixel image is centered in the full terminal
area using CUP:

```
row = (wsrow - dh) / 2 + 1
col = (info_col - dw) / 2 + 1
```

When stdout is not a TTY, CUP positioning escapes are omitted and the
image renders inline at the cursor position. In `--once` mode with a TTY,
CUP is also omitted (the image renders inline) but horizontal centering
via cursor-forward `\033[<N>C` is still applied per [Mode-specific rendering](#mode-specific-rendering).

The 85% cap (`aw = info_col * 85 / 100`, `ah = wsrow * 85 / 100`) applies
**universally** — in both loop and `--once` mode, with or without a TTY. The
outer canvas is always computed against the capped dimensions.

### Non‑TTY fallback example

For piped output `info_col = 80, wsrow = 24` (VT100 default), giving
`aw = 68, ah = 20` (85% of 80 = 68, 85% of 24 = 20).  With `cw = 5, ch = 9` (fallback default):

```
68 × 5 × 7 = 2380 ≤ 20 × 9 × 29 = 5220   → width-constrained
dw = 68,  dh = ((68 × 5 × 7 + 9 - 1) / 9) / 29 = 9

iw = 68 × 5 × 27 / 29 = 316
ih = 316 × 5 / 27 = 58
(dh * ch - ih) = 9 × 9 - 58 = 23  (odd → ih = 59)

pixel_width  = 68 × 5 = 340
pixel_height = 9 × 9 = 81
```

Output goes to pipe; no centering applied.

### Layout

When widgets are active:
```
┌──────────────────────┬──────────────────────┐
│   sixel clock        │  info bar (text)     │
│   (scaled to fit)    │  CPU:  12.3%  48°C   │
│                      │  MEM:  45%  2.3/8G   │
│                      │  GPU:  45%  72°C     │
│                      │  FAN:  1 3200RPM     │
│                      │  48°C  45°C  42°C    │
│                      │  ...                 │
└──────────────────────┴──────────────────────┘
```

When no widgets are active (full-screen clock):
```
┌────────────────────────────────────────────┐
│                                            │
│        ██  ██  ██████                      │
│        ██  ██  ██                          │
│        ██████  ██████                      │
│        ██  ██  ██                          │
│        ██  ██  ██████                      │
│                                            │
│              12:34:56                      │
│                                            │
└────────────────────────────────────────────┘
```

- Info bar width per [Display Layout](#display-layout).
- Clock is centered in remaining space (or the full terminal width).

## Monitor Thread Action FIFO

The monitor thread does not set shared atomic flags. Instead, each netlink
event is pushed as a typed action into a mutex-protected FIFO. The main thread
drains the FIFO every tick and decides what to invalidate or refresh based on
the action type and its arguments. This preserves per-event information
(a link-remove followed by a link-add between ticks produces two distinct
actions rather than collapsing to a single "links changed" bit).

### Types

```
enum { IFLA_KIND_MAX = 32 };  // max length of IFLA_INFO_KIND string

enum mon_action_type {
    MON_NONE = 0,
    MON_LINK_ADD,        // RTM_NEWLINK — netlink new interface
    MON_LINK_REMOVE,     // RTM_DELLINK — netlink interface removed
    MON_ADDR4_ADD,       // RTM_NEWADDR, ifa_family == AF_INET
    MON_ADDR4_REMOVE,    // RTM_DELADDR, ifa_family == AF_INET
    MON_ADDR6_ADD,       // RTM_NEWADDR, ifa_family == AF_INET6
    MON_ADDR6_REMOVE,    // RTM_DELADDR, ifa_family == AF_INET6
    MON_ROUTE_ADD,       // RTM_NEWROUTE — main-table route added
    MON_ROUTE_REMOVE,    // RTM_DELROUTE — main-table route removed
};

union mon_action_arg {
    struct {                     // used by LINK_ADD, LINK_REMOVE
        int ifindex;             // kernel interface index (always present)
        char name[IFACE_NAME_LEN]; // IFLA_IFNAME if available, else ""
        char kind[IFLA_KIND_MAX];  // IFLA_INFO_KIND from IFLA_LINKINFO
        unsigned int flags;      // ifi_flags (RTM_NEWLINK only; 0 for DEL)
    } link;
    struct {                     // used by ADDR4_ADD/REMOVE, ADDR6_ADD/REMOVE
        int ifindex;             // interface this address belongs to
        unsigned char prefixlen; // ifa_prefixlen
        union {
            struct in_addr v4;
            struct in6_addr v6;
        } addr;
        int family;              // AF_INET or AF_INET6 (redundant with type,
                                 // but avoids casting)
    } addr;
    struct {                     // used by ROUTE_ADD, ROUTE_REMOVE
        int ifindex;             // RTA_OIF
        unsigned char table;     // rta_table (main vs others)
        unsigned char dst_len;   // destination prefix length (0 = default)
        int family;              // AF_INET or AF_INET6
    } route;
};

struct mon_action {
    enum mon_action_type type;
    union mon_action_arg arg;
};

struct mon_action_fifo {
    struct mon_action *items;    // heap-allocated, dynamic
    size_t count;
    size_t cap;
    pthread_mutex_t lock;
};
```

### rtnl_mon_ctx

```
struct rtnl_mon_ctx {
    int fd;                         // monitor socket, -1 = closed
    volatile int stop;              // main thread sets to 1 to signal exit
    struct mon_action_fifo fifo;    // producer = monitor thread, consumer = main
    pthread_t thread;
    int started;
};
```

### FIFO lifecycle

| Phase | Thread | Operation |
|-------|--------|-----------|
| Init (`rtnl_open_monitor`) | Main | `memset(&fifo, 0)`, `pthread_mutex_init(&lock)`. Bootstrap: set `addr4_changed=1` and `addr6_changed=1` directly (not pushed to the FIFO) so the first tick does initial IP refresh. |
| Produce | Monitor | Lock FIFO, push action with received netlink fields, unlock. No parsing, conversion, or filtering beyond type detection and field extraction. |
| Consume | Main (each tick) | Trylock loop: up to 10 attempts, 10ms `nanosleep` between each. No `clock_gettime` — see CONTRIBUTING.md §Design Principles. If all 10 fail, skip this tick — next tick will try again. Otherwise drain all items, set `count = 0`, unlock, then process per table below. |
| Destroy (`rtnl_monitor_stop`) | Main | Close socket (triggers monitor thread exit), `pthread_join`, free `fifo.items`, `pthread_mutex_destroy`. |

### Monitor thread behaviour

The thread runs the same `poll(100)` loop as today. On each `recvmsg`, it
iterates the netlink messages and appends one action per relevant message:

```
for each nlmsghdr in recv buffer:
    switch nlmsg_type:
        RTM_NEWLINK:
            if (!(ifi_flags & IFF_LOOPBACK))  // skip loopback
                // extract IFLA_IFNAME and IFLA_LINKINFO→IFLA_INFO_KIND
                // from attrs; kind="" when IFLA_INFO_KIND absent (physical)
                push MON_LINK_ADD(ifindex, name, kind, ifi_flags)
            // NOTE: RTM_NEWLINK is also emitted on link up/down and
            // parameter changes (MTU, etc.). The monitor thread does NOT
            // distinguish — the main thread checks ifindex against cache.

        RTM_DELLINK:
            push MON_LINK_REMOVE(ifindex, name, kind)

        RTM_NEWADDR:
            if ifa_family == AF_INET:  push MON_ADDR4_ADD(...)
            if ifa_family == AF_INET6: push MON_ADDR6_ADD(...)

        RTM_DELADDR:
            if ifa_family == AF_INET:  push MON_ADDR4_REMOVE(...)
            if ifa_family == AF_INET6: push MON_ADDR6_REMOVE(...)

        RTM_NEWROUTE:
            push MON_ROUTE_ADD(ifindex, table, dst_len, family)

        RTM_DELROUTE:
            push MON_ROUTE_REMOVE(ifindex, table, dst_len, family)
```

The monitor thread checks `m->stop || m->fd < 0` after each `poll()` return
to detect shutdown. The main thread sets `m->stop = 1` before closing the
socket.

### Main thread processing (per tick, in `get_net_info`)

**Mixed approach**: actions that have local/per-item effects (name_idx clear,
wcache, addr flags) are applied immediately. Actions that trigger full dumps
(links_stale, needs_route) are accumulated into booleans and applied once
after the loop.

```
// ——— Phase 1: drain FIFO ———
// No clock_gettime — trylock loop complies with "exactly three
// time-source calls" rule (CONTRIBUTING.md §Design Principles).
locked = 0
for i in 0..9:
    if pthread_mutex_trylock(&fifo.lock) == 0:
        locked = 1
        break
    nanosleep(10ms)
if !locked:
    skip — next tick will try again
n = fifo.count
copy all n items
fifo.count = 0
unlock(&fifo.lock)

// ——— Phase 2: process each action ———
acc_links_stale = 0
acc_needs_route = 0
acc_reprimary = 0

for i = 0 .. n-1:
    switch items[i].type:

        MON_LINK_ADD:
            if in_links(r, items[i].arg.link.ifindex):
                break                      // already cached — parameter change
            if is_virtual_kind(items[i].arg.link.kind) && c->has_phys:
                break                      // virtual + have physical primary
                                          // → no route impact
            acc_links_stale = 1            // need full dump to discover NIC
            c->wcache_stale = 1            // may be wireless
            // No acc_needs_route — a new NIC alone does not change routes.
            // If it becomes a gateway, a ROUTE_ADD event will follow.

        MON_LINK_REMOVE:
            idx = find_link_idx(r, items[i].arg.link.ifindex)
            if idx < 0:
                break                      // never cached — ignore
            // Local cleanup — no link dump needed:
            r->link_count--
            memmove(r->links + idx, r->links + idx + 1,
                    (r->link_count - idx) * sizeof r->links[0])
            name_idx_clear_entry(r, items[i].arg.link.ifindex)
            wcache_remove_entry(&c->wcache, items[i].arg.link.ifindex)
            for each j in c->link_idx:
                if c->link_idx[j] > idx:
                    c->link_idx[j]--    // adjust for the shift
            if r->links[idx].is_virtual && c->has_phys:
                break                      // virtual + physical primary exists
                                          // → no route impact
            acc_reprimary = 1              // re-evaluate pick_primary with
                                          // existing route data (no link dump,
                                          // route dump inside pick_primary)

        MON_ADDR4_ADD / MON_ADDR4_REMOVE:
            if in_link_idx(c, items[i].arg.addr.ifindex):
                addr4_changed = 1

        MON_ADDR6_ADD / MON_ADDR6_REMOVE:
            if in_link_idx(c, items[i].arg.addr.ifindex):
                addr6_changed = 1

        MON_ROUTE_ADD:
            if items[i].arg.route.table == RT_TABLE_MAIN
               && items[i].arg.route.dst_len == 0:
                acc_needs_route = 1        // new default route — re-evaluate
                // No acc_links_stale — LINK_ADD precedes or already set it.
                // Route dump + sys_if_indextoname in pick_primary resolves
                // the OIF without a link dump.

        MON_ROUTE_REMOVE:
            if items[i].arg.route.table == RT_TABLE_MAIN
               && items[i].arg.route.dst_len == 0
               && (in_gw_cache(c, items[i].arg.route.ifindex)
                   || in_link_idx(c, items[i].arg.route.ifindex)):
                acc_needs_route = 1        // default route for one of our
                                          // gateways or primaries — re-evaluate
                // No acc_links_stale — NIC is already known

// ——— Phase 3: apply accumulated flags ———
if acc_links_stale:
    r->links_stale = 1
if acc_needs_route:
    c->needs_route = 1
if acc_reprimary:
    c->needs_reprimary = 1
```

**Helpers** (for Phase 2 lookups):

- `in_links(r, ifindex)` — scan `r->links[0..link_count-1]` for matching
  `ifindex`. Returns 1 if found.
- `find_link_idx(r, ifindex)` — returns the index in `r->links[]`, or -1.
- `is_virtual_kind(kind)` — matches against the known virtual IFLA_INFO_KIND
  types (`tun`, `tap`, `veth`, `bridge`, `bond`, `dummy`, `sit`, `gre`,
  `gretap`, `vti`, `vlan`, `vxlan`, `geneve`). Returns 1 if the kind string
  matches a known virtual type, 0 otherwise. When the kind is "" (physical
  NIC, no IFLA_LINKINFO), always returns 0. Same logic as the dump-path
  `is_virtual_kind` in `rtnl_link.c`; defined in `net_fmt.c` for the FIFO
  processing path.
- `name_idx_clear_entry(r, ifindex)` — removes the stale ifindex→name
  mapping without invalidating the rest of the cache.
- `wcache_remove_entry(c, ifindex)` — scans `c->wcache.ifindices[]`, and
  if `ifindex` is found, shifts remaining entries down and decrements
  `c->wcache.count`. No full nl80211 re-scan needed — surviving entries
  stay valid.
- `in_link_idx(c, ifindex)` — checks if `ifindex` matches any
  `c->ifindex[i]` (the primary NIC ifindices selected by the last
  `pick_primary`). This is a direct int comparison — no r->links[]
  dependency needed.
- `in_gw_cache(c, ifindex)` — checks if `ifindex` matches any
  `c->gw_idx[i]` (the gateway candidates from the last route dump).
- **`c->has_phys`**: set by `pick_primary` — 1 when at least one selected
  primary NIC is non-virtual, 0 when every selected NIC is virtual
  (fallback mode — no physical NIC was available via the default route).
  When `has_phys == 0`, LINK_REMOVE of a virtual NIC must set
  `acc_reprimary` (our only NIC might be gone). A LINK_ADD alone does not
  trigger route re-evaluation — if the new physical NIC becomes a gateway,
  a ROUTE_ADD event will follow.
- **`c->needs_reprimary`**: set from `acc_reprimary`, consumed by
  `get_net_info` after `poll_refresh`. Calls `pick_primary(c, r, 1)`
  to re-evaluate with cached route data (`c->gw_idx[]` / `c->gw_metric[]`)
  — no route dump, no link dump.

**`name_idx` lifecycle**:
- On LINK_REMOVE: `name_idx_clear_entry` removes the stale ifindex→name
  entry immediately. Surviving NICs retain their valid entries across dumps
  — no full invalidation needed.

After the loop, `poll_refresh` consumes `links_stale` (full link dump) and
`needs_route` (route dump + `pick_primary`). `needs_reprimary` is checked
in `get_net_info` after `poll_refresh` — it calls `pick_primary` with
cached route data (no dump at all). Before any `pick_primary` call, the
current `c->ifindex[0..count-1]` is saved. After both triggers (if both
fire in the same tick), the new selection is compared:

```
// get_net_info, after FIFO drain + poll_refresh:
int old_idx[MAX_NET];
memcpy(old_idx, c->ifindex, sizeof old_idx);
int old_count = c->count;

// ——— re-evaluate primary ———
if (poll_refresh(ci, r, c))        // handles links_stale + needs_route
    pick_primary(c, r, 0);         // fresh route dump

if (c->needs_reprimary) {          // LINK_REMOVE only — routes unchanged
    c->needs_reprimary = 0;
    pick_primary(c, r, 1);         // reuse cached gw_idx/gw_metric
}

// ——— skip IP refresh if same NICs selected ———
int changed = (c->count != old_count);
for (int i = 0; !changed && i < c->count; i++)
    changed = (c->ifindex[i] != old_idx[i]);
if (changed) {
    ci->keep.rtnl_mon.addr4_changed = 1;
    ci->keep.rtnl_mon.addr6_changed = 1;
}
```

When either flag is set, IP refresh (`refresh_local_ips`) runs. If a local
IP changed from its previously cached value, `refetch_wan()` is called
regardless of what triggered the refresh — this is the existing behaviour.

`pick_primary(c, r, use_cached)`:
- `use_cached = 0`: calls `gather_routes` (fresh route dump) to populate
  `c->gw_idx[]` / `c->gw_metric[]`, then `compact_virtuals` + gateway
  resolution
- `use_cached = 1`: skips `gather_routes` — `gt` is initialised from
  the existing `c->gw_idx[]` / `c->gw_metric[]` (populated by the last
  `use_cached = 0` call). Runs `compact_virtuals` (re-filters virtuals
  against the current `r->links[]`), then gateway resolution.
- Stores the selected primary NIC ifindices in `c->ifindex[0..count-1]`.
  The corresponding `c->link_idx[i]` (index into `r->links[]` for
  throughput) is populated later by `scan_idx`. Display code resolves
  name lazily via `name_idx_by_idx(r, c->ifindex[i])`.

This is required because `pick_primary` may select a **different** primary
NIC whose IPs have not been read yet. The post-pick_primary set forces
`refresh_local_ips` to re-read the IP for whichever NIC `pick_primary` chose.

The `addr4_changed`/`addr6_changed` flags are same-thread (both written
and read in the main thread — see [Refresh and reconciliation order](#refresh-and-reconciliation-order)),
so plain `int` assignment suffices for both writing (`= 1`) and
reading-and-clearing (save to local, then `= 0`). No atomics needed.

### Termination

The main thread signals the monitor thread to exit via a single
`volatile int stop` flag (no mutex needed — one writer, occasional reader):

```
// main thread (rtnl_monitor_stop):
m->stop = 1;
if (m->fd >= 0)
    sys_close(m->fd);   // poll() in monitor returns with fd error
pthread_join(m->thread, NULL);
...

// monitor thread:
if (m->stop || m->fd < 0)
    break;
```

After `pthread_join` returns the main thread safely frees `fifo.items` and
destroys the FIFO mutex.




