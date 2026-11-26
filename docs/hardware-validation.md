# Real-board validation

What is proven where, and the results of the maintainer's board runs. Firmware
in this project is verified in simulation and in CI; a real board adds the parts
simulation cannot show, such as the USB-serial adapter, the USB reset on an Uno,
and the bootloader timing.

## What is tested where

| Target | Coverage | What that proves |
| --- | --- | --- |
| Uno, Mega | Simulation in CI (simavr) | Frames, handshake, services, actions and the end-to-end scenarios, on the board's own instruction set |
| ESP32 | build in CI | The sketch and the library compile for the target |
| ESP32-S3 (UART) | build in CI | The sketch and the library compile for UART0 through the bridge chip |
| ESP32-S3 (USB CDC) | build in CI | The sketch and the library compile for the native USB port |
| ESP32-C3 (USB CDC) | build in CI | The sketch and the library compile for the native USB port |
| CH32V003, STM32F103, RP2040 | Build in CI | The core and port compile for the target |
| PIC18F4550 | Build in CI, advisory | The port compiles with epic-cc; the simulator run is advisory |
| A real Uno and a real Mega | Board run by the maintainer | Everything above plus the adapter, the reset and the bootloader |

Compiling for a board is not a promise about its hardware. Only the rows marked
as simulation or a board run say anything about behavior.

## How to run a board check

From the repository root, with the board plugged in and the dev image built
(`make image`):

```sh
make hardware-check PORT=/dev/ttyACM0 BOARD=uno
```

An ESP32 board names its row in `ports/arduino/boards.list` instead. An
ESP32-S3 leaves the factory either as a COM/UART bridge or as native USB,
and the row fixes which one the check expects. [ESP32 boards](esp32.md)
covers the board, port and FQBN choices behind those rows:

```sh
make hardware-check PORT=/dev/ttyACM0 BOARD=esp32s3
make hardware-check PORT=/dev/ttyACM0 BOARD=esp32s3-usb
```

The first row is the COM port: a CH343 bridge enumerates as ttyACM, while a
CP210x or CH340 enumerates as ttyUSB. The second row is the USB port, the
board's native USB. Before the container starts, the script compares the
port's USB vendor against the row: a `-usb` board on a port whose vendor is
not `303a`, or a non-`-usb` ESP32 board on a `303a` port, stops with which
port to use. Use `PORT=/dev/serial/by-id/...` when the board re-enumerates
under another name, so the rerun finds the same board.

It flashes `button_led`, starts the bridge, samples `/button` for half a
minute, reads the link counters, drives `/led` and asks you whether it lit,
then prints one report block ending in a `verdict:` line. The port and the
board are both required, so the check cannot flash a board it was not told
about. `BOARD` accepts every name in `ports/arduino/boards.list`, and the
usage line lists them. [Checking a real board](without-hardware.md#checking-a-real-board)
describes every field of the report and what each failure means.

An ESP32 needs its Arduino core first. `scripts/esp32-core.sh` fetches the
core pinned in `ci/versions.env` into the cache (about 1.7 GB to download,
about 7.8 GB on disk including the extracted toolchains, measured 2026-11-23),
so the first ESP32 check pays a one-time download and every later one is
offline. The upload then waits up to 10 s for the port to come back, because
a USB-Serial/JTAG board re-enumerates after the reset.

Two things differ on real hardware:

- Opening an Uno's or Mega's port resets the board through DTR, and the
  bootloader runs for about two seconds. The bridge waits `boot_wait_ms` (2500
  by default) before it talks, which also covers an ESP32 reset on open.
- The adapters differ. A clone board may carry a CH340, whose port name and
  reset behavior differ from the 16U2 on an official board.

## Results

The results below are filled in by the maintainer after each board run. A row
with no run yet reads `pending` in both the commit and the verdict cells, and a
board that was unavailable reads `not run`; nothing here is inferred from
simulation.

| Run | Board | Commit | Verdict | Notes |
| --- | --- | --- | --- | --- |
| 0.5.0 board run | Uno | - | not run for 0.5.0 | Board unavailable |
| 0.5.0 board run | Mega | `master` before the 0.5.0 cut | PASS | `make hardware-check` on 2026-11-18, Arduino AVR core 1.8.6; no figures recorded |

A row is filled in with the firmware commit, the Arduino core version and the
verdict, or with the failure and the ticket it became.

## Reporting one

Include the board and adapter, the commit, `ros2 run r2s_gen r2s-gen version`,
the Arduino core version, the upload command and the bridge log. The
[troubleshooting page](troubleshooting.md) lists the checks that come first.
