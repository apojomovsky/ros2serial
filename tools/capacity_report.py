# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""How many topics and services fit on a board: measured, never guessed.

The tool builds a ladder of synthetic sketches (publishers, subscriptions,
services, and a mixed list carrying one Twist) for the runtime objects route
and the X-macro route, on each board, at the same explicit R2S_MAX_FRAME for
both. It then walks a mixed list up until a build stops linking, so the
ceiling the page states is a firmware that linked, not an extrapolation.
`measure` takes the numbers, `render` writes docs/capacity.md from them, and
`check` fails when the page or the sources behind it moved.
"""

import argparse
import hashlib
import json
import math
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PAGE = ROOT / 'docs' / 'capacity.md'
MEASUREMENTS = ROOT / 'tools' / 'capacity_measurements.json'

SCHEMA = 1
ROUTES = ('runtime', 'xmacro')
BOARDS = ('uno', 'mega')
# The two limits the page compares: the one a firmware caps itself at to fit
# an Uno beside its own buffers, and the one the bundled library emits.
FRAMES = {'128': 128, 'bundle': 325}
# The service channel keeps three frame-sized buffers, and 128-byte frames
# crash an Uno on the first QUERY, so a service firmware caps lower; this is
# the cap examples/arduino_service already builds at.
SERVICE_FRAME = 64
TOPIC_COUNTS = (1, 2, 4, 8, 16)
SERVICE_COUNTS = (1, 2, 4)
# The ceiling probe: a mixed list that grows until the board refuses it.
PROBE_COUNTS = (16, 32, 48, 64, 96, 128, 160, 192, 224, 240, 248, 255)
# Bytes each board reserves for the application's own code, libraries and the
# worst-case stack before the library's entities may claim the rest. The
# stack figure follows the budgets page, whose worst case for a shipped
# sketch is under a quarter of it.
ALLOWANCE = {'uno': 512, 'mega': 512}
ALLOWANCE_FIELD = 'application allowance'

STAGED_FILES = (
    'ports/arduino/r2s_arduino_port.cpp',
    'ports/arduino/r2s_arduino_port.h',
    'cpp/include/r2s/runtime.hpp',
    'cpp/src/runtime.cpp',
    'cpp/include/r2s/service.hpp',
    'cpp/src/service.cpp',
)
# What moves a measured byte: the core, the runtime layer, the port glue, the
# generator that emits the bundle, and this tool's own sketch templates.
DIGEST_ROOTS = ('core', 'cpp', 'ports/arduino', 'gen/r2s_gen')
# The pins alone, not the whole of ci/: a pin sets the code the boards are
# measured with, while the PIC gap lists in that directory have no say in an
# AVR measurement.
DIGEST_FILES = ('ci/versions.env',)

RUNTIME_SKETCH = """#include <ros2serial.h>
#include <std_msgs/msg/float32.hpp>
#include <geometry_msgs/msg/twist.hpp>

{callbacks}
r2s::Node node("capacity");
{publishers}
{subscriptions}
void setup()
{{
    node.begin(Serial, 115200);
}}

void loop()
{{
{body}    node.spin_some();
}}
"""

XM_SKETCH = """#define R2S_NODE_NAME "capacity"
#define R2S_TOPICS(X) \\
{rows}
#include <ros2serial.h>

static uint32_t s_sent;
{callbacks}
void setup(void)
{{
    Serial.begin(115200);
    if (r2s_session_configure(R2S_NODE_NAME, r2s_topics, R2S_TOPIC_COUNT) != R2S_OK) {{
        return;
    }}
    r2s_init();
}}

void loop(void)
{{
    r2s_spin_some();
    if (r2s_connected() == 0u || s_sent != 0u) {{
        return;
    }}
    s_sent++;
{body}}}
"""

SERVICE_SKETCH = """#include <ros2serial.h>
#include <r2s/service.hpp>
#include <std_srvs/srv/trigger.hpp>

{callbacks}
r2s::Node node("capacity");
{servers}
void setup()
{{
    node.begin(Serial, 115200);
}}

