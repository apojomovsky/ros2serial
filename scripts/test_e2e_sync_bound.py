# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Scenario 8 reads its median bound from R2S_SYNC_BOUND_NS.

The bound defaults to 5 ms of shared-host headroom; the CI e2e jobs
set the same value explicitly, and the nightly lone suite judges the
strict 1 ms through R2S_SYNC_BOUND_NS. These tests execute the
scenario module's bound lines with stubbed ROS imports, never a suite.
"""

import importlib.util
import sys
import types
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCENARIO = ROOT / 'tests' / 'e2e' / 'test_scenario_8_posix.py'
MODULE_NAME = 'scenario_8_bound_probe'


def _stub_module(name):
    module = types.ModuleType(name)
    sys.modules[name] = module
    return module


def _install_ros_stubs():
    """Stand-ins for the ROS imports the scenario module only uses in tests."""
    if 'rclpy' in sys.modules:
        return
    launch_testing = _stub_module('launch_testing')
    launch_testing.markers = _stub_module('launch_testing.markers')
    launch_testing.markers.keep_alive = lambda func: func
    _stub_module('rclpy')
    qos = _stub_module('rclpy.qos')
    qos.QoSProfile = type('QoSProfile', (), {})
    qos.ReliabilityPolicy = type('ReliabilityPolicy', (), {})
    builtin_interfaces = _stub_module('builtin_interfaces')
    builtin_interfaces.msg = _stub_module('builtin_interfaces.msg')
    builtin_interfaces.msg.Time = type('Time', (), {})
    diagnostic_msgs = _stub_module('diagnostic_msgs')
    diagnostic_msgs.msg = _stub_module('diagnostic_msgs.msg')
    diagnostic_msgs.msg.DiagnosticArray = type('DiagnosticArray', (), {})
    support = _stub_module('e2e_support')
    for leaf in ('bridge', 'checks', 'firmware', 'relay', 'teardown'):
        child = _stub_module(f'e2e_support.{leaf}')
        setattr(support, leaf, child)
    support.relay.PtyRelay = type('PtyRelay', (), {})


_install_ros_stubs()


def load_module(monkeypatch, raw):
    """The scenario module with R2S_SYNC_BOUND_NS set to raw (None unsets)."""
    if raw is None:
        monkeypatch.delenv('R2S_SYNC_BOUND_NS', raising=False)
    else:
        monkeypatch.setenv('R2S_SYNC_BOUND_NS', raw)
    sys.modules.pop(MODULE_NAME, None)
    spec = importlib.util.spec_from_file_location(MODULE_NAME, SCENARIO)
    module = importlib.util.module_from_spec(spec)
    sys.modules[MODULE_NAME] = module
    spec.loader.exec_module(module)
    return module


def load_bound(monkeypatch, raw):
    """The scenario module's BOUND_NS with R2S_SYNC_BOUND_NS set to raw."""
    return load_module(monkeypatch, raw).BOUND_NS


def test_bound_defaults_to_5ms(monkeypatch):
    assert load_bound(monkeypatch, None) == 5_000_000


def test_default_is_a_named_constant(monkeypatch):
    module = load_module(monkeypatch, None)
    assert module.DEFAULT_BOUND_NS == 5_000_000
    assert module.BOUND_NS == module.DEFAULT_BOUND_NS


def test_bound_follows_env(monkeypatch):
    assert load_bound(monkeypatch, '5000000') == 5_000_000


def test_bound_accepts_the_nightly_strict_value(monkeypatch):
    assert load_bound(monkeypatch, '1000000') == 1_000_000


def test_bound_rejects_a_non_integer(monkeypatch):
    try:
        load_bound(monkeypatch, 'one millisecond')
    except ValueError as exc:
        assert 'R2S_SYNC_BOUND_NS' in str(exc)
    else:
        raise AssertionError('a non-integer bound was silently accepted')
