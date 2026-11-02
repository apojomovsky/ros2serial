"""Split the SCRIPTS scope's collected test node ids across CI shards.

Shard 0 holds scripts/test_e2e_isolation.py on its own until #337 moves it
to the nightly. Every other node id spreads round-robin over the remaining
shards in sorted order, so the split is deterministic and covers every test
exactly once with no stored list.
"""

from pathlib import PurePosixPath

N_SHARDS = 4
E2E_ISOLATION_FILE = "scripts/test_e2e_isolation.py"
E2E_ISOLATION_NAME = "test_e2e_isolation.py"


def is_pinned(node_id):
    """Pinned node ids run in shard 0 whatever directory pytest started in."""
    return PurePosixPath(node_id.split("::", 1)[0]).name == E2E_ISOLATION_NAME


def parse_spec(spec, n=N_SHARDS):
    """A --shard value naming one shard, failing precisely otherwise."""
    text = str(spec).strip()
    if text.isdigit() and 0 <= int(text) < n:
        return int(text)
    raise ValueError(f"bad shard {spec!r}: expected 0 to {n - 1}")


def shard_of(node_id, position, n=N_SHARDS):
    """The shard for one node id; position counts unpinned ids in sorted order."""
    if is_pinned(node_id):
        return 0
    return 1 + position % (n - 1)


def nodes_for_shard(node_ids, shard, n=N_SHARDS):
    """The node ids of one shard: pinned ids in shard 0, the rest round-robin."""
    parse_spec(shard, n)
    pinned = sorted(node_id for node_id in node_ids if is_pinned(node_id))
    if shard == 0:
        return pinned
    rest = sorted(node_id for node_id in node_ids if not is_pinned(node_id))
    return [node_id for position, node_id in enumerate(rest) if shard_of(node_id, position, n) == shard]
