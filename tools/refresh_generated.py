#!/usr/bin/env python3
"""Regenerate every version-stamped generated file in the tree.

The emitted banners carry the generator version, so a version bump without
this step leaves the golden files, the example libraries and the posix
sources behind. `make regen` runs this for developers; scripts/release.sh
runs it as part of the cut. Each group mirrors its golden test's inputs, so
a drift between this script and the tests fails the suite.
"""

import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "gen"))

from r2s_gen import bounds, bundle, config, descriptors, emit, layouts, msgs, topics  # noqa: E402
from r2s_gen.bounds import ResolvedMessage  # noqa: E402

GOLDEN = ROOT / "gen" / "tests" / "golden"
EXAMPLES = ROOT / "examples"
POSIX = ROOT / "ports" / "posix"

HEADER_BOUNDS = {"std_msgs/msg/Header.frame_id": 24}
TYPES_CASES = [
    ("std_msgs/msg/Float32", {}, "float32.h"),
    ("geometry_msgs/msg/Twist", {}, "twist.h"),
    ("std_msgs/msg/Header", HEADER_BOUNDS, "header.h"),
    (
        "sensor_msgs/msg/JointState",
        {
            **HEADER_BOUNDS,
            "sensor_msgs/msg/JointState.name": [4, 12],
            "sensor_msgs/msg/JointState.position": 4,
            "sensor_msgs/msg/JointState.velocity": 4,
            "sensor_msgs/msg/JointState.effort": 4,
        },
        "joint_state.h",
    ),
]
DESCRIPTOR_ROOTS = ["std_msgs/msg/Float32", "geometry_msgs/msg/Twist"]

TOPICS_TEXT = """\
node: r2s_posix
topics:
  - name: battery
    type: std_msgs/msg/Float32
    direction: pub
  - name: cmd_vel
    type: geometry_msgs/msg/Twist
    direction: sub
  - name: log_text
    type: std_msgs/msg/String
    direction: pub
bounds:
  std_msgs/msg/String.data: 8
"""


def _resolve(loaded: config.Config) -> tuple[msgs.MessageRepository, dict[str, ResolvedMessage]]:
    repository = msgs.MessageRepository()
    roots = [topic.type_name for topic in loaded.topics]
    return repository, bounds.resolve(repository, roots, bounds=dict(loaded.bounds))


def _refresh_types() -> None:
    for type_name, bound_map, golden in TYPES_CASES:
        resolved = bounds.resolve(msgs.MessageRepository(), [type_name], bounds=bound_map)
        (GOLDEN / "types" / golden).write_text(emit.emit_types(resolved), encoding="utf-8")


def _refresh_descriptors() -> None:
    repository = msgs.MessageRepository()
    resolved = bounds.resolve(repository, DESCRIPTOR_ROOTS, bounds={})
    table = descriptors.build(resolved, DESCRIPTOR_ROOTS)
    (GOLDEN / "descriptors" / "float32_twist.c").write_text(descriptors.emit_c(table), encoding="utf-8")


def _load_text(text: str) -> config.Config:
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "r2s.yaml"
        path.write_text(text, encoding="utf-8")
        return config.load(path)


def _refresh_topics() -> None:
    loaded = _load_text(TOPICS_TEXT)
    repository, resolved = _resolve(loaded)
    header, source = topics.generate(loaded, resolved, repository)
    (GOLDEN / "topics" / "posix.h").write_text(header, encoding="utf-8")
    (GOLDEN / "topics" / "posix.c").write_text(source, encoding="utf-8")


def _refresh_bundle() -> None:
    for name, text in bundle.bundle().items():
        (GOLDEN / "bundle" / Path(name).name).write_text(text, encoding="utf-8")


def _refresh_example(name: str, layout: str) -> None:
    loaded = config.load(EXAMPLES / name / "r2s.yaml")
    repository, resolved = _resolve(loaded)
    for rel, text in layouts.generate(loaded, resolved, repository, layout).items():
        (EXAMPLES / name / "lib" / rel).write_text(text, encoding="utf-8")


def _refresh_posix() -> None:
    loaded = config.load(POSIX / "r2s.yaml")
    repository, resolved = _resolve(loaded)
    header, source = topics.generate(loaded, resolved, repository)
    (POSIX / "src" / "r2s_generated.h").write_text(header, encoding="utf-8")
    (POSIX / "src" / "r2s_generated.c").write_text(source, encoding="utf-8")


def main() -> int:
    _refresh_types()
    _refresh_descriptors()
    _refresh_topics()
    _refresh_bundle()
    _refresh_example("cmake_minimal", "cmake")
    _refresh_example("arduino_minimal", "arduino")
    _refresh_posix()
    return 0


if __name__ == "__main__":
    sys.exit(main())
