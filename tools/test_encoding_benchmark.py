"""Cross-checks for the P10 encoding benchmark.

The Python models in benchmark/encoding are the readable spec of each
alternative encoding; these tests pin them to the reviewed spec vectors
(packed), to the ROS container's own serialization (CDR, via rclpy), and
to hand-computed frames (MAVLink-style). The write-up embeds the table
the models render, pinned verbatim so numbers cannot drift from the code.
"""

import json
import shutil
import subprocess
import sys
from pathlib import Path

import pytest
import yaml

ROOT = Path(__file__).resolve().parent.parent
ENCODING = ROOT / "benchmark" / "encoding"

sys.path.insert(0, str(ENCODING))  # noqa: E402

import sizes  # noqa: E402

VECTORS = (
    ("float32_1_5.yaml", False),
    ("twist_full_mixed.yaml", False),
    ("twist_narrow_exact.yaml", True),
    ("range_sonar.yaml", False),
    ("range_sonar_narrow.yaml", True),
    ("imu_spin.yaml", False),
    ("imu_spin_narrow.yaml", True),
    ("joint_state_two_joints.yaml", False),
    ("joint_state_two_joints_narrow.yaml", True),
    ("transform_stamped_full.yaml", False),
    ("tf_message_one.yaml", False),
    ("tf_message_one_narrow.yaml", True),
)

SINGLETONS = {
    "builtin_interfaces/msg/Time": {"sec": 100, "nanosec": 200},
    "geometry_msgs/msg/Vector3": {"x": 0.1, "y": -0.2, "z": 0.3},
    "geometry_msgs/msg/Quaternion": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
    "std_msgs/msg/Header": {"stamp": {"sec": 100, "nanosec": 200}, "frame_id": "imu0"},
    "std_msgs/msg/String": {"data": "hello"},
    "geometry_msgs/msg/Transform": {
        "translation": {"x": 1.5, "y": -2.0, "z": 0.5},
        "rotation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
    },
}

SINGLETON_PACKED = {
    "builtin_interfaces/msg/Time": 8,
    "geometry_msgs/msg/Vector3": 24,
    "geometry_msgs/msg/Quaternion": 32,
    "std_msgs/msg/Header": 13,
    "std_msgs/msg/String": 6,
    "geometry_msgs/msg/Transform": 56,
}

MAVLINK_HAND = {
    # type, narrow, version: total frame bytes on the wire.
    ("std_msgs/msg/Float32", False, 1): 12,
    ("std_msgs/msg/Float32", False, 2): 16,
    ("geometry_msgs/msg/Twist", False, 1): 56,
    ("geometry_msgs/msg/Twist", False, 2): 60,
    ("geometry_msgs/msg/Twist", True, 1): 32,
    ("geometry_msgs/msg/Twist", True, 2): 36,
    ("sensor_msgs/msg/Range", False, 1): 53,
    ("sensor_msgs/msg/Range", False, 2): 52,
    ("sensor_msgs/msg/Range", True, 1): 53,
    ("sensor_msgs/msg/Range", True, 2): 52,
    ("sensor_msgs/msg/Imu", False, 1): 328,
    ("sensor_msgs/msg/Imu", False, 2): 329,
    ("sensor_msgs/msg/Imu", True, 1): 180,
    ("sensor_msgs/msg/Imu", True, 2): 184,
    ("sensor_msgs/msg/JointState", False, 1): 272,
    ("sensor_msgs/msg/JointState", False, 2): 273,
    ("sensor_msgs/msg/JointState", True, 1): 200,
    ("sensor_msgs/msg/JointState", True, 2): 201,
    ("builtin_interfaces/msg/Time", False, 1): 16,
    ("builtin_interfaces/msg/Time", False, 2): 17,
    ("geometry_msgs/msg/Vector3", False, 1): 32,
    ("geometry_msgs/msg/Vector3", False, 2): 36,
    ("geometry_msgs/msg/Quaternion", False, 1): 40,
    ("geometry_msgs/msg/Quaternion", False, 2): 44,
    ("std_msgs/msg/Header", False, 1): 32,
    ("std_msgs/msg/Header", False, 2): 33,
    ("std_msgs/msg/String", False, 1): 24,
    ("std_msgs/msg/String", False, 2): 17,
    ("std_msgs/msg/Bool", False, 1): 9,
    ("std_msgs/msg/Bool", False, 2): 13,
    ("geometry_msgs/msg/Transform", False, 1): 64,
    ("geometry_msgs/msg/Transform", False, 2): 68,
    ("geometry_msgs/msg/TransformStamped", False, 1): 104,
    ("geometry_msgs/msg/TransformStamped", False, 2): 105,
    ("tf2_msgs/msg/TFMessage", False, 1): 104,
    ("tf2_msgs/msg/TFMessage", False, 2): 105,
    ("tf2_msgs/msg/TFMessage", True, 1): 76,
    ("tf2_msgs/msg/TFMessage", True, 2): 80,
}


