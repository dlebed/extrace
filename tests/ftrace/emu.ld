/* SPDX-License-Identifier: MIT */
/* emu.ld - emulator images (run_emu.py): one RAM region; the site table stays in the ELF. */
SECTIONS
{
    . = 0x20000000;
    .text : { *(.text*) }
    .rodata : { *(.rodata*) *(.srodata*) }
    .data : { *(.data*) *(.sdata*) }
    .bss : { *(.bss*) *(.sbss*) *(COMMON) }
}
INCLUDE extrace_ftrace.ld
