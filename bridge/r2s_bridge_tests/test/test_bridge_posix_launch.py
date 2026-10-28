"""Launch test: the bridge launch file against the posix virtual MCU.

Builds the posix firmware, joins it to the bridge with two ptys tied by a
relay thread, then drives the bridge through its own launch file: the
handshake must announce the firmware topics, the params file must set the
publisher depth, and Float32 traffic must flow from the MCU to the host.
"""

import importlib.util
import math
import os
import pty
import select
import subprocess
import tempfile
import threading
import time
import unittest
from pathlib import Path

import launch
import launch_testing.actions
import launch_testing.markers
import rclpy
import yaml
from ament_index_python.packages import get_package_share_directory
from launch import LaunchContext
from launch.actions import ExecuteProcess, IncludeLaunchDescription
from launch.launch_description_sources import PythonLaunchDescriptionSource
from launch_ros.substitutions import FindPackageShare
from rclpy.qos import QoSProfile, ReliabilityPolicy
from std_msgs.msg import Float32

DOCUMENTED_PARAMETERS = frozenset(
    [
        "port",
        "baud",
        "low_latency",
        "hupcl",
        "boot_wait_ms",
        "node_map",
        "reconnect_period_ms",
        "heartbeat_period_ms",
        "link_timeout_ms",
        "time_sync.enabled",
        "time_sync.period_ms",
        "rate_limit.default_hz",
        "rate_limit.overrides",
        "tx_budget_pct",
        "qos_depth",
    ]
)

REPO_ROOT = Path(__file__).resolve().parents[3]
HANDSHAKE_TIMEOUT = 20.0
FLOW_TIMEOUT = 20.0

_SCRATCH = None
_FIRMWARE = None
_PARAMS = None
_RELAY = None


class PtyRelay:
    """One serial line between two pty slaves: bytes written on one slave
    come out of the other. Each end opens its slave by name; this thread
    joins the two masters, since the test images carry no socat."""

    def __init__(self):
        self._mcu_master, mcu_slave = pty.openpty()
        self._host_master, host_slave = pty.openpty()
        self.mcu_port = os.ttyname(mcu_slave)
        self.host_port = os.ttyname(host_slave)
        os.close(mcu_slave)
        os.close(host_slave)

    def start(self):
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name="pty-relay", daemon=True)
        self._thread.start()

    def _forward(self, source, target):
        # A master reads EIO until its slave is open and after it closes, so
        # a failure here only skips one turn: both processes open their slaves
        # just after this thread starts.
        try:
            chunk = os.read(source, 4096)
        except OSError:
            time.sleep(0.005)
            return
        if not chunk:
            return
        view = memoryview(chunk)
        while view:
            try:
                written = os.write(target, view)
            except OSError:
                time.sleep(0.005)
                return
            view = view[written:]

    def _run(self):
        while not self._stop.is_set():
            readable, _, _ = select.select([self._mcu_master, self._host_master], [], [], 0.05)
            for source in readable:
                target = self._host_master if source == self._mcu_master else self._mcu_master
                self._forward(source, target)

    def stop(self):
        self._stop.set()
        self._thread.join(timeout=5.0)
        os.close(self._mcu_master)
        os.close(self._host_master)


def build_posix_firmware(scratch):
    """Build the posix virtual MCU into scratch with its own run script and
    return the firmware binary."""
    env = dict(os.environ)
    env["R2S_BUILD_DIR"] = str(scratch)
    proc = subprocess.run(
        ["bash", str(REPO_ROOT / "ports" / "posix" / "run.sh"), "--build-only"],
        capture_output=True,
        text=True,
        timeout=300,
        env=env,
        check=False,
    )
    if proc.returncode != 0:
        raise AssertionError("posix firmware build failed:\n" + proc.stderr[-4000:])
    firmware = scratch / "ports" / "posix" / "r2s_posix_firmware"
    if not firmware.is_file():
        raise AssertionError("posix firmware binary missing after build: " + str(firmware))
    return firmware


_BOOT_ENV = "R2S_POSIX_BOOT_ID_FILE"


def _example_params():
    path = Path(get_package_share_directory("r2s_bridge")) / "config" / "r2s_bridge.params.yaml"
    with open(path) as handle:
        return yaml.safe_load(handle)["r2s_bridge"]["ros__parameters"]


def _flatten(mapping, prefix=""):
    names = []
    for key, value in mapping.items():
        name = prefix + key
        if isinstance(value, dict):
            names.extend(_flatten(value, name + "."))
        else:
            names.append(name)
    return names


def _scratch_params(scratch):
    """Copy the installed example file to scratch with a non-default QoS
    depth, so the launch proves the params file reaches the bridge."""
    source = Path(get_package_share_directory("r2s_bridge")) / "config" / "r2s_bridge.params.yaml"
    text = source.read_text()
    assert text.count("qos_depth: 10") == 1
    target = scratch / "r2s_bridge.params.yaml"
    target.write_text(text.replace("qos_depth: 10", "qos_depth: 7"))
    return target


