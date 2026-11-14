# Changelog

## 0.4.0 (2026-11-14)

### Features

- **ci:** Stop cancelling master runs and make master CI diff-selected (#537)
- **docs:** Publish the docs site and finalize the port guide (#539)
- **ci:** Wire the PIC canary job with epic-cc (advisory) (#551)
- **bridge:** Doxygen-form doc comments on the bridge public API (#553)
- **core:** Add an MCU action server with a ROS 2-consistent API (#555)
- **bridge:** Serve and proxy services over the reliable channel (#559)
- **ci:** Publish the size and performance budgets and gate on them (#570)
- **cpp:** Ros services and getParam over X5 and X6 (#569)
- **ports:** PlatformIO layout with install-and-build gate (#575)
- **ports:** Build STM32F103 and RP2040 firmware and record budgets (#574)
- **ci:** Gate nightly mutation run and publish fuzz coverage (#573)
- **ci:** Cap concurrent suites with a host-wide gate (#585)
- **ports:** Bring up the CH32V003 tier M proof port (#579)
- **ci:** Measure the bridge mutation score in the nightly run (#589)
- **ci:** Judge the lines a pull request changes, not the package average (#591)
- **ci:** Check line length and cheap format rules on pushed lines (#595)
- **ci:** Fail the PIC canary on unrecorded epic-cc gaps (#596)
- **ci:** Bounded retries around apt installs (#598)
- **bridge:** Action server for MCU actions (#584)
- **ports:** Porting guide and ports/template skeleton for new boards (#605)
- **ports:** Fail ports without a build script, host test or source (#610)
- **examples:** Arduino service sketch and exercised service and action examples (#611)
- **docs:** ROS-native onboarding, the guide set and an Arduino CLI helper (#615)
- **e2e:** Serve the spin action on posix and avr with scenario coverage (#620)

### Fixes

- **e2e:** Anchor the scenario 6 replug and keep the isolation pair logs (#538)
- **ports:** Fail the flood run fast on mid-run disconnect (#541)
- **ci:** Single-quote the site tools so the LINT scope passes (#547)
- **rosdoc2:** Doxyfile, rosdoc2 pages and quality declaration with an advisory build (#554)
- **cpp:** Doxygen-form doc comments on the C++ layer and port headers (#558)
- **gen:** Clear flake8 builtin shadows and D401 residue (#565)
- **gen:** Settle the ruff/ament lint divergence (I100, E203) (#572)
- **ci:** Coverage gate crashes on the floor-only path with an unbound $4 (#608)
- **ports:** Accumulate ch32v tick across the SysTick wrap (#609)
- **bridge:** Answer in-flight service calls on re-handshake (#616)
- **bridge:** Pack service payloads narrow on 32-bit-double MCUs (#618)
- **e2e:** Pace the avr harness and log re-handshake causes (#619)
- **docs:** Match every document to the code before 0.4.0 (#624)
- **gen:** Regenerate and stage every version-stamped golden (#628)

### Refactoring

- **tree:** Reformat the C core, ports and the Python generator to the ROS 2 style (#540)

### Documentation

- **research:** Add R4 generic service and client API inventory (#536)
- Licence and copyright headers on every source file, with a gate (#548)
- **core:** Doxygen-form doc comments in core/include (#546)
- **gen:** Bring r2s_gen docstrings to PEP 257 and Sphinx fields (#550)
- Replace the full MIT blocks with the agreed SPDX two-line header, applied by a tool (#552)
- **bridge:** Drop the unresolvable Doxygen link requests (#557)
- **research:** Add R15 codebase organization review (#568)
- **ci:** Measure what the per-job package installs cost (#602)

### Tests

- **ci:** Pin the Arduino zip as the PlatformIO release artifact (#588)

### CI

- **release:** Author the release commit as the token owner (#534)
- **scripts:** Widen the SPDX header gate to the whole tree (#563)
- **nightly:** Raise e2e-isolation timeout to 60 minutes and log section times (#581)
- Keep the pre-push ritual to the fast gates (#580)
- Move e2e-isolation out of the nightly workflow (#587)
- **pic-canary:** Pin epic-cc 0.8.0, build PIC16F1938 compact (#622)
## 0.3.0 (2026-11-08)

### Breaking

- **spec:** Parameters and the reliable channel (#457)
- **spec:** Services and actions over the reliable channel (#500)

### Features

- **ci:** Audit released dependencies against rosdep (#485)
- **ci:** Build and test the released packages outside the dev image (#486)
- **cpp:** Opt-in QoS selection on runtime topics (#488)
- **bridge:** Publish MCU error report counts on /diagnostics (#489)
- **bridge:** Key per-topic diagnostics by topic id (#490)
- **ci:** Gate the generator on head-vs-base coverage (#495)
- **core:** Reliable channel and parameter walk (#496)
- **ports:** Lead the quickstart with Arduino Uno (#498)
- **ci:** Kill leg trees and retry the walkthrough echo (#517)
- **e2e:** Report per-failure summary lines from run-avr.sh (#518)
- **bridge:** Declare MCU parameters as ROS parameters and push their sets (#523)
- **ci:** Run ament_lint_common on the ROS packages, advisory with a recorded baseline (#526)
- **gen:** Parse .action files to Goal Result and Feedback (#525)
- **ci:** Keep fuzz-long off pull requests unless labelled fuzz-long (#530)

### Chores

- **bridge:** Reformat to the ament uncrustify style (#527)

### Fixes

- **e2e:** Thread an explicit clock through wait_for_handshake (#484)
- **ci:** Ship the s390x C++ compiler in the big-endian image (#494)
- **gen:** Pin the gen coverage measurement (#502)
- **core:** Little-endian wire order on big-endian targets (#499)
- **ports:** Keep the flood host alive with keep-alive PINGs (#506)
- **e2e:** Stage AVR scenarios 3 and 4 past the handshake (#505)
- **e2e:** Spin the discovery wait and report the last graph (#516)
- **bridge:** Repair routes missing despite an ok verdict (#524)

### Documentation

- **architecture:** Describe v2 today, name pending S5 and S8 (#483)
- **readme:** Overhaul for 0.3.0 with a posix-first quickstart (#491)
- **ci:** Adopt the ROS 2 comment and documentation rules (#520)
- Audit README and all docs before 0.3.0 (#521)
- Final README accuracy check before 0.3.0 (#531)

### CI

- Keep the budgets table in the job summary, not a pull request comment (#487)
## 0.2.0 (2026-11-04)

### Features

- **ci:** Run the nightly jobs on PRs that touch them (#383)
- **ci:** Publish full releases marked Latest by default (#384)
- **ci:** Check mermaid blocks in the docs with mermaid's own parser (#390)
- **e2e:** Fault-injecting pty proxy and scenario 10 (#393)
- **ports:** Saturate the link and prove TX_BUSY in scenario 12 (#397)
- **e2e:** Corrupt delimiters and code bytes in scenario 10 (#399)
- **e2e:** Avr leg for link-saturation scenario 12 (#401)
- **bridge:** Injectable termios seam and error-path tests toward section 6 floors (#406)
- **e2e:** Two hour soak on posix and simavr, nightly (#400)
- **ci:** Require the bridge floors with throw branches excluded (#410)
- **core:** Send and decode error reports on message ID 5 (#409)
- **cpp:** Add rosserial-compatible runtime aliases (#419)
- **e2e:** Paint the stack in scenarios and table it with static (#421)
- **cpp:** Remaining NodeHandle surface over the runtime registry (#422)
- **gen:** Range, Imu and JointState with bounds and field-carrying bound reports (#420)
- **e2e:** Simavr baud matrix at 115200, 500000 and 1000000 (#429)
- **benchmark:** Framing and CRC benchmark (AVR Uno/Mega) (#431)
- **e2e:** Scenarios 6 and 7 for reconnect and reset (#430)
- **benchmark:** P10 encoding benchmark and write-up (#434)
- **cpp:** Tf broadcaster, service stubs and ros e2e leg (#436)
- **examples:** Task-shaped sketches with the native runtime API (#446)
- **gen:** Rosidl plugin wrapper for colcon builds (#456)
- **ci:** Per-section change detection with Task checksums (#435)
- **ci:** Make concurrent branches merge cleanly (#466)
- **ci:** Nightly mutation testing with a triage report (#467)
- **ports:** ESP32 build-only via the Arduino layout (#468)
- **ci:** Save matrix checksums only when every leg passed (#465)
- **bridge:** Add namespace and remappings launch arguments (#475)
- **bridge:** Several live links share one bridge process (#476)
- **ci:** Run the big-endian qemu job nightly (#478)

### Fixes

- **gen:** Tab-tolerant limit guards and named failures for bad bytes (#387)
- **e2e:** Scenario 3 waits for handshake and discovery before priming (#388)
- **gen:** Stage the cpp tree inside gen for the sdist (#396)
- **e2e:** Hold scenario 10 slice edges and name isolation failures (#403)
- **ci:** Forward R2S_RMWS and R2S_E2E_BOARDS into the image (#408)
- **e2e:** Run scenario 1 discovery budget from the completed handshake (#414)
- **e2e:** Run discovery waits from the completed handshake (#416)
- **ports:** Static firmware refuses overlong --node (#427)
- **bridge:** Catch a failed diagnostics publish in the poll entry (#432)
- **ci:** Leave the end-to-end sections out of pre-push and takeoff (#464)
- **bridge:** Count a failed serialize instead of aborting the node (#470)
- **e2e:** Scenario 8 bound defaults to 5 ms, strict 1 ms runs nightly (#473)
- **ci:** Forward R2S_SUITE_LOCK_WAIT and R2S_LOCK_NOWAIT into the image (#477)

### Documentation

- **architecture:** Add contributor page with mermaid diagrams (#385)
- **testing:** Add Testing in practice section with test levels and CI flow (#389)
- **gen:** Record padding-gap struct rejection by the emitted size check (#392)
- **design:** Bring actions into scope in the design doc (#424)
- **ports:** Draft port your rosserial sketch guide (#425)
- **design:** Note ros2serial stays serial-only, point WiFi boards onward (#451)
- **design:** Rename services and actions spec S6 to S8 (#453)

### Tests

- **e2e:** Flash-to-RAM topic rebind over simavr (#394)
- **e2e:** Anchor scenario 6 and 7 handshakes past the boot wait (#441)
- **e2e:** Name isolation failures and anchor handshakes past boot wait (#445)
- **e2e:** Read the handshake on a short spin (#455)

### CI

- **versions:** Pin the rolling bridge image (#474)
## 0.1.0 (2026-11-02)

### Breaking

- **spec:** Host-assigned session token replaces the port boot ID (#239)
- **gen:** Spell X-macro rows with ROS 2 types (#305)
- **cpp:** Add the static C++ API over the X-macro list (#322)
- **gen:** Expand underscore C rows next to the C++ spelling (#329)

### Features

- **gen:** Add the reference CRC, rCOBS and varint codec, and reject cases in the vectors
- **ci:** Check spec coverage from vectors and test markers, with a shrinking pending list
- **core:** Implement the fake port, the state reset hook and random test order
- **core:** Add the bitwise CRC-16/MCRF4XX
- **core:** Encode and decode varint lengths
- **core:** Encode and decode rCOBS frames
- **ci:** The takeoff ritual checks the PR title against the commits (#53)
- **gen:** The generator package, its CLI and the version it reports (#104)
- **core:** Cut, check and count frames on the receive path (#105)
- **bridge:** The bridge package skeleton, component and parameters (#107)
- **gen:** Load messages and resolve bounds from search paths and distro snapshots (#111)
- **e2e:** The simavr harness runs echo firmware on Uno and Mega (#112)
- **core:** Stream frames out within the port's free space (#113)
- **bridge:** Transport and clock interfaces, fake transport and manual clock (#114)
- **bridge:** Open termios ports behind the transport interface (#115)
- **gen:** Emit C99 structs for the resolved message types (#118)
- **core:** The optional receive ring behind R2S_WITH_RX_RING (#119)
- **gen:** Python reference codec with rCOBS, CRC and narrow topics (#125)
- **bridge:** Link the core's framing and CRC into the bridge (#127)
- **core:** MCU session announce, handshake and heartbeat (#128)
- **gen:** RIHS01 type hashes without a ROS install (#171)
- **gen:** Vectors subcommand prints payload hex from the reference (#173)
- **bridge:** Host session boot wait, handshake and reset rules (#176)
- **gen:** Config schema and check with golden error messages (#178)
- **ci:** Coverage gates per component, head against base (#179)
- **core:** Iterative codec interpreter with handwritten Float32 and Twist (#181)
- **bridge:** Type support loading, hash check, TOPIC_STATUS (#186)
- **ports:** Posix port with pty firmware, tick and boot ID counter (#188)
- **gen:** Emit codec descriptor tables in S6 format (#192)
- **ci:** Compile docs snippets in CI (#194)
- **core:** Topic accessors with the flash-table backend (#198)
- **bridge:** Packed to CDR converter with cached plans (#201)
- **ports:** Publish boot and periodic lines on posix log_text (#202)
- **gen:** Emit topic table, publish functions, dispatch, limits, name checks (#204)
- **ci:** Skip documented non-code snippet tags by tag (#205)
- **gen:** Generate cmake and arduino layouts with matrix-built examples (#218)
- **core:** Flash-string logging calls on the log channel (#226)
- **bridge:** Generic publishers and subscriptions, remapping, QoS, rate limits (#228)
- **core:** Flash-aware topic reads on AVR (#236)
- **gen:** Bundle subcommand and the X-macro header (#237)
- **bridge:** Launch file and example parameter file (#241)
- **ports:** Serve the Arduino port from the library (#245)
- **core:** Fuzz targets and seed corpora in fuzz-smoke and nightly (#247)
- **ci:** Check one version everywhere and pre-release on a tag (#248)
- **e2e:** Framework, posix backend, scenarios 1 to 4 (#253)
- **gen:** Name config hash in generated pair banner (#256)
- **bridge:** Publish /diagnostics from the bridge (#257)
- **ci:** Compile snippets against generated-header fixtures (#261)
- **bridge:** Bound host-side pending for control frames (#265)
- **gen:** Libr2s_host and the Python session model (#270)
- **spec:** Time vectors for wraparound, maximum drift, anchor expiry (#273)
- **bridge:** Refuse a name announced in both directions (#277)
- **ci:** Report quoted fences and skip literal examples (#283)
- **core:** Reenact loss and reset in the transcript replay (#286)
- **cpp:** Add the ROS 2-named runtime API (#287)
- **ci:** Scope the local gate to the sections the diff touches (#298)
- **ci:** Regenerate version-stamped files in the cut, clean artifact build (#300)
- **e2e:** Uno and Mega firmware for the slice; e2e-avr runs scenarios 1 to 4 (#288)
- **e2e:** Lease per-suite isolation and time scenario 1 by bridge state (#301)
- **core:** PONG with delimiter tick, CORRECTION and r2s_now (#304)
- **bridge:** Host time-sync estimator and scheduling (#310)
- **ci:** One staged header per snippet, one diagnostic per listed fence (#312)
- **ci:** Run the avr e2e leg for bridge and e2e changes (#314)
- **bridge:** Forward MCU log lines to rosout (#331)
- **ci:** Leave the full suite to CI for local gates (#333)
- **ci:** Local gates leave the whole suite to CI (#336)
- **ci:** Flash, RAM and stack budgets with PR comment (#325)
- **ci:** Lease a DDS domain for the bridge and coverage sections (#339)
- **docs:** Getting started walkthrough from install to topic echo (#343)
- **ci:** Budgets job comments only when a number moves (#344)
- **ci:** Shard the SCRIPTS scope by test node (#345)
- **ci:** Run only the jobs a pull request's diff touches (#346)
- **e2e:** Scenario 8 time sync and isolation canary proof (#347)
- **ci:** Nightly and release runs take the e2e isolation tests (#349)
- **e2e:** Run scenarios 1 to 4 on the static C++ firmware (#359)
- **ci:** Add everyday make targets next to the CI scopes (#361)

### Chores

- **spike:** Add K1 Uno spike with host script, simavr harness and size bench
- **ci:** Add repository skeleton, AGENTS.md and ADR-001
- Stop tracking a Python bytecode cache

### Fixes

- **ci:** Run the nested suite with the caller's tools and no inherited scope
- **core:** Catch libm calls, header inlines and cpp includes in the zone check
- **ci:** Harden ticket selection, PR body check and release cut per review
- **ci:** Run the PR's own body check when the base has none
- **ci:** Check the PR body on every push so the check exists on the head commit
- **ci:** Accept git merge messages in the commit-msg hook during a merge
- **ci:** Clear the hook's git variables before tests run
- **ci:** Accept merge.log shortlogs, document the hook isolation per review
- **core:** Separate fixture runners, run cppcheck in the suite, and harden the harness per review
- **spec:** Reject stray zero bytes, order the reject rules and check reject IDs per review
- **ci:** Scan only test roots and comments, and keep the pending list from growing per review
- **ci:** Let the PR that introduces the pending list fill it
- **core:** Run each test alone in random order, fail unnoticed overruns, per review
- **core:** Give the core headers C linkage and test the CRC at the largest frame, per review
- **core:** Return the decoded frame's length from the rCOBS decoder
- **core:** Give the fuzz target room for the re-encoding
- **core:** Take the review findings on the rCOBS codec
- **ci:** The ritual does not leak its scopes into the suite
- **ci:** The ritual's own inputs do not reach the suite either
- **ci:** The suite keeps the commit range and the title
- **ci:** The suite does not pass TEST down to its own tests
- **ci:** The suite drops TEST once, and the child keeps the scopes
- **ci:** Every pull request is squashed into one commit on master (#48)
- **ci:** Trim the repository to code, specification and design (#102)
- **spec:** Specify the codec descriptor format and the local section rule (#108)
- **ci:** Drop placeholders from directories that hold files (#109)
- **spec:** Clarify narrow conversion and add hand-made vectors (#116)
- **ci:** Key the suite lock by the tree, not its path in the container (#117)
- **spec:** Report a cyclic YAML alias instead of crashing (#175)
- **spec:** Float32/Twist frames, handshake transcript, vectors-only spec sync (#207)
- **core:** Encode and decode string arrays element by element (#208)
- **ports:** Reject over-long launch topic names before configure (#213)
- **spec:** Keep exact-float32 findings and index nested array paths (#217)
- **ci:** The PR title check takes its range from the merge base (#220)
- **spec:** Report a cyclic frames name instead of crashing (#221)
- **ci:** Fail a scoped run that executes no section (#229)
- **ports:** Reject over-long launch node names before configure (#230)
- **ci:** Resolve ci.yml ranges from the merge base (#232)
- **ci:** Report non-UTF-8 spec as a sync problem (#243)
- **gen:** Reject unknown message keys (#249)
- **ci:** Keep the last versions.env pin without trailing newline (#255)
- **gen:** Reject an empty topic list with a precise error (#263)
- **spec:** Boot_handshake follows the session the core replays (#266)
- **gen:** Report a cyclic alias as an error, not a traceback (#269)
- **gen:** Fail an empty X-macro topic list in the bundle header (#272)
- **ci:** Report non-UTF-8 spec as a sync problem in spec-changed branch (#278)
- **ports:** Publish every missed tick in the posix firmware (#280)
- **gen:** Wrap long topic names the way clang-format wraps them (#281)
- **ci:** Report a non-UTF-8 spec instead of crashing or passing silently (#284)
- **e2e:** Lease isolation slot in the avr runner and time scenario 4 (#309)
- **spec:** Full paths for exact-float32 findings and a bounded alias walk (#306)
- **ci:** Fail on versions.env read errors with a precise message (#307)
- **ports:** Resync the example loops without bursts (#313)
- **ci:** Leave every local diff to CI with the cheap scopes (#341)
- **bridge:** Survive a failed serialized publish (#342)
- **ci:** Bound hangs and isolate the bridge pair (#356)
- **e2e:** Wait out a full pty instead of dropping UART bytes (#357)
- **ci:** Write the plan scopes outside the checkout (#364)
- **docs:** Truth pass on what works and what does not yet (#360) (#366)
- **ci:** Tag the release fixture at the tree's version (#370)
- **e2e:** Resolve the isolation root from tests/e2e (#372)
- **e2e:** Tolerate slow zenoh session close in teardown (#375)
- **ci:** Run SCRIPTS for tests/e2e changes and stub the teardown module (#377)
- **ci:** Give fuzz-long time for its four targets (#379)
- **ci:** Release skips the time-boxed fuzzing (#381)

### Refactoring

- **bridge:** Tidy the session's log wording and table check (#177)

### Documentation

- Add kickoff proposal v1 and v2
- Reframe v2 around rosserial drop-in on Uno/Mega, PIC16-class as deferred bound
- Tier by resources, add 32-bit targets (CH32V002, STM32, RP2040, ESP32)
- Adopt epic-cc agent-friendly tooling section
- Close scout gaps in tooling section
- Add kickoff proposal v3
- Split engineering practices out of the v3 proposal
- **research:** Add R1 rosserial archaeology and R2 prior art
- Fold R1 and R2 findings into the v3 proposal
- Add the testing strategy as docs/01-testing.md
- Format the launch example with the pinned ruff
- Keep master as the default branch
- **spec:** Add the protocol specification v0.1 with spec IDs
- **spec:** Tighten frame limits, rCOBS decoding and the encoding rules per review
- **design:** Name the varint module and its API
- **core:** Say what the bridge uses the varint codec for
- **design:** Name the rCOBS module and its API
- **core:** Shorten the rCOBS decoder's contract comment
- **core:** Keep the rCOBS decoder contract within the prose cap
- **ci:** Drop the test run from the PR body ritual
- **design:** The definition of done no longer asks the PR to show it
- **ci:** Name the R2S_SUITE override in the ritual's header
- **ci:** Name the fixture in the leak test's comment
- **spec:** The system channels and the session become normative (#45)
- **contributing:** Quickstart and workflow guide for developers (#362)

### Tests

- **ci:** Add repository layout check
- **ci:** Require a single suite script that gates lint and layout
- **ci:** Require a commit message check and a grouped changelog
- **spec:** Require a schema check for every vector kind
- **ci:** Require the make entry points, image tag, doctor, guard and lock
- **ci:** Require the prose gate, the takeoff ritual, the hooks and the push helper
- **core:** Require the zone check on compiled core objects
- **core:** Cover the zone check gaps the review found
- **ci:** Require ticket selection and the release version bump
- **ci:** Require a shared PR body check for CI and the ritual
- **spec:** Require unique, well-formed spec IDs
- **ci:** Cover the selector, body check and release gaps the review found
- **spec:** Check bare ID mentions against the defined IDs
- **ci:** Let the commit-msg hook accept git merge messages during a merge
- **ci:** Reproduce hook git variables leaking into the test suite
- **ci:** Pin the pre-push isolation and merge message rules the review found
- **core:** Add the first unit tests and the runner generator's tests
- **core:** Reproduce the runner, script and must-fail gaps the review found
- **spec:** Add CRC, rCOBS and varint vectors and the reference codec's tests
- **spec:** Reproduce the vector and reference gaps the review found
- **ci:** Specify the spec coverage check with fixture trees
- **ci:** Reproduce the spec coverage gaps the review found
- **ci:** Let the PR that introduces the pending list fill it
- **core:** Specify the fake port, the state reset and random test order
- **core:** Reproduce the fake port and test isolation gaps the review found
- **core:** Run the CRC vectors and the residue property through the core
- **core:** Run the varint vectors and the length properties through the core
- **core:** Drop the unused includes from the varint test
- **core:** Refuse a varint that ends with a third high byte
- **core:** Cover the varint cases the review found untested
- **core:** Run the rCOBS vectors and the framing properties through the core
- **core:** Seed the rCOBS decoder's fuzz corpus
- **core:** Check the canonical encoding on more shapes
- **core:** Check the rCOBS canonical encoding on random inputs
- **ports:** Cover the missing-default launch-name guard (#223)
- **ports:** Pin posix tick test to CLOCK_MONOTONIC (#238)
- **gen:** Prove bundled hashes against installed support per distro (#244)
- **core:** Replay the handshake transcripts through the MCU session (#259)
- **bridge:** Pin bool-sequence resize count, document field_path empty path (#285)
- **bridge:** Replay refused_hello and version_mismatch on the host session (#293)

### Build

- **docker:** Add the dev image, the full Makefile and run CI in the image
- **core:** Add the CMake project, Unity, the vector runner and the sanitizer and coverage builds
- **core:** Mark the fuzz corpus as binary

### CI

- Add the CI skeleton, the single suite script and pinned versions
- Add the commit message and PR title check and the changelog config
- Add the vector schema check and keep merge commits for stacked PRs
- Add the git hooks, the takeoff ritual, the prose gate and the push helper
- Add the zone check on compiled core objects
- Add the ticket selector, the PR body check and the release cut
- Give the lint job the PR base for the spec coverage check
- **bridge:** Run the bridge job in the pinned distro images (#121)
