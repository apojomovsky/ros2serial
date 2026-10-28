"""One serial line between two pty slaves, joined by a relay thread.

Bytes written on one slave come out of the other, so the bridge and the
firmware each open a plain serial device. The test images carry no socat,
hence the thread. Each end opens its slave by name; this thread joins the
two masters.
"""

import os
import pty
import select
import threading
import time


class PtyRelay:
    """Two pty slaves tied master to master, forwarding both ways."""

    def __init__(self):
        self._mcu_master, mcu_slave = pty.openpty()
        self._host_master, host_slave = pty.openpty()
        self.mcu_port = os.ttyname(mcu_slave)
        self.host_port = os.ttyname(host_slave)
        os.close(mcu_slave)
        os.close(host_slave)

    def start(self):
        """Serve both directions until stop is called."""
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name="pty-relay", daemon=True)
        self._thread.start()

    def _forward(self, source, target):
        # A master reads EIO until its slave is open and after it closes, so
        # a failure here only skips one turn: both processes open their slaves
        # just after this thread starts.
        try:
            chunk = os.read(source, 4096)
        except OSError:
            time.sleep(0.005)
            return
        if not chunk:
            return
        view = memoryview(chunk)
        while view:
            try:
                written = os.write(target, view)
            except OSError:
                time.sleep(0.005)
                return
            view = view[written:]

    def _run(self):
        while not self._stop.is_set():
            readable, _, _ = select.select([self._mcu_master, self._host_master], [], [], 0.05)
            for source in readable:
                target = self._host_master if source == self._mcu_master else self._mcu_master
                self._forward(source, target)

    def stop(self):
        """Join the thread and close both masters."""
        self._stop.set()
        self._thread.join(timeout=5.0)
        os.close(self._mcu_master)
        os.close(self._host_master)
