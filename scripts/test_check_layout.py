# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

import subprocess
from pathlib import Path

import check_layout


def make_skeleton(root: Path) -> None:
    for d in check_layout.REQUIRED_DIRS:
        (root / d).mkdir(parents=True, exist_ok=True)
    for f in check_layout.REQUIRED_FILES:
        (root / f).parent.mkdir(parents=True, exist_ok=True)
        (root / f).write_text('')
    (root / 'LICENSE').write_text('MIT License\n')
    (root / 'AGENTS.md').write_text('See docs/00-design.md\n')
    (root / 'CONTRIBUTING.md').write_text('AGENTS.md is the rulebook\n')
    (root / '.gitignore').write_text('scratch/\n.worktrees/\n')
    # The alias is a link, so the rulebook it points at cannot drift.
    (root / 'CLAUDE.md').unlink(missing_ok=True)
    (root / 'CLAUDE.md').symlink_to('AGENTS.md')
    # Every port proves its build, its host test and its implementation,
    # except the shared staging area, which is not a port.
    for port in sorted((root / 'ports').iterdir()):
        if port.is_dir() and port.name != 'common':
            make_port(root, port.name)


def make_port(root: Path, name: str) -> None:
    """Minimal valid port: a build script, a host test and one source
    naming both port contract symbols."""
    port = root / 'ports' / name
    (port / 'build.sh').write_text('#!/usr/bin/env bash\n')
    (port / 'tests').mkdir(exist_ok=True)
    (port / 'tests' / f'test_{name}_port.c').write_text('pass\n')
    (port / f'r2s_{name}_port.c').write_text(
        'void r2s_port_read(void) {}\nvoid r2s_port_tick(void) {}\n'
    )


def track_all(root: Path) -> None:
    subprocess.run(['git', 'init', '-q', str(root)], check=True)
    subprocess.run(['git', '-C', str(root), 'add', '-A'], check=True)


def test_complete_skeleton_passes(tmp_path):
    make_skeleton(tmp_path)
    assert check_layout.check(tmp_path) == []


def test_missing_directory_is_named(tmp_path):
    make_skeleton(tmp_path)
    (tmp_path / 'core/fuzz').rmdir()
    assert check_layout.check(tmp_path) == ['missing directory: core/fuzz']


def test_rulebook_links_are_required(tmp_path):
    make_skeleton(tmp_path)
    (tmp_path / 'AGENTS.md').write_text('no link\n')
    (tmp_path / 'CONTRIBUTING.md').write_text('no deferral\n')
    assert check_layout.check(tmp_path) == [
        'AGENTS.md does not link docs/00-design.md',
        'CONTRIBUTING.md does not defer to AGENTS.md',
    ]


def test_superseded_documents_are_flagged(tmp_path):
    make_skeleton(tmp_path)
    (tmp_path / 'proposal-v3.md').write_text('')
    (tmp_path / 'engineering-practices.md').write_text('')
    assert check_layout.check(tmp_path) == [
        'superseded document still at the root: engineering-practices.md',
        'superseded document still at the root: proposal-v3.md',
    ]


def test_gitignore_entries_are_required(tmp_path):
    make_skeleton(tmp_path)
    (tmp_path / '.gitignore').write_text('scratch/\n')
    assert check_layout.check(tmp_path) == ['.gitignore does not ignore .worktrees/']


def test_the_gate_asks_for_the_design_and_the_testing_rules(tmp_path):
    """The gate asks for what a contributor needs, and nothing else."""
    make_skeleton(tmp_path)
    assert check_layout.check(tmp_path) == []
    assert [d for d in check_layout.REQUIRED_DIRS if d.startswith('docs/')] == []
    assert [f for f in check_layout.REQUIRED_FILES if f.startswith('docs/')] == [
        'docs/00-design.md',
        'docs/01-testing.md',
    ]


def test_the_testing_rules_are_required(tmp_path):
    """The testing strategy is normative, so the gate must ask for it."""
    make_skeleton(tmp_path)
    (tmp_path / 'docs/01-testing.md').unlink()
    assert check_layout.check(tmp_path) == ['missing file: docs/01-testing.md']


