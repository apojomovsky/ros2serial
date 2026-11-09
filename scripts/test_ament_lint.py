"""The advisory ament_lint_common gate: wiring, runner and CI job.

r2s_bridge, r2s_gen and r2s_rosidl declare the lint test dependencies and
wire ament_lint_auto behind the R2S_AMENT_LINT option, off by default so
lint findings never fail the bridge or farm builds. scripts/ament-lint.sh
enables the option, runs only the lint tests, prints the per-linter
baseline, and exits nonzero only when a cleaned area (copyright
everywhere, cpplint on the bridge) reports findings; ci.yml runs it as a
continue-on-error job, so the gate stays non-blocking. The tests stub
colcon, python3 and pytest, so they never build.
"""

import os
import re
import stat
import subprocess
import xml.etree.ElementTree as ET
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "scripts" / "ament-lint.sh"

PACKAGES = (
    ROOT / "bridge" / "r2s_bridge" / "package.xml",
    ROOT / "gen" / "package.xml",
    ROOT / "gen" / "r2s_rosidl" / "package.xml",
)

CMAKE_PACKAGES = (
    ROOT / "bridge" / "r2s_bridge" / "CMakeLists.txt",
    ROOT / "gen" / "r2s_rosidl" / "CMakeLists.txt",
)

GEN_LINT_TESTS = (
    ("test_flake8.py", "flake8"),
    ("test_pep257.py", "pep257"),
    ("test_copyright.py", "copyright"),
    ("test_xmllint.py", "xmllint"),
)


def test_packages_declare_the_lint_test_depends():
    for package in PACKAGES:
        depends = {e.text.strip() for e in ET.parse(package).getroot().iter("test_depend")}
        assert "ament_lint_auto" in depends, f"{package} misses ament_lint_auto"
        assert "ament_lint_common" in depends, f"{package} misses ament_lint_common"


def test_cmake_wires_lint_auto_behind_the_advisory_option():
    for cmake in CMAKE_PACKAGES:
        text = cmake.read_text()
        assert "option(R2S_AMENT_LINT" in text, f"{cmake} misses the advisory option"
        default = re.search(r'option\(R2S_AMENT_LINT\s+"[^"]*"\s+(\w+)\)', text)
        assert default is not None and default.group(1) == "OFF", f"{cmake} option is not off by default"
        testing = text.index("if(BUILD_TESTING)")
        assert testing < text.index("find_package(ament_lint_auto REQUIRED)"), cmake
        assert testing < text.index("ament_lint_auto_find_test_dependencies()"), cmake


def test_gen_has_env_gated_lint_tests():
    for name, linter in GEN_LINT_TESTS:
        path = ROOT / "gen" / "tests" / name
        assert path.is_file(), f"missing {path}"
        text = path.read_text()
        assert f"@pytest.mark.{linter}" in text, f"{path} misses its linter mark"
        assert "R2S_AMENT_LINT" in text, f"{path} runs outside the advisory job"


COLCON_STUB = """#!/usr/bin/env bash
echo "$@" >> "$R2S_COLCON_LOG"
if [ "$1" = test ]; then
  exit 1
fi
exit 0
"""

# python3 stands in for pytest only: the runner calls nothing else through it.
PYTHON_STUB = """#!/usr/bin/env bash
echo "R2S_AMENT_LINT=$R2S_AMENT_LINT" >> "$R2S_PYTEST_LOG"
echo "$@" >> "$R2S_PYTEST_LOG"
exit 1
"""


def run_advisory(tmp_path, colcon_stub=COLCON_STUB, python_stub=PYTHON_STUB, with_results=True, copyright_failures=0):
    bindir = tmp_path / "bin"
    bindir.mkdir(exist_ok=True)
    (bindir / "colcon").write_text(colcon_stub)
    (bindir / "python3").write_text(python_stub)
    for tool in ("colcon", "python3"):
        path = bindir / tool
        path.chmod(stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH | path.stat().st_mode)
    # Pre-existing results the summary reads: copyright is clean on three
    # checks, flake8 is clean, rosidl has none.
    if with_results:
        results = tmp_path / "build" / "ament-lint" / "build" / "r2s_bridge" / "test_results" / "r2s_bridge"
        results.mkdir(parents=True)
        suite = f'<testsuite name="copyright" tests="3" failures="{copyright_failures}"'
        suite += ' errors="0" skipped="0"></testsuite>\n'
        (results / "copyright.xunit.xml").write_text(suite)
        (results / "flake8.xunit.xml").write_text(
            '<testsuite name="flake8" tests="1" failures="0" errors="0" skipped="0"></testsuite>\n'
        )
    env = {
        "PATH": os.pathsep.join([str(bindir), "/usr/bin", "/bin"]),
        "HOME": str(tmp_path),
        "R2S_CACHE_DIR": str(tmp_path),
        "R2S_BUILD_DIR": str(tmp_path / "build"),
        "R2S_COLCON_LOG": str(tmp_path / "colcon.log"),
        "R2S_PYTEST_LOG": str(tmp_path / "pytest.log"),
        "ROS_DISTRO": "jazzy",
        "AMENT_PREFIX_PATH": "/stub",
    }
    return subprocess.run(["bash", str(SCRIPT)], cwd=ROOT, capture_output=True, text=True, env=env)


