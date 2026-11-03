// SPDX-License-Identifier: MIT
/*
 * qemu_main.c - QEMU harness of the ftrace images (run_emu.py --emulator qemu):
 * runs start() of emu_<arch>.c or rec.c, prints its result and the failed
 * checks of every trampoline entry, then stops the machine.
 *
 *   rv32    qemu-system-riscv32 -M virt: tests/perf/rv32/crt0.S calls main(),
 *           16550 UART at 0x10000000, exit through the SiFive test device
 *   armv7m  qemu-system-arm -M mps2-an386 (Cortex-M4F): vector table here,
 *           semihosting for output and exit
 */
#include <stdint.h>
#include "extrace_ftrace.h"

uint32_t start(void);
extern volatile uint32_t entry_fail[EXTRACE_FTRACE_ENTRIES];
int main(void);

#if defined(__riscv)
static void out(char ch)
{
    *(volatile uint8_t *)0x10000000u = (uint8_t)ch;
}
#elif defined(__arm__)
static int semi(int op, const void *arg)
{
    register int r0 __asm__("r0") = op;
    register const void *r1 __asm__("r1") = arg;

    __asm__ volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}

static void out(char ch)
{
    semi(0x03, &ch); /* SYS_WRITEC */
}

static void stop(int ok)
{
    semi(0x18, (const void *)(uintptr_t)(ok ? 0x20026u : 0x20023u)); /* SYS_EXIT */
    for (;;)
        ;
}

static void puts_(const char *s);

__attribute__((used)) static void fault(void)
{
    puts_("FAULT\n");
    stop(0);
}

void _start(void);

__attribute__((noreturn)) void _start(void)
{
    *(volatile uint32_t *)0xE000ED88u |= 0xFu << 20; /* CPACR: CP10, CP11 full access */
    __asm__ volatile("dsb\n\tisb" ::: "memory");
    stop(main() == 0);
    for (;;)
        ;
}

extern uint32_t __stack_top[];
__attribute__((section(".vectors"), used)) static const void *const vectors[16] = {
    __stack_top, _start, fault, fault, fault, fault, fault,
};
#endif

static void puts_(const char *s)
{
    while (*s)
        out(*s++);
}

static void puthex(uint32_t v)
{
    char b[11] = "0x";

    for (int i = 0; i < 8; i++)
        b[2 + i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15];
    b[10] = 0;
    puts_(b);
}

int main(void)
{
    uint32_t fail = start();

    puts_("result ");
    puthex(fail);
    puts_("\n");
    for (int k = 0; k < EXTRACE_FTRACE_ENTRIES; k++) {
        puts_("entry ");
        puthex((uint32_t)k);
        puts_(" ");
        puthex(entry_fail[k]);
        puts_("\n");
    }
    puts_("done\n");
    return fail != 0;
}