def read_vector(name):
    data = yaml.safe_load((ROOT / "spec" / "vectors" / "messages" / name).read_text())
    payload = bytes(int(token, 16) for token in data["payload"].split())
    narrow = data["profile"].get("narrow_float64", False)
    return data["type"], data["message"], narrow, payload


def test_packed_sizes_match_the_spec_vectors():
    for name, _ in VECTORS:
        type_name, message, narrow, payload = read_vector(name)
        assert sizes.packed_size(type_name, message, narrow) == len(payload), name


def test_packed_singletons_match_hand_counts():
    for type_name, message in SINGLETONS.items():
        assert sizes.packed_size(type_name, message) == SINGLETON_PACKED[type_name], type_name


def payload_case(name, narrow):
    type_name, message, vector_narrow, _ = read_vector(name)
    assert vector_narrow == narrow, name
    return type_name, message


def run_probe(cases):
    try:
        import rclpy  # noqa: F401
    except ImportError:
        pytest.skip("rclpy not importable: run in the dev image")
    payload = {key: {"type": type_name, "message": message} for key, (type_name, message) in cases.items()}
    proc = subprocess.run(
        [sys.executable, str(ENCODING / "cdr_probe.py")],
        input=json.dumps(payload),
        capture_output=True,
        text=True,
        check=True,
    )
    return json.loads(proc.stdout)


def probe_cases():
    cases = {}
    for name, narrow in VECTORS:
        type_name, message = payload_case(name, narrow)
        cases[f"{name}/{'narrow' if narrow else 'wide'}"] = (type_name, message)
    for type_name, message in SINGLETONS.items():
        cases[f"singleton/{type_name.split('/')[-1]}"] = (type_name, message)
    return cases


def test_cdr_model_matches_rmw_serialize():
    cases = probe_cases()
    probe = run_probe(cases)
    assert sorted(probe) == sorted(cases)
    for key, (type_name, message) in cases.items():
        assert sizes.cdr_size(type_name, message) == probe[key], key


def test_mavlink_sizes_match_hand_vectors():
    cases = probe_cases()
    for key, (type_name, message) in cases.items():
        narrow = "narrow" in key
        for version in (1, 2):
            if (type_name, narrow, version) not in MAVLINK_HAND:
                continue
            expected = MAVLINK_HAND[(type_name, narrow, version)]
            assert sizes.mavlink_size(type_name, message, version, narrow) == expected, (key, version)


SEQ_VARIANTS = [
    ("all-empty", {"name": [], "position": [], "velocity": [], "effort": []}, 52),
    ("effort-two", {"effort": [1.5, 2.5]}, 124),
    ("vel-empty", {"velocity": [], "effort": [1.5]}, 100),
    ("one-joint", {"name": ["a"], "position": [1.0], "velocity": [2.0], "effort": [3.0]}, 84),
    ("name-only", {"position": [], "velocity": [], "effort": []}, 76),
]


def test_cdr_sequence_variants_match_probe_and_hand_lengths():
    import copy

    type_name, base = payload_case("joint_state_two_joints.yaml", False)
    cases = {}
    for label, override, _ in SEQ_VARIANTS:
        message = copy.deepcopy(base)
        message.update(override)
        cases[f"seq/{label}"] = (type_name, message)
    probe = run_probe(cases)
    for (label, _, hand), (key, (case_type, message)) in zip(SEQ_VARIANTS, cases.items(), strict=True):
        assert sizes.cdr_size(case_type, message) == probe[key] == hand, label


def test_mavlink_rejects_other_versions():
    with pytest.raises(ValueError):
        sizes.mavlink_size("std_msgs/msg/Float32", {"data": 1.5}, 3)


def test_narrow_breakdown_matches_hand_groups():
    type_name, message = payload_case("imu_spin.yaml", False)
    assert sizes.narrow_breakdown(type_name, message) == [
        ("orientation", 32, 16),
        ("orientation_covariance", 72, 36),
        ("angular_velocity", 24, 12),
        ("angular_velocity_covariance", 72, 36),
        ("linear_acceleration", 24, 12),
        ("linear_acceleration_covariance", 72, 36),
    ]
    wide = sizes.packed_size(type_name, message, False)
    narrow = sizes.packed_size(type_name, message, True)
    groups = sizes.narrow_breakdown(type_name, message)
    assert wide - narrow == sum(w - n for _, w, n in groups)


def test_narrow_breakdown_is_empty_without_float64():
    assert sizes.narrow_breakdown("sensor_msgs/msg/Range", payload_case("range_sonar.yaml", False)[1]) == []


