import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def run(args: list[str], cwd: Path, tmp_path: Path) -> subprocess.CompletedProcess:
    env = {"PATH": "/usr/bin:/bin:/usr/local/bin", "HOME": str(tmp_path), "R2S_BUILD_DIR": str(tmp_path / "b")}
    return subprocess.run(args, cwd=cwd, env=env, capture_output=True, text=True)


def test_core_test_rejects_unknown_configurations_from_any_directory(tmp_path):
    for cwd, script in ((ROOT, "scripts/core-test.sh"), (ROOT / "scripts", "core-test.sh"), (ROOT.parent, None)):
        path = script or str(ROOT / "scripts/core-test.sh")
        result = run(["bash", path, "bogus"], cwd, tmp_path)
        assert result.returncode == 1, (cwd, result.stdout + result.stderr)
        assert "unknown configuration bogus" in result.stdout


def test_c_format_skips_vendored_files_and_checks_the_rest(tmp_path):
    vendor = tmp_path / "vendor"
    vendor.mkdir()
    bad = "int  main( void ){return 0;}\n"
    (vendor / "x.c").write_text(bad)
    (tmp_path / "y.c").write_text(bad)
    assert run(["bash", str(ROOT / "scripts/c-format.sh"), str(vendor / "x.c")], ROOT, tmp_path).returncode == 0
    assert run(["bash", str(ROOT / "scripts/c-format.sh"), str(tmp_path / "y.c")], ROOT, tmp_path).returncode != 0
