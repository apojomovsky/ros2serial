"""The everyday make targets developers use next to the CI scopes."""

import os
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def run_make(*args, cwd=ROOT, env=None):
    merged = {**os.environ, **(env or {})}
    return subprocess.run(["make", *args], cwd=cwd, capture_output=True, text=True, env=merged)


def test_help_shows_the_everyday_group_first():
    out = run_make("help")
    assert out.returncode == 0, out.stderr
    assert "Everyday" in out.stdout
    assert "CI and agents" in out.stdout
    assert out.stdout.index("Everyday") < out.stdout.index("CI and agents")


def test_each_alias_expands_to_its_scope():
    aliases = {
        "test-core": "CORE=1",
        "test-gen": "GEN=1",
        "test-bridge": "BRIDGE=1",
        "test-cpp": "CPP=1",
        "test-scripts": "SCRIPTS=1",
        "e2e-posix": "E2E=posix",
        "e2e-avr": "E2E=avr",
    }
    for alias, scope in aliases.items():
        out = run_make("-n", alias)
        assert out.returncode == 0, alias + out.stderr
        assert scope in out.stdout, alias
        assert "scripts/ci-test.sh" in out.stdout, alias


def test_test_all_expands_to_the_unscoped_suite():
    out = run_make("-n", "test-all")
    assert out.returncode == 0, out.stderr
    assert "scripts/ci-test.sh" in out.stdout
    assert re.search(r"\b(CORE|GEN|BRIDGE|CPP|SCRIPTS|LINT|E2E)=", out.stdout) is None


def test_check_expands_to_the_diff_scope_script():
    out = run_make("-n", "check")
    assert out.returncode == 0, out.stderr
    assert "scripts/check.sh" in out.stdout


def test_pytest_runs_a_single_selection():
    out = run_make("-n", "pytest", "ARGS=gen/test_x.py::test_y")
    assert out.returncode == 0, out.stderr
    assert "scripts/pytest-one.sh" in out.stdout


def test_pytest_without_args_shows_usage():
    out = run_make("pytest")
    assert out.returncode != 0
    assert "usage" in (out.stdout + out.stderr).lower()


def test_ctest_runs_a_filtered_build():
    out = run_make("-n", "ctest", "ARGS=rcobs")
    assert out.returncode == 0, out.stderr
    assert "scripts/ctest-one.sh" in out.stdout


def test_ctest_without_args_shows_usage():
    out = run_make("ctest")
    assert out.returncode != 0
    assert "usage" in (out.stdout + out.stderr).lower()


def test_misspelled_scope_fails_with_the_valid_scopes():
    out = run_make("-n", "test", "CROE=1")
    assert out.returncode != 0
    text = out.stdout + out.stderr
    assert "CROE" in text
    assert "CORE" in text


def test_lowercase_scope_suggests_the_uppercase_spelling():
    out = run_make("-n", "test", "core=1")
    assert out.returncode != 0
    assert "CORE" in (out.stdout + out.stderr)


def test_valid_scope_names_match_the_suite():
    script = (ROOT / "scripts/check-scope-vars.sh").read_text()
    scopes = set(re.search(r'^scopes="([^"]+)"', script, re.MULTILINE).group(1).split())
    wants = set(re.findall(r"wants ([A-Z][A-Z0-9_]*)", (ROOT / "scripts/ci-test.sh").read_text()))
    assert scopes == wants | {"SCRIPTS_SHARD"}


def test_pytest_one_builds_the_host_lib_and_runs_pytest():
    text = (ROOT / "scripts/pytest-one.sh").read_text()
    assert "build-host-lib" in text
    assert "python3 -m pytest" in text


def test_ctest_one_filters_a_built_config():
    text = (ROOT / "scripts/ctest-one.sh").read_text()
    assert "core-test.sh" in text
    assert "--build-only" in text
    assert re.search(r"ctest .* -R", text) is not None


def _build_default(text, leaf):
    return re.search(r"\$\{R2S_BUILD_DIR:-(.+?)\}/" + leaf, text).group(1)


def test_one_test_scripts_share_the_suite_build_dir_default():
    core = (ROOT / "scripts/core-test.sh").read_text()
    one = (ROOT / "scripts/ctest-one.sh").read_text()
    assert _build_default(one, "core") == _build_default(core, "core")
    suite = (ROOT / "scripts/ci-test.sh").read_text()
    pytest_one = (ROOT / "scripts/pytest-one.sh").read_text()
    assert _build_default(pytest_one, "host") == _build_default(suite, "host")


