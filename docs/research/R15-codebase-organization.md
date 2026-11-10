# R15: codebase organization review

Nov 10, 2026 · research for issue #562. R15 is the next free
number in the `docs/00-design.md` table; the table owns R-numbers,
so this proposal needs a table row there if the maintainer adopts it.

**Sources.** The tree on `origin/master` at `858d545` (Nov 10,
2026); `docs/00-design.md` Part II Repository layout; `AGENTS.md`;
`CONTRIBUTING.md`; `scripts/check_layout.py`;
`scripts/release-files.list`;
`scripts/build-release-artifacts.sh`; `gen/setup.py`;
`bridge/r2s_bridge/CMakeLists.txt`; `Taskfile.yml`; git history
(`git log`, per-directory counts); the reference layouts read from
`ros2/rclcpp`, `ros2/rcl`, `ros2/rclpy` and `libuv/libuv` on GitHub
(top-level listings only, read for structure, nothing copied). No
repo code was changed for this review; every count below comes from
`git ls-tree -r --name-only origin/master` and commands quoted in
Method. Counts drift as the tree moves; the shapes are the point.

**Method.** File counts per top-level directory, then per
subdirectory for the four large ones (`core`, `gen`, `spec`,
`scripts`). Churn as commit counts touching each directory over the
last 90 days (the whole repo history fits in that window) plus the
file stats of the reformat, header and doc-comment commits.
Coupling as `grep` over `origin/master` for `../` includes,
`core/` and `cpp/` path references outside their trees, and Python
imports across directories. Release contents from the two build
scripts and `setup.py`, not from a built artifact. Open-branch
conflict exposure as the worktree count under `.worktrees/`.

## Summary

Do not reorganize now. The tree matches its specified layout, every
cross-directory reference found is load-bearing and tested, and the
two real deviations found are small enough to schedule, not urgent
enough to pay for during Phase 2 with 19 open worktrees. The
concrete proposal: land this note, sync the layout diagram in the
design (it omits four paths that exist), and defer the at most two
micro-moves below to a quiet window at the end of Phase 2. No
follow-up move is needed for the Arduino zip, the sdist, the ROS
package layout, or any gate.

## Criteria and weights

1. Release fitness (hard constraint): nothing may break the ROS
   package layout colcon and the build farm see, the Arduino zip
   staging, or the sdist contents. Weight: veto.
2. Zone integrity (hard constraint): `core/` stays MCU-pure under
   `tools/zone_check.py`. Weight: veto.
3. Local equals CI: every path a move touches must update its gate,
   script, workflow and test in the same PR. Weight: high; it sets
   the cost side of every move.
4. Conflict cost: a move conflicts with every open branch that
   touches those paths. 19 worktrees are open now. Weight: high,
   and time-varying, which is why timing is part of the verdict.
5. Discoverability for a newcomer (README, CONTRIBUTING, docs,
   directory names that mean what they hold). Weight: medium.
6. Coupling between directories (includes and imports crossing a
   top-level boundary). Weight: medium; coupling that is explicit
   and tested is fine.
7. Scriptability: a recommended move must be `git mv` plus a
   mechanical path fix, with the full list of files that pin the
   old path. Weight: required for any move proposed.

## Measurements

1360 tracked files on `origin/master`. Per top-level directory:

- `core/` 622, of which 515 are `core/fuzz/corpus` seeds, 63 are
  `core/tests`, 17 headers under `core/include/r2s`, 16 sources
  under `core/src`, 3 shims under `core/host`. Real code is about
  36 files; the fuzzer corpus is 83 percent of the directory by
  count.
- `gen/` 150: `r2s_gen` (19 modules plus `reference/`, `schema/`,
  `snapshots/` with 51 `.msg` files across jazzy, lyrical and
  rolling), `r2s_rosidl`, `tests/` (34 files, 32 of them
  `test_*.py`), `tests/golden` (22 files).
- `spec/` 134: 126 vector files, 7 files under `spec/schema`, plus
  `spec/protocol.md`.
- `scripts/` 121: 65 implementation and helper files, 56
  `test_*.py` files beside them.
- `bridge/` 69: two ROS packages with the standard
  `src`/`include`/`test` shape plus `launch/` and `config/`.
- `tests/` 59: e2e scenarios (posix and avr variants sharing
  `e2e_support/`) plus `tests/harness/simavr` (13 files).
- `tools/` 38: 18 implementation files, 16 `test_*.py` beside
  them, plus `vendor/`.
- `ports/` 28: `posix/` (a full virtual-MCU firmware) and
  `arduino/` (glue plus host tests) are real; `avr`, `ch32v`,
  `stm32` and `epic-hal` hold only `.gitkeep` placeholders.
- `examples/` 28: ten single-sketch `arduino_*` directories,
  `cmake_minimal`, and `examples/tests`.
- `docs/` 7, `benchmark/` 18, `.github` 12, `.githooks` 3,
  `docker/` 2, `ci/` 1 (`versions.env`), `mutation/` 1.
