# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The action example on an emulated Uno: the spin action over raw frames.

The firmware announces no topics, declares the spin action on the walk,
accepts one LookupTransform goal and runs it: three empty feedbacks, then
a succeed result carrying the fixed transform. The test drives the whole
exchange from the host side of the pty, asserting the exact DECLARE,
GOAL_RESP, FEEDBACK and RESULT bytes the generated action header leads
the firmware to send, so a regression in the MCU wiring or in that
header fails here without any ROS installation. It proves the action wire
path on the AVR; the ROS 2 client side is scenario 16's.

Every payload is computed by the reference codec from the interface the
action's own generated header names, so the test pins the wire form and
not a transcription of it.

The handshake tolerates the session's own traffic the way the hello and
blink tests do: HELLO repeats while unconnected (SES-5) and heartbeats
once connected (SES-9). Reliable answers are acked as they arrive, so
the MCU never retransmits into the assertions; a duplicate reliable
frame is re-acked and skipped, and anything else unexpected fails.
Error reports fail the wait naming the bound the firmware hit: the
sketch publishes nothing, so no report is benign here.

covers: ACT-2, ACT-3, ACT-4, ACT-6, ACT-7, ACT-8
"""

import time

import serial
from r2s_gen import msgs
from r2s_gen import typehash
from r2s_gen.reference import frames
from r2s_gen.reference import messages
from test_announce import read_string
from test_echo import Harness

PROTOCOL_VERSION = 4
RUN_MS = ('--run-ms', '60000')
ACTION_NODE = 'base_controller'
ACTION_CHANNEL = 8
ACK_CHANNEL = 6
ERROR_TOPIC = 5
HEARTBEAT_TOPIC = 4
READ_TIMEOUT = 10.0

ACTION_TYPE = 'tf2_msgs/action/LookupTransform'
REPOSITORY = msgs.MessageRepository()
# An AVR build's double is 32 bits, so the action travels narrow (ENC-12);
# the reference codec asks for the same width the firmware announces.


def _declare() -> bytes:
    """The DECLARE the sketch sends: its index, the action's hash and both names."""
    digest = typehash.action_hash(REPOSITORY, ACTION_TYPE)
    return (
        bytes((0x02, 0x00))
        + bytes.fromhex(digest[len('RIHS01_') :])[:8]
        + bytes((len(ACTION_TYPE),))
        + ACTION_TYPE.encode()
        + bytes((len('spin'),))
        + b'spin'
    )


def _goal() -> bytes:
    """The goal the test sends: a target and a source frame, no advanced API."""
    return messages.encode(
        REPOSITORY,
        f'{ACTION_TYPE}_Goal',
        {
            'target_frame': 'odom',
            'source_frame': 'base',
            'source_time': {'sec': 0, 'nanosec': 0},
            'timeout': {'sec': 0, 'nanosec': 0},
            'target_time': {'sec': 0, 'nanosec': 0},
            'fixed_frame': '',
            'advanced': False,
        },
        narrow=True,
    )


def _feedback() -> bytes:
    """The empty feedback every step carries."""
    return messages.encode(REPOSITORY, f'{ACTION_TYPE}_Feedback', {}, narrow=True)


def _result() -> bytes:
    """The result the sketch sends: the fixed transform, no error."""
    return messages.encode(
        REPOSITORY,
        f'{ACTION_TYPE}_Result',
        {
            'transform': {
                'header': {'stamp': {'sec': 0, 'nanosec': 0}, 'frame_id': ''},
                'child_frame_id': '',
                'transform': {
                    'translation': {'x': 1.0, 'y': 2.0, 'z': 3.0},
                    'rotation': {'x': 0.0, 'y': 0.0, 'z': 0.0, 'w': 1.0},
                },
            },
            'error': {'error': 0, 'error_string': ''},
        },
        narrow=True,
    )


SPIN_DECLARE = _declare()
# The GOAL frame: opcode, rel_seq 0, goal_seq 0, action index 0, then the goal.
GOAL_PAYLOAD = bytes((0x04, 0x00, 0x00, 0x00)) + _goal()
GOAL_RESP_ACCEPTED = bytes((0x05, 0x00, 0x00, 0x00))
# The RESULT frame: opcode, its own rel_seq 1, goal_seq 0, succeeded (ACT-6).
RESULT_SUCCEEDED = bytes((0x09, 0x01, 0x00, 0x04)) + _result()
# A FEEDBACK carries no rel_seq: opcode, goal_seq 0, action index 0 (ACT-1).
FEEDBACKS = tuple(bytes((0x08, 0x00, 0x00)) + _feedback() for _ in range(3))


def next_unit(port, deadline):
    """The next decodable unit, skipping garbled ones until the deadline."""
    while True:
        remaining = deadline - time.monotonic()
        assert remaining > 0, 'no valid unit within the wait'
        unit = bytearray()
        unit_deadline = time.monotonic() + min(remaining, READ_TIMEOUT)
        while time.monotonic() < unit_deadline:
            unit += port.read(1)
            if unit.endswith(b'\x00') and len(unit) > 1:
                break
        if not unit.endswith(b'\x00'):
            continue
        try:
            return frames.decode(bytes(unit))
        except Exception:  # noqa: BLE001 - a garbled unit is skipped, not diagnosed here
            continue


