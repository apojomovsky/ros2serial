#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""One command that validates a real board end to end.

Compiles the button_led example for the board, uploads it, starts the bridge,
waits for the handshake, samples /button for half a minute, reads the link
counters, drives /led and asks the person at the board whether it lit, then
prints one report block with the sizes, the handshake, the rate, the counters
and a verdict. The report goes to stdout, so it survives a copy; progress and
the LED question go to stderr.

The device reaches the container through `make hardware-check`, which passes
it in with its own group: the caller's uid need not be in dialout. Boards,
their FQBNs and their size-tool prefixes come from ports/arduino/boards.list,
so a new row there needs no code change here.
"""

import argparse
import os
import shlex
import signal
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from size_report import SizeError  # noqa: E402
from size_report import parse_avr_size  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
# The example that exercises both directions: a 10 Hz /button publisher and an
# /led subscription driving LED_BUILTIN.
SKETCH = 'examples/arduino_button_led'
BOARDS_LIST = ROOT / 'ports/arduino/boards.list'
VERSIONS_ENV = ROOT / 'ci/versions.env'


def load_boards(path=None):
    """Read the board list into FQBN and size-tool prefix tables.

    Args:
        path: the boards.list file to read.

    Returns:
        A pair of dicts keyed by board name: the FQBNs, then the prefixes.
    """
    wanted = Path(path) if path is not None else BOARDS_LIST
    fqbns = {}
    prefixes = {}
    for number, line in enumerate(wanted.read_text(encoding='utf-8').splitlines(), 1):
        parts = line.split()
        if not parts or parts[0].startswith('#'):
            continue
        if len(parts) < 3:
            raise ValueError(f'{wanted}:{number} names no FQBN and prefix: {line!r}')
        fqbns[parts[0]] = parts[1]
        prefixes[parts[0]] = parts[2]
    return fqbns, prefixes


BOARDS, BOARD_PREFIXES = load_boards()
# The diagnostics status the bridge labels with this task carries the link
# state in its message and every counter in its values.
LINK_TASK = 'link'
# The sketch republishes the debounced state every 100 ms while connected, so
# half a minute holds about 300 messages at the 10 Hz the README documents.
SAMPLE_SECONDS = 30.0
RATE_HZ = 10.0
RATE_TOLERANCE_HZ = 2.0
GAP_MS = 300.0
HANDSHAKE_BUDGET_S = 30.0

FIELD_ORDER = (
    'board',
    'port',
    'firmware',
    'core',
    'flash',
    'ram',
    'handshake',
    'button rate',
    'longest gap',
    'crc errors',
    'rx overflows',
    'dropped frames',
    'sequence gaps',
    'dropped topic frames',
    'led',
)
# How long an ESP32 USB-Serial/JTAG re-enumeration may take after the upload.
PORT_WAIT_S = 10.0


class CheckError(Exception):
    """The run cannot produce a verdict: a device, a build or a launch failed."""


@dataclass(frozen=True)
class Sample:
    """What the /button arrivals say about the published rate."""

    count: int
    hz: float
    gap_ms: float


@dataclass(frozen=True)
class Counters:
    """The loss counters a clean link keeps at zero."""

    crc_errors: int = 0
    rx_overflows: int = 0
    dropped_frames: int = 0
    seq_gaps: int = 0
    topic_drops: int = 0


@dataclass(frozen=True)
class Observation:
    """Everything one sample window observed at the ports."""

    arrivals: list
    window_end: float
    diagnostics: dict | None
    handshake_s: float
    led_lit: bool | None


def arrivals(stamps, window_end=None):
    """The count, rate and longest gap of the arrival times, in seconds.

    The rate measures the publication interval, so a slow board reads below
    10 Hz whatever the window. The gap is the longest silence, and it counts
    the tail of the window too: a board that stops publishing midway must
    fail, not look healthy on the messages it managed.
    """
    if len(stamps) < 2:
        noun = 'message' if len(stamps) == 1 else 'messages'
        raise CheckError(f'the sample holds {len(stamps)} /button {noun}, not enough for a rate')
    span = stamps[-1] - stamps[0]
    if span <= 0.0:
        raise CheckError(f'the sample of {len(stamps)} /button messages spans no time')
    edges = list(stamps[1:]) + ([window_end] if window_end is not None else [])
    gap = max(later - earlier for earlier, later in zip(stamps, edges, strict=False))
    return Sample(len(stamps), (len(stamps) - 1) / span, gap * 1000.0)


def _counter(values, key):
    text = values[key]
    try:
        return int(text)
    except ValueError:
        raise CheckError(f'the link diagnostics report {key}: {text!r}') from None


def counters(values):
    """The loss counters summed from one diagnostics status.

    Every key ending in " seq gaps" or " dropped" belongs to one topic, so the
    per-topic keys sum into the run's figure. Publish failures stay out: they
    count a ROS endpoint refusing a message, not a loss on the wire.
    """
    missing = [
        key
        for key in ('mcu crc errors', 'mcu rx overflows', 'mcu dropped frames')
        if key not in values
    ]
    if missing:
        raise CheckError('the link diagnostics name no ' + ', '.join(missing))
    return Counters(
        crc_errors=_counter(values, 'mcu crc errors'),
        rx_overflows=_counter(values, 'mcu rx overflows'),
        dropped_frames=_counter(values, 'mcu dropped frames'),
        seq_gaps=sum(_counter(values, key) for key in values if key.endswith(' seq gaps')),
        topic_drops=sum(_counter(values, key) for key in values if key.endswith(' dropped')),
    )


def is_esp32(board):
    """Whether the board builds against the ESP32 Arduino core.

    Args:
        board: the board name from the board list.

    Returns:
        True for the ESP32 rows of the list, False for AVR ones.
    """
    return BOARDS.get(board, '').startswith('esp32:')


def size_tool(board):
    """The Berkeley size tool for the board's toolchain.

    Args:
        board: the board name from the board list.

    Returns:
        The `<prefix>-size` name the list rows for the board.
    """
    return f'{BOARD_PREFIXES[board]}-size'


def core_for_board(board, versions_path=None):
    """The `core:` report field for the board's pinned Arduino core.

    Args:
        board: the board name from the board list.
        versions_path: the versions.env file to read.

    Returns:
        The core coordinate with its pinned version.
    """
    wanted = Path(versions_path) if versions_path is not None else VERSIONS_ENV
    pins = {}
    for line in wanted.read_text(encoding='utf-8').splitlines():
        if '=' in line and not line.startswith('#'):
            name, _, value = line.partition('=')
            pins[name.strip()] = value.strip()
    pin = 'ARDUINO_ESP32_CORE_VERSION' if is_esp32(board) else 'ARDUINO_AVR_CORE_VERSION'
    if pin not in pins:
        raise CheckError(f'{wanted} names no {pin}')
    if is_esp32(board):
        return f'esp32:esp32@{pins[pin]}'
    return f'arduino:avr@{pins[pin]}'


def wait_for_port(port, timeout_s=PORT_WAIT_S, poll_s=0.2):
    """Wait for the device node to exist and open after an ESP32 upload.

    The USB-Serial/JTAG re-enumerates after the reset, so the node the upload
    used may be gone for a moment. Retries opening the same node until it
    opens or the budget runs out.

    Args:
        port: the serial device node to wait for.
        timeout_s: how long to keep retrying.
        poll_s: how long to sleep between tries.

    Raises:
        CheckError: the node never came back.
    """
    deadline = time.monotonic() + timeout_s
    while True:
        if os.path.exists(port):
            try:
                with open(port, 'rb'):
                    return
            except OSError:
                pass
        if time.monotonic() >= deadline:
            raise CheckError(
                'the port did not come back after the upload: '
                'a board which re-enumerates under another name needs '
                'PORT=/dev/serial/by-id/... and a rerun'
            )
        time.sleep(poll_s)


def failures(link, sample, rate_min, rate_max, gap_ms, led_lit):
    """Every reason the run fails, in report order.

    sample is the Sample the window produced, or the CheckError that stopped
    it from being measured. led_lit is None when the board has no LED to
    check, and a skipped LED never fails the run.
    """
    reasons = []
    if link.crc_errors:
        reasons.append(f'crc errors: the MCU dropped {link.crc_errors} frames for a bad CRC')
    if link.rx_overflows:
        reasons.append(f'rx overflows: {link.rx_overflows} frames exceeded the MCU buffer')
    if link.dropped_frames:
        reasons.append(f'dropped frames: the MCU dropped {link.dropped_frames}')
    if link.seq_gaps:
        reasons.append(f'sequence gaps: the host missed {link.seq_gaps} publishes')
    if link.topic_drops:
        reasons.append(f'dropped topic frames: the bridge dropped {link.topic_drops}')
    # A sample too short to measure is itself the reason the rate is unknown,
    # so it stands where the rate and the gap checks would.
    if isinstance(sample, CheckError):
        reasons.append(str(sample))
    else:
        if not rate_min <= sample.hz <= rate_max:
            reasons.append(
                f'the button rate is {sample.hz:.1f} Hz, outside {rate_min:.1f} to '
                f'{rate_max:.1f} Hz'
            )
        if sample.gap_ms > gap_ms:
            reasons.append(f'{sample.gap_ms:.0f} ms with no /button message, over {gap_ms:.0f} ms')
    if led_lit is False:
        reasons.append('the LED did not light when /led went true')
    return reasons


def window_observation(arrivals, window_end, diagnostics, handshake_s, led_lit):
    """The sample window's Observation, with the arrivals copied at its end.

    The LED phase keeps pumping the node afterwards, and /button keeps
    arriving, so the window's own list is copied here: the reported count and
    rate describe the sample, never the messages the question took.
    """
    return Observation(tuple(arrivals), window_end, diagnostics, handshake_s, led_lit)


def link_failures(observed):
    """The reasons the link itself gives, for a run that never connected."""
    if observed.handshake_s is not None:
        return []
    return [
        f'the bridge reported no connected link within {HANDSHAKE_BUDGET_S:.0f} s: check that '
        'the sketch is the flashed one and no serial monitor holds the port'
    ]


def format_report(values, failed):
    """The paste-able report block: every field, each failure, then the verdict.

    A skipped LED keeps the run green but says so: the verdict then reads
    `PASS (LED not checked)`.
    """
    lines = ['== ros2serial hardware check']
    lines += [f'{name}: {values.get(name, "n/a")}' for name in FIELD_ORDER]
    lines += [f'failed: {reason}' for reason in failed]
    if failed:
        lines.append('verdict: FAIL')
    elif values.get('led') == 'skipped':
        lines.append('verdict: PASS (LED not checked)')
    else:
        lines.append('verdict: PASS')
    return '\n'.join(lines) + '\n'


def say(message):
    """Progress on stderr, so stdout carries the report alone."""
    print(f'hardware-check: {message}', file=sys.stderr, flush=True)


def build_dir():
    """The build directory the make targets share, keyed by this tree."""
    given = os.environ.get('R2S_BUILD_DIR')
    if given:
        return Path(given)
    cache = os.environ.get('R2S_CACHE_DIR') or str(Path.home() / '.cache' / 'ros2serial')
    return Path(cache) / ('build' + str(ROOT).replace('/', '-'))


def run(command, what, cwd=ROOT):
    """One step that must succeed; its last output lines explain a failure."""
    proc = subprocess.run(command, cwd=cwd, capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        tail = (proc.stderr or proc.stdout).strip().splitlines()[-3:]
        raise CheckError(f'{what} failed: ' + ' | '.join(tail))
    return proc.stdout


def sketch_name(sketch):
    """The sketch's Arduino name, which its ELF is built under."""
    return Path(sketch).name


