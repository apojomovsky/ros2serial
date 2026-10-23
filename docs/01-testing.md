# ros2serial testing

Oct 20, 2026 · @Alexis Pojomovsky · part of the design series after `docs/00-design.md`

> **What this is.** The testing strategy: the principles, how code must be written so it can be tested, what is tested at which level, when each test runs, how much is enough, and how the tests themselves are kept honest. It is normative: a ticket that adds code follows it, and a reviewer rejects code that breaks it. `docs/00-design.md` Part II summarizes it and links here.

## 1. Principles

1. **Tests are the specification's executable form.** The spec says what must happen on the wire; the vectors and tests prove each implementation does it. A behavior without a test is not guaranteed.
2. **Test behavior, not implementation.** Tests go through a module's public interface and check observable results: bytes out, return codes, counters, callbacks, published ROS messages. Refactoring a module's internals never requires changing its tests.
3. **One source of truth per fact.** Wire behavior lives in `spec/vectors/`, and every implementation reads the same files. No test hard-codes an expected frame that a vector already defines.
4. **Fast, deterministic, hermetic.** A test gives the same result on every run, on every machine, in any order, without network access or wall-clock time. A test that can't meet this moves to a level where it can be isolated (nightly, hardware), never into the PR suite as a flaky test.
5. **Push tests down.** Each fact is tested at the lowest level that can observe it. End-to-end tests prove that the parts are wired together and that the system meets its user-visible goals; they do not re-test framing edge cases.
6. **Every failure is a test.** A bug is fixed by first adding the test, vector or corpus entry that reproduces it.
7. **Measure the tests, not just the code.** Coverage says what ran; mutation testing says what was actually checked. Both are tracked.
8. **Testability is a design constraint.** Code that is hard to test is redesigned, not left untested.

## 2. Designing for testability

These rules shape how the code is written. They are part of the code review, not optional advice.

### 2.1 MCU core (C99)

- **All mutable state lives in one struct per module, as a single static instance.** The public API stays handle-free (one node per firmware), and internal functions access the instance directly, so small targets pay nothing for testability. Host test builds compile with `R2S_TESTING`, which adds `r2s_test_reset()` to zero every instance; every test starts from it. No other code differs between test and target builds.
- **Seams are link-time.** The only boundaries to the outside are the port functions (`r2s_port_*`) and the user handlers (`r2s_on_*`). Tests link a fake port (`core/tests/support/fake_port.c`) instead of a real one. No function pointers are needed, so the zone rules and testability never conflict.
- **The fake port is scriptable.** It queues RX bytes, captures every TX byte, sets the tick, sets `tx_free`, returns a chosen boot ID, and records handler calls. Assertions are made on captured bytes decoded with the same vector helpers the conformance tests use.
- **Time is an input.** The core reads time only through `r2s_port_tick()`. Nothing in the core waits, sleeps or busy-loops. Tests advance the fake clock explicitly, including across the 32-bit wraparound.
- **Algorithms are pure functions.** CRC, rCOBS, varint, the codec interpreter and the time arithmetic take inputs and return outputs, with no hidden state. They are tested directly from vectors and properties.
- **The session is a state machine with explicit events.** Frame received, tick advanced and publish requested are the only inputs; frames sent, state changes and counters are the only outputs. This is what makes model-based testing (section 5.4) possible.
- **Every error path is reachable from a test.** Each `R2S_ERR_*` code and each counter has a way to be triggered through the fake port. Code that cannot be reached that way is dead code and is removed.
- **Compile-time features are tested as builds.** Each `R2S_WITH_*` switch changes what is compiled, so the core unit suite runs on several configurations: minimal (tier S), default (tier M) and full (tier L).
- **The host build is the same code.** The arch macros (`R2S_FLASH`, `R2S_FLASH_READ_BYTE`) have host definitions, and nothing else changes. What only the target can show (flash placement, timing, stack) is tested in simavr.
- **A host shared library for model-based tests.** `libr2s_host` is the core plus the fake port, built with `R2S_TESTING` and exposing the fake port's controls, so Python tests can drive the real C code through `ctypes`.

### 2.2 C++ layer

- Header-only templates are instantiated in test translation units for every message type the tests use, on the host and with avr-g++.
- The runtime registration path keeps its RAM table behind the same two accessors as the static path, so the core's tests cover both backends with the same scenarios.

