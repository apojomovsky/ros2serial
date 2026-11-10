# Quality Declaration

This document declares the software quality of the ros2serial packages,
following [REP-2004](https://reps.openrobotics.org/rep-2004/)
(Package Quality Categories).

The packages `r2s_bridge`, `r2s_bridge_tests`, `r2s_gen` and `r2s_rosidl`
claim to be in the **Quality Level 4** category. Level 4 covers demos,
tutorials and experiments without strict requirements, while still
expecting good documentation and tests. That matches this repository:
pre-1.0, Linux-focused, with an extensive test suite and documented public
APIs, but without a stability promise or tier-1 platform coverage yet.

Below are the rationales, notes, and caveats for this claim, organized by
each requirement listed in the
[Package Requirements for Quality Level 4 in REP-2004](https://reps.openrobotics.org/rep-2004/#quality-level-4).

## Version Policy [1]

REP-2004 sets no version requirements for Level 4, while still
recommending a policy such as semver, even before the version is stable.

The repository carries one version for all packages in `VERSION`,
currently `0.3.0`, and follows semver: breaking changes carry `!` in the
commit subject, as `AGENTS.md` requires. There is no API or ABI stability
promise before `1.0`.

## Change Control Process [2]

REP-2004 sets no change control requirement for Level 4, while still
recommending one.

Every change lands through a pull request from a `.worktrees/` branch,
one ticket per pull request, squash-merged onto `master` only on green CI,
as `AGENTS.md` requires. The maintainer reviews each pull request before
the takeoff ritual runs.

## Documentation [3]

Level 4 requires a declared license and stated copyrights within the
project, with attribution of all authors.

The repository is MIT licensed (`LICENSE`). Every hand-written source file
carries the MIT header with the copyright line, checked by
`scripts/check_headers.py`. Public APIs carry Doxygen contracts: the C
core is fully converted and gated by `scripts/doxygen_check.py`, and each
ROS package ships a `rosdoc2.yaml` with a `doc/` landing page referenced
from its `package.xml` under `<export>`, so the API docs build on
docs.ros.org once the packages release.

## Testing [4]

REP-2004 sets no testing requirements for Level 4, while still
recommending covering some if not all features with tests.

The test strategy in `docs/01-testing.md` is normative: unit, contract
and conformance vector tests per component, coverage floors, end-to-end
scenarios on simavr hardware models, and nightly mutation testing as the
measure of test quality.

## Dependencies [5]

REP-2004 sets no dependency restrictions for Level 4.

Runtime ROS dependencies are released ROS packages (`rclcpp`,
`rosidl_runtime_cpp` and the message packages the bridge loads). The
toolchain versions the build measures against live in `ci/versions.env`.

## Platform Support [6]

Level 4 expects support for all target platforms of the package
ecosystem, which for ROS 2 means the tier-1 platforms of REP-2000. This
is the one requirement the repository does not meet yet, so it is the
exception to the Level 4 claim above: host code builds and tests on
Linux, and the firmware targets AVR MCUs through avr-gcc, which are not
REP-2000 tier-1 platforms. Windows and macOS hosts are not exercised.
The claim stays at Level 4, with this exception, until that changes.

## Security [7]

REP-2004 sets no security requirements for Level 4.

There is no separate vulnerability disclosure policy yet; reporters use
the GitHub issue tracker until the release pipeline lands one.
