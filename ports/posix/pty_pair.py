#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""One serial line between two pty slaves, for the posix quickstart.

Prints MCU_PORT and HOST_PORT assignments, then relays both ways until
killed. The firmware and the bridge each open a plain serial device.
"""

import os
import pty
import select
import sys
import time


def _forward(source, target):
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


def main():
    mcu_master, mcu_slave = pty.openpty()
    host_master, host_slave = pty.openpty()
    print(f'MCU_PORT={os.ttyname(mcu_slave)}', flush=True)
    print(f'HOST_PORT={os.ttyname(host_slave)}', flush=True)
    os.close(mcu_slave)
    os.close(host_slave)
    while True:
        readable, _, _ = select.select([mcu_master, host_master], [], [], 0.05)
        for source in readable:
            target = host_master if source == mcu_master else mcu_master
            _forward(source, target)


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