### 2.3 Generator (Python)

- **Functional core, thin shell.** The pipeline is parse, resolve, model, emit, with every stage a pure function from data to data (emitted files are strings). File and environment access stays in `cli.py`.
- **Errors are values until the edge.** Each stage returns precise error objects (file, key path, message, fix); the CLI formats them. Error messages are tested as data, plus one golden test per message.
- **Deterministic output.** No timestamps, no dictionary-order dependence, no absolute paths in generated files.

### 2.4 Bridge (C++)

- **The ROS node is a shell.** The session state machine, the time estimator, the rate limiter and scheduler, and the packed-to-CDR converter are plain C++ classes with no rclcpp dependency, except the converter's use of type support. `r2s_bridge::Bridge` wires them to rclcpp.
- **Interfaces at the edges:** a `Transport` (real termios, fake in tests) and a `Clock` (steady clock in production, a manual clock in tests). Nothing reads time or the serial port directly.
- **Fakes over mocks.** Tests use the fake transport and the manual clock and assert on what was sent and published. gmock is allowed only where no fake can observe the behavior.

## 3. What to test

| Test | Don't test |
| --- | --- |
| Every normative statement of the spec, through vectors | Private helpers directly (test them through their module) |
| Every public function's contract, including each error code | The compiler, the C library, rclcpp or pyserial themselves |
| Boundaries: empty, one, maximum, maximum plus one, wraparound | Trivial accessors with no logic |
| Every state machine transition and every timeout | Exact log wording, beyond the fields the spec defines (level, logger, topic, field) |
| Every counter, by triggering it | Generated code line by line (golden files and behavior cover it) |
| Invariants as properties (round trips, no zero bytes in rCOBS output, zero CRC residue, monotonic time) | Implementation choices the spec leaves open |
| Resource budgets: flash, RAM, stack, cycles per byte | |
| Every bug ever fixed | |

**Spec traceability.** Every normative statement in `spec/protocol.md` is a list item starting with its ID (for example `[FRM-4]`, `[SES-7]`, `[TIM-2]`). Vectors and tests name the IDs they cover, and `tools/spec_coverage.py` fails CI when an ID has no test or a test names an unknown ID. This turns "is the spec tested" into a checked fact. A test names IDs on a marker line of its own, `covers: FRM-4, FRM-5`, as a comment or in a docstring. A rule specified before its implementation lands is listed in `tools/spec_pending.txt` with the ticket that will cover it. The check fails once a listed rule is covered, rejects tickets that are not in the design, and on a PR accepts new entries only for rules the PR itself adds to the spec, so a rule that was ever required can never be parked there.

## 4. Test levels

From the bottom of the pyramid up. Most tests live at the first three levels; the upper levels have few, carefully chosen tests.

| Level | Scope | Doubles | Examples | Tooling |
| --- | --- | --- | --- | --- |
| **Unit** | One function or module | None, or the fake port | CRC, rCOBS, varint, codec, time math; config validation; estimator math | Unity (C), pytest (Python), gtest (C++) |
| **Component** | One subsystem through its public API | Fake port, fake transport, manual clock | Core session handshake and reset rules; bridge session with a fake transport; generator end to end on example configs | Same |
| **Contract** | Every implementation against the shared vectors | None | C core, Python reference and bridge framing and conversion on `spec/vectors/` | Generated runners per language |
| **Integration** | Real components together, simulated MCU | The posix virtual MCU over a pty | Bridge plus posix MCU: handshake, pub, sub, log, time, remapping, reconnect | launch_testing |
| **System** | Real firmware, simulated or real hardware | simavr; real boards | The key scenarios (section 9) on simavr; the hardware checklist on a Uno and a Mega | simavr harness, HIL runner |

Alongside the levels, these test kinds apply where they fit:

