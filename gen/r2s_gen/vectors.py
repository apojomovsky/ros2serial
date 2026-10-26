"""The `vectors` subcommand: payload hex from the reference codec for one message.

Reads message definitions like every other subcommand and encodes with the
reference, so its output is what a message vector writes in `payload`.
"""

from __future__ import annotations

import argparse
import sys
from collections.abc import Mapping
from pathlib import Path

from r2s_gen import msgs
from r2s_gen.reference import messages


def add_parser(subparsers: argparse._SubParsersAction) -> argparse.ArgumentParser:
    """Add the `vectors` subcommand to the command line."""
    parser = subparsers.add_parser(
        "vectors",
        help="print the payload hex the reference codec makes for a message",
        description="Encode one message with the reference codec and print its payload hex, "
        "in the form a message vector writes it.",
    )
    parser.add_argument("--type", required=True, help="the message type as package/msg/Type")
    parser.add_argument("--message", required=True, help="the message as YAML text, or @path to read it from a file")
    parser.add_argument(
        "--msg-path", action="append", default=[], metavar="DIR", help="search DIR for message packages; repeatable"
    )
    parser.add_argument(
        "--distro", default="jazzy", choices=("jazzy", "lyrical", "rolling"), help="the snapshot distro (default jazzy)"
    )
    parser.add_argument(
        "--narrow", action="store_true", help="encode float64 fields as binary32, as a narrow topic does"
    )
    parser.set_defaults(run=run)
    return parser


def run(args: argparse.Namespace) -> int:
    """Encode the message and print its payload hex; a bad input is exit 1."""
    try:
        values = _read_message(args.message)
        repository = msgs.MessageRepository(msg_paths=args.msg_path, distro=args.distro)
        payload = messages.encode(repository, args.type, values, narrow=args.narrow)
    except (msgs.MessageError, ValueError) as error:
        print(f"r2s-gen: error: {error}", file=sys.stderr)
        return 1
    print(payload.hex(" "))
    return 0


def _read_message(spec: str) -> Mapping:
    """The message mapping `spec` holds, inline or behind @path."""
    if spec.startswith("@"):
        path = Path(spec[1:])
        try:
            text = path.read_text(encoding="utf-8")
        except OSError as error:
            raise ValueError(f"{path}: cannot read the message file ({error}); check the path") from error
    else:
        text = spec
    try:
        import yaml
    except ImportError as error:
        raise ValueError("vectors needs PyYAML; install it with pip install pyyaml") from error
    try:
        values = yaml.safe_load(text)
    except yaml.YAMLError as error:
        raise ValueError(
            "--message: not valid YAML; write the message as a YAML mapping, or @path to read it from a file"
        ) from error
    if not isinstance(values, Mapping):
        raise ValueError(
            "--message: expected a mapping of field names to values; "
            "write the message as a YAML mapping, or @path to read it from a file"
        )
    return values
