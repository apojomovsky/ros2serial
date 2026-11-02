"""The scoped local gate: changed files map to the suite sections that read them.

Each top-level tree maps to the sections that test it and the sections that
consume it, derived from what each ci-test.sh section actually reads. Suite
machinery and unknown paths map to the full suite: unknown means full, never
less. An empty diff runs the cheap checks only.
"""

import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import scopes_for_diff

ROOT = Path(__file__).resolve().parent.parent

CHEAP = scopes_for_diff.CHEAP


def scopes(*paths):
    return scopes_for_diff.scopes_for_paths(list(paths))


def test_core_maps_to_its_sections_and_its_consumers():
    assert scopes("core/src/rcobs.c") == {
        **CHEAP,
        "CORE": "1",
        "COVERAGE": "1",
        "FUZZ": "1",
        "GEN": "1",
        "EXAMPLES": "1",
        "E2E": "both",
        "DOCS": "1",
        "BRIDGE": "1",
        "CPP": "1",
    }


def test_core_fuzz_corpus_maps_to_fuzz():
    assert scopes("core/fuzz/corpus/framer/empty")["FUZZ"] == "1"


def test_gen_maps_to_gen_examples_docs_and_bridge():
    assert scopes("gen/r2s_gen/cli.py") == {
        **CHEAP,
        "GEN": "1",
        "EXAMPLES": "1",
        "DOCS": "1",
        "BRIDGE": "1",
    }


def test_bridge_maps_to_bridge_coverage_and_both_e2e():
    assert scopes("bridge/r2s_bridge/src/node.cpp") == {
        **CHEAP,
        "BRIDGE": "1",
        "COVERAGE": "1",
        "E2E": "both",
    }


def test_cpp_maps_to_cpp_and_its_consumers():
    assert scopes("cpp/src/runtime.cpp") == {**CHEAP, "CPP": "1", "EXAMPLES": "1", "E2E": "avr", "DOCS": "1"}


def test_spec_maps_to_vectors_fuzz_scripts_and_bridge():
    expected = {**CHEAP, "CORE": "1", "GEN": "1", "FUZZ": "1", "SCRIPTS": "1", "BRIDGE": "1"}
    assert scopes("spec/protocol.md") == expected
    assert scopes("spec/vectors/framer/empty.yaml") == expected


def test_examples_map_to_examples_and_gen():
    assert scopes("examples/arduino_minimal/arduino_minimal.ino") == {
        **CHEAP,
        "EXAMPLES": "1",
        "GEN": "1",
    }


def test_posix_port_maps_to_core_posix_e2e_and_bridge():
    assert scopes("ports/posix/src/r2s_posix_firmware.c") == {
        **CHEAP,
        "CORE": "1",
        "E2E": "posix",
        "BRIDGE": "1",
    }


def test_arduino_port_maps_to_core_and_examples():
    assert scopes("ports/arduino/run.sh") == {**CHEAP, "CORE": "1", "EXAMPLES": "1"}


def test_other_ports_fail_safe_to_full():
    assert scopes("ports/stm32/src/port.c") == {"FULL": "1"}


# scripts/test_e2e_* load files from tests/e2e/, so every e2e path also runs SCRIPTS.
E2E_SHARED = {**CHEAP, "E2E": "both", "BRIDGE": "1", "SCRIPTS": "1"}


def test_e2e_tests_map_to_both_backends_bridge_and_scripts():
    assert scopes("tests/e2e/test_scenario_1_posix.py") == E2E_SHARED
    assert scopes("tests/e2e/e2e_support/relay.py") == E2E_SHARED
    assert scopes("tests/e2e/e2e_support/teardown.py") == E2E_SHARED
    assert scopes("tests/e2e/run.sh") == E2E_SHARED
    assert scopes("tests/harness/simavr/run.sh") == {**CHEAP, "E2E": "avr"}


def test_nightly_isolation_pair_runs_outside_the_pr_scopes():
    assert scopes("tests/e2e/test_isolation.py") == {**CHEAP, "NIGHTLY": "1", "SCRIPTS": "1"}


def test_avr_only_e2e_paths_map_to_the_avr_leg():
    for path in (
        "tests/e2e/run-avr.sh",
        "tests/e2e/avr_slice/avr_slice.ino",
        "tests/e2e/e2e_support/avr.py",
        "tests/e2e/test_avr_support.py",
        "tests/e2e/test_scenario_1_avr.py",
        "tests/e2e/test_scenario_2_avr.py",
        "tests/e2e/test_scenario_3_avr.py",
        "tests/e2e/test_scenario_4_avr.py",
    ):
        assert scopes(path) == {**CHEAP, "E2E": "avr", "SCRIPTS": "1"}, path