def test_the_rulebook_alias_must_exist(tmp_path):
    make_skeleton(tmp_path)
    (tmp_path / 'CLAUDE.md').unlink()
    assert check_layout.check(tmp_path) == ['missing file: CLAUDE.md']


def test_the_rulebook_alias_must_be_a_link(tmp_path):
    """A copy of the rulebook would drift from it, so only a link passes."""
    make_skeleton(tmp_path)
    (tmp_path / 'CLAUDE.md').unlink()
    (tmp_path / 'CLAUDE.md').write_text('See docs/00-design.md\n')
    assert check_layout.check(tmp_path) == ['CLAUDE.md is not a symlink to AGENTS.md']


def test_the_rulebook_alias_must_point_at_the_rulebook(tmp_path):
    make_skeleton(tmp_path)
    (tmp_path / 'CLAUDE.md').unlink()
    (tmp_path / 'CLAUDE.md').symlink_to('README.md')
    assert check_layout.check(tmp_path) == ['CLAUDE.md is not a symlink to AGENTS.md']


def test_a_link_that_points_at_itself_is_reported_not_raised(tmp_path):
    """A loop must be a problem line, not an exception that stops the gate."""
    make_skeleton(tmp_path)
    (tmp_path / 'CLAUDE.md').unlink()
    (tmp_path / 'CLAUDE.md').symlink_to('CLAUDE.md')
    assert check_layout.check(tmp_path) == [
        'missing file: CLAUDE.md',
        'CLAUDE.md is not a symlink to AGENTS.md',
    ]


def test_a_gitkeep_next_to_a_file_is_named(tmp_path):
    make_skeleton(tmp_path)
    (tmp_path / 'tools/.gitkeep').write_text('')
    (tmp_path / 'tools/size_report.py').write_text('')
    track_all(tmp_path)
    assert check_layout.check(tmp_path) == ['.gitkeep in a non-empty directory: tools/.gitkeep']


def test_a_gitkeep_next_to_a_subdirectory_with_a_file_is_named(tmp_path):
    """A placeholder is only needed while nothing below it is tracked."""
    make_skeleton(tmp_path)
    (tmp_path / 'spec/vectors/crc/.gitkeep').write_text('')
    (tmp_path / 'spec/vectors/crc/cases').mkdir()
    (tmp_path / 'spec/vectors/crc/cases/check.yaml').write_text('')
    track_all(tmp_path)
    assert check_layout.check(tmp_path) == [
        '.gitkeep in a non-empty directory: spec/vectors/crc/.gitkeep'
    ]


def test_a_gitkeep_in_an_empty_directory_passes(tmp_path):
    make_skeleton(tmp_path)
    (tmp_path / 'core/fuzz/.gitkeep').write_text('')
    track_all(tmp_path)
    assert check_layout.check(tmp_path) == []


def test_without_a_checkout_every_present_file_counts(tmp_path):
    """No index means the files there are the whole content, so they count."""
    make_skeleton(tmp_path)
    (tmp_path / 'tools/.gitkeep').write_text('')
    (tmp_path / 'tools/size_report.py').write_text('')
    assert check_layout.check(tmp_path) == ['.gitkeep in a non-empty directory: tools/.gitkeep']


def test_an_untracked_neighbour_does_not_count(tmp_path):
    """A scratch file must not turn a needed placeholder into a failure."""
    make_skeleton(tmp_path)
    (tmp_path / 'core/fuzz/.gitkeep').write_text('')
    track_all(tmp_path)
    (tmp_path / 'core/fuzz/local.c').write_text('')
    assert check_layout.check(tmp_path) == []


def test_repository_matches_the_layout():
    root = Path(check_layout.__file__).resolve().parent.parent
    assert check_layout.check(root) == []


def test_template_is_a_required_directory(tmp_path):
    import shutil

    make_skeleton(tmp_path)
    shutil.rmtree(tmp_path / 'ports/template')
    assert check_layout.check(tmp_path) == ['missing directory: ports/template']


