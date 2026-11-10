/* CH32V003 reset: stack, trap park, BSS and data, then main.
 * QingKe V2A enters here from the flash base; the polled firmware sets no
 * vectored interrupts, so every trap parks in one loop. */
  .section .text.Reset_Handler, "ax"
  .global Reset_Handler
  .type Reset_Handler, @function
Reset_Handler:
  .option push
  .option norelax
  la gp, __global_pointer$
  .option pop
  la sp, _estack
  /* The CSR control below needs the extension named on old drivers. */
  .option arch, +zicsr
  /* Machine mode with interrupts off: the firmware polls everything. */
  li a0, 0x1880
  csrw mstatus, a0
  /* Direct trap park: no vectored handlers on this firmware. */
  la a0, trap_park
  csrw mtvec, a0
  /* Clear BSS. */
  la a0, _sbss
  la a1, _ebss
  li a2, 0
  bge a0, a1, 2f
1:
  sw a2, 0(a0)
  addi a0, a0, 4
  blt a0, a1, 1b
2:
  /* Copy initialized data from flash to RAM. */
  la a0, _data_lma
  la a1, _data_vma
  la a2, _edata
1:
  beq a1, a2, 2f
  lw a3, 0(a0)
  sw a3, 0(a1)
  addi a0, a0, 4
  addi a1, a1, 4
  bne a1, a2, 1b
2:
  call main
  /* main never returns; a return parks beside the traps. */
  j trap_park
trap_park:
  j trap_park
  .size Reset_Handler, .-Reset_Handler
