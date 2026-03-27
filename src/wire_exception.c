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
 * saved r4-r11.  Signal 11 = SIGSEGV, suitable for HardFault.
 * Not static: naked inline asm cannot take a proper relocation to a
 * static (internal-linkage) symbol; hidden visibility keeps it local. */
__attribute__((visibility("hidden")))
void wire_fault_entry(uint32_t *frame, uint32_t *saved, int signal)
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
    __attribute__((naked, weak)) void name(void)                         \
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

/* Weak aliases: if the application defines stronger symbols (e.g. in
 * interrupts.c), the linker selects those.  The app can then call
 * wire_debug_loop() from its own handler if desired. */
WIRE_FAULT_SHIM(HardFault_Handler,    11)  /* SIGSEGV */
WIRE_FAULT_SHIM(MemManage_Handler,    11)
WIRE_FAULT_SHIM(BusFault_Handler,     11)
WIRE_FAULT_SHIM(UsageFault_Handler,    4)  /* SIGILL */

/* ── DebugMonitor: resumable live-debug entry ────────────────────────────── */

/* DEMCR.MON_STEP (bit 18) — cleared on DebugMonitor entry so a single
 * 's' command steps exactly one instruction, not indefinitely. */
#define _DEMCR_REG      (*(volatile uint32_t *)0xE000EDFCu)
#define _DEMCR_MON_STEP (1u << 18)

/* Like wire_fault_entry() but RETURNS after wire_debug_loop() exits.
 * DebugMonitor is the only Cortex-M exception that can return normally
 * (the MCU resumes execution when the handler returns).
 * Called from WIRE_DEBUG_MON_SHIM — NOT from WIRE_FAULT_SHIM. */
__attribute__((visibility("hidden")))
void wire_debug_entry(uint32_t *frame, uint32_t *saved, int signal)
{
    _DEMCR_REG &= ~_DEMCR_MON_STEP;   /* clear step flag before blocking */
    wire_regs_t regs;
    wire_regs_capture_cm(frame, saved, &regs);
    wire_debug_loop(&regs, signal);
    /* Returns here; WIRE_DEBUG_MON_SHIM then pops r4-r11+EXC_RETURN → bx pc */
}

/* Resumable shim for DebugMonitor_Handler.
 *
 * Unlike WIRE_FAULT_SHIM (which ends with "b ." — never returns), this shim
 * uses "pop {r4-r11, pc}" to restore callee-saved registers and trigger the
 * Cortex-M exception return mechanism via the saved EXC_RETURN value.
 *
 * Stack layout after "push {r4-r11, lr}":
 *   sp+0  : r4   ← r1 (saved ptr passed to wire_debug_entry)
 *   sp+4  : r5
 *   ...
 *   sp+28 : r11
 *   sp+32 : lr   (EXC_RETURN — loaded into pc by pop to trigger return)
 */
#define WIRE_DEBUG_MON_SHIM(name, signal)                                   \
    __attribute__((naked, weak)) void name(void)                             \
    {                                                                        \
        __asm volatile (                                                      \
            "tst    lr, #4              \n" /* EXC_RETURN[2]: 0=MSP 1=PSP */ \
            "ite    eq                  \n"                                   \
            "mrseq  r0, msp             \n"                                   \
            "mrsne  r0, psp             \n"                                   \
            "push   {r4-r11, lr}        \n" /* save callee-saved + EXC_RETURN */ \
            "mov    r1, sp              \n" /* r1 = pointer to r4-r11        */ \
            "mov    r2, %0              \n" /* r2 = GDB signal number         */ \
            "bl     wire_debug_entry    \n"                                   \
            "pop    {r4-r11, pc}        \n" /* restore + exception return     */ \
            :                                                                 \
            : "i"(signal)                                                     \
            : "r0", "r1", "r2"                                               \
        );                                                                   \
    }

/* DebugMonitor fires on:
 *   - FPBv1 hardware breakpoint match (Z1 comparator)
 *   - DEMCR.MON_STEP single-step completion
 * Signal 5 = SIGTRAP (same as GDB breakpoint / step halt). */
WIRE_DEBUG_MON_SHIM(DebugMonitor_Handler, 5)

#endif /* WIRE_ARCH_CORTEX_M */
