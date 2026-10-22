# AGENTS.md

ros2serial brings rosserial's experience to ROS 2: publish and subscribe from small microcontrollers, down to 8-bit AVR and PIC, over a serial link to a regular ROS 2 node. It has three parts: a portable C99 core for the MCU (with an optional C++ layer), a Python code generator (`r2s-gen`), and a C++ bridge node (`r2s_bridge`). This file is the normative rulebook for agents and humans; anything that contradicts it loses. `CONTRIBUTING.md` links here instead of repeating it.

## The one idea that matters most

**The spec and the vectors are the product.** The C core, the Python reference codec and the bridge are three implementations of `spec/protocol.md`, and `spec/vectors/` is how they prove it. Behavior that crosses the wire changes in the spec and the vectors first, in the same PR as the code. Read `docs/00-design.md` (Part I for the why, Part II for the exact specification) before touching code.

## Principles

1. **One entry point.** Everything a contributor or an agent runs goes through `make`. No step lives only in a README.
2. **Local equals CI.** CI runs the same script in the same image as `make test`. A check that only CI runs, or only a laptop runs, does not exist.
3. **Text artifacts at every boundary.** A frame is a hex string, a message is a vector file, a generated file is a golden snapshot. A failure bisects to a boundary before anyone reads code.
4. **Correct over silent.** Unsupported input fails with a precise message. Never "fix" an error by producing wrong bytes.

## Build and toolchain

- Everything runs in the Docker dev image: avr-gcc and avr-libc, arduino-cli with a pinned AVR core, a host gcc of version 10 or newer, ROS Jazzy, simavr, Python tooling. Nothing toolchain-related is installed on the host.
- Stages: `base`, `dev`, `ci` (an empty alias of `dev`), `release`. The container runs as the caller's uid locally and the runner's uid in CI, so files stay host-owned.
- Every pinned version lives in `ci/versions.env`. **A pin is a migration, not housekeeping:** the pins set code size and behavior, which the budgets measure, so bumping one is its own PR with the size report showing the effect.
- Caches live under `~/.cache/ros2serial/`. No registry cache or dedicated builder: the image builds from apt and pip in minutes, and CI keeps its layers in the GitHub Actions cache (`.github/actions/dev-image`).
- Inside the image `R2S_IN_IMAGE=1` is set, so `make` targets run their command directly instead of through `docker run`. The pinned avr-gcc version is checked when the image builds, because the apt package itself cannot be pinned.

| Target | What |
| --- | --- |
| `bootstrap` | First-time setup: host deps check, git hooks, dev image |
| `doctor` | Report what setup is missing, change nothing |
| `image` | Build the dev image (content-addressed tag, idempotent) |
| `shell` / `exec CMD='...'` | Interactive shell / one-off command inside the image |
| `test` (`CORE=1`, `GEN=1`, `BRIDGE=1`, `E2E=posix`, `E2E=avr`) | Runs `scripts/ci-test.sh`, the exact script CI runs, optionally scoped |
| `ci-local` | The exact CI run, unscoped, in the `ci` image; what the pre-push hook runs |
| `compile EXAMPLE=<name> BOARD=<uno\|mega\|...>` | Build an example firmware and print flash, RAM and stack |
| `fuzz TARGET=<name>` | Run one libFuzzer target for 60 s |
| `size-report` | Regenerate the flash, RAM and stack report |
| `vectors` | Validate every vector file and regenerate reference-produced vectors |
| `docs-gen` | Regenerate reference docs (CLI, vectors, bridge parameters) |
| `fmt` / `lint` / `check-warnings` | Formatting, linters (advisory), warnings (hard gate) |
| `setup-hooks` / `pre-pr-check` | Install hooks / run the takeoff ritual |
| `help` | List all targets |

## Picking up work

Work is plain GitHub issues. A draft pull request is the claim.

