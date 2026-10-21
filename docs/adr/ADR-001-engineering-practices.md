# ADR-001: engineering practices adopted from epic-cc

**Status:** Accepted 2026-10-20

## Decision

ros2serial reuses the engineering practices of epic-cc, the maintainer's PIC compiler, where the problem is the same, adapts them where ros2serial differs, and leaves out what only a compiler needs. The rules themselves live in `AGENTS.md`; this record keeps why each one is there.

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
| Docs lifecycle (design series, ADRs, ephemeral plans, no logs) | Adopted | Keeps `master` readable for someone who wasn't there |
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

## Rationale

The practices worked in epic-cc for the same setting: one maintainer, many agents working in parallel, several toolchains, and a project meant to be defended in a design review. Reusing them avoids rediscovering the same failure modes (agents corrupting each other's builds, plan files on the main branch, broken PR bodies, prose drift, budgets relaxed by the PR they judge).

## Rejected alternatives

- **Joining the epic-tasks board.** ros2serial is a separate project; a draft-PR claim rule and `scripts/next_ticket.py` give the same protection against two agents taking one ticket or one area.
- **Copying epic-cc's tooling wholesale.** Its registry cache, dedicated builder and compiler oracles protect problems ros2serial does not have, and would cost maintenance for nothing.

## Revisit if

ros2serial grows several repositories or regular outside contributors, at which point a shared board or a heavier claim tool may pay for itself.
