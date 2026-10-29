"""ci-test.sh is the single suite script: it must pass on the tree and fail on a lint error.

Runs the script on a copy of the tracked files, scoped to LINT=1 so the
suite never re-enters this test.
"""

import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def copy_tree(dest: Path) -> Path:
    """Copy the working tree as git sees it: tracked plus untracked, minus ignored."""
    listed = subprocess.run(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
        cwd=ROOT,
        capture_output=True,
        check=True,
    ).stdout
    for name in filter(None, listed.decode().split("\0")):
        src = ROOT / name
        if src.is_symlink():
            # A link copied as a file is a different tree, and the layout gate
            # checks the rulebook alias for being a link.
            (dest / name).parent.mkdir(parents=True, exist_ok=True)
            (dest / name).symlink_to(os.readlink(src))
        elif src.is_file():
            (dest / name).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dest / name)
    return dest


SCOPES = (
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
)


def run_lint(root: Path) -> subprocess.CompletedProcess:
    # Keep the caller's tools (CI installs them outside the system PATH), use this
    # interpreter's directory first, and drop inherited scopes so the nested run is
    # LINT only and never re-enters this test.
    env = {k: v for k, v in os.environ.items() if k not in SCOPES}
    env["PATH"] = os.pathsep.join([str(Path(sys.executable).parent), env.get("PATH", "")])
    env["LINT"] = "1"
    return subprocess.run(["bash", "scripts/ci-test.sh"], cwd=root, capture_output=True, text=True, env=env)


def test_lint_passes_on_the_tree(tmp_path):
    result = run_lint(copy_tree(tmp_path))
    assert result.returncode == 0, result.stdout + result.stderr


def test_the_suite_does_not_pass_test_down_to_its_own_tests(tmp_path):
    """The script tests call the ritual, and a ritual that sees TEST=1 runs
    the suite again, so TEST must not reach them.

    "make test TEST=1" forwards TEST into the container, so the suite has to
    drop it before it starts pytest, or the pair forks there as well.
    """
    seen = tmp_path / "seen.txt"
    # A tree with the suite, the script it sources, and one test that records
    # the environment it was started with. The test writes to a file, because
    # pytest swallows what a test prints. Nothing else is in the tree, so the
    # suite collects the probe and nothing else.
    copy = tmp_path / "copy"
    (copy / "scripts").mkdir(parents=True)
    (copy / "tools").mkdir()
    for name in ("ci-test.sh", "clean-git-env.sh", "suite-lock.sh"):
        shutil.copy2(ROOT / "scripts" / name, copy / "scripts" / name)
    (copy / "scripts" / "test_probe_env.py").write_text(
        '"""Records the environment the script tests are started with."""\n'
        "\n"
        "import os\n"
        "\n"
        "\n"
        "def test_the_environment():\n"
        f"    with open({str(seen)!r}, 'w') as f:\n"
        "        f.write(os.environ.get('TEST', ''))\n"
    )
    env = {**os.environ, "TEST": "1", "SCRIPTS": "1"}
    env["PATH"] = os.pathsep.join([str(Path(sys.executable).parent), env.get("PATH", "")])
    result = subprocess.run(
        ["bash", "scripts/ci-test.sh"],
        cwd=copy,
        capture_output=True,
        text=True,
        env=env,
        timeout=600,
    )
    assert seen.exists(), result.stdout + result.stderr
    assert seen.read_text() == "", "TEST reached the script tests"


def test_lint_fails_on_a_python_lint_error(tmp_path):
    root = copy_tree(tmp_path)
    (root / "scripts" / "broken.py").write_text("import os\n")
    result = run_lint(root)
    assert result.returncode != 0
    assert "broken.py" in result.stdout + result.stderr


def test_lint_fails_on_a_layout_problem(tmp_path):
    root = copy_tree(tmp_path)
    (root / "proposal-v4.md").write_text("")
    result = run_lint(root)
    assert result.returncode != 0
    assert "proposal-v4.md" in result.stdout + result.stderr


def run_scope(
    root: Path, scope: str, stubs: Path | None = None, build_dir: Path | None = None
) -> subprocess.CompletedProcess:
    """Run ci-test.sh on the copy with only `scope` set; stubs shadow the example toolchain."""
    env = {k: v for k, v in os.environ.items() if k not in SCOPES}
    env["PATH"] = os.pathsep.join([str(Path(sys.executable).parent), env.get("PATH", "")])
    env[scope] = "1"
    if stubs is not None:
        env["PATH"] = os.pathsep.join([str(stubs), env["PATH"]])
    if build_dir is not None:
        env["R2S_BUILD_DIR"] = str(build_dir)
    return subprocess.run(["bash", "scripts/ci-test.sh"], cwd=root, capture_output=True, text=True, env=env)