| Kind | Where | Purpose |
| --- | --- | --- |
| Property-based | Codec, rCOBS, varint, CRC, time math, generator | Find inputs nobody thought of; check invariants |
| Model-based | Core session against a Python model of the state machine | Check long random event sequences, including timeouts and resets |
| Differential | C core vs Python reference vs bridge, on random messages | Catch disagreement between implementations |
| Fuzzing | Every byte parser: rCOBS decoder, framer, codec, session input, bridge framer | No crash, hang or out-of-bounds access on any input |
| Fault injection | Bridge and MCU through a corrupting pty proxy | Recovery and counters under corruption, loss, duplication, resets |
| Golden | Generator outputs, error messages | Make every output change visible in review |
| Static analysis | All C, C++, Python | cppcheck, clang-tidy, gcc `-fanalyzer`, mypy, ruff; MISRA subset checks (advisory) |
| Sanitizers | Host builds of core and bridge | ASan and UBSan on every PR |
| Budget and performance | Firmware builds, simavr | Flash, RAM, stack, cycles per byte, latency at given rates |
| Soak | Integration and system levels | Hours of traffic without leaks, counter drift or lost sync |
| Compatibility | Whole stack | Distro and RMW matrix, compilers, Arduino core versions, big-endian |
| Documentation | Examples, README and docs snippets | Every code snippet compiles; every example builds |

## 5. How to test

### 5.1 Writing tests

- **One behavior per test**, named for it: `test_<unit>_<condition>_<expected>` (C and Python), `TEST(Unit, ConditionExpected)` (gtest). The name reads as a sentence in the failure report.
- **Arrange, act, assert**, visibly separated. No logic in tests beyond iterating over vectors or cases; a test that needs an `if` is two tests.
- **Failure messages explain themselves:** expected and actual bytes in hex, the vector name, the spec ID.
- **No sleeps.** Time advances through the fake clock or the manual clock. At the integration and system levels, waits poll a condition with a timeout; they never sleep a fixed duration.
- **Independence.** Every test resets state (`r2s_test_reset()`, fresh fixtures) and passes in any order and alone. Test suites run in random order on every PR.
- **Test code is code.** Same formatting, linting and review standards; shared helpers live in `tests/support/` directories, never copied.

### 5.2 Randomness

- Property-based and fuzz tests use a fixed seed on PRs and a fresh seed nightly. Every random test prints its seed, and a failure is reproducible from the seed alone.
- Every input that ever failed is added permanently: to the hypothesis example database, the fuzz corpus or a vector file.

### 5.3 Test doubles

- **Fakes first:** fake port, fake transport, manual clock, posix virtual MCU. They are small, have their own tests, and behave like the real thing within documented limits.
- **Never mock what you don't own.** rclcpp, type support and pyserial are exercised for real at the integration level, not mocked at the unit level.

### 5.4 Model-based testing of the session

A Python model of the MCU session (states, timers, counters) lives next to the reference codec. A hypothesis state machine generates random event sequences (frames from the host, valid and corrupted, tick advances, publishes, resets) and applies each to the model and to the real C core through `libr2s_host`. After every step, the frames sent, the state and the counters must match. The same approach covers the bridge's session with a model of the host state machine.

### 5.5 Vectors

```yaml
# spec/vectors/messages/float32_1_5.yaml
name: float32_1_5
covers: [ENC-3]
type: std_msgs/msg/Float32
profile: {narrow_float64: false}
message: {data: 1.5}
payload: "00 00 c0 3f"
```

```yaml
# spec/vectors/rcobs/basic.yaml (delimiter included in "encoded")
covers: [COB-1, COB-2, COB-4, COB-7]
cases:
  - {decoded: "",            encoded: "01 00"}
  - {decoded: "00",          encoded: "01 01 00"}
  - {decoded: "00 00",       encoded: "01 01 01 00"}
  - {decoded: "11 22 00 33", encoded: "11 22 03 33 02 00"}
  - {decoded: "11 22 33 44", encoded: "11 22 33 44 05 00"}
  - {decoded: "11 00 00 00", encoded: "11 02 01 01 01 00"}
```

```yaml
# spec/vectors/rcobs/invalid.yaml: encodings a decoder must reject
covers: [COB-5, COB-6]
cases:
  - {encoded: "05 00",    reject: COB-5}
  - {encoded: "11 03 00", reject: COB-5}
```

A reject case gives only the encoding and the ID of the rule that rejects it, which must be one of the file's `covers`. Each reject vector breaks exactly one rule, so every correct decoder names the same one.

```yaml
# spec/vectors/frames/float32_topic16.yaml
name: float32_topic16
covers: [FRM-1, FRM-2, CHK-3]
topic_id: 16
seq: 0
payload: "00 00 c0 3f"
frame: "10 00 00 00 c0 3f 1e 49"        # before rCOBS; CRC 0x491e stored little-endian
wire: "10 02 01 01 c0 3f 1e 49 05 00"
```