def _load_launch_module():
    path = Path(get_package_share_directory("r2s_bridge")) / "launch" / "r2s_bridge.launch.py"
    spec = importlib.util.spec_from_file_location("r2s_bridge_launch", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _prepare():
    global _SCRATCH, _FIRMWARE, _RELAY, _PARAMS
    _SCRATCH = tempfile.TemporaryDirectory(prefix="r2s-h9-")
    scratch = Path(_SCRATCH.name)
    _FIRMWARE = build_posix_firmware(scratch)
    _PARAMS = _scratch_params(scratch)
    _RELAY = PtyRelay()
    _RELAY.start()


@launch_testing.markers.keep_alive
def generate_test_description():
    _prepare()
    bridge = IncludeLaunchDescription(
        PythonLaunchDescriptionSource([FindPackageShare("r2s_bridge"), "/launch/r2s_bridge.launch.py"]),
        launch_arguments={"port": _RELAY.host_port, "params_file": str(_PARAMS)}.items(),
    )
    firmware = ExecuteProcess(
        cmd=[str(_FIRMWARE), _RELAY.mcu_port, "--rate", "20"],
        output="screen",
        additional_env={_BOOT_ENV: str(Path(_SCRATCH.name) / "boot")},
    )
    return launch.LaunchDescription([bridge, firmware, launch_testing.actions.ReadyToTest()])


def tearDownModule():
    if _RELAY is not None:
        _RELAY.stop()
    if _SCRATCH is not None:
        _SCRATCH.cleanup()


class TestBridgePosixLaunch(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rclpy.init()

    @classmethod
    def tearDownClass(cls):
        rclpy.shutdown()

    def test_handshake_announces_topics(self):
        node = rclpy.create_node("h9_handshake_probe")
        try:
            deadline = time.monotonic() + HANDSHAKE_TIMEOUT
            while True:
                names = {name for name, _ in node.get_topic_names_and_types()}
                if "/battery" in names and "/cmd_vel" in names:
                    return
                self.assertLess(time.monotonic(), deadline, "bridge never announced battery and cmd_vel")
                time.sleep(0.2)
        finally:
            node.destroy_node()

    def test_params_file_sets_qos_depth(self):
        # The scratch params file copies the example with qos_depth 7, so a
        # depth of 7 on the announced publisher proves the file was applied.
        node = rclpy.create_node("h9_qos_probe")
        try:
            deadline = time.monotonic() + HANDSHAKE_TIMEOUT
            while True:
                infos = node.get_publishers_info_by_topic("/battery")
                if infos and infos[0].qos_profile.depth == 7:
                    return
                self.assertLess(time.monotonic(), deadline, "bridge ignored qos_depth from the params file")
                time.sleep(0.2)
        finally:
            node.destroy_node()

    def test_mcu_to_host_float_flow(self):
        node = rclpy.create_node("h9_flow_probe")
        received = []
        # The bridge publishes MCU topics best effort ([SES-31]), so the probe
        # subscribes best effort too; a reliable probe would never match.
        best_effort = QoSProfile(depth=10, reliability=ReliabilityPolicy.BEST_EFFORT)
        node.create_subscription(Float32, "battery", lambda msg: received.append(msg.data), best_effort)
        try:
            deadline = time.monotonic() + FLOW_TIMEOUT
            while len(received) < 3:
                rclpy.spin_once(node, timeout_sec=0.5)
                self.assertLess(time.monotonic(), deadline, "no Float32 flow on battery")
            self.assertTrue(all(math.isfinite(value) for value in received))
            ordered = all(received[i] < received[i + 1] for i in range(len(received) - 1))
            self.assertTrue(ordered, "battery values arrived out of order")
        finally:
            node.destroy_node()


class TestLaunchFileChecks(unittest.TestCase):
    def test_default_params_file_is_installed(self):
        prefix = FindPackageShare("r2s_bridge").perform(LaunchContext())
        params = Path(prefix) / "config" / "r2s_bridge.params.yaml"
        self.assertTrue(params.is_file(), "example params file missing from the install: " + str(params))

    def test_example_names_the_documented_parameters(self):
        # An empty list carries no YAML type the parameter parser accepts, so
        # the example keeps rate_limit.overrides commented out.
        found = set(_flatten(_example_params()))
        self.assertEqual(found - DOCUMENTED_PARAMETERS, set())
        self.assertEqual(DOCUMENTED_PARAMETERS - found, {"rate_limit.overrides"})

    def test_overrides_example_uncomments_cleanly(self):
        path = Path(get_package_share_directory("r2s_bridge")) / "config" / "r2s_bridge.params.yaml"
        lines = path.read_text().splitlines()
        commented = [line for line in lines if line.strip() == '# overrides: ["cmd_vel=20"]']
        self.assertEqual(len(commented), 1, "overrides example line missing or duplicated")
        idx = lines.index(commented[0])
        fixed = lines[:idx] + [commented[0].replace("# ", "", 1)] + lines[idx + 1 :]
        doc = yaml.safe_load("\n".join(fixed))
        self.assertEqual(doc["r2s_bridge"]["ros__parameters"]["rate_limit"]["overrides"], ["cmd_vel=20"])

    def test_missing_params_file_fails_fast(self):
        module = _load_launch_module()
        context = LaunchContext()
        context.launch_configurations["params_file"] = "/nonexistent/r2s.params.yaml"
        with self.assertRaisesRegex(RuntimeError, "not a file"):
            module.check_params_file(context)