def read_hello(port, deadline):
    """The next HELLO unit, skipping the session's own repeats (SES-5)."""
    while True:
        topic_id, _, payload = next_unit(port, deadline)
        assert topic_id != ERROR_TOPIC, 'the sketch reported an error before connecting'
        assert topic_id == 0, f'unexpected topic {topic_id} while waiting for HELLO'
        if payload[0] == 0x02:
            return payload
        raise AssertionError(f'unexpected op {payload[0]} while waiting for HELLO')


def read_controlled(port, deadline):
    """The next unit past the HELLO repeats and heartbeats (SES-5, SES-9)."""
    while True:
        topic_id, _, payload = next_unit(port, deadline)
        if topic_id == HEARTBEAT_TOPIC:
            continue  # SES-9: the connected heartbeat shares the link
        assert topic_id != ERROR_TOPIC, 'the sketch reported an error'
        if topic_id == 0 and payload[0] == 0x02:
            continue  # SES-5: the unconnected repeat answers nothing
        return topic_id, payload


def solicit_hello(port, seq):
    """The HELLO answering a HELLO_REQ, with the action features set."""
    port.write(frames.encode_wire(0, seq, bytes((0x01, PROTOCOL_VERSION))))
    payload = read_hello(port, time.monotonic() + READ_TIMEOUT)
    assert payload[1] == PROTOCOL_VERSION
    assert payload[2] == 0x00  # full handshake mode
    assert int.from_bytes(payload[3:7], 'little') == 0, 'no token before the first READY'
    assert payload[15] == 0x98, f'action firmware without the action features: {payload[15]:#04x}'
    assert payload[16] == 0, 'the action sketch announces no topics'
    name, _ = read_string(payload, 17)
    assert name == ACTION_NODE


def walk_actions(port):
    """The action walk: the spin DECLARE with its ok verdict, then STATUS 1 past the table."""
    port.write(frames.encode_wire(ACTION_CHANNEL, 0, bytes((0x01, 0x00))))
    topic_id, payload = read_controlled(port, time.monotonic() + READ_TIMEOUT)
    assert topic_id == ACTION_CHANNEL, f'unexpected topic {topic_id} on the action walk'
    assert bytes(payload) == SPIN_DECLARE, f'unexpected DECLARE {payload.hex()}'
    # The host's verdict resolves the action; without it the MCU refuses
    # every goal as unknown (ACT-7).
    port.write(frames.encode_wire(ACTION_CHANNEL, 1, bytes((0x03, 0x00, 0x00, 0x00))))
    port.write(frames.encode_wire(ACTION_CHANNEL, 2, bytes((0x01, 0x01))))
    topic_id, payload = read_controlled(port, time.monotonic() + READ_TIMEOUT)
    assert topic_id == ACTION_CHANNEL
    assert bytes(payload) == bytes((0x03, 0x00, 0x01, 0x01)), f'unexpected STATUS {payload.hex()}'


def send_ack(port, seq, channel, rel):
    """Ack one reliable frame, so the MCU never retransmits it (REL-2)."""
    port.write(frames.encode_wire(ACK_CHANNEL, seq, bytes((channel, rel))))


def test_action_walk_declares_spin():
    with Harness('atmega328p', *RUN_MS, image='action') as harness:
        with serial.Serial(harness.pty, 115200, timeout=2) as port:
            solicit_hello(port, 0)
            walk_actions(port)
    assert harness.stop() == 0


def test_goal_runs_to_succeeded_result():
    with Harness('atmega328p', *RUN_MS, image='action') as harness:
        with serial.Serial(harness.pty, 115200, timeout=2) as port:
            solicit_hello(port, 0)
            walk_actions(port)
            port.write(frames.encode_wire(0, 1, bytes((0x06, 0x2A, 0x00, 0x00, 0x00))))
            ack_seq = 0
            # The walk above spent channel seqs 0 to 2, so the GOAL
            # continues the stream at 3.
            port.write(frames.encode_wire(ACTION_CHANNEL, 3, GOAL_PAYLOAD))
            deadline = time.monotonic() + READ_TIMEOUT
            seen_feedbacks = []
            seen_ack = False
            while True:
                topic_id, payload = read_controlled(port, deadline)
                if topic_id == ACK_CHANNEL:
                    # The MCU acks the GOAL on the ack channel (REL-3).
                    assert bytes(payload) == bytes((ACTION_CHANNEL, 0x00)), (
                        f'unexpected ack {payload.hex()}'
                    )
                    seen_ack = True
                    continue
                assert topic_id == ACTION_CHANNEL, f'unexpected topic {topic_id} during the goal'
                if payload[0] == 0x05:
                    # A retransmitted GOAL_RESP is re-acked and skipped;
                    # only the first one proves the accept.
                    send_ack(port, ack_seq, ACTION_CHANNEL, payload[1])
                    ack_seq += 1
                    assert bytes(payload) == GOAL_RESP_ACCEPTED, (
                        f'unexpected GOAL_RESP {payload.hex()}'
                    )
                elif payload[0] == 0x08:
                    seen_feedbacks.append(bytes(payload))
                elif payload[0] == 0x09:
                    send_ack(port, ack_seq, ACTION_CHANNEL, payload[1])
                    ack_seq += 1
                    assert bytes(payload) == RESULT_SUCCEEDED, f'unexpected RESULT {payload.hex()}'
                    break
                else:
                    raise AssertionError(f'unexpected action op {payload[0]} during the goal')
            assert seen_feedbacks == list(FEEDBACKS), (
                f'feedback carried {[p.hex() for p in seen_feedbacks]}'
            )
            assert seen_ack, 'the MCU never acked the GOAL'
    assert harness.stop() == 0