```yaml
# spec/vectors/varint/basic.yaml
covers: [VAR-1]
cases:
  - {value: 0,   encoded: "00"}
  - {value: 127, encoded: "7f"}
  - {value: 128, encoded: "80 01"}
  - {value: 300, encoded: "ac 02"}
```

- The first message vectors (Float32, Twist) are computed by hand and reviewed. The Python reference must reproduce them before it generates any further vector.
- Session vectors (handshake transcripts as ordered lists of host and MCU frames) and time vectors (tick, correction and expected `r2s_now()` results, including wraparound and maximum drift) are produced by the reference, reviewed by hand once, then frozen.
- Changing a frozen vector is a breaking wire change: it needs the `!` marker and a protocol version decision.
- `tools/vectors_check.py` validates every file against its schema.

### 5.6 The core's host test harness

The core's host tests run on Unity, vendored unchanged at `core/tests/vendor/unity/` at a named tag. Updating it is a commit that copies the files from a newer tag and changes the version in its README. The rules below are what the harness does and why.

- **Generated runners, no hand-written mains.** `tools/unity_runner.py` writes each test executable's `main`: from the `test_*` definitions of a source file, or from the cases of vector files. Failures are reported against the test or check source, not against the generated file.
- **Vector checks chosen at link time.** A generated vector runner calls `r2s_vec_check_<kind>()` for each case and does not know which implementation it gets. The runner self-test links checks that compare each record's bytes with the vector's own text; each module's conformance test links its own check for the same files. The same mechanism serves both, with no function pointers in test code that might be copied into the core.
- **Fixtures are a separate tree.** `core/tests/fixtures/vectors/` holds files that exercise the runner, named `fixture_*`, and never counts as conformance or spec coverage.
- **Must-fail tests.** Tests under `core/tests/must_fail/` pass only when they fail with the expected message, which proves the assertion helpers and checks can fail at all.
- **One ctest entry per test function.** A generated runner runs one named test when given its name, and CMake registers each test function separately, so `ctest --schedule-random` runs every test alone, in a fresh process, in a random order. Vector runners stay one entry per kind: their checks hold no state.
- **Reset by convention.** A stateful core module keeps its state in one static struct and defines `r2s_<module>_test_reset()` under `R2S_TESTING`; `r2s_test_reset()` calls each, and Unity's `setUp` calls it with `fake_port_reset()` before every test.
- **Configurations as builds.** One CMake project, configured once per CI configuration by `scripts/core-test.sh`: gcc and clang optimized, gcc `-m32`, clang with ASan and UBSan, and gcc with coverage held to the floors of section 6.

### 5.7 Oracles

| Oracle | Checks |
| --- | --- |
| Conformance vectors | C core, Python reference, bridge framing and conversion |
| Python reference codec and session model | A deliberately simple, readable implementation. It generates vectors and is the oracle for property-based, model-based and differential tests |
| `rclpy.serialization.serialize_message` and rclcpp serialization | The bridge's packed-to-CDR output, byte for byte, per RMW |
| Installed type support hashes | The generator's RIHS01 hashes, per distro |
| Published check values | CRC-16/MCRF4XX of `"123456789"` is `0x6F91` |
| Hand computation | The first vectors, reviewed once |

## 6. How much is enough

Coverage is a floor that catches untested code; mutation score is the measure of how well the code is checked. Both are judged head-vs-base on PRs (a PR may not lower them) and against the absolute floors below.

| Component | Line coverage | Branch coverage | Mutation score (nightly) |
| --- | --- | --- | --- |
| `core/` | 95% | 90% | 85% |
| `cpp/` | 90% | 85% | 75% |
| `gen/` (`r2s_gen`) | 95% | 90% | 80% |
| `bridge/` (excluding the rclcpp shell) | 90% | 80% | 70% |
| `scripts/`, `tools/` | 80% | | |

- **Spec coverage is 100%:** every spec ID has at least one test (`tools/spec_coverage.py`).
- **Error-path coverage is 100%:** every `R2S_ERR_*` code and every counter is triggered by at least one test.
- **Mutation testing** runs nightly with Mull (C and C++, LLVM-based) and mutmut (Python); research ticket R13 confirms the tools. Surviving mutants are triaged: either a test is added, or the mutant is recorded as equivalent with a one-line reason.
- Lines excluded from coverage carry an exclusion comment with a reason, and the exclusions are counted in the report.
- The floors start at these values from the first ticket of each component, rather than being raised later: tests are written first, so there is no untested backlog to catch up on.

