"""The posix port's contract tests: the test firmware on a pty pair.

`ports/posix/run.sh` builds the firmware and points `R2S_POSIX_FW` at it,
then runs this file; a bare pytest run fails on the missing build. Every
test launches the firmware on a fresh pty pair with the boot ID file in a
temporary directory, and speaks the wire protocol through the reference
framing (`r2s_gen.reference.frames`), so a failure bisects to the firmware's
bytes before anyone reads its source.
"""

import os
import pty
import select
import signal
import struct
import subprocess
import time

from r2s_gen.reference import frames as wire

FIRMWARE = os.environ.get("R2S_POSIX_FW", "")
BOOT_ENV = "R2S_POSIX_BOOT_ID_FILE"
START_TIMEOUT = 10.0
FRAME_TIMEOUT = 5.0
STOP_TIMEOUT = 10.0

HELLO_OP = 0x02
TOPIC_OP = 0x04


def firmware_binary():
    assert FIRMWARE, "R2S_POSIX_FW is unset: run ports/posix/run.sh, which builds it"
    assert os.path.exists(FIRMWARE), f"no firmware at {FIRMWARE!r}: run ports/posix/run.sh"
    return FIRMWARE


# Bytes read from the pty past the end of the wanted frame. One test runs one
# firmware at a time, so a single buffer does, cleared on launch and on stop.
_PENDING = bytearray()


def read_frame(master, want=None, timeout=FRAME_TIMEOUT):
    """The next frame as (topic, seq, payload), skipping empty units and, when
    want is set, frames on other topics such as heartbeats. Bytes past the
    wanted frame stay buffered for the next call, since one read often holds
    several frames."""
    deadline = time.monotonic() + timeout
    while True:
        while _PENDING.find(b"\x00") < 0:
            remaining = deadline - time.monotonic()
            assert remaining > 0, "timed out waiting for a frame"
            ready, _, _ = select.select([master], [], [], remaining)
            assert ready, "timed out waiting for a frame"
            chunk = os.read(master, 4096)
            assert chunk, "the firmware closed the pty"
            _PENDING.extend(chunk)
        end = _PENDING.find(b"\x00")
        unit = bytes(_PENDING[: end + 1])
        del _PENDING[: end + 1]
        frame = wire.decode(unit)
        if frame is None:
            continue
        if want is None or frame[0] == want:
            return frame


def take_varint(payload, pos):
    value = 0
    shift = 0
    while True:
        assert pos < len(payload), "the payload ends mid-varint"
        byte = payload[pos]
        pos += 1
        value |= (byte & 0x7F) << shift
        if byte < 0x80:
            return value, pos
        shift += 7
        assert shift < 21, "a varint exceeds 3 bytes"


def take_string(payload, pos):
    length, pos = take_varint(payload, pos)
    end = pos + length
    assert end <= len(payload), "the payload ends mid-string"
    return payload[pos:end].decode(), end


def parse_hello(payload):
    """The HELLO fields in [CTL-16] order, after the opcode."""
    assert payload[0] == HELLO_OP, f"expected HELLO, got opcode {payload[0]:#04x}"
    boot = int.from_bytes(payload[3:7], "little")
    max_frame = int.from_bytes(payload[7:9], "little")
    rx_capacity = int.from_bytes(payload[9:11], "little")
    tick_hz = int.from_bytes(payload[11:15], "little")
    node, end = take_string(payload, 17)
    assert end == len(payload), "bytes after the node name"
    return {
        "version": payload[1],
        "mode": payload[2],
        "boot": boot,
        "max_frame": max_frame,
        "rx_capacity": rx_capacity,
        "tick_hz": tick_hz,
        "features": payload[15],
        "count": payload[16],
        "node": node,
    }


def parse_topic(payload):
    """The TOPIC fields in [CTL-16] order, after the opcode."""
    assert payload[0] == TOPIC_OP, f"expected TOPIC, got opcode {payload[0]:#04x}"
    type_name, pos = take_string(payload, 12)
    topic_name, pos = take_string(payload, pos)
    assert pos == len(payload), "bytes after the topic name"
    return {
        "index": payload[1],
        "id": payload[2],
        "flags": payload[3],
        "hash": payload[4:12],
        "type": type_name,
        "topic": topic_name,
    }


