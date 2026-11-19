#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Run a walkthrough page's commands as it prints them.

A page that walks a reader from their own `.msg` to a running board carries
blocks the check owns: files to create, commands to run, the bridge to start
and the topic to read. Each block carries its marker on the line above its
fence, so the check never guesses which fence is a command and which is prose:

    <!-- docs-walkthrough: file firmware/lib.txt -->
    <!-- docs-walkthrough: run -->
    <!-- docs-walkthrough: serve -->
    <!-- docs-walkthrough: live -->

`file` writes the fence's text under the walkthrough home, `run` runs the fence
as a shell script there, `serve` holds the one long-running bridge launch and
`live` the one `ros2 topic echo` a caller replays once a board answers.

The bridge and the echo need hardware, so the run of those two is the caller's:
`--emit DIR` writes each as a script, with `--port` replacing the serve block's
`port:=` argument, and the caller starts the first and repeats the second.

Usage:
  docs_walkthrough.py --page PATH                      parse and report
  docs_walkthrough.py --page PATH --run --home DIR     write files, run commands
  docs_walkthrough.py --page PATH --emit DIR [--port PORT]
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

MARKER = re.compile(
    r'^ {0,3}<!-- docs-walkthrough:[ \t]*(file|run|serve|live)[ \t]*([^>]*?)[ \t]*-->[ \t]*$'
)
FENCE = re.compile(r'^ {0,3}```([A-Za-z0-9+#-]+)?[ \t]*$')
# Any marker-shaped line, however deep: a marker past column 3 is literal text to
# a Markdown renderer, so the scan reports it instead of reading it as one.
MARKER_LIKE = re.compile(r'^ {4,}<!-- docs-walkthrough:.*-->[ \t]*$')
# The one command each long-running kind may hold: a block that starts a
# second process, lists the graph or publishes would check the host rather
# than the page.
LAUNCH = re.compile(r'^\s*ros2 launch r2s_bridge ')
ECHO = re.compile(r'^\s*ros2 topic echo (/[A-Za-z0-9_/]+) ')
PORT = re.compile(r'port:=\S+')
SHELL_TAGS = ('sh', 'console')


@dataclass
class Block:
    """One block the page owns: what it is, which line, and its text."""

    kind: str
    line: int
    code: str
    path: str | None = None
    topic: str | None = None


def _refuse_path(path: str) -> str | None:
    if not path:
        return 'a file marker names the path it writes, as `file firmware/lib.txt`'
    if Path(path).is_absolute():
        return f'{path!r} is not a path under the walkthrough home'
    if '..' in Path(path).parts:
        return f'{path!r} climbs out of the walkthrough home'
    return None


def parse(text: str) -> tuple[list[Block], list[str]]:
    """Extract the marked blocks; problems, empty when every marker has a block.

    A marker binds the fence on the line below it and nothing else, so a page
    that loses a fence fails instead of attaching the marker to the next code
    sample. A marker indented past column 3 is literal text to a Markdown
    renderer, so it is reported rather than silently dropped.
    """
    lines = text.splitlines()
    blocks: list[Block] = []
    problems: list[str] = []
    saw_marker = False
    i = 0
    while i < len(lines):
        marker = MARKER.match(lines[i])
        if marker is None:
            if MARKER_LIKE.match(lines[i]):
                problems.append(
                    f'line {i + 1}: a docs-walkthrough marker starts past column 3, '
                    'so it is literal text'
                )
                saw_marker = True
            i += 1
            continue
        saw_marker = True
        kind, at, detail = marker.group(1), i + 1, marker.group(2).strip()
        named = detail or kind
        if i + 1 >= len(lines) or not FENCE.match(lines[i + 1]):
            problems.append(f'line {at}: the marker naming {named} sits above no fence')
            i += 1
            continue
        fence = FENCE.match(lines[i + 1])
        body = []
        i += 2
        while i < len(lines) and not FENCE.match(lines[i]):
            body.append(lines[i])
            i += 1
        if i == len(lines):
            problems.append(f'line {at}: the fence after {named} never closes')
            break
        i += 1
        code = '\n'.join(body) + '\n'
        if kind == 'file':
            problem = _refuse_path(detail)
            if problem is not None:
                problems.append(f'line {at}: {problem}')
                continue
            blocks.append(Block('file', at, code, path=detail))
            continue
        if (fence.group(1) or '') not in SHELL_TAGS:
            problems.append(f'line {at}: a {kind} block is shell, so its fence is tagged sh')
            continue
        if kind == 'run':
            blocks.append(Block('run', at, code))
            continue
        if kind == 'serve':
            if len(LAUNCH.findall(code)) != 1 or len(code.splitlines()) != 1:
                problems.append(
                    f'line {at}: a serve block holds one `ros2 launch r2s_bridge` line'
                )
                continue
            blocks.append(Block('serve', at, code.strip()))
            continue
        topics = ECHO.findall(code)
        if len(topics) != 1 or len(code.splitlines()) != 1:
            problems.append(f'line {at}: a live block holds one `ros2 topic echo <topic>` line')
            continue
        blocks.append(Block('live', at, code.strip(), topic=topics[0]))
    if not blocks and not saw_marker:
        problems.append('the page marks no block; add a docs-walkthrough marker')
    return blocks, problems


