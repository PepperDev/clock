Quality Gates (cppcheck, gcov, lizard thresholds) are defined in CONTRIBUTING.md §Quality Gate.

## Bug Prevention

Every bug found must have a regression test — a unit test or integration test that reproduces the bug, fails before the fix, and passes after. Writing tests is harder, but it is mandatory to prevent the same bug from recurring.

When a bug is genuinely impossible to test (e.g. hardware-dependent race condition, external service behavior), add a prevention entry to §Domain-Specific Pitfalls with bug description, root cause, and prevention plan as a last resort.

New prevention entries must not include a recurrence annotation — the DoD regression test requirement already prevents recurrence. Legacy entries retain their original annotations for reference.

Also consult the Memory Safety Checklist and Domain-Specific Pitfalls below as part of the done criteria for every new task — review applicable items before committing.

## Memory Safety Checklist

When modifying any dynamic memory allocation (malloc/calloc/realloc/free, slab pools, or any code that manipulates allocated memory), audit for ALL of the following hazards:

**Note**: Several hazards below (data race, torn read/write, visibility, deadlock) also apply to **statically allocated fixed-size memory** that is shared among multiple threads. Any shared memory — whether malloc'd or global — must be checked for concurrent access, atomicity of multi-word writes, memory ordering, and lock ordering hazards.

| Hazard | What to look for |
|--------|-----------------|
| **Use-after-free** | Pointer read/written after free; double-free; missing NULL after free |
| **Data race** | Shared allocation accessed by both main thread and worker without ownership handoff or mutex |
| **Out-of-bounds** | Array index from user-controlled data; fd-indexed array with unbounded fd values |
| **Dangling pointer** | realloc/malloc result assigned to global without failure check freeing old data first; pointer to realloc'd array element after growth |
| **Memory corruption** | memcpy without bounds check; missing NULL check after malloc before write |
| **Write-after-read** | Swap-last compaction reading source after potential realloc invalidation |
| **Torn read/write** | Multi-word struct written non-atomically and read from another thread |
| **Visibility** | Missing memory barrier around flag set by one thread and read by another |
| **ABA** | Lock-free freelist reuse with pointer comparison as identity |
| **Deadlock** | Multiple mutexes acquired in different order; mutex held across blocking call |
| **Init-ordering dependency** | Registration/add function called before object fields are fully initialized. Three variants: **(a)** function validates a precondition (e.g. `used` flag) that isn't set yet and silently returns; caller has no way to detect the registration was skipped; failure manifests as a later lookup miss. **(b)** guard condition involves only zero-initialized fields (e.g. `(used+tombs)*2 > cap` on globals: `0 > 0` → **false**) so a required init step (grow, allocate) is skipped, leading to NULL-pointer deref. Generic defense for any new hash table: always add `!ht \|\|` to the grow trigger — `if (!ht \|\| (used+tombs)*2 > cap) grow();`. **(c) zero-init ambiguity**: sentinel value is 0 (same as calloc/memset zero state), so a guard like `if (idx >= 0)` thinks a zero-initialized field is legitimately set, causing incorrect side effects (e.g. `pend_free_buf` frees slab index 0 that was never allocated). Defense: use only negative sentinels for "not set" (-1, -2) instead of 0. |
| **Stale buffer data** | Static/reused scratch buffer not cleared between calls; leftover bytes from previous operation contaminate checksums, padding bytes, or length-dependent computations (e.g. odd-length UDP checksum reads padding byte from stale data) |
| **Realloc ordering** | Multiple realloc calls in one success-check — if earlier succeeds and later fails, earlier allocation's old block is already freed, leaving dangling global |

## Domain-Specific Pitfalls

This section consolidates two kinds of bug prevention knowledge: a pre-merge safety checklist of generic C pitfalls (consult before every commit), generic bug patterns with code examples, and project-specific entries (populate as domain knowledge grows).

### Pre-Merge Safety Checklist

Before merging any change, verify these generic C items:

