## Getting Started

1. Fork the repository.
2. Create a feature branch from `main`.
3. Make changes following the project conventions.
4. Run the full quality gate locally.
5. Submit a pull request.

## Code Style

Use `make format` to apply code style.

## Code Standards

### Design Principles

1. **Fatal errors must cascade by return** — errors propagate up the call chain until reaching `main()`, where all allocated resources are released before exit. Calling `exit()` / `_exit()` anywhere in the program is not permitted. The sole exception is the async-signal-safe signal handler, which calls `_exit(128 + sig)` as the only POSIX-safe way to terminate after a handled signal. Normal cleanup runs via `cleanup_all()` called explicitly before `return` — no `atexit` is used.

2. **No global or static shared variables** — all shared data must be passed explicitly through function arguments/parameters. A context struct may be used to group related state and threaded through the call chain. File-scope `static` variables (module-internal state) are also prohibited — any state that lives beyond a single function call must be held in a caller-provided buffer or context. The sole exception is `__thread` (TLS) variables in the async-signal-safe signal handler, where a context pointer cannot be passed and TLS is the only POSIX-safe mechanism.

    **Exception — immutable compile-time constants**: `static const` tables and scalars at file scope are permitted when they hold read-only constant data that is inherently part of the module's logic (lookup tables, emoji maps, string tables, device-prefix lists, and similar). Such constants must use UPPER_CASE names. This exception does not extend to mutable state, run-time initialized data, or variables holding per-instance configuration — those must still follow the caller-provided parameter rule.

3. **Avoid magic numbers** — use `sizeof` for buffer sizes and stdlib constants (`STDOUT_FILENO`, `EXIT_FAILURE`, `SIG_DFL`, etc.) when available, even if it increases token count.

4. **Deduplicate every operation — no repeated syscalls, libc calls, or computation in the same tick** — every kernel file open, netlink dump, ioctl, socket operation, syscall, libc function (strlen, memcmp, etc.), and computation block must execute at most once per monitor cycle. Any result consumed by multiple call sites must be computed once, stored in an intermediate struct, and passed by pointer. No consumer may re-issue the same syscall, re-parse the same data, or re-compute the same derived value that another consumer already produced.

   This covers:
   - **Syscalls**: open, read, write, ioctl, socket, sendmsg, recvmsg, poll, glob, access, stat; `ioctl(SIOCGIFNAME)` / `ioctl(SIOCGIFINDEX)` for the same ifindex↔name pair must execute at most once per tick
   - **Netlink**: RTM_GETROUTE, RTM_GETLINK, RTM_GETADDR, nl80211 commands — each message type sent at most once per tick; parsed data shared across consumers
   - **Stdlib calls**: strlen, memcmp, strcmp, sscanf, snprintf on identical inputs; cache string lengths and parse results
   - **Time queries — exactly three time-source calls in the entire application**:
     - `clock_gettime(CLOCK_REALTIME)` at `wait_next_tick` — the single per-tick capture. Its `tv_nsec` is used for drift-free nanosleep alignment to the next second boundary; its `tv_sec + 1` is returned as the next tick's epoch time.
     - `time(0)` at `clock_main` for startup — cascaded by argument to the first tick, init, and `once_wait`. Provides timestamps for refresh intervals, deadline, and delta computations.
     - `time(0)` in the `once_wait` poll loop — startup-only deadline check. Called up to 30 times in the worst case, but only in `--once` mode, outside the per-tick loop.
   - **Big O**: O(n²) or O(n×m) loops over the same dataset must be avoided in favour of a single O(n) pass with accumulated state. Review hot paths (rendering, route-wireless selection, widget iteration) for polynomial complexity
   - **Computation**: any derived value (unit selection, thresholds, percentages, formatted strings) computed once and reused; no re-formatting the same number twice
   - **ENOENT caching**: any `access()`, `open()`, `statx()` call that returns `ENOENT` must be cached via a per-feature presence flag (bitmask or similar); the same file must never be probed again in subsequent ticks. This applies across all lifecycle stages.

     **Exception — hot-pluggable discovery paths**: globs over directories that can change at runtime (hot-plugged USB drives, NVMe, eGPU) must NOT cache their absence permanently. Instead, re-glob the directory each tick and compare against the previous result set to detect changes. If the set is identical, skip all per-device feature discovery. On change, re-run full discovery for the affected paths. Hot-pluggable paths: `/sys/block/*`, `/sys/class/hwmon/hwmon*`, `/sys/class/drm/card*`.

   Applies to all lifecycle stages: startup discovery, per-tick collection, on-demand refreshes (public IP, weather), and rendering.