def git(repo, *args):
    subprocess.run(["git", *args], cwd=repo, check=True, capture_output=True, text=True)


def fixture_repo(path):
    repo = path / "repo"
    repo.mkdir()
    git(repo, "init", "-q", "-b", "master")
    git(repo, "config", "user.email", "t@example.com")
    git(repo, "config", "user.name", "t")
    (repo / "core").mkdir()
    (repo / "core" / "a.c").write_text("int x;\n")
    git(repo, "add", ".")
    git(repo, "commit", "-q", "-m", "feat(core): start")
    git(repo, "update-ref", "refs/remotes/origin/master", "HEAD")
    return repo


STRIPPED_SCOPES = (
    "LINT",
    "SCRIPTS",
    "COMMITS",
    "CORE",
    "GEN",
    "BRIDGE",
    "CPP",
    "E2E",
    "COVERAGE",
    "DOCS",
    "EXAMPLES",
    "BUDGETS",
    "FUZZ",
    "README",
    "NIGHTLY",
    "FULL",
    "R2S_FULL",
    "SCRIPTS_SHARD",
)


def run_check(repo, out_file, **env):
    recorder = out_file.parent / "recorder.sh"
    recorder.write_text(f'#!/usr/bin/env bash\nenv | sort > "{out_file}"\n')
    recorder.chmod(0o755)
    # One pass: the runner's own scopes must not leak in as caller scopes.
    clean = {k: v for k, v in os.environ.items() if k not in STRIPPED_SCOPES}
    clean.update({"PATH": os.environ["PATH"], "BASE": "origin/master", "R2S_SUITE": str(recorder), **env})
    result = subprocess.run(
        ["bash", str(ROOT / "scripts/check.sh")], cwd=repo, capture_output=True, text=True, env=clean
    )
    assert result.returncode == 0, result.stdout + result.stderr
    recorded = dict(
        line.split("=", 1) for line in out_file.read_text().splitlines() if re.match(r"^[A-Z][A-Z0-9_]*=", line)
    )
    return result.stdout, recorded


def test_check_runs_the_scopes_of_a_core_diff(tmp_path):
    repo = fixture_repo(tmp_path)
    (repo / "core" / "b.c").write_text("int y;\n")
    out_file = tmp_path / "scopes.txt"
    stdout, recorded = run_check(repo, out_file)
    assert recorded.get("CORE") == "1"
    assert recorded.get("LINT") == "1"
    assert recorded.get("COMMITS") == "1"
    assert "CORE=1" in stdout


def test_check_runs_the_fast_gates_for_a_machinery_diff(tmp_path):
    repo = fixture_repo(tmp_path)
    (repo / "Makefile").write_text("# machinery\n")
    out_file = tmp_path / "scopes.txt"
    stdout, recorded = run_check(repo, out_file)
    assert recorded.get("LINT") == "1"
    assert recorded.get("COMMITS") == "1"
    assert "CORE" not in recorded
    assert "full suite to CI" in stdout


def test_check_runs_everything_only_when_asked_for_full(tmp_path):
    repo = fixture_repo(tmp_path)
    (repo / "Makefile").write_text("# machinery\n")
    out_file = tmp_path / "scopes.txt"
    stdout, recorded = run_check(repo, out_file, R2S_FULL="1")
    assert "CORE" not in recorded
    assert "LINT" not in recorded
    assert "FULL" in stdout


def test_check_merges_a_caller_e2e_with_the_computed_one(tmp_path):
    repo = fixture_repo(tmp_path)
    (repo / "tests" / "e2e").mkdir(parents=True)
    (repo / "tests" / "e2e" / "run-avr.sh").write_text("# avr only\n")
    out_file = tmp_path / "scopes.txt"
    _, recorded = run_check(repo, out_file, E2E="posix")
    assert recorded.get("E2E") == "both"


def test_make_forwards_the_e2e_loop_narrowers():
    # tests/e2e/README.md documents both for a quicker local loop; make
    # must hand them into the dev image or they are silently ignored.
    line = next(line for line in (ROOT / "Makefile").read_text().splitlines() if line.startswith("PASS_VARS"))
    for var in ("R2S_RMWS", "R2S_E2E_BOARDS"):
        assert var in line.split(), f"make drops {var} before the container"
