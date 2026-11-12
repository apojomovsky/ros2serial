# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

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
        ['git', 'ls-files', '-z', '--cached', '--others', '--exclude-standard'],
        cwd=ROOT,
        capture_output=True,
        check=True,
    ).stdout
    for name in filter(None, listed.decode().split('\0')):
        src = ROOT / name
        if src.is_symlink():
            # A link copied as a file is a different tree, and the layout gate
            # checks the rulebook alias for being a link.
            (dest / name).parent.mkdir(parents=True, exist_ok=True)
            (dest / name).symlink_to(os.readlink(src))
        elif src.is_file():
            (dest / name).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dest / name)
    # A repository, not loose files: the taskfile-coverage gate reads the
    # tracked set, as it does on the real tree.
    identity = ['-c', 'user.email=t@example.com', '-c', 'user.name=t']
    subprocess.run(['git', 'init', '-q', '-b', 'master'], cwd=dest, check=True)
    subprocess.run(['git', *identity, 'add', '-A'], cwd=dest, check=True)
    subprocess.run(['git', *identity, 'commit', '-qm', 'tree'], cwd=dest, check=True)
    return dest


SCOPES = (
    'LINT',
    'SCRIPTS',
    'COMMITS',
    'CORE',
    'GEN',
    'BRIDGE',
    'AMENT_LINT',
    'CPP',
    'E2E',
    'COVERAGE',
    'DOCS',
    'EXAMPLES',
    'ESP32',
    'PIC',
    'PLATFORMIO',
    'STM32',
    'RP2040',
    'BUDGETS',
    'FUZZ',
    'README',
    'NIGHTLY',
    'SOAK',
    'SCRIPTS_SHARD',
)


def run_lint(root: Path) -> subprocess.CompletedProcess:
    # Keep the caller's tools (CI installs them outside the system PATH), use this
    # interpreter's directory first, and drop inherited scopes so the nested run is
    # LINT only and never re-enters this test.
    env = {k: v for k, v in os.environ.items() if k not in SCOPES}
    env['PATH'] = os.pathsep.join([str(Path(sys.executable).parent), env.get('PATH', '')])
    env['LINT'] = '1'
    return subprocess.run(
        ['bash', 'scripts/ci-test.sh'], cwd=root, capture_output=True, text=True, env=env
    )


def test_lint_passes_on_the_tree(tmp_path):
    result = run_lint(copy_tree(tmp_path))
    assert result.returncode == 0, result.stdout + result.stderr


def test_the_suite_does_not_pass_test_down_to_its_own_tests(tmp_path):
    """The script tests call the ritual, and a ritual that sees TEST=1 runs
    the suite again, so TEST must not reach them.

    "make test TEST=1" forwards TEST into the container, so the suite has to
    drop it before it starts pytest, or the pair forks there as well.
    """
    seen = tmp_path / 'seen.txt'
    # A tree with the suite, the script it sources, and one test that records
    # the environment it was started with. The test writes to a file, because
    # pytest swallows what a test prints. Nothing else is in the tree, so the
    # suite collects the probe and nothing else.
    copy = tmp_path / 'copy'
    (copy / 'scripts').mkdir(parents=True)
    (copy / 'tools').mkdir()
    for name in ('ci-test.sh', 'clean-git-env.sh', 'suite-lock.sh'):
        shutil.copy2(ROOT / 'scripts' / name, copy / 'scripts' / name)
    (copy / 'scripts' / 'test_probe_env.py').write_text(
        '"""Records the environment the script tests are started with."""\n'
        '\n'
        'import os\n'
        '\n'
        '\n'
        'def test_the_environment():\n'
        f"    with open({str(seen)!r}, 'w') as f:\n"
        "        f.write(os.environ.get('TEST', ''))\n"
    )
    env = {**os.environ, 'TEST': '1', 'SCRIPTS': '1'}
    env['PATH'] = os.pathsep.join([str(Path(sys.executable).parent), env.get('PATH', '')])
    result = subprocess.run(
        ['bash', 'scripts/ci-test.sh'],
        cwd=copy,
        capture_output=True,
        text=True,
        env=env,
        timeout=600,
    )
    assert seen.exists(), result.stdout + result.stderr
    assert seen.read_text() == '', 'TEST reached the script tests'


