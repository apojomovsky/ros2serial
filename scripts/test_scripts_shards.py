"""Node sharding for the SCRIPTS scope: every collected test runs in exactly one CI shard.

Shard 0 holds scripts/test_e2e_isolation.py on its own until #337 moves it to
the nightly. Every other node id spreads round-robin over shards 1 to N-1 in
sorted order, so the split is deterministic and needs no stored list.
"""

import re
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent

sys.path.insert(0, str(ROOT / "scripts"))

import shard_nodes  # noqa: E402


def sample_nodes():
    """A small node list with a pinned file, plain and parametrized ids."""
    e2e = [
        "scripts/test_e2e_isolation.py::test_two_concurrent_e2e_sections_pass_without_crosstalk",
        "scripts/test_e2e_isolation.py::test_avr_and_posix_sections_pass_side_by_side",
    ]
    others = [
        "scripts/test_ritual.py::test_a_clean_pr_passes",
        "scripts/test_ritual.py::test_a_dirty_pr_gets_one_fix_line_per_problem",
        "tools/test_vectors_check.py::test_each_case_decodes[param-0]",
        "tools/test_vectors_check.py::test_each_case_decodes[param-1]",
        "tools/test_zone_check.py::test_no_float_ops",
    ]
    return e2e + others


def test_shard_selection_is_deterministic():
    nodes = sample_nodes()
    first = [shard_nodes.nodes_for_shard(nodes, s) for s in range(shard_nodes.N_SHARDS)]
    second = [shard_nodes.nodes_for_shard(nodes, s) for s in range(shard_nodes.N_SHARDS)]
    assert first == second


def test_every_node_lands_in_exactly_one_shard():
    nodes = sample_nodes()
    picked = [shard_nodes.nodes_for_shard(nodes, s) for s in range(shard_nodes.N_SHARDS)]
    assert sorted(n for shard in picked for n in shard) == sorted(nodes)
    assert sum(len(shard) for shard in picked) == len(nodes)


def test_e2e_isolation_stays_together_in_shard_zero():
    nodes = sample_nodes()
    shard_zero = shard_nodes.nodes_for_shard(nodes, 0)
    assert sorted(shard_zero) == sorted(n for n in nodes if "test_e2e_isolation.py" in n)
    for shard in range(1, shard_nodes.N_SHARDS):
        assert not any("test_e2e_isolation.py" in n for n in shard_nodes.nodes_for_shard(nodes, shard))


def test_pinning_matches_the_bare_filename_too():
    assert shard_nodes.shard_of("test_e2e_isolation.py::test_x", 0, shard_nodes.N_SHARDS) == 0
    assert shard_nodes.shard_of("scripts/test_e2e_isolation.py::test_x", 1, shard_nodes.N_SHARDS) == 0


def test_ritual_spreads_across_the_round_robin_shards():
    nodes = [f"scripts/test_ritual.py::test_case_{i:02d}" for i in range(50)]
    for shard in range(1, shard_nodes.N_SHARDS):
        assert shard_nodes.nodes_for_shard(nodes, shard), f"shard {shard} got no ritual nodes"


def test_shard_zero_holds_only_the_pinned_file():
    nodes = sample_nodes()
    for node in shard_nodes.nodes_for_shard(nodes, 0):
        assert node.split("::")[0].endswith("test_e2e_isolation.py")


def test_bad_specs_fail_with_the_valid_range():
    for bad in ("", "x", "-1", str(shard_nodes.N_SHARDS), f"1/{shard_nodes.N_SHARDS + 1}"):
        with pytest.raises(ValueError, match="0"):
            shard_nodes.parse_spec(bad)


def test_each_test_file_on_disk_is_in_exactly_one_shard():
    """The ticket's file check: no scripts/test_*.py or tools/test_*.py file
    lands in zero shards or in two."""
    files = sorted(ROOT.glob("scripts/test_*.py")) + sorted(ROOT.glob("tools/test_*.py"))
    assert files, "no test files found"
    for file in files:
        probe = f"{file.relative_to(ROOT).as_posix()}::probe"
        hits = [shard for shard in range(shard_nodes.N_SHARDS) if probe in shard_nodes.nodes_for_shard([probe], shard)]
        assert hits and len(hits) == 1, f"{file.name} is in shards {hits}"


def test_pin_target_exists_on_disk():
    assert (ROOT / shard_nodes.E2E_ISOLATION_FILE).is_file()


def test_ci_matrix_matches_the_shard_count():
    """The ci.yml scripts matrix must list exactly shards 0 to N-1."""
    text = (ROOT / ".github" / "workflows" / "ci.yml").read_text()
    match = re.search(r"shard:\s*\[([0-9,\s]+)\]", text)
    assert match, "no shard matrix in ci.yml"
    assert [int(n) for n in match.group(1).split(",")] == list(range(shard_nodes.N_SHARDS))


def collect_one_file():
    """The node ids pytest collects for a tiny file, without running them."""
    result = subprocess.run(
        [sys.executable, "-m", "pytest", "scripts/test_core_order.py", "--collect-only"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    return [line for line in result.stdout.splitlines() if "::" in line]


def test_bad_shard_flag_fails_precisely():
    result = subprocess.run(
        [sys.executable, "-m", "pytest", "scripts/test_core_order.py", "--collect-only", "--shard=9"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    assert result.returncode == 4, result.stdout + result.stderr
    assert "bad shard '9'" in result.stderr, result.stdout + result.stderr


def test_wiring_covers_every_collected_node_exactly_once():
    full = collect_one_file()
    assert full, "collection found no nodes"
    covered = []
    for shard in range(shard_nodes.N_SHARDS):
        # Exit 5 means the shard holds nothing of this file, which is correct
        # for shard 0: the pin lives elsewhere.
        result = subprocess.run(
            [sys.executable, "-m", "pytest", "scripts/test_core_order.py", "--collect-only", f"--shard={shard}"],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        assert result.returncode in (0, 5), result.stdout + result.stderr
        got = [line for line in result.stdout.splitlines() if "::" in line]
        assert got == shard_nodes.nodes_for_shard(full, shard)
        covered += got
    assert sorted(covered) == sorted(full)
