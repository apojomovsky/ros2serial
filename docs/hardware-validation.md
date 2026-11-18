# Real-board validation

What is proven where, and the results of the maintainer's board runs. Firmware
in this project is verified in simulation and in CI; a real board adds the parts
simulation cannot show, such as the USB-serial adapter, the USB reset on an Uno,
and the bootloader timing.

## What is tested where

| Target | Coverage | What that proves |
| --- | --- | --- |
| Uno, Mega | Simulation in CI (simavr) | Frames, handshake, services, actions and the end-to-end scenarios, on the board's own instruction set |
| ESP32 | Build in CI | The sketch and the library compile for the target |
| CH32V003, STM32F103, RP2040 | Build in CI | The core and port compile for the target |
| PIC18F4550 | Build in CI, advisory | The port compiles with epic-cc; the simulator run is advisory |
| A real Uno and a real Mega | Board run by the maintainer | Everything above plus the adapter, the reset and the bootloader |

Compiling for a board is not a promise about its hardware. Only the rows marked
as simulation or a board run say anything about behavior.

## How to run a board check

A one-command board check is not built yet; it is tracked as
[#664](https://github.com/apojomovsky/ros2serial/issues/664). Until it lands,
follow the [quickstart](../README.md) on the real board: compile the example,
upload it with `arduino-cli`, launch the bridge on the reported port, and read
the topics with `ros2 topic echo`.

Two things differ on real hardware:

- Opening an Uno's or Mega's port resets the board through DTR, and the
  bootloader runs for about two seconds. The bridge waits `boot_wait_ms` (2500
  by default) before it talks.
- The adapters differ. A clone board may carry a CH340, whose port name and
  reset behavior differ from the 16U2 on an official board.

## Results

The results below are filled in by the maintainer after each board run. A
result that has not been produced yet reads `pending`; nothing here is inferred
from simulation.

| Run | Board | Commit | Verdict | Notes |
| --- | --- | --- | --- | --- |
| 0.5.0 board run | Uno | pending | pending | Waiting for the maintainer's run |
| 0.5.0 board run | Mega | pending | pending | Waiting for the maintainer's run |

A row is filled in with the firmware commit, the Arduino core version and the
verdict, or with the failure and the ticket it became.

## Reporting one

Include the board and adapter, the commit, `ros2 run r2s_gen r2s-gen version`,
the Arduino core version, the upload command and the bridge log. The
[troubleshooting page](troubleshooting.md) lists the checks that come first.
