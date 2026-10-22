#!/usr/bin/env python3
"""Validate every conformance vector against the JSON Schema of its kind.

The kind is the directory under spec/vectors/; its schema is
spec/schema/vectors/<kind>.json. A frame vector is also checked against the
reference codec, so that the bytes it carries are the bytes the reference
makes. Exits 1 with one line per problem, naming the file and the key path.
"""

import json
import struct
import sys
from pathlib import Path

import jsonschema
import yaml

ROOT = Path(__file__).resolve().parent.parent
SCHEMAS = ROOT / "spec" / "schema" / "vectors"
sys.path.insert(0, str(ROOT / "gen"))

from r2s_gen.reference import crc, rcobs  # noqa: E402


def _schemas() -> dict[str, jsonschema.protocols.Validator]:
    out = {}
    for path in sorted(SCHEMAS.glob("*.json")):
        schema = json.loads(path.read_text())
        cls = jsonschema.validators.validator_for(schema)
        cls.check_schema(schema)
        out[path.stem] = cls(schema)
    return out


def _location(error: jsonschema.ValidationError) -> str:
    return "/".join(str(p) for p in error.absolute_path) or "<root>"


def _message(error: jsonschema.ValidationError) -> str:
    # A oneOf failure only says "not valid under any of the given schemas"; the
    # reasons each alternative was rejected are what tells the author what to fix.
    if error.validator in ("oneOf", "anyOf") and error.context:
        reasons = list(dict.fromkeys(sub.message for sub in error.context))
        return "matches none of the allowed forms: " + "; ".join(reasons)
    return error.message


def check_file(path: Path, name: str, validators: dict) -> list[str]:
    kind = name.split("/", 1)[0]
    if kind not in validators:
        return [f"{name}: unknown vector kind {kind!r}; known: {', '.join(validators)}"]
    if path.suffix != ".yaml":
        return [f"{name}: vector files must end in .yaml"]
    try:
        data = yaml.safe_load(path.read_text())
    except yaml.YAMLError as e:
        return [f"{name}: not valid YAML: {e}".splitlines()[0]]
    errors = []
    for e in validators[kind].iter_errors(data):
        # When an alternative fails only inside one field, the case clearly meant
        # that form: report the field itself rather than every alternative.
        deeper = [s for s in e.context or [] if len(s.absolute_path) > len(e.absolute_path)]
        errors += deeper if e.validator in ("oneOf", "anyOf") and deeper else [e]
    errors.sort(key=lambda e: [str(p) for p in e.absolute_path])
    lines = [f"{name}: {_location(e)}: {_message(e)}" for e in errors]
    covers = set((data or {}).get("covers") or []) if isinstance(data, dict) else set()
    for i, case in enumerate((data or {}).get("cases") or [] if isinstance(data, dict) else []):
        rule = case.get("reject") if isinstance(case, dict) else None
        if isinstance(rule, str) and rule not in covers:
            lines.append(f"{name}: cases/{i}/reject: {rule} is not in the file's covers")
    # Alternatives of a oneOf that fail on the same field report the same message.
    return list(dict.fromkeys(lines))


def check_frame(name: str, data: dict) -> list[str]:
    """Check a frame vector against the reference codec.

    The frame and the wire bytes must be the ones the reference makes, and
    the topic id must be in the range the spec gives its channel, so a
    vector cannot claim a rule it does not carry.
    """
    problems = []
    try:
        payload = bytes.fromhex(data["payload"])
    except (KeyError, TypeError, ValueError) as e:
        return [f"{name}/payload: not hexadecimal bytes: {e}"]
    try:
        frame = bytes.fromhex(data["frame"])
        wire = bytes.fromhex(data["wire"])
        topic_id = data["topic_id"]
        seq = data["seq"]
        body = bytes((topic_id, seq)) + payload
    except (KeyError, TypeError, ValueError):
        # The schema reports the field that is out of range, so there is
        # nothing to add here.
        return []

    expected_frame = body + crc.crc16(body).to_bytes(2, "little")
    expected_wire = rcobs.encode(expected_frame) + b"\x00"
    if frame != expected_frame:
        problems.append(f"{name}/frame: not the frame the reference makes")
    if wire != expected_wire:
        problems.append(f"{name}/wire: not the bytes the reference makes")
    if len(frame) < 4:
        problems.append(f"{name}/frame: shorter than 4 bytes")
    if not wire.endswith(b"\x00"):
        problems.append(f"{name}/wire: does not end with the delimiter")
    frame_name = data.get("name", "")
    problems += check_channel(name, frame_name, topic_id, payload)
    problems += check_payload(name, frame_name, payload, topic_id)
    return problems