- Four `package.xml` holders: `gen`, `gen/r2s_rosidl`,
  `bridge/r2s_bridge`, `bridge/r2s_bridge_tests`. `core/`, `cpp/`,
  `ports/` and `examples/` are not ROS packages.

Churn: the repo is five weeks old and all 359 commits fit in the
last 90 days, so per-directory counts measure construction order,
not hotspots (`scripts` 177, `docs` 122, `tools` 94, `core` 70,
`gen` 66, `.github` 64, `tests` 54, `bridge` 49, `ports` 42).
The recent reformat, header and doc-comment wave touched 485
distinct files in two days (union of the named commits'
file lists): the `#510` core/ports/remaining reformats (79, 12
and 38 files), the `#527` bridge reformat (54), the `#552` SPDX
header sweep (290), the `#546`/`#553`/`#558` doc-comment
conversions (17, 17 and 26), and the follow-up tree reformat (369
files in one commit).

Coupling: no `#include` with `../` exists anywhere in `bridge`,
`cpp`, `ports`, `examples`, `tests` or `benchmark`, and nothing
under `core/` includes from `cpp/`. Cross-directory references are
relative build paths, each with a consumer: the bridge compiles
three core sources plus `core/include` (named in
`bridge/r2s_bridge/CMakeLists.txt`); `examples/tests/run.sh`
stages `core/include` and `core/src` plus the arduino port and its
stubs; `scripts/build-*.sh` stage the same trees for the example
matrix, the PIC canary and the release artifacts. Python coupling
is the importable `r2s_gen` package (`pythonpath = ["gen"]` in the
root `pyproject.toml`) plus `gen/tests/session_harness.py`
building a host library out of `core/` sources.

Release contents: the Arduino zip is staged at release time from
the generator bundle plus `core/include`, `core/src` (minus the
test-reset translation unit), the `cpp` runtime pair and the
arduino port glue, so the zip's `src/` layout is decoupled from
the tree layout by `scripts/build-release-artifacts.sh`. The sdist
builds from a throwaway copy of `gen/` with `cpp/` copied inside
as `cpp_stage`, because setuptools cannot stage a package from
outside the project directory; `gen/setup.py` documents this with
a comment. `scripts/release-files.list` pins about 50
version-stamped paths across `gen`, `bridge`, `examples`,
`ports/posix` and `cpp`.

Reference layouts: an `rclcpp`/`rcl` package is one directory with
`include/<pkg>`, `src`, `test`, `package.xml` and
`CMakeLists.txt`, and their repos hold sibling package
directories, never nested ones (`rclcpp`, `rclcpp_action`,
`rclcpp_lifecycle`; `rclpy` adds the same-named Python package
beside `src`). `libuv` is one `include/`, one `src/` with per-OS
subdirectories (`src/unix`, `src/win`), one `test/`, plus `docs/`
and `tools/`.

Conflict exposure: 19 worktrees are open under `.worktrees/`,
so a tree-wide move would conflict-merge against up to 18 other
agents' branches.

## Problems found, each tied to a measurement or a pain

F1. `gen/r2s_rosidl` nests a ROS package inside another ROS
    package's directory. The reference repos keep sibling package
    directories; here `gen/` is an ament_python package and
    `gen/r2s_rosidl` is a second package inside it. No failure
    today (CI builds both, `test_rosidl_plugin.py` covers the
    plugin), so this is latent: nested-package discovery and the
    sdist staging, which copies all of `gen/` including the
    rosidl CMake tree into a Python source distribution. Small,
    mechanical, optional.
F2. `core/host/` keeps host-only test support inside the
    MCU-purity tree. `host_shim.c` exists for the Python driver
    (`scripts/build-host-lib.sh` is its only consumer); it is not
    MCU code and `core/` is the one directory with a purity gate.
    The gate does not inspect it today, so nothing fails, but a
    newcomer reading `core/` meets host test scaffolding next to
    firmware sources. Three files, one consumer, mechanical.
F3. The layout diagram in the design omits paths that exist:
    `spec/schema/`, `core/host/`, `gen/r2s_rosidl`,
    `benchmark/`, `docker/`, `mutation/` and `ci/`. The diagram is
    the first thing a newcomer compares against the tree, and it
    currently fails that comparison in seven places. Doc-only fix.
F4. `cpp/` beside `gen/` forces the `cpp_stage` copy in
    `gen/setup.py` and pins `cpp/` paths in about 15 files
    (Taskfile, `bundle.py`, release scripts, `test_merge_friendly`,
    zone and spec tooling). This looks like the strongest move
    candidate until the consumers are listed: the Arduino zip, the
    cmake examples, the release file list and the generator all
    consume `cpp/` as an independent sibling deliverable, and
    nesting the MCU C++ layer inside a Python package tree would
    trade one documented staging copy for colcon and bloom
    confusion. Rated below as leave-alone with the reason.

