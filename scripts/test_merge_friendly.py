"""The merge-friendly repository: driver routing, union lists, one bundle hash.

Concurrent branches must merge without a person when they do not really
disagree. mergiraf merges order-free additions, one-item-per-line lists
union-merge, generated files take one side and regenerate, and the bundle
hash lives in one file instead of every banner. These tests pin the
.gitattributes routing, the list shapes and the single hash home.
"""

import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

sys.path.insert(0, str(ROOT / 'gen'))

from r2s_gen import bundle  # noqa: E402

MERGIRAF_CASES = [
    'core/src/codec.c',
    'core/include/r2s/r2s_codec.h',
    'cpp/src/runtime.cpp',
    'cpp/include/r2s/runtime.hpp',
    'gen/r2s_gen/bundle.py',
    'ports/posix/r2s.yaml',
    '.github/workflows/ci.yml',
    'pyproject.toml',
    'cpp/CMakeLists.txt',
    'scripts/pinned.cmake',
    'Makefile',
    'scripts/pinned.mk',
    'GNUmakefile',
    'scripts/release.sh',
    'scripts/legacy.bash',
    'README.md',
]

GENERATED_CASES = [
    'gen/tests/golden/types/float32.h',
    'gen/tests/golden/bundle/r2s_bundled.h',
    'examples/cmake_minimal/lib/r2s_generated.h',
    'ports/posix/src/r2s_generated.c',
    'cpp/include/std_msgs/msg/float32.hpp',
    'cpp/include/r2s_bundled.h',
]

HANDWRITTEN_CASES = [
    'ports/posix/src/r2s_posix_port.c',
    'cpp/include/r2s/runtime.hpp',
]

LIST_FILES = [
    'scripts/pass-vars.list',
    'scripts/release-files.list',
    'tests/e2e/legs.list',
    'tests/e2e/scenarios.list',
    'tests/e2e/scenarios-compat.list',
]

GENERATED_DIRS = [
    'gen/tests/golden',
    'examples/cmake_minimal/lib',
    'ports/posix/src',
    'cpp/include/builtin_interfaces',
    'cpp/include/geometry_msgs',
    'cpp/include/sensor_msgs',
    'cpp/include/std_msgs',
    'cpp/include/tf2_msgs',
]

GENERATED_FILES = [
    'cpp/include/r2s_bundled.h',
    'cpp/include/ros2serial.h',
    'cpp/include/ros2serial.hpp',
]


