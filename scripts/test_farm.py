"""The farm-shaped check: build and test the released packages outside the dev image.

The ROS build farm builds from a clean chroot with no Docker and no network
at build time. `scripts/farm-test.sh` is the same shape in CI
(`.github/workflows/farm.yml`): colcon build and test of `r2s_gen`,
`r2s_bridge` and `r2s_rosidl` in a clean ROS base image, then a smoke run of
the installed entry points. The dev-image paths stay; this job stands
alongside them.
"""

from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent
WORKFLOW = ROOT / ".github" / "workflows" / "farm.yml"
SCRIPT = ROOT / "scripts" / "farm-test.sh"


def workflow():
    return yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))


def script_text():
    return SCRIPT.read_text(encoding="utf-8")


def test_the_farm_workflow_exists_and_runs_on_pull_requests():
    assert WORKFLOW.is_file(), "missing .github/workflows/farm.yml"
    text = WORKFLOW.read_text(encoding="utf-8")
    assert "pull_request" in text, "farm must run on pull requests"


def test_the_farm_job_runs_in_a_clean_ros_image_not_the_dev_image():
    jobs = workflow()["jobs"]
    assert "farm" in jobs, "farm.yml names no farm job"
    job = jobs["farm"]
    assert "container" in job or "image" in str(job), "farm must run in a clean ROS container"
    text = WORKFLOW.read_text(encoding="utf-8")
    assert "actions/dev-image" not in text, "farm must not use the dev image"
    assert "docker run" not in text.lower(), "farm must not run Docker inside"
    assert "docker/build" not in text.lower(), "farm must not build images inside"


def test_the_farm_job_builds_tests_and_smokes_the_released_packages():
    text = WORKFLOW.read_text(encoding="utf-8") + "\n" + script_text()
    assert "colcon" in text and "build" in text, "farm must colcon build"
    assert "colcon" in text and "test" in text, "farm must colcon test"
    assert "r2s-gen" in text or "r2s_gen" in text, "farm must smoke the installed generator"
    for package in ("r2s_gen", "r2s_bridge", "r2s_rosidl"):
        assert package in text, f"farm covers {package}"


def test_the_farm_script_runs_no_docker_or_network_fetch():
    text = script_text()
    for banned in ("docker run", "docker build", "curl ", "wget ", "pip install"):
        assert banned not in text, f"farm build must not run `{banned.strip()}`"


def test_colcon_tests_the_generator_with_pytest():
    text = (ROOT / "gen" / "setup.py").read_text(encoding="utf-8")
    assert "pytest" in text and "tests_require" in text, (
        "gen/setup.py must declare tests_require with pytest, or colcon test "
        "falls back to unittest discovery and fails with no tests collected"
    )


def test_the_generator_test_tooling_resolves_through_rosdep():
    text = (ROOT / "gen" / "package.xml").read_text(encoding="utf-8")
    for key in ("python3-pytest", "python3-pip", "python3-hypothesis"):
        assert key in text, f"gen/package.xml names no {key} test dependency"


def test_every_released_package_carries_the_farm_metadata():
    for manifest in (
        ROOT / "gen" / "package.xml",
        ROOT / "gen" / "r2s_rosidl" / "package.xml",
        ROOT / "bridge" / "r2s_bridge" / "package.xml",
    ):
        text = manifest.read_text(encoding="utf-8")
        assert "<maintainer" in text, manifest
        assert "<license>" in text, manifest
        assert "<export>" in text and "build_type" in text, manifest
