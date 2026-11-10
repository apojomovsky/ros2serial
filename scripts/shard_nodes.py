# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Split the SCRIPTS scope's collected test node ids across CI shards.

Every node id spreads round-robin over the shards in sorted order, so the
split is deterministic and covers every test exactly once with no stored
list. #337 moved the e2e isolation pairs to the nightly workflow, so no
shard is pinned anymore.
"""

N_SHARDS = 4


def parse_spec(spec, n=N_SHARDS):
    """A --shard value naming one shard, failing precisely otherwise."""
    text = str(spec).strip()
    if text.isdigit() and 0 <= int(text) < n:
        return int(text)
    raise ValueError(f'bad shard {spec!r}: expected 0 to {n - 1}')


def shard_of(position, n=N_SHARDS):
    """The shard for one position in the sorted node ids."""
    return position % n


def nodes_for_shard(node_ids, shard, n=N_SHARDS):
    """The node ids of one shard: the sorted ids dealt round-robin."""
    parse_spec(shard, n)
    return [
        node_id
        for position, node_id in enumerate(sorted(node_ids))
        if shard_of(position, n) == shard
    ]
