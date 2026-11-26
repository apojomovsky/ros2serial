# ESP32 boards

The ESP32, ESP32-S3 and ESP32-C3 are built in CI via the Arduino layout.
That proves the examples compile for each chip; it is not a board run.
[Real-board validation](hardware-validation.md) records the level of each
variant, and a variant moves to a board run only when a maintainer run
passes there. The link stays serial, over UART or USB CDC; WiFi is not
used as a transport.

## Install the ESP32 core

In the Arduino IDE, add this Boards Manager URL under *File > Preferences >
Additional Boards Manager URLs*, then install the `esp32` core by
Espressif Systems at version 3.3.12:

```text
https://espressif.github.io/arduino-esp32/package_esp32_index.json
```

With Arduino CLI, the same install is:

```sh
arduino-cli core update-index --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core install --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json esp32:esp32@3.3.12
```

Version 3.3.12 is the pinned core version tested in CI; it is the
`ARDUINO_ESP32_CORE_VERSION` pin in `ci/versions.env`. A different core
version may compile, but it is not the version the CI builds use.

## Choose the board

Pick the board name that matches the chip, in the IDE under *Tools >
Board* or as the `arduino-cli` FQBN:

| Chip | Board to choose | FQBN for UART | FQBN for USB CDC |
| --- | --- | --- | --- |
| ESP32 | ESP32 Dev Module | `esp32:esp32:esp32` | none, the classic ESP32 has no native USB |
| ESP32-S3 | ESP32S3 Dev Module | `esp32:esp32:esp32s3` | `esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc` |
| ESP32-C3 | ESP32C3 Dev Module | `esp32:esp32:esp32c3` | `esp32:esp32:esp32c3:CDCOnBoot=cdc` |

These FQBNs are the rows of `ports/arduino/boards.list`, whose names
(`esp32`, `esp32s3`, `esp32s3-usb`, `esp32c3`, `esp32c3-usb`) are also
the `BOARD` values `make hardware-check` accepts. The classic ESP32
reaches the host through a USB-to-UART bridge chip, so `Serial` is
UART0. The S3 and C3 have native USB as well, which the next section
covers.

## Two-port boards: pick one port and build for it

Many ESP32-S3 boards carry two USB connectors, and the same choice
applies to an ESP32-C3 wired for both UART and native USB. The one labelled `COM`
or `UART` goes through a bridge chip to UART0. The one labelled `USB`
goes straight to the chip's own USB. The sketch's `Serial` must be the
port the bridge will open, and a board option decides which one that is:

- On the `COM`/`UART` port, set `USB CDC On Boot: Disabled`. `Serial`
  is then UART0, and the plain FQBN for the chip is the right build.
- On the `USB` port, set `USB CDC On Boot: Enabled`, and on the S3 also
  `USB Mode: Hardware CDC and JTAG`. Then `Serial` is USB and
  `Serial0` is UART0, and the `-usb` FQBN above is the right build.

The examples call `node.begin(Serial, 115200)`, so they follow
`Serial`. A board built for one port and plugged into the other runs,
but the bridge talks to the silent port and no topics appear; see
[Troubleshooting](troubleshooting.md). Note that the baud is ignored on USB CDC:
`115200` sets no line speed there, while the same value is the real
baud rate on UART0.

## Tell the ports apart on the host

Run `lsusb` with the board plugged in. A vendor of `303a` is the
chip's own USB (Espressif); any other vendor is the bridge chip on the
`COM`/`UART` port, so the vendor alone says which cable you hold.
`arduino-cli board list` shows the current device node for each port.

Prefer the stable name under `/dev/serial/by-id/` over `ttyACM0` or
`ttyACM1` in the bridge launch and in reports: the `ttyACMn` name can
change after a reset, when the board re-enumerates. For example:

```sh
ls -l /dev/serial/by-id/
ros2 launch r2s_bridge r2s_bridge.launch.py port:=/dev/serial/by-id/<your-board>
```

[Installation](installation.md) covers the host workspace and the AVR
core; this page adds only the ESP32 core, board and port choices.
