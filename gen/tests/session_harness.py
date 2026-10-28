"""The pair the session tests step together: the C core and the Python model.

Every helper applies one event to both sides and compares every observable:
the frames sent, the connection state, the token, the topic statuses and the
counters. The state machine and the injected-bug test share it, so the
demonstration runs through the same comparison the machine uses.
"""

from __future__ import annotations

import struct
from pathlib import Path

from r2s_gen.reference import frames as frames_mod
from r2s_gen.reference import host as host_mod
from r2s_gen.reference import session as model

BATTERY_ID = host_mod.BATTERY_ID
CMD_VEL_ID = host_mod.CMD_VEL_ID

TOPIC_IDS = (BATTERY_ID, CMD_VEL_ID, 18)
TOPIC_NAMES = ("battery", "cmd_vel", "log_text")
TYPE_NAMES = ("std_msgs/msg/Float32", "geometry_msgs/msg/Twist", "std_msgs/msg/String")
TYPE_HASHES = (bytes(range(8)), bytes(range(0x10, 0x18)), bytes(range(0x20, 0x28)))
DIRECTIONS = ("pub", "sub", "pub")


def model_topics(flags: list[int]) -> list[model.Topic]:
    """The handwritten table with the C build's own flags and widths."""
    topics = []
    for topic_id, flag, type_hash, type_name, topic_name, direction in zip(
        TOPIC_IDS, flags, TYPE_HASHES, TYPE_NAMES, TOPIC_NAMES, DIRECTIONS, strict=True
    ):
        narrow = bool(flag & 0x02)
        codec_len = 24 if narrow and direction == "sub" else 48 if direction == "sub" else 0
        topics.append(model.Topic(topic_id, flag, type_hash, type_name, topic_name, direction, codec_len))
    return topics


def wire(topic_id: int, seq: int, payload: bytes) -> bytes:
    """One host frame on the wire, delimiter included."""
    return frames_mod.encode_wire(topic_id, seq, payload)


def format_frames(frames: list[bytes]) -> str:
    """Hex lines for a mismatch report, one frame per line."""
    return "\n".join(b.hex(" ") for b in frames)


class Pair:
    """A host driver and a model stepped in lockstep, compared after each."""

    def __init__(self, lib_path: Path | None = None) -> None:
        self.host = host_mod.HostSession(lib_path)
        self.host.reset()
        flags = [self.host.topic_flags(i) for i in range(3)]
        self.model = model.McuSession(topics=model_topics(flags))
        self.model.reset(0)
        self.tick = 0
        self.check("boot")

    def check(self, label: str) -> None:
        """Fail when the C core and the model disagree after `label`."""
        host_tx = self.host.take_tx()
        model_tx = self.model.take_tx()
        assert host_tx == model_tx, (
            f"{label}: TX differs\nC:\n{format_frames(host_tx)}\nmodel:\n{format_frames(model_tx)}"
        )
        host_snap = self.host.snapshot()
        model_snap = self.model.snapshot()
        assert host_snap == model_snap, f"{label}: state differs\nC: {host_snap}\nmodel: {model_snap}"

    def reset(self) -> None:
        """Reboot both sides to tick 0 and compare the boot HELLO."""
        self.host.reset()
        self.model.reset(0)
        self.tick = 0
        self.check("reset")

    def feed_frame(self, topic_id: int, seq: int, payload: bytes) -> None:
        """One host frame through both sides."""
        data = wire(topic_id, seq, payload)
        self.host.feed(data)
        self.model.feed(data)
        self.model.spin()
        self.check(f"frame topic {topic_id} seq {seq} payload {payload.hex(' ')}")

    def feed_raw(self, data: bytes) -> None:
        """Arbitrary bytes through both sides: corrupt, partial or empty."""
        self.host.feed(data)
        self.model.feed(data)
        self.model.spin()
        self.check(f"raw bytes {data.hex(' ')}")

    def advance(self, ms: int) -> None:
        """Move both clocks by `ms` milliseconds and spin once."""
        self.tick = (self.tick + ms) & 0xFFFFFFFF
        self.host.advance_to(self.tick)
        self.model.advance_to(self.tick)
        self.check(f"advance {ms} ms to tick {self.tick}")

    def publish(self, value: float) -> None:
        """Publish `value` on battery through both wrappers."""
        host_rc = self.host.publish_battery(value)
        model_rc = self.model.publish(BATTERY_ID, struct.pack("<f", value))
        assert host_rc == model_rc, f"publish {value}: return {host_rc} != model {model_rc}"
        self.check(f"publish {value}")
