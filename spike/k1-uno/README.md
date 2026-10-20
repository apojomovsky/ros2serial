# K1 spike: the v3 protocol on a real Uno

Throwaway code for ticket K1 in `docs/00-design.md`: handwritten firmware that speaks the v3 framing (rCOBS, CRC-16/MCRF4XX) and a minimal session, plus a host script that measures the link. No ROS involved. Nothing here ships; the directory is deleted once K1's measurements are recorded.

## What is already done (no hardware)

- **Sizes** on an ATmega328P, built with avr-gcc 7.3.0 and the Arduino AVR core 1.8.6 flags (`bench/run_bench.sh`):

  | Build | Flash | Static RAM |
  | --- | --- | --- |
  | Baseline (Serial only) | 1,460 B | 184 B |
  | K1 spike | 4,082 B | 321 B |
  | rosserial, smallest NodeHandle | 10,448 B | 890 B |
  | rosserial, Uno default | 10,554 B | 1,354 B |

  The spike is +2,622 B flash and +137 B RAM over the baseline: inside the tier M targets (under 4 KB and 400 B). On a Mega it builds to 4,424 B flash and the same RAM.
- **Function**, in simavr at real-time pace with the UART on a pty (`sim/run_sim_test.sh`): handshake, 50 Hz Float32 with no losses, every Twist echoed back intact, no CRC errors, round trip about 2.5 ms minimum and 5 ms median, MCU reset detected and recovered within the run. The drift figure from simavr only reflects the simulator's pacing.

## What the hardware run measures

The things simavr cannot tell us: real USB-serial latency (the Uno's ATmega16U2, or a CH340 on clones), the resonator's clock error, behavior at higher baud rates, the DTR reset on port open, and sizes from your own toolchain.

## Weekend steps

You need a Uno (and a Mega if you have one), `arduino-cli` or the Arduino IDE, and Python 3 with `pip install pyserial`.

1. **Build and flash.**
   ```bash
   arduino-cli compile --fqbn arduino:avr:uno firmware/k1_spike
   arduino-cli upload  --fqbn arduino:avr:uno -p /dev/ttyACM0 firmware/k1_spike
   ```
   Note the sizes `arduino-cli` prints. They should be within a few bytes of the table above.
2. **Baseline run, 60 s at 115200.**
   ```bash
   python3 host/spike_host.py --port /dev/ttyACM0 --seconds 60 --json | tee uno-115200.json
   ```
   Expect `battery_lost: 0` and `echo_ok == twist_sent`. Record `drift_ppm` (the resonator error, likely hundreds to thousands of ppm), `rtt_ms_min` and `rtt_ms_median`.
3. **Reset while running.** Start a 30 s run and press the board's reset button halfway. The script should print `MCU reset detected` and finish with `resets: 1`. Note how long the bootloader keeps it silent.
4. **Port open reset.** Every run already opens the port, which resets the Uno; the script waits up to 2.5 s for the unsolicited HELLO. Note whether the HELLO arrives within that window (`--boot-wait` changes it).
5. **Baud matrix.** For each of 250000, 500000 and 1000000:
   ```bash
   arduino-cli compile --fqbn arduino:avr:uno --build-property "build.extra_flags=-DSPIKE_BAUD=500000" firmware/k1_spike
   arduino-cli upload  --fqbn arduino:avr:uno -p /dev/ttyACM0 firmware/k1_spike
   python3 host/spike_host.py --port /dev/ttyACM0 --baud 500000 --seconds 60 --json | tee uno-500000.json
   ```
6. **Stress.** At 115200, `--twist-hz 100` and `--twist-hz 200`. Watch `mcu.rx_overflows`, `mcu.dropped` and `mcu.tx_busy`: this is where the 64-byte Arduino RX buffer shows up.
7. **Mega,** if available: repeat steps 1 and 2 with `--fqbn arduino:avr:mega` and its port.
8. **Record** the JSON files and observations on the K1 issue. If the sizes and the zero-loss results hold, K1's gate (tier M targets confirmed) is met.

## Layout

- `firmware/k1_spike/`: the sketch. Topics 16 `battery` (Float32, published at 50 Hz with a running count so losses are exact), 17 `cmd_vel` (Twist, subscribed), 18 `cmd_vel_echo` (Twist, published back on every `cmd_vel`). Heartbeats carry the MCU counters plus a `tx_busy` count.
- `host/spike_host.py`: handshake, traffic, measurements, reset detection.
- `sim/`: the simavr harness (real-time pacing, UART on a pty, `SIGUSR1` resets the MCU) and its test script.
- `bench/`: the size comparison with rosserial, including a generator driver that produces rosserial's message headers without ROS 1.
- `tools/abuild.sh`: builds a sketch with the Arduino AVR core's own flags, without `arduino-cli`.

## Simplifications on purpose

- RIHS01 type hashes are sent as zeros; the host script does not check them.
- No time correction: the MCU only answers pings, and the host computes drift.
- Control frames (HELLO, TOPIC) block on a full TX buffer; only user topics check free space first.
- Twist travels narrow (six float32), as v3 forces on a 32-bit `double` build.