def test_docs_map_to_docs():
    assert scopes("docs/01-testing.md") == {**CHEAP, "DOCS": "1"}
    assert scopes("README.md") == {**CHEAP, "DOCS": "1", "README": "1"}


def test_tools_map_to_scripts_and_the_section_each_serves():
    assert scopes("tools/snippets_check.py") == {**CHEAP, "SCRIPTS": "1", "DOCS": "1"}
    assert scopes("tools/coverage_gate.py") == {**CHEAP, "SCRIPTS": "1", "COVERAGE": "1"}
    assert scopes("tools/fuzz_corpus.py") == {**CHEAP, "SCRIPTS": "1", "FUZZ": "1"}
    assert scopes("tools/size_report.py") == {**CHEAP, "SCRIPTS": "1", "BUDGETS": "1"}
    assert scopes("tools/stack_report.py") == {**CHEAP, "SCRIPTS": "1", "BUDGETS": "1"}
    assert scopes("tools/budgets.yaml") == {**CHEAP, "SCRIPTS": "1", "BUDGETS": "1"}
    assert scopes("tools/vectors_check.py") == {**CHEAP, "SCRIPTS": "1"}
    assert scopes("tools/test_zone_check.py") == {**CHEAP, "SCRIPTS": "1"}


def test_section_scripts_map_to_their_section():
    assert scopes("scripts/core-test.sh") == {**CHEAP, "CORE": "1"}
    assert scopes("scripts/bridge-test.sh") == {**CHEAP, "BRIDGE": "1"}
    assert scopes("scripts/coverage-gate.sh") == {**CHEAP, "COVERAGE": "1"}
    assert scopes("scripts/build-examples.sh") == {**CHEAP, "EXAMPLES": "1", "BUDGETS": "1"}
    assert scopes("scripts/budgets-check.sh") == {**CHEAP, "BUDGETS": "1"}
    assert scopes("scripts/budgets_comment.py") == {**CHEAP, "SCRIPTS": "1", "BUDGETS": "1"}
    assert scopes("scripts/check-readme.sh") == {**CHEAP, "README": "1"}
    assert scopes("scripts/cpp-test.sh") == {**CHEAP, "CPP": "1"}
    assert scopes("scripts/fuzz.sh") == {**CHEAP, "FUZZ": "1"}
    assert scopes("scripts/build-host-lib.sh") == {**CHEAP, "CORE": "1", "GEN": "1"}
    assert scopes("scripts/test_ritual.py") == {**CHEAP, "SCRIPTS": "1"}
    assert scopes("scripts/push.sh") == {**CHEAP, "SCRIPTS": "1"}


def test_suite_machinery_maps_to_full():
    for path in (
        ".githooks/pre-push",
        "Makefile",
        "Dockerfile",
        "ci/versions.env",
        ".github/workflows/ci.yml",
        "scripts/ci-test.sh",
        "scripts/pre-pr-check.sh",
        "scripts/scopes_for_diff.py",
        "scripts/suite-stamp.sh",
        "scripts/suite-lock.sh",
        "scripts/clean-git-env.sh",
        "scripts/dev-image-tag.sh",
    ):
        assert scopes(path) == {"FULL": "1"}, path


def test_unknown_paths_fail_safe_to_full():
    assert scopes("VERSION") == {"FULL": "1"}
    assert scopes("brand-new-dir/file.c") == {"FULL": "1"}


def test_empty_diff_runs_the_cheap_checks_only():
    assert scopes() == CHEAP


def test_scopes_merge_across_paths():
    assert scopes("core/src/rcobs.c", "bridge/r2s_bridge/src/node.cpp") == {
        **CHEAP,
        "CORE": "1",
        "COVERAGE": "1",
        "FUZZ": "1",
        "GEN": "1",
        "EXAMPLES": "1",
        "E2E": "both",
        "DOCS": "1",
        "BRIDGE": "1",
        "CPP": "1",
    }


def test_full_wins_over_scoped():
    assert scopes("core/src/rcobs.c", "Makefile") == {"FULL": "1"}


def test_e2e_backends_merge_to_both():
    assert scopes("tests/e2e/run.sh", "tests/harness/simavr/run.sh") == {
        **CHEAP,
        "BRIDGE": "1",
        "E2E": "both",
    }


def test_bridge_and_avr_only_paths_merge_to_both():
    assert scopes("bridge/r2s_bridge/src/node.cpp", "tests/e2e/run-avr.sh") == {
        **CHEAP,
        "BRIDGE": "1",
        "COVERAGE": "1",
        "E2E": "both",
    }


def git(repo: Path, *args: str) -> None:
    subprocess.run(["git", *args], cwd=repo, check=True, capture_output=True, text=True)


def tiny_repo(tmp_path: Path) -> Path:
    repo = tmp_path / "repo"
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