void loop()
{{
    node.spin_some();
}}
"""


class CapacityError(Exception):
    """The measurement cannot run, or the numbers cannot be used as given."""


def shapes() -> tuple:
    """The ladders every route measures."""
    return ('publisher', 'subscription', 'mixed')


def split(shape: str, count: int) -> tuple:
    """Publishers, subscriptions and Twist subscriptions for one ladder step.

    A mixed list splits the count in half, publishers first, and its first
    subscription carries a Twist so the ladder pays for the largest bundled
    payload as well as the narrow one.
    """
    if shape == 'publisher':
        return count, 0, 0
    if shape == 'subscription':
        return 0, count, 0
    publishers = count // 2
    subscriptions = count - publishers
    return publishers, subscriptions, 1 if subscriptions else 0


def sketch(route: str, shape: str, count: int) -> str:
    """The synthetic sketch one ladder step builds."""
    if shape == 'service':
        return _service_sketch(count)
    publishers, subscriptions, twists = split(shape, count)
    callbacks, pubdecl, subdecl, body = [], [], [], []
    for index in range(publishers):
        body.append(f'    std_msgs::msg::Float32 p{index} = {{}};\n')
        if route == 'xmacro':
            body.append(f'    (void)r2s_publish_cap_p{index}(&p{index});\n')
        else:
            pubdecl.append(
                f'r2s::Publisher<std_msgs::msg::Float32> cap_p{index}(node, "cap_p{index}");'
            )
            body.append(f'    (void)cap_p{index}.publish(p{index});\n')
    for index in range(subscriptions):
        kind = 'geometry_msgs::msg::Twist' if index < twists else 'std_msgs::msg::Float32'
        callbacks.append(
            f'{_linkage(route)}void on_s{index}(const {kind} &msg)\n{{\n    (void)msg;\n}}'
        )
        if route == 'xmacro':
            subdecl.append('')
        else:
            subdecl.append(
                f'r2s::Subscription<{kind}> cap_s{index}(node, "cap_s{index}", on_s{index});'
            )
    if route == 'xmacro':
        rows = [
            f'    X(Publisher, std_msgs::msg::Float32, cap_p{index}, "cap_p{index}") \\'
            for index in range(publishers)
        ]
        rows += [
            f'    X(Subscription, {_kind(index, twists)}, cap_s{index}, "cap_s{index}",'
            f' on_s{index}) \\'
            for index in range(subscriptions)
        ]
        if not rows:
            return EMPTY_XM_SKETCH
        rows[-1] = rows[-1].rstrip(' \\')
        return XM_SKETCH.format(
            rows='\n'.join(rows), callbacks='\n'.join(callbacks), body=''.join(body)
        )
    return RUNTIME_SKETCH.format(
        callbacks='\n'.join(callbacks),
        publishers='\n'.join(pubdecl),
        subscriptions='\n'.join(subdecl),
        body=''.join(body),
    )


EMPTY_XM_SKETCH = """#define R2S_NODE_NAME "capacity"
#include <ros2serial.h>

void setup(void)
{
    Serial.begin(115200);
    r2s_init();
}

