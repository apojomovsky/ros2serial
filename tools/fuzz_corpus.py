# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Seed corpora for the libFuzzer targets from the conformance vectors.

Each target in core/fuzz/corpus/ holds whole inputs: rCOBS encodings for the
decoder, wire units for the framer, wire units and transcripts for the
session, and headed payloads for the codec interpreter. The bridge targets
in bridge/r2s_bridge/fuzz/corpus/ hold decoded-frame records for the session
and headed payloads for the converter. The generator keeps the committed
seeds byte-identical to the vectors; `make vectors` reruns it.
"""

import argparse
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'gen'))

from r2s_gen.reference import rcobs  # noqa: E402

VECTORS = ROOT / 'spec' / 'vectors'
CORPUS = ROOT / 'core' / 'fuzz' / 'corpus'
BRIDGE_CORPUS = ROOT / 'bridge' / 'r2s_bridge' / 'fuzz' / 'corpus'

TARGETS = ('rcobs_decode', 'framer', 'session', 'codec')

# The bridge converter target's type indices, in the order
# fuzz_bridge_converter.cpp numbers them.
BRIDGE_CONVERTER_TYPE = {
    'sensor_msgs/msg/Imu': 0,
    'sensor_msgs/msg/JointState': 1,
    'tf2_msgs/msg/TFMessage': 2,
    'std_msgs/msg/Int32MultiArray': 3,
}

# The codec target's type indices, in the order fuzz_codec_table.h numbers
# them: the generator heads each payload with the index its seeds decode as.
CODEC_TYPE = {
    'std_msgs/msg/Float32': 11,
    'geometry_msgs/msg/Twist': 10,
    'sensor_msgs/msg/Range': 15,
    'sensor_msgs/msg/Imu': 17,
    'sensor_msgs/msg/JointState': 18,
    'geometry_msgs/msg/TransformStamped': 19,
    'tf2_msgs/msg/TFMessage': 20,
}


def _hex(text):
    return bytes.fromhex(text)


def _rcobs():
    # The vector's encoding ends with the delimiter; the seed is the
    # encoding the decoder reads.
    seeds = {}
    for stem in ('basic', 'edges', 'invalid'):
        doc = yaml.safe_load((VECTORS / 'rcobs' / f'{stem}.yaml').read_text())
        for i, case in enumerate(doc['cases']):
            encoded = _hex(case['encoded'])
            assert encoded.endswith(b'\x00')
            seeds[f'vector_{stem}_{i}'] = encoded[:-1]
    return seeds


def _framer():
    seeds = {}
    for path in sorted((VECTORS / 'frames').glob('*.yaml')):
        seeds[f'vector_{path.stem}'] = _hex(yaml.safe_load(path.read_text())['wire'])
    return seeds


def _session():
    seeds = {}
    for path in sorted((VECTORS / 'session').glob('*.yaml')):
        steps = yaml.safe_load(path.read_text())['steps']
        units = [rcobs.encode(_hex(step['frame'])) + b'\x00' for step in steps if step['frame']]
        for i, unit in enumerate(units):
            seeds[f'step_{path.stem}_{i:02d}'] = unit
        hosts = [
            rcobs.encode(_hex(step['frame'])) + b'\x00'
            for step in steps
            if step['frame'] and step['from'] == 'host'
        ]
        seeds[f'transcript_{path.stem}_hosts'] = b''.join(hosts)
        seeds[f'transcript_{path.stem}_all'] = b''.join(units)
    return seeds


def _codec_payloads():
    payloads = []
    for path in sorted((VECTORS / 'messages').glob('*.yaml')):
        doc = yaml.safe_load(path.read_text())
        payloads.append((doc['type'], doc['profile']['narrow_float64'], _hex(doc['payload'])))
    for stem in ('twist_full', 'twist_narrow', 'float32_full', 'float32_narrow'):
        doc = yaml.safe_load((VECTORS / 'frames' / f'{stem}.yaml').read_text())
        kind = 'geometry_msgs/msg/Twist' if stem.startswith('twist') else 'std_msgs/msg/Float32'
        payloads.append((kind, 'narrow' in stem, _hex(doc['payload'])))
    return payloads


def _codec():
    seeds = {}
    counts: dict[str, int] = {}
    for type_name, narrow, payload in _codec_payloads():
        if type_name not in CODEC_TYPE:
            raise KeyError(f'no codec seed type for {type_name}')
        stem = type_name.split('/')[-1].lower() + ('_narrow' if narrow else '')
        head = bytes((CODEC_TYPE[type_name], 1 if narrow else 0))
        seeds[f'vector_{stem}_{counts.get(stem, 0)}'] = head + payload
        counts[stem] = counts.get(stem, 0) + 1
    return seeds


def _bridge_session():
    seeds = {}
    for path in sorted((VECTORS / 'session').glob('*.yaml')):
        steps = yaml.safe_load(path.read_text())['steps']
        records = []
        for step in steps:
            if not step['frame'] or step['from'] != 'mcu':
                continue
            frame = _hex(step['frame'])
            # A decoded frame is topic, seq, payload and the two check
            # bytes; the record drops the check the harness recomputes.
            payload = frame[2:-2]
            records.append(frame[:2] + len(payload).to_bytes(2, 'little') + payload)
        if not records:
            continue
        first = records[0]
        if first[0] == 0 and len(first) > 4 and first[4] == 0x02:
            seeds[f'transcript_{path.stem}'] = b''.join(records)
        else:
            for i, record in enumerate(records):
                seeds[f'step_{path.stem}_{i:02d}'] = record
    return seeds


def _bridge_converter():
    seeds = {}
    counts: dict[str, int] = {}
    for path in sorted((VECTORS / 'messages').glob('*.yaml')):
        doc = yaml.safe_load(path.read_text())
        if doc['type'] not in BRIDGE_CONVERTER_TYPE:
            continue
        stem = doc['type'].split('/')[-1].lower()
        narrow = doc['profile']['narrow_float64']
        head = bytes((BRIDGE_CONVERTER_TYPE[doc['type']], 1 if narrow else 0))
        name = f'vector_{stem}_{counts.get(stem, 0)}'
        seeds[name] = head + _hex(doc['payload'])
        counts[stem] = counts.get(stem, 0) + 1
    # std_msgs/msg/Int32MultiArray has no message vector: the reference
    # snapshots carry no definition for it, so no vector can seed it. Its
    # seeds are laid out by hand instead: one dim holding label "x", size
    # 2 and stride 2, a zero data_offset and the data [1, -1].
    hand = _hex('01 01 78 02 00 00 00 02 00 00 00 00 00 00 00 02 01 00 00 00 ff ff ff ff')
    index = BRIDGE_CONVERTER_TYPE['std_msgs/msg/Int32MultiArray']
    seeds['hand_int32multiarray_0'] = bytes((index, 0)) + hand
    seeds['hand_int32multiarray_narrow_0'] = bytes((index, 1)) + hand
    return seeds


def generate():
    return {
        'rcobs_decode': _rcobs(),
        'framer': _framer(),
        'session': _session(),
        'codec': _codec(),
        'bridge_session': _bridge_session(),
        'bridge_converter': _bridge_converter(),
    }


def _corpus_root(target):
    return BRIDGE_CORPUS if target.startswith('bridge_') else CORPUS


def check(generated):
    problems = []
    for target, seeds in generated.items():
        directory = _corpus_root(target) / target
        committed = (
            {path.name: path.read_bytes() for path in directory.iterdir() if path.is_file()}
            if directory.is_dir()
            else {}
        )
        for name, want in seeds.items():
            if committed.get(name) != want:
                problems.append(f'{target}/{name}: not the generated bytes')
        managed = ('vector_', 'step_', 'transcript_')
        if target.startswith('bridge_'):
            managed += ('hand_',)
        for name in committed:
            if name.startswith(managed) and name not in seeds:
                problems.append(f'{target}/{name}: no vector generates it')
    return problems


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    generated = generate()
    if args.check:
        problems = check(generated)
        for problem in problems:
            print(problem)
        return 1 if problems else 0
    for target, seeds in generated.items():
        directory = _corpus_root(target) / target
        directory.mkdir(parents=True, exist_ok=True)
        for name, blob in seeds.items():
            (directory / name).write_bytes(blob)
    return 0


if __name__ == '__main__':
    sys.exit(main())
