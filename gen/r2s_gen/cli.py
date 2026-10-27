"""The r2s-gen command line: the parser, the subcommands, and the exit codes.

The subcommands that generate code land with the tickets that implement them;
this module is the shell every one of them hangs off.
"""

import argparse
import sys

from r2s_gen import config
from r2s_gen import vectors
from r2s_gen.version import PROTOCOL_VERSION, __version__


def _print_version(_args: argparse.Namespace) -> int:
    print(f"r2s-gen {__version__}")
    print(f"protocol version {PROTOCOL_VERSION}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    """The parser with every subcommand the tool has today."""
    parser = argparse.ArgumentParser(
        prog="r2s-gen",
        description="Generate the ros2serial firmware sources from .msg files and r2s.yaml.",
    )
    subparsers = parser.add_subparsers(dest="command", required=True, metavar="command")
    version = subparsers.add_parser("version", help="print the generator version and the protocol version")
    version.set_defaults(run=_print_version)
    config.add_parser(subparsers)
    vectors.add_parser(subparsers)
    return parser


def main(argv: list[str] | None = None) -> int:
    """Run the command line and return the exit code; a usage error is 2, as argparse reports it."""
    try:
        args = build_parser().parse_args(argv)
    except SystemExit as error:
        return int(error.code or 0)
    return int(args.run(args))


if __name__ == "__main__":
    sys.exit(main())