void loop(void) { r2s_spin_some(); }
"""


def _linkage(route: str) -> str:
    """The X-macro header carries C linkage, so only the runtime rows need static."""
    return '' if route == 'xmacro' else 'static '


def _kind(index: int, twists: int) -> str:
    return 'geometry_msgs::msg::Twist' if index < twists else 'std_msgs::msg::Float32'


def _service_sketch(count: int) -> str:
    callbacks, servers = [], []
    for index in range(count):
        callbacks.append(
            f'static void on_r{index}(const std_srvs::srv::Trigger::Request &req,\n'
            f'  std_srvs::srv::Trigger::Response &res)\n'
            f'{{\n    (void)req;\n    res.success = 1u;\n}}'
        )
        servers.append(
            f'r2s::Service<std_srvs::srv::Trigger> cap_r{index}'
            f'(node, "cap_r{index}", on_r{index});'
        )
    return SERVICE_SKETCH.format(callbacks='\n'.join(callbacks), servers='\n'.join(servers))


def fit(runs: list, metric: str) -> tuple | None:
    """The least-squares slope and intercept of one ladder, or None under two points.

    The zero row stays out: it is the base cost the count ladder starts from,
    and its topic table holds one slot rather than none.
    """
    points = [(run['count'], run[metric]) for run in runs if metric in run and run['count'] > 0]
    if len(points) < 2:
        return None
    total = len(points)
    mean_x = sum(x for x, _ in points) / total
    mean_y = sum(y for _, y in points) / total
    spread = sum((x - mean_x) ** 2 for x, _ in points)
    if spread == 0:
        return None
    slope = sum((x - mean_x) * (y - mean_y) for x, y in points) / spread
    return slope, mean_y - slope * mean_x


def capacity(marginal: float, base: float, usable: float) -> int:
    """How many entities fit in the RAM left after the base cost.

    Only whole entities count, and a marginal cost of zero or less fits
    nothing: the ladder measured no per-entity cost to divide by.
    """
    if marginal <= 0:
        return 0
    return max(0, int(math.floor((usable - base) / marginal)))


def input_digest(root: Path = ROOT) -> str:
    """A digest of everything that moves a measured byte.

    The tree's sources and this tool's build recipe, not its prose: editing a
    heading must not demand a remeasure, while a changed count list, frame
    limit or flag must.
    """
    digest = hashlib.sha256()
    paths = [root / name for name in DIGEST_FILES]
    for entry in DIGEST_ROOTS:
        base = root / entry
        if base.exists():
            paths.extend(
                path
                for path in sorted(base.rglob('*'))
                if path.is_file() and '__pycache__' not in path.parts
            )
    for path in paths:
        if not path.is_file():
            continue
        digest.update(str(path.relative_to(root)).encode())
        digest.update(b'\0')
        digest.update(path.read_bytes())
        digest.update(b'\0')
    recipe = repr(
        (
            BOARDS,
            FRAMES,
            SERVICE_FRAME,
            TOPIC_COUNTS,
            SERVICE_COUNTS,
            PROBE_COUNTS,
            defines(FRAMES['128'], 1),
            defines(FRAMES['bundle'], 16),
            defines(SERVICE_FRAME, 1, 0),
            defines(SERVICE_FRAME, 1, max(SERVICE_COUNTS)),
            # The templates themselves, through one sample sketch per shape
            # and route, so a changed body is a changed digest.
            [sketch(route, shape, 5) for route in ROUTES for shape in shapes()],
            sketch('runtime', 'service', 3),
        )
    )
    digest.update(recipe.encode())
    return digest.hexdigest()


def stage(root: Path, out: Path) -> Path:
    """Lay the bundled library plus the runtime layer into an Arduino library.

    The stage is rebuilt on every run: a reused copy would compile the ladder
    against sources the digest no longer names, which is the one way these
    numbers could describe a tree that does not exist.
    """
    if out.exists():
        shutil.rmtree(out)
    (out / 'src' / 'r2s').mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env['PYTHONPATH'] = str(root / 'gen')
    _run(
        [sys.executable, '-m', 'r2s_gen.cli', 'bundle', '--distro', 'jazzy', '--out', str(out)],
        cwd=root,
        env=env,
    )
    for header in sorted((root / 'core' / 'include' / 'r2s').glob('*.h')):
        shutil.copy2(header, out / 'src' / 'r2s' / header.name)
    for source in sorted((root / 'core' / 'src').glob('*.c')):
        if source.name != 'test_reset.c':
            shutil.copy2(source, out / 'src' / source.name)
    for name in STAGED_FILES:
        target = out / 'src' / ('r2s' if '/include/' in name else '')
        shutil.copy2(root / name, target / Path(name).name)
    return out


def _run(command: list, cwd: Path, env: dict | None = None) -> str:
    proc = subprocess.run(
        command, cwd=cwd, capture_output=True, text=True, env=env or dict(os.environ), check=False
    )
    if proc.returncode != 0:
        raise CapacityError(f'{" ".join(command)} failed: {(proc.stderr or proc.stdout)[-400:]}')
    return proc.stdout


def defines(frame: int, topics: int, services: int | None = None) -> str:
    """The compile flags one ladder step needs.

    A topic ladder sizes the runtime topic table to the count the step
    registers, so the step pays for its own slot. A service ladder turns the
    channel on and keeps the topic table at one slot while the service table
    grows, so its slope is the service's cost and not a topic table counted
    twice. Both routes compile at the same explicit frame limit.
    """
    flags = [
        '-DR2S_WITH_CODEC',
        f'-DR2S_MAX_FRAME={frame}',
        '-DR2S_MAX_NEST=4',
        f'-DR2S_TOPIC_MAX_COUNT={max(topics, 1)}',
    ]
    if services is not None:
        flags += ['-DR2S_WITH_SERVICES', f'-DR2S_SERVICE_MAX_COUNT={max(services, 1)}']
    return ' '.join(flags)


def build(
    root: Path,
    work: Path,
    library: Path,
    board: str,
    frame: int,
    name: str,
    text: str,
    services: int | None = None,
    topics: int = 1,
) -> dict | None:
    """Compile one sketch and measure it; None when the board refuses it."""
    sketch_dir = work / 'sketches' / name
    sketch_dir.mkdir(parents=True, exist_ok=True)
    (sketch_dir / f'{name}.ino').write_text(text, encoding='utf-8')
    flags = defines(frame, topics, services)
    proc = subprocess.run(
        [
            'arduino-cli',
            'compile',
            '--fqbn',
            f'arduino:avr:{board}',
            '--build-property',
            f'compiler.c.extra_flags={flags}',
            '--build-property',
            f'compiler.cpp.extra_flags={flags}',
            '--library',
            str(library),
            '--build-path',
            str(work / 'build' / name),
            '--output-dir',
            str(work / 'out' / name),
            str(sketch_dir),
        ],
        capture_output=True,
        text=True,
        env=dict(os.environ),
        check=False,
    )
    if proc.returncode != 0:
        return None
    sizes = parse_size(_run(['avr-size', str(work / 'out' / name / f'{name}.ino.elf')], cwd=root))
    sizes['limits'] = parse_limits(proc.stdout)
    return sizes


def parse_size(text: str) -> dict:
    """Flash and static RAM from an avr-size run, as the budgets page reads them."""
    for line in text.splitlines():
        fields = line.split()
        if len(fields) >= 6 and all(part.isdigit() for part in fields[:3]):
            code, data, bss = (int(part) for part in fields[:3])
            return {'flash': code + data, 'ram': data + bss}
    raise CapacityError(f'no avr-size table in: {text.strip()[:120]}')


def parse_limits(text: str) -> dict:
    """The board's flash and RAM ceilings, as the compiler reports them."""
    limits = {}
    flash = re.search(r'Maximum is (\d+) bytes', text)
    ram = re.search(r'Maximum is (\d+) bytes', text[flash.end() :] if flash else text)
    if flash:
        limits['flash'] = int(flash.group(1))
    if ram:
        limits['ram'] = int(ram.group(1))
    return limits


