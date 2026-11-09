"""The isolation log collector gathers what the pair leaves behind.

scripts/collect-isolation-logs.sh copies per-scenario logs and full
section outputs from the cache build dirs into one directory, so the
nightly workflow can upload them. Missing inputs are fine: a section
that never built leaves nothing to copy.
"""

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "scripts" / "collect-isolation-logs.sh"


def collect(cache, dest):
    """Run the collector, failing the test when it fails."""
    proc = subprocess.run(["bash", str(SCRIPT), str(cache), str(dest)], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    return proc


def test_missing_cache_dir_leaves_an_empty_dest(tmp_path):
    dest = tmp_path / "out"
    collect(tmp_path / "no-such-cache", dest)
    assert dest.is_dir()
    assert list(dest.iterdir()) == []


def test_empty_build_dirs_copy_nothing(tmp_path):
    cache = tmp_path / "cache"
    (cache / "build-tree" / "e2e" / "posix" / "scenario-logs").mkdir(parents=True)
    dest = tmp_path / "out"
    collect(cache, dest)
    assert dest.is_dir()
    assert list(dest.iterdir()) == []


def test_scenario_logs_land_namespaced_by_build_dir(tmp_path):
    cache = tmp_path / "cache"
    logs = cache / "build-tree-a-iso-a" / "e2e" / "posix" / "scenario-logs"
    logs.mkdir(parents=True)
    (logs / "static.rmw_fastrtps_cpp.test_scenario_2_posix.log").write_text("boom\n")
    other = cache / "build-tree-a-iso-b" / "e2e" / "avr" / "scenario-logs"
    other.mkdir(parents=True)
    (other / "uno.rmw_zenoh_cpp.test_scenario_3_avr.log").write_text("avr boom\n")
    dest = tmp_path / "out"
    collect(cache, dest)
    posix = dest / "build-tree-a-iso-a" / "static.rmw_fastrtps_cpp.test_scenario_2_posix.log"
    assert posix.is_file()
    assert posix.read_text() == "boom\n"
    avr = dest / "build-tree-a-iso-b" / "uno.rmw_zenoh_cpp.test_scenario_3_avr.log"
    assert avr.is_file()
    assert avr.read_text() == "avr boom\n"


def test_section_logs_land_at_the_top(tmp_path):
    cache = tmp_path / "cache"
    section = cache / "e2e-section-logs"
    section.mkdir(parents=True)
    (section / "posix-pair-a.log").write_text("section says hi\n")
    dest = tmp_path / "out"
    collect(cache, dest)
    copied = dest / "posix-pair-a.log"
    assert copied.is_file()
    assert copied.read_text() == "section says hi\n"
