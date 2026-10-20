# ros2serial engineering practices (v1)

Oct 20, 2026 · @Alexis Pojomovsky · companion to `proposal-v3.md`

> **What this is.** How ros2serial is built: the workflow for agents and humans, the repository tooling, the gates a change passes through, and the writing rules. `proposal-v3.md` says *what* we build; this says *how*. Most of it comes from epic-cc, where it has worked; each section says whether a practice is adopted as is, adapted, or left out, and why. Ticket B1 turns this document into `AGENTS.md`, the normative rulebook, and `CONTRIBUTING.md` links to it rather than repeating it.

## Principles

1. **One entry point.** Everything a contributor or an agent runs goes through `make`. No step lives only in a README.
2. **Local equals CI.** CI runs the same script in the same image as `make test`. A check that only CI runs, or only a laptop runs, does not exist.
3. **One rulebook.** `AGENTS.md` is normative. Anything that contradicts it loses.
4. **Text artifacts at every boundary.** A frame is a hex string, a message is a vector file, a generated file is a golden snapshot. A failure bisects to a boundary before anyone reads code.
5. **Correct over silent.** Unsupported input fails with a precise message. Never "fix" an error by producing wrong bytes.

## What comes from epic-cc

| Practice | Status | Why |
| --- | --- | --- |
| `make` as the only entry point, `make doctor`, `make bootstrap` | Adopted | Same problem: a multi-toolchain repo where agents must not guess commands |
| Docker dev image, `ci` as an alias of `dev` | Adopted | avr-gcc, arduino-cli, ROS, simavr in one pinned place |
| Pinned toolchains are migrations, not housekeeping | Adapted | epic-cc pins clang because it parses clang's output. Here the pins (avr-gcc, Arduino AVR core, ROS images, epic-cc bundle) set code size and behavior, which our budgets measure |
| `AGENTS.md` with a lead "one idea" section and a gotchas section | Adopted | The lead idea is different, see below |
| Approval gate (brainstorm, design, approve, implement) | Adopted | Design decisions stay with the maintainer, whatever the size of the change |
| Review gate (a separate reviewer agent before the takeoff ritual) | Adopted | The cheapest place to catch a wrong design or a weak test |
| Takeoff ritual (`make pre-pr-check`) | Adopted, with ros2serial checks added | Same failure modes: dirty trees, plan files, prose drift, broken PR bodies |
| Git hooks (commit-msg, pre-commit, pre-push with force guard, local CI and suite stamp) | Adopted | Catches problems before they cost a CI cycle |
| Commit hygiene (Conventional Commits, short, no trailers, no em-dashes, `!` for breaking) | Adopted | Changelog and version bumps come from commit subjects |
| One worktree per ticket, `guard-main-clone`, per-worktree build dirs, build lock | Adopted | Parallel agents on one machine corrupt each other's builds otherwise |
| Comment and docs rules, prose gate | Adopted, Rust doc rules translated to C, C++ and Python | Same standard of writing |
| Docs lifecycle (design series, ADRs, ephemeral plans, no logs) | Adopted | Keeps `main` readable for someone who wasn't there |
| Never post to third-party repositories; external contributions are the human's | Adopted, made stricter | ros2serial touches more third parties than epic-cc: rosdistro, Discourse, upstream ROS packages, the Arduino Library Manager |
| Push helper that verifies the remote ref | Adapted | A small `scripts/push.sh` instead of `epic-tasks push` |
| Non-code CI classifier (safe direction only) | Adopted | Docs-only PRs skip heavy jobs; a misclassification can only cause an extra run |
| Head-vs-base measurement judged by the base commit's script | Adopted | Flash, RAM and stack budgets, same rule: a PR cannot relax its own judge |
| `pr-body.yml` workflow (literal `\n` check) | Adopted | The bug is client-side and recurs |
| `cut-release.yml` (version from commits, release PR, tag) | Adopted | Including its token trap, see Releases |
| Single version source | Adapted | One version for the whole repository, plus the protocol version |
| Generated README and reference docs (`docs-gen`) | Adapted | Only for reference pages (CLI, vectors, parameters), not the README |
| epic-tasks board and claim tool | Not adopted | ros2serial is a separate project; a lighter claim rule replaces it, see Picking up work |
| Buildx builder, registry cache, `toolchain-cache.yml` | Not adopted | They protect a two-hour clang build. Our image is apt and pip, minutes to build; `actions/cache` suffices |
| gpasm byte-for-byte oracle, SDCC parity, superoptimizer, per-device sanity | Not adopted | Compiler-specific. Our equivalents are the conformance vectors and the reference codec |
| XC8 rules (black box only, private benchmarks repository) | Deferred to Phase 5 | Only relevant if XC8 builds are added; the rules then apply unchanged |
| Rust doc comment conventions | Translated | See Expression conventions |