## Proposed layout

The target layout is the current layout, plus the diagram sync
from F3 and, in a quiet window, at most the two micro-moves from
F1 and F2:

- `gen/r2s_rosidl` becomes a top-level sibling (name owned by the
  migration ticket; `rosidl/` matches the sibling-package
  convention). Same contents, same package name, only the
  directory moves.
- `core/host/` becomes `core/tests/host/`, next to the other
  host-side test support (`core/tests/support/`,
  `core/tests/vendor/`). Same contents, only the directory moves.
- Everything else stays where it is, for the reasons under
  Deliberately left alone.

## Migration plan, in order, with the cost of each step

Step 0, now, in this ticket's PR: land this note and post the
Findings comment. Cost: one new file, no moves, no conflicts.
Step 1, with any follow-up docs pass, not this ticket: sync the
layout diagram (F3) and add the R15 row to the research table.
Cost: two small doc edits in `docs/00-design.md`; conflicts only
with docs edits on the same sections.
Step 2, deferred to a quiet window at the end of Phase 2, only if
the maintainer wants it: move `core/host/` to
`core/tests/host/`. Cost: `git mv` of 3 files plus one path fix
in `scripts/build-host-lib.sh` and any Taskfile or test reference
found by grep at migration time; verify with the core scope and
the zone check. Do not take this step while more than a couple of
worktrees are open.
Step 3, same window, same condition, after step 2:
`gen/r2s_rosidl` to the top level. Cost: `git mv` of 7 files plus
mechanical fixes in `scripts/release-files.list`,
`scripts/release.sh`, the Taskfile rosidl legs, rosdoc2 configs,
`test_rosidl_plugin.py` and the farm/colcon discovery paths;
verify with the gen scope, the packaging tests and a colcon
build. This is the largest move proposed and still fits in one
small PR.
Trigger for steps 2 and 3: end of Phase 2, few open worktrees,
and no open PR touching `core/`, `gen/` or the release scripts.
If that window never comes, steps 2 and 3 stay shelved: both
findings are stable as documented deviations, not rot.

## Risks

- Any move taken now conflicts with up to 18 open branches and
  re-touches files the `#510`/`#527`/`#552` wave just
  rewrote (485 files in two days, per the union above). `git mv` preserves history,
  but every open branch touching those paths pays a merge, and
  the same-PR gate updates span about 30 files for a large move.
  This is why the plan defers.
- Nested-package discovery (F1) and the `cpp_stage` copy (F4)
  both work today because CI builds them; a move that "fixes"
  either without running the packaging and farm legs could trade
  a latent smell for a real release failure. Both deferred steps
  name their verifying scopes.
- The counts above go stale on the next merge; they are dated to
  `origin/master 858d545` and only the shapes they support (corpus
  dominance, test colocation, placeholder ports) carry forward.

## Deliberately left alone

- `core/fuzz/corpus` (515 files): seed corpora live next to
  their targets by libFuzzer convention; the count looks alarming
  and means nothing.
- `cpp/` as a sibling of `gen/`: independent deliverable with
  non-Python consumers; the staging copy is documented and
  guarded by `test_merge_friendly.py`.
- `scripts/` and `tools/` keeping `test_*.py` beside
  implementation: the design specifies exactly this, and the
  per-directory suites rely on it.
- Placeholder port directories (`avr`, `ch32v`, `stm32`,
  `epic-hal`): required by `check_layout.py` and named by the
  design as Phase 3 and 5 landing slots; removing them would fail
  the gate and hide the roadmap.
- Two checked-in generated copies (`ports/posix/src`,
  `examples/cmake_minimal/lib`): different configs, different
  bytes, both refreshed by `make regen` and pinned in
  `release-files.list`; deduplicating them would couple two
  consumers' configs.
- `bridge/r2s_bridge_tests` as a separate package: standard ROS 2
  launch_testing shape, matches upstream convention.
- `tests/e2e` central with per-directory unit tests beside code:
  matches the design's test strategy, unlike `libuv`'s single
  `test/`; the e2e matrix needs one shared scenario tree.
- Root files (`Makefile`, `Taskfile.yml`, `pyproject.toml`,
  `conftest.py`, `Dockerfile`, `mull.yml`, `cliff.toml`): each is
  the documented entry point or repo-wide config for its tool;
  none is staging for a move.
- `spec/vectors` sharding by topic: 126 files across seven
  directories is the bisect-by-artifact strategy working, not
  fragmentation.

## Follow-up tickets the migration would need

1. `docs`: sync the Part II layout diagram and add the R15
   research-table row (step 1; no moves).
2. `chore(core)`: move `core/host/` under `core/tests/`
   (step 2; deferred to the quiet window).
3. `chore(gen)`: lift `gen/r2s_rosidl` to a top-level sibling
   (step 3; deferred to the quiet window).

No recommendation and no API proposal beyond this layout call;
those stay with the maintainer.
