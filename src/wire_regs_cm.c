/* wire_regs_cm.c — Cortex-M register capture and serialisation
 *
 * Cortex-M exception frame (pushed automatically by hardware, grows downward):
 *
 *   sp+0x1c  xpsr
 *   sp+0x18  pc   (return address)
 *   sp+0x14  lr   (r14)
 *   sp+0x10  r12
 *   sp+0x0c  r3
 *   sp+0x08  r2
 *   sp+0x04  r1
 *   sp+0x00  r0   ← frame pointer
 *
 * Callee-saved registers r4-r11 are NOT automatically stacked by hardware.
 * wire_exception.c saves them manually before calling this function.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../include/wire_regs.h"

#ifdef WIRE_ARCH_CORTEX_M

#include <string.h>

/* ── Internal helpers ────────────────────────────────────────────────────── */

static void u32_to_hex_le(uint32_t val, char *out)
{
    /* GDB expects little-endian hex: least significant byte first */
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 4; i++) {
        uint8_t b = (uint8_t)(val >> (i * 8));
        *out++ = hex[b >> 4];
        *out++ = hex[b & 0xf];
    }
}

static uint32_t hex_le_to_u32(const char *in)
{
    uint32_t val = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t hi = (uint8_t)in[i * 2];
        uint8_t lo = (uint8_t)in[i * 2 + 1];
        hi = (uint8_t)(hi >= 'a' ? hi - 'a' + 10 : hi >= 'A' ? hi - 'A' + 10 : hi - '0');
        lo = (uint8_t)(lo >= 'a' ? lo - 'a' + 10 : lo >= 'A' ? lo - 'A' + 10 : lo - '0');
        val |= (uint32_t)((hi << 4) | lo) << (i * 8);
    }
    return val;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void wire_regs_capture_cm(const uint32_t *frame, const uint32_t *saved,
                           wire_regs_t *out)
{
    /* Hardware-stacked: r0-r3, r12, lr, pc, xpsr */
    out->r[0]  = frame[0];
    out->r[1]  = frame[1];
    out->r[2]  = frame[2];
    out->r[3]  = frame[3];
    out->r[12] = frame[4];
    out->r[14] = frame[5];  /* lr */
    out->r[15] = frame[6];  /* pc */
    out->xpsr  = frame[7];

    /* Manually stacked: r4-r11 */
    for (int i = 0; i < 8; i++)
        out->r[4 + i] = saved[i];

    /* sp points just above the exception frame */
    out->r[13] = (uint32_t)(uintptr_t)(frame + 8);
}

void wire_regs_to_hex(const wire_regs_t *regs, char *hex)
{
    /* GDB 'g' response: r0-r15, xpsr (17 × 8 hex chars = 136 chars) */
    for (int i = 0; i < 16; i++)
        u32_to_hex_le(regs->r[i], hex + i * 8);
    u32_to_hex_le(regs->xpsr, hex + 16 * 8);
}

void wire_regs_from_hex(const char *hex, wire_regs_t *regs)
{
    for (int i = 0; i < 16; i++)
        regs->r[i] = hex_le_to_u32(hex + i * 8);
    regs->xpsr = hex_le_to_u32(hex + 16 * 8);
}

#endif /* WIRE_ARCH_CORTEX_M */