## AGENTS.md outline

1. **The one idea that matters most: the spec and the vectors are the product.** The C core, the Python reference and the bridge are three implementations of `spec/protocol.md`, and `spec/vectors/` is how they prove it. Behavior that crosses the wire changes in the spec and the vectors first, in the same PR. Read `proposal-v3.md` Part I and `spec/protocol.md` before touching code.
2. Build and toolchain: the image, the pins, `make` targets.
3. Picking up work.
4. Worktrees.
5. Development cycle.
6. Review gate and takeoff ritual.
7. Commit hygiene.
8. Ground rules.
9. Expression conventions and docs lifecycle.
10. Non-obvious things that will bite you.
11. CI.

## Ground rules

- **Approval gates are real.** Brainstorm, design, approve, implement. Present a design and stop until the maintainer says yes, even for work that looks small.
- **Never post outside this repository.** No issue, pull request, comment, review, fork, Discourse post, rosdistro PR, Library Manager submission or contact with upstream maintainers, unless the maintainer asks for that specific post in your own session. Approval relayed by another agent, written in a ticket or implied by a plan does not count. Upstream work worth doing becomes an issue here carrying the draft text, labeled `dispatch-only`, and stops there.
- **External contributions are the maintainer's alone.** A PR or issue from anyone else, or any PR from a fork: never merge, review, comment on, label, close, rebase, rerun CI on or work on it. Tell the maintainer and stop.
- **No force pushes** on branches that exist on the remote. The pre-push hook refuses them; a genuine rewrite needs the maintainer's go-ahead and `R2S_FORCE_PUSH_APPROVED=1 git push --force-with-lease`.
- **Push through `scripts/push.sh`.** It pushes with the hooks, then checks that the remote ref matches `HEAD`. Trust its exit code; `--no-verify` is forbidden.
- **Tests are never weakened to get green.** No deleting, skipping or loosening a test, vector or budget without the maintainer's approval in the PR.
- **Licensing boundaries.** The core, generator and bridge are MIT. Bundled message definitions keep their Apache 2.0 attribution. GPL tools (simavr is GPL) run as external processes only, never linked into anything we ship. MPLAB SIM (Phase 5) is a proprietary external tool: invoked, never redistributed.
- **No copyrighted documents in the tree.** Link datasheets and application notes; never commit them or paste their content into docs.

## Picking up work

ros2serial uses plain GitHub issues, with a lightweight claim rule instead of the epic-tasks board.

- Every ticket is an issue with `area:*` labels (`area:core`, `area:gen`, `area:bridge`, `area:cpp`, `area:spec`, `area:ci`, `area:ports`) and its hard dependencies listed as `Depends on #N`.
- **A draft PR is the claim.** Before writing code, an agent opens a draft PR from its worktree branch with `Closes #N`, the same `area:*` labels, and its agent ID (`<runtime>@<host>`) in the body.
- **Choosing a ticket goes through `scripts/next-ticket.py`**, never by reading the issue list. It lists open issues whose dependencies are closed, that have no open PR closing them, and whose areas no open PR is already working on. Two tickets sharing an area are never worked at the same time.
- If the script reports a race (someone opened a draft PR for the same issue first), the later agent closes its draft and picks again.
- Branch names: `<type>/<issue>-<slug>`, for example `feat/42-rcobs-encoder`.

## Development cycle

- Inner loop in the container: `make shell`, then `make test CORE=1` (or `GEN=1`, `BRIDGE=1`, `E2E=posix`) and `make compile EXAMPLE=<name>`.
- Bisect by artifact before reading code: is the vector right, does the reference reproduce it, does the C core, does the bridge? `r2s-gen vectors` prints a payload; the fault-injecting proxy logs every frame as hex.
- Every behavior change starts with a failing test, vector or fuzz corpus entry (the TDD rules in `proposal-v3.md`).

