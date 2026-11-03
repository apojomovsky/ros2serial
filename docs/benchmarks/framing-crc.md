# Framing and CRC benchmark on AVR (P9)

Question: for the 8-bit link, how do rCOBS, COBS, SLIP and a rosserial-style
framing compare on wire size, CPU, flash, RAM and recovery, and does any of
them argue for changing the shipped framing or the CRC?

Verdict: no. rCOBS matches or beats the alternatives on wire size and CPU
while carrying the strongest check (CRC-16/MCRF4XX, HD4 over the whole
default range per R7). The wire protocol, the framing and the CRC stay as
they are. A table-driven CRC would halve CRC cost for 498 bytes of flash;
that is an implementation option for later, not a wire change.

## What ran

Five configs on two boards: rCOBS with bitwise CRC (the shipped code from
`core/`, linked as is), COBS with bitwise CRC, SLIP with bitwise CRC, a
rosserial-style length-prefixed framing with additive checks, and rCOBS
with a table-driven CRC. The alternatives live under `benchmark/framing`
and never in `core/src`. Six payloads cover the realistic span: a Float32
(P1), a zero-heavy Twist-sized payload (P2), 32 zero bytes (P3), the longest
default-max payload (P4), dense mixed bytes (P5) and SLIP-hostile bytes with
`0xC0`, `0xDB` and `0x00` (P6). Decoded sizes run 8 to 126 bytes.

Boards are Uno (ATmega328P) and Mega (ATmega2560), both at 16 MHz, built
with the pinned avr-gcc 7.3.0 at `-Os`, run under simavr from the dev image.
Each firmware encodes and decodes every payload once per round, timed by
Timer1 at prescaler 1, and reports over UART. Every config ran twice; the
two runs agreed cycle for cycle, and the run refuses to report otherwise.
Flash and static RAM come from `avr-size`, stack from the harness
`--stack-paint` high-water mark, recovery from a corruption suite driven
through the same C sources on the host and cross-checked against Python
models (`tools/test_framing_benchmark.py`).

Reproduce with:

```sh
make exec CMD="bash benchmark/framing/run.sh"
make pytest ARGS="tools/test_framing_benchmark.py"
```

## Wire size in bytes

Encoded frame plus delimiter. Decoded sizes: P1 8, P2 52, P3 36, P4 126,
P5 68, P6 36. Identical on both boards.

| Payload | rCOBS | COBS | SLIP | RS model | rCOBS+table |
| --- | --- | --- | --- | --- | --- |
| P1 | 10 | 10 | 9 | 13 | 10 |
| P2 | 54 | 54 | 53 | 57 | 54 |
| P3 | 38 | 38 | 37 | 41 | 38 |
| P4 | 128 | 128 | 127 | 131 | 128 |
| P5 | 70 | 70 | 69 | 73 | 70 |
| P6 | 38 | 38 | 53 | 41 | 38 |

rCOBS and COBS tie everywhere here; SLIP saves one byte except on hostile
input, where its escapes cost 15 bytes over rCOBS on P6. The RS model pays
a fixed 9 bytes per frame (sync, version, length, two checks, 16-bit topic)
against 2 to 3 for the delimited three.

## CPU in cycles per frame on the Uno

Encode covers check computation plus stuffing; decode covers unstuffing,
residue or checksum verification and the round-trip compare. Timer1 deltas,
one encode plus one decode per payload.

| Payload | rCOBS enc/dec | COBS enc/dec | SLIP enc/dec | RS enc/dec | Table enc/dec |
| --- | --- | --- | --- | --- | --- |
| P1 | 1230/1442 | 1339/1515 | 1106/1380 | 291/600 | 732/800 |
| P2 | 8767/8923 | 8196/9456 | 7903/8441 | 1215/2404 | 4552/4564 |
| P3 | 5951/6474 | 5672/7041 | 5412/5854 | 879/1748 | 3107/3486 |
| P4 | 21792/19433 | 19765/19334 | 19293/20275 | 2769/5438 | 11367/8864 |
| P5 | 11714/10821 | 10763/10924 | 10415/11049 | 1551/3060 | 6107/5070 |
| P6 | 6086/5865 | 5711/6000 | 5483/6173 | 879/1748 | 3227/2862 |