def measure(root: Path, work: Path) -> dict:
    """Build every ladder and the ceiling probe; the numbers the page renders."""
    library = stage(root, work / 'library')
    boards = {board: {} for board in BOARDS}
    topics, services, ceiling = [], [], []
    for board in BOARDS:
        for label, frame in FRAMES.items():
            for route in ROUTES:
                for shape in shapes():
                    for count in TOPIC_COUNTS:
                        name = f'{board}_{label}_{route}_{shape}_{count}'
                        sizes = build(
                            root,
                            work,
                            library,
                            board,
                            frame,
                            name,
                            sketch(route, shape, count),
                            topics=count,
                        )
                        if sizes is None:
                            continue
                        boards[board].update(sizes['limits'])
                        topics.append(_run_row(board, label, route, shape, count, sizes))
            # The library's fixed cost: a sketch that registers nothing.
            name = f'{board}_{label}_runtime_base'
            sizes = build(
                root,
                work,
                library,
                board,
                frame,
                name,
                sketch('runtime', 'publisher', 0),
                topics=0,
            )
            if sizes is not None:
                boards[board].update(sizes['limits'])
                topics.append(_run_row(board, label, 'runtime', 'publisher', 0, sizes))
            # The ceiling: a mixed list that grows until the board refuses it.
            for route in ROUTES:
                ceiling.append(_probe(root, work, library, board, label, frame, route, boards))
        # Services run at their own cap, once per board: the channel's three
        # frame buffers make a bigger frame a crash rather than a tight fit.
        for count in (0, *SERVICE_COUNTS):
            name = f'{board}_service_{count}'
            sizes = build(
                root,
                work,
                library,
                board,
                SERVICE_FRAME,
                name,
                sketch('runtime', 'service', count),
                services=count,
            )
            if sizes is None:
                continue
            boards[board].update(sizes['limits'])
            services.append(
                {
                    'board': board,
                    'count': count,
                    'flash': sizes['flash'],
                    'ram': sizes['ram'],
                }
            )
    return {
        'schema': SCHEMA,
        'inputs': input_digest(root),
        'boards': {board: boards[board] for board in BOARDS},
        'frames': FRAMES,
        'topics': topics,
        'services': services,
        'ceiling': ceiling,
    }


