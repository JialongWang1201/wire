/* wire_exception.c — Fault handler installation and entry points (Cortex-M)
 *
 * The assembly shims below are the actual exception vectors. They:
 *   1. Determine which stack pointer was active (MSP or PSP).
 *   2. Push r4-r11 (callee-saved) onto the active stack.
 *   3. Pass (frame_ptr, saved_ptr) to the C handler.
 *
 * After wire_regs_capture_cm() builds the snapshot, control transfers to
 * wire_debug_loop() which never returns (halts in RSP poll loop).
 *
 * SPDX-License-Identifier: MIT
 */

#include "../include/wire.h"
#include "../include/wire_regs.h"

#ifdef WIRE_ARCH_CORTEX_M

#include <stdint.h>

/* ── Weak symbols — user can override individual handlers ─────────────────── */

/* Called from assembly shim with the stacked frame and the manually
 * saved r4-r11.  Signal 11 = SIGSEGV, suitable for HardFault. */
static void wire_fault_entry(uint32_t *frame, uint32_t *saved, int signal)
{
    wire_regs_t regs;
    wire_regs_capture_cm(frame, saved, &regs);
    wire_debug_loop(&regs, signal);
    /* wire_debug_loop never returns in Phase 1 */
    for (;;) {}
}

/* ── Assembly shims ──────────────────────────────────────────────────────── */

/* Macro: emit an exception handler shim for a given C entry function.
 *
 * The shim:
 *   - Selects MSP or PSP based on EXC_RETURN[2].
 *   - Pushes r4-r11 onto that stack.
 *   - Passes frame ptr (r0) and saved-regs ptr (r1) to wire_fault_entry.
 *   - Passes the signal number (r2).
 */
#define WIRE_FAULT_SHIM(name, signal)                                    \
    __attribute__((naked)) void name(void)                               \
    {                                                                    \
        __asm volatile (                                                  \
            "tst    lr, #4          \n" /* EXC_RETURN[2]: 0=MSP 1=PSP */ \
            "ite    eq              \n"                                   \
            "mrseq  r0, msp         \n"                                   \
            "mrsne  r0, psp         \n"                                   \
            "push   {r4-r11}        \n" /* save callee-saved on stack   */ \
            "mov    r1, sp          \n" /* r1 = pointer to r4-r11       */ \
            "mov    r2, %0          \n" /* r2 = GDB signal number        */ \
            "bl     wire_fault_entry\n"                                   \
            "b      .               \n" /* should never reach here       */ \
            :                                                             \
            : "i"(signal)                                                 \
            : "r0", "r1", "r2"                                           \
        );                                                               \
    }

/* Install override for each Cortex-M fault vector.
 * These are weak aliases — if the application already defines them,
 * it can call wire_debug_loop() manually from its own handler. */
WIRE_FAULT_SHIM(HardFault_Handler,    11)  /* SIGSEGV */
WIRE_FAULT_SHIM(MemManage_Handler,    11)
WIRE_FAULT_SHIM(BusFault_Handler,     11)
WIRE_FAULT_SHIM(UsageFault_Handler,    4)  /* SIGILL */

#endif /* WIRE_ARCH_CORTEX_M */