def test_the_suite_consumes_the_shard_before_starting_pytest():
    """SCRIPTS_SHARD selects the outer run's own shard, and the script tests
    start nested suite runs. The suite consumes the variable up front, the way
    it unsets TEST, so a nested run inherits only what its caller sets."""
    lines = (ROOT / 'scripts' / 'ci-test.sh').read_text().splitlines()
    capture_at = next(i for i, line in enumerate(lines) if line.startswith('scripts_shard='))
    unset_at = next(i for i, line in enumerate(lines) if line == 'unset SCRIPTS_SHARD')
    pytest_at = next(
        i for i, line in enumerate(lines) if 'python3 -m pytest scripts tools' in line
    )
    assert capture_at < unset_at < pytest_at, (
        'the shard must be captured and unset before pytest starts'
    )


def test_lint_fails_on_a_python_lint_error(tmp_path):
    root = copy_tree(tmp_path)
    (root / 'scripts' / 'broken.py').write_text('import os\n')
    result = run_lint(root)
    assert result.returncode != 0
    assert 'broken.py' in result.stdout + result.stderr


def test_lint_fails_on_a_layout_problem(tmp_path):
    root = copy_tree(tmp_path)
    (root / 'proposal-v4.md').write_text('')
    result = run_lint(root)
    assert result.returncode != 0
    assert 'proposal-v4.md' in result.stdout + result.stderr


def run_scope(
    root: Path, scope: str, stubs: Path | None = None, build_dir: Path | None = None
) -> subprocess.CompletedProcess:
    """Run ci-test.sh on the copy with only `scope` set; stubs shadow the example toolchain."""
    env = {k: v for k, v in os.environ.items() if k not in SCOPES}
    env['PATH'] = os.pathsep.join([str(Path(sys.executable).parent), env.get('PATH', '')])
    env[scope] = '1'
    if stubs is not None:
        env['PATH'] = os.pathsep.join([str(stubs), env['PATH']])
    if build_dir is not None:
        env['R2S_BUILD_DIR'] = str(build_dir)
    return subprocess.run(
        ['bash', 'scripts/ci-test.sh'], cwd=root, capture_output=True, text=True, env=env
    )


