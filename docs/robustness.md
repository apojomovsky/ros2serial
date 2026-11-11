# Robustness numbers

How the nightly mutation and fuzz runs turn into published, gated numbers.
The size and stack side lives on the [budgets](budgets.md) page; this page
covers the test-quality side. Everything here runs nightly only: no pull
request check and no release gate runs it, so the numbers never block a
change, only a regression on the schedule.

## Mutation scores

Mull mutates the core C code and runs the core unit binaries; mutmut
mutates the generator and runs the generator suite; Mull also mutates
the bridge's ROS-free classes and runs their harness binaries. Each run
normalizes its tool output and `scripts/mutation_triage.py` scores every
area:

| area | floor | measured by |
| --- | --- | --- |
| core | 85% | Mull over the core unit binaries |
| cpp | 75% | not measured yet, reads no data |
| gen | 80% | mutmut over the generator suite |
| bridge | 70% | Mull over the harness in `mutation/bridge` |

The floors repeat `tools/robustness.yaml`, which is the single source the
triage script reads; `scripts/test_robustness.py` fails when this page or
the script drifts from that file. Only the C++ layer still reads no
data. The bridge harness compiles the scheduler, the time sync, the
framing and the core port slot with their gtest binaries, all without
ROS, so the existing mutation image measures them with no ROS install.
The rclcpp shell around those classes has no harness yet and stays out
of the score; the core framing sources the harness links stay out too,
since the core row owns them.

A measured score below its floor fails the nightly job after the report
is written, and the scheduled failure reporter files the regression.
Unmeasured areas never fail the gate. The report lands as the
`mutation-report` artifact plus the job summary, which is also where the
trend lives: past nightly runs keep their reports, and the summary shows
the current scores against the floors above.

## Equivalent mutants

A surviving mutant is either a missing test or a mutant no test could
kill. The maintainers record the second kind in
`mutation/equivalent.yaml`, one mutant per line with the reason it is
equivalent. Anything missing from that file lands in the report's triage
list. An entry that names no surviving mutant is reported as stale, so
the list cannot rot silently.

## Fuzz coverage

Each libFuzzer target replays its committed seeds under gcov
(`R2S_FUZZ_COVERAGE=1`), and `tools/fuzz_coverage.py` renders which lines
each target's seeds reach:

| target | code its seeds reach |
| --- | --- |
| framer | the framer, rCOBS and the CRC |
| rcobs_decode | the rCOBS decoder |
| session | the session and everything it decodes through |
| codec | the codec interpreter and its helpers |

The exact file list per target lives in `tools/robustness.yaml`.
The coverage report is published as the `fuzz-coverage` artifact plus the
job summary, and it never gates: a low percentage informs, only a broken
measurement fails the run. The seeds themselves come from the conformance
vectors (`tools/fuzz_corpus.py` generates them; `make vectors` refreshes
them), plus hand-written edge cases beside them.

## Regression inputs

A timed fuzz run keeps whatever libFuzzer adds beyond the copied seeds:
crashers, timeouts and new coverage-increasing inputs, under
`R2S_FUZZ_ARTIFACTS_DIR`. The nightly uploads them as the
`fuzz-new-inputs` artifact. A finding becomes a permanent seed by hand:

```sh
cp fuzz-artifacts/framer/crash-deadbeef core/fuzz/corpus/framer/regression_deadbeef
R2S_FUZZ_SECONDS=0 make test FUZZ=1
```

Anything named `regression_*` is kept as committed: the corpus check only
polices the generated `vector_*`, `step_*` and `transcript_*` names, and
the 60-second smoke run on every pull request replays the new seed from
then on.

## How to reproduce

Run the nightly jobs on demand with `workflow_dispatch` on the nightly
workflow, or locally in the dev image. The mutation run needs its own
image (`docker/mutation/Dockerfile`); the fuzz side runs in the dev image
itself:

```sh
R2S_FUZZ_SECONDS=0 R2S_FUZZ_COVERAGE=1 R2S_FUZZ_ARTIFACTS_DIR=fuzz-artifacts bash scripts/fuzz.sh
```

The coverage summary prints per target, and the full report lands in
`fuzz-artifacts/coverage/fuzz-coverage.md`. The bridge harness configures
and runs on its own in either image, which is how its binaries are
checked outside the nightly (the mutation image adds the Mull flags):

```sh
cmake -S mutation/bridge -B /tmp/bridge-harness -G Ninja
cmake --build /tmp/bridge-harness
ctest --test-dir /tmp/bridge-harness
```
