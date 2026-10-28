# End-to-end tests

The key scenarios from `docs/01-testing.md`, section 10, against real
firmware and the real bridge over a serial line.

- `run.sh` is the entry point: it builds the bridge workspace and the posix
  firmwares, checks the helper assertions, then runs scenarios 1 to 4 plus the publish catch-up test under each RMW the design names, once per registration path. The static leg uses the generated C firmware and the runtime leg the C++ runtime firmware; both announce the same topics, so the scenario files stay shared and read the firmware from `R2S_E2E_FW`. The suite runs it for `E2E=posix`; `R2S_RMWS` narrows the RMW loop for a quicker iteration.
- `e2e_support/` is the framework: the pty relay, the firmware launcher, the
  bridge launch through its own launch file, and the checks every backend
  shares. A new backend (simavr, hardware) adds its own launcher and scenario
  files and reuses the checks.
- `test_scenario_N_posix.py` is one launch_testing test per scenario on the
  posix backend. Each file launches only the bridge; the firmware spawns
  inside the test once the bridge answers, so timing budgets measure the
  handshake, not node startup. `test_publish_catchup_posix.py` stalls the
  firmware mid-window and holds the 50 Hz count to the scenario 2 floor.
- `checks_test.py` unit-tests the shared checks with no ROS in the loop.

A scenario file also runs on its own for iteration: source the ROS and
bridge installs, point `R2S_E2E_FW` at a firmware binary (either leg's), and
run `launch_test` with `PYTHONPATH=tests/e2e`.