def build_firmware(board, sketch, build):
    """Compile one sketch for the board and return the linked ELF."""
    out = build / 'hardware-check' / sketch_name(sketch)
    say(f'building {sketch} for {board}')
    run(
        [
            'bash',
            'ports/arduino/build.sh',
            '--board',
            board,
            '--sketch',
            sketch,
            '--lib',
            str(build / 'hardware-check-lib'),
            '--out',
            str(out),
        ],
        'the firmware build',
    )
    elf = out / f'{sketch_name(sketch)}.ino.elf'
    if not elf.is_file():
        raise CheckError(f'the build left no firmware at {elf}')
    return elf


def upload(board, port, sketch, build):
    """Flash the built firmware to the board.

    An ESP32 upload runs through the pinned core setup, so the upload uses
    the core scripts/esp32-core.sh fetches into the cache.
    """
    say(f'uploading to {port}')
    command = [
        'arduino-cli',
        'upload',
        '--port',
        port,
        '--fqbn',
        BOARDS[board],
        '--input-dir',
        str(build / 'hardware-check' / sketch_name(sketch)),
        sketch,
    ]
    if is_esp32(board):
        quoted = ' '.join(shlex.quote(part) for part in command[1:])
        run(
            ['bash', '-c', f'source scripts/esp32-core.sh && exec arduino-cli {quoted}'],
            'the upload',
        )
        return
    run(command, 'the upload')