def stub_toolbox(path: Path) -> Path:
    """Fake the example toolchain: every build step succeeds and the ELF carries its symbols.

    The runtime sketches' firmware is checked for the runtime topic table and
    the subscriber records, the action sketch for the action table, the
    service sketch for the service table, and the static ones for the
    generated receive buffer, so the stub's nm prints the receive buffer
    always and the runtime symbols only for the sketches whose paths name
    them: the static firmware must not link the runtime table.
    """
    path.mkdir(parents=True, exist_ok=True)
    (path / 'cmake').write_text('#!/usr/bin/env bash\nexit 0\n', encoding='utf-8')
    # The stubbed core install plants a stub Xtensa toolchain where the real
    # install would put it, so the build finds its binutils. The size tool
    # only needs to exist: the stubbed compiles produce no firmware for the
    # budgets step to measure.
    (path / 'arduino-cli').write_text(
        '#!/usr/bin/env bash\n'
        'if [ "${1:-}" = core ] && [ "${2:-}" = install ]; then\n'
        '    bindir="$ARDUINO_DIRECTORIES_DATA/packages/esp32/tools/'
        'xtensa-esp32-elf-gcc/0.0.0/bin"\n'
        '    mkdir -p "$bindir"\n'
        '    ln -sf "$(command -v xtensa-esp32-elf-nm)" "$bindir/xtensa-esp32-elf-nm"\n'
        '    printf "#!/usr/bin/env bash\\nexit 0\\n" > "$bindir/xtensa-esp32-elf-size"\n'
        '    chmod +x "$bindir/xtensa-esp32-elf-size"\n'
        'fi\n'
        'exit 0\n',
        encoding='utf-8',
    )
    (path / 'avr-size').write_text(
        '#!/usr/bin/env bash\n'
        "printf '   text\\t   data\\t    bss\\t    dec\\t    hex\\tfilename\\n"
        "   8000\\t    100\\t    200\\t   8300\\t   206c\\tfirmware.elf\\n'\n",
        encoding='utf-8',
    )
    (path / 'avr-objdump').write_text(
        '#!/usr/bin/env bash\n'
        "echo '00000100 <main>:'\n"
        "echo ' 100:\t1c d1 \trcall\t.+56\t; 0x13a <setup>'\n"
        "echo '0000013a <setup>:'\n"
        "echo ' 13a:\t2a d1 \trcall\t.+84\t; 0x190 <r2s_spin>'\n"
        "echo '00000190 <r2s_spin>:'\n"
        "echo ' 190:\t08 95 \tret'\n",
        encoding='utf-8',
    )
    (path / 'avr-nm').write_text(
        '#!/usr/bin/env bash\n'
        "echo '008001eb b r2s_rx_buf'\n"
        'case "$*" in\n'
        '    *arduino_minimal*|*arduino_hello*|*arduino_blink*|*arduino_button*|'
        '*arduino_servo*|*arduino_range*)\n'
        "        echo '00800460 000000e0 b r2s_runtime_topics'\n"
        "        echo '00800410 00000050 b r2s_runtime_subscribers'\n"
        "        echo '00800540 00000050 b r2s_runtime_publishers'\n"
        '        ;;\n'
        '    *arduino_action*)\n'
        "        echo '00800460 00000030 b r2s_action_table'\n"
        "        echo '00800410 00000018 b r2s_action_servers'\n"
        "        echo '00800540 00000092 b fib'\n"
        '        ;;\n'
        '    *arduino_service*)\n'
        "        echo '00800460 00000030 b r2s_service_table'\n"
        "        echo '00800410 00000018 b r2s_service_servers'\n"
        "        echo '00800540 00000092 b reset_srv'\n"
        "        echo '00800560 00000010 b trigger_cli'\n"
        '        ;;\n'
        'esac\n',
        encoding='utf-8',
    )
    (path / 'xtensa-esp32-elf-nm').write_text(
        '#!/usr/bin/env bash\n'
        "echo '008001eb b r2s_rx_buf'\n"
        'case "$*" in\n'
        '    *arduino_minimal*|*arduino_hello*|*arduino_blink*|*arduino_button*|'
        '*arduino_servo*|*arduino_range*)\n'
        "        echo '00800460 000000e0 b r2s_runtime_topics'\n"
        "        echo '00800410 00000050 b r2s_runtime_subscribers'\n"
        "        echo '00800540 00000050 b r2s_runtime_publishers'\n"
        '        ;;\n'
        'esac\n',
        encoding='utf-8',
    )
    # The CH32V firmware builds with the RISC-V toolchain, which the stubbed
    # run lacks: the stubs exit 0 and the static firmware's buffer symbol
    # stands in for the linker, the way the AVR stubs stand in.
    (path / 'riscv-none-elf-gcc').write_text('#!/usr/bin/env bash\nexit 0\n', encoding='utf-8')
    (path / 'riscv-none-elf-size').write_text(
        '#!/usr/bin/env bash\n'
        "printf '   text\\t   data\\t    bss\\t    dec\\t    hex\\tfilename\\n"
        "   8000\\t    100\\t    200\\t   8300\\t   206c\\tfirmware.elf\\n'",
        encoding='utf-8',
    )
    (path / 'riscv-none-elf-nm').write_text(
        "#!/usr/bin/env bash\necho '008001eb b r2s_rx_buf'\necho '20000800 A _edata'\n",
        encoding='utf-8',
    )
    for tool in (
        'cmake',
        'arduino-cli',
        'avr-size',
        'avr-objdump',
        'avr-nm',
        'xtensa-esp32-elf-nm',
        'riscv-none-elf-gcc',
        'riscv-none-elf-size',
        'riscv-none-elf-nm',
    ):
        (path / tool).chmod(0o755)
    return path


def test_docs_scope_runs_its_section(tmp_path):
    result = run_scope(copy_tree(tmp_path), 'DOCS')

    assert result.returncode == 0, result.stdout + result.stderr
    assert '== docs' in result.stdout