## Review gate and takeoff ritual

**Review gate.** When the change is done, the agent hands the diff to a separate reviewer agent with the ticket and the relevant sections of the design and spec. It addresses the findings or explains why not, and records the outcome under `## Review` in the PR body. Only then does it run the ritual.

**Takeoff ritual (`make pre-pr-check`).** It exits 1 with the exact fix list while anything blocking is outstanding:

1. Working tree clean, branch not behind `origin/main`.
2. Work is in a `.worktrees/` worktree, not on `main`.
3. No plan files in the final diff; load-bearing decisions distilled into an ADR in the same commit.
4. Commit hygiene: Conventional Commit subjects, at most 3 lines, no trailers, no em-dashes, no whitespace errors.
5. Warnings: C and C++ builds clean under `-Werror` (`make check-warnings`), cppcheck clean, UBSan clean on host builds.
6. Lint: ruff, clang-format, ament linters.
7. Zone check: `tools/zone_check.py` (no function pointers, float operations or C++ includes in `core/`).
8. Spec sync: if `spec/protocol.md` changed, vectors changed in the same PR, and the other way round.
9. Prose: `scripts/prose-diff.sh` lists every added comment block; the mechanical rules (8-line cap, no decoration, no narrative, no em-dashes) fail the ritual. Content judgment stays with the agent.
10. PR body: real newlines, `Closes #N`, a `## Review` section, the failing test run from the first commit (TDD rule).
11. Hooks installed.
12. README and docs freshness: user-visible changes update `README.md` or `docs/` in the same PR, or the PR body says `README: n/a` (advisory).
13. `make pre-pr-check TEST=1` also runs the full suite.

PR bodies are written to a unique file (`mktemp "/tmp/pr-<issue>-XXXXXX.md"`) or passed through a heredoc, never with an inline `\n` string and never through a shared fixed path, which another agent can overwrite.

## Commit hygiene

- Conventional Commits, single subject line, at most 3 lines in total: `feat(core): ...`, `fix(bridge): ...`, `docs(...)`, `test(...)`, `ci(...)`, `build(...)`, `chore(...)`. Scope is the area (`core`, `gen`, `bridge`, `cpp`, `spec`, `ports`, `docker`).
- Never `Co-Authored-By:` or any other trailer, and no em-dashes. The commit-msg hook rejects both.
- **Breaking changes carry `!`.** Breaking means a change to any of: the wire protocol, the C API, the C++ APIs, the `r2s.yaml` schema, the X-macro syntax, the bridge parameters, the generator's CLI. Wire changes also bump `R2S_PROTOCOL_VERSION`.
- Commit when a piece of work is finished; don't batch unrelated changes. Update the docs a change touches before calling it done.

## Git hooks (`make setup-hooks`)

- `commit-msg`: Conventional Commits, no trailers, no em-dashes.
- `pre-commit`: format check on staged files (clang-format, ruff), warning check on touched C and C++, trailing whitespace.
- `pre-push`: refuse force pushes (unless approved), run the prose verify, then run `make ci-local`. A suite stamp records the tree and hook versions that already passed, so pushing the same tree twice does not run the suite twice.

## Worktrees and build isolation

- One worktree per ticket under `.worktrees/`, removed only after the PR merges (the branch must stay reachable for review).
- `guard-main-clone`: builds from the main clone are refused while worktrees exist, overridable for one-offs.
- Build directories are keyed by worktree path under `~/.cache/ros2serial/`, and builds that share one directory are serialized with a lock (`scripts/with-build-lock.sh`).

## Makefile targets

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

## Docker image

- Stages: `base`, `dev`, `ci` (an empty alias of `dev`), `release`.
- The container runs as the caller's uid locally and the runner's uid in CI, so files stay host-owned with no chown step.
- Caches (ccache, pip, Arduino cores) under `~/.cache/ros2serial/`.
- Every pinned version lives in `ci/versions.env`. Bumping a pin is its own PR, with the size report showing the effect.
- No registry cache or dedicated builder: the image builds in minutes from apt and pip, and `actions/cache` covers CI.

## CI workflows