- **Integer overflow**: check all arithmetic on untrusted input for overflow (buffer sizes, timeout calculations, token arithmetic, etc.)
- **Buffer bounds**: ensure every `memcpy`/`snprintf`/`strncpy` uses the destination buffer size; no `strcpy`, `sprintf`, `gets`
- **Endianness**: convert network byte-order fields with `htonl`/`htons`/`ntohl`/`ntohs` as needed; avoid double-conversion (e.g. `s_addr` from `recvfrom` is already NBO)
- **File descriptor leaks**: pair every `open`/`socket`/`accept` with a `close` on all error paths
- **Signal safety**: call only async-signal-safe functions from signal handlers (see `signal-safety(7)`). The SIGINT/SIGTERM handler writes one `__thread volatile sig_atomic_t` flag and returns — this is the only POSIX-guaranteed safe operation. All cleanup (including terminal ECHO restoration via `tcsetattr`) runs in `cleanup_all()` from the main thread after the handler returns. On double signal, `_exit(128 + sig)` terminates immediately.
  Do NOT use RIS (`\033c`) for echo restoration — RIS resets emulator state only, not the kernel's `ECHO` flag.
- **EINTR**: wrap all blocking syscalls (`poll`, `recvfrom`, `sendto`, `accept`) in a loop that retries on `EINTR`
- **Decode safety**: reject invalid characters in Base32/Base64/Hex input; validate output buffer length before writing
- **Revectored init**: guard conditions on zero-initialized globals may evaluate `0 > 0` as false, skipping required setup — always add `!ptr \|\|` to grow triggers
- **Hash table probe budget**: add a bounded probe budget to every open-addressing find/remove/add loop to prevent livelock
- **Dynamic array stale pointers**: store an index instead of a pointer across a potential realloc
- **Grow-forgot-cap**: set `cap = new_cap` immediately after malloc/realloc for a hash table or dynamic array, before any mask/budget computation
- **IPv4-mapped IPv6**: when using `struct sockaddr_in6` to represent IPv4 addresses, ensure `IN6_IS_ADDR_V4MAPPED` checks and correct `sin6_flowinfo`/`sin6_scope_id` handling — these fields are meaningful for v6 but must be zero for v4-mapped-v6 addresses
- **Derived index bounds**: when computing an index from variable-length data (replay table, array offset, hash from untrusted input), verify the index is within bounds before access; out-of-range indices from pointer arithmetic are memory corruption
- **Kernel buffer size assumptions**: kernel interface files and netlink messages have sizes not guaranteed by any API contract; always validate the returned length against the buffer capacity and do not assume a fixed-size buffer will hold a complete message
- **Configuration struct defaults**: after `memset(0)` or `calloc`, all fields are zero — ensure sentinel values distinguish "not set" from valid values (use negative sentinels like `-1` instead of `0` when zero is a legitimate value)
- **Terminal escape response validation**: after successfully parsing a terminal escape response (e.g. `\033[14t` pixel query, `\033[c` DA1), validate the returned values are non-zero and within sensible bounds before using them in size/capability computations. A terminal may reply `\033[4;0;0t` (zero pixels) which parses successfully, but using zero as a divisor or dimension yields degenerate output. Parse success is not a guarantee of semantic validity.

### Generic bug patterns with code examples

#### Event-loop fairness

Every event handler that drains an fd in a `for(;;)` loop must yield back to the main event loop after a bounded number of iterations. Without a budget, a flood of events on one fd can starve all other event sources (timers, other fds, signal handlers).

```c
// WRONG — unbounded drain
static void handle_fd(void)
{
    for (;;) {
        int n = read(fd, buf, sizeof(buf));
        if (n <= 0) return;
        process(buf, n);
    }
}

// RIGHT — bounded iteration budget
static void handle_fd(void)
{
    int budget = 64;
    for (;;) {
        int n = read(fd, buf, sizeof(buf));
        if (n <= 0) return;
        process(buf, n);
        if (--budget == 0) return;   // yield to main loop
    }
}
```

Check every `for(;;)` / `while(1)` that reads from a non-blocking fd; budget 32–128.

#### Non-blocking recv infinite loop on socket error

When `recv()` on a non-blocking socket returns -1 with an errno other than `EAGAIN` (e.g. `ENOTCONN`, `ECONNRESET`), the socket is in an error state. Calling `poll(POLLIN)` on such a socket returns immediately with `POLLERR`, creating an infinite read-poll loop.

```c
// WRONG — infinite loop when recv returns permanent error
for (;;) {
    int r = recv(fd, buf, sz, 0);
    if (r > 0) { n += r; continue; }
    if (r == 0) break;
    poll(&p, 1, 5000);   // returns immediately on POLLERR
}

// RIGHT — bail on non-retryable errors
for (;;) {
    int r = recv(fd, buf, sz, 0);
    if (r > 0) { n += r; continue; }
    if (r == 0) break;
    if (errno != EAGAIN) break;  // non-retryable error
    if (poll(&p, 1, 5000) < 1) break;  // timeout
}
```

