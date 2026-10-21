#!/usr/bin/env python3
"""Validate every conformance vector against the JSON Schema of its kind.

The kind is the directory under spec/vectors/; its schema is
spec/schema/vectors/<kind>.json. A frame vector is also checked against the
reference codec, so that the bytes it carries are the bytes the reference
makes. Exits 1 with one line per problem, naming the file and the key path.
"""

import json
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
        frame = bytes.fromhex(data["frame"])
        wire = bytes.fromhex(data["wire"])
        topic_id = data["topic_id"]
        seq = data["seq"]
        body = bytes((topic_id, seq)) + payload
    except (KeyError, TypeError, ValueError) as e:
        # The schema reports the field that is out of range; this only has to
        # not crash on it.
        return [] if isinstance(e, ValueError) else [f"{name}: the frame fields are not usable: {e}"]

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
    problems += check_channel(name, data, topic_id)
    return problems


# The system channels of spec/protocol.md section 7, and the range a user
# topic takes.
SYSTEM_CHANNELS = {
    "hello_req": 0,
    "hello": 0,
    "ready": 0,
    "topic": 0,
    "ping": 1,
    "pong": 1,
    "host_heartbeat": 4,
    "reserved_channel": None,  # any id from 7 to 15
}


def check_channel(name: str, data: dict, topic_id: int) -> list[str]:
    """Check that the frame is on the channel its name says."""
    vector_name = data.get("name")
    if vector_name not in SYSTEM_CHANNELS:
        return []
    wanted = SYSTEM_CHANNELS[vector_name]
    if wanted is None:
        if not 7 <= topic_id <= 15:
            return [f"{name}/topic_id: {topic_id} is not in the reserved range 7 to 15"]
        return []
    if topic_id != wanted:
        return [f"{name}/topic_id: {topic_id} is not the channel {wanted} of this vector"]
    return []


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
