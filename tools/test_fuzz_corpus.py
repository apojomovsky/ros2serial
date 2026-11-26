# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Seed corpora under core/fuzz/corpus/ come from the conformance vectors.

tools/fuzz_corpus.py extracts them; these tests check every seed against the
reference codec and that the committed files match the generator byte for
byte, so a vector change without regenerated seeds fails the suite. The
bridge corpora under bridge/r2s_bridge/fuzz/corpus/ follow the same rule.
"""

from pathlib import Path

import fuzz_corpus
import pytest
import yaml
from r2s_gen.reference import frames
from r2s_gen.reference import rcobs

ROOT = Path(__file__).resolve().parent.parent
CORPUS = ROOT / 'core' / 'fuzz' / 'corpus'
BRIDGE_CORPUS = ROOT / 'bridge' / 'r2s_bridge' / 'fuzz' / 'corpus'
VECTORS = ROOT / 'spec' / 'vectors'


def _seeds(target):
    return {
        path.name: path.read_bytes()
        for path in sorted((CORPUS / target).iterdir())
        if path.is_file()
    }


def _wire_units(seed):
    # A seed is whole frame units; splitting on the delimiter checks each one.
    assert seed.endswith(b'\x00'), 'a seed is frame units with their delimiters'
    return [unit + b'\x00' for unit in seed.split(b'\x00') if unit]


def test_corpus_matches_generator():
    assert fuzz_corpus.check(fuzz_corpus.generate()) == []


def test_rcobs_seeds_are_canonical_encodings():
    # The invalid vectors are negative seeds: they must decode to nothing.
    from r2s_gen.reference.errors import Rejected

    for name, seed in _seeds('rcobs_decode').items():
        if not name.startswith('vector_'):
            continue
        if 'invalid' in name:
            with pytest.raises(Rejected):
                rcobs.decode(seed)
        else:
            assert rcobs.encode(rcobs.decode(seed)) == seed


def test_framer_seeds_cover_every_frame_vector():
    seeds = _seeds('framer')
    for path in sorted((VECTORS / 'frames').glob('*.yaml')):
        wire = bytes.fromhex(yaml.safe_load(path.read_text())['wire'])
        assert wire in seeds.values(), f'{path.name} has no seed'
    for name, seed in seeds.items():
        if name.startswith('vector_'):
            for unit in _wire_units(seed):
                assert frames.decode(unit) is not None


def test_session_seeds_cover_every_step_frame():
    seeds = _seeds('session')
    covered = 0
    for path in sorted((VECTORS / 'session').glob('*.yaml')):
        for step in yaml.safe_load(path.read_text())['steps']:
            if not step['frame']:
                continue
            wire = rcobs.encode(bytes.fromhex(step['frame'])) + b'\x00'
            assert any(wire in seed for seed in seeds.values()), f'{path.name} step has no seed'
            covered += 1
    assert covered > 0
    for seed in seeds.values():
        for unit in _wire_units(seed):
            assert frames.decode(unit) is not None


def _bridge_seeds(target):
    return {
        path.name: path.read_bytes()
        for path in sorted((BRIDGE_CORPUS / target).iterdir())
        if path.is_file()
    }


def _records(seed):
    records = []
    offset = 0
    while offset < len(seed):
        assert offset + 4 <= len(seed), 'a record header is four bytes'
        length = int.from_bytes(seed[offset + 2 : offset + 4], 'little')
        payload = seed[offset + 4 : offset + 4 + length]
        assert len(payload) == length, 'a record payload is whole'
        records.append((seed[offset], seed[offset + 1], payload))
        offset += 4 + length
    return records


def test_bridge_session_seeds_start_with_a_valid_hello():
    seeds = _bridge_seeds('bridge_session')
    assert seeds, 'the bridge session corpus is empty'
    for name, seed in seeds.items():
        if not name.startswith('transcript_'):
            continue
        records = _records(seed)
        assert records, f'{name} holds no frame'
        topic_id, _seq, payload = records[0]
        assert topic_id == 0, f'{name} does not open on the control channel'
        assert payload and payload[0] == 0x02, f'{name} does not open with HELLO'


def test_bridge_session_seeds_cover_every_mcu_step():
    seeds = _bridge_seeds('bridge_session')
    covered = 0
    for path in sorted((VECTORS / 'session').glob('*.yaml')):
        for step in yaml.safe_load(path.read_text())['steps']:
            if not step['frame'] or step['from'] != 'mcu':
                continue
            frame = bytes.fromhex(step['frame'])
            record = frame[:2] + (len(frame) - 4).to_bytes(2, 'little') + frame[2:-2]
            assert any(record in seed for seed in seeds.values()), (
                f'{path.name} mcu step has no seed'
            )
            covered += 1
    assert covered > 0


def test_bridge_converter_seeds_cover_the_fuzzed_types():
    seeds = _bridge_seeds('bridge_converter')
    assert seeds, 'the bridge converter corpus is empty'
    types = {
        0: 'sensor_msgs/msg/Imu',
        1: 'sensor_msgs/msg/JointState',
        2: 'tf2_msgs/msg/TFMessage',
    }
    for path in sorted((VECTORS / 'messages').glob('*.yaml')):
        doc = yaml.safe_load(path.read_text())
        if doc['type'] not in types.values():
            continue
        payload = bytes.fromhex(doc['payload'])
        assert any(payload in seed for seed in seeds.values()), f'{path.name} payload has no seed'
    for seed in seeds.values():
        assert seed[0] in types, 'a seed names a type the target does not fuzz'
        assert seed[1] in (0, 1), 'a seed narrow flag is 0 or 1'


def test_codec_seeds_cover_every_message_payload():
    seeds = _seeds('codec')
    payloads = []
    for path in sorted((VECTORS / 'messages').glob('*.yaml')):
        payloads.append(bytes.fromhex(yaml.safe_load(path.read_text())['payload']))
    for stem in ('twist_full', 'twist_narrow', 'float32_full', 'float32_narrow'):
        doc = yaml.safe_load((VECTORS / 'frames' / f'{stem}.yaml').read_text())
        payloads.append(bytes.fromhex(doc['payload']))
    for payload in payloads:
        assert any(payload in seed for seed in seeds.values()), 'a message payload has no seed'
