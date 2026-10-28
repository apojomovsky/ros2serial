"""The bridge job's distro images are pins: scripts/bridge-images.sh reads them
from ci/versions.env, and the workflow carries no image of its own."""

import json
import shutil
import subprocess
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "scripts" / "bridge-images.sh"
WORKFLOW = ROOT / ".github" / "workflows" / "ci.yml"

JAZZY = "ros:jazzy-ros-base-noble@sha256:" + "1" * 64
LYRICAL = "ros:lyrical-ros-base-resolute@sha256:" + "2" * 64
PINS = f"PYTHON_VERSION=3.12\nROS_JAZZY_IMAGE={JAZZY}\nROS_LYRICAL_IMAGE={LYRICAL}\n"


def tree(tmp_path: Path, text: str) -> Path:
    (tmp_path / "scripts").mkdir()
    shutil.copy2(SCRIPT, tmp_path / "scripts" / "bridge-images.sh")
    (tmp_path / "ci").mkdir()
    (tmp_path / "ci" / "versions.env").write_text(text)
    return tmp_path


def run(root: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["bash", str(root / "scripts" / "bridge-images.sh")],
        capture_output=True,
        text=True,
    )


def outputs(root: Path) -> dict[str, str]:
    result = run(root)
    assert result.returncode == 0, result.stderr
    return dict(line.split("=", 1) for line in result.stdout.splitlines())


def test_the_matrix_and_the_images_come_from_the_pinned_file(tmp_path):
    out = outputs(tree(tmp_path, PINS))
    assert json.loads(out["matrix"]) == {"distro": ["jazzy", "lyrical"]}
    assert json.loads(out["images"]) == {"jazzy": JAZZY, "lyrical": LYRICAL}


def test_the_dev_image_pin_is_not_a_matrix_entry(tmp_path):
    out = outputs(tree(tmp_path, "ROS_IMAGE=ros:jazzy-ros-base-noble\n" + PINS))
    assert json.loads(out["matrix"]) == {"distro": ["jazzy", "lyrical"]}


def test_a_distro_added_to_the_file_reaches_the_matrix(tmp_path):
    humble = "ros:humble-ros-base-jammy@sha256:" + "3" * 64
    out = outputs(tree(tmp_path, PINS + f"ROS_HUMBLE_IMAGE={humble}\n"))
    assert json.loads(out["matrix"]) == {"distro": ["jazzy", "lyrical", "humble"]}
    assert json.loads(out["images"])["humble"] == humble


def test_a_pin_without_a_digest_is_refused(tmp_path):
    result = run(tree(tmp_path, PINS.replace(JAZZY, "ros:jazzy-ros-base-noble")))
    assert result.returncode == 1
    assert "ROS_JAZZY_IMAGE" in result.stderr
    assert "digest" in result.stderr


def test_a_last_pin_without_a_trailing_newline_is_kept(tmp_path):
    out = outputs(tree(tmp_path, PINS[:-1]))
    assert json.loads(out["matrix"]) == {"distro": ["jazzy", "lyrical"]}
    assert json.loads(out["images"]) == {"jazzy": JAZZY, "lyrical": LYRICAL}


def test_an_unpinned_last_line_without_a_newline_is_refused(tmp_path):
    text = f"ROS_JAZZY_IMAGE={JAZZY}\nROS_HUMBLE_IMAGE=ros:humble-ros-base-jammy"
    result = run(tree(tmp_path, text))
    assert result.returncode == 1
    assert "ROS_HUMBLE_IMAGE" in result.stderr


def test_an_image_pinned_for_another_distro_is_refused(tmp_path):
    result = run(tree(tmp_path, PINS.replace(JAZZY, LYRICAL)))
    assert result.returncode == 1
    assert "ROS_JAZZY_IMAGE" in result.stderr


def test_a_distro_pinned_twice_is_refused(tmp_path):
    result = run(tree(tmp_path, PINS + f"ROS_JAZZY_IMAGE={JAZZY}\n"))
    assert result.returncode == 1
    assert "ROS_JAZZY_IMAGE" in result.stderr
    assert "more than once" in result.stderr


def test_a_duplicate_pin_without_a_trailing_newline_is_refused(tmp_path):
    result = run(tree(tmp_path, PINS + f"ROS_JAZZY_IMAGE={JAZZY}"))
    assert result.returncode == 1
    assert "ROS_JAZZY_IMAGE" in result.stderr
    assert "more than once" in result.stderr


def test_a_file_without_pins_is_refused(tmp_path):
    result = run(tree(tmp_path, "PYTHON_VERSION=3.12\n"))
    assert result.returncode == 1
    assert "ci/versions.env" in result.stderr


def test_the_workflow_carries_no_image_of_its_own():
    workflow = yaml.safe_load(WORKFLOW.read_text())
    bridge = workflow["jobs"]["bridge"]
    assert bridge["needs"] == "bridge-images"
    assert "needs.bridge-images.outputs.matrix" in bridge["strategy"]["matrix"]
    assert "needs.bridge-images.outputs.images" in bridge["container"]["image"]
    assert "matrix.distro" in bridge["container"]["image"]
    for value in images_in(workflow):
        raise AssertionError(f"the workflow names an image itself: {value}")


def images_in(node) -> list[str]:
    if isinstance(node, dict):
        return [value for child in node.values() for value in images_in(child)]
    if isinstance(node, list):
        return [value for child in node for value in images_in(child)]
    if isinstance(node, str) and node.startswith("ros:"):
        return [node]
    return []
