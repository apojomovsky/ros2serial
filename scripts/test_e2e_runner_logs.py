"""The e2e runners keep every scenario launch behind its own log file.

run.sh tees each launch_test into scenario-logs and reports the failed
file's tail from its own log, so a nonzero exit always names its cause.
run-avr.sh mirrors it, so the avr pair leaves the same artifacts for
the nightly upload. These tests run the runners' own log fragments
with stubbed launch commands, never a suite.
"""

import shlex
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
RUN_SH = ROOT / "tests" / "e2e" / "run.sh"
RUN_AVR_SH = ROOT / "tests" / "e2e" / "run-avr.sh"

START = "scenario_log=$build/scenario-logs/"


def log_fragment(path, occurrence=0):
    """The per-scenario log block: the scenario_log line to its closing fi."""
    lines = path.read_text().splitlines()
    starts = [i for i, line in enumerate(lines) if START in line]
    assert len(starts) > occurrence, f"no scenario log block in {path.name}"
    start = starts[occurrence]
    end = next(i for i in range(start, len(lines)) if lines[i].strip() == "fi")
    return "\n".join(lines[start : end + 1])


def run_fragment(path, variables, stub_code, occurrence=0):
    """The fragment's output and the files it wrote, from a blank build dir."""
    prelude = (
        'timeout() { shift; "$@"; }\n'
        f'launch_test() {{ echo "LAUNCH $*"; return {stub_code}; }}\n'
        'status=0\nfailures=""\n'
    )
    setup = "".join(f"{name}={shlex.quote(value)}\n" for name, value in variables.items())
    setup += 'mkdir -p "$build/scenario-logs"\n'
    proc = subprocess.run(
        ["bash", "-c", prelude + setup + log_fragment(path, occurrence)],
        capture_output=True,
        text=True,
        cwd=variables["build"],
    )
    assert proc.returncode == 0, proc.stderr
    return proc.stdout


def test_every_launch_lands_in_its_own_log_file():
    for runner in (RUN_SH, RUN_AVR_SH):
        launches = [line for line in runner.read_text().splitlines() if "launch_test" in line and "timeout" in line]
        assert launches, f"{runner.name} launches nothing"
        for line in launches:
            assert line.rstrip().endswith('| tee "$scenario_log"'), f"unlogged launch in {runner.name}:\n{line}"


def test_runners_create_the_log_directory():
    for runner in (RUN_SH, RUN_AVR_SH):
        assert 'mkdir -p "$build/scenario-logs"' in runner.read_text(), runner.name


@pytest.mark.parametrize(
    ("runner", "variables", "log_name"),
    [
        (
            RUN_SH,
            {"leg": "static", "rmw": "rmw_fastrtps_cpp", "test_file": "test_scenario_2_posix"},
            "static.rmw_fastrtps_cpp.test_scenario_2_posix.log",
        ),
        (
            RUN_AVR_SH,
            {"board": "uno", "rmw": "rmw_fastrtps_cpp", "scenario": "2"},
            "uno.rmw_fastrtps_cpp.test_scenario_2_avr.log",
        ),
    ],
)
def test_failing_launch_names_its_file_and_keeps_its_log(tmp_path, runner, variables, log_name):
    variables = dict(variables, build=str(tmp_path))
    out = run_fragment(runner, variables, stub_code=1)
    assert "e2e-failure:" in out
    log = tmp_path / "scenario-logs" / log_name
    assert log.is_file(), "failing launch left no log file"
    assert "LAUNCH" in log.read_text()
    assert "LAUNCH" in out, "failing launch tail missing from the report"


@pytest.mark.parametrize(
    ("runner", "variables", "log_name"),
    [
        (
            RUN_SH,
            {"leg": "static", "rmw": "rmw_fastrtps_cpp", "test_file": "test_scenario_2_posix"},
            "static.rmw_fastrtps_cpp.test_scenario_2_posix.log",
        ),
        (
            RUN_AVR_SH,
            {"board": "uno", "rmw": "rmw_fastrtps_cpp", "scenario": "2"},
            "uno.rmw_fastrtps_cpp.test_scenario_2_avr.log",
        ),
    ],
)
def test_passing_launch_stays_quiet_but_keeps_its_log(tmp_path, runner, variables, log_name):
    variables = dict(variables, build=str(tmp_path))
    out = run_fragment(runner, variables, stub_code=0)
    assert "e2e-failure:" not in out
    assert (tmp_path / "scenario-logs" / log_name).is_file()