class Firmware:
    """A firmware process on a pty pair, with its stderr captured."""

    def __init__(self, *args, boot=None, keep_env=True):
        _PENDING.clear()
        self._master, slave = pty.openpty()
        try:
            cmd = [firmware_binary(), os.ttyname(slave), *args]
            env = dict(os.environ) if keep_env else {}
            if boot is not None:
                env[BOOT_ENV] = boot
            self._proc = subprocess.Popen(
                cmd,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env=env,
                bufsize=0,
            )
        finally:
            os.close(slave)
        self._error = bytearray()
        self.ready = self._await_ready()

    @property
    def master(self):
        return self._master

    def _read_error_line(self, timeout):
        deadline = time.monotonic() + timeout
        while b"\n" not in self._error:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            ready, _, _ = select.select([self._proc.stderr.fileno()], [], [], remaining)
            if not ready:
                if self._proc.poll() is not None:
                    return None
                continue
            chunk = os.read(self._proc.stderr.fileno(), 4096)
            if not chunk:
                return None
            self._error += chunk
        line, _, self._error = self._error.partition(b"\n")
        return line.decode() + "\n"

    def _await_ready(self):
        deadline = time.monotonic() + START_TIMEOUT
        while True:
            remaining = deadline - time.monotonic()
            assert remaining > 0, "the firmware never announced readiness"
            line = self._read_error_line(remaining)
            assert line is not None, "the firmware exited before announcing readiness"
            if line.startswith("r2s_posix: ready"):
                return line.strip()

    def next_line(self, timeout=FRAME_TIMEOUT):
        line = self._read_error_line(timeout)
        assert line is not None, "the firmware printed nothing"
        return line.strip()

    def stop(self):
        if self._proc.poll() is None:
            self._proc.terminate()
        try:
            self._proc.communicate(timeout=STOP_TIMEOUT)
        except subprocess.TimeoutExpired:
            self._proc.kill()
            self._proc.communicate()
            raise AssertionError("the firmware ignored SIGTERM") from None
        os.close(self._master)
        _PENDING.clear()
        return self._proc.returncode

    def __enter__(self):
        return self

    def __exit__(self, *exc_info):
        self.stop()
        return False


class Host:
    """The test's side of the link: numbered control frames into the pty."""

    def __init__(self, master):
        self.master = master
        self.seq = 0

    def send(self, topic, payload):
        os.write(self.master, wire.encode_wire(topic, self.seq, payload))
        self.seq = (self.seq + 1) % 256

    def hello_req(self):
        self.send(0, b"\x01\x01")

    def topic_req(self, index):
        self.send(0, b"\x03" + bytes((index,)))

    def topic_status(self, topic_id, status=0):
        self.send(0, b"\x05" + bytes((topic_id, status)))

    def ready(self):
        self.send(0, b"\x06")


def do_handshake(fw):
    """HELLO_REQ, one TOPIC_REQ per announced topic with an ok status each,
    then READY: returns the parsed HELLO and the TOPICs in index order."""
    host = Host(fw.master)
    _, _, payload = read_frame(fw.master, want=0)
    hello = parse_hello(payload)
    host.hello_req()
    topics = []
    for index in range(hello["count"]):
        host.topic_req(index)
        while True:
            _, _, payload = read_frame(fw.master, want=0)
            if payload[0] == TOPIC_OP:
                break
        parsed = parse_topic(payload)
        assert parsed["index"] == index, f"TOPIC answers index {parsed['index']}, not {index}"
        topics.append(parsed)
        host.topic_status(parsed["id"])
    host.ready()
    return hello, topics, host


# covers: SES-1
def test_hello_announces_firmware(tmp_path):
    with Firmware(boot=str(tmp_path / "boot")) as fw:
        _, _, payload = read_frame(fw.master, want=0)
        hello = parse_hello(payload)
    assert hello["version"] == 1
    assert hello["mode"] == 0
    assert hello["max_frame"] == 512
    assert hello["rx_capacity"] == 512 + 512 // 254 + 1
    # covers: CTL-8
    assert hello["tick_hz"] == 1000
    # covers: CTL-10
    assert hello["max_frame"] <= 65277
    # covers: CTL-2
    assert hello["features"] & 0xC0 == 0
    assert hello["count"] == 3
    assert hello["node"] == "r2s_posix"


