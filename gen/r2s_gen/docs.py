# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""
The published page each generator message points a reader at.

A bound refusal is the moment a reader asks where the field's capacity came
from, so the message carries the page that answers it, and the commands that
take a bound name it in their help. The URL names a page tools/site_build.py
publishes, and tools/docs_drift.py fails when it stops matching one, so a
renamed page breaks the gate instead of shipping a dead pointer.
"""

import argparse

BOUNDS_URL = 'https://apojomovsky.github.io/ros2serial/message-bounds.html'


def note(parser: argparse.ArgumentParser) -> argparse.ArgumentParser:
    """
    Append the bounds section to a subcommand's help.

    Every command that takes a bound installs this, so the one page that
    explains where a capacity comes from is reachable from the help as well
    as from the refusal.

    :param parser: the subcommand's parser, with its flags already added.
    :return: the same parser, for the caller to return.
    """
    parser.epilog = f'Message bounds: {BOUNDS_URL}'
    return parser