def snapshot_fields(type_name):
    package, name = type_name.replace("/msg/", " ").split()
    text = (ROOT / "gen" / "r2s_gen" / "snapshots" / "jazzy" / package / "msg" / f"{name}.msg").read_text()
    fields = []
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line or "=" in line or " " not in line:
            continue
        parts = line.split()
        field_type, field_name = parts[0], parts[1]
        assert "__" not in field_name, field_name
        fields.append((field_type, field_name))
    return fields


NESTED = {
    "Time": "builtin_interfaces/msg/Time",
    "Header": "std_msgs/msg/Header",
    "Vector3": "geometry_msgs/msg/Vector3",
    "Quaternion": "geometry_msgs/msg/Quaternion",
    "Transform": "geometry_msgs/msg/Transform",
    "TransformStamped": "geometry_msgs/msg/TransformStamped",
}


def flattened_layout(type_name):
    fields = []
    for field_type, field_name in snapshot_fields(type_name):
        if "/msg/" in field_type:
            fields.extend(flattened_layout(field_type))
        elif "/" in field_type:
            package, name = field_type.split("/")
            fields.extend(flattened_layout(f"{package}/msg/{name.removesuffix('[]')}"))
        elif field_type in NESTED:
            fields.extend(flattened_layout(NESTED[field_type]))
        else:
            fields.append((field_type, field_name))
    return fields


def test_layouts_match_the_snapshots():
    for type_name in list(sizes.REPRESENTATIVE) + list(SINGLETONS):
        assert sizes.leaf_fields(type_name) == flattened_layout(type_name), type_name


def bundle_bounds():
    import ast

    wanted = {"BUNDLED_TYPES", "BUNDLE_BOUNDS", "DEFAULT_STRING_BOUND"}
    tree = ast.parse((ROOT / "gen" / "r2s_gen" / "bundle.py").read_text())
    values = {}
    for node in ast.walk(tree):
        if (
            isinstance(node, ast.Assign)
            and len(node.targets) == 1
            and isinstance(node.targets[0], ast.Name)
            and node.targets[0].id in wanted
        ):
            values[node.targets[0].id] = ast.literal_eval(node.value)
    return values["BUNDLED_TYPES"], values["BUNDLE_BOUNDS"], values["DEFAULT_STRING_BOUND"]


def test_mavlink_bounds_match_the_bundle():
    bundled, bounds, default_string = bundle_bounds()
    assert sorted(sizes.REPRESENTATIVE) == sorted(bundled)
    assert sizes.MAVLINK_BOUNDS == dict(bounds, **{"std_msgs/msg/String.data": default_string})


def test_writeup_table_matches_the_model():
    table = sizes.render_table()
    assert "MAVLink figures are a model, not a real dialect" in table
    writeup = ROOT / "docs" / "benchmarks" / "encoding-packed-vs-cdr-vs-mavlink.md"
    assert writeup.exists(), "missing docs/benchmarks/encoding-packed-vs-cdr-vs-mavlink.md"
    assert table in writeup.read_text()


# Cycle timing on the MCU, measured in simavr cycles (the firmware times
# itself with Timer1 at one tick per cycle). Caps are regression tripwires,
# not tuned budgets: a byte takes about 1390 cycles to leave the wire at
# 115200 baud, so an encoder under a third of that stays out of the link's
# way. Per-byte cost falls with message size; the float32 row, measured at
# 264 encode cycles per byte, sets the ceiling the cap must clear.
CYCLE_CASES = {
    "float32": "std_msgs/msg/Float32",
    "twist": "geometry_msgs/msg/Twist",
    "range": "sensor_msgs/msg/Range",
    "imu": "sensor_msgs/msg/Imu",
    "joint_state": "sensor_msgs/msg/JointState",
}

CYCLE_BUDGET_PER_BYTE = 400


def cycle_message(type_name):
    profiles = sizes.REPRESENTATIVE[type_name]
    return profiles["narrow"] if profiles["narrow"] is not None else profiles["wide"]


def stage_bundle(stage):
    import os

    env = dict(os.environ, PYTHONPATH=str(ROOT / "gen"))
    command = [sys.executable, "-m", "r2s_gen.cli", "bundle", "--distro", "jazzy", "--out", str(stage)]
    subprocess.run(command, capture_output=True, text=True, check=True, env=env)


def core_sources():
    return sorted((ROOT / "core" / "src").glob("*.c"), key=lambda path: path.name)