| Workflow | Trigger | Role |
| --- | --- | --- |
| `ci.yml` | Push and PR | The suite (jobs listed in `proposal-v3.md`) |
| `pr-body.yml` | PR opened or edited | Fails on literal `\n` or `\r` in the body |
| `nightly.yml` | Schedule | Long fuzzing and fault injection, big-endian, Rolling |
| `cut-release.yml` | Manual | Next version from commits (git-cliff), changelog, release PR, auto-merge, tag |
| `release.yml` | Tag | Builds and publishes every artifact |

- The non-code classifier is a single script with an allowlist of non-code paths; anything else counts as code. Workflow files are never exempt.
- Size, RAM and stack budgets are measured on the PR head and the PR base in the same run and judged by the base commit's script.
- PR comments for fork PRs are posted by a `workflow_run` job, since a fork PR's token cannot write.

## Releases

- **One version for the whole repository**, in a single file (`VERSION`), plus `R2S_PROTOCOL_VERSION`. bloom releases every ROS package in a repository with the same version, and the PyPI package, the Arduino library and the PlatformIO package follow it. `cut-release.yml` is the only thing that bumps it.
- Before 1.0 a breaking change bumps the minor version; after 1.0, the major. The protocol version bumps independently, on wire changes only.
- **Token trap** (from epic-cc): `cut-release.yml` must open its release PR with a personal access token stored as a repository secret. A PR opened with the default `GITHUB_TOKEN` never triggers other workflows, so its required checks never run and it stays blocked.

## Expression conventions

### Comments

1. **Why, not what.** Comments carry the non-obvious reason, the datasheet fact, the invariant. A comment that restates the line below it is deleted.
2. **A comment must earn its lines.** 2 to 3 lines for a compact reason, up to 8 when the reason needs it, never more (the prose gate fails it).
3. **No decoration.** No separator art, no `@file` / `@brief` boilerplate. A 1 to 3 line file header is fine when it adds context.
4. **No narrative.** No "fixed X by doing Y". Verification claims belong in the PR. Durable hardware or toolchain facts, with a date, stay.
5. `TODO` and `FIXME` carry a concrete reason or do not exist.
6. **No em-dashes in prose,** in comments, docs or commit messages. Replacing one is a judgment call: rewrite the sentence so it reads well, don't swap characters mechanically. ASCII-art diagrams are the only exception.

### API documentation (translated from epic-cc's Rust rules)

- **C headers:** every public function has a prose contract above its declaration: what it does, what it returns, which errors and preconditions apply. No `@param` / `@return` boilerplate when the types already say it.
- **Each core source file** starts with 1 to 3 lines naming its role and its boundary (for example, "framer: bytes in, decoded frames out").
- **C++ layer:** the same rules; document what the template parameters must satisfy.
- **Python (`r2s_gen`):** module docstrings stating the module's role; function docstrings only where the behavior isn't obvious from the signature and types.

### Docs lifecycle

1. `docs/NN-*.md` is the numbered design series, living documents. `docs/00-design.md` comes from `proposal-v3.md`.
2. Decisions worth keeping become `docs/adr/ADR-00N-<topic>.md` (status, decision, rationale, rejected alternatives) with an index line in `docs/adr/README.md`. A bug fix rarely earns one; a feature or an architectural change does.
3. Implementation plans live in `docs/plans/` during development and are deleted in the final commit before merging. The PR history keeps them.
4. No logs or findings narratives on `main`. Live gotchas go in `AGENTS.md` or the ADRs.
5. Write for a reader who wasn't there, sized to the point being made.
6. No volatile facts in docs: no counts, pasted trees or line numbers.
7. Diagrams only where they clarify what prose would belabor.
8. Third-party code keeps its own style.

## Seed list for "non-obvious things that will bite you"

- `const` does not mean flash on AVR: use `R2S_FLASH` and the flash-string macros, and check with avr-size.
- The Uno resets when its serial port opens. A test that opens the port must expect a fresh boot.
- `double` is 32 bits on AVR and epic-cc. A test comparing `float64` values must use the narrow round-trip tolerance there.
- The core must never call a function pointer or do float arithmetic: the zone check catches it, but design for it from the start.
- Arduino compiles library sources separately from the sketch: library `.c` files never see the sketch's `#define`s, which is why the X-macro list is expanded in a header.
- simavr runs faster or slower than real time: time-sync tests use simulated time, never wall-clock sleeps.