def sizes(elf, board='uno'):
    """The flash and static RAM the linked firmware costs, in bytes.

    The Berkeley table reads the same for every toolchain, so the parser
    stays while the tool follows the board list prefix.

    Args:
        elf: the linked firmware to measure.
        board: the board name from the board list.
    """
    tool = size_tool(board)
    if is_esp32(board):
        # bash -c assigns its first extra argument to $0, so the ELF is $1,
        # the way launch_bridge above numbers its own arguments.
        inner = f'source scripts/esp32-core.sh && exec {tool} "$1"'
        command = ['bash', '-c', inner, 'hardware-check-size', str(elf)]
    else:
        command = [tool, str(elf)]
    try:
        return parse_avr_size(run(command, 'the size report'))
    except SizeError as error:
        raise CheckError(str(error)) from None


def ensure_install(build):
    """The bridge install a launch needs, naming the build when it is absent."""
    install = build / 'bridge' / 'install'
    if not (install / 'setup.bash').is_file():
        raise CheckError(f'no bridge install at {install}: the bridge build did not finish')
    return install


def build_bridge():
    """Build the bridge workspace the launch file runs from."""
    say('building the bridge')
    run(['bash', 'scripts/bridge-test.sh', '--build-only'], 'the bridge build')


def launch_bridge(port, install, log_path):
    """Start the bridge on the port through its installed launch file.

    The install joins a fresh bash rather than this interpreter: sourcing it
    here would change the environment of the check itself. The node's own
    output goes to log_path, so stdout carries the report and nothing else,
    and the log stays there to read when a run fails.
    """
    # ROS joins from /opt/ros when the caller runs outside a sourced shell,
    # which is what a plain container exec gives.
    inner = (
        'if [ -z "${AMENT_PREFIX_PATH:-}" ] && [ -f "/opt/ros/${ROS_DISTRO}/setup.bash" ]; then '
        'source "/opt/ros/${ROS_DISTRO}/setup.bash"; fi; '
        'source "$1"/setup.bash && exec ros2 launch r2s_bridge r2s_bridge.launch.py port:="$2"'
    )
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log = open(log_path, 'w', encoding='utf-8')  # noqa: SIM115
    return subprocess.Popen(
        ['bash', '-c', inner, 'r2s-bridge', str(install), port],
        cwd=ROOT,
        stdout=log,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )


def stop_bridge(proc):
    """Terminate the launch and the node it started, killing what lingers.

    The group is signalled even when the launch already exited: its node runs
    in the same session, and a node left holding the device fails the next run.
    """
    try:
        os.killpg(proc.pid, signal.SIGTERM)
    except ProcessLookupError:
        if proc.poll() is not None:
            return
    try:
        proc.wait(timeout=10.0)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        proc.wait(timeout=10.0)


def ask_led(board):
    """Ask the person at the board, which is the only reader of its LED.

    `s` means the board has no user LED on the sketch pin: only ESP32 boards
    accept it, since every AVR board in the list wires one.

    Args:
        board: the board name from the board list.

    Returns:
        True when the LED lit, False when it did not, None when skipped.
    """
    choices = '[y/n/s]' if is_esp32(board) else '[y/n]'
    while True:
        question = f'did the LED on the board light up? {choices}: '
        print(question, end='', file=sys.stderr, flush=True)
        answer = sys.stdin.readline().strip().lower()
        if answer in ('y', 'yes'):
            return True
        if answer in ('n', 'no', ''):
            # A bare Enter or a closed stdin counts as no
            # instead of asking forever.
            return False
        if answer in ('s', 'skip', 'skipped') and is_esp32(board):
            return None
        print(f'answer y, n{", s" if is_esp32(board) else ""}', file=sys.stderr, flush=True)