5. **Prefer bounded buffers with incremental scanning** — proc and sys files are kernel-internal streams whose sizes are not guaranteed by any API contract. Do not guess a fixed stack buffer large enough to hold an entire file. Instead, prefer incremental scanning: read a fixed-size chunk, extract what is needed (filter/reduce), discard the chunk, and reuse the same buffer for the next chunk. When a full-file view is genuinely required (e.g. JSON parsing, line reordering), allocate dynamically with automatic cleanup (`free` paired with the allocation point) rather than picking a magic-numbered stack buffer. See [DOMAIN.md §Buffer Strategy Audit](DOMAIN.md#buffer-strategy-audit) for the complete read-strategy table.

6. **Active widget set exclusively drives all computation** — every kernel file open, syscall, memory allocation, thread creation, socket operation, DNS resolve, and parse must be strictly scoped to the active widget set. No code path outside the active widgets may execute. If a widget is not in the active set, absolutely no computation for its data sources may occur — not merely skipping display, but skipping every open, read, thread spawn, allocation, and parse that would be needed to produce its output. This applies across all lifecycle phases: initial discovery (glob/scan), per-tick collection, async background operations, and I/O thread setup. A widget's absence from the list must make its entire dependency tree unreachable.

7. **Signal handler must be async-signal-safe — set a flag, defer cleanup** — the SIGINT/SIGTERM handler must only write one `__thread volatile sig_atomic_t` flag (`tls_terminated = 1`). This is the only POSIX-guaranteed async-signal-safe mechanism for communicating with the main thread. All terminal state restoration (ECHO, cursor), I/O shutdown, and fd cleanup runs in `cleanup_all()` called from the main thread after the loop detects `tls_terminated`. On a second signal (flag already 1), call `_exit(128 + sig)` — the only POSIX-safe way to terminate inside a handler.

    Terminal ECHO restoration (`tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios)`) must use `tcsetattr`, not escape sequences — the kernel's `ECHO` flag is a line-discipline attribute, not a terminal-emulator visual property. RIS (`\033c`) and other DEC private mode resets only affect the emulator layer and cannot restore `ECHO`. Although `tcsetattr` is not in the POSIX async-signal-safe list, on Linux it is a direct `ioctl` syscall with no allocation or locking — it is safe to call from `cleanup_all` (main thread, not in signal context).

    Cursor-show (`\033[?25h`) is an emulator attribute and is restored via async-signal-safe `write` in `cleanup_all`.

8. **Avoid duplication** — when the same logic appears in more than one place, extract it into a shared function or module. Be vigilant about copy-pasted code blocks that differ only in variable names, repeated inlining of the same algorithm, and logic scattered across functions that performs the same conceptual operation. Prefer a single authoritative implementation — even small duplications drift apart and become bugs.

9. **Eliminate dead production code** — production code (function, variable, branch) only executed by tests must be removed or moved to `tests/` as a shared utility. Weak-symbol definitions in `syscall.c` always overridden by strong test definitions are the only exception.

10. **Remove dead unit tests** — unit tests that do not execute any production code path and exercise only mocks, stubs, or test infrastructure provide no coverage value and must be removed.

### Code Organization

Apply SOLID principles — especially Single Responsibility and Dependency Inversion.
Follow the Transformation Priority Premise and Object Calisthenics.
To make unit tests easy, keep code decoupled and context-agnostic. Write independent functions that receive arguments, process, and return, without relying on global, static, or shared variables. Apply interface segregation and dependency inversion.

### Safety Checklists

Before committing, consult the applicable checklists in [QA.md](QA.md):
- [Memory Safety Checklist](QA.md#memory-safety-checklist) — required when modifying dynamic or shared memory
- [Pre-Merge Safety Checklist](QA.md#pre-merge-safety-checklist) — required for every change
- [Domain-Specific Pitfalls](QA.md#domain-specific-pitfalls) — review project-specific entries

## Testing

Create unit tests without relying on third-party testing library/framework, build our own.

Never rely on operations that require root access or are machine-state dependent:
- For filesystem operations, use `memfd_create` (from `<sys/mman.h>`) when an open fd suffices; otherwise use `mkdtemp` to create a random temporary directory before each test and clean it up on exit regardless of success or failure.
- For socket operations, always bind to port 0 (random port).
- For operations that require root or are not suitable for temporary files or random socket binds, use stubs/mocks/spies to mock the stdlib or syscall layer — do not rely on real kernel resources.
- Reading from `/proc`, `/dev`, or `/sys` is considered machine-state dependent and must be avoided or mocked.

Tests that depend on the execution of external system tools (database, proxy, external service, external binary) are considered integration tests. They must not block unit tests — they are optional and serve only to help test functionality.

Use dummies, stubs, fakes, mocks, and spies where appropriate.

Never add `#ifdef`, `#ifndef`, or `#else` blocks to production code to alter its behavior under test. Use dependency injection, weak symbols, or link-time substitution instead — test concerns must not leak into production sources.

### Test isolation strategies

#### Mocking via weak symbols

Define syscall and stdlib wrappers as weak symbols in a single `src/util/syscall.c`. Tests provide strong overrides in `tests/mock_syscall.c` — at link time, the linker picks the strong definition, replacing the real implementation without `#ifdef` guards or separate compilation.

Each mock is controlled by global variables (`_mock_<name>_ret`, `_mock_<name>_count`, etc.) defined in `tests/mock_syscall.c` and reset before each test via `mock_sys_reset()`. The mock function checks these variables and returns a controlled value; when the control is set to a fall-through sentinel (e.g. `-1` for functions that return 0 on success), it calls the real implementation via the weak default.

Add stdlib wrappers (`open`, `read`, `write`, `stat`, `access`, etc.) only when a test concretely needs them — no speculative wrapping. The cost of adding is low (declare + weak impl + mock override), so there is no reason to front-load. Extend on demand.

#### Coverage exclusions

No piece of production code shall be excluded from coverage. Avoid using `#ifdef`/`#ifndef`/`#else` preprocessor blocks with flags that remove code blocks from coverage — they create untested code paths that silently rot.

Weak wrappers that are always overridden by strong test definitions are an exception: they are dead code in the test binary and shall be excluded from coverage calculations.

Global cppcheck suppression via `--suppress=*` CLI argument is not allowed. In-source suppression comments (e.g. `// cppcheck-suppress <rule-id>`) may be used only when the check is unfixable and a comment explaining why is included.

## Quality Gate

No commit may bypass any quality gate item. All thresholds are mandatory.

### Running checks

`make lint` chains format, cppcheck, lizard, and loc-check — always run it first as it catches formatting and lint issues without needing a build.

`make lint` is the preferred first gate: it catches formatting and
lint issues before a build is needed. Run `make coverage` to run tests
and verify coverage in one step (note: coverage depends on test, so
a separate `make test` is redundant). Finish with `make [-j4]` to
confirm the production binary builds cleanly with no warnings.

### Fixing violations

Do not circumvent build warnings, lint violations, or quality gate failures
with workarounds, suppressions, or half-baked fixes. Every violation must be
resolved at the root cause by refactoring the code.

Follow the action table below strictly. Do not circumvent metrics by using
ternary operators (`? :`) instead of `if`/`else`, merging unrelated statements
onto one line, or similar cosmetic workarounds — address the structural issue
directly. Any cosmetic attempt to reduce line count (collapsing `if`/`for`/`while`
bodies onto the same line, removing blank lines, joining unrelated statements)
will be undone by `make format` anyway, so it is always wasted effort.

| Issue | Required action |
|---|---|
| **File > 300 LOC** | Split into multiple files based on responsibility |
| **Function > 40 LOC** | Split into multiple smaller functions |
| **Cyclomatic complexity > 6** | Split into multiple functions (extract conditions, reduce nesting) |
| **Tokens per function > 150** | Split into multiple functions |
| **Nested control depth > 3** | Extract inner blocks into helper functions |
| **Parameters > 5** | Split into multiple functions; when splitting is not feasible (e.g. callback conforming to a fixed signature), pack parameters into a struct and pass the struct |
| **Unused parameter** | Remove it from the function signature and update all callers. Never suppress with `(void)param;`, `__attribute__((unused))`, or any other mechanism that papers over the unused parameter — make it used or remove it. Instead of suppressing, put the information in the context structure so the parameter is genuinely needed. Exceptions: the `sigaction` handler (fixed POSIX signature `void handler(int sig)`) and mock functions conforming to a weak-symbol interface — both permit a single `(void)param;` without comment. |
| **Unused function** | Remove it entirely |
| **Unused local variable** | Remove the variable or its dead assignment. If the value is genuinely needed (e.g. reading from a device register or bit stream), use the value in a validation check instead of `(void)` suppression |
| **Coverage < 80%** | Add missing test cases to reach 80% line coverage. Run `make cov-list` to see all files below 80% sorted ascending; prioritize files with the lowest coverage first. |
| **gcov output in project root** | `.gcov` files are generated inside `obj/gcov/` — never run gcov from the project root directory. If `.gcov` files appear in the root, delete them immediately; they will break `git status` and confuse the build. |

Additional checks enforced by the quality gate:
- cppcheck `--enable=all --check-level=exhaustive` must succeed
- gcov — at least 80% line coverage
- cohesion & coupling — max 1 public abstraction per file (recommended, not enforced)

### Cppcheck suppression discipline

Global cppcheck suppression by adding `--suppress=*` to the Makefile is not
allowed. Must try to fix the root cause by refactoring first.

In-source suppression comments (e.g. `// cppcheck-suppress <rule-id>`) may be
used only as a last resort when the check is genuinely unfixable (e.g. a false
positive due to cross-file usage that cppcheck cannot see). Include a comment
explaining why the suppression is necessary.

## Definition of Done

All items below are mandatory. A change is not complete until every item passes.

1. **Design Principles and Safety Checklists applied** — confirm that the code follows §Design Principles and that applicable §Safety Checklists were consulted
2. **Code style applied and lint passes** — `make lint` must succeed (see §Quality Gate). When lint reports issues, consult §Fixing violations and §Cppcheck suppression discipline before re-running.
3. **All tests pass and coverage threshold met** — `make coverage` must succeed (see §Testing). When coverage is below threshold, consult §Fixing violations before re-running.
4. **Code builds with no warnings** — `make [-j4]` must succeed
5. **Regression test for every bug** — every fixed bug must have a test that failed before the fix and passes after (see QA.md §Bug Prevention)
6. **TODO.md pruned** — completed sections deleted from file per §TODO.md Pruning rules
7. **Documents consistent with code** — DOMAIN.md is source of truth; code must match spec

Items 2 and 3 may be replaced by a single `make report` invocation (see §Running checks), which runs lint and coverage in sequence and prints a summary. Note that `make report` only shows pass/fail per step — if it fails, run `make lint` and `make coverage` individually to see the full detail.

## TODO.md Pruning

Apply these steps in order:
1. Read the current TODO.md
2. For each section (`## ...`), check every bullet:
   - If **all** bullets are ticked (`[x]`), **delete the entire section** (header + all its bullets) from TODO.md
   - If **any** bullet remains unticked (`[ ]`), keep the section
3. Write the pruned file — do not leave empty headings

## Domain Knowledge

Refer to DOMAIN.md for business rules, CLI arguments, entities, flow/lifecycle, retry landscape, memory management strategies, threading architecture, and glossary.

## Task Lifecycle

A task is considered completed only when it fulfils all conditions in the Definition of Done.

## Pull Request Process

1. Ensure the complete Definition of Done is met (see §Definition of Done above).
2. Ensure all quality gate checks pass.
3. Update DOMAIN.md if architecture, CLI, or business rules change.
4. Update TODO.md following the TODO.md Pruning rules.
5. Follow the bug prevention process — see QA.md §Bug Prevention.