def test_examples_scope_runs_its_section(tmp_path, monkeypatch):
    # The stubbed toolchain cannot produce runnable firmware, so the simavr
    # behavior leg stays off here; the real EXAMPLES run always proves it.
    monkeypatch.setenv('R2S_EXAMPLES_NO_SIMAVR', '1')
    result = run_scope(
        copy_tree(tmp_path),
        'EXAMPLES',
        stubs=stub_toolbox(tmp_path / 'bin'),
        build_dir=tmp_path / 'build',
    )

    assert result.returncode == 0, result.stdout + result.stderr
    assert '== examples' in result.stdout


def test_esp32_scope_runs_its_section(tmp_path, monkeypatch):
    # The stubbed core install plants its toolchain under the data dir, so
    # the run keeps a scratch directory instead of the shared cache.
    monkeypatch.setenv('R2S_ARDUINO_DATA_DIR', str(tmp_path / 'arduino-data'))
    result = run_scope(
        copy_tree(tmp_path),
        'ESP32',
        stubs=stub_toolbox(tmp_path / 'bin'),
        build_dir=tmp_path / 'build',
    )

    assert result.returncode == 0, result.stdout + result.stderr
    assert '== esp32' in result.stdout


def test_pic_scope_runs_its_section(tmp_path, monkeypatch):
    # The stubbed compiler satisfies the toolchain lookup, so the run
    # compiles nothing real and downloads nothing. A clean compile closes
    # every gap, so the run needs an empty baseline instead of the copy's.
    stub = tmp_path / 'bin'
    stub.mkdir()
    baseline = tmp_path / 'baseline.txt'
    baseline.write_text('# no known gaps\n')
    (stub / 'epic-cc').write_text('#!/usr/bin/env bash\nexit 0\n')
    (stub / 'epic-cc').chmod(0o755)
    monkeypatch.setenv('R2S_EPIC_CC_BIN', str(stub / 'epic-cc'))
    monkeypatch.setenv('R2S_PIC_CANARY_BASELINE', str(baseline))
    result = run_scope(copy_tree(tmp_path), 'PIC', build_dir=tmp_path / 'build')

    assert result.returncode == 0, result.stdout + result.stderr
    assert '== pic' in result.stdout


def test_platformio_scope_runs_its_section(tmp_path, monkeypatch):
    # The stubbed PlatformIO binary succeeds without downloads, so the run
    # keeps a scratch directory instead of the shared cache.
    stub = tmp_path / 'bin'
    stub.mkdir()
    # Mimics `pio ci`: one firmware per named board under .pio/build, so
    # the gate's artifact check runs against the same layout as the build.
    (stub / 'pio').write_text(
        '#!/usr/bin/env bash\n'
        'echo "$@" >> "$R2S_PIO_LOG"\n'
        'want_board=0\n'
        'lib=""\n'
        'for arg in "$@"; do\n'
        '  case "$arg" in\n'
        '  --board)\n'
        '    want_board=1\n'
        '    ;;\n'
        '  --board=*)\n'
        '    mkdir -p ".pio/build/${arg#--board=}"\n'
        '    : > ".pio/build/${arg#--board=}/firmware.elf"\n'
        '    ;;\n'
        '  --lib=*)\n'
        '    lib="${arg#--lib=}"\n'
        '    ;;\n'
        '  *)\n'
        '    if [ "$want_board" = 1 ]; then\n'
        '      mkdir -p ".pio/build/$arg"\n'
        '      : > ".pio/build/$arg/firmware.elf"\n'
        '      want_board=0\n'
        '    fi\n'
        '    ;;\n'
        '  esac\n'
        'done\n'
        '# Mimic verbose compile lines from the staged manifest, so the\n'
        '# gate proves the flags it staged reach the log it greps.\n'
        'if [ -n "$lib" ] && [ -f "$lib/library.json" ]; then\n'
        '  grep -o \'"-D[^"]*"\' "$lib/library.json" | tr -d \'"\' | while read -r flag; do\n'
        '    echo "avr-gcc $flag -c lib.c"\n'
        '  done\n'
        'fi\n'
        'exit 0\n'
    )
    (stub / 'pio').chmod(0o755)
    monkeypatch.setenv('R2S_PIO_BIN', str(stub / 'pio'))
    monkeypatch.setenv('R2S_PIO_LOG', str(tmp_path / 'pio.log'))
    monkeypatch.setenv('R2S_PIO_DATA_DIR', str(tmp_path / 'pio-data'))
    result = run_scope(
        copy_tree(tmp_path),
        'PLATFORMIO',
        stubs=stub_toolbox(tmp_path / 'bin'),
        build_dir=tmp_path / 'build',
    )

    assert result.returncode == 0, result.stdout + result.stderr
    assert '== platformio' in result.stdout


