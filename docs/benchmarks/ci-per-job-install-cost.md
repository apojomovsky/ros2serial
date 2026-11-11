# Per-job package install cost (CI)

Question: before baking job dependencies into derived images (issue 598),
measure what the runtime installs actually cost: wall time per job, share
of the job, and how often apt fails there. All figures are observed step
durations from the GitHub API, not estimates. Each run below is named by
its run id so the numbers can be checked.

Scope: the three jobs that install packages at runtime. The
`rosdoc2` job installs `doxygen` and `python3-pip` through apt plus
`rosdoc2` through pip. The `e2e-posix` legs install the two non-default
RMWs (`ros-<distro>-rmw-cyclonedds-cpp`, `ros-<distro>-rmw-zenoh-cpp`).
The `farm` legs install package dependencies through rosdep. The dev
image build is included as the restore baseline every other job pays.

Method: per-step `started_at` and `completed_at` from
`gh api repos/apojomovsky/ros2serial/actions/jobs/<id>`, with failure
causes from the step logs. Sample window is 2026-11-10 to 2026-11-11,
which covers one quiet period and one Ubuntu mirror outage (issue 599),
so both the success cost and the flake cost are visible.

## Dev image: cache hit against cache miss

Step `Run ./.github/actions/dev-image` in `ci.yml` jobs. The image tag
is content addressed (`scripts/dev-image-tag.sh`), so a Dockerfile or
`ci/versions.env` change starts cold.

