# R2: prior art

Oct 20, 2026 · research ticket R2 in `proposal-v3.md`

**Method.** Web searches (ROS Discourse, GitHub, project documentation, blogs) on Oct 20, 2026, plus reading the repositories that came up. This is a sweep, not an exhaustive survey: GitLab, non-English forums and private projects were not covered. Links are at the end.

## Recommendation: go

No maintained project offers general publish and subscribe between an 8-bit AVR (Uno or Mega) and ROS 2 with a rosserial-like experience. The official direction (micro-ROS) explicitly targets 32-bit parts with tens of kilobytes of RAM. The closest prior art on the host side is a 2019 OSRF bridge with no matching MCU library. The gap rosserial left when ROS 1 reached end of life in May 2025 is still open.

## Landscape

| Project | What it is | MCU targets | Status | Relevance |
| --- | --- | --- | --- | --- |
| rosserial | The ROS 1 original | 8-bit AVR and up | ROS 1 only; ROS 1 end of life May 2025; last commit on `noetic-devel` June 2021 | The UX to match; see R1 for its limits |
| ros2/ros2#365, "Port rosserial to ROS 2" | 2017 feature request | | Closed in 2018: maintainers judged a direct port the wrong approach. Comments propose a rewrite (named "ros2serial" by its proposer) and later a partial port of `rosserial_python` (2020); neither became a release | Confirms the demand and that no direct port exists. Note the name |
| micro-ROS | ROS 2 client on Micro XRCE-DDS, with an agent on the host | 32-bit MCUs; its docs say it generally needs tens of kilobytes of RAM. The Arduino library supports boards such as Teensy and OpenCR, not the Uno | Active; the official answer for MCUs | Not a competitor on 8-bit parts; a maintainer called Uno and Mega "8 bits MCUs with very few RAM memory" |
| Micro XRCE-DDS client alone | The DDS-XRCE client under micro-ROS | 32-bit; profiled on STM32 | Active | A 2020 analysis cites about 2.5 KB of RAM for a complete publisher and subscriber, more than a Uno has |
| ROBOTIS ros2arduino | Arduino library over XRCE-DDS | OpenCR and other 32-bit boards; its author said it "is not used in 8bit MCU" | Last release 0.2.1, May 2020 | Abandoned in practice |
| osrf/ros2_serial_example | Host bridge: serial to ROS 2, COBS framing with a one-byte topic ID and CRC, topic map from YAML or queried at startup | No MCU library described | README and build examples from 2019 (Crystal) | Closest prior art for the host side. It validates COBS-style framing and one-byte IDs. Differences: types are compiled into the bridge, there is no MCU library, no time sync, no type hashes |
| zenoh-pico over serial with rmw_zenoh | MCU joins a Zenoh network directly, no agent | ESP32, STM32, PX4 flight controllers; the `rmw_zenoh_pico` work targets Linux so far | Experimental | A future option for 32-bit tier L parts; nothing for AVR |
| ros_arduino_bridge and its ROS 2 ports | Application-specific bridge: text commands for motors, encoders, sensors | Uno | Partial ROS 2 ports, unmaintained | Not general publish and subscribe |
| Application bridges (for example CMU cabot-arduino-ace, ESP32 serial bridges) | One-off protocols per project | Mega, ESP32 | Per project | Evidence of demand: cabot moved to raw serial because "ROS2 does not support rosserial, and micro-ROS did not fit" its Mega's RAM |
| Generic serial drivers (`ros2_serial`, `openvmp/serial`, `serial_driver`) | Byte-stream access from ROS 2 nodes | None | Various | Possible building blocks for the bridge's serial layer (R8), not alternatives |

## What ros2serial offers that nothing else does

1. **8-bit AVR as a first-class target**, with measured budgets (2.6 KB flash and 137 B RAM for the K1 workload; see R1).
2. **rosserial's experience in ROS 2**: an Arduino library, a few lines in the sketch, a rosserial-compatible API for porting.
3. **Professional robustness**: a written spec with vectors, a real CRC, bounds checks, fuzzing, reset detection, drift-corrected time.
4. **Any RMW on the host**, including rmw_zenoh, because the bridge is a normal ROS 2 node.
5. **Type-hash checking** (RIHS01), which none of the serial bridges does.

## Risks this sweep surfaced

- **Naming.** "ros2serial" was proposed as the name of a rosserial rewrite in ros2/ros2#365 (2018), and a 2020 comment there started `ros2serial_python` and `ros2serial_msgs`. No released or maintained project under that name was found. The name stays usable, but the open decision on the project name in `proposal-v3.md` should consider searchability and this history.
- **micro-ROS on bigger 8-bit parts.** A trimmed XRCE-DDS client might fit the Mega's 8 KB. Nobody has published such a configuration, but the docs and the Phase 0 post should be precise: ros2serial's case is strongest on the Uno and similar parts, and on simplicity everywhere.
- **zenoh-pico maturing on 32-bit parts** could make tier L less distinctive. That is already a non-goal area (v3 does not compete with micro-ROS on 32-bit MCUs with an RTOS).

## Sources

- rosserial: <https://github.com/ros-drivers/rosserial>, <https://index.ros.org/p/rosserial_arduino/>
- Port rosserial to ROS 2: <https://github.com/ros2/ros2/issues/365>
- micro-ROS supported hardware: <https://micro.ros.org/docs/overview/hardware/>
- micro-ROS on Arduino (Discourse): <https://discourse.ros.org/t/micro-ros-on-arduino/17009>
- micro-ROS Arduino library release: <https://www.hackster.io/news/micro-ros-project-brings-ros-2-to-the-arduino-ide-and-cli-through-an-experimental-library-release-656a72fff2fa>
- Micro XRCE-DDS memory profiling: <https://micro.ros.org/docs/concepts/middleware/memo_prof>, <https://micro-xrce-dds.readthedocs.io/en/stable/optimization.html>
- Notes on ROS 2 and rosserial (2020): <https://newscrewdriver.com/2020/08/05/notes-on-ros2-and-rosserial/>
- ros2arduino: <https://reference.arduino.cc/reference/en/libraries/ros2arduino>, <https://www.arduinolibraries.info/libraries/ros2arduino>, <https://discourse.openrobotics.org/t/ros2arduino-discussion-for-development-ros2-library-for-arduino/6498>
- osrf/ros2_serial_example: <https://github.com/osrf/ros2_serial_example>
- Zenoh and microcontrollers: <https://discourse.ros.org/t/integrating-ros-2-with-microcontrollers-when-using-zenoh/43463>, <https://discourse.openrobotics.org/t/reference-implementation-of-zenoh-pico-to-micro-ros/40750>, <https://docs.px4.io/main/en/middleware/zenoh>
- ros_arduino_bridge ports: <https://github.com/hbrobotics/ros_arduino_bridge>, <https://github.com/benbongalon/ros_arduino_bridge>
- Application bridges: <https://github.com/CMU-cabot/cabot-arduino-ace>, <https://github.com/marcospontoexe/comunication_serial>, <https://github.com/anasderkaoui/ROS2-and-Arduino-serial-communication>
- Generic serial drivers: <https://github.com/RozaGkliva/ros2_serial>, <https://github.com/openvmp/serial/blob/main/README.md>
