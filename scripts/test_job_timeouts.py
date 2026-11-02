"""Every CI job carries a timeout-minutes bound near its usual duration.

A hung job must fail with its logs instead of holding a runner until
GitHub's six hour default: #350 saw a SCRIPTS shard sit silent for 38
minutes. These tests pin the contract from the workflow files.
"""

from pathlib import Path

import re

import yaml

ROOT = Path(__file__).resolve().parent.parent
WORKFLOWS = (
    ROOT / ".github" / "workflows" / "ci.yml",
    ROOT / ".github" / "workflows" / "nightly-jobs.yml",
    ROOT / ".github" / "workflows" / "release.yml",
)


def jobs_of(path):
    assert path.is_file(), f"{path.name} is missing"
    return yaml.safe_load(path.read_text(encoding="utf-8"))["jobs"]


def test_every_job_sets_a_timeout():
    # A reusable workflow call takes no timeout-minutes of its own; the
    # called jobs carry theirs, which bound the call.
    missing = [
        f"{path.name}.{name}"
        for path in WORKFLOWS
        for name, job in jobs_of(path).items()
        if "timeout-minutes" not in job and "uses" not in job
    ]
    assert not missing, "jobs without timeout-minutes: " + ", ".join(missing)


# The long fuzzing is the one job whose length is chosen, not observed:
# its bound comes from its targets and seconds (see below).
LONG_JOBS = {("nightly-jobs.yml", "fuzz-long")}


def test_every_timeout_stays_within_two_hours():
    out_of_range = [
        f"{path.name}.{name}={job['timeout-minutes']}"
        for path in WORKFLOWS
        for name, job in jobs_of(path).items()
        if (path.name, name) not in LONG_JOBS
        and "timeout-minutes" in job
        and not (isinstance(job["timeout-minutes"], (int, float)) and job["timeout-minutes"] <= 120)
    ]
    assert not out_of_range, "timeout-minutes above 120: " + ", ".join(out_of_range)


# Jobs that build nothing keep the small bound; everything else builds or
# pulls an image (see the rule comment in ci.yml).
NO_BUILD_JOBS = {("ci.yml", "plan"), ("ci.yml", "bridge-images")}


def test_image_jobs_carry_a_thirty_minute_floor():
    thin = [
        f"{path.name}.{name}={job['timeout-minutes']}"
        for path in WORKFLOWS
        for name, job in jobs_of(path).items()
        if (path.name, name) not in NO_BUILD_JOBS and "timeout-minutes" in job and job["timeout-minutes"] < 30
    ]
    assert not thin, "image jobs below the 30 minute floor: " + ", ".join(thin)


def test_jobs_that_build_nothing_stay_at_five():
    for file_name, job_name in NO_BUILD_JOBS:
        job = jobs_of(next(p for p in WORKFLOWS if p.name == file_name))[job_name]
        assert job.get("timeout-minutes") == 5, f"{file_name}.{job_name} should stay at 5"


def fuzz_targets():
    script = (ROOT / "scripts" / "fuzz.sh").read_text(encoding="utf-8")
    match = re.search(r"targets=\(\$\{TARGET:-([^}]*)\}\)", script)
    assert match, "fuzz.sh no longer lists its default targets"
    return match.group(1).split()


def test_fuzz_long_outlasts_its_targets_plus_the_image_floor():
    job = jobs_of(ROOT / ".github" / "workflows" / "nightly-jobs.yml")["fuzz-long"]
    seconds = [
        int(step["env"]["R2S_FUZZ_SECONDS"]) for step in job["steps"] if "R2S_FUZZ_SECONDS" in step.get("env", {})
    ]
    assert len(seconds) == 1
    need = len(fuzz_targets()) * seconds[0] // 60 + 30
    assert job["timeout-minutes"] >= need, f"fuzz-long needs {need} minutes, has {job['timeout-minutes']}"
