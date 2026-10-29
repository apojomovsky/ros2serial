# End-to-end tests

The key scenarios from `docs/01-testing.md`, section 10, against real
firmware and the real bridge over a serial line.

- `run.sh` is the posix entry point: it builds the bridge workspace and the
  posix firmwares, checks the helper assertions, then runs scenarios 1 to 4
  plus the publish catch-up test under each RMW the design names, once per
  registration path. The static leg uses the generated C firmware and the
  runtime leg the C++ runtime firmware; both announce the same topics, so the
  scenario files stay shared and read the firmware from `R2S_E2E_FW`. The
  suite runs it for `E2E=posix`; `R2S_RMWS` narrows the RMW loop for a
  quicker iteration.
- `run-avr.sh` is the avr entry point: it builds the simavr harness and the
  slice sketch for Uno and Mega, checks the helper assertions, then runs
  scenarios 1 to 4 on both boards under each RMW. The suite runs it for
  `E2E=avr`; `R2S_RMWS` and `R2S_E2E_BOARDS` narrow the loops.
- `e2e_support/` is the framework: the pty relay, the posix firmware
  launcher, the simavr launcher, the bridge launch through its own launch
  file, and the checks every backend shares.
- `test_scenario_N_posix.py` is one launch_testing test per scenario on the
  posix backend. Each file launches only the bridge; the firmware spawns
  inside the test once the bridge answers, so timing budgets measure the
  handshake, not node startup. `test_publish_catchup_posix.py` stalls the
  firmware mid-window and holds the 50 Hz count to the scenario 2 floor.
- `test_scenario_N_avr.py` is the same scenario on the simavr backend. Each
  file spawns the board's firmware first and launches the bridge on the
  harness pty; scenarios 1 and 4 re-stamp the HELLO time at bridge-up, since
  the boot HELLO predates it. Scenario 2 collects a fixed message count
  instead of a time window, so simulated time running off the wall clock
  cannot skew the gate, and scenario 3 passes the float32 tolerance the 8-bit
  round trip keeps.
- `avr_slice/` is the slice sketch the avr backend runs: the four slice
  topics over the static X-macro API, publishing battery at 50 Hz and
  echoing cmd_vel to echo_vel.
- `checks_test.py` unit-tests the shared checks with no ROS in the loop.

A scenario file also runs on its own for iteration: source the ROS and
bridge installs, point `R2S_E2E_FW` at a firmware binary (either leg's), and
run `launch_test` with `PYTHONPATH=tests/e2e`. An avr scenario file needs the
harness and the board firmware built: set `R2S_SIMAVR_BIN`,
`R2S_E2E_AVR_FW` and `R2S_E2E_BOARD`, then run `launch_test` the same way.