# covers: SES-2, SES-6
def test_handshake_reaches_connected(tmp_path):
    with Firmware("--rate", "10", boot=str(tmp_path / "boot")) as fw:
        hello, topics, _host = do_handshake(fw)
        assert [t["topic"] for t in topics] == ["battery", "cmd_vel", "log_text"]
        assert [(t["type"], t["id"]) for t in topics] == [
            ("std_msgs/msg/Float32", 16),
            ("geometry_msgs/msg/Twist", 17),
            ("std_msgs/msg/String", 18),
        ]
        # covers: FRM-3
        assert all(16 <= t["id"] <= 255 for t in topics)
        # covers: CTL-4
        assert all(t["flags"] & 0xF0 == 0 for t in topics)
        assert [t["flags"] & 0x01 for t in topics] == [0, 1, 0]
        _, _, payload = read_frame(fw.master, want=4, timeout=2.0)
        assert len(payload) == 8, "the heartbeat carries four u16 counters"
    assert hello["node"] == "r2s_posix"


# covers: SES-10
def test_boot_id_increments_across_restarts(tmp_path):
    boot = str(tmp_path / "boot")
    first = boot_of(boot)
    second = boot_of(boot)
    assert second == (first + 1) % 2**32
    assert second != first


def boot_of(boot):
    fw = Firmware(boot=boot)
    try:
        _, _, payload = read_frame(fw.master, want=0)
        return parse_hello(payload)["boot"]
    finally:
        assert fw.stop() == -signal.SIGTERM


# covers: CTL-16
def test_configured_topics(tmp_path):
    args = ["--node", "custom", "--pub", "volt", "--sub", "drive", "--no-log-text"]
    with Firmware(*args, boot=str(tmp_path / "boot")) as fw:
        hello, topics, _host = do_handshake(fw)
        assert hello["node"] == "custom"
        assert hello["count"] == 2
        assert [(t["id"], t["topic"]) for t in topics] == [(16, "volt"), (17, "drive")]


# covers: FRM-4, FRM-5
def test_connected_publishes_battery(tmp_path):
    with Firmware("--rate", "20", boot=str(tmp_path / "boot")) as fw:
        _hello, _topics, _host = do_handshake(fw)
        values = []
        seqs = []
        for _ in range(3):
            _topic, seq, payload = read_frame(fw.master, want=16)
            assert len(payload) == 4
            values.append(struct.unpack("<f", payload)[0])
            seqs.append(seq)
    assert values == [0.0, 1.0, 2.0]
    assert seqs == [0, 1, 2]


def test_subscribes_twist(tmp_path):
    with Firmware("--rate", "1", boot=str(tmp_path / "boot")) as fw:
        _hello, _topics, host = do_handshake(fw)
        host.send(17, struct.pack("<6d", 1.0, 2.0, 3.0, 4.0, 5.0, 6.0))
        assert fw.next_line() == "r2s_posix: cmd_vel n=1"


def run_once(args, boot=None, keep_env=True):
    """One firmware run that must fail before it opens any port: returns
    (returncode, stderr) with no pty involved."""
    cmd = [firmware_binary(), "/dev/pts/does-not-exist", *args]
    env = dict(os.environ) if keep_env else {}
    if boot is not None:
        env[BOOT_ENV] = boot
    elif BOOT_ENV in env:
        del env[BOOT_ENV]
    proc = subprocess.run(cmd, stdin=subprocess.DEVNULL, capture_output=True, timeout=START_TIMEOUT, env=env)
    return proc.returncode, proc.stderr.decode()


def test_unknown_option_fails(tmp_path):
    code, error = run_once(["--bogus"], boot=str(tmp_path / "boot"))
    assert code == 2
    assert "--bogus" in error


def test_bad_rate_fails(tmp_path):
    code, error = run_once(["--rate", "0"], boot=str(tmp_path / "boot"))
    assert code == 2
    assert "--rate" in error


def test_missing_boot_file_env_fails(tmp_path):
    code, error = run_once([], keep_env=False)
    assert code == 2
    assert BOOT_ENV in error


def test_corrupt_boot_file_fails(tmp_path):
    boot = tmp_path / "boot"
    boot.write_text("not-a-number\n")
    code, error = run_once([], boot=str(boot))
    assert code == 2
    assert str(boot) in error


def test_unopenable_port_fails(tmp_path):
    code, error = run_once([], boot=str(tmp_path / "boot"))
    assert code == 1
    assert "does-not-exist" in error


def test_help_names_options():
    proc = subprocess.run(
        [firmware_binary(), "--help"], stdin=subprocess.DEVNULL, capture_output=True, timeout=START_TIMEOUT
    )
    assert proc.returncode == 0
    assert "--node" in proc.stdout.decode()
