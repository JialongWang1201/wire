/* wire_regs.h — Register snapshot types (per-architecture)
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WIRE_REGS_H
#define WIRE_REGS_H

#include <stdint.h>
#include "wire_arch.h"

/* ── Cortex-M (ARMv7-M / ARMv7E-M) ─────────────────────────────────────── */
#ifdef WIRE_ARCH_CORTEX_M

typedef struct {
    uint32_t r[16];  /* r0-r15 (r13=sp, r14=lr, r15=pc) */
    uint32_t xpsr;
} wire_regs_t;

/* Index helpers */
#define WIRE_REG_SP    13
#define WIRE_REG_LR    14
#define WIRE_REG_PC    15

/* Populate from Cortex-M exception stack frame.
 *
 * frame  — pointer to the stacked frame (pushed automatically by hardware):
 *          [r0, r1, r2, r3, r12, lr, pc, xpsr]
 * saved  — pointer to manually saved r4-r11 (pushed in assembly shim)
 */
void wire_regs_capture_cm(const uint32_t *frame, const uint32_t *saved,
                           wire_regs_t *out);

/* Serialise regs into GDB RSP hex string (WIRE_REG_HEX_CHARS chars, no NUL). */
void wire_regs_to_hex(const wire_regs_t *regs, char *hex);

/* Deserialise from GDB RSP hex string into regs. */
void wire_regs_from_hex(const char *hex, wire_regs_t *regs);

#endif /* WIRE_ARCH_CORTEX_M */

/* ── Stubs for future architectures — add wire_regs_capture_<arch>() ──────
 * Phase 2: AARCH64, X86_64
 * Phase 3: RISCV
 */

#endif /* WIRE_REGS_H */