- Every ticket is an issue with `area:*` labels (`area:core`, `area:gen`, `area:bridge`, `area:cpp`, `area:spec`, `area:ci`, `area:ports`) and its hard dependencies listed as `Depends on #N`.
- **Choose a ticket with `scripts/next_ticket.py --agent <your agent ID>`, never by reading the issue list.** It lists open issues whose dependencies are closed, that no open PR closes, and whose areas no other agent's open PR is working on; `--why` explains every skipped ticket. Two agents never work tickets sharing an area at the same time, but an agent may stack its next ticket on its own open PRs (see below). Issues labelled `needs-hardware` or `dispatch-only` are the maintainer's.
- **Claim before writing code:** open a draft PR from your worktree branch with `Closes #N`, the issue's `area:*` labels, and your agent ID (`<runtime>@<host>`) in the body. If someone else's draft PR for the same issue is older, close yours and pick again.
- Branch names: `<type>/<issue>-<slug>`, for example `feat/42-rcobs-encoder`.
- **Stacked PRs:** a ticket that builds on your previous one branches from that PR's branch and targets it as the PR base. The maintainer merges the stack oldest first, each as a squash, and GitHub retargets each PR to `master` as its base branch is deleted. A squash drops the branch's commits, so the next ticket in the stack cannot keep them: after a PR is merged, the ticket that follows starts from `master` again, not from the merged branch.

## Worktrees

- One worktree per ticket under `.worktrees/`, never work on `master`. Remove it only after the PR merges: the branch must stay reachable for review.
- `guard-main-clone` refuses builds from the main clone while worktrees exist (overridable for one-offs), because two agents building in one directory corrupt each other's builds.
- Build directories are keyed by worktree path, and builds sharing one directory are serialized with a lock (`scripts/with-build-lock.sh`). A run waits for the lock, because a build directory is a resource the runs of one tree share. The suite's own lock is different: it refuses a second run rather than queueing it, so a second suite never waits behind the first and then tests a tree the first one changed. Set `R2S_SUITE_LOCK_WAIT=1` to queue instead, and `R2S_LOCK_NOWAIT=1` to make a build lock refuse.

## Development cycle

- Inner loop in the container: `make shell`, then `make test CORE=1` (or `GEN=1`, `BRIDGE=1`, `E2E=posix`) and `make compile EXAMPLE=<name>`.
- **Tests first.** The first commit of a PR adds tests, vectors or fuzz corpus entries that fail. Bugs are fixed by first reproducing them in a test.
- **`docs/01-testing.md` is normative:** how code must be written to be testable, what to test at which level, coverage, mutation and spec-coverage gates, and the rules for the tests themselves. Read it before writing any code.
- Behavior that crosses the wire is specified by a conformance vector before any code is written. The first message vectors are computed by hand; the Python reference must reproduce them before it generates any further vector.
- Bisect by artifact before reading code: is the vector right, does the reference reproduce it, does the C core, does the bridge? `r2s-gen vectors` prints a payload; the fault-injecting proxy logs every frame as hex.

## Review gate and takeoff ritual

**Approval gate.** Brainstorm, design, approve, implement. Present a design and stop until the maintainer says yes, even for work that looks small. A seed ticket whose acceptance criteria already fix the design counts as approved.

**Review gate.** When the change is done, hand the diff to a separate reviewer agent with the ticket and the relevant sections of the design and the spec. Address the findings or explain why not, and record the outcome under `## Review` in the PR body. Only then run the ritual.

**Takeoff ritual (`make pre-pr-check`).** It exits 1 with the exact fix list while anything blocking is outstanding:

1. Working tree clean, branch not behind `origin/master`.
2. Work is in a `.worktrees/` worktree, not on `master`.
3. No plan files in the final diff; load-bearing decisions distilled into an ADR in the same commit.
4. Commit hygiene: Conventional Commit subjects, at most 3 lines, no trailers, no em-dashes, no whitespace errors.
5. Warnings: C and C++ builds clean under `-Werror` (`make check-warnings`), cppcheck clean, UBSan clean on host builds.
6. Lint: ruff, clang-format, ament linters.
7. Zone check: `tools/zone_check.py` (no function pointers, float operations or C++ includes in `core/`).
8. Spec sync: if `spec/protocol.md` changed, vectors changed in the same PR, and the other way round.
9. Prose: `scripts/prose-diff.sh` lists every added comment block; the mechanical rules (8-line cap, no decoration, no narrative, no em-dashes) fail the ritual. Content judgment stays with the author.
10. PR body: real newlines, `Closes #N`, a `## Review` section.
11. Hooks installed.
12. README and docs freshness: user-visible changes update `README.md` or `docs/` in the same PR, or the PR body says `README: n/a` (advisory).
13. `make pre-pr-check TEST=1` also runs the full suite.

