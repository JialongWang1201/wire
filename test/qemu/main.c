/* test/qemu/main.c — Minimal wire integration test for QEMU mps2-an385
 *
 * The mps2-an385 board models a Cortex-M3 with a CMSDK APB UART at
 * 0x40004000.  This firmware:
 *   1. Initialises UART0 (TX+RX enable via CTRL register).
 *   2. Calls wire_init() to install fault handlers and register RAM bounds.
 *   3. Calls wire_debug_loop() directly — simulating a "crash" so the host
 *      test can connect GDB without needing to trigger a real fault.
 *
 * Board UART registers (CMSDK APB UART, offset from base 0x40004000):
 *   0x000  DATA  — byte TX/RX
 *   0x004  STATE — bit0 = TX full, bit1 = RX full
 *   0x008  CTRL  — bit0 = TX enable, bit1 = RX enable
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include "wire.h"

/* ── CMSDK APB UART0 (mps2-an385) ──────────────────────────────────────── */

#define UART0_BASE  0x40004000U

typedef struct {
    volatile uint32_t DATA;    /* 0x000 */
    volatile uint32_t STATE;   /* 0x004  bit0=TX_full  bit1=RX_full */
    volatile uint32_t CTRL;    /* 0x008  bit0=TX_en    bit1=RX_en  */
    volatile uint32_t INTSTATUS; /* 0x00C */
    volatile uint32_t BAUDDIV; /* 0x010 */
} CMSDK_UART_TypeDef;

#define UART0  ((CMSDK_UART_TypeDef *)UART0_BASE)

#define UART_STATE_TX_FULL  (1U << 0)
#define UART_STATE_RX_FULL  (1U << 1)
#define UART_CTRL_TX_EN     (1U << 0)
#define UART_CTRL_RX_EN     (1U << 1)

/* ── Public hooks required by wire.h ───────────────────────────────────── */

void wire_uart_write(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        while (UART0->STATE & UART_STATE_TX_FULL) {}
        UART0->DATA = buf[i];
    }
}

int wire_uart_read(uint8_t *byte)
{
    while (!(UART0->STATE & UART_STATE_RX_FULL)) {}
    *byte = (uint8_t)UART0->DATA;
    return 0;
}

/* ── Entry point ────────────────────────────────────────────────────────── */

int main(void)
{
    /* Enable UART0 TX and RX. */
    UART0->CTRL = UART_CTRL_TX_EN | UART_CTRL_RX_EN;

    /*
     * RAM bounds for mps2-an385: 256 KB SRAM starting at 0x20000000.
     * wire_init() installs the Cortex-M fault handlers so that a real
     * fault would enter the GDB debug loop automatically.  For this test
     * we call wire_debug_loop() directly to allow GDB to connect without
     * triggering an actual fault.
     */
    wire_init(0x20000000U, 0x20000000U + 256U * 1024U);

    /* Enter GDB stub — the host test connects here. */
    wire_debug_loop();

    for (;;) {}
}
