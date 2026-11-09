"""A failed master run reruns once, and only on infrastructure trouble.

ci.yml never cancels master runs, and master pushes run the plan-selected
sections. When such a run still fails, ci-infra-retry.yml reruns its failed
jobs once, and only when every failed job died of an infrastructure error:
an HTTP 5xx in its log, before the tests ran. A real test failure never
retries, and neither does a log the classifier cannot read as infra.
"""

import os
import stat
import subprocess
import sys
from pathlib import Path

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ci_infra_retry

ROOT = Path(__file__).resolve().parent.parent
WORKFLOW = ROOT / ".github" / "workflows" / "ci-infra-retry.yml"
DRIVER = ROOT / "scripts" / "ci-infra-retry.sh"

INFRA_LOG = """Downloading junit.xml
Received HTTP 503 from uploads.github.com: Service Unavailable
Error: unexpected status from upload: HTTP 503
"""

TEST_LOG = """FAILED tests/test_bridge.py::test_reconnect - AssertionError: no heartbeat
1 failed, 42 passed in 12.3s
"""

MIXED_LOG = TEST_LOG + "and once a proxy returned HTTP 504\n"

BLANK_LOG = "everything passed, then the runner lost the network\n"


def test_an_http_5xx_is_infrastructure():
    assert ci_infra_retry.classify_log(INFRA_LOG) == "infra"
    assert ci_infra_retry.classify_log("action failed: HTTP 504 from ghcr.io\n") == "infra"
    assert ci_infra_retry.classify_log("request failed with status code 500\n") == "infra"


def test_a_test_failure_is_never_infrastructure():
    assert ci_infra_retry.classify_log(TEST_LOG) == "test"
    assert ci_infra_retry.classify_log("Traceback (most recent call last):\nAssertionError\n") == "test"
    assert ci_infra_retry.classify_log("not ok 3 - reconnects after reset\n") == "test"


def test_a_mixed_log_is_a_test_failure():
    """A 5xx beside a test failure refuses the retry: the failure is real."""
    assert ci_infra_retry.classify_log(MIXED_LOG) == "test"


def test_an_unreadable_log_retries_nothing():
    assert ci_infra_retry.classify_log(BLANK_LOG) == "unknown"
    assert ci_infra_retry.classify_log("") == "unknown"


def test_should_retry_needs_every_log_to_be_infrastructure():
    assert ci_infra_retry.should_retry([INFRA_LOG, INFRA_LOG]) is True
    assert ci_infra_retry.should_retry([INFRA_LOG]) is True
    assert ci_infra_retry.should_retry([INFRA_LOG, TEST_LOG]) is False
    assert ci_infra_retry.should_retry([INFRA_LOG, BLANK_LOG]) is False
    assert ci_infra_retry.should_retry([]) is False


def test_decide_exits_zero_only_for_an_all_infra_set(tmp_path):
    infra = tmp_path / "infra.log"
    infra.write_text(INFRA_LOG)
    assert ci_infra_retry.decide([infra]) == 0
    failing = tmp_path / "failing.log"
    failing.write_text(TEST_LOG)
    assert ci_infra_retry.decide([infra, failing]) == 1
    assert ci_infra_retry.decide([]) == 2


def retry_workflow():
    assert WORKFLOW.is_file(), "ci-infra-retry.yml is missing"
    return yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))


def retry_trigger():
    return retry_workflow().get(True, retry_workflow().get("on"))


def retry_job():
    jobs = retry_workflow()["jobs"]
    assert len(jobs) == 1, f"one retry job, found {sorted(jobs)}"
    return next(iter(jobs.values()))


def test_the_retry_triggers_on_completed_ci_runs():
    trigger = retry_trigger()
    assert trigger["workflow_run"]["workflows"] == ["ci"]
    assert trigger["workflow_run"]["types"] == ["completed"]


def test_the_retry_runs_once_on_a_failed_master_push():
    """First attempt only, so the rerun itself never reruns."""
    condition = retry_job()["if"]
    for clause in (
        "conclusion == 'failure'",
        "event == 'push'",
        "head_branch == 'master'",
        "run_attempt == 1",
    ):
        assert clause in condition, f"retry guard lost {clause}: {condition}"


def test_the_retry_job_can_rerun_failed_jobs():
    job = retry_job()
    assert job.get("timeout-minutes") is not None, "a hung retry must fail with its logs"
    text = "\n".join(str(step.get("run", "")) for step in job.get("steps", []))
    assert "ci-infra-retry.sh" in text, "the retry delegates failed-job handling to the driver"
    assert "--failed" in DRIVER.read_text(encoding="utf-8"), "the rerun covers only the failed jobs"


def test_the_retry_keeps_write_where_it_acts():
    """The rerun needs actions:write; the decision stays in the classifier."""
    permissions = retry_workflow().get("permissions", {})
    assert permissions.get("actions") == "write", permissions
    driver = DRIVER.read_text(encoding="utf-8")
    assert "ci_infra_retry" in driver, "the driver decides through the classifier, not inline patterns"


def write_gh_stub(path, failed_ids, log_dir, record):
    """A gh stub serving one failed-job list, log files and a rerun record."""
    path.write_text(
        "#!/usr/bin/env bash\n"
        'if [ "$1" = "api" ]; then printf "%s\\n" $STUB_FAILED_IDS; exit 0; fi\n'
        'if [ "$1" = "run" ] && [ "$2" = "view" ]; then\n'
        '  id=""; prev=""; for arg in "$@"; do [ "$prev" = "--job" ] && id="$arg"; prev="$arg"; done\n'
        '  cat "$STUB_LOG_DIR/job-$id.log"; exit 0\n'
        "fi\n"
        'if [ "$1" = "run" ] && [ "$2" = "rerun" ]; then echo "$*" >> "$STUB_RECORD"; exit 0; fi\n'
        "exit 2\n"
    )
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def run_driver(tmp_path, failed_ids, logs):
    log_dir = tmp_path / "logs"
    log_dir.mkdir(exist_ok=True)
    for job_id, body in logs.items():
        (log_dir / f"job-{job_id}.log").write_text(body)
    record = tmp_path / "rerun.txt"
    record.write_text("")
    stub = tmp_path / "bin" / "gh"
    stub.parent.mkdir(exist_ok=True)
    write_gh_stub(stub, failed_ids, log_dir, record)
    env = dict(os.environ)
    env.update(
        PATH=str(stub.parent) + ":" + env["PATH"],
        GITHUB_REPOSITORY="owner/repo",
        STUB_FAILED_IDS=failed_ids,
        STUB_LOG_DIR=str(log_dir),
        STUB_RECORD=str(record),
    )
    proc = subprocess.run(["bash", str(DRIVER), "123"], capture_output=True, text=True, env=env, timeout=60)
    return proc, record


def test_the_driver_reruns_failed_jobs_only_on_all_infra(tmp_path):
    proc, record = run_driver(tmp_path, "11 12", {"11": INFRA_LOG, "12": INFRA_LOG})
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert record.read_text().strip() == "run rerun 123 --failed"


def test_the_driver_refuses_a_test_failure(tmp_path):
    proc, record = run_driver(tmp_path, "11 12", {"11": INFRA_LOG, "12": TEST_LOG})
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert record.read_text() == ""


def test_the_driver_reruns_nothing_without_failed_jobs(tmp_path):
    proc, record = run_driver(tmp_path, "", {})
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert record.read_text() == ""