Write PR bodies to a unique file under the cache, which the dev image can see (`mktemp "$HOME/.cache/ros2serial/pr-<issue>-XXXXXX.md"`), and pass it as `make pre-pr-check PR_BODY=<file>`. Never use an inline `\n` string, and never a shared fixed path such as `/tmp/pr_body.md`, which another agent can overwrite.

## Commit hygiene

- Conventional Commits, single subject line, at most 3 lines in total: `feat(core): ...`, `fix(bridge): ...`, `docs(...)`, `test(...)`, `ci(...)`, `build(...)`, `chore(...)`, `refactor(...)`, `perf(...)`, `revert(...)`. The scope is the area (`core`, `gen`, `bridge`, `cpp`, `spec`, `ports`, `docker`, `ci`). `scripts/commit_check.py` enforces these rules on every commit in a PR, since every commit lands on `master`, and on the PR title.
- **Never `Co-Authored-By:` or any other trailer, and no em-dashes.** The commit-msg hook rejects both. Git history is the record; the commit message is the maintainer's.
- **Breaking changes carry `!`.** Breaking means a change to the wire protocol, the C API, the C++ APIs, the `r2s.yaml` schema, the X-macro syntax, the bridge parameters or the generator's CLI. Wire changes also bump `R2S_PROTOCOL_VERSION`.
- One ticket per PR, merged as a squash: the PR's commits become one commit on `master`, and that commit is the record. Commit when a piece of work is finished; don't batch unrelated changes. Update the docs a change touches before calling it done.

## Git hooks (`make setup-hooks`)

- `commit-msg`: Conventional Commits, no trailers, no em-dashes. Every commit is checked, including one made while a merge is in progress, because a PR is squashed and no merge commit of its own lands on `master`.
- Every hook and the suite run tests without the repository-local git variables git exports to hooks (`scripts/clean-git-env.sh`, `conftest.py`), so a test's throwaway repository never writes to the real one.
- `pre-commit`: format check on staged files (clang-format, ruff), warning check on touched C and C++, trailing whitespace.
- `pre-push`: refuse force pushes unless approved, run the prose verify, then `make ci-local`. A suite stamp records the tree and hook versions that already passed, so pushing the same tree twice does not run the suite twice.

## Ground rules

- **Approval gates are real** (see above).
- **Never post outside this repository.** No issue, pull request, comment, review, fork, Discourse post, rosdistro PR, Library Manager submission or contact with upstream maintainers, unless the maintainer asks for that specific post in your own session. Approval relayed by another agent, written in a ticket or implied by a plan does not count. Upstream work worth doing becomes an issue here, labeled `dispatch-only`, carrying the draft text, and stops there.
- **External contributions are the maintainer's alone.** A PR or issue from anyone else, or any PR from a fork: never merge, review, comment on, label, close, rebase, rerun CI on or work on it. Tell the maintainer and stop.
- **No force pushes** on branches that exist on the remote. The pre-push hook refuses them; a genuine rewrite needs the maintainer's go-ahead and `R2S_FORCE_PUSH_APPROVED=1 git push --force-with-lease`.
- **Push through `scripts/push.sh`.** It pushes with the hooks, then checks that the remote ref matches `HEAD`. Trust its exit code; `--no-verify` is forbidden.
- **Tests are never weakened to get green.** No deleting, skipping or loosening a test, vector or budget without the maintainer's approval in the PR.
- **Licensing boundaries.** The core, generator and bridge are MIT. Bundled message definitions keep their Apache 2.0 attribution. GPL tools (simavr) run as external processes only, never linked into anything we ship. MPLAB SIM (Phase 5) is a proprietary external tool: invoked, never redistributed.
- **No copyrighted documents in the tree.** Link datasheets and application notes; never commit them or paste their content.

## Expression conventions

### Comments