def write_files(blocks: list[Block], home: Path) -> None:
    """Write every file block under home, as the page prints it."""
    for block in blocks:
        if block.kind != 'file' or block.path is None:
            continue
        target = home / block.path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(block.code, encoding='utf-8')


def execute(block: Block, cwd: Path, timeout: int, command: str | None = None) -> str | None:
    """Run one block as a shell script in cwd; a failure names the block's line."""
    env = dict(os.environ)
    env['HOME'] = str(cwd)
    shown = command or block.code.strip()
    # -e stops at the failing step and pipefail keeps a pipeline's status, but
    # no -u: a sourced ROS setup script reads unset variables by design.
    try:
        done = subprocess.run(
            ['bash', '-eo', 'pipefail', '-c', command or block.code],
            cwd=cwd,
            env=env,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return f'line {block.line}: timeout after {timeout}s: {shown}'
    if done.returncode == 0:
        return None
    return (
        f'line {block.line}: exit {done.returncode}: {shown}\n{done.stdout}{done.stderr}'.rstrip()
    )


def emitted(blocks: list[Block], directory: Path, port: str | None) -> list[str]:
    """Write the serve and live blocks as scripts; problems, empty when green.

    A page holds at most one of each kind, because the caller starts one
    bridge and repeats one echo. A second block is reported rather than
    overwriting the first, so an added block cannot go unexercised.
    """
    problems: list[str] = []
    written: dict[str, Block] = {}
    for block in blocks:
        if block.kind not in ('serve', 'live'):
            continue
        if block.kind in written:
            problems.append(
                f'line {block.line}: a second {block.kind} block; the page may hold one'
            )
            continue
        written[block.kind] = block
    for kind, block in written.items():
        command = block.code
        if kind == 'serve':
            if PORT.search(command) is None:
                problems.append(
                    f'line {block.line}: a serve block names the port it runs on, `port:=...`'
                )
                continue
            if port is None:
                problems.append('--port names the serial port the serve block runs on')
                continue
            command = PORT.sub(f'port:={port}', command)
        directory.mkdir(parents=True, exist_ok=True)
        (directory / f'{kind}.sh').write_text(command + '\n', encoding='utf-8')
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description='Run a walkthrough page`s commands as it prints them.',
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument('--page', required=True, help='the page holding the markers')
    parser.add_argument('--run', action='store_true', help='run the run blocks in page order')
    parser.add_argument(
        '--home', help='the directory the blocks run in, a temporary one by default'
    )
    parser.add_argument(
        '--timeout', type=int, default=1800, help='seconds each run block may take'
    )
    parser.add_argument('--emit', metavar='DIR', help='write the serve and live blocks as scripts')
    parser.add_argument('--port', help='the serial port the serve block runs on')
    args = parser.parse_args(argv)

    page = Path(args.page)
    blocks, problems = parse(page.read_text(encoding='utf-8'))
    for problem in problems:
        print(f'{page}: {problem}', file=sys.stderr)
    if problems:
        return 1
    if args.emit:
        problems = emitted(blocks, Path(args.emit), args.port)
        for problem in problems:
            print(f'{page}: {problem}', file=sys.stderr)
        if problems:
            return 1
    if not args.run:
        print(f'{page}: {len(blocks)} blocks parsed')
        return 0

    home = Path(args.home) if args.home else Path(tempfile.mkdtemp(prefix='r2s-walkthrough-'))
    home.mkdir(parents=True, exist_ok=True)
    status = 0
    # Page order: a file is written where the page prints it, so a command that
    # reads it finds it, and one that must not find it yet still does not.
    for block in blocks:
        if block.kind == 'file':
            write_files([block], home)
            continue
        if block.kind != 'run':
            continue
        failure = execute(block, home, args.timeout)
        if failure is not None:
            print(f'{page}: {failure}', file=sys.stderr)
            status = 1
            break
    print(f'{page}: {len(blocks)} blocks, home {home}')
    if not args.home and status == 0:
        shutil.rmtree(home, ignore_errors=True)
    return status


if __name__ == '__main__':
    sys.exit(main())
