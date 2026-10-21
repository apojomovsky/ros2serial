# ADR-002: the core's host test harness

**Status:** Accepted 2026-10-21

## Decision

- **Unity, vendored.** The core's host tests use Unity, copied unchanged into `core/tests/vendor/unity/` at a named tag. Updating it is a commit that copies the files from a newer tag and changes the version in its README.
- **Generated runners, no hand-written mains.** `tools/unity_runner.py` writes each test executable's `main`: from the `test_*` definitions of a source file, or from the cases of vector files. Failures are reported against the test or check source, not against the generated file.
- **Vector checks chosen at link time.** A generated vector runner calls `r2s_vec_check_<kind>()` for each case and does not know which implementation it gets. The runner self-test links checks that compare each record's bytes with the vector's own text; each module's conformance test links its own check for the same files. The same mechanism serves both, with no function pointers in test code that might be copied into the core.
- **Fixtures are a separate tree.** `core/tests/fixtures/vectors/` holds files that exercise the runner, named `fixture_*`, and never counts as conformance or spec coverage.
- **Must-fail tests.** Tests under `core/tests/must_fail/` pass only when they fail with the expected message, which proves the assertion helpers and checks can fail at all.
- **Configurations as builds.** One CMake project, configured once per CI configuration by `scripts/core-test.sh`: gcc and clang optimized, gcc `-m32`, clang with ASan and UBSan, and gcc with coverage held to the floors of `docs/01-testing.md`.

## Rationale

Unity is plain C99, small, MIT licensed and runs on the same 8-bit targets the core does, so the same tests can later run on simavr. Generating the runners removes the commonest Unity mistake, a test written but never registered. Choosing checks at link time lets the vector runner exist and be tested before any algorithm does, and makes adding a module's conformance test a one-line CMake change.

## Rejected alternatives

- **Unity's Ruby runner generator.** It would add Ruby to the image for one script and does not read the vector YAML.
- **CMocka or Check.** Larger, and Check needs fork(), which rules out running the suite on a simulated MCU.
- **A git submodule for Unity.** Builds would need network access and a second checkout step, and the pinned version would live outside the tree's review.