| run | job | image step | job total | layers |
| --- | --- | --- | --- | --- |
| 42926324058 (master, new tag after #579) | core | 470s | 547s | heavy stages rebuilt (#14 129.9s, #16 227.9s) |
| 42926324058 | docs | 289s | 306s | heavy stages rebuilt |
| 42926324058 | e2e-isolation | 334s | 2070s | heavy stages rebuilt |
| 43035059491 (PR branch, tag unchanged) | core | 149s | 214s | early layers CACHED |
| 43035059491 | docs | 117s | 126s | early layers CACHED |
| 43035059491 | e2e-isolation | 90s | 1772s | early layers CACHED |
| 43035059491 | lint | 87s | 124s | early layers CACHED |
| 43035059491 | fuzz-smoke | 75s | 331s | early layers CACHED |

A cold tag costs roughly 300 to 470s of image build; a warm cache
costs roughly 75 to 150s. One failure mode seen in the window: run
43042714686 failed the image step after 91 to 104s with `cache write
denied: token has no writable scopes` at the cache export, after the
layers themselves built. That failure is about the token scope, not apt.

Restoring the ROS distro images (`Initialize containers`, digest pinned
in `ci/versions.env`) costs 16 to 25s on every container job observed,
with no hit or miss split: each job lands on a fresh runner and pulls
by digest.

## rosdoc2: install against a 57s job

Container: `ros:jazzy` image. Step `Install doc tools`
(`.github/workflows/rosdoc2.yml`):

```console
apt-get update
apt-get install -y --no-install-recommends doxygen python3-pip
pip install --break-system-packages "rosdoc2==$(grep ^ROSDOC2_VERSION= ci/versions.env | cut -d= -f2)"
```

| run | install step | job total | install share | outcome |
| --- | --- | --- | --- | --- |
| 43031770681 (job 130560418821) | 15s | 57s | 26% | success, doc build 8s |
| 43042647636 (job 130598281676) | 101s | 125s | fail | apt mirror unresolvable, single attempt |
| 43045050881 (job 130606736853) | 304s | 333s | fail | apt mirror unresolvable, retried attempts |

Over the last 10 `rosdoc2` runs: 9 job failures, 1 success. Every
failure shows `Could not resolve 'archive.ubuntu.com'` and
`Could not resolve 'security.ubuntu.com'`, ending in `Package 'doxygen'
has no installation candidate`. The workflow is `continue-on-error`, so
the run stays green while the job fails. The 304s case runs on the
#597 retries branch: retries turn a 101s failure into a 304s failure
when the mirror itself is down.

## e2e-posix: install against a 440 to 488s leg

Container: per-distro ROS images. Unnamed apt step (`.github/workflows/ci.yml`):

```console
apt-get update
apt-get install -y --no-install-recommends "ros-${DISTRO}-rmw-cyclonedds-cpp" "ros-${DISTRO}-rmw-zenoh-cpp"
```

| run | leg | install step | test step | leg total | install share |
| --- | --- | --- | --- | --- | --- |
| 43031770666 | jazzy, fastrtps | 8s | 437s | 470s | 2% |
| 43031770666 | jazzy, cyclonedds | 7s | 452s | 482s | 1% |
| 43031770666 | jazzy, zenoh | 5s | 411s | 440s | 1% |
| 43031770666 | lyrical, cyclonedds | 14s | 448s | 488s | 3% |
| 43031770666 | lyrical, zenoh | 8s | 444s | 485s | 2% |
| 43035059491 | jazzy, fastrtps | 92s | skipped | 120s | fail |

Over 6 recent `ci` runs (54 legs): 45 leg failures, 9 successes. All
failures are the apt mirror outage at the install step; the test step
never runs. The install is 5 to 14s of a roughly 450s leg when apt
works: about 2%.

## farm: setup against inherent install time

Container: per-distro ROS images. Step `Install dependencies through
rosdep` (`.github/workflows/farm.yml`):

```console
apt-get update
rosdep update
rosdep install --from-paths gen bridge --ignore-src -r -y --rosdistro "$ROS_DISTRO"
```

| run | leg | install step | build, test, smoke | leg total | install share |
| --- | --- | --- | --- | --- | --- |
| 43031770668 | lyrical | 20s | 81s | 128s | 16% |
| 43031770668 | jazzy | 26s | 98s | 147s | 18% |
| 43031770668 | rolling | 21s | 85s | 136s | 15% |
| 43045051035 | jazzy | 107s | skipped | 135s | fail |
| 43045051035 | rolling | 107s | skipped | 132s | fail |
| 43045051035 | lyrical | 107s | skipped | 131s | fail |

Over the last 10 `farm` runs (30 legs): 27 failures, 3 successes. All
failures are the apt mirror outage inside the rosdep step (`Failed to
detect successful installation` after the mirror errors).

Split of the 26s jazzy success, from the step log: `apt-get update`
about 4s, `rosdep update` about 6s, `rosdep install` about 16s. The
first 10s is pure setup. The remaining 16s installs the actual package
dependencies, which is what the job proves: `farm` checks the
release shape, that the packages resolve from released packages and
rosdep rules with no network fetch at build time. Moving that part
into an image would weaken the proof the job exists to give.

## Recommendation

None of the three clears a time-only bar for a derived image. The
measured success-path savings are 5 to 14s per `e2e-posix` leg (about
2% of the leg), 15s for `rosdoc2` (26% of a 57s advisory job), and 20
to 26s per `farm` leg (about 16% of the leg, half of it setup). A
derived image costs standing maintenance: a registry and a publish
job (the repository has neither; images build from apt and pip with
only the GitHub Actions cache), one more image per distro (three
today: jazzy, lyrical, rolling), new pins in `ci/versions.env` where
every bump is its own PR, content-tag invalidation rules, and a debug
cost (a red job then means inspecting image contents, not workflow
commands). Saving seconds per run does not pay for that.

The flake side changes the shape but not the verdict yet. During the
mirror outage every runtime install failed: 45 of 54 `e2e-posix`
legs, 27 of 30 `farm` legs, 9 of 10 `rosdoc2` jobs. A derived image
with the packages already inside would have stayed green through the
outage, since no runtime apt would run. But baking only moves apt to
image build time; a rebuild during an outage flakes the same way, and
only already-published images are immune. The bounded retries in #597
already cut the non-outage flake rate, and during a full outage
retries only stretch the failure (101s to 304s on `rosdoc2`).

Threshold: a derived image pays for itself when the install it removes
costs more than its maintenance. In concrete terms, when a job's
runtime install durably exceeds about a fifth of the job on the
success path, or when outage-driven red runs outnumber apt-healthy
runs over several weeks despite the retries. Neither holds today:
the largest inherent share is `farm` at 16%, and the outage in this
window is one incident, not the baseline.

First candidate if that threshold is ever met: `rosdoc2`. It is one
image, not a distro matrix (one distro proves the shared doc config);
its install is the largest share of its job (26%); it is advisory
(`continue-on-error`), so a broken derived image blocks no merge;
and its pip install of `rosdoc2` is the least reproducible step of
the three. `farm` is the wrong first candidate despite the largest
absolute saving, because its dependency install is the proof. The
`e2e-posix` install at about 2% never justifies a per-distro image.

No workflow or image changed in this ticket.

## How to recheck

```console
gh run list -R apojomovsky/ros2serial --workflow rosdoc2.yml --limit 10
gh api repos/apojomovsky/ros2serial/actions/runs/<run>/jobs --paginate
gh api repos/apojomovsky/ros2serial/actions/jobs/<job>
gh api repos/apojomovsky/ros2serial/actions/jobs/<job>/logs --allow-escape-sequences
```

Durations are `completed_at` minus `started_at` per step. Failure
causes are the `Err`, `Could not resolve`, and `E:` lines in the
failing step's log.