def spin_for(node, seconds):
    """Pump the node's callbacks for a fixed span."""
    import rclpy

    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        rclpy.spin_once(node, timeout_sec=0.1)


def probe(port, board, seconds, launched_at):
    """Wait for the handshake, sample /button, read the counters, drive /led."""
    import rclpy
    from diagnostic_msgs.msg import DiagnosticArray
    from rclpy.qos import QoSProfile
    from rclpy.qos import ReliabilityPolicy
    from std_msgs.msg import Bool

    state = {'arrivals': [], 'values': None, 'connected_at': None}
    rclpy.init()
    node = rclpy.create_node('r2s_hardware_check')
    try:
        # The bridge routes MCU topics best effort, so the probe matches it.
        best_effort = QoSProfile(depth=100, reliability=ReliabilityPolicy.BEST_EFFORT)

        def on_diagnostics(message):
            for status in message.status:
                if not status.name.endswith(': ' + LINK_TASK):
                    continue
                state['values'] = {entry.key: entry.value for entry in status.values}
                if status.message == 'connected' and state['connected_at'] is None:
                    state['connected_at'] = time.monotonic()

        node.create_subscription(DiagnosticArray, '/diagnostics', on_diagnostics, 10)
        node.create_subscription(
            Bool, '/button', lambda _msg: state['arrivals'].append(time.monotonic()), best_effort
        )
        led = node.create_publisher(Bool, '/led', 10)

        say(f'waiting for the handshake on {port}')
        deadline = time.monotonic() + HANDSHAKE_BUDGET_S
        while state['connected_at'] is None:
            if time.monotonic() > deadline:
                # A board holding the wrong sketch or a port held open by a
                # serial monitor is a failed check, so the report still prints.
                return window_observation(
                    state['arrivals'], time.monotonic(), state['values'], None, False
                )
            rclpy.spin_once(node, timeout_sec=0.2)
        handshake_s = state['connected_at'] - launched_at

        say(f'sampling /button for {seconds:.0f} s')
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            rclpy.spin_once(node, timeout_sec=0.2)
        sampled = window_observation(
            state['arrivals'], time.monotonic(), state['values'], handshake_s, False
        )

        say('driving /led true')
        led.publish(Bool(data=True))
        spin_for(node, 1.0)
        lit = ask_led(board)
        say('driving /led false')
        led.publish(Bool(data=False))
        spin_for(node, 0.5)
        return Observation(
            sampled.arrivals, sampled.window_end, sampled.diagnostics, sampled.handshake_s, lit
        )
    finally:
        node.destroy_node()
        try:
            rclpy.shutdown()
        except RuntimeError:
            pass


def check_port(port):
    """Refuse a device this process cannot open, before any work is spent."""
    if not os.path.exists(port):
        raise CheckError(
            f'no device at {port}: plug the board in and read `arduino-cli board list`'
        )
    if not os.access(port, os.R_OK | os.W_OK):
        raise CheckError(f'{port} is not readable and writable by uid {os.getuid()}')