def test_port_without_a_build_script_fails(tmp_path):
    make_skeleton(tmp_path)
    (tmp_path / 'ports/arduino/build.sh').unlink()
    assert check_layout.check(tmp_path) == [
        'ports/arduino has no build script (build.sh or run.sh)'
    ]


def test_run_sh_alone_is_a_build_script(tmp_path):
    make_skeleton(tmp_path)
    (tmp_path / 'ports/arduino/build.sh').unlink()
    (tmp_path / 'ports/arduino/run.sh').write_text('#!/usr/bin/env bash\n')
    assert check_layout.check(tmp_path) == []


def test_port_without_a_host_test_fails(tmp_path):
    import shutil

    make_skeleton(tmp_path)
    shutil.rmtree(tmp_path / 'ports/arduino/tests')
    assert check_layout.check(tmp_path) == ['ports/arduino has no host test (run.sh or tests/)']


def test_run_sh_alone_is_a_host_test(tmp_path):
    import shutil

    make_skeleton(tmp_path)
    shutil.rmtree(tmp_path / 'ports/arduino/tests')
    (tmp_path / 'ports/arduino/run.sh').write_text('#!/usr/bin/env bash\n')
    assert check_layout.check(tmp_path) == []


def test_port_without_an_implementation_fails(tmp_path):
    make_skeleton(tmp_path)
    (tmp_path / 'ports/arduino/r2s_arduino_port.c').write_text('void setup(void) {}\n')
    assert check_layout.check(tmp_path) == [
        'ports/arduino has no implementation (a source naming r2s_port_read and r2s_port_tick)'
    ]


def test_test_sources_do_not_count_as_an_implementation(tmp_path):
    """Symbols under tests/ prove the test, not the port, so they pass nothing."""
    make_skeleton(tmp_path)
    (tmp_path / 'ports/arduino/r2s_arduino_port.c').write_text('void setup(void) {}\n')
    (tmp_path / 'ports/arduino/tests/test_arduino_port.c').write_text(
        'void r2s_port_read(void) {}\nvoid r2s_port_tick(void) {}\n'
    )
    assert check_layout.check(tmp_path) == [
        'ports/arduino has no implementation (a source naming r2s_port_read and r2s_port_tick)'
    ]


def test_a_build_script_naming_the_symbols_is_not_an_implementation(tmp_path):
    """The template build script lists the contract symbols it checks for."""
    make_skeleton(tmp_path)
    (tmp_path / 'ports/arduino/r2s_arduino_port.c').write_text('void setup(void) {}\n')
    (tmp_path / 'ports/arduino/build.sh').write_text('r2s_port_read r2s_port_tick\n')
    assert check_layout.check(tmp_path) == [
        'ports/arduino has no implementation (a source naming r2s_port_read and r2s_port_tick)'
    ]


def test_common_is_not_a_port(tmp_path):
    make_skeleton(tmp_path)
    (tmp_path / 'ports/common/firmware.c').write_text('shared\n')
    assert check_layout.check(tmp_path) == []


def test_a_gitkeep_only_directory_is_not_a_port(tmp_path):
    import shutil

    make_skeleton(tmp_path)
    port = tmp_path / 'ports/avr'
    (port / 'build.sh').unlink()
    (port / 'r2s_avr_port.c').unlink()
    shutil.rmtree(port / 'tests')
    (port / '.gitkeep').write_text('')
    assert check_layout.check(tmp_path) == []


def test_stm32_and_rp2040_are_exempt_from_the_host_test(tmp_path):
    import shutil

    make_skeleton(tmp_path)
    for name in ('stm32', 'rp2040'):
        shutil.rmtree(tmp_path / 'ports' / name / 'tests')
    assert check_layout.check(tmp_path) == []


def test_stm32_still_needs_a_build_script_and_an_implementation(tmp_path):
    make_skeleton(tmp_path)
    (tmp_path / 'ports/stm32/build.sh').unlink()
    (tmp_path / 'ports/stm32/r2s_stm32_port.c').write_text('void setup(void) {}\n')
    assert check_layout.check(tmp_path) == [
        'ports/stm32 has no build script (build.sh or run.sh)',
        'ports/stm32 has no implementation (a source naming r2s_port_read and r2s_port_tick)',
    ]
