#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""One command that validates a real Uno or Mega end to end.

Compiles the button_led example for the board, uploads it, starts the bridge,
waits for the handshake, samples /button for half a minute, reads the link
counters, drives /led and asks the person at the board whether it lit, then
prints one report block with the sizes, the handshake, the rate, the counters
and a verdict. The report goes to stdout, so it survives a copy; progress and
the LED question go to stderr.

The device reaches the container through `make hardware-check`, which passes
it in with its own group: the caller's uid need not be in dialout.
"""

import argparse
import os
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
BOARDS = {'mega': 'arduino:avr:mega', 'uno': 'arduino:avr:uno'}
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
    led_lit: bool


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


def failures(link, sample, rate_min, rate_max, gap_ms, led_lit):
    """Every reason the run fails, in report order.

    sample is the Sample the window produced, or the CheckError that stopped
    it from being measured.
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
    if not led_lit:
        reasons.append('the LED did not light when /led went true')
    return reasons


def link_failures(observed):
    """The reasons the link itself gives, for a run that never connected."""
    if observed.handshake_s is not None:
        return []
    return [
        f'the bridge reported no connected link within {HANDSHAKE_BUDGET_S:.0f} s: check that '
        'the sketch is the flashed one and no serial monitor holds the port'
    ]


def format_report(values, failed):
    """The paste-able report block: every field, each failure, then the verdict."""
    lines = ['== ros2serial hardware check']
    lines += [f'{name}: {values.get(name, "n/a")}' for name in FIELD_ORDER]
    lines += [f'failed: {reason}' for reason in failed]
    lines.append('verdict: ' + ('FAIL' if failed else 'PASS'))
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
    """Flash the built firmware to the board."""
    say(f'uploading to {port}')
    run(
        [
            'arduino-cli',
            'upload',
            '--port',
            port,
            '--fqbn',
            BOARDS[board],
            '--input-dir',
            str(build / 'hardware-check' / sketch_name(sketch)),
            sketch,
        ],
        'the upload',
    )


def sizes(elf):
    """The flash and static RAM the linked firmware costs, in bytes."""
    try:
        return parse_avr_size(run(['avr-size', str(elf)], 'the size report'))
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


def ask_led():
    """Ask the person at the board, which is the only reader of its LED."""
    print('did the LED on the board light up? [y/n]: ', end='', file=sys.stderr, flush=True)
    return sys.stdin.readline().strip().lower() in ('y', 'yes')


def spin_for(node, seconds):
    """Pump the node's callbacks for a fixed span."""
    import rclpy

    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        rclpy.spin_once(node, timeout_sec=0.1)


def probe(port, seconds, launched_at):
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
                return Observation(
                    (),
                    time.monotonic(),
                    state['values'],
                    None,
                    False,
                )
            rclpy.spin_once(node, timeout_sec=0.2)
        handshake_s = state['connected_at'] - launched_at

        say(f'sampling /button for {seconds:.0f} s')
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            rclpy.spin_once(node, timeout_sec=0.2)
        window_end = time.monotonic()
        sampled = tuple(state['arrivals'])

        say('driving /led true')
        led.publish(Bool(data=True))
        spin_for(node, 1.0)
        lit = ask_led()
        say('driving /led false')
        led.publish(Bool(data=False))
        spin_for(node, 0.5)
        return Observation(sampled, window_end, state['values'], handshake_s, lit)
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
        description='Check a real Uno or Mega against the button_led example end to end.'
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
            measured = sizes(measured_elf)
            values['flash'] = f'{measured["flash"]} bytes'
            values['ram'] = f'{measured["ram"]} bytes'
        if elf is not None:
            upload(args.board, args.port, args.sketch, build)
        log_path = build / 'hardware-check' / 'bridge.log'
        say(f'starting the bridge on {args.port} (log: {log_path})')
        bridge = launch_bridge(args.port, install, log_path)
        observed = probe(args.port, args.seconds, time.monotonic())
        reasons = link_failures(observed)
        if observed.diagnostics is None:
            link = Counters()
            if not reasons:
                reasons = ['the bridge published no link diagnostics, so no counter was read']
        else:
            link = counters(observed.diagnostics)
        values['crc errors'] = link.crc_errors
        values['rx overflows'] = link.rx_overflows
        values['dropped frames'] = link.dropped_frames
        values['sequence gaps'] = link.seq_gaps
        values['dropped topic frames'] = link.topic_drops
        values['led'] = 'lit' if observed.led_lit else 'did not light'
        if observed.handshake_s is None:
            values['handshake'] = 'never'
            values['button rate'] = 'n/a'
            values['longest gap'] = 'n/a'
            sample = None
        else:
            values['handshake'] = f'{observed.handshake_s:.1f} s'
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
        if sample is not None:
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
