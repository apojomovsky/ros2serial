# Copyright (c) 2026 Alexis Pojomovsky
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

"""The static posix firmware's node name check: an overlong --node fails fast.

The static C++ firmware announces through the fixed R2S_TOPICS list, so an
overlong node name never reaches the handshake: the firmware exits 2 with
the limit before the ready line, like the generated C firmware does.
"""

import os
import pty
import select
import subprocess
import time
from pathlib import Path

from r2s_gen.reference import frames as wire

FIRMWARE = os.environ.get("R2S_POSIX_FW", "")
START_TIMEOUT = 10.0
FRAME_TIMEOUT = 5.0


def static_binary():
    assert FIRMWARE, "R2S_POSIX_FW is unset: run ports/posix/run.sh, which builds it"
    candidate = Path(FIRMWARE).parent / "r2s_posix_static_firmware"
    assert candidate.is_file(), f"no static firmware at {candidate!r}: run ports/posix/run.sh"
    return str(candidate)


def static_hello_max_frame():
    """The static firmware's HELLO frame limit, read off the wire."""
    master, slave = pty.openpty()
    proc = subprocess.Popen(
        [static_binary(), os.ttyname(slave)],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    os.close(slave)
    try:
        error = bytearray()
        deadline = time.monotonic() + START_TIMEOUT
        while b"r2s_posix: ready" not in error:
            remaining = deadline - time.monotonic()
            assert remaining > 0, "the static firmware printed no ready line"
            ready, _, _ = select.select([proc.stderr], [], [], remaining)
            assert ready, "the static firmware printed no ready line"
            chunk = os.read(proc.stderr.fileno(), 4096)
            assert chunk, "the static firmware exited before its ready line"
            error.extend(chunk)
        pending = bytearray()
        deadline = time.monotonic() + FRAME_TIMEOUT
        while True:
            remaining = deadline - time.monotonic()
            assert remaining > 0, "the static firmware sent no HELLO"
            ready, _, _ = select.select([master], [], [], remaining)
            assert ready, "the static firmware sent no HELLO"
            chunk = os.read(master, 4096)
            assert chunk, "the static firmware closed the pty"
            pending.extend(chunk)
            while pending.find(b"\x00") >= 0:
                end = pending.find(b"\x00")
                unit = bytes(pending[: end + 1])
                del pending[: end + 1]
                frame = wire.decode(unit)
                if frame is not None and frame[0] == 0:
                    return int.from_bytes(frame[2][7:9], "little")
    finally:
        proc.kill()
        proc.wait()
        os.close(master)


def varint_len(value):
    if value < 128:
        return 1
    if value < 16384:
        return 2
    return 3


def node_name_limit(max_frame):
    """Longest --node name fitting the HELLO entry: the same sum the core
    gate refuses, from the frame limit the firmware announces."""
    budget = max_frame - 4
    limit = budget - 17 - 1
    while limit > 0 and 17 + varint_len(limit) + limit > budget:
        limit -= 1
    return limit


def run_static_once(args):
    """One static firmware run that must fail: returns (returncode, stderr)
    with no pty involved."""
    cmd = [static_binary(), "/dev/pts/does-not-exist", *args]
    proc = subprocess.run(cmd, stdin=subprocess.DEVNULL, capture_output=True, timeout=START_TIMEOUT)
    return proc.returncode, proc.stderr.decode()


def test_static_node_name_over_limit_fails():
    limit = node_name_limit(static_hello_max_frame())
    name = "n" * (limit + 1)
    code, error = run_static_once(["--node", name])
    assert code == 2
    assert "--node" in error
    assert name in error
    assert str(limit + 1) in error
    assert f"limit {limit}" in error
    assert "ready" not in error


def test_static_node_name_at_limit_passes_name_check():
    limit = node_name_limit(static_hello_max_frame())
    code, error = run_static_once(["--node", "n" * limit])
    assert code == 1
    assert "does-not-exist" in error
