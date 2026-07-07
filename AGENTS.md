## Your role

Keep the project minimalistic, compact, and secure. Write readable source code that a single human can maintain.

Make the project correct, performant, and lightweight. When multiple solutions are possible, choose the one that performs better under heavy workload and extreme usage scenarios.

Worship lightweight resource usage. Prefer stack memory; use dynamic allocation only when static limits would constrain performance or throughput. Reuse buffers and objects.

Measure first. Before optimising, require a microbenchmark or latency profile.

Refer to [DOMAIN.md](DOMAIN.md) for the full retry landscape and memory management strategies.

## Domain Reference

For business rules, entities, CLI arguments, flows, retry landscape, memory management strategies, threading architecture, and glossary, refer to [DOMAIN.md](DOMAIN.md).

## Contributing

[CONTRIBUTING.md](CONTRIBUTING.md) is an extension of this document — read it together with this section whenever you read AGENTS.md. The rules in CONTRIBUTING.md apply to agents as they apply to human contributors, with the exceptions noted below.

Agents work directly on the repository and commit without pull requests, so the Getting Started (forking) and Pull Request Process sections do not apply. All other sections — Definition of Done, TODO.md Pruning, Code Standards, Bug Prevention, Quality Gate, Testing — are mandatory.

All items of the Definition of Done are mandatory before every commit, with one exception: a commit that contains only documentation changes (.md files) may skip the lint, coverage, and build steps. Every other commit must pass lint, coverage (test + coverage ≥ 80%), and build steps, plus TODO.md pruning and all other checks listed in CONTRIBUTING.md. Do not commit partial work — a commit that fails any quality gate item is not permitted.

## Constraints

An agent must never start a task or bugfix that is not written to TODO.md (for this scope, a "task" is any change that modifies code). When the user asks to do a task not yet in TODO.md, the agent must write it down in TODO.md before starting.

Run as an unprivileged user without sudo. When a required system tool is missing, ask the user to install it — do not attempt privilege escalation or circumvent this directive.

All items of the Definition of Done are mandatory before every commit, along with all other checks listed in CONTRIBUTING.md. A commit that fails any quality gate item is not permitted.

## Code Quality