def stub_toolbox(path: Path) -> Path:
    """Fake the example toolchain: every build step succeeds and the ELF carries the runtime symbols.

    The runtime sketch's firmware is checked for the runtime topic table and
    the subscriber records, and the static one for the generated receive
    buffer, so the stub's nm prints all three.
    """
    path.mkdir(parents=True, exist_ok=True)
    (path / "cmake").write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
    (path / "arduino-cli").write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
    (path / "avr-size").write_text(
        "#!/usr/bin/env bash\n"
        "printf '   text\\t   data\\t    bss\\t    dec\\t    hex\\tfilename\\n"
        "   8000\\t    100\\t    200\\t   8300\\t   206c\\tfirmware.elf\\n'\n",
        encoding="utf-8",
    )
    (path / "avr-objdump").write_text(
        "#!/usr/bin/env bash\n"
        "echo '00000100 <main>:'\n"
        "echo ' 100:\t1c d1 \trcall\t.+56\t; 0x13a <setup>'\n"
        "echo '0000013a <setup>:'\n"
        "echo ' 13a:\t2a d1 \trcall\t.+84\t; 0x190 <r2s_spin>'\n"
        "echo '00000190 <r2s_spin>:'\n"
        "echo ' 190:\t08 95 \tret'\n",
        encoding="utf-8",
    )
    (path / "avr-nm").write_text(
        "#!/usr/bin/env bash\n"
        "echo '008001eb b r2s_rx_buf'\n"
        "echo '00800460 000000e0 b r2s_runtime_topics'\n"
        "echo '00800410 00000050 b r2s_runtime_subscribers'\n"
        "echo '00800540 00000050 b r2s_runtime_publishers'\n",
        encoding="utf-8",
    )
    for tool in ("cmake", "arduino-cli", "avr-size", "avr-objdump", "avr-nm"):
        (path / tool).chmod(0o755)
    return path


def test_docs_scope_runs_its_section(tmp_path):
    result = run_scope(copy_tree(tmp_path), "DOCS")

    assert result.returncode == 0, result.stdout + result.stderr
    assert "== docs" in result.stdout


def test_examples_scope_runs_its_section(tmp_path):
    result = run_scope(
        copy_tree(tmp_path), "EXAMPLES", stubs=stub_toolbox(tmp_path / "bin"), build_dir=tmp_path / "build"
    )

    assert result.returncode == 0, result.stdout + result.stderr
    assert "== examples" in result.stdout


def test_budgets_scope_runs_its_section(tmp_path):
    root = copy_tree(tmp_path)
    build = tmp_path / "build" / "examples"
    shipped = [
        "cmake_minimal/hello_cmake",
        "arduino_minimal/uno/arduino_minimal.ino.elf",
        "arduino_minimal/mega/arduino_minimal.ino.elf",
        "arduino_xmacro/uno/arduino_xmacro.ino.elf",
        "arduino_xmacro/mega/arduino_xmacro.ino.elf",
    ]
    for rel in shipped:
        fake = build / rel
        fake.parent.mkdir(parents=True, exist_ok=True)
        fake.write_bytes(b"")
    (build / "cmake_minimal" / "hello_cmake").chmod(0o755)
    su = "main:32:static\nsetup:16:static\nr2s_spin:64:static\n"
    (build / "cmake_minimal" / "firmware.su").write_text(su)
    for rel in shipped:
        if not rel.startswith("cmake"):
            source = build / "stack" / rel
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_bytes(b"")
            (source.parent / "firmware.su").write_text(su)
    result = run_scope(root, "BUDGETS", stubs=stub_toolbox(tmp_path / "bin"), build_dir=tmp_path / "build")

    assert result.returncode == 0, result.stdout + result.stderr
    assert "== budgets" in result.stdout


def test_fuzz_scope_runs_its_section(tmp_path, monkeypatch):
    # Seeds only: the run still builds every target and runs each corpus.
    monkeypatch.setenv("R2S_FUZZ_SECONDS", "0")
    result = run_scope(copy_tree(tmp_path), "FUZZ", build_dir=tmp_path / "build")
    assert result.returncode == 0, result.stdout + result.stderr
    assert "== fuzz" in result.stdout