## 7. When tests run

| Moment | What runs | Time budget |
| --- | --- | --- |
| While editing | `make test CORE=1` (or the component being changed) | Seconds |
| `pre-commit` hook | Format, warnings, unit tests of the touched component | Under 30 s |
| `pre-push` hook (`make ci-local`) | The full PR suite, skipped when the suite stamp shows this tree already passed | Same as PR CI |
| Every PR | Unit, component, contract, integration on posix, simavr scenarios, fuzz smoke (60 s per target), sanitizers, coverage, spec coverage, budgets, build matrix, golden, docs snippets | Under 15 min wall clock, in parallel jobs |
| Nightly | Long fuzzing (30 min per target), mutation testing, extended property and model-based runs, long fault injection, soak (2 h), big-endian, Rolling | Hours |
| Release | Everything above, the hardware checklist on a Uno and a Mega, the full distro and RMW matrix, frozen-vector check | |

When the PR suite exceeds its budget, the slowest tests are made faster or moved down a level; the budget is not raised without the maintainer's approval.

## 8. Keeping the tests honest

- **Flaky tests are bugs.** A test that fails without a code change gets an issue and is fixed at the root cause. Tests are never retried automatically, skipped or quarantined to get green (the ground rule in `AGENTS.md`).
- **Tests are reviewed first.** The reviewer agent and the maintainer read the tests before the code: do they cover the ticket's acceptance criteria, the boundaries and the error paths, and would they fail if the code was wrong?
- **Golden files and vectors change only on purpose.** A diff in either is called out in the PR body with the reason.
- **Fakes are tested.** The fake port, fake transport, manual clock and posix virtual MCU have their own tests against the real components' contracts.
- **Coverage and mutation results are published** per PR (coverage) and per night (mutation), so regressions are visible.

## 9. Test layers in CI

| Layer | What | Tooling | Runs |
| --- | --- | --- | --- |
| Core unit and component | CRC, rCOBS, varint, framer, ring, session, time math, log encoding, codec, topic accessors, each error path; on the minimal, default and full feature builds | Unity, CMake/CTest, gcc and clang, `-m32` build too | Every PR |
| Core contract | Every vector in `spec/vectors/` through the C code | Unity runner generated from YAML | Every PR |
| Core sanitizers | Unit tests under ASan and UBSan | clang | Every PR |
| Core model-based | Session against the Python model through `libr2s_host` | hypothesis + ctypes | Every PR (bounded), longer nightly |
| Coverage and spec coverage | Line, branch, spec IDs, error paths | gcovr, llvm-cov, `tools/spec_coverage.py` | Every PR |
| Mutation testing | Core, C++ layer, generator, bridge logic | Mull, mutmut (R13) | Nightly |
| Zone check | Core free of function pointers, float operations and C++ includes | `tools/zone_check.py` on object files | Every PR |
| Big-endian | Core unit tests on a big-endian target | qemu-user (for example s390x) | Nightly |
| Fuzzing | rCOBS decoder, framer, codec with every descriptor, session input, bridge framer | libFuzzer; corpora in `core/fuzz/corpus/` | 60 s per target on PRs, 30 min nightly |
| Generator unit and golden | Config validation, bounds, naming and collisions, error messages; generated files for example configs | pytest; `--update-golden` for intentional changes | Every PR |
| Generator property and differential | Random message definitions and values: generate C, compile on host, round-trip, compare with the reference | hypothesis + host gcc | Every PR (bounded), more nightly |
| Generator hashes | RIHS01 equals the installed type support's hash for bundled types, per distro | pytest in the ROS containers | Every PR |
| C++ layer | Static and runtime APIs: registration, dispatch, construction order, registration-after-spin error | Host builds + avr-g++ compile tests | Every PR |
| Bridge unit and component | Session with a fake transport and manual clock, converter against rclcpp serialization, estimator with a simulated drifting clock, rate limiting and bandwidth cap | gtest via `ament_cmake_gtest` | Every PR |
| Integration, posix | Bridge plus the posix virtual MCU over a pty: the key scenarios, on both registration paths | launch_testing | Every PR, RMW matrix |
| Fault injection | A pty proxy that flips bits, drops, duplicates and inserts bytes, truncates frames and resets the MCU mid-session | launch_testing + `tests/harness/` | Every PR (short), nightly (long) |
| System, AVR | The same scenarios against firmware in simavr (ATmega328P, ATmega2560), UART on a pty | simavr harness, `tests/harness/simavr/` | Every PR |
| System, PIC | The same scenarios under MPLAB SIM | epic-hal mdb gate | Phase 5 |
| Soak | Two hours of traffic on posix and simavr | launch_testing | Nightly |
| Build matrix and docs | Examples for Uno and Mega (both registration paths), PlatformIO, bare AVR; every code snippet in `README.md` and `docs/` extracted and compiled | arduino/compile-sketches, `pio ci`, make | Every PR |
| PIC canary | Core and static C++ compile with epic-cc for PIC16F1938 and PIC18F4550 | epic-cc release bundle | Every PR from Phase 2, advisory until Phase 5 |
| Budgets and performance | Flash, RAM and worst-case stack per example and tier; cycles per byte for framing and codec in simavr | avr-size, `tools/stack_report.py`, simavr cycle counts | Every PR |
| Hardware in the loop | Real Uno and Mega | Self-hosted runner, `workflow_dispatch` by maintainers only | Phase 1 onwards; PIC boards in Phase 5 |