def test_stm32_scope_runs_its_section(tmp_path, monkeypatch):
    # The stubbed Arm toolchain satisfies the lookup and touches each
    # output, so the run links nothing real and downloads nothing.
    stub = tmp_path / 'bin'
    stub.mkdir()
    stub_arm_toolchain(stub)
    monkeypatch.setenv('R2S_ARM_GCC', str(stub / 'arm-none-eabi-gcc'))
    result = run_scope(copy_tree(tmp_path), 'STM32', build_dir=tmp_path / 'build')

    assert result.returncode == 0, result.stdout + result.stderr
    assert '== stm32' in result.stdout


def test_rp2040_scope_runs_its_section(tmp_path, monkeypatch):
    # The stubbed Arm toolchain satisfies the lookup and touches each
    # output, so the run links nothing real and downloads nothing. The
    # stub stage source stands in for the SDK boot stage.
    stub = tmp_path / 'bin'
    stub.mkdir()
    stub_arm_toolchain(stub)
    monkeypatch.setenv('R2S_ARM_GCC', str(stub / 'arm-none-eabi-gcc'))
    monkeypatch.setenv('R2S_ARM_OBJCOPY', str(stub / 'arm-none-eabi-objcopy'))
    sdk = tmp_path / 'sdk'
    fixture_sdk(sdk)
    monkeypatch.setenv('R2S_PICO_SDK', str(sdk))
    result = run_scope(copy_tree(tmp_path), 'RP2040', build_dir=tmp_path / 'build')

    assert result.returncode == 0, result.stdout + result.stderr
    assert '== rp2040' in result.stdout


def stub_arm_toolchain(stub):
    """A stub Arm toolchain: compiles and objcopy touch their outputs."""
    (stub / 'arm-none-eabi-gcc').write_text(
        '#!/usr/bin/env bash\n'
        'out=""\n'
        'prev=""\n'
        'for arg in "$@"; do\n'
        '    if [ "$prev" = "-o" ]; then out="$arg"; fi\n'
        '    prev="$arg"\n'
        'done\n'
        '[ -n "$out" ] && mkdir -p "$(dirname "$out")" && : > "$out"\n'
        'exit 0\n',
        encoding='utf-8',
    )
    (stub / 'arm-none-eabi-gcc').chmod(0o755)
    (stub / 'arm-none-eabi-objcopy').write_text(
        '#!/usr/bin/env bash\n: > "${@: -1}"\nexit 0\n', encoding='utf-8'
    )
    (stub / 'arm-none-eabi-objcopy').chmod(0o755)


def fixture_sdk(sdk_dir):
    """The boot stage tree the STM32 build reads: a stub stage stands in."""
    stage = sdk_dir / 'src' / 'rp2040' / 'boot_stage2'
    stage.mkdir(parents=True)
    (stage / 'boot2_w25q080.S').write_text(
        '.cpu cortex-m0plus\n.thumb\n.section .entry\n b .\n', encoding='utf-8'
    )
    (stage / 'boot_stage2.ld').write_text(
        'MEMORY { SRAM(rx) : ORIGIN = 0x20041f00, LENGTH = 252 }\n', encoding='utf-8'
    )
    pad = stage / 'pad_checksum'
    pad.write_text('#!/usr/bin/env bash\n: > "${@: -1}"\nexit 0\n')
    pad.chmod(0o755)
    (stage / 'asminclude').mkdir()


PORT_FIRMWARE = ('stm32/firmware.elf', 'rp2040/firmware.elf')


