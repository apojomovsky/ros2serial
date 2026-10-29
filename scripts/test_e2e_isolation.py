"""One e2e section next to another on one host: both pass without cross-talk.

Each run.sh and run-avr.sh leases its own DDS domain and zenoh port, so two
suites never share a domain even though they share the tree. Narrowed to
Fast DDS and Zenoh, one board in the avr scenario loop; the RMW matrix
covers each leg alone, and these tests cover the overlap.

Beyond distinct leases, each side publishes a uniquely named canary topic
for the whole run, and the test asserts the other side's graph never lists
it while both run. A canary the lease reports would merely repeat the lease
numbers; a canary on the graph proves absence of cross-talk. Both sides run
in their own build directory, so the shared build lock cannot serialize
them: without overlap there would be no concurrency to prove anything
about. A proof counts only when both sides are on the probed RMW at once
and only after repeating, so a slow graph or a missed probe cannot pass
it.
"""

import concurrent.futures
import os
import re
import shutil
import signal
import subprocess
import tempfile
import time
import uuid
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
# The shared cache both runners inherit: the lease script resolves it the
# same way, so an image that remaps HOME still lands on real disk.
CACHE = Path(
    os.environ.get("R2S_CACHE_DIR", os.path.join(os.environ.get("HOME", tempfile.gettempdir()), ".cache", "ros2serial"))
)
RMWS = ("rmw_fastrtps_cpp", "rmw_zenoh_cpp")
DOMAIN_TIMEOUT = 900.0
SWEEP_PAUSE = 10.0
PROBE_TIMEOUT = 120
PROOFS_REQUIRED = 2


def fresh_canary():
    """A per-run topic name no other suite can collide with."""
    return "r2s_canary_" + uuid.uuid4().hex[:12]


def port_of(output):
    """The leased zenoh port the run reports, failing the run that reports none."""
    match = re.search(r"e2e isolation: ROS_DOMAIN_ID=\d+ ZENOH_ROUTER_PORT=(\d+)", output)
    assert match is not None, "no leased zenoh port in the last 4000 bytes:\n" + output[-4000:]
    return int(match.group(1))


def zenoh_session_file(port):
    """A session config pointing at one run's router, multicast off."""
    fd, name = tempfile.mkstemp(prefix="r2s-e2e-canary-", suffix=".json5")
    with os.fdopen(fd, "w") as handle:
        handle.write(
            "{\n  scouting: { multicast: { enabled: false } },\n"
            f'  connect: {{\n    endpoints: ["tcp/127.0.0.1:{port}"],\n  }},\n}}\n'
        )
    return name