## 10. Key end-to-end scenarios

In `tests/e2e/`, each a launch_testing test parameterized by MCU backend (posix, simavr, hardware) and registration path:

1. Handshake completes; `ros2 topic list` shows the announced topics within 3 s of the MCU's HELLO.
2. MCU publishes Float32 at 50 Hz for 10 s; the host receives at least 99% with correct values.
3. Host publishes Twist; the MCU echoes it on a test topic; values match after the narrow round trip.
4. Launch remapping and namespace apply to the MCU's topics.
5. A type hash mismatch rejects only that topic, with an error naming the type.
6. Unplug (close the pty) and replug: CONNECTED again within `boot_wait_ms` + 3 s without restarting the bridge.
7. MCU reset: detected within 500 ms (HELLO in reply to a heartbeat), re-handshake follows; publishers are kept when the table is unchanged.
8. Time sync: with a simulated MCU clock off by 5000 ppm, stamps stay within 1 ms of host time after the initial burst.
9. Log messages at each level appear on `/rosout` with the right severity and logger name.
10. Fault injection at 1% byte corruption: no crash, the counters match the injected faults, and delivery recovers on the next frame.
11. A subscribed message that exceeds a bound is dropped, and the bridge logs the topic and the field path.
12. Link saturation: the MCU publishes more than the link carries; publishes return `R2S_ERR_TX_BUSY`, no frame is corrupted, and the counters report it.

These scenarios prove the system's user-visible goals. Edge cases of the parts they exercise are tested at the lower levels, not here.

## 11. Tickets this adds

| ID | Ticket | Deps | Acceptance |
| --- | --- | --- | --- |
| T1 | Core test support: state structs with `r2s_test_reset()`, the scriptable fake port and its own tests, randomized test order | C1 | A test can script a full handshake byte by byte and assert on decoded TX frames |
| T2 | Spec IDs in `spec/protocol.md` and `tools/spec_coverage.py` | S1, B4 | CI fails on an uncovered ID and on a test naming an unknown ID |
| T3 | `libr2s_host` and the Python session model; model-based tests | T1, C8, G4 | Hypothesis state machine finds a deliberately injected session bug |
| T4 | Coverage gates per component, head-vs-base | C1, G1, H1 | A PR lowering coverage fails |
| T5 | Mutation testing nightly with triage report | R13, T4 | Report published; a deliberately weakened test shows up as a surviving mutant |
| T6 | Docs snippet extraction and compilation | B2 | A broken snippet in `README.md` fails CI |
| T7 | Soak job (posix and simavr, 2 h nightly) | V1, V3 | Runs nightly; fails on counter drift or memory growth |
| T8 | Bridge test support: `Transport` and `Clock` interfaces, fake transport, manual clock, with their own tests | H1 | Bridge session tests run without an rclcpp executor |
| R13 | Mutation testing tools for C, C++ and Python (Mull, mutmut, alternatives): accuracy, speed, CI fit | | Recommendation with a trial run on the core and the generator |