See [CONTRIBUTING.md §Quality Gate](CONTRIBUTING.md#quality-gate) for mandatory code quality rules and thresholds.

## Design Mode (Approval Workflow)

When the user says "we are in design mode" or asks to investigate, fix, or implement something, follow this strict workflow:

1. **Update specification/planning docs as needed** — DOMAIN.md, AGENTS.md, and
   other spec/planning files are the expected outputs of design mode. Do NOT
   write DOMAIN.md or other spec updates to TODO.md — updating them during
   planning is the norm, not a separate task. Only add TODO.md entries when
   the user asks for code changes (bugfixes, features, refactors).
   Per CONTRIBUTING.md, doc-only changes skip the quality gate — no
   `make report` needed during planning. DOMAIN.md must reflect the
   **intended/designed behavior** (the spec), not what the current code
   happens to do — the code will be changed later to match.
   **Note:** System Plan mode is read-only — the user must switch to Build
   mode for the agent to write these files. Build mode during design does
   NOT constitute approval to start coding or committing.
2. **Present the plan** — show it and wait for the user to speak. While waiting, you are in *design mode* — only planning docs may be touched. Do NOT commit TODO.md, start coding, or modify any source/header (.c/.h) files until the user explicitly approves. End your presentation with **"Ready for review?"** — do not commit, do not proceed.
   - After editing planning docs, show `git diff --stat` so the user sees what changed.
   - Do not `git add` or `git commit` until the user says "commit", "approve", or equivalent.
3. **Wait for approval, then commit** — after the user approves, commit TODO.md (the plan) first.
4. **Proceed to code** — user specifies either: "tackle task 1" (single task), or "enter loop mode" (tackle multiple tasks sequentially).

Violations: committing code without approval, coding before the user says to, or skipping the TODO-first step are prohibited.

> **⚠️ Agent must not act on its own analysis**
> During design mode the agent may be asked to investigate or audit. The
> agent must **never** edit TODO.md or any other file based on findings
> from that investigation unless the user explicitly instructs it to do so.
> Present the findings, wait for instructions — do not pre-emptively add
> tasks, update plans, or fix things the user hasn't asked for.
> "Only planning docs may be touched" means ONLY docs the user explicitly
> names or agrees to in that conversation turn.

> **⚠️ System Plan/Build mode vs AGENTS.md Design Mode**
> The system's plan/build mode switching is independent of this workflow.
> In system Plan mode the agent's tools are read-only — no file of any kind
> can be written. The user must switch to system Build mode to enable the
> agent to edit spec/planning files (DOMAIN.md, TODO.md, etc.) on their behalf.
>
> System build mode does **NOT** constitute user approval for coding or
> committing — the agent must remain in AGENTS.md design mode (no code,
> only planning docs) until the user explicitly says "tackle", "proceed",
> "enter loop mode", or otherwise signals approval. Only then may code be
> modified.
>
> Even doc-only commits (DOMAIN.md, TODO.md, AGENTS.md, QA.md) require the
> user's go-ahead — "commit the plan" or "proceed" at step 3 of the workflow
> below. Do not commit any file during design mode without explicit
> permission.

> **⚠️ Incremental edits**
> When editing a document, check if there are already uncommitted changes before adding more. When the user asks for additional changes and the agent makes a mistake and wants to revert, NEVER run `git checkout` — it will lose the previous uncommitted state. Instead, compare the current state with what was there before and try to solve the problem. As a last resource, restore only the affected lines, not the entire document.

## Loop Mode

Agents enter loop mode on user request only.

During context compaction, make sure to keep in the context if agent is currently on loop mode or not.

When entering loop mode follow the steps:
- Check for work-in-progress changes in the worktree and try to match the task they belong to; resume it.
- If none, tackle the next task that makes sense.
- File and function names need not strictly match those in TODO.md — agents are free to reorganize them.
- Add multi level source directories (only main at src/, remaining grouped in meaningful subdirectories, e.g. src/module/code.c), moving files to group related responsibilities (SOLID).
- Apply the Definition of Done, including TODO.md pruning.
- Commit once per task (one task, one commit). Squash related changes into a single logical commit.
- Loop back until the user interrupts. For relevant missing critical technical details or blockers (e.g. a required system tool or permission), query the user. Do not query the user to decide the next task — pick the most relevant one and proceed.

## CI/CD Hardening

Treat all CI pipeline actions (steps with `uses:`) as a supply-chain risk — they run third-party code with full access to secrets and the runner. Replace them with direct CLI calls (git clone, gh, curl, docker). Since the project is public, an initial clone needs no credentials and leaves no trace. For pushing, use:
```
git -c "remote.origin.url=${GITHUB_SERVER_URL%%://*}://x-access-token:$GITHUB_TOKEN@${GITHUB_SERVER_URL#*://}/$GITHUB_REPOSITORY" push origin ...
```
Add `-c user.name=... -c user.email=...` as needed. For other operations, prefer `gh` with the ephemeral token.

Never interpolate GitHub expressions (`${{ ... }}`) inside `run:` shell scripts. Pass them through a step-level `env:` variable instead and reference the env var in the script. Direct interpolation bypasses shell quoting and opens injection vectors for untrusted input (PR titles, branch names, issue bodies, etc.).

Prefer `docker run <image> <tool>` over installing a binary and invoking it directly. A containerised tool only sees what is explicitly mounted — it cannot read the runner's environment variables, credentials, or filesystem.

Never mount `/var/run/docker.sock` into a tool container. The socket is equivalent to unrestricted root on the host: the container can spawn arbitrary privileged containers (`--net=host`, `--pid=host`, `--privileged`), defeating isolation entirely.

Run tool containers as the current user to avoid creating root-owned files in host-mounted directories: pass `-u "$(id -u):$(id -g)"` to `docker run`. This is especially important when the container writes output into a mounted volume.

For Trivy image scanning without the socket: export the image with `docker save <image> -o /tmp/image.tar`, then pass it via `--input /image.tar`. If the image was already pulled in a prior step, reuse that pull with `docker save` — no second registry hit needed.
