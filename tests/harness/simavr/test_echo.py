"""The simavr harness's own test: echo firmware on an emulated Uno and Mega.

`tests/harness/simavr/run.sh` builds `r2s_simavr` and the firmware images and
points `R2S_SIMAVR_BIN` and `R2S_SIMAVR_FW` at them; the suite runs this file
through that script, so a bare pytest run fails on the missing build.
"""

import os
import select
import subprocess
import time

import pytest
import serial

HARNESS = os.environ.get("R2S_SIMAVR_BIN", "")
FIRMWARE_DIR = os.environ.get("R2S_SIMAVR_FW", "")
SRAM_BYTES = {"atmega328p": 2048, "atmega2560": 8192}
START_TIMEOUT = 10.0
STOP_TIMEOUT = 10.0


def harness_binary():
    assert HARNESS, "R2S_SIMAVR_BIN is unset: run tests/harness/simavr/run.sh, which builds it"
    assert os.path.exists(HARNESS), f"no harness at {HARNESS!r}: run tests/harness/simavr/run.sh"
    return HARNESS


def firmware_image(image, mcu):
    assert FIRMWARE_DIR, "R2S_SIMAVR_FW is unset: run tests/harness/simavr/run.sh, which builds the firmware"
    path = os.path.join(FIRMWARE_DIR, f"{image}-{mcu}.elf")
    assert os.path.exists(path), f"no firmware at {path!r}: run tests/harness/simavr/run.sh"
    return path


class Harness:
    """A running simulator, with the pty path it announced on startup."""

    def __init__(self, mcu, *options, image="echo"):
        self.mcu = mcu
        self.summary = {}
        self.returncode = None
        self._output = []
        self._buffer = b""
        self._proc = subprocess.Popen(
            [harness_binary(), "--mcu", mcu, "--firmware", firmware_image(image, mcu), *options],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            bufsize=0,
        )
        try:
            self.pty = self._await_pty()
        except Exception:
            self._proc.kill()
            self._proc.wait()
            raise

    def _read_line(self, timeout):
        """One line of output, read from the pipe's descriptor.

        select plus a buffered reader would lose lines the reader had already
        taken, so the bytes go through this buffer instead.
        """
        deadline = time.monotonic() + timeout
        while b"\n" not in self._buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            ready, _, _ = select.select([self._proc.stdout.fileno()], [], [], remaining)
            if not ready:
                return None
            chunk = os.read(self._proc.stdout.fileno(), 4096)
            if not chunk:
                return None
            self._buffer += chunk
        line, _, self._buffer = self._buffer.partition(b"\n")
        return line.decode() + "\n"

    def _await_pty(self):
        deadline = time.monotonic() + START_TIMEOUT
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError("no pty line from the harness:\n" + "".join(self._output))
            line = self._read_line(remaining)
            if not line:
                code = self._proc.wait()
                raise AssertionError(
                    f"the harness exited with {code} before the pty was ready:\n" + "".join(self._output)
                )
            self._output.append(line)
            if line.startswith("r2s_simavr: pty="):
                return line.strip().split("=", 1)[1]

    def _record(self, tail):
        tail = tail.decode()
        self._output.append(tail)
        self.returncode = self._proc.returncode
        for line in tail.splitlines():
            if line.startswith("r2s_simavr: stop"):
                for field in line.split()[2:]:
                    key, _, value = field.partition("=")
                    self.summary[key] = int(value)

    def wait(self):
        """Wait for the harness to stop on its own, as the run limit makes it."""
        self._record(self._proc.communicate(timeout=STOP_TIMEOUT)[0])

    def stop(self):
        if self.returncode is not None:
            return self.returncode
        if self._proc.poll() is None:
            self._proc.terminate()
        try:
            tail, _ = self._proc.communicate(timeout=STOP_TIMEOUT)
        except subprocess.TimeoutExpired as expired:
            self._proc.kill()
            tail, _ = self._proc.communicate()
            raise AssertionError("the harness ignored SIGTERM:\n" + "".join(self._output) + tail.decode()) from expired
        self._record(tail)
        return self.returncode

    def __enter__(self):
        return self

    def __exit__(self, *exc_info):
        self.stop()
        return False