def _run_row(board: str, label: str, route: str, shape: str, count: int, sizes: dict) -> dict:
    """One measured run, keyed by the fields the page reads.

    The file keeps one compact object per line: the keys repeat on every row,
    so indenting them would triple a generated file nobody edits by hand.
    """
    return {
        'board': board,
        'frame': label,
        'route': route,
        'shape': shape,
        'count': count,
        'flash': sizes['flash'],
        'ram': sizes['ram'],
    }


def _probe(
    root: Path,
    work: Path,
    library: Path,
    board: str,
    label: str,
    frame: int,
    route: str,
    boards: dict,
) -> dict:
    """The largest mixed list that still links, found by building every step.

    The ladder stops at the first refusal, and also stops at the end of the
    probe: a step that linked at the last rung is a lower bound, never a
    ceiling, and the page marks it as one.
    """
    last = None
    refused = False
    for count in PROBE_COUNTS:
        name = f'{board}_{label}_{route}_probe_{count}'
        sizes = build(
            root,
            work,
            library,
            board,
            frame,
            name,
            sketch(route, 'mixed', count),
            topics=count,
        )
        if sizes is None:
            refused = True
            break
        boards[board].update(sizes['limits'])
        last = {'count': count, 'flash': sizes['flash'], 'ram': sizes['ram']}
    # A first rung that refuses is a ceiling too: the ladder stopped because
    # the board said no, not because the probe ran out of steps.
    row = {'board': board, 'frame': label, 'route': route, 'links': 0, 'ceiling': refused}
    if last:
        row.update(
            {
                'links': last['count'],
                'flash': last['flash'],
                'ram': last['ram'],
                'ceiling': refused,
            }
        )
    return row


def _ladder(rows: list, board: str, frame: str, route: str, shape: str) -> list:
    return sorted(
        (
            row
            for row in rows
            if row['board'] == board
            and row['frame'] == frame
            and row['route'] == route
            and row['shape'] == shape
        ),
        key=lambda row: row['count'],
    )


def _base(rows: list):
    return next((row for row in rows if row['count'] == 0), None)


def _marginal(rows: list, metric: str):
    fitted = fit(rows, metric)
    return None if fitted is None else fitted[0]


def _fmt(value) -> str:
    if value is None:
        return 'n/a'
    if isinstance(value, float):
        return f'{value:.1f}'
    return str(value)


NOTE = (
    '<!-- DO NOT EDIT: generated by tools/capacity_report.py from'
    ' tools/capacity_measurements.json. Refresh with make capacity-report. -->'
)

INTRO = """# How many topics and services fit

Measured firmware per board, frame limit and route, so the cost of one more
publisher, subscription or service reads off a table instead of a guess. The
[budgets page](budgets.md) measures the shipped examples; this page measures a
ladder of synthetic ones, and every ceiling below is a firmware that linked.
"""

METHOD = """## Method

`make capacity-report` builds one synthetic sketch per row: N topics split
evenly between publishers and subscriptions of `std_msgs/msg/Float32`, the
first subscription carrying a `geometry_msgs/msg/Twist`, once for the runtime
objects route and once for the X-macro route, on each board, at the same
explicit `R2S_MAX_FRAME`. Flash is text plus data and static RAM is data plus
bss, as `avr-size` prints them.

The marginal cost of one entity is the least-squares slope of its ladder and
the base cost is the measured build that registers nothing. A count fits when
`base + count * marginal <= RAM - allowance`, where the allowance is the
{allowance} reserved for the application's own code, libraries and stack.
"""

