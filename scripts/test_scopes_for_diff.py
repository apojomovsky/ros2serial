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


def scopes(*paths):
    return scopes_for_diff.scopes_for_paths(list(paths))


CHEAP = {"LINT": "1", "COMMITS": "1"}


def test_core_maps_to_its_sections_and_its_consumers():
    assert scopes("core/src/rcobs.c") == {
        **CHEAP,
        "CORE": "1",
        "COVERAGE": "1",
        "FUZZ": "1",
        "GEN": "1",
        "EXAMPLES": "1",
        "E2E": "both",
    }


def test_core_fuzz_corpus_maps_to_fuzz():
    assert scopes("core/fuzz/corpus/framer/empty")["FUZZ"] == "1"


def test_gen_maps_to_gen_examples_and_docs():
    assert scopes("gen/r2s_gen/cli.py") == {
        **CHEAP,
        "GEN": "1",
        "EXAMPLES": "1",
        "DOCS": "1",
    }


def test_bridge_maps_to_bridge_coverage_and_posix_e2e():
    assert scopes("bridge/r2s_bridge/src/node.cpp") == {
        **CHEAP,
        "BRIDGE": "1",
        "COVERAGE": "1",
        "E2E": "posix",
    }


def test_spec_maps_to_the_vector_checks_in_core_and_gen():
    assert scopes("spec/protocol.md") == {**CHEAP, "CORE": "1", "GEN": "1"}
    assert scopes("spec/vectors/framer/empty.yaml") == {**CHEAP, "CORE": "1", "GEN": "1"}


def test_examples_map_to_examples():
    assert scopes("examples/arduino_minimal/arduino_minimal.ino") == {
        **CHEAP,
        "EXAMPLES": "1",
    }


def test_posix_port_maps_to_core_and_posix_e2e():
    assert scopes("ports/posix/src/r2s_posix_firmware.c") == {
        **CHEAP,
        "CORE": "1",
        "E2E": "posix",
    }


def test_arduino_port_maps_to_core():
    assert scopes("ports/arduino/run.sh") == {**CHEAP, "CORE": "1"}


def test_other_ports_fail_safe_to_full():
    assert scopes("ports/stm32/src/port.c") == {"FULL": "1"}


def test_e2e_tests_map_to_their_backend_only():
    assert scopes("tests/e2e/test_scenario_1_posix.py") == {**CHEAP, "E2E": "posix"}
    assert scopes("tests/harness/simavr/run.sh") == {**CHEAP, "E2E": "avr"}


def test_docs_map_to_docs():
    assert scopes("docs/01-testing.md") == {**CHEAP, "DOCS": "1"}
    assert scopes("README.md") == {**CHEAP, "DOCS": "1"}


def test_tools_map_to_scripts_and_the_section_each_serves():
    assert scopes("tools/snippets_check.py") == {**CHEAP, "SCRIPTS": "1", "DOCS": "1"}
    assert scopes("tools/coverage_gate.py") == {**CHEAP, "SCRIPTS": "1", "COVERAGE": "1"}
    assert scopes("tools/fuzz_corpus.py") == {**CHEAP, "SCRIPTS": "1", "FUZZ": "1"}
    assert scopes("tools/vectors_check.py") == {**CHEAP, "SCRIPTS": "1"}
    assert scopes("tools/test_zone_check.py") == {**CHEAP, "SCRIPTS": "1"}


def test_section_scripts_map_to_their_section():
    assert scopes("scripts/core-test.sh") == {**CHEAP, "CORE": "1"}
    assert scopes("scripts/bridge-test.sh") == {**CHEAP, "BRIDGE": "1"}
    assert scopes("scripts/coverage-gate.sh") == {**CHEAP, "COVERAGE": "1"}
    assert scopes("scripts/build-examples.sh") == {**CHEAP, "EXAMPLES": "1"}
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
    assert scopes("cpp/r2s/node.hpp") == {"FULL": "1"}
    assert scopes("brand-new-dir/file.c") == {"FULL": "1"}


def test_empty_diff_runs_the_cheap_checks_only():
    assert scopes() == CHEAP


def test_scopes_merge_across_paths():
    assert scopes("core/src/rcobs.c", "docs/01-testing.md") == {
        **CHEAP,
        "CORE": "1",
        "COVERAGE": "1",
        "FUZZ": "1",
        "GEN": "1",
        "EXAMPLES": "1",
        "E2E": "both",
        "DOCS": "1",
    }


def test_full_wins_over_scoped():
    assert scopes("core/src/rcobs.c", "Makefile") == {"FULL": "1"}


def test_e2e_backends_merge_to_both():
    assert scopes("tests/e2e/run.sh", "tests/harness/simavr/run.sh") == {
        **CHEAP,
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


def test_cli_lists_assignments_for_committed_and_untracked_changes(tmp_path):
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
