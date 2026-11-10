/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors
 */

/* RP2040: the SDK boot stage opens flash, then the firmware follows it.
 * Flash is 2 MB; RAM is 256 KB. */
MEMORY
{
  BOOT2 (rx) : ORIGIN = 0x10000000, LENGTH = 256
  FLASH (rx) : ORIGIN = 0x10000100, LENGTH = 2M - 256
  RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 256K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
  .boot2 :
  {
    KEEP(*(.boot2))
  } > BOOT2

  .isr_vector :
  {
    KEEP(*(.isr_vector))
  } > FLASH

  .text :
  {
    *(.text*)
    *(.rodata*)
  } > FLASH

  _sidata = LOADADDR(.data);
  .data :
  {
    _sdata = .;
    *(.data*)
    _edata = .;
  } > RAM AT > FLASH

  .bss :
  {
    _sbss = .;
    *(.bss*)
    *(COMMON)
    _ebss = .;
  } > RAM

  /DISCARD/ :
  {
    *(.ARM.exidx*)
    *(.ARM.extab*)
    *(.eh_frame*)
  }
}