def run_scopes(repo: Path, **env) -> subprocess.CompletedProcess:
    merged = {**os.environ, **env}
    return subprocess.run(
        [sys.executable, str(ROOT / "scripts/scopes_for_diff.py"), "--base", "origin/master"],
        cwd=repo,
        capture_output=True,
        text=True,
        env=merged,
    )


def test_cli_lists_assignments_for_committed_and_untracked_changes(tmp_path, monkeypatch):
    monkeypatch.setenv("GITHUB_ACTIONS", "true")
    repo = tiny_repo(tmp_path)
    (repo / "core" / "b.c").write_text("int y;\n")
    out = run_scopes(repo)
    assert out.returncode == 0, out.stdout + out.stderr
    assert "CORE=1" in out.stdout
    assert "LINT=1" in out.stdout


def test_cli_empty_diff_lists_the_cheap_checks_only(tmp_path):
    out = run_scopes(tiny_repo(tmp_path))
    assert out.returncode == 0, out.stdout + out.stderr
    assert sorted(out.stdout.split()) == ["COMMITS=1", "LINT=1"]


def test_cli_full_env_forces_the_full_suite(tmp_path):
    repo = tiny_repo(tmp_path)
    (repo / "core" / "b.c").write_text("int y;\n")
    out = run_scopes(repo, R2S_FULL="1")
    assert out.returncode == 0, out.stdout + out.stderr
    assert out.stdout.split() == ["FULL=1"]


def test_cli_machinery_only_diff_runs_the_cheap_scopes_locally(tmp_path, monkeypatch):
    monkeypatch.delenv("GITHUB_ACTIONS", raising=False)
    repo = tiny_repo(tmp_path)
    (repo / "Makefile").write_text("# machinery\n")
    out = run_scopes(repo)
    assert out.returncode == 0, out.stdout + out.stderr
    assert sorted(out.stdout.split()) == ["COMMITS=1", "LINT=1"]
    assert "leaving the full suite to CI" in out.stderr


def test_cli_bridge_diff_runs_the_cheap_scopes_locally(tmp_path, monkeypatch):
    monkeypatch.delenv("GITHUB_ACTIONS", raising=False)
    repo = tiny_repo(tmp_path)
    (repo / "bridge").mkdir()
    (repo / "bridge" / "node.cpp").write_text("// bridge\n")
    out = run_scopes(repo)
    assert out.returncode == 0, out.stdout + out.stderr
    assert sorted(out.stdout.split()) == ["COMMITS=1", "LINT=1"]
    assert "leaving the full suite to CI" in out.stderr


def test_cli_core_diff_runs_the_cheap_scopes_locally(tmp_path, monkeypatch):
    monkeypatch.delenv("GITHUB_ACTIONS", raising=False)
    repo = tiny_repo(tmp_path)
    (repo / "core" / "b.c").write_text("int y;\n")
    out = run_scopes(repo)
    assert out.returncode == 0, out.stdout + out.stderr
    assert sorted(out.stdout.split()) == ["COMMITS=1", "LINT=1"]
    assert "leaving the full suite to CI" in out.stderr


def test_cli_machinery_plus_bridge_diff_runs_the_cheap_scopes(tmp_path, monkeypatch):
    monkeypatch.delenv("GITHUB_ACTIONS", raising=False)
    repo = tiny_repo(tmp_path)
    (repo / "Makefile").write_text("# machinery\n")
    (repo / "bridge").mkdir()
    (repo / "bridge" / "node.cpp").write_text("// bridge\n")
    out = run_scopes(repo)
    assert out.returncode == 0, out.stdout + out.stderr
    assert sorted(out.stdout.split()) == ["COMMITS=1", "LINT=1"]
    assert "leaving the full suite to CI" in out.stderr


def test_cli_full_env_still_runs_the_full_suite_for_machinery(tmp_path, monkeypatch):
    monkeypatch.delenv("GITHUB_ACTIONS", raising=False)
    repo = tiny_repo(tmp_path)
    (repo / "Makefile").write_text("# machinery\n")
    out = run_scopes(repo, R2S_FULL="1")
    assert out.returncode == 0, out.stdout + out.stderr
    assert out.stdout.split() == ["FULL=1"]
    assert "leaving the full suite to CI" not in out.stderr


def test_cli_ci_runs_the_full_suite_for_machinery(tmp_path, monkeypatch):
    monkeypatch.setenv("GITHUB_ACTIONS", "true")
    repo = tiny_repo(tmp_path)
    (repo / "Makefile").write_text("# machinery\n")
    out = run_scopes(repo)
    assert out.returncode == 0, out.stdout + out.stderr
    assert out.stdout.split() == ["FULL=1"]
    assert "leaving the full suite to CI" not in (out.stdout + out.stderr)