Per decoded byte at P4 (126 bytes): encode 173/157/153/22/90, decode
154/153/161/43/70.
The three delimited framers sit within about 10 percent of each other. RS
encodes far faster because two additive checks replace the CRC; its check
is weaker (see R7: no HD4 span), so the cycles are not comparable.
The table CRC roughly halves encode cost wherever the CRC dominates;
decode follows once the residue check uses the table too.

At 115200 baud a 128 byte P4 frame needs about 11 ms of wire time; the
Uno spends about 2.6 ms of CPU on its framing plus CRC. The link binds
first, not the CPU.

The Mega repeats the pattern 2 to 3 percent slower per frame (encode P4:
22298/20019/19545/2771/11873; decode P4: 19689/19590/20531/5440/9120;
full per-payload table in the run output). The wider PC on the 2560 makes
calls and returns cost a cycle more; the code is the same.

Cycle counts depend on exact code layout (a branch flipped by linking
changes single cycles), so small gaps between framers are noise. The
robust gaps are the wire sizes above, the RS speedup with its weaker
check, and the table factor of about two on CRC-heavy frames.

## Flash, RAM and stack in bytes

Totals per firmware on the Uno (Mega in parentheses). Every image carries
the same scaffolding: UART and Timer1 setup, the report printer, 302 bytes
of payload fixtures and 388 bytes of static encode/decode buffers, so the
deltas isolate the framer plus check.

| Config | Flash | RAM | Stack |
| --- | --- | --- | --- |
| rCOBS + bitwise CRC | 1804 (1930) | 798 (798) | 27 (30) |
| COBS + bitwise CRC | 1874 (2000) | 798 (798) | 28 (30) |
| SLIP + bitwise CRC | 1788 (1914) | 798 (798) | 30 (32) |
| RS model | 1922 (2052) | 670 (670) | 54 (56) |
| rCOBS + table CRC | 2302 (2428) | 798 (798) | 27 (30) |

Deltas against shipped on the Uno: COBS +70, SLIP -16, RS +118, table
+498. Symbol sizes (`avr-nm`): COBS encode 198 plus decode 238, SLIP
encode 152 plus decode 198, RS encode 232 plus scan 372, table 512 plus
28 plus 40. RS uses 128 bytes less static RAM (no decode buffer in this
harness) and about twice the stack (the scan loop). None of the framers
moves the tier M budgets: the whole benchmark image, fixtures included,
stays near 2 KB of flash.

## Recovery after corruption

Same C sources as the AVR run, driven on the host through a fixed suite:
F1, one corruption, F2. Delimited framers resynchronize at the next
delimiter; the RS model hunts the next sync byte.

| Corruption | rCOBS / COBS / SLIP | RS model |
| --- | --- | --- |
| One flipped byte in F1 | F1 dropped, F2 accepted | F1 skipped, F2 accepted |
| F1 delimiter lost | merged frame dropped, F1 and F2 lost | F1 skipped, F2 accepted |
| 3 garbage bytes between frames | garbage dropped, F1 and F2 accepted | garbage skipped, F1 and F2 accepted |

Byte counts are exact in the test (`test_c_scan_recovers_the_second_frame`
and the model twins): every stream is consumed through the last good
frame, and nothing after a corruption leaks into a later frame. The one
structural loss is shared by all delimiter framers: losing a delimiter
merges two frames and both are dropped. The RS scanner never merges, at
the price of a byte-by-byte hunt whose cost grows with the garbage run.

## Notes and limits

* The RS model is a faithful shape of rosserial framing (sync, version,
  length plus additive checks, 16-bit topic), reimplemented for the
  benchmark; it is not rosserial code and its numbers do not describe
  rosserial.
* CRC strength is R7's result, not remeasured here: poly `0x1021` holds
  HD4 over the whole default range; the additive RS checks do not.
* The scan cost of long garbage runs was not isolated on AVR; intact-frame
  decode is measured, recovery in bytes and frames is exact.
* Numbers are per exact binary and toolchain (avr-gcc 7.3.0, `-Os`,
  simavr from the dev image). Rerunning `run.sh` reproduces them;
  rebuilding after unrelated core changes may shift single cycles.

## Recommendation

Keep the shipped framing (rCOBS) and the shipped check (CRC-16/MCRF4XX,
bitwise). Nothing here argues for a wire change, so no `needs-decision`
follows. If framing plus CRC ever shows up in a CPU profile, the
table-driven CRC is the first lever: same wire bytes, about twice as fast
on CRC-heavy frames, 498 bytes of flash. That would be its own ticket.
