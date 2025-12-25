/* test/qemu/startup.c — Minimal Cortex-M3 startup for QEMU mps2-an385
 *
 * Provides Reset_Handler and a minimal vector table.  The test firmware
 * lives entirely in main.c; no libc, no CMSIS required.
 *
 * Memory map (mps2-an385):
 *   FLASH  0x00000000  256 KB
 *   RAM    0x20000000  256 KB
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

extern int main(void);

/* Defined by the linker script. */
extern uint32_t _sidata;   /* load address of .data */
extern uint32_t _sdata;    /* start of .data in RAM */
extern uint32_t _edata;    /* end of .data in RAM */
extern uint32_t _sbss;     /* start of .bss */
extern uint32_t _ebss;     /* end of .bss */
extern uint32_t _estack;   /* initial stack pointer (top of RAM) */

void Reset_Handler(void)
{
    /* Copy .data from FLASH to RAM. */
    uint32_t *src = &_sidata;
    uint32_t *dst = &_sdata;
    while (dst < &_edata) {
        *dst++ = *src++;
    }

    /* Zero .bss. */
    dst = &_sbss;
    while (dst < &_ebss) {
        *dst++ = 0U;
    }

    main();

    /* Should never reach here. */
    for (;;) {}
}

/* Default handler: spin. */
static void Default_Handler(void) { for (;;) {} }

/* Weak aliases so users only need to define the ones they care about. */
void NMI_Handler(void)          __attribute__((weak, alias("Default_Handler")));
void HardFault_Handler(void)    __attribute__((weak, alias("Default_Handler")));
void MemManage_Handler(void)    __attribute__((weak, alias("Default_Handler")));
void BusFault_Handler(void)     __attribute__((weak, alias("Default_Handler")));
void UsageFault_Handler(void)   __attribute__((weak, alias("Default_Handler")));
void SVC_Handler(void)          __attribute__((weak, alias("Default_Handler")));
void DebugMon_Handler(void)     __attribute__((weak, alias("Default_Handler")));
void PendSV_Handler(void)       __attribute__((weak, alias("Default_Handler")));
void SysTick_Handler(void)      __attribute__((weak, alias("Default_Handler")));

/* Minimal vector table — 16 Cortex-M core vectors only. */
__attribute__((section(".isr_vector"), used))
static void (*const g_vectors[])(void) = {
    (void (*)(void))&_estack,  /* initial SP */
    Reset_Handler,
    NMI_Handler,
    HardFault_Handler,
    MemManage_Handler,
    BusFault_Handler,
    UsageFault_Handler,
    0, 0, 0, 0,                /* reserved */
    SVC_Handler,
    DebugMon_Handler,
    0,                         /* reserved */
    PendSV_Handler,
    SysTick_Handler,
};
