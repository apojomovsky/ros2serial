"""e2e-lease.sh hands each concurrent suite its own domain and zenoh port.

Two holders never share a slot, a dead holder frees its slot, and an
explicit domain skips allocation. The cache dir is a throwaway, so the
tests never touch real suites' leases.
"""

import os
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LEASE = ROOT / "scripts" / "e2e-lease.sh"


def lease_env(cache, extra=None):
    """A minimal environment with an isolated lease table."""
    env = {"PATH": os.environ["PATH"], "HOME": str(cache), "R2S_CACHE_DIR": str(cache)}
    if extra:
        env.update(extra)
    return env


def start_holder(cache, extra=None):
    """Hold one lease slot until terminated, printing its domain and port."""
    return subprocess.Popen(
        ["bash", str(LEASE), "bash", "-c", "echo $R2S_E2E_DOMAIN_ID $R2S_ZENOH_ROUTER_PORT; exec sleep 60"],
        stdout=subprocess.PIPE,
        text=True,
        env=lease_env(cache, extra),
    )


def read_slot(holder):
    """The domain and port line the holder prints on startup."""
    line = holder.stdout.readline().strip().split()
    assert len(line) == 2, "lease holder printed no slot: " + repr(line)
    return int(line[0]), int(line[1])


def stop_holder(holder):
    holder.terminate()
    holder.wait(timeout=10)


def test_two_concurrent_holders_get_distinct_slots(tmp_path):
    first = start_holder(tmp_path)
    second = start_holder(tmp_path)
    try:
        domain1, port1 = read_slot(first)
        domain2, port2 = read_slot(second)
        assert domain1 != domain2, "two holders share a domain"
        assert port1 != port2, "two holders share a zenoh port"
        assert 89 <= domain1 <= 120 and 89 <= domain2 <= 120, "domain outside the lease range"
        assert 7447 <= port1 <= 7478 and 7447 <= port2 <= 7478, "zenoh port outside the lease range"
    finally:
        stop_holder(first)
        stop_holder(second)


def test_a_released_slot_is_reused(tmp_path):
    holder = start_holder(tmp_path)
    try:
        domain, _ = read_slot(holder)
    finally:
        stop_holder(holder)
    again = start_holder(tmp_path)
    try:
        assert read_slot(again)[0] == domain, "the freed slot was not reused"
    finally:
        stop_holder(again)


def test_a_lease_survives_the_build_lock_chain(tmp_path):
    """run.sh execs with-build-lock.sh (fd 9) inside the lease: the slot stays held."""
    build_lock = tmp_path / "build.lock"
    first = subprocess.Popen(
        [
            "bash",
            str(LEASE),
            "bash",
            str(ROOT / "scripts" / "with-build-lock.sh"),
            str(build_lock),
            "bash",
            "-c",
            "echo $R2S_E2E_DOMAIN_ID $R2S_ZENOH_ROUTER_PORT; exec sleep 60",
        ],
        stdout=subprocess.PIPE,
        text=True,
        env=lease_env(tmp_path),
    )
    second = start_holder(tmp_path)
    try:
        domain1, port1 = read_slot(first)
        domain2, port2 = read_slot(second)
        assert (domain1, port1) != (domain2, port2), "the build lock released the lease"
    finally:
        stop_holder(first)
        stop_holder(second)


def test_an_explicit_domain_skips_allocation(tmp_path):
    result = subprocess.run(
        ["bash", str(LEASE), "bash", "-c", "echo $R2S_E2E_DOMAIN_ID $R2S_ZENOH_ROUTER_PORT"],
        capture_output=True,
        text=True,
        env=lease_env(tmp_path, {"R2S_E2E_DOMAIN_ID": "200"}),
    )
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "200 7558", result.stdout


def test_an_explicit_port_is_honored(tmp_path):
    result = subprocess.run(
        ["bash", str(LEASE), "bash", "-c", "echo $R2S_E2E_DOMAIN_ID $R2S_ZENOH_ROUTER_PORT"],
        capture_output=True,
        text=True,
        env=lease_env(tmp_path, {"R2S_E2E_DOMAIN_ID": "200", "R2S_ZENOH_ROUTER_PORT": "9999"}),
    )
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "200 9999", result.stdout


def test_a_full_lease_table_refuses(tmp_path):
    holders = [start_holder(tmp_path) for _ in range(32)]
    try:
        for holder in holders:
            read_slot(holder)
        result = subprocess.run(
            ["bash", str(LEASE), "true"],
            capture_output=True,
            text=True,
            env=lease_env(tmp_path),
        )
        assert result.returncode != 0, "allocation succeeded with no free slot"
        assert "no free slot" in result.stderr, result.stderr
    finally:
        for holder in holders:
            stop_holder(holder)