FITS = """## What fits

The runtime route, from each ladder's fitted numbers. A count is whole entities
that fit the RAM left after the allowance, so it is not a promise that a given
application compiles: its own code, libraries and stack are what the allowance
covers. The X-macro route costs less, and the section further down states the
difference at equal frame limits.
"""


def render_page(measurements: dict) -> str:
    """docs/capacity.md from the measurements, with the method and the formula."""
    boards = measurements['boards']
    values = sorted({ALLOWANCE[board] for board in boards})
    allowance = (
        f'{values[0]} bytes'
        if len(values) == 1
        else f'the {ALLOWANCE_FIELD} each board row states'
    )
    parts = [NOTE, '', INTRO, METHOD.format(allowance=allowance), FITS]
    for board in BOARDS:
        if board in boards:
            parts.append(_board_section(measurements, board))
    parts.append(_services_section(measurements))
    parts.append(_gap_section(measurements))
    parts.append(_ceiling_section(measurements))
    return '\n'.join(part.strip('\n') + '\n' for part in parts if part)


def _frame_label(label: str, frames: dict) -> str:
    """What a frame column reads: the byte count, naming the bundle's own when it is that."""
    return f'{frames[label]} bytes' + ('' if label.isdigit() else ' (the bundle limit)')


def _board_section(measurements: dict, board: str) -> str:
    limits = measurements['boards'][board]
    usable = limits['ram'] - ALLOWANCE[board]
    lines = [
        f'### {board.capitalize()}',
        '',
        f'Flash {limits["flash"]} bytes, RAM {limits["ram"]} bytes. The'
        f' {ALLOWANCE_FIELD} is {ALLOWANCE[board]} bytes, stack included, which'
        f' leaves {usable} bytes for the library and its entities.',
        '',
    ]
    for label in measurements['frames']:
        lines += [
            f'#### {_frame_label(label, measurements["frames"])}',
            '',
            '| entity | base RAM | marginal RAM | marginal flash | count that fits |',
            '| --- | --- | --- | --- | --- |',
        ]
        base = _base(_ladder(measurements['topics'], board, label, 'runtime', 'publisher'))
        for shape in ('publisher', 'subscription'):
            runs = _ladder(measurements['topics'], board, label, 'runtime', shape)
            ram, flash = _marginal(runs, 'ram'), _marginal(runs, 'flash')
            fits = capacity(ram, base['ram'], usable) if ram and base else 0
            lines.append(
                f'| {shape} | {base["ram"] if base else "n/a"}'
                f' | {_fmt(ram)} | {_fmt(flash)} | {fits} |'
            )
        lines.append('')
        if base:
            lines += [
                f'The library alone costs {base["ram"]} bytes of RAM and'
                f' {base["flash"]} bytes of flash at this limit.',
                '',
            ]
    lines.append('#### Every ladder, for the record')
    lines.append('')
    lines.append('| frame | route | shape | topics | flash | RAM |')
    lines.append('| --- | --- | --- | --- | --- | --- |')
    for label in measurements['frames']:
        for route in ROUTES:
            for shape in shapes():
                for row in _ladder(measurements['topics'], board, label, route, shape):
                    lines.append(
                        f'| {measurements["frames"][label]} | {route} | {shape}'
                        f' | {row["count"]} | {row["flash"]} | {row["ram"]} |'
                    )
    lines.append('')
    for label in measurements['frames']:
        for route in ROUTES:
            runs = _ladder(measurements['topics'], board, label, route, 'mixed')
            if not runs:
                continue
            label_text = _frame_label(label, measurements['frames'])
            lines.append(f'#### {route} route, mixed list, {label_text}')
            lines.append('')
            lines.append('| topics | flash | RAM |')
            lines.append('| --- | --- | --- |')
            for row in runs:
                lines.append(f'| {row["count"]} | {row["flash"]} | {row["ram"]} |')
            lines.append('')
    return '\n'.join(lines)