Common causes: `send()` on a failed non-blocking connect returns `EPIPE`, then `recv()` returns `ENOTCONN`. Always check `send()` return value and bail early.

When modifying any `for(;;)` recv loop on a non-blocking socket, check:
- Does `recv` distinguish between `EAGAIN` (retryable) and other errno values?
- Is there a `poll(POLLIN)` after `EAGAIN` with a timeout to prevent infinite spin?
- Is `send()` return value checked for early bailout on broken connections?

#### Resource teardown on error paths

In manual-cleanup code, every early `return` after a successful allocation is a potential leak. Free in reverse allocation order and NULL-out after free.

```c
    conn = alloc_conn();
    if (!conn) return -1;
    job = alloc_job();
    if (!job) { free_conn(conn); return -1; }
    if (register(conn, job) < 0) {
        free_job(job);
        free_conn(conn);
        return -1;
    }
```

Check every error path clears all resources allocated so far, in reverse order.

#### Init-ordering gotchas

Zero-initialized hash tables and dynamic arrays are vulnerable to guard conditions that silently skip init:

```c
// WRONG — (used+tombs)*2 > cap → 0 > 0 → false, grow never called
if ((used+tombs)*2 > cap) grow(ht);

// RIGHT — always guard NULL first
if (!ht || (used+tombs)*2 > cap) grow(ht);
```

Also avoid sentinel value 0 when 0 is also a valid index — use -1 or -2 for "not set".

#### Mock bypass: existing tests silently become mock tests when bypass is added

When a function gains a mock bypass (return early with mock value), any existing test that calls that function will now receive the mock default instead of exercising the real code path — the test silently becomes a mock test.

Common mistakes:
- Adding `_mock_*_ret` bypass to a function without auditing existing callers
- Assuming existing tests will still exercise real code paths after the bypass is added
- Not resetting the mock control to `-1` in tests that need the real code path

When modifying a function to add a mock bypass, check:
- Are all existing callers of this function (in tests) updated to set the mock control to `-1` before calling if they need real code execution?
- Is the mock control initialized to the bypass-default (e.g. 0) in `mock_sys_reset()`?

Correct pattern:
```c
// WRONG — test expects real code but gets mock value 0
void test_some_error_path(void)
{
  mock_sys_reset();         // sets _mock_same_fs_ret = 0 (bypass)
  // ... setup error condition ...
  ASSERT_EQ(function_under_test(...), -1);  // gets 0 instead!
}

// RIGHT — disable bypass before calling real code
void test_some_error_path(void)
{
  mock_sys_reset();
  _mock_same_fs_ret = -1;   // disable bypass, exercise real code
  // ... setup error condition ...
  ASSERT_EQ(function_under_test(...), -1);  // gets expected -1
  _mock_same_fs_ret = 0;    // restore default for subsequent tests
}
```

#### Test mock infrastructure: weak symbols

All syscall and stdlib wrappers are defined as weak symbols in `src/util/syscall.c`. Tests provide strong overrides in `tests/mock_syscall.c`. At link time, the linker picks the strong definition — no `#ifdef` guards, no separate test compilation, no `TEST_STATIC`.

Mock control variables (`_mock_mount_ret`, `_mock_mount_count`, etc.) live in `tests/mock_syscall.c`. Reset via `mock_sys_reset()` before each test.

Common mistakes:
- Using `_mock_*_ret` without calling `mock_sys_reset()` first — stale values from the previous test bleed over
- Using `_mock_*_ret = 0` to mean "disable mock" when 0 is a valid success return — use `-1` as the sentinel for "no mock" (see Mock bypass pitfall above)
- Forgetting to restore mock defaults after a test that changes them — `mock_sys_reset()` at the start of each test prevents this

When modifying or adding mock infrastructure, check:
- Does the new mock need `_mock_*_fail_after` for Nth-call positioning?
- Does the new mock record call count for ordering assertions?
- Is `mock_sys_reset()` called at the start of each test that uses mocks?

### GPU detection: confirm hardware type with multiple identifying files, not just one

**Bug description**: Intel GPU detection checked only `gt_act_freq_mhz` alone.
The reference script requires both `gt_act_freq_mhz` AND `gt_max_freq_mhz` to
exist before treating the card as Intel. A card that happens to have only
`gt_act_freq_mhz` (e.g. a future or third-party driver) would be misidentified.