def probe_topics(domain, rmw, session_file=None):
    """The topic list another suite's graph shows, or None when unseen.

    None means the probe learned nothing: the other side runs no bridge on
    that RMW right now, or discovery itself stalled. Callers treat None as
    vacuous, never as proof for or against cross-talk.
    """
    env = dict(os.environ)
    env["ROS_DOMAIN_ID"] = str(domain)
    env["RMW_IMPLEMENTATION"] = rmw
    env.pop("ZENOH_ROUTER_CONFIG_URI", None)
    if session_file is not None:
        env["ZENOH_SESSION_CONFIG_URI"] = session_file
    else:
        env.pop("ZENOH_SESSION_CONFIG_URI", None)
    try:
        proc = subprocess.run(
            ["ros2", "topic", "list", "--no-daemon"],
            capture_output=True,
            text=True,
            timeout=PROBE_TIMEOUT,
            env=env,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if proc.returncode != 0:
        return None
    return set(proc.stdout.split())


def current_rmw(log_text, posix):
    """The RMW iteration the log's last header names, or None while building."""
    if posix:
        matches = re.findall(r"== e2e-posix \(\w+\): (\S+)", log_text)
    else:
        matches = re.findall(r"== e2e-avr: (\S+)", log_text)
    return matches[-1] if matches else None


def wait_for_domain(path, deadline):
    """The leased domain a run reports, once its builds finish."""
    while time.monotonic() < deadline:
        match = re.search(r"e2e isolation: ROS_DOMAIN_ID=(\d+)", path.read_text())
        if match is not None:
            return int(match.group(1))
        time.sleep(5.0)
    raise AssertionError("a run reported no leased domain within the timeout:\n" + path.read_text()[-4000:])


def sweep(infos, proofs, stats):
    """One cross-talk sweep over every side and RMW, recording proof.

    Each probe names what it saw, but only when both sides are on the
    probed RMW at once. A graph with the run's own canary proves the
    canary plumbing ran there; a graph without the other side's canary
    proves isolation for that RMW, but only once the other's canary is
    known up, so a canary that simply has not started yet cannot pass.
    Every proof needs repeating, so one slow graph cannot pass it.
    """
    stats["sweeps"] += 1
    logs = {name: info["log"].read_text() for name, info in infos.items()}
    rmws = {name: current_rmw(logs[name], infos[name]["posix"]) for name in infos}
    jobs = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        for name in infos:
            for rmw in RMWS:
                other = next(candidate for candidate in infos if candidate != name)
                if rmws[name] != rmw or rmws[other] != rmw:
                    continue
                jobs[(name, rmw)] = pool.submit(probe_topics, infos[name]["domain"], rmw, infos[name]["session"])
        seen = {}
        for (name, rmw), job in jobs.items():
            topics = job.result()
            if topics is None or "/diagnostics" not in topics:
                continue
            stats["live"] += 1
            seen[(name, rmw)] = topics
            if "/" + infos[name]["canary"] in topics:
                proofs[("own", name, rmw)] += 1
        for (name, rmw), topics in seen.items():
            other = next(candidate for candidate in infos if candidate != name)
            if proofs[("own", other, rmw)] == 0:
                continue
            if "/" + infos[other]["canary"] not in topics:
                proofs[("cross", name, rmw)] += 1


def run_pair(first, second):
    """Two suites side by side, failing on any cross-talk either way.

    first and second name (script, canary, extra env) triples. Each side
    builds and runs in its own build directory, so the shared build lock
    cannot serialize them. Each side's graph must show its own canary and
    never the other's, on every RMW, at least twice while both sides are
    on that RMW. Distinct leases alone would pass without that.
    """
    logs = {}
    procs = {}
    builds = []
    CACHE.mkdir(parents=True, exist_ok=True)
    barrier = Path(tempfile.mkdtemp(prefix="r2s-e2e-iso-barrier-", dir=CACHE))
    try:
        for name, (script, canary, extra) in (("first", first), ("second", second)):
            other = "second" if name == "first" else "first"
            handle = tempfile.NamedTemporaryFile(prefix="r2s-e2e-" + name + "-", suffix=".log", delete=False)
            logs[name] = Path(handle.name)
            handle.close()
            build = Path(tempfile.mkdtemp(prefix="r2s-e2e-iso-build-", dir=CACHE))
            builds.append(build)
            env = dict(
                os.environ,
                R2S_RMWS=" ".join(RMWS),
                R2S_E2E_CANARY=canary,
                R2S_BUILD_DIR=str(build),
                R2S_E2E_BARRIER=str(barrier),
                R2S_E2E_SLOT=name,
                R2S_E2E_PEER=other,
            )
            env.update(extra)
            with logs[name].open("w") as output:
                procs[name] = subprocess.Popen(
                    ["bash", script], cwd=ROOT, stdout=output, stderr=subprocess.STDOUT, env=env, start_new_session=True
                )
        deadline = time.monotonic() + DOMAIN_TIMEOUT
        domains = {name: wait_for_domain(logs[name], deadline) for name in procs}
        infos = {}
        sessions = []
        try:
            for name, (_script, canary, _extra) in (("first", first), ("second", second)):
                session = zenoh_session_file(port_of(logs[name].read_text()))
                sessions.append(session)
                infos[name] = {
                    "domain": domains[name],
                    "canary": canary,
                    "session": session,
                    "log": logs[name],
                    "posix": _script == "tests/e2e/run.sh",
                }
            proofs = {("own", name, rmw): 0 for name in infos for rmw in RMWS}
            proofs.update({("cross", name, rmw): 0 for name in infos for rmw in RMWS})
            stats = {"sweeps": 0, "live": 0}
            while any(proc.poll() is None for proc in procs.values()):
                sweep(infos, proofs, stats)
                time.sleep(SWEEP_PAUSE)
            sweep(infos, proofs, stats)
            outs = {name: logs[name].read_text() for name in procs}
            for name, proc in procs.items():
                assert proc.returncode == 0, f"{name} section failed:\n" + outs[name][-4000:]
            assert domains["first"] != domains["second"], "both sections ran on one domain"
            missing = sorted(key for key, count in proofs.items() if count < PROOFS_REQUIRED)
            assert not missing, (
                "no proof of isolation for: "
                + ", ".join("/".join(part) for part in missing)
                + f" ({stats['sweeps']} sweeps, {stats['live']} live evaluations)"
            )
            return outs
        finally:
            for session in sessions:
                Path(session).unlink(missing_ok=True)
    finally:
        for proc in procs.values():
            if proc.poll() is None:
                try:
                    os.killpg(proc.pid, signal.SIGTERM)
                except (ProcessLookupError, PermissionError):
                    pass
        deadline = time.monotonic() + 60.0
        while any(proc.poll() is None for proc in procs.values()) and time.monotonic() < deadline:
            time.sleep(1.0)
        for proc in procs.values():
            if proc.poll() is None:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except (ProcessLookupError, PermissionError):
                    pass
        for proc in procs.values():
            proc.wait()
        for path in builds + [barrier]:
            shutil.rmtree(path, ignore_errors=True)


@pytest.mark.skipif(shutil.which("colcon") is None or shutil.which("ros2") is None, reason="colcon is not installed")
def test_two_concurrent_e2e_sections_pass_without_crosstalk():
    outs = run_pair(
        ("tests/e2e/run.sh", fresh_canary(), {}),
        ("tests/e2e/run.sh", fresh_canary(), {}),
    )
    assert port_of(outs["first"]) != port_of(outs["second"]), "both sections share a zenoh port"


@pytest.mark.skipif(shutil.which("colcon") is None or shutil.which("ros2") is None, reason="colcon is not installed")
def test_avr_and_posix_sections_pass_side_by_side():
    run_pair(
        ("tests/e2e/run-avr.sh", fresh_canary(), {"R2S_E2E_BOARDS": "uno"}),
        ("tests/e2e/run.sh", fresh_canary(), {}),
    )
