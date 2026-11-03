/* SPDX-License-Identifier: MIT */
/* qemu_armv7m.ld - QEMU mps2-an386 images (run_emu.py --emulator qemu): the vector
 * table at 0, everything else in SSRAM2 at 0x20000000; the site table stays in the ELF. */
SECTIONS
{
    . = 0x0;
    .vectors : { KEEP(*(.vectors)) }
    . = 0x20000000;
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
    . = ALIGN(8);
    . += 0x4000;
    __stack_top = .;
    /DISCARD/ : { *(.ARM.exidx*) *(.comment) }
}
INCLUDE extrace_ftrace.ld