def _minimal_suite(copy: Path) -> Path:
    """A tree with the suite and the scripts it sources, running no repo sections."""
    (copy / "scripts").mkdir(parents=True)
    (copy / "tools").mkdir()
    for name in ("ci-test.sh", "clean-git-env.sh", "suite-lock.sh"):
        shutil.copy2(ROOT / "scripts" / name, copy / "scripts" / name)
    return copy


def test_scoped_run_with_no_section_fails_naming_scope(tmp_path):
    """A scope with no section must fail: an empty scoped run passes today."""
    copy = _minimal_suite(tmp_path / "copy")
    script = copy / "scripts" / "ci-test.sh"
    text = script.read_text()
    docs_line = [line for line in text.splitlines() if "wants DOCS" in line]
    assert docs_line, "the probe needs a DOCS block to remove"
    script.write_text(text.replace(docs_line[0] + "\n", ""))
    env = {k: v for k, v in os.environ.items() if k not in SCOPES}
    env["PATH"] = os.pathsep.join([str(Path(sys.executable).parent), env.get("PATH", "")])
    env["DOCS"] = "1"
    result = subprocess.run(
        ["bash", "scripts/ci-test.sh"],
        cwd=copy,
        capture_output=True,
        text=True,
        env=env,
        timeout=600,
    )
    assert result.returncode != 0
    assert "DOCS" in result.stdout + result.stderr


def test_scoped_run_names_an_empty_scope_beside_a_running_one(tmp_path):
    """A removed section must fail even when another scope in the same run passes."""
    copy = _minimal_suite(tmp_path / "copy")
    script = copy / "scripts" / "ci-test.sh"
    text = script.read_text()
    docs_line = [line for line in text.splitlines() if "wants DOCS" in line]
    assert docs_line, "the probe needs a DOCS block to remove"
    script.write_text(text.replace(docs_line[0] + "\n", ""))
    (copy / "scripts" / "core-test.sh").write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
    env = {k: v for k, v in os.environ.items() if k not in SCOPES}
    env["PATH"] = os.pathsep.join([str(Path(sys.executable).parent), env.get("PATH", "")])
    env["DOCS"] = "1"
    env["CORE"] = "1"
    result = subprocess.run(
        ["bash", "scripts/ci-test.sh"],
        cwd=copy,
        capture_output=True,
        text=True,
        env=env,
        timeout=600,
    )
    assert result.returncode != 0
    assert "DOCS" in result.stdout + result.stderr


def test_every_header_scope_runs_a_section():
    """Each scope the header comment advertises must gate a section."""
    text = (ROOT / "scripts" / "ci-test.sh").read_text()
    # The header names the scopes over two lines, and the second one starts
    # with whichever scope comes first, so every line that opens with a
    # scope name is part of the pin.
    header = "\n".join(line for line in text.splitlines() if "Scope with" in line or re.match(r"^# [A-Z]{2,}", line))
    scopes = re.findall(r"\b([A-Z][A-Z0-9]*)(?:=1|=<backend>)", header)
    assert "E2E" in scopes, "the header pin must cover E2E"
    assert scopes, "no scopes found in the header comment"
    missing = [scope for scope in scopes if f"wants {scope}" not in text]
    assert not missing, f"header scopes with no section: {missing}"


def test_e2e_both_backend_runs_all_three_suites(tmp_path):
    """E2E=both runs the harness, the avr and the posix end-to-end suites in one scope."""
    copy = _minimal_suite(tmp_path / "copy")
    (copy / "scripts" / "core-test.sh").write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
    (copy / "tests" / "e2e").mkdir(parents=True)
    (copy / "tests" / "harness" / "simavr").mkdir(parents=True)
    (copy / "tests" / "e2e" / "run.sh").write_text('#!/usr/bin/env bash\necho "posix ran"\n', encoding="utf-8")
    (copy / "tests" / "e2e" / "run-avr.sh").write_text('#!/usr/bin/env bash\necho "avr ran"\n', encoding="utf-8")
    (copy / "tests" / "harness" / "simavr" / "run.sh").write_text(
        '#!/usr/bin/env bash\necho "harness ran"\n', encoding="utf-8"
    )
    env = {k: v for k, v in os.environ.items() if k not in SCOPES}
    env["PATH"] = os.pathsep.join([str(Path(sys.executable).parent), env.get("PATH", "")])
    env["E2E"] = "both"
    result = subprocess.run(
        ["bash", "scripts/ci-test.sh"],
        cwd=copy,
        capture_output=True,
        text=True,
        env=env,
        timeout=600,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert "== e2e-avr-harness" in result.stdout
    assert "== e2e-avr" in result.stdout
    assert "== e2e-posix" in result.stdout