def _services_section(measurements: dict) -> str:
    if not measurements['services']:
        return ''
    lines = [
        '## Services',
        '',
        'The runtime route serving `std_srvs/srv/Trigger` at 64-byte frames, the'
        " cap the service channel's three frame buffers allow on an Uno. The"
        ' service table grows with the ladder while the topic table stays at one'
        ' slot, so the slope is the cost of the service alone, fitted over the'
        ' ladder that registers one, two and four.',
        '',
        '| board | per service RAM | per service flash | base RAM | count that fits |',
        '| --- | --- | --- | --- | --- |',
    ]
    for board in BOARDS:
        runs = [row for row in measurements['services'] if row['board'] == board]
        if not runs:
            continue
        marginal = _marginal(runs, 'ram')
        base = _base(runs)
        usable = measurements['boards'][board]['ram'] - ALLOWANCE[board]
        fits = capacity(marginal, base['ram'], usable) if marginal and base else 0
        lines.append(
            f'| {board} | {_fmt(marginal)} | {_fmt(_marginal(runs, "flash"))}'
            f' | {base["ram"] if base else "n/a"} | {fits} |'
        )
    lines.append('')
    lines.append('The measured ladder, for the record:')
    lines.append('')
    lines.append('| board | services | flash | RAM |')
    lines.append('| --- | --- | --- | --- |')
    for board in BOARDS:
        for row in sorted(
            (row for row in measurements['services'] if row['board'] == board),
            key=lambda row: row['count'],
        ):
            lines.append(f'| {board} | {row["count"]} | {row["flash"]} | {row["ram"]} |')
    lines.append('')
    return '\n'.join(lines)


def _gap_section(measurements: dict) -> str:
    lines = [
        '## Runtime route against the X-macro route',
        '',
        'The same mixed ladder on both routes at equal frame limits, so the gap'
        ' between the routes is a subtraction of measured firmware, not an'
        ' estimate. A positive gap is what the runtime route costs more.',
        '',
        '| board | frame | topics | runtime RAM | X-macro RAM | gap | runtime flash |'
        ' X-macro flash | gap |',
        '| --- | --- | --- | --- | --- | --- | --- | --- | --- |',
    ]
    frames = measurements['frames']
    for board in BOARDS:
        for label in frames:
            runtime = {
                row['count']: row
                for row in _ladder(measurements['topics'], board, label, 'runtime', 'mixed')
            }
            for row in _ladder(measurements['topics'], board, label, 'xmacro', 'mixed'):
                other = runtime.get(row['count'])
                if other is None:
                    continue
                lines.append(
                    f'| {board} | {frames[label]} | {row["count"]}'
                    f' | {other["ram"]} | {row["ram"]} | {other["ram"] - row["ram"]:+d}'
                    f' | {other["flash"]} | {row["flash"]}'
                    f' | {other["flash"] - row["flash"]:+d} |'
                )
    lines.append('')
    return '\n'.join(lines)


def _ceiling_section(measurements: dict) -> str:
    if not measurements.get('ceiling'):
        return ''
    lines = [
        '## The largest ladder that linked',
        '',
        'Each row is a firmware that compiled and fit its board, so the count is a'
        ' build result. A row whose count is a ceiling refused the next step of the'
        ' probe; a row that ran out of probe without a refusal is a lower bound,'
        ' marked `>=`, and says nothing about where the board actually stops.',
        '',
        'The X-macro route stops at 240 rows on any board because the bundled'
        ' header asserts `R2S_SLOT_COUNT <= 240`, so its 240 is that compile-time'
        ' bound and not the board running out of RAM. The runtime route carries no'
        ' such bound and stops where the board does.',
        '',
        '| board | frame | route | topics that linked | flash | RAM |',
        '| --- | --- | --- | --- | --- | --- |',
    ]
    frames = measurements['frames']
    for row in measurements['ceiling']:
        mark = '' if row.get('ceiling') else '>= '
        lines.append(
            f'| {row["board"]} | {frames[row["frame"]]} | {row["route"]}'
            f' | {mark}{row["links"]}'
            f' | {row.get("flash", "n/a")} | {row.get("ram", "n/a")} |'
        )
    lines.append('')
    return '\n'.join(lines)


