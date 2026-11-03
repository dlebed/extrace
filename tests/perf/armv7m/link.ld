/* SPDX-License-Identifier: MIT */
/* link.ld - unicorn RAM at 0x20000000; the runner calls main() */
SECTIONS
{
    . = 0x20000000;
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
    /DISCARD/ : { *(.ARM.exidx*) *(.comment) }
}
