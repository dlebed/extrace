/* SPDX-License-Identifier: MIT */
/* crt0.S - qemu virt: stack, main(), then power off through the sifive test device. */
    .section .text.start, "ax"
    .globl _start
_start:
    la      sp, __stack_top
    call    main
    li      t0, 0x100000
    li      t1, 0x5555
    sw      t1, 0(t0)
1:  j       1b
