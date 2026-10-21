#!/usr/bin/env python3
"""Validate every conformance vector against the JSON Schema of its kind.

The kind is the directory under spec/vectors/; its schema is
spec/schema/vectors/<kind>.json. Exits 1 with one line per problem, naming the
file and the key path.
"""

import json
import sys
from pathlib import Path

import jsonschema
import yaml

ROOT = Path(__file__).resolve().parent.parent
SCHEMAS = ROOT / "spec" / "schema" / "vectors"


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
    return [f"{name}: {_location(e)}: {_message(e)}" for e in errors]


def check_tree(vectors: Path) -> list[str]:
    validators = _schemas()
    problems = []
    for path in sorted(p for p in vectors.rglob("*") if p.is_file() and p.name != ".gitkeep"):
        problems += check_file(path, path.relative_to(vectors).as_posix(), validators)
    return problems


def main() -> int:
    problems = check_tree(ROOT / "spec" / "vectors")
    for p in problems:
        print(p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
