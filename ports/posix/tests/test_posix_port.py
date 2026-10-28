"""The posix port's contract tests: the test firmware on a pty pair.

`ports/posix/run.sh` builds the firmware and points `R2S_POSIX_FW` at it,
then runs this file; a bare pytest run fails on the missing build. Every
test launches the firmware on a fresh pty pair, and speaks the wire protocol
through the reference framing (`r2s_gen.reference.frames`), so a failure
bytes before anyone reads its source.
"""

import dataclasses
import os
import pty
import select
import shutil
import signal
import struct
import subprocess
import time
from pathlib import Path

from r2s_gen import bounds, config, msgs, topics
from r2s_gen.reference import frames as wire

FIRMWARE = os.environ.get("R2S_POSIX_FW", "")
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
    token = int.from_bytes(payload[3:7], "little")
    max_frame = int.from_bytes(payload[7:9], "little")
    rx_capacity = int.from_bytes(payload[9:11], "little")
    tick_hz = int.from_bytes(payload[11:15], "little")
    node, end = take_string(payload, 17)
    assert end == len(payload), "bytes after the node name"
    return {
        "version": payload[1],
        "mode": payload[2],
        "token": token,
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

    def __init__(self, *args):
        _PENDING.clear()
        self._master, slave = pty.openpty()
        try:
            cmd = [firmware_binary(), os.ttyname(slave), *args]
            env = dict(os.environ)
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
        self.seq = {}
        self.next_token = 1

    def send(self, topic, payload):
        """One frame on topic, with that topic's own seq counter ([FRM-4])."""
        seq = self.seq.get(topic, 0)
        os.write(self.master, wire.encode_wire(topic, seq, payload))
        self.seq[topic] = (seq + 1) % 256

    def hello_req(self):
        self.send(0, b"\x01\x02")

    def topic_req(self, index):
        self.send(0, b"\x03" + bytes((index,)))

    def topic_status(self, topic_id, status=0):
        self.send(0, b"\x05" + bytes((topic_id, status)))

    def ready(self):
        """READY with the next nonzero session token ([SES-10], [SES-17])."""
        token = self.next_token
        self.next_token = self.next_token % 0xFFFFFFFF + 1
        self.send(0, b"\x06" + struct.pack("<I", token))
        return token


def do_handshake(fw, host=None):
    """HELLO_REQ, one TOPIC_REQ per announced topic with an ok status each,
    then READY: returns the parsed HELLO and the TOPICs in index order. The
    host models the bridge side, so a rebooted firmware reuses the same one
    and its token counter keeps counting."""
    host = host or Host(fw.master)
    host.master = fw.master
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


# covers: SES-1, SES-10
def test_hello_announces_firmware(tmp_path):
    with Firmware() as fw:
        _, _, payload = read_frame(fw.master, want=0)
        hello = parse_hello(payload)
    assert hello["version"] == 2
    assert hello["mode"] == 0
    assert hello["token"] == 0, "no token before the first READY"
    assert hello["max_frame"] == 52
    assert hello["rx_capacity"] == 52 + 52 // 254 + 1
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
    with Firmware("--rate", "10") as fw:
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


# covers: SES-10, SES-19, SES-21
def test_restarted_firmware_is_detected_as_a_reset(tmp_path):
    fw = Firmware("--rate", "20")
    hello, _topics, host = do_handshake(fw)
    assigned = host.next_token - 1
    assert hello["token"] == 0, "no token before the first READY"
    assert assigned != 0
    assert fw.stop() == -signal.SIGTERM
    with Firmware("--rate", "20") as fw:
        _, _, payload = read_frame(fw.master, want=0)
        rebooted = parse_hello(payload)
        assert rebooted["token"] == 0, "a reboot wipes the RAM token"
        assert rebooted["token"] != assigned, "the host tells the reboot from a reconnect"
        _hello, _topics, host = do_handshake(fw, host)
        assert host.next_token - 1 != assigned, "the re-handshake assigns a fresh token"
        _topic, _seq, payload = read_frame(fw.master, want=16)
        assert len(payload) == 4, "the re-handshake connects again"


# covers: CTL-16
def test_configured_topics(tmp_path):
    args = ["--node", "custom", "--pub", "volt", "--sub", "drive", "--no-log-text"]
    with Firmware(*args) as fw:
        hello, topics, _host = do_handshake(fw)
        assert hello["node"] == "custom"
        assert hello["count"] == 2
        assert [(t["id"], t["topic"]) for t in topics] == [(16, "volt"), (17, "drive")]


# covers: FRM-4, FRM-5
def test_connected_publishes_battery(tmp_path):
    with Firmware("--rate", "20") as fw:
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


# covers: SES-9, SES-10
def test_connected_publishes_log_text(tmp_path):
    with Firmware("--rate", "20") as fw:
        _hello, _topics, host = do_handshake(fw)
        assigned = host.next_token - 1
        _topic, _seq, payload = read_frame(fw.master, want=18)
        text, end = take_string(payload, 0)
        assert end == len(payload), "bytes after the log string"
        assert text == format(assigned, "08x"), "the boot line carries the assigned token"
        batteries = 0
        while True:
            topic, _seq, payload = read_frame(fw.master)
            if topic == 18:
                repeat, end = take_string(payload, 0)
                assert end == len(payload), "bytes after the log string"
                assert repeat, "the periodic log line is empty"
                return
            if topic == 16:
                batteries += 1
            assert batteries <= 10, "no periodic log_text within 10 battery publishes"


# covers: SES-9
def test_log_text_boot_line_retried_after_reject(tmp_path):
    with Firmware("--rate", "20") as fw:
        host = Host(fw.master)
        _topic, _seq, payload = read_frame(fw.master, want=0)
        hello = parse_hello(payload)
        host.hello_req()
        for index in range(hello["count"]):
            host.topic_req(index)
            while True:
                _topic, _seq, payload = read_frame(fw.master, want=0)
                if payload[0] == TOPIC_OP:
                    break
            parsed = parse_topic(payload)
            assert parsed["index"] == index
            host.topic_status(parsed["id"], 1 if parsed["id"] == 18 else 0)
        host.ready()
        quiet_until = time.monotonic() + 1.2
        while True:
            remaining = quiet_until - time.monotonic()
            if remaining <= 0:
                break
            try:
                topic, _seq, _payload = read_frame(fw.master, timeout=remaining)
            except AssertionError as error:
                assert "timed out" in str(error), f"lost the firmware: {error}"
                break
            assert topic != 18, "log_text published while the host rejects it"
        host.topic_status(18, 0)
        _topic, _seq, payload = read_frame(fw.master, want=18)
        text, end = take_string(payload, 0)
        assert end == len(payload), "bytes after the log string"
        assert text == format(host.next_token - 1, "08x")


def test_subscribes_twist(tmp_path):
    with Firmware("--rate", "1") as fw:
        _hello, _topics, host = do_handshake(fw)
        host.send(17, struct.pack("<6d", 1.0, 2.0, 3.0, 4.0, 5.0, 6.0))
        assert fw.next_line() == "r2s_posix: cmd_vel n=1"


def run_once(args):
    """One firmware run that must fail: returns (returncode, stderr) with no
    pty involved."""
    cmd = [firmware_binary(), "/dev/pts/does-not-exist", *args]
    proc = subprocess.run(cmd, stdin=subprocess.DEVNULL, capture_output=True, timeout=START_TIMEOUT)
    return proc.returncode, proc.stderr.decode()


def test_unknown_option_fails(tmp_path):
    code, error = run_once(["--bogus"])
    assert code == 2
    assert "--bogus" in error


def test_bad_rate_fails(tmp_path):
    code, error = run_once(["--rate", "0"])
    assert code == 2
    assert "--rate" in error


LAUNCH_NAME_TYPES = [
    ("--pub", "std_msgs/msg/Float32"),
    ("--sub", "geometry_msgs/msg/Twist"),
    ("--log-text", "std_msgs/msg/String"),
]


def varint_len(value):
    if value < 128:
        return 1
    if value < 16384:
        return 2
    return 3


def topic_name_limit(type_name, max_frame):
    """Longest launch name fitting one TOPIC announce entry: the same sum
    the core gate refuses, from the frame limit the firmware announces."""
    budget = max_frame - 4
    fixed = 12 + varint_len(len(type_name)) + len(type_name)
    limit = budget - fixed - 1
    while limit > 0 and fixed + varint_len(limit) + limit > budget:
        limit -= 1
    return limit


def launch_name_cases(tmp_path):
    """Per-flag (flag, type, limit) from the frame limit this firmware build
    announces, so the boundary tests track the generated limits (G8)."""
    with Firmware() as fw:
        _, _, payload = read_frame(fw.master, want=0)
        max_frame = parse_hello(payload)["max_frame"]
    return [(flag, type_name, topic_name_limit(type_name, max_frame)) for flag, type_name in LAUNCH_NAME_TYPES]


def test_launch_topic_name_over_limit_fails(tmp_path):
    for flag, type_name, limit in launch_name_cases(tmp_path):
        name = "n" * (limit + 1)
        code, error = run_once([flag, name])
        assert code == 2
        assert flag in error
        assert name in error
        assert str(limit + 1) in error
        assert f"limit {limit}" in error
        assert type_name in error


def test_launch_topic_name_at_limit_passes_name_check(tmp_path):
    for flag, _, limit in launch_name_cases(tmp_path):
        code, error = run_once([flag, "n" * limit])
        assert code == 1
        assert "does-not-exist" in error


def test_launch_topic_name_at_limit_is_announced(tmp_path):
    cases = launch_name_cases(tmp_path)
    args = []
    for flag, _, limit in cases:
        args += [flag, "n" * limit]
    with Firmware(*args) as fw:
        _, topics, _ = do_handshake(fw)
        assert [t["topic"] for t in topics] == ["n" * limit for _, _, limit in cases]


def node_name_limit(max_frame):
    """Longest --node name fitting the HELLO entry: the same sum the core
    gate refuses, from the frame limit the firmware announces."""
    budget = max_frame - 4
    limit = budget - 17 - 1
    while limit > 0 and 17 + varint_len(limit) + limit > budget:
        limit -= 1
    return limit


def test_launch_node_name_over_limit_fails(tmp_path):
    with Firmware() as fw:
        _, _, payload = read_frame(fw.master, want=0)
        limit = node_name_limit(parse_hello(payload)["max_frame"])
    name = "n" * (limit + 1)
    code, error = run_once(["--node", name])
    assert code == 2
    assert "--node" in error
    assert name in error
    assert str(limit + 1) in error
    assert f"limit {limit}" in error


def test_launch_node_name_at_limit_passes_name_check(tmp_path):
    with Firmware() as fw:
        _, _, payload = read_frame(fw.master, want=0)
        limit = node_name_limit(parse_hello(payload)["max_frame"])
    code, error = run_once(["--node", "n" * limit])
    assert code == 1
    assert "does-not-exist" in error


def test_launch_node_name_at_limit_is_announced(tmp_path):
    with Firmware() as fw:
        _, _, payload = read_frame(fw.master, want=0)
        limit = node_name_limit(parse_hello(payload)["max_frame"])
    name = "n" * limit
    with Firmware("--node", name) as fw:
        hello, _, _ = do_handshake(fw)
        assert hello["node"] == name


# The Twist structs the dropped subscription's handler is declared with: the
# firmware defines r2s_on_cmd_vel unconditionally, so a build without cmd_vel
# pre-includes its message type, which the narrowed configuration no longer
# emits. The layout is the standard Twist, never instantiated here.
TWIST_SHIM = """\
typedef struct geometry_msgs__msg__Vector3 {
    double x;
    double y;
    double z;
} geometry_msgs__msg__Vector3;
typedef struct geometry_msgs__msg__Twist {
    geometry_msgs__msg__Vector3 linear;
    geometry_msgs__msg__Vector3 angular;
} geometry_msgs__msg__Twist;
"""


def generated_define(header, name):
    for line in header.splitlines():
        if line.startswith(f"#define {name} "):
            return line.split()[2]
    return None


def build_firmware_without_cmd_vel(tmp_path):
    """A firmware binary built from the checked-in configuration minus cmd_vel."""
    root = Path(__file__).resolve().parents[3]
    loaded = config.load(root / "ports" / "posix" / "r2s.yaml")
    narrowed = dataclasses.replace(loaded, topics=tuple(t for t in loaded.topics if t.name != "cmd_vel"))
    assert len(narrowed.topics) == len(loaded.topics) - 1, "r2s.yaml names no cmd_vel topic to omit"
    repository = msgs.MessageRepository()
    roots = [topic.type_name for topic in narrowed.topics]
    resolved = bounds.resolve(repository, roots, bounds=dict(narrowed.bounds))
    header, source = topics.generate(narrowed, resolved, repository)
    gen_dir = tmp_path / "narrowed"
    gen_dir.mkdir()
    (gen_dir / "r2s_generated.h").write_text(header, encoding="utf-8")
    (gen_dir / "r2s_generated.c").write_text(source, encoding="utf-8")
    (gen_dir / "twist_shim.h").write_text(TWIST_SHIM, encoding="utf-8")
    # A quoted include resolves beside the including file first, so the port
    # sources build as copies next to the narrowed pair: otherwise they pick
    # up the checked-in generated headers from ports/posix/src/.
    port_dir = root / "ports" / "posix" / "src"
    for name in ("r2s_posix_port.c", "r2s_posix_port.h", "r2s_posix_firmware.c"):
        shutil.copy(port_dir / name, gen_dir / name)
    core = root / "core" / "src"
    sources = sorted(p for p in core.glob("*.c") if p.name != "test_reset.c")
    sources += [gen_dir / "r2s_posix_port.c", gen_dir / "r2s_posix_firmware.c", gen_dir / "r2s_generated.c"]
    command = [
        "cc",
        "-std=c99",
        "-I",
        str(gen_dir),
        "-I",
        str(root / "core" / "include"),
        "-I",
        str(root / "ports" / "posix" / "src"),
        "-DR2S_WITH_CODEC",
        "-include",
        str(gen_dir / "twist_shim.h"),
        *[str(p) for p in sources],
        "-o",
        str(tmp_path / "r2s_posix_narrowed"),
    ]
    for name in ("R2S_MAX_FRAME", "R2S_MAX_NEST", "R2S_TOPIC_MAX_COUNT"):
        value = generated_define(header, name)
        if value is not None:
            command.insert(-3, f"-D{name}={value}")
    compiled = subprocess.run(command, capture_output=True, text=True, timeout=300)
    assert compiled.returncode == 0, compiled.stderr
    return tmp_path / "r2s_posix_narrowed"


def test_missing_default_topic_flag_fails(tmp_path):
    binary = build_firmware_without_cmd_vel(tmp_path)
    command = [str(binary), "/dev/pts/does-not-exist"]
    proc = subprocess.run(command, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=START_TIMEOUT)
    assert proc.returncode == 2
    assert "r2s_posix: --sub names a topic" in proc.stderr
    assert "cmd_vel" in proc.stderr


def test_unopenable_port_fails(tmp_path):
    code, error = run_once([])
    assert code == 1
    assert "does-not-exist" in error


# covers: FRM-4
def test_host_seq_is_per_topic():
    """The harness numbers frames per topic: the first frame on any topic
    carries seq 0, whatever went before on other topics. A shared counter
    would inflate the MCU's lost count with gaps that never existed."""
    read_fd, write_fd = os.pipe()
    try:
        host = Host(write_fd)
        host.send(0, b"\x01\x01")
        host.send(0, b"\x03\x00")
        host.send(17, struct.pack("<6d", 0.0, 0.0, 0.0, 0.0, 0.0, 0.0))
        host.send(0, b"\x06")
        host.send(17, struct.pack("<6d", 1.0, 0.0, 0.0, 0.0, 0.0, 0.0))
    finally:
        os.close(write_fd)
    frames = []
    deadline = time.monotonic() + FRAME_TIMEOUT
    unit = bytearray()
    while len(frames) < 5 and time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        ready, _, _ = select.select([read_fd], [], [], remaining)
        assert ready, "timed out waiting for the harness frames"
        for byte in os.read(read_fd, 4096):
            unit.append(byte)
            if byte == 0x00:
                frame = wire.decode(bytes(unit))
                unit = bytearray()
                if frame is not None:
                    frames.append((frame[0], frame[1]))
    os.close(read_fd)
    assert frames == [(0, 0), (0, 1), (17, 0), (0, 2), (17, 1)]


def test_help_names_options():
    proc = subprocess.run(
        [firmware_binary(), "--help"], stdin=subprocess.DEVNULL, capture_output=True, timeout=START_TIMEOUT
    )
    assert proc.returncode == 0
    assert "--node" in proc.stdout.decode()