def stage_defines(stage):
    import os

    header = stage / "src" / "r2s_bundled.h"
    script = (
        "import sys\n"
        "from r2s_gen import layouts\n"
        "print(' '.join('-D' + define for define in layouts.build_defines(open(sys.argv[1]).read())))\n"
    )
    env = dict(os.environ, PYTHONPATH=str(ROOT / "gen"))
    proc = subprocess.run(
        [sys.executable, "-", str(header)],
        input=script,
        capture_output=True,
        text=True,
        env=env,
        check=True,
    )
    defines = proc.stdout.split()
    assert any(define == "-DR2S_WITH_CODEC" for define in defines), defines
    assert any(define.startswith("-DR2S_MAX_FRAME=") for define in defines), defines
    assert any(define.startswith("-DR2S_MAX_NEST=") for define in defines), defines
    return defines


def build_firmware(stage, elf, mcu):
    firmware = ENCODING / "fw_bench.c"
    assert firmware.exists(), "missing benchmark/encoding/fw_bench.c"
    command = (
        [
            "avr-gcc",
            f"-mmcu={mcu}",
            "-DF_CPU=16000000UL",
            "-Os",
            "-std=c99",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-ffunction-sections",
            "-fdata-sections",
            "-Wl,--gc-sections",
        ]
        + stage_defines(stage)
        + [
            f"-I{stage / 'src'}",
            f"-I{ROOT / 'core' / 'include'}",
            str(firmware),
            str(stage / "src" / "r2s_bundled.c"),
        ]
        + [str(path) for path in core_sources() if path.name != "test_reset.c"]
        + ["-o", str(elf)]
    )
    subprocess.run(command, capture_output=True, text=True, check=True)


@pytest.mark.parametrize("mcu", ("atmega328p", "atmega2560"))
def test_benchmark_firmware_builds_for_mcu(tmp_path, mcu):
    if shutil.which("avr-gcc") is None:
        pytest.skip("avr-gcc not on PATH: run in the dev image")
    stage = tmp_path / "stage"
    stage_bundle(stage)
    build_firmware(stage, tmp_path / f"fw-{mcu}.elf", mcu)


def build_harness(path):
    command = [
        "cc",
        "-std=c99",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-O2",
        "-o",
        str(path),
        str(ROOT / "tests" / "harness" / "simavr" / "r2s_simavr.c"),
        "-lsimavr",
        "-lelf",
    ]
    proc = subprocess.run(command, capture_output=True, text=True)
    if proc.returncode != 0:
        pytest.skip(f"the simavr harness does not build here: {proc.stderr.strip().splitlines()[-1]}")
    return str(path)


def read_until_done(pty_path, deadline):
    import os
    import select
    import time

    lines = []
    # The harness sets VMIN=0 VTIME=0, so an empty read is a quiet moment,
    # not end of file: only the deadline or BM DONE ends the wait. A dead
    # harness just runs the deadline out and fails on the missing DONE.
    fd = os.open(pty_path, os.O_RDONLY | os.O_NOCTTY)
    try:
        tail = b""
        while time.monotonic() < deadline:
            ready, _, _ = select.select([fd], [], [], 5.0)
            if not ready:
                continue
            tail += os.read(fd, 256)
            while b"\n" in tail:
                raw, tail = tail.split(b"\n", 1)
                line = raw.decode("ascii", "replace").strip()
                lines.append(line)
                if line == "BM DONE":
                    return lines
    finally:
        os.close(fd)
    return lines


def test_encode_cycles_within_budget(tmp_path):
    import time

    if shutil.which("avr-gcc") is None:
        pytest.skip("avr-gcc not on PATH: run in the dev image")
    stage = tmp_path / "stage"
    stage_bundle(stage)
    elf = tmp_path / "fw-atmega328p.elf"
    build_firmware(stage, elf, "atmega328p")
    harness = build_harness(tmp_path / "r2s_simavr")
    proc = subprocess.Popen(
        [harness, "--mcu", "atmega328p", "--firmware", str(elf), "--run-ms", "60000"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    try:
        pty = None
        for line in proc.stdout:
            if line.startswith("r2s_simavr: pty="):
                pty = line.split("=", 1)[1].strip()
                break
        assert pty is not None, "the harness announced no pty"
        lines = read_until_done(pty, time.monotonic() + 90)
    finally:
        proc.terminate()
        proc.wait(timeout=30)
    assert "BM DONE" in lines, lines[-5:]
    seen = {}
    for line in lines:
        if not line.startswith("BM ") or line == "BM DONE":
            continue
        _, name, numbers = line.split(" ", 2)
        fields = dict(part.split("=") for part in numbers.split(" "))
        seen[name] = {key: int(value) for key, value in fields.items()}
    assert sorted(seen) == sorted(CYCLE_CASES), sorted(seen)
    for name, type_name in CYCLE_CASES.items():
        length = sizes.packed_size(type_name, cycle_message(type_name), True)
        assert seen[name]["len"] == length, name
        assert seen[name]["enc"] <= CYCLE_BUDGET_PER_BYTE * length, name
        assert seen[name]["dec"] <= CYCLE_BUDGET_PER_BYTE * length, name
