# Copyright (c) 2026 Alexis Pojomovsky
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

"""The r2s-gen command line: the parser, the subcommands, and the exit codes.

The subcommands that generate code land with the tickets that implement them;
this module is the shell every one of them hangs off.
"""

import argparse
import sys

from r2s_gen import bundle
from r2s_gen import config
from r2s_gen import layouts
from r2s_gen import vectors
from r2s_gen.version import PROTOCOL_VERSION
from r2s_gen.version import __version__


def _print_version(_args: argparse.Namespace) -> int:
    print(f'r2s-gen {__version__}')
    print(f'protocol version {PROTOCOL_VERSION}')
    return 0


def build_parser() -> argparse.ArgumentParser:
    """The parser with every subcommand the tool has today."""
    parser = argparse.ArgumentParser(
        prog='r2s-gen',
        description='Generate the ros2serial firmware sources from .msg files and r2s.yaml.',
    )
    subparsers = parser.add_subparsers(dest='command', required=True, metavar='command')
    version = subparsers.add_parser(
        'version', help='print the generator version and the protocol version'
    )
    version.set_defaults(run=_print_version)
    config.add_parser(subparsers)
    layouts.add_parser(subparsers)
    bundle.add_parser(subparsers)
    vectors.add_parser(subparsers)
    return parser


def main(argv: list[str] | None = None) -> int:
    """Run the command line and return the exit code.

    A usage error is 2, as argparse reports it.
    """
    try:
        args = build_parser().parse_args(argv)
    except SystemExit as error:
        return int(error.code or 0)
    return int(args.run(args))


if __name__ == '__main__':
    sys.exit(main())