def plant_port_stack(build, su):
    """The stack sources the gate needs beside the stub-linked firmware."""
    for rel in PORT_FIRMWARE:
        source = build / 'stack' / rel
        source.parent.mkdir(parents=True, exist_ok=True)
        source.write_bytes(b'')
        (source.parent / 'firmware.su').write_text(su)


def test_budgets_scope_runs_its_section(tmp_path, monkeypatch):
    # The budgets run builds the ESP32 firmware too, whose stubbed install
    # plants its toolchain under the data dir: keep a scratch directory.
    # The STM32 firmware builds with the stubbed Arm toolchain and SDK.
    monkeypatch.setenv('R2S_ARDUINO_DATA_DIR', str(tmp_path / 'arduino-data'))
    stub = tmp_path / 'bin'
    stub.mkdir()
    stub_arm_toolchain(stub)
    monkeypatch.setenv('R2S_ARM_GCC', str(stub / 'arm-none-eabi-gcc'))
    monkeypatch.setenv('R2S_ARM_OBJCOPY', str(stub / 'arm-none-eabi-objcopy'))
    sdk = tmp_path / 'sdk'
    fixture_sdk(sdk)
    monkeypatch.setenv('R2S_PICO_SDK', str(sdk))
    root = copy_tree(tmp_path)
    stubs = stub_toolbox(stub)
    build = tmp_path / 'build' / 'examples'
    shipped = [
        'cmake_minimal/hello_cmake',
        'ch32v_minimal/ch32v/ch32v_minimal.elf',
        'arduino_minimal/uno/arduino_minimal.ino.elf',
        'arduino_minimal/mega/arduino_minimal.ino.elf',
        'arduino_xmacro/uno/arduino_xmacro.ino.elf',
        'arduino_xmacro/mega/arduino_xmacro.ino.elf',
        'arduino_static/uno/arduino_static.ino.elf',
        'arduino_static/mega/arduino_static.ino.elf',
    ]
    for rel in shipped:
        fake = build / rel
        fake.parent.mkdir(parents=True, exist_ok=True)
        fake.write_bytes(b'')
    (build / 'cmake_minimal' / 'hello_cmake').chmod(0o755)
    su = 'main:32:static\nsetup:16:static\nr2s_spin:64:static\n'
    (build / 'cmake_minimal' / 'firmware.su').write_text(su)
    for rel in shipped:
        if not rel.startswith('cmake'):
            source = build / 'stack' / rel
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_bytes(b'')
            (source.parent / 'firmware.su').write_text(su)
    plant_port_stack(build, su)
    # The gate refuses a stale docs/budgets.md, so the run first refreshes
    # it from the stubbed measurements, the way make size-report does.
    env = {k: v for k, v in os.environ.items() if k not in SCOPES}
    env['PATH'] = os.pathsep.join(
        [str(stubs), str(Path(sys.executable).parent), env.get('PATH', '')]
    )
    env['R2S_BUILD_DIR'] = str(tmp_path / 'build')
    report = subprocess.run(
        ['bash', 'scripts/budgets-check.sh', '--report'],
        cwd=root,
        capture_output=True,
        text=True,
        env=env,
    )
    assert report.returncode == 0, report.stdout + report.stderr
    result = run_scope(root, 'BUDGETS', stubs=stubs, build_dir=tmp_path / 'build')

    assert result.returncode == 0, result.stdout + result.stderr
    assert '== budgets' in result.stdout