def test_advisory_run_enables_lint_selects_lint_tests_and_reports_a_baseline(tmp_path):
    assert SCRIPT.is_file(), "scripts/ament-lint.sh is missing"
    result = run_advisory(tmp_path)
    out = result.stdout + result.stderr
    assert result.returncode == 0, f"clean baseline must pass:\n{out}"
    build_calls = (tmp_path / "colcon.log").read_text()
    assert "-DR2S_AMENT_LINT=ON" in build_calls, f"lint option not enabled:\n{build_calls}"
    assert "--ctest-args" in build_calls and "-R" in build_calls, f"lint tests not selected:\n{build_calls}"
    pytest_log = (tmp_path / "pytest.log").read_text()
    assert "R2S_AMENT_LINT=1" in pytest_log, f"gen lint tests not enabled:\n{pytest_log}"
    assert "-m" in pytest_log and "pytest" in pytest_log, f"gen lint not run through pytest:\n{pytest_log}"
    assert "baseline" in out.lower(), f"no baseline table:\n{out}"
    assert "r2s_bridge copyright 0 3" in out, f"copyright count missing:\n{out}"
    assert "r2s_bridge flake8 0 1" in out, f"flake8 count missing:\n{out}"


def test_cleaned_area_findings_fail_the_run_but_not_the_gate(tmp_path):
    result = run_advisory(tmp_path, copyright_failures=2)
    out = result.stdout + result.stderr
    assert result.returncode != 0, f"copyright findings must fail the run:\n{out}"
    assert "r2s_bridge copyright 2 3" in out, f"copyright count missing:\n{out}"
    assert "expected zero" in out, f"no zero expectation:\n{out}"


def test_advisory_run_fails_loudly_when_no_linter_ran(tmp_path):
    result = run_advisory(tmp_path, with_results=False)
    assert result.returncode != 0, "a silent no-run must fail, not report an empty baseline"
    assert "no lint results" in result.stderr, result.stderr


def test_advisory_run_fails_without_colcon(tmp_path):
    assert SCRIPT.is_file(), "scripts/ament-lint.sh is missing"
    # An empty PATH hides colcon; the absolute bash never needs the lookup.
    env = {
        "PATH": str(tmp_path),
        "HOME": str(tmp_path),
        "ROS_DISTRO": "jazzy",
        "AMENT_PREFIX_PATH": "/stub",
    }
    result = subprocess.run(["/bin/bash", str(SCRIPT)], cwd=ROOT, capture_output=True, text=True, env=env)
    assert result.returncode != 0, "missing colcon must fail"
    assert "colcon not found" in result.stderr, result.stderr


def test_ci_runs_the_advisory_job_that_reports_without_failing():
    workflow = yaml.safe_load((ROOT / ".github" / "workflows" / "ci.yml").read_text())
    job = workflow["jobs"].get("ament-lint")
    assert job is not None, "ci.yml has no ament-lint job"
    assert job.get("continue-on-error") is True, "the lint gate must stay advisory"
    assert job.get("timeout-minutes", 0) >= 30, "the job builds, so it keeps the 30 minute floor"
    needs = job.get("needs", [])
    needs = needs if isinstance(needs, list) else [needs]
    assert "plan" in needs, "the job must gate on the plan like every section"
    assert "needs.plan.outputs.ament_lint" in job.get("if", ""), "the job must run only when its task is stale"
    steps = job.get("steps", [])
    assert any("make test AMENT_LINT=1" in str(step.get("run", "")) for step in steps), (
        "the job must run the section through make, so the dev image provides colcon"
    )


def test_the_taskfile_runs_the_advisory_section_through_the_suite():
    taskfile = yaml.safe_load((ROOT / "Taskfile.yml").read_text())
    task = taskfile["tasks"].get("ament_lint")
    assert task is not None, "Taskfile has no ament_lint task"
    assert task["cmds"] == ["AMENT_LINT=1 bash scripts/ci-test.sh"], task["cmds"]
    assert "scripts/ament-lint.sh" in task["sources"], "the runner is not a task input"
    for aggregate in ("default", "local"):
        assert "ament_lint" in taskfile["tasks"][aggregate]["deps"], f"{aggregate} skips the advisory section"