**Root cause**: When determining hardware type from sysfs file presence, a single
file can match incorrectly (e.g. a different driver exposes the same filename).
Confirming with a second identifying file reduces false positives.

**Prevention plan**: When adding a new GPU/vendor detection path in `gpu_init`,
require at least two distinguishing sysfs files before assigning a non-zero
`inited` value. One file may be shared across vendors; the second file should
be vendor-specific.

When modifying `gpu_init`, check:
- Is `gpu_has_file` called for at least two files before assigning `inited`?
- Are the files vendor-specific, or could a different GPU driver expose them too?
- Does the fallback path (e.g. RC6) have adequate guards to prevent misuse on
  incompatible hardware?

### Route selection: output-before-success pattern — `best_route` caller must initialize `out`

**Bug description**: `pick_primary` declared `char v4[IFACE_NAME_LEN]` without
initializing it, then passed it to `best_route`. `best_route` writes to `out`
only when it finds a matching route. On failure `v4` remains uninitialized, and
the subsequent `if (!v4[0])` check reads stack garbage — a false-negative skips
the IPv6 fallback; a false-positive uses garbage as the interface name.

**Root cause**: `best_route` has a "write on success, leave untouched on
failure" contract that is invisible to the compiler and to human readers. The
caller assumed `out` would be zeroed on failure.

**Prevention plan**: When calling any function that conditionally writes an
output buffer (success → writes, failure → leaves untouched), always initialize
the output buffer to a safe default before the call. This applies to all
instances of this pattern, not just `best_route`.

When modifying `pick_primary` or adding a new `best_route`-like caller, check:
- Is the output buffer initialized before the call?
- Is the fallback path (second `best_route` call) guarded by a check that
  cannot be fooled by stack garbage?

### CLI args: parsed-but-never-read — refresh intervals not wired to runtime

**Bug description**: `--ip-refresh` and `--weather-refresh` CLI arguments were parsed
correctly and stored in `struct args`, but the values were never read after startup.
Only `--local-ip-refresh` had a proper timer check wired in (`refresh_local_ips`).
The result: public IP was fetched once at startup and never re-fetched (unless
a local IP change triggered it). Weather was fetched once and never re-fetched.

**Root cause**: Adding a new CLI option that controls runtime behavior requires
three separate wiring steps: (1) parse and store in `struct args`, (2) propagate
to `struct cpu_keep` in `init_clock`/`start_*_fetch`, (3) add timer check in
`poll_async_fetches` or equivalent. Step 2 was partially done (argparse
stores correctly), step 3 was entirely missing. No cross-reference or grep
pattern alerts when a field is stored but never consumed.

**Prevention plan**: When adding a new CLI option that controls a timed refresh
interval, grep the field name in all `.c` files after implementation to confirm
it is both stored AND read in a runtime timing path. The pattern to verify:
`struct args` field → propagated to `cpu_keep` → checked in `poll_async_fetches`
or the tick-loop equivalent. Pay special attention to new options that
resemble existing ones (e.g. `--ip-refresh` alongside `--local-ip-refresh`)
— the existing one may be properly wired while the new one is not.

When modifying CLI args or the main loop:
- Search `src/*.c src/*/*.c` for the args struct field name — it should appear
  in both a parser handler and a runtime check (or propagation point).
- If the option controls a time interval, ensure the timer fields exist in
  `cpu_keep` and are checked in `poll_async_fetches` or `refresh_local_ips`.

### Wired NIC speed: driver may report speed only via legacy ETHTOOL_GSET, not GLINKSETTINGS

When `ETHTOOL_GLINKSETTINGS` succeeds but returns `speed=0`, some drivers
implement speed reporting only via the legacy `ETHTOOL_GSET` ioctl
(`struct ethtool_cmd`). The kernel's ethtool shim fills `GLINKSETTINGS` fields
but leaves `speed` at 0 because the driver's `get_link_ksettings` op is either
missing or returns 0.

