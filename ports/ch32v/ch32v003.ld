/* CH32V003F4P6 memory: 16 KB flash for the image, 2 KB SRAM for data.
 * The stack grows down from the top of SRAM; the heap is unused, since
 * the core never allocates. Small-data inputs ride in the data and BSS
 * outputs, so the startup copy and zero loops cover them. */
MEMORY
{
  FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 16K
  RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 2K
}

ENTRY(Reset_Handler)

/* The global pointer sits at the top of SRAM: every RAM address lands
 * within its reach, and the linker keeps out-of-reach uses loud. */
PROVIDE(__global_pointer$ = ORIGIN(RAM) + LENGTH(RAM));

SECTIONS
{
  .text :
  {
    KEEP(*(.text.Reset_Handler))
    *(.text .text.*)
    *(.rodata .rodata.*)
    *(.srodata .srodata.*)
    _etext = .;
  } > FLASH

  _data_lma = LOADADDR(.data);
  .data :
  {
    _data_vma = .;
    *(.data .data.*)
    *(.sdata .sdata.*)
    _edata = .;
  } > RAM AT > FLASH

  .bss :
  {
    _sbss = .;
    *(.bss .bss.*)
    *(.sbss .sbss.*)
    *(COMMON)
    _ebss = .;
  } > RAM

  _estack = ORIGIN(RAM) + LENGTH(RAM);
}