def main(argv=None):
    """Run the check and print its report; 0 on PASS, 1 on FAIL, 2 on a stop."""
    parser = argparse.ArgumentParser(
        description='Check a real board against the button_led example end to end.'
    )
    parser.add_argument('--port', required=True, help='serial device the board is on')
    parser.add_argument('--board', required=True, choices=sorted(BOARDS), help='board to check')
    parser.add_argument(
        '--seconds',
        type=float,
        default=SAMPLE_SECONDS,
        help='how long to sample /button (default: 30)',
    )
    parser.add_argument(
        '--sketch',
        default=SKETCH,
        help='sketch to flash instead of the example (default: %(default)s)',
    )
    parser.add_argument(
        '--skip-flash',
        action='store_true',
        help='run against the firmware already on the port, as a simulator leaves it',
    )
    parser.add_argument(
        '--skip-bridge-build',
        action='store_true',
        help='use the bridge install already built',
    )
    args = parser.parse_args(argv)
    build = build_dir()
    values = {
        'board': args.board,
        'port': args.port,
        'firmware': f'{sketch_name(args.sketch)} {BOARDS[args.board]}',
        'core': core_for_board(args.board),
    }
    bridge = None
    try:
        check_port(args.port)
        elf = None if args.skip_flash else build_firmware(args.board, args.sketch, build)
        if not args.skip_bridge_build:
            build_bridge()
        install = ensure_install(build)
        # An earlier build of the same sketch still stands beside the rate
        # when --skip-flash runs against a simulator's copy of it.
        measured_elf = elf or build / 'hardware-check' / sketch_name(args.sketch) / (
            f'{sketch_name(args.sketch)}.ino.elf'
        )
        if measured_elf.is_file():
            measured = sizes(measured_elf, args.board)
            values['flash'] = f'{measured["flash"]} bytes'
            values['ram'] = f'{measured["ram"]} bytes'
        if elf is not None:
            upload(args.board, args.port, args.sketch, build)
            if is_esp32(args.board):
                say('waiting for the port to come back after the upload')
                wait_for_port(args.port)
        log_path = build / 'hardware-check' / 'bridge.log'
        say(f'starting the bridge on {args.port} (log: {log_path})')
        bridge = launch_bridge(args.port, install, log_path)
        observed = probe(args.port, args.board, args.seconds, time.monotonic())
        reasons = link_failures(observed)
        # No status means no counter was read, so every one stays n/a: a zero
        # there would claim a clean link that never reported.
        link = None if observed.diagnostics is None else counters(observed.diagnostics)
        if link is not None:
            values['crc errors'] = link.crc_errors
            values['rx overflows'] = link.rx_overflows
            values['dropped frames'] = link.dropped_frames
            values['sequence gaps'] = link.seq_gaps
            values['dropped topic frames'] = link.topic_drops
        if observed.handshake_s is None:
            # /led is only driven after the handshake, so a link that never
            # connected says nothing about the LED.
            values['led'] = 'not driven'
            values['handshake'] = 'never'
            values['button rate'] = 'n/a'
            values['longest gap'] = 'n/a'
            sample = None
        else:
            if observed.led_lit is None:
                values['led'] = 'skipped'
            else:
                values['led'] = 'lit' if observed.led_lit else 'did not light'
            values['handshake'] = f'{observed.handshake_s:.1f} s'
            if link is None:
                reasons.append('the bridge published no link diagnostics, so no counter was read')
            try:
                sample = arrivals(observed.arrivals, observed.window_end)
            except CheckError as error:
                sample = error
            if isinstance(sample, CheckError):
                values['button rate'] = 'n/a'
                values['longest gap'] = 'n/a'
            else:
                values['button rate'] = f'{sample.hz:.1f} Hz from {sample.count} messages'
                values['longest gap'] = f'{sample.gap_ms:.0f} ms'
        if sample is not None and link is not None:
            reasons += failures(
                link,
                sample,
                RATE_HZ - RATE_TOLERANCE_HZ,
                RATE_HZ + RATE_TOLERANCE_HZ,
                GAP_MS,
                observed.led_lit,
            )
        print(format_report(values, reasons), end='')
        return 1 if reasons else 0
    except CheckError as error:
        print(f'hardware-check: {error}', file=sys.stderr)
        print(
            f'nothing was checked; from the repository root run: make hardware-check '
            f'PORT={args.port} BOARD={args.board}',
            file=sys.stderr,
        )
        return 2
    finally:
        if bridge is not None:
            stop_bridge(bridge)


if __name__ == '__main__':
    sys.exit(main())
