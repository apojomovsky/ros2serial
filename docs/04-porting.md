# Port to a new board

A port is four C functions plus an init, a build script, and a host
test. The core never touches hardware: it reaches the device only
through `core/include/r2s/r2s_port.h`, resolved at link time, so a port
compiles its glue against the headers and links it with the core. The
worked examples are `ports/posix/`, `ports/arduino/`, `ports/ch32v/`,
`ports/stm32/`, and `ports/rp2040/`; the starting point for a new one is
`ports/template/`, which this guide copies.

## The contract

| Function | What it does |
| --- | --- |
| `r2s_port_read(buf, max)` | Copies up to `max` waiting bytes, returns the count, 0 when none wait |
| `r2s_port_write(buf, len)` | Sends `len` bytes |
| `r2s_port_tx_free()` | Bytes writable without blocking, or `0xFFFF` when blocking is fine |
| `r2s_port_tick()` | Free-running tick at `R2S_TICK_HZ` (1000 per second), wraps at 2^32 |

While the core assembles a frame it asks for one byte at a time, so a
port never has to buffer past a delimiter for it. After the core asks
`r2s_port_tx_free()`, it writes at most that many bytes in one or more
calls before asking again; the port accepts them without blocking
unless it reported `0xFFFF`. A build with `R2S_WITH_RX_RING` gets
`r2s_port_read()` from the core itself, and the port must not define it.
A build with `R2S_MAX_LINKS` above 1 also implements
`r2s_port_read_link()` and `r2s_port_tick_link()`; with 1 the framer
reaches every port through the four functions above.

## Mapping the contract onto a device

`main()` calls the port init once at boot, then the core calls the four
functions. The init sets the clock, maps the pins, starts the UART at
115200 8-N-1 with transmitter and receiver on, and starts the tick
source. The read and write poll the status bits: RXNE (a byte waits),
TXE (space to send). Where the vendor HAL already keeps its own
interrupt-driven ring, as on Arduino (`HardwareSerial`) and epic-hal
(`lib/serial`), the read drains that ring and the core keeps no second
one, which saves RAM on a small part.

## What the tick may do

The core reads time only through `r2s_port_tick()`: it never waits,
sleeps, or busy-loops. The tick is a free-running counter that wraps at
2^32 with no interrupt needed; a 32-bit aligned read is atomic on most
cores, so the reader needs no lock. It must never block or sleep, and
it should stay near 1 ms accuracy, since the host estimates the MCU
clock from it during time sync. The rate is announced in the handshake.
The template runs its timer at 1 kHz and reads the counter as the tick; ch32v
accumulates the SysTick count into a millisecond tick; Arduino reads `micros()`;
the posix port reads `CLOCK_MONOTONIC`.

## Transport and budgets

ros2serial stays serial-only: the port provides UART or USB CDC bytes,
and the bridge opens that line on the host. On an Uno the line resets
the board when it opens, so the bridge waits out the bootloader
(`boot_wait_ms` in the bridge parameters).

A port has to meet a memory and flash budget by tier: tier S stays
under 200 B of RAM including buffers and under 4K words of flash; tier
M stays under 400 B of RAM and under 4 KB of flash over an empty sketch
with the same serial driver. `make size-report` measures the firmware
with the pinned toolchains and judges the budgets in CI. Constant data
stays in flash with `R2S_FLASH` on parts that copy constants to RAM.

The glue keeps the core portable: no function pointers, no float
arithmetic, no `malloc`, bounded loops. C++ glue is for gcc-class
targets only, as in `ports/arduino/`.

## Host tests with stubs

Every port tests its contract on the host, in the `core` scope. The
pattern is a fake peripheral plus a `run.sh` pair:

- Fake registers behind the port init, as in
  `ports/ch32v/r2s_ch32v_regs.h`: a struct per peripheral, real base
  addresses on target, test structs under `R2S_CH32V_HOST_TEST` on host.
  The test primes status and data bits and reads back the order bytes
  left the capture buffer in.
- A test binary in `tests/` in the repo C style: one `check_*` function
  per behavior, a `main` that sums failures, no sleeps, state reset
  before each check.
- A `run.sh` that compiles the port with the host compiler under the
  `R2S_*_HOST_TEST` flag and runs the binary, and a `build.sh` that
  compiles the port against the real core. Both keep the tree clean by
  building under `R2S_BUILD_DIR` and serialize on the port lock, as in
  `ports/ch32v/run.sh`.

## Copy the template

```sh
cp -r ports/template ports/myboard
mv ports/myboard/r2s_template_port.c ports/myboard/r2s_myboard_port.c
mv ports/myboard/r2s_template_port.h ports/myboard/r2s_myboard_port.h
mv ports/myboard/r2s_template_regs.h ports/myboard/r2s_myboard_regs.h
mv ports/myboard/tests/test_template_port.c ports/myboard/tests/test_myboard_port.c
```

Then rename `TEMPLATE` to `MYBOARD` in the four files, in both
cases: the type names, the register macros, the `R2S_MYBOARD_HOST_TEST`
flag, and the `#include` lines all move to the new name.

```sh
sed -i 's/TEMPLATE/MYBOARD/g; s/template/myboard/g' \
  ports/myboard/r2s_myboard_port.c ports/myboard/r2s_myboard_port.h \
  ports/myboard/r2s_myboard_regs.h ports/myboard/tests/test_myboard_port.c
```

The two scripts need no edits: they find their sources from their own
directory, so the copy builds and tests under its own name. Point the
two base addresses in `r2s_myboard_regs.h` at the new device:

```text
r2s_template_uart_t -> r2s_myboard_uart_t (rename the types)
R2S_TEMPLATE_UART -> ((r2s_myboard_uart_t *)0x40013800u) (the new UART base)
R2S_TEMPLATE_TIMER -> ((r2s_myboard_timer_t *)0xE000E010u) (the new counter base)
```

Fill in `r2s_myboard_init()` for the new clock tree and pins, keeping
the 8-N-1 setup and the free-running tick. Then build the port against
the real core and run its host test:

```sh
bash ports/myboard/build.sh
bash ports/myboard/run.sh
```

The build compiles the port with the host compiler against
`core/include`, then lists the port object for the four contract
symbols, so a rename that drifts from the contract fails there. The run
compiles the port with `R2S_MYBOARD_HOST_TEST` against the fake
registers and runs `tests/test_myboard_port.c`. Extend that test for
each behavior the new init adds: the divisor for the new clock, the pin
mapping, the tick rate.

A layout gate fails `ports/` directories missing a build script,
host test or implementation: `scripts/check_layout.py` enforces all three,
and `scripts/test_check_layout.py` pins the rule. `ports/common/` is shared
sources, not a port, and a directory holding only a `.gitkeep` is a
placeholder, not a port yet.

## Checklist

- The four functions behave as the table above says, including the
  empty-read and zero-length cases.
- The tick runs free at 1000 Hz, wraps at 2^32, and never blocks.
- The host test passes, and `build.sh` compiles the port against the
  real core and proves its four symbols.
- The firmware meets its tier budget under `make size-report`.
