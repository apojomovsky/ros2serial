"""Drive the real C session (libr2s_host) through ctypes.

The shared library is the core plus the fake port and the handwritten test
firmware, built with R2S_TESTING by scripts/build-host-lib.sh. Each method
is one C call; the tests step this driver and the session model together and
compare every observable. The host-side vector runner of #250 reuses this
module to replay its transcripts through the same library.
"""

from __future__ import annotations

import ctypes
import os
import subprocess
from pathlib import Path

BATTERY_ID = 16
CMD_VEL_ID = 17

_COUNTER_FIELDS = [
    ("rx_frames", ctypes.c_uint16),
    ("crc_errors", ctypes.c_uint16),
    ("rx_overflows", ctypes.c_uint16),
    ("dropped_frames", ctypes.c_uint16),
]


class _Counters(ctypes.Structure):
    _fields_ = _COUNTER_FIELDS  # type: ignore[assignment]


class _Battery(ctypes.Structure):
    _fields_ = [("data", ctypes.c_float)]


def default_lib_dir() -> Path:
    """Where build-host-lib.sh puts the libraries, without arguments."""
    override = os.environ.get("R2S_HOST_LIB_DIR")
    if override:
        return Path(override)
    build = os.environ.get("R2S_BUILD_DIR")
    if not build:
        top = subprocess.run(
            ["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True, check=True
        ).stdout.strip()
        build = str(Path.home() / ".cache" / "ros2serial" / f"build{top.replace('/', '-')}")
    return Path(build) / "host"


def find_library(name: str = "libr2s_host.so", lib_dir: Path | None = None) -> Path:
    """The library path, or a precise error when it is not built yet."""
    directory = lib_dir if lib_dir is not None else default_lib_dir()
    path = directory / name
    if not path.is_file():
        raise FileNotFoundError(
            f"{path} is missing: run scripts/build-host-lib.sh {directory} first "
            "(the GEN suite section builds it before pytest runs)"
        )
    return path


class HostSession:
    """The C session behind the fake port: feed bytes, spin, read state."""

    def __init__(self, lib_path: Path | None = None) -> None:
        self.lib = ctypes.CDLL(str(find_library() if lib_path is None else lib_path))
        lib = self.lib
        lib.fake_port_reset.argtypes = []
        lib.fake_port_reset.restype = None
        lib.fake_port_queue_rx.argtypes = [ctypes.c_char_p, ctypes.c_uint16]
        lib.fake_port_queue_rx.restype = None
        lib.fake_port_set_tick.argtypes = [ctypes.c_uint32]
        lib.fake_port_set_tick.restype = None
        lib.fake_port_tx_frame_count.argtypes = []
        lib.fake_port_tx_frame_count.restype = ctypes.c_uint16
        lib.fake_port_tx_frame.argtypes = [ctypes.c_uint16, ctypes.POINTER(ctypes.c_uint16)]
        lib.fake_port_tx_frame.restype = ctypes.c_void_p
        lib.fake_port_tx_clear.argtypes = []
        lib.fake_port_tx_clear.restype = None
        lib.r2s_test_reset.argtypes = []
        lib.r2s_test_reset.restype = None
        lib.r2s_handwritten_configure.argtypes = []
        lib.r2s_handwritten_configure.restype = ctypes.c_int8
        lib.r2s_init.argtypes = []
        lib.r2s_init.restype = None
        lib.r2s_spin_some.argtypes = []
        lib.r2s_spin_some.restype = None
        lib.r2s_connected.argtypes = []
        lib.r2s_connected.restype = ctypes.c_uint8
        lib.r2s_session_token.argtypes = []
        lib.r2s_session_token.restype = ctypes.c_uint32
        lib.r2s_session_topic_status.argtypes = [ctypes.c_uint8]
        lib.r2s_session_topic_status.restype = ctypes.c_uint8
        lib.r2s_session_topic_flags.argtypes = [ctypes.c_uint8]
        lib.r2s_session_topic_flags.restype = ctypes.c_uint8
        lib.r2s_session_tx_seq.argtypes = [ctypes.c_uint8]
        lib.r2s_session_tx_seq.restype = ctypes.c_uint8
        lib.r2s_session_rx_lost.argtypes = [ctypes.c_uint8]
        lib.r2s_session_rx_lost.restype = ctypes.c_uint16
        lib.r2s_framer_counters.argtypes = []
        lib.r2s_framer_counters.restype = ctypes.POINTER(_Counters)
        lib.r2s_publish_battery.argtypes = [ctypes.POINTER(_Battery)]
        lib.r2s_publish_battery.restype = ctypes.c_int8
        lib.r2s_host_error.argtypes = []
        lib.r2s_host_error.restype = ctypes.c_char_p

    def reset(self) -> None:
        """The boot the model mirrors: blank port and counters, then HELLO."""
        self.lib.fake_port_reset()
        self.lib.r2s_test_reset()
        assert self.lib.r2s_handwritten_configure() == 0
        self.lib.r2s_init()
        self.check_error()

    def queue(self, data: bytes) -> None:
        """Queue host bytes without spinning, so a backlog can build."""
        self.lib.fake_port_queue_rx(data, len(data))
        self.check_error()

    def spin_now(self) -> None:
        """Spin once, processing at most R2S_SPIN_MAX_FRAMES units."""
        self.lib.r2s_spin_some()
        self.check_error()

    def feed(self, data: bytes) -> None:
        """Queue host bytes and spin once, as one slow link delivery."""
        self.queue(data)
        self.spin_now()

    def advance_to(self, tick: int) -> None:
        """Move the fake clock, then spin once for the timers."""
        self.lib.fake_port_set_tick(tick & 0xFFFFFFFF)
        self.lib.r2s_spin_some()
        self.check_error()

    def publish_battery(self, value: float) -> int:
        """Publish on battery; the return code is part of the comparison."""
        rc = int(self.lib.r2s_publish_battery(ctypes.byref(_Battery(value))))
        self.check_error()
        return rc

    def take_tx(self) -> list[bytes]:
        """The captured frames since the last call, without delimiters."""
        out = []
        count = int(self.lib.fake_port_tx_frame_count())
        for i in range(count):
            size = ctypes.c_uint16()
            ptr = self.lib.fake_port_tx_frame(i, ctypes.byref(size))
            assert ptr is not None
            out.append(ctypes.string_at(ptr, size.value))
        self.lib.fake_port_tx_clear()
        return out

    def snapshot(self) -> dict[str, object]:
        """Every observable the tests compare after a step."""
        counters = self.lib.r2s_framer_counters().contents
        return {
            "connected": int(self.lib.r2s_connected()),
            "token": int(self.lib.r2s_session_token()),
            "status": [int(self.lib.r2s_session_topic_status(i)) for i in range(3)],
            "counters": (
                int(counters.rx_frames),
                int(counters.crc_errors),
                int(counters.rx_overflows),
                int(counters.dropped_frames),
            ),
            "tx_seq": [int(self.lib.r2s_session_tx_seq(i)) for i in range(3)],
            "rx_lost": [int(self.lib.r2s_session_rx_lost(i)) for i in range(3)],
        }

    def topic_flags(self, index: int) -> int:
        """The announced CTL-4 flags of the topic at `index`."""
        return int(self.lib.r2s_session_topic_flags(index))

    def check_error(self) -> None:
        """Fail on a fake port misuse, which would hide lost bytes."""
        message = self.lib.r2s_host_error()
        assert not message, message.decode()