Common mistakes:
- Assuming `GLINKSETTINGS speed > 0` when the probe succeeded (the ioctl
  returns success even when the driver doesn't fill the speed field)
- Retrying `GLINKSETTINGS` on every tick when the driver is known to return 0

When modifying `read_link_speed` or related ethtool code, check:
- Does the code fall through to `ETHTOOL_GSET` when `GLINKSETTINGS` returns
  speed=0?
- Is `no_glinksettings` set when GLINKSETTINGS returns 0 (not just on probe
  failure), so subsequent ticks skip the GLINKSETTINGS probe?
- Are both probe failure (ioctl -1) and speed=0 handled the same way (set flag,
  fall to GSET)?

Correct pattern:
```c
if (!c->no_glinksettings) {
    int nw = ethtool_need_nwords(fd, c->iface[i]);
    if (nw < 0) {
        c->no_glinksettings = 1;
    } else {
        int spd = ethtool_speed(fd, c->iface[i], nw > 0 ? nw : 0);
        if (spd > 0)
            return spd;
        c->no_glinksettings = 1;
    }
}
return read_speed_gset(fd, c->iface[i]);
```

### Init-ordering: `restore_weather` must run after `poll_async_fetches`

The `tick()` function in `main_loop.c` calls `poll_async_fetches` (which updates
`keep.weather_valid` + `keep.weather_temp/desc` via `pump_weather_ok`), then
`restore_weather` (which copies `keep→ci`), then `do_render` (which reads
`ci->weather_temp/desc`).

If `restore_weather` runs before `poll_async_fetches`, the fresh data from pump is written to
`keep` but never propagated to `ci`, so `ci->weather_temp == 0 && !ci->weather_desc[0]`
is true → empty widget for one tick.

**Root cause**: `restore_weather` copies `keep→ci` but `pump_weather_ok` updates `keep` later
in the same function. The copy-before-update window drops one tick's worth of data.

Exception — `--once` mode: `restore_weather` is not preceded by `poll_async_fetches`.
Instead, `gather_all` runs before `once_wait` (which pumps results via `once_pump_results`),
and `restore_weather` runs after `once_wait`. The ordering constraint
(pump-before-restore) is satisfied by `once_pump_results` → `restore_weather`.

Common mistakes:
- Assuming `pump_weather_ok` writes directly to `ci` fields (it doesn't — it writes to `keep`)
- Adding a new "restore" operation in `gather_all` without checking whether callers expect
  it to run before or after async pumps

When modifying `tick()` or the weather/WAN pipeline:
- Check ordering: `restore_*` must run after its corresponding `pump_*`, not before
- If adding a new `restore_*` call, verify the data it copies was last written by the
  preceding operation (sensor read vs. async pump)

### Rendering: info line must never silently omit content due to unknown terminal width

**Bug**: When output goes to a pty/pipe with unknown terminal size (TIOCGWINSZ returns 0),
`write_wrapped` returned early on `info_w < 1`, silently omitting the info line (wifi bars,
temp, SSID, etc.). The bars are self-sufficient in their formatting and don't need wrapping,
but the guard prevented any output at all.

**Root cause**: `write_wrapped` assumed rendering was impossible without a known width,
returning early rather than falling back to passthrough output. The panel buffer already
contains `\033[K` clear-line prefixes, making lines self-rendering.

**Prevention plan**: When modifying `write_wrapped` or adding a new rendering path for
info/overlay lines, never return early or skip output due to lack of terminal width.
Instead, write the line content as-is (no wrapping, no cursor positioning). The content
must always be visible regardless of output environment.

When modifying rendering code, check:
- Does any early-return guard silently drop output?
- Is there a fallback path that writes content when width is unknown?
- Is `info_w` being used as a "renderable" flag rather than a "wrapping width" hint?

### Local IP refresh — `refresh_local_ip_once` duplicates `ld_refresh`

Two functions fetch local IPs: `ld_refresh` (continuous mode, triggered by
`addr4_changed`/`addr6_changed`) and `refresh_local_ip_once` (`--once` mode). They share the
same `do_get_ip`/`get_local_ip6` logic but `refresh_local_ip_once` was missing
the `memcpy(keep.ipv6_local, local_ip6, 48)` step that saves the freshly
fetched IPv6 back to `keep` — the final `memcpy` to `local_ip6` then
overwrote it with stale `keep.ipv6_local`.

Common mistakes:
- Adding a fix to `ld_refresh` without mirroring it to `refresh_local_ip_once`.
- Calling `refresh_local_ip_once` before `gather_all` has populated `net.count`
  and opened `rtnl.fd`.

When modifying either function, check:
- Does the other function need the same change?
- Is the call site after `gather_all` (so `net.count` and `rtnl.fd` are ready)?

### Entry template (for project-specific entries)

```markdown
### <subsystem>: <bug pattern title>

<context — what makes this domain tricky>

Common mistakes:
- <mistake 1>
- <mistake 2>

When modifying <subsystem>, check:
- <checklist item 1>
- <checklist item 2>

Correct pattern:
\`\`\`c
// example code
\`\`\`

Areas that commonly produce project-specific entries:
- **State machines**: protocol handshakes, sequence tracking, ordering constraints — desync is silent
- **Variable-length encoding**: always validate remaining buffer length against declared length before reading
- **Cross-resource lifecycle**: paired acquire-use-release across subsystems — verify cleanup on ALL error paths
```

### Netlink address dump: `rtnl_dump` must use correct data struct per message type

`rtnl_dump` sends a family-filtered dump request to the kernel. With
`NETLINK_GET_STRICT_CHK` enabled (kernel 6.x+), the kernel validates
`nlmsg_len` against the expected struct for the message type:
`RTM_GETADDR` expects `struct ifaddrmsg`, `RTM_GETLINK` expects
`struct ifinfomsg`, etc. Using the wrong struct size causes the kernel
to silently return zero results — no error, no messages, just an empty
dump.

Common mistakes:
- Using a generic `struct ifinfomsg` for all dump types because all
  request structs share `unsigned char family` as their first field.
  The family byte is accepted regardless of struct size, but the kernel
  rejects the wrong `nlmsg_len` under strict checking.
- Not noticing the dump succeeds (returns 0) while producing no output
  — the error is invisible to the caller.

When modifying `rtnl_dump` or adding a new netlink dump function, check:
- Does the message type (`RTM_GETADDR`, `RTM_GETLINK`, `RTM_GETROUTE`,
  etc.) use the correct kernel struct for `nlmsg_len`?
- If adding a new message type to `rtnl_dump`, add a branch for its
  struct size.
- If the kernel adds strict checking for a new message type, existing
  dump functions may silently break.

Correct pattern: separate by message type with matching struct sizes
(`struct ifaddrmsg` for `RTM_GETADDR`, `struct ifinfomsg` for
`RTM_GETLINK`, `struct rtmsg` for `RTM_GETROUTE`).

### Three-tier async HTTP architecture (DNS + I/O thread + main loop)

The async HTTP subsystem uses three tiers:
1. **DNS threads** (parallel, one per target) — run `getaddrinfo` (blocking, a POSIX cancellation point), signal I/O thread via `eventfd_write` on completion.
2. **I/O thread** (single, on-demand) — `poll(-1)` on eventfd + socket fds, handles connect/send/recv for all targets, exits when idle.
3. **Main loop** — reads `struct http_result` slots, starts DNS threads on timers, never touches sockets.

Common mistakes:
- Forgetting to cancel DNS threads in cleanup or `--once` mode — `dns_cancel` must be called on each slot that may have a thread running (`state==1`).
- Weather HTTP result stored in `struct http_result` (mutex-guarded `data[96]`) — not a separate heap buffer. No manual free needed, but `http_result_cancel` must be called on early termination.
- Starting a new DNS resolve while a previous thread is still running — `start_dns` has an implicit precondition that `slot->state == 0`.
- Calling `http_result_write` from the I/O thread without holding `slot->lock` — the main loop reads under the mutex.

When modifying the async HTTP path (`once_wait`, `weather_cleanup`, `dns_cancel`, any slot/result code), check:
- Is `dns_cancel` called for every DNS slot that might have a running thread? (wan4, wan6, weather)
- Is `weather_fd` closed before the HTTP result slot is cancelled (correct order: fd → result cancel)?
- Are DNS threads cancelled before closing `weather_fd`? (DNS → fd → result slot)
- In `--once` mode: are straggler DNS threads cancelled and stale buffers freed before the single tick() render?

### `once_wait` — per-widget readiness, not "any"

`once_wait` previously used `any_results_ready` which returned 1 as soon as
any active widget (NET or WEATHER) completed. Weather typically resolves faster
than WAN, causing an early exit before WAN's DNS+conn+http chain finished.
The fix replaces `any_results_ready` with `active_wan_ready` + `active_weather_ready`
— two helper functions that each return 1 when their widget is either inactive
or has a result. The loop now only breaks when ALL active widgets are ready
(or the 3s deadline expires).

**Root cause**: Asymmetric fetch times (weather completes in <1s, WAN takes
1–3s) combined with an "any ready" exit condition — the fast widget's
completion terminated the wait for the slow one.

Common mistakes:
- Using a single "any results ready" check when multiple independent async
  targets have different expected completion times
- Assuming all async fetches complete within the same time window
- Cancelling all DNS/IO in `once_cleanup` without giving slow targets the
  full deadline window

When modifying `once_wait` or the once-mode pipeline:
- Each active widget must be checked independently — don't short-circuit
  the full wait when a fast widget completes
- Keep the 3s deadline as a ceiling for the complete wait, not per-widget

### `--once` mode cleanup pattern

`--once` mode runs a 3-second deadline loop, then cancels all in-flight operations before the single tick(). The cleanup must cover:

1. **I/O thread**: `io_cancel_all()` closes conn fds directly under `ctl.lock` (POLLNVAL wakes the I/O thread); `io_cmd` also checks and clears `cancel_all` on the next loop iteration.
2. **WAN DNS**: `dns_cancel(wan_dns_slot(0))` + `dns_cancel(wan_dns_slot(1))` — cancels running DNS threads.
3. **WAN HTTP results**: `http_result_cancel(&wan4_result)` + `http_result_cancel(&wan6_result)` — marks in-flight results as cancelled.
4. **Weather**: `dns_cancel(&weather_dns)` + close `weather_fd` + `http_result_cancel(&weather_result)` + reset `weather_state`.

Common mistakes:
- Ordering: close fd before freeing the buffer that fd reads into.
- Missing weather cleanup: weather uses its own state machine (not the I/O thread), so it needs explicit buffer/fd/DNS cleanup.
- Cancelling a DNS slot that was never started: `dns_cancel` checks `state != 1` and returns early — safe, but verifying `state` first avoids unnecessary lock contention.

## Multi-Thread Safety Audit

Verified against the checklist in TODO.md §Multi-thread safety checklist.

| # | Check | Status | Notes |
|---|-------|--------|-------|
| 1 | **DNS use-after-free** | ✓ | `dns_arg` freed exactly once: `dns_cleanup` (on cancel via `pthread_cleanup_pop(1)`) frees `d->ai` (if non-NULL) then `d`; normal exit frees `d->ai` inline and `free(d)` via `pthread_cleanup_pop(1)`. No double-free. |
| 2 | **DNS zombie avoidance** | ✓ | Every exit path calls `pthread_detach`: normal exit at end of `dns_thread_run`, external cancel in `dns_cancel` calls `pthread_detach(t)`. Double-detach returns EINVAL — harmless. |
| 3 | **I/O thread zombie avoidance** | ✓ | Detached via `pthread_detach(th)` immediately after `pthread_create` success in `ensure_io_thread` (dns.c:87). Kernel reclaims on thread exit. |
| 4 | **Race on I/O thread exit** | ✓ | I/O thread checks `pending_work` and `cancel_all` under `ctl.lock` before deciding to exit. DNS thread increments `pending_work` and reads `io_thread_active` under the same lock. If I/O thread exits between DNS completion and `ensure_io_thread` call, DNS sees `io_thread_active=0` and creates a fresh I/O thread. |
| 5 | **eventfd close race** | ⚠️ | DNS thread reads `ctl.efd` under lock, then writes to cached efd after unlock. I/O thread may close efd between DNS unlock and write. The failed write (`EBADF`/`EPIPE`) is benign — the eventfd is only a wakeup mechanism. A new I/O thread is created (or already running) which owns a fresh efd. No write to a user-data fd. |
| 6 | **No double I/O thread create** | ✓ | `ensure_io_thread` checks `io_thread_active` under `ctl.lock` before `pthread_create`. Only one thread can be inside this critical section at a time. |
| 7 | **Data races** | ✓ | `struct dns_slot` — all fields accessed under `slot->lock`. `struct http_result` — all fields under `result->lock`. `struct ioserv_ctl` — all fields under `ctl->lock`. `rtnl_mon_ctx.addr4_changed`/`addr6_changed`/`routes_changed` — writer uses `__atomic_store_n` (RELEASE), reader uses `__atomic_exchange_n` (ACQ_REL) or `__atomic_load_n` (ACQUIRE). |
| 8 | **Deadlock prevention** | ✓ | No function nests different lock types. `ctl.lock` is never held while acquiring `dns_slot->lock` or `http_result->lock`. Slot locks (`dns_slot.lock`, `http_result.lock`) are never nested. `has_dns_running` locks/releases each slot sequentially. |
| 9 | **FD leak** | ✓ | I/O thread closes socket fds in `io_close_conn` (called from `io_cmd`/cancel_all, `io_thread_exit`, `io_conn_error`, `io_conn_done`). eventfd closed in `io_thread_exit` and `io_idle`. Main thread never opens socket fds. |
| 10 | **EINTR** | ✓ | `poll()` in `io_loop_once` checks `errno == EINTR` and returns 0 (retry). Corect. |
| 11 | **Stale DNS after cancel** | ✓ | `dns_cancel` sets `cancelled=1` and `state=0` under lock. I/O thread's `io_start_conn` → `dns_read_slot` sees `state=0` and returns `0` (no addr). New `dns_start` begins with `state=0`. |
| 12 | **Per-state counters** | ✓ | `dns_try`/`conn_try`/`http_try` and retry timestamps in `struct cpu_keep` are accessed only by main loop (single-threaded `poll_async_fetches`). No synchronization needed. `io_conn` has no retry counter — I/O thread tries once and writes error to `http_result` on failure. |

**Overall**: No concurrency bugs found. One benign race (eventfd write to closed fd) is acknowledged — the write failure is safe because the eventfd is only a wakeup notification, not a data channel. A future improvement could move the write inside the lock to eliminate the race entirely, at the cost of increased lock hold time. The existing lock semantics (`PTHREAD_MUTEX_INITIALIZER` — non-recursive, non-errorcheck) are correct for all use cases.

The `rtnl_mon_ctx.addr4_changed`/`addr6_changed`/`routes_changed` race (item 7 above) was originally protected only by `volatile int`, which prevents compiler reordering but provides no atomicity or memory ordering guarantees. Fixed with `__atomic_store_n` (RELEASE) for the writer thread and `__atomic_exchange_n`/`__atomic_load_n` (ACQUIRE) for the main-thread reader — eliminating the read-then-clear race window and ensuring cross-thread visibility.


## Past Incidents

### cpu_info buffer overflow → corrupted IPv6 display

**Bug**: When multiple disk or network devices are present, `sto_line[64]`, `net_line[64]`, and `stou_line[128]` in `struct cpu_info` overflow into adjacent `local_ip[16]` and `local_ip6[48]` fields, causing garbled mount-options text (e.g. `:w,nosuid,nodev,noatime`) to replace the IPv6 address in the display.

**Root cause**: The per-device formatting functions use `snprintf` with a fixed 48-byte limit per entry but no overall bounds checking on the destination buffer. The pointer `*pp` always advances by `snprintf`'s return value (even when truncated), eventually pointing past the buffer. Adjacent struct members are overwritten.

**Prevention plan** (medium recurrence):
- All line buffers (`sto_line`, `net_line`, `stou_line`) increased to 1024 bytes.
- Render scratch buffers (`buf` in `render_text_info`/`render_panel`) increased to 1024 bytes.
- When adding new output to any of these buffers, compute remaining capacity and check bounds. Use the `fmt_usage` helper pattern (extract formatting into a small function) to keep token/branch limits.
- When modifying `struct cpu_info`, ensure any new fixed-size char buffer is ≥1024 if it accumulates multiple device entries.

### Terminal echo lost on exit when clock_main called from tests

**Bug**: After `make test`, the terminal shell stops showing typed characters (ECHO disabled). The `cleanup()` atexit handler restored terminal attributes from a stack variable (`restore_termios_ptr`) that had already gone out of scope, restoring garbage termios flags.

**Recurring track**: This is the second occurrence. The first fix replaced the dangling stack pointer with a static `struct termios saved_tios`. The second occurrence: `setup_terminal` is called multiple times per process (once per `clock_main` call in `main_test`), and each call overwrites `saved_tios` with the *current* terminal state — which already has ECHO off from the previous call. The final `cleanup` then restores ECHO off instead of on.

**Root cause**: Two related issues: **(a)** `saved_tios` was saved unconditionally on every call to `setup_terminal`, so after the first call disabled ECHO, the second call saved a snapshot with ECHO already disabled. **(b)** `atexit(cleanup)` is called once per `clock_main` invocation, but all registered handlers share the same `saved_tios` — the last writer wins.

**Prevention plan** (high recurrence):
- `setup_terminal` must save `saved_tios` only on the **first** call (`if (!saved_tios_valid) delayed save`).
- `clock_main` should call `setup_terminal` at most once — hoist the termios setup out of `clock_main` (e.g. into `main`) so atexit is registered exactly once.
- When modifying any code path that calls `setup_terminal` or registers `atexit(cleanup)`, verify that `saved_tios` captures the **original** terminal state, never a state that was already modified by a prior call.
- In test code (`main_test.c`): if tests call `clock_main` in sequence, and any of them enables terminal setup, the first call's termios state is the only valid snapshot. Future calls must skip saving.