def save(measurements: dict, path: Path) -> None:
    """Write the measurements where the page and the check read them.

    One run per line: the file is generated and never edited, so the line is
    the unit a reader diffs when two measurements of one run disagree.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(dumps(measurements), encoding='utf-8')


def dumps(measurements: dict) -> str:
    """The measurements as JSON text, one run per line."""
    lines = [
        '{',
        f'  "schema": {measurements["schema"]},',
        f'  "inputs": "{measurements["inputs"]}",',
        '  "frames": ' + json.dumps(measurements['frames'], sort_keys=True) + ',',
        '  "boards": ' + json.dumps(measurements['boards'], sort_keys=True) + ',',
    ]
    for name in ('topics', 'services', 'ceiling'):
        rows = measurements[name]
        lines.append(f'  "{name}": [')
        lines += [
            '    ' + json.dumps(row, sort_keys=True) + (',' if index < len(rows) - 1 else '')
            for index, row in enumerate(rows)
        ]
        lines.append('  ],' if name != 'ceiling' else '  ]')
    lines.append('}')
    return '\n'.join(lines) + '\n'


def load(path: Path) -> dict:
    """Read the measurements; anything but this schema is refused."""
    try:
        data = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError) as error:
        raise CapacityError(f'measurements {path} cannot be read: {error}') from error
    if data.get('schema') != SCHEMA:
        raise CapacityError(f'measurements {path} carry schema {data.get("schema")}, not {SCHEMA}')
    return data


def _name(path: Path, root: Path) -> str:
    """A path as the message names it: relative to the tree when it lives there."""
    try:
        return str(path.relative_to(root))
    except ValueError:
        return str(path)


def check(
    measurements_path: Path = MEASUREMENTS, page_path: Path = PAGE, root: Path = ROOT
) -> list:
    """Drift problems: a missing or stale page, or sources the numbers no longer match."""
    try:
        measurements = load(measurements_path)
    except CapacityError as error:
        return [str(error)]
    problems = []
    if not page_path.is_file():
        problems.append(f'{_name(page_path, root)}: missing')
    elif page_path.read_text(encoding='utf-8') != render_page(measurements):
        problems.append(f'{_name(page_path, root)}: stale, run make capacity-report and commit it')
    if measurements.get('inputs') != input_digest(root):
        problems.append(
            f'{_name(measurements_path, root)}: measured a different tree,'
            ' run make capacity-report and commit it'
        )
    return problems


def main(argv: list | None = None) -> int:
    """measure, render or check the capacity page."""
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    measure_cmd = sub.add_parser('measure', help='build the ladders and save the numbers')
    measure_cmd.add_argument('--build-dir', required=True)
    measure_cmd.add_argument('--out', default=str(MEASUREMENTS))
    measure_cmd.add_argument('--page', default=str(PAGE))
    render_cmd = sub.add_parser('render', help='write the page from the saved numbers')
    render_cmd.add_argument('--measurements', default=str(MEASUREMENTS))
    render_cmd.add_argument('--out', default=str(PAGE))
    check_cmd = sub.add_parser('check', help='fail on a stale page or moved sources')
    check_cmd.add_argument('--measurements', default=str(MEASUREMENTS))
    check_cmd.add_argument('--page', default=str(PAGE))
    args = parser.parse_args(argv)
    try:
        if args.command == 'measure':
            work = Path(args.build_dir) / 'capacity'
            work.mkdir(parents=True, exist_ok=True)
            measurements = measure(ROOT, work)
            save(measurements, Path(args.out))
            Path(args.page).write_text(render_page(measurements), encoding='utf-8')
            print(
                f'capacity: {len(measurements["topics"])} topic runs,'
                f' {len(measurements["services"])} service runs,'
                f' {len(measurements["ceiling"])} ceilings'
            )
            return 0
        if args.command == 'render':
            Path(args.out).write_text(render_page(load(Path(args.measurements))), encoding='utf-8')
            return 0
        problems = check(Path(args.measurements), Path(args.page))
    except CapacityError as error:
        print(f'capacity: error: {error}', file=sys.stderr)
        return 1
    for problem in problems:
        print(problem)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
