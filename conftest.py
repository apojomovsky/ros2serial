"""Every pytest run starts without the caller's repository-local git variables.

Git exports GIT_DIR, GIT_INDEX_FILE and the like to hooks; a test that builds a
throwaway repo would otherwise run its git commands on the real repository.
"""

import os
import subprocess
import sys
from pathlib import Path

try:
    _vars = subprocess.run(["git", "rev-parse", "--local-env-vars"], capture_output=True, text=True).stdout.split()
except FileNotFoundError:
    _vars = []  # no git, so nothing could have been exported for it
for _var in _vars:
    os.environ.pop(_var, None)

sys.path.insert(0, str(Path(__file__).resolve().parent / "scripts"))

import pytest  # noqa: E402
import shard_nodes  # noqa: E402


def pytest_addoption(parser):
    parser.addoption(
        "--shard",
        default=None,
        help="run only one CI shard of the SCRIPTS scope: 0 to N-1",
    )


def pytest_collection_modifyitems(config, items):
    spec = config.getoption("shard")
    if spec is None:
        return
    try:
        shard = shard_nodes.parse_spec(spec)
    except ValueError as err:
        raise pytest.UsageError(str(err)) from err
    wanted = set(shard_nodes.nodes_for_shard([item.nodeid for item in items], shard))
    items[:] = [item for item in items if item.nodeid in wanted]