def test_budgets_gate_fails_on_stale_docs(tmp_path, monkeypatch):
    # The checked-in docs/budgets.md holds real measurements, so a run
    # over stubbed firmware must fail naming the stale copy. The STM32
    # firmware builds with the stubbed Arm toolchain and SDK, and its
    # stack sources are planted so the failure stays about the docs.
    monkeypatch.setenv('R2S_ARDUINO_DATA_DIR', str(tmp_path / 'arduino-data'))
    stub = tmp_path / 'bin'
    stub.mkdir()
    stub_arm_toolchain(stub)
    monkeypatch.setenv('R2S_ARM_GCC', str(stub / 'arm-none-eabi-gcc'))
    monkeypatch.setenv('R2S_ARM_OBJCOPY', str(stub / 'arm-none-eabi-objcopy'))
    sdk = tmp_path / 'sdk'
    fixture_sdk(sdk)
    monkeypatch.setenv('R2S_PICO_SDK', str(sdk))
    root = copy_tree(tmp_path)
    build = tmp_path / 'build' / 'examples'
    fake = build / 'cmake_minimal' / 'hello_cmake'
    fake.parent.mkdir(parents=True, exist_ok=True)
    fake.write_bytes(b'')
    fake.chmod(0o755)
    (build / 'cmake_minimal' / 'firmware.su').write_text('main:32:static\n')
    plant_port_stack(build, 'main:32:static\n')
    # The gate builds every example, so the CH32V firmware needs its stub
    # too before the run can reach the stale copy: the helper above only
    # plants the STM32 and RP2040 stack sources.
    ch32v = build / 'ch32v_minimal' / 'ch32v' / 'ch32v_minimal.elf'
    ch32v.parent.mkdir(parents=True, exist_ok=True)
    ch32v.write_bytes(b'')
    (build / 'ch32v_minimal' / 'ch32v' / 'firmware.su').write_text('main:32:static\n')
    result = run_scope(
        root, 'BUDGETS', stubs=stub_toolbox(tmp_path / 'bin'), build_dir=tmp_path / 'build'
    )

    assert result.returncode != 0
    assert 'docs/budgets.md is stale' in result.stdout + result.stderr


def test_examples_builds_isolate_their_build_paths():
    """Every sketch build keeps its own arduino build path: the shared cache
    once leaked the e2e slice serial flags into a sketch without one."""
    text = (ROOT / 'scripts' / 'build-examples.sh').read_text()
    joined = re.sub(r'\\\n', ' ', text)
    compiles = [line for line in joined.splitlines() if 'arduino-cli compile' in line]
    assert compiles, 'no arduino-cli compile line left to isolate'
    for line in compiles:
        assert '--build-path' in line, f'sketch build shares the arduino cache:\n{line.strip()}'


def test_fuzz_scope_runs_its_section(tmp_path, monkeypatch):
    # Seeds only: the run still builds every target and runs each corpus.
    monkeypatch.setenv('R2S_FUZZ_SECONDS', '0')
    result = run_scope(copy_tree(tmp_path), 'FUZZ', build_dir=tmp_path / 'build')
    assert result.returncode == 0, result.stdout + result.stderr
    assert '== fuzz' in result.stdout


def _minimal_suite(copy: Path) -> Path:
    """A tree with the suite and the scripts it sources, running no repo sections."""
    (copy / 'scripts').mkdir(parents=True)
    (copy / 'tools').mkdir()
    for name in ('ci-test.sh', 'clean-git-env.sh', 'suite-lock.sh'):
        shutil.copy2(ROOT / 'scripts' / name, copy / 'scripts' / name)
    return copy


def test_scoped_run_with_no_section_fails_naming_scope(tmp_path):
    """A scope with no section must fail: an empty scoped run passes today."""
    copy = _minimal_suite(tmp_path / 'copy')
    script = copy / 'scripts' / 'ci-test.sh'
    text = script.read_text()
    docs_line = [line for line in text.splitlines() if 'wants DOCS' in line]
    assert docs_line, 'the probe needs a DOCS block to remove'
    script.write_text(text.replace(docs_line[0] + '\n', ''))
    env = {k: v for k, v in os.environ.items() if k not in SCOPES}
    env['PATH'] = os.pathsep.join([str(Path(sys.executable).parent), env.get('PATH', '')])
    env['DOCS'] = '1'
    result = subprocess.run(
        ['bash', 'scripts/ci-test.sh'],
        cwd=copy,
        capture_output=True,
        text=True,
        env=env,
        timeout=600,
    )
    assert result.returncode != 0
    assert 'DOCS' in result.stdout + result.stderr