# The system channels of spec/protocol.md section 7. A channel's opcodes
# name the layout its payload has, so a vector is checked by the channel and
# the opcode it carries, not by the name it happens to have.
CONTROL_OPCODES = {
    0x01: "hello_req",
    0x02: "hello",
    0x03: "topic_req",
    0x04: "topic",
    0x05: "topic_status",
    0x06: "ready",
}
TIME_OPCODES = {0x01: "ping", 0x02: "pong", 0x03: "correction"}
# The channels whose payload has no opcode byte, so an empty payload on
# them is what the spec says.
NO_OPCODE_CHANNELS = {4, 5}
SYSTEM_CHANNELS = {
    0: CONTROL_OPCODES,
    1: TIME_OPCODES,
    2: {"log": "log"},
    4: {"heartbeat": "heartbeat"},
    5: {"error_report": "error_report"},
}


def layout_of(topic_id: int, payload: bytes) -> str:
    """The layout a payload has, from its channel and its opcode.

    A frame on a system channel begins with an opcode byte, which names the
    layout. A frame on a user topic carries a message, which this does not
    know, so it is left alone.
    """
    if topic_id not in SYSTEM_CHANNELS:
        return "user"
    opcodes = SYSTEM_CHANNELS[topic_id]
    if not opcodes:
        return "user"
    if topic_id == 4:
        # The heartbeat channel carries two frames with no opcode, and the
        # payload tells them apart: the host's is empty, the MCU's carries
        # four counters.
        return "mcu_heartbeat" if payload else "heartbeat"
    if len(opcodes) == 1:
        # The channel's payload has no opcode byte, so it is empty or not.
        layout = next(iter(opcodes))
        return layout if payload else "empty"
    if not payload:
        return "empty"
    opcode = payload[0]
    if opcode not in opcodes:
        return "unknown"
    return opcodes[opcode]


# The channel each opcode the spec gives belongs to, by the name of the
# frame. A vector on a system channel names the frame it carries, so the
# name is what says which channel the frame was meant for.
SPEC_NAMES = {
    "hello_req": 0,
    "hello": 0,
    "topic_req": 0,
    "topic": 0,
    "topic_status": 0,
    "ready": 0,
    "ping": 1,
    "pong": 1,
    "correction": 1,
    "log": 2,
    "heartbeat": 4,
    "host_heartbeat": 4,
    "mcu_heartbeat": 4,
    "error_report": 5,
}


def check_channel(name: str, vector_name: str, topic_id: int, payload: bytes) -> list[str]:
    """Check that the frame sits on the channel its name says."""
    if 7 <= topic_id <= 15:
        if payload:
            return [f"{name}/payload: a reserved channel carries no payload"]
        return []
    if topic_id not in SYSTEM_CHANNELS:
        return []
    frame_name = vector_name.removesuffix("_compact")
    if frame_name not in SPEC_NAMES:
        return [f"{name}/name: {vector_name} is not a frame the spec gives"]
    wanted = SPEC_NAMES[frame_name]
    if topic_id != wanted:
        return [f"{name}/topic_id: {topic_id} is not the channel {wanted} of a {frame_name} frame"]
    return []


class _Reader:
    """Reads the fields of a payload, and says where it went wrong."""

    def __init__(self, data: bytes):
        self.data = data
        self.pos = 0

    def take(self, n: int) -> bytes:
        if self.pos + n > len(self.data):
            raise ValueError(f"the payload ends after {self.pos} bytes, wanted {n} more")
        out = self.data[self.pos : self.pos + n]
        self.pos += n
        return out

    def u8(self) -> int:
        return self.take(1)[0]

    def u16(self) -> int:
        return struct.unpack("<H", self.take(2))[0]

    def u32(self) -> int:
        return struct.unpack("<I", self.take(4))[0]

    def i16(self) -> int:
        return struct.unpack("<h", self.take(2))[0]

    def i32(self) -> int:
        return struct.unpack("<i", self.take(4))[0]

    def varint(self) -> int:
        """A varint as the spec gives it: at most 3 bytes, and the shortest.

        The third byte carries no continuation bit, and a value that fits in
        fewer bytes is not written in more.
        """
        value = 0
        for i in range(3):
            byte = self.u8()
            value |= (byte & 0x7F) << (7 * i)
            if not byte & 0x80:
                if i > 0 and value < (1 << (7 * i)):
                    raise ValueError("the varint is not in its shortest form")
                return value
            if i == 2:
                raise ValueError("the varint is longer than 3 bytes")
        raise ValueError("the varint is longer than 3 bytes")

    def string(self) -> bytes:
        return self.take(self.varint())