def check_attr(attribute, paths):
    """The attribute values git resolves for paths, in path order."""
    proc = subprocess.run(
        ['git', 'check-attr', attribute, '--', *paths],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    return [line.rsplit(': ', 1)[1] for line in proc.stdout.splitlines()]


def test_mergiraf_routes_the_supported_source_extensions():
    assert check_attr('merge', MERGIRAF_CASES) == ['mergiraf'] * len(MERGIRAF_CASES)


def test_generated_paths_take_one_side_and_mark_generated():
    assert check_attr('linguist-generated', GENERATED_CASES) == ['set'] * len(GENERATED_CASES)
    assert check_attr('merge', GENERATED_CASES) == ['ours'] * len(GENERATED_CASES)
    assert check_attr('linguist-generated', HANDWRITTEN_CASES) == ['unspecified'] * len(
        HANDWRITTEN_CASES
    )
    assert check_attr('merge', HANDWRITTEN_CASES) == ['mergiraf'] * len(HANDWRITTEN_CASES)


def list_items(path):
    """The list entries of a one-item-per-line file, skipping blanks and comments."""
    items = []
    for line in (ROOT / path).read_text(encoding='utf-8').splitlines():
        if not line or line.startswith('#'):
            continue
        assert line.split() == [line], f'{path} line holds more than one item: {line!r}'
        items.append(line)
    return items


def test_union_lists_are_one_item_per_line_without_duplicates():
    for path in LIST_FILES:
        assert (ROOT / path).is_file(), f'missing list file: {path}'
        items = list_items(path)
        assert items, f'empty list file: {path}'
        assert len(set(items)) == len(items), f'duplicate entry in {path}'
    assert check_attr('merge', LIST_FILES) == ['union'] * len(LIST_FILES)


def generated_texts():
    """Every checked-in generated file the bundle and layout emitters write."""
    paths = [
        path
        for name in GENERATED_DIRS
        for path in sorted((ROOT / name).rglob('*'))
        if path.is_file()
    ]
    paths += [ROOT / name for name in GENERATED_FILES]
    return paths


def test_no_generated_banner_carries_the_bundle_hash():
    offenders = [
        str(path.relative_to(ROOT))
        for path in generated_texts()
        if re.search(r'bundle [0-9a-f]{16}', path.read_text(encoding='utf-8'))
    ]
    assert offenders == []


def test_the_bundle_hash_lives_in_the_bundled_header_only():
    digest = bundle._digest('jazzy')
    golden = ROOT / 'gen/tests/golden/bundle/r2s_bundled.h'
    assert f'#define R2S_BUNDLE_HASH "{digest}"' in golden.read_text(encoding='utf-8')
    holders = [
        str(path.relative_to(ROOT))
        for path in generated_texts()
        if 'R2S_BUNDLE_HASH' in path.read_text(encoding='utf-8')
    ]
    assert sorted(holders) == [
        'cpp/include/r2s_bundled.h',
        'gen/tests/golden/bundle/r2s_bundled.h',
    ]


WORDS = {
    'one': 1,
    'two': 2,
    'three': 3,
    'four': 4,
    'five': 5,
    'six': 6,
    'seven': 7,
    'eight': 8,
    'nine': 9,
    'ten': 10,
    'eleven': 11,
    'twelve': 12,
    'thirteen': 13,
    'fourteen': 14,
    'fifteen': 15,
    'sixteen': 16,
    'seventeen': 17,
    'eighteen': 18,
    'nineteen': 19,
    'twenty': 20,
}


def test_readme_bundled_types_match_the_bundle():
    line = next(
        line
        for line in (ROOT / 'README.md').read_text(encoding='utf-8').splitlines()
        if 'common types' in line
    )
    assert re.search(r'ships (\w+) common types', line).group(1) in WORDS
    assert WORDS[re.search(r'ships (\w+) common types', line).group(1)] == len(
        bundle.BUNDLED_TYPES
    )
    assert re.findall(r'`([^`]+)`', line) == list(bundle.BUNDLED_TYPES)


def _path_without_mergiraf():
    """This PATH minus every directory holding a mergiraf executable."""
    keep = []
    for entry in os.environ.get('PATH', '').split(os.pathsep):
        candidate = Path(entry or '.', 'mergiraf')
        if candidate.is_file() and os.access(candidate, os.X_OK):
            continue
        keep.append(entry)
    return os.pathsep.join(keep) or os.pathsep.join(['/usr/bin', '/bin'])


def test_setup_hooks_registers_the_merge_drivers(tmp_path):
    repo = tmp_path / 'repo'
    repo.mkdir()
    subprocess.run(['git', 'init', '-q', '-b', 'master'], cwd=repo, check=True)
    env = {**os.environ, 'PATH': _path_without_mergiraf()}
    subprocess.run(['bash', str(ROOT / 'scripts/setup-hooks.sh'), str(repo)], check=True, env=env)
    local = ['git', 'config', '--local']
    assert subprocess.run(
        [*local, 'core.hooksPath'], cwd=repo, capture_output=True, text=True
    ).stdout.strip()
    assert (
        subprocess.run(
            [*local, 'merge.ours.driver'], cwd=repo, capture_output=True, text=True
        ).stdout.strip()
        == 'true'
    )
    assert (
        subprocess.run(
            [*local, 'merge.mergiraf.driver'], cwd=repo, capture_output=True, text=True
        ).stdout.strip()
        == ''
    )


def test_setup_hooks_registers_mergiraf_when_it_is_on_path(tmp_path):
    repo = tmp_path / 'repo'
    repo.mkdir()
    subprocess.run(['git', 'init', '-q', '-b', 'master'], cwd=repo, check=True)
    bindir = tmp_path / 'bin'
    bindir.mkdir()
    (bindir / 'mergiraf').write_text('#!/bin/sh\nexit 0\n', encoding='utf-8')
    (bindir / 'mergiraf').chmod(0o755)
    env = {**os.environ, 'PATH': os.pathsep.join([str(bindir), os.environ.get('PATH', '')])}
    subprocess.run(['bash', str(ROOT / 'scripts/setup-hooks.sh'), str(repo)], check=True, env=env)
    out = subprocess.run(
        ['git', 'config', '--local', 'merge.mergiraf.driver'],
        cwd=repo,
        capture_output=True,
        text=True,
    ).stdout.strip()
    assert out == 'mergiraf merge --git %O %A %B -s %S -x %X -y %Y -p %P -l %L'