def test_scoped_run_names_an_empty_scope_beside_a_running_one(tmp_path):
    """A removed section must fail even when another scope in the same run passes."""
    copy = _minimal_suite(tmp_path / 'copy')
    script = copy / 'scripts' / 'ci-test.sh'
    text = script.read_text()
    docs_line = [line for line in text.splitlines() if 'wants DOCS' in line]
    assert docs_line, 'the probe needs a DOCS block to remove'
    script.write_text(text.replace(docs_line[0] + '\n', ''))
    (copy / 'scripts' / 'core-test.sh').write_text(
        '#!/usr/bin/env bash\nexit 0\n', encoding='utf-8'
    )
    env = {k: v for k, v in os.environ.items() if k not in SCOPES}
    env['PATH'] = os.pathsep.join([str(Path(sys.executable).parent), env.get('PATH', '')])
    env['DOCS'] = '1'
    env['CORE'] = '1'
    result = subprocess.run(
        ['bash', 'scripts/ci-test.sh'],
        cwd=copy,
        capture_output=True,
        text=True,
        env=env,
        timeout=600,
    )
    assert result.returncode != 0
    assert 'DOCS' in result.stdout + result.stderr


def test_every_header_scope_runs_a_section():
    """Each scope the header comment advertises must gate a section."""
    text = (ROOT / 'scripts' / 'ci-test.sh').read_text()
    # The header names the scopes over two lines, and the second one starts
    # with whichever scope comes first, so every line that opens with a
    # scope name is part of the pin.
    header = '\n'.join(
        line
        for line in text.splitlines()
        if 'Scope with' in line or re.match(r'^# [A-Z]{2,}', line)
    )
    scopes = re.findall(r'\b([A-Z][A-Z0-9]*)(?:=1|=<backend>)', header)
    assert 'E2E' in scopes, 'the header pin must cover E2E'
    assert scopes, 'no scopes found in the header comment'
    missing = [scope for scope in scopes if f'wants {scope}' not in text]
    assert not missing, f'header scopes with no section: {missing}'


def test_nightly_scope_names_its_section():
    """NIGHTLY runs the isolation pairs file as its own section, not a suite."""
    text = (ROOT / 'scripts' / 'ci-test.sh').read_text()
    assert 'wants NIGHTLY' in text
    assert 'tests/e2e/test_isolation.py' in text


def test_nightly_section_shows_pytest_output_live():
    """The isolation wall-time lines must reach the job log, not pytest's capture."""
    text = (ROOT / 'scripts' / 'ci-test.sh').read_text()
    assert re.search(r'pytest -s tests/e2e/test_isolation\.py', text), (
        'the NIGHTLY pytest run must pass -s so section wall times reach the log'
    )


def test_e2e_both_backend_runs_all_three_suites(tmp_path):
    """E2E=both runs the harness, the avr and the posix end-to-end suites in one scope."""
    copy = _minimal_suite(tmp_path / 'copy')
    (copy / 'scripts' / 'core-test.sh').write_text(
        '#!/usr/bin/env bash\nexit 0\n', encoding='utf-8'
    )
    (copy / 'tests' / 'e2e').mkdir(parents=True)
    (copy / 'tests' / 'harness' / 'simavr').mkdir(parents=True)
    (copy / 'tests' / 'e2e' / 'run.sh').write_text(
        '#!/usr/bin/env bash\necho "posix ran"\n', encoding='utf-8'
    )
    (copy / 'tests' / 'e2e' / 'run-avr.sh').write_text(
        '#!/usr/bin/env bash\necho "avr ran"\n', encoding='utf-8'
    )
    (copy / 'tests' / 'harness' / 'simavr' / 'run.sh').write_text(
        '#!/usr/bin/env bash\necho "harness ran"\n', encoding='utf-8'
    )
    env = {k: v for k, v in os.environ.items() if k not in SCOPES}
    env['PATH'] = os.pathsep.join([str(Path(sys.executable).parent), env.get('PATH', '')])
    env['E2E'] = 'both'
    result = subprocess.run(
        ['bash', 'scripts/ci-test.sh'],
        cwd=copy,
        capture_output=True,
        text=True,
        env=env,
        timeout=600,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert '== e2e-avr-harness' in result.stdout
    assert '== e2e-avr' in result.stdout
    assert '== e2e-posix' in result.stdout