def check_payload(name: str, frame_name: str, payload: bytes, topic_id: int) -> list[str]:
    """Parse the payload the way the spec's tables describe it.

    A length that does not match the name it precedes is caught here, which
    the frame and wire bytes alone cannot show. The layout comes from the
    channel and the opcode, so a vector that is added later is checked
    without this tool changing.
    """
    problems: list[str] = []
    layout = layout_of(topic_id, payload)
    if layout == "user":
        return problems
    if layout == "unknown":
        problems.append(f"{name}/payload: the opcode is not one the spec gives for this channel")
        return problems
    if layout == "empty":
        if topic_id in NO_OPCODE_CHANNELS:
            return problems
        problems.append(f"{name}/payload: a frame on this channel carries an opcode")
        return problems
    r = _Reader(payload)
    try:
        if layout == "hello_req":
            if r.u8() != 0x01:
                problems.append(f"{name}/payload: the opcode is not HELLO_REQ")
            if r.u8() != 0x01:
                problems.append(f"{name}/payload: the protocol version is not 1")
        elif layout == "ready":
            if r.u8() != 0x06:
                problems.append(f"{name}/payload: the opcode is not READY")
        elif layout == "hello":
            if r.u8() != 0x02:
                problems.append(f"{name}/payload: the opcode is not HELLO")
            if r.u8() != 0x01:
                problems.append(f"{name}/payload: the protocol version is not 1")
            mode = r.u8()
            if mode not in (0, 1):
                problems.append(f"{name}/payload: the handshake mode is neither full nor compact")
            r.u32()  # boot id
            if r.u16() > 65277:
                problems.append(f"{name}/payload: max_frame is above the limit")
            r.u16()  # rx capacity
            tick_hz = r.u32()
            if tick_hz < 1000 or tick_hz % 1000 or 1000000 % (tick_hz // 1000):
                problems.append(f"{name}/payload: tick_hz is not 1000 times k")
            if r.u8() & ~0x3F:
                problems.append(f"{name}/payload: the features byte has a reserved bit set")
            r.u8()  # topic count
            if mode == 0:
                if not r.string():
                    problems.append(f"{name}/payload: the node name is empty")
            else:
                r.u32()  # node name hash
        elif layout == "topic":
            if r.u8() != 0x04:
                problems.append(f"{name}/payload: the opcode is not TOPIC")
            r.u8()  # index
            if r.u8() < 16:
                problems.append(f"{name}/payload: the topic id is in the reserved range")
            if r.u8() & ~0x0F:
                problems.append(f"{name}/payload: the flags byte has a reserved bit set")
            r.take(8)  # type hash
            if not r.string():
                problems.append(f"{name}/payload: the type name is empty")
            # The topic name is a string in a full handshake and a hash in
            # a compact one. A TOPIC carries no mode byte, so the vector's
            # name says which it is.
            if frame_name.endswith("_compact"):
                r.u32()  # topic name hash
            elif not r.string():
                problems.append(f"{name}/payload: the topic name is empty")
        elif layout == "heartbeat":
            pass  # the host's heartbeat has no payload
        elif layout == "mcu_heartbeat":
            for _ in range(4):
                r.u16()  # rx frames, crc errors, rx overflows, dropped frames
        elif layout == "topic_req":
            if r.u8() != 0x03:
                problems.append(f"{name}/payload: the opcode is not TOPIC_REQ")
            r.u8()  # index
        elif layout == "topic_status":
            if r.u8() != 0x05:
                problems.append(f"{name}/payload: the opcode is not TOPIC_STATUS")
            r.u8()  # topic id
            if r.u8() > 3:
                problems.append(f"{name}/payload: the status is not one the spec gives")
        elif layout == "ping":
            if r.u8() != 0x01:
                problems.append(f"{name}/payload: the opcode is not PING")
            r.u8()  # ping id
        elif layout == "pong":
            if r.u8() != 0x02:
                problems.append(f"{name}/payload: the opcode is not PONG")
            r.u8()  # ping id
            r.u32()  # tick
        elif layout == "correction":
            if r.u8() != 0x03:
                problems.append(f"{name}/payload: the opcode is not CORRECTION")
            r.u32()  # tick0
            r.i32()  # sec
            if r.u32() >= 1000000000:
                problems.append(f"{name}/payload: nanosec is not below 10^9")
            if abs(r.i16()) > 20000:
                problems.append(f"{name}/payload: the drift is outside 20000 ppm")
        elif layout == "log":
            if r.u8() not in (10, 20, 30, 40, 50):
                problems.append(f"{name}/payload: the level is not a ROS severity")
            r.string()  # text
            kind = r.u8()
            if kind > 5:
                problems.append(f"{name}/payload: the value kind is not one the spec gives")
            elif kind in (1, 2):
                r.take(2)
            elif kind in (3, 4, 5):
                r.take(4)
        elif layout == "error_report":
            r.u8()  # code
            r.u8()  # topic id
            r.u16()  # count
            r.u8()  # field
        if r.pos != len(payload):
            problems.append(f"{name}/payload: {len(payload) - r.pos} bytes were not read")
    except ValueError as e:
        problems.append(f"{name}/payload: {e}")
    return problems


def check_tree(vectors: Path) -> list[str]:
    validators = _schemas()
    problems = []
    for path in sorted(p for p in vectors.rglob("*") if p.is_file() and p.name != ".gitkeep"):
        name = path.relative_to(vectors).as_posix()
        problems += check_file(path, name, validators)
        if name.startswith("frames/") and path.suffix == ".yaml":
            try:
                data = yaml.safe_load(path.read_text())
            except yaml.YAMLError:
                continue
            if isinstance(data, dict):
                problems += check_frame(name, data)
    return problems


def main() -> int:
    problems = check_tree(ROOT / "spec" / "vectors")
    for p in problems:
        print(p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