def read_exactly(port, count, timeout=5.0):
    got = bytearray()
    deadline = time.monotonic() + timeout
    while len(got) < count and time.monotonic() < deadline:
        got += port.read(count - len(got))
    return bytes(got)


@pytest.mark.parametrize("mcu", sorted(SRAM_BYTES))
def test_echo_returns_every_byte_value(mcu):
    payload = bytes(range(256))
    with Harness(mcu) as harness:
        with serial.Serial(harness.pty, 115200, timeout=2) as port:
            port.write(payload)
            echoed = read_exactly(port, len(payload))
    assert echoed == payload
    assert harness.returncode == 0


@pytest.mark.parametrize("mcu", sorted(SRAM_BYTES))
def test_the_mcu_runs_at_the_requested_frequency(mcu):
    # avr_init resets the clock from the core table, so a harness that sets it
    # too early runs the firmware 16 times slower than it was built for.
    harness = Harness(mcu, "--run-ms", "200")
    try:
        harness.wait()
    finally:
        harness.stop()
    frequency = harness.summary["frequency"]
    assert frequency == 16_000_000
    assert harness.summary["cycles"] * 1000 // frequency == harness.summary["simulated_ms"]


def test_echo_carries_more_than_the_input_ring():
    # The ring holds 4095 bytes and the UART FIFO 64, so a longer burst has to
    # come back paced by the pty rather than dropped. Pacing it from both ends
    # also keeps the pty's own buffers from filling.
    payload = bytes(range(256)) * 20
    with Harness("atmega328p") as harness:
        with serial.Serial(harness.pty, 115200, timeout=1) as port:
            echoed = bytearray()
            sent = 0
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline and (sent < len(payload) or len(echoed) < len(payload)):
                if sent < len(payload):
                    sent += port.write(payload[sent : sent + 512])
                if len(echoed) < len(payload):
                    echoed += port.read(512)
    assert bytes(echoed) == payload


@pytest.mark.parametrize("mcu", sorted(SRAM_BYTES))
def test_stack_paint_tracks_call_depth(mcu):
    harness = Harness(mcu, "--stack-paint", "--run-ms", "300", image="deep")
    try:
        harness.wait()
    finally:
        harness.stop()
    assert 150 < harness.summary["stack_peak"] < SRAM_BYTES[mcu]


@pytest.mark.parametrize("mcu", sorted(SRAM_BYTES))
def test_stack_paint_reports_the_high_water_mark(mcu):
    with Harness(mcu, "--stack-paint") as harness:
        with serial.Serial(harness.pty, 115200, timeout=2) as port:
            port.write(b"\x5a")
            assert read_exactly(port, 1) == b"\x5a"
    peak = harness.summary["stack_peak"]
    assert 0 < peak < SRAM_BYTES[mcu]


def test_pace_keeps_simulated_time_on_the_wall_clock():
    # No client connects, so the firmware polls its UART. Paced, the run limit
    # stops it with about the same simulated time; unpaced it runs ahead.
    harness = Harness("atmega328p", "--pace", "--run-ms", "300")
    try:
        harness.wait()
    finally:
        harness.stop()
    assert harness.returncode == 3
    assert harness.summary["simulated_ms"] <= harness.summary["wall_ms"] + 100


def test_an_unknown_mcu_is_rejected():
    result = subprocess.run(
        [harness_binary(), "--mcu", "atmega999", "--firmware", firmware_image("echo", "atmega328p")],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 2
    assert "atmega999" in result.stderr


def test_a_missing_firmware_is_rejected():
    assert FIRMWARE_DIR, "R2S_SIMAVR_FW is unset: run tests/harness/simavr/run.sh"
    missing = os.path.join(FIRMWARE_DIR, "absent.elf")
    result = subprocess.run(
        [harness_binary(), "--mcu", "atmega328p", "--firmware", missing],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 2
    assert missing in result.stderr
