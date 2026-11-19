# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""mutmut runs from a workspace that matches the paths the suite assumes.

Two facts decide the layout. mutmut names its mutants after the source file
path it was given, so the package must sit at the top of the directory it
runs in for its keys to match the name the tests import. And it runs pytest
from mutants/, so the suite and every tree it resolves through the root must
be staged there. The stage script builds both, and refuses a tree that
cannot carry them.
"""

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
STAGE = ROOT / 'scripts' / 'stage-mutmut-workspace.sh'


def make_repo(tmp_path):
    """A miniature repository with the pieces the stage must carry."""
    repo = tmp_path / 'repo'
    files = {
        'gen/r2s_gen/__init__.py': '',
        'gen/r2s_gen/bounds.py': '',
        'gen/tests/test_bounds.py': 'from r2s_gen import bounds\n',
        'gen/tests/test_packaging.py': 'def test_installs():\n    pass\n',
        'gen/tests/test_rosidl_plugin.py': 'def test_builds():\n    pass\n',
        'gen/pyproject.toml': '[tool.mutmut]\nsource_paths = ["gen/r2s_gen"]\n',
        'conftest.py': '',
        'spec/vectors/m.yaml': 'name: m\n',
        'core/src/framer.c': '',
        'pyproject.toml': '[tool.mutmut]\nsource_paths = ["gen/r2s_gen"]\n',
    }
    for name, body in files.items():
        path = repo / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(body)
    return repo


def stage(repo, workspace):
    return subprocess.run(
        ['bash', str(STAGE), str(repo), str(workspace)],
        capture_output=True,
        text=True,
        timeout=120,
    )


def test_the_package_sits_at_the_workspace_top(tmp_path):
    """mutmut names mutants after the source path, so the path is the name."""
    repo = make_repo(tmp_path)
    workspace = tmp_path / 'ws'
    result = stage(repo, workspace)
    assert result.returncode == 0, result.stderr
    assert (workspace / 'r2s_gen' / 'bounds.py').is_file()
    assert not (workspace / 'gen' / 'r2s_gen').exists()


def test_the_staged_tree_carries_the_suite_and_its_inputs(tmp_path):
    """pytest runs from mutants/, so the suite and its root lookups land there."""
    repo = make_repo(tmp_path)
    workspace = tmp_path / 'ws'
    assert stage(repo, workspace).returncode == 0
    tree = workspace / 'mutants'
    assert (tree / 'gen' / 'tests' / 'test_bounds.py').is_file()
    assert (tree / 'spec' / 'vectors' / 'm.yaml').is_file()
    assert (tree / 'core' / 'src' / 'framer.c').is_file()


def test_the_staged_tree_leaves_the_package_to_mutmut(tmp_path):
    """A copy already in mutants/ would stop mutmut copying the mutated one."""
    repo = make_repo(tmp_path)
    workspace = tmp_path / 'ws'
    assert stage(repo, workspace).returncode == 0
    assert not (workspace / 'mutants' / 'r2s_gen').exists()
    assert not (workspace / 'mutants' / 'gen' / 'r2s_gen').exists()


def test_the_workspace_config_names_the_top_level_package(tmp_path):
    """The key mutmut derives must be the name the tests import."""
    repo = make_repo(tmp_path)
    workspace = tmp_path / 'ws'
    assert stage(repo, workspace).returncode == 0
    config = (workspace / 'pyproject.toml').read_text()
    assert 'source_paths = ["r2s_gen"]' in config
    assert 'gen/tests' in config


def test_the_workspace_leaves_git_and_the_old_tree_out(tmp_path):
    """Git metadata is not an input, and a stale copy would answer a read."""
    repo = make_repo(tmp_path)
    (repo / '.git').mkdir()
    (repo / '.git' / 'config').write_text('[core]\n')
    workspace = tmp_path / 'ws'
    (workspace / 'mutants' / 'stale').mkdir(parents=True)
    assert stage(repo, workspace).returncode == 0
    assert not (workspace / '.git').exists()
    assert not (workspace / 'mutants' / 'stale').exists()


def test_a_workspace_inside_the_tree_is_not_copied_into_itself(tmp_path):
    """A caller may put the workspace under the report directory in the tree.

    The runner stages into a temporary directory, but the script must stay
    safe for a destination inside the tree: a copy that steps into its own
    output fails with "file changed as we read it" and produces no mutants/.
    """
    repo = make_repo(tmp_path)
    report = repo / 'mutation-report'
    (report / 'core-build').mkdir(parents=True)
    for index in range(50):
        (report / 'core-build' / f'file{index}.c').write_text('x' * 200)
    workspace = report / 'mutmut-workspace'
    result = subprocess.run(
        ['bash', str(STAGE), str(repo), str(workspace)],
        capture_output=True,
        text=True,
        timeout=120,
    )
    assert result.returncode == 0, result.stderr
    assert 'file changed as we read it' not in result.stderr, result.stderr
    assert (workspace / 'mutants' / 'gen' / 'tests' / 'test_bounds.py').is_file()
    # The report directory is this run's output, never an input.
    assert not (workspace / 'mutation-report').exists()
    assert not (workspace / 'mutants' / 'mutation-report').exists()


def test_the_workspace_top_is_repo_shaped_but_carries_no_conftest(tmp_path):
    """A tool that reads its own path upward must find the tree.

    r2s-arduino resolves the firmware assets from its own location, so the
    package at the top needs the rest of the tree beside it. Its conftest
    must stay out: pytest loads the conftest of every directory above its
    rootdir, and a second copy registers --shard twice.
    """
    repo = make_repo(tmp_path)
    (repo / 'core' / 'include' / 'r2s').mkdir(parents=True)
    (repo / 'core' / 'include' / 'r2s' / 'r2s.h').write_text('')
    workspace = tmp_path / 'ws'
    assert stage(repo, workspace).returncode == 0
    assert (workspace / 'core' / 'include' / 'r2s' / 'r2s.h').is_file()
    assert (workspace / 'mutants' / 'core' / 'include' / 'r2s' / 'r2s.h').is_file()
    assert not (workspace / 'conftest.py').exists()
    assert (workspace / 'mutants' / 'conftest.py').is_file()


def test_the_workspace_config_survives_the_repository_copy(tmp_path):
    """The repository's own pyproject.toml must not overwrite the run's."""
    repo = make_repo(tmp_path)
    workspace = tmp_path / 'ws'
    assert stage(repo, workspace).returncode == 0
    config = (workspace / 'pyproject.toml').read_text()
    assert 'source_paths = ["r2s_gen"]' in config
    assert 'gen/tests' in config


def test_the_workspace_carries_one_conftest_so_pytest_loads_it_once(tmp_path):
    """Two copies of the root conftest register its options twice.

    pytest loads the conftest of every directory above its rootdir, so a copy
    at the workspace top beside the one in mutants/ raised "option names
    {'--shard'} already added" and mutmut never ran.
    """
    repo = make_repo(tmp_path)
    workspace = tmp_path / 'ws'
    assert stage(repo, workspace).returncode == 0
    found = sorted(
        path.relative_to(workspace).as_posix() for path in workspace.rglob('conftest.py')
    )
    assert found == ['mutants/conftest.py'], found


def test_the_stage_refuses_a_tree_without_the_suite(tmp_path):
    """A wrong root must fail loudly instead of staging a tree pytest cannot use."""
    repo = make_repo(tmp_path)
    (repo / 'gen' / 'tests').rename(repo / 'gen' / 'elsewhere')
    result = stage(repo, tmp_path / 'ws')
    assert result.returncode != 0
    assert 'gen/tests' in result.stderr, result.stderr


def test_the_stage_skips_the_files_mutmut_cannot_run(tmp_path):
    """The two files that install the package cannot import mutmut's trampoline.

    mutmut rewrites the sources to import it, and a pip or colcon subprocess
    has no mutmut on its path. The stage names both files as ignores, in
    mutmut's argument order: the selection first, the flags after it, which
    is the order in which pytest honors them.
    """
    repo = make_repo(tmp_path)
    workspace = tmp_path / 'ws'
    assert stage(repo, workspace).returncode == 0
    config = (workspace / 'pyproject.toml').read_text()
    import tomllib

    settings = tomllib.loads(config)['tool']['mutmut']
    assert settings['pytest_add_cli_args_test_selection'] == ['gen/tests']
    assert settings['pytest_add_cli_args'] == [
        '--ignore',
        'gen/tests/test_packaging.py',
        '--ignore',
        'gen/tests/test_rosidl_plugin.py',
    ]