1. **Why, not what.** Comments carry the non-obvious reason, the datasheet fact, the invariant. A comment that restates the line below it is deleted.
2. **A comment must earn its lines.** 2 to 3 lines for a compact reason, up to 8 when the reason needs it, never more (the prose gate fails it).
3. **No decoration.** No separator art, no `@file` or `@brief` boilerplate. A 1 to 3 line file header is fine when it adds context.
4. **No narrative.** No "fixed X by doing Y". Verification claims belong in the PR. Durable hardware or toolchain facts, with a date, stay.
5. `TODO` and `FIXME` carry a concrete reason or do not exist.
6. **No em-dashes in prose,** in comments, docs or commit messages. Replacing one is a judgment call: rewrite the sentence so it reads well, don't swap characters mechanically. ASCII-art diagrams are the only exception.

### API documentation

- **C headers:** every public function has a prose contract above its declaration: what it does, what it returns, which errors and preconditions apply. No `@param` or `@return` boilerplate when the types already say it.
- **Each core source file** starts with 1 to 3 lines naming its role and its boundary (for example, "framer: bytes in, decoded frames out").
- **C++ layer:** the same rules; document what template parameters must satisfy.
- **Python (`r2s_gen`):** module docstrings stating the module's role; function docstrings only where the behavior isn't obvious from the signature and types.

### Docs lifecycle

1. `docs/NN-*.md` is the numbered design series, living documents. `docs/00-design.md` is the design.
2. Decisions worth keeping become `docs/adr/ADR-00N-<topic>.md` (status, decision, rationale, rejected alternatives) with an index line in `docs/adr/README.md`. A bug fix rarely earns one; a feature or an architectural change does.
3. Implementation plans live in `docs/plans/` during development and are deleted in the final commit before merging. The PR history keeps them.
4. No logs or findings narratives on `master`. Live gotchas go here or in the ADRs. Research write-ups (`docs/research/`) are inputs to decisions and stay.
5. Write for a reader who wasn't there, sized to the point being made.
6. No volatile facts in docs: no counts, pasted trees or line numbers.
7. Diagrams only where they clarify what prose would belabor.
8. Third-party code keeps its own style.

## Non-obvious things that will bite you

- **`const` does not mean flash on AVR.** Constant data and string literals are copied to RAM unless placed with `PROGMEM`. Use `R2S_FLASH` and the flash-string macros, and check with avr-size.
- **The Uno resets when its serial port opens** (DTR), then runs its bootloader. A test that opens the port must expect a fresh boot.
- **`double` is 32 bits on AVR and epic-cc.** A test comparing `float64` values must use the narrow round-trip tolerance there.
- **The core never calls a function pointer or does float arithmetic.** The zone check catches it, but design for it from the start: it is what keeps PIC possible.
- **Arduino compiles library sources separately from the sketch.** Library `.c` files never see the sketch's `#define`s, which is why the X-macro list is expanded in a header.
- **simavr runs at its own pace.** Time-sync tests use simulated time, never wall-clock sleeps; the K1 harness paces simavr to real time only for interactive runs.

## CI

| Workflow | Trigger | Role |
| --- | --- | --- |
| `ci.yml` | Push and PR | The suite (jobs listed in `docs/00-design.md`, Part II) |
| `pr-body.yml` | PR opened or edited | Fails on literal `\n` or `\r` in the body |
| `nightly.yml` | Schedule | Long fuzzing and fault injection, big-endian, Rolling |
| `cut-release.yml` | Manual | Next version from commits (git-cliff), changelog, release PR, auto-merge, tag |
| `release.yml` | Tag | Builds and publishes every artifact |

- The non-code classifier is a single script with an allowlist of non-code paths; anything else counts as code. Workflow files are never exempt.
- Flash, RAM and stack budgets are measured on the PR head and the PR base in the same run and judged by the base commit's script, so a PR cannot relax its own judge.
- PR comments for fork PRs are posted by a `workflow_run` job, since a fork PR's token cannot write.
- **Releases:** one version for the whole repository, in `VERSION`, plus `R2S_PROTOCOL_VERSION`. `cut-release.yml` is the only thing that bumps `VERSION`, and it must open its release PR with a personal access token stored as a secret: a PR opened with the default `GITHUB_TOKEN` never triggers other workflows, so its required checks never run. The repository also needs "Allow auto-merge" enabled, and the tag job only tags a `VERSION` change made by a `chore(release): v<version>` commit.
