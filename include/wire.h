/* wire.h — Public API
 *
 * Minimal GDB RSP stub for baremetal targets.
 * One UART wire is all you need.
 *
 * USAGE
 * -----
 * 1. Implement the two UART hooks (write + read).
 * 2. Call wire_init() early in main() or SystemInit().
 * 3. On crash, wire installs its own exception handlers automatically.
 *    The CPU halts in an RSP wait loop until a GDB session connects.
 *
 * HOST SIDE
 * ---------
 *   wire-host --port /dev/ttyUSB0 --baud 115200
 *   arm-none-eabi-gdb firmware.elf
 *   (gdb) target remote :3333
 *
 * RAM BUDGET (Cortex-M, Phase 1)
 * --------------------------------
 *   RSP packet buffer  ~512 B
 *   Register snapshot  ~68 B
 *   Total              ~600 B
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WIRE_H
#define WIRE_H

#include <stddef.h>
#include <stdint.h>
#include "wire_regs.h"

/* ── User-provided UART hooks (you must implement these) ─────────────────── */

/* Write len bytes to UART. Blocking or DMA — either is fine. */
void wire_uart_write(const uint8_t *buf, size_t len);

/* Read one byte from UART. MUST block until a byte is available.
 * Return 0 on success, negative on error. */
int  wire_uart_read(uint8_t *byte);

/* ── Initialisation ──────────────────────────────────────────────────────── */

/* Install fault exception handlers and prepare RSP state.
 * Call as early as possible (before any user tasks start).
 *
 * ram_start / ram_end  — bounds for memory r/w via GDB 'm'/'M' commands.
 *   GDB will be denied access outside [ram_start, ram_end).
 *   Pass 0/0 to allow any address (not recommended in production).
 */
void wire_init(uint32_t ram_start, uint32_t ram_end);

/* ── Advanced ────────────────────────────────────────────────────────────── */

/* Enter the RSP debug loop with an explicit register snapshot.
 * Normally called automatically by the fault handler.
 * You can also call this manually (e.g. on a breakpoint button press).
 *
 * halt_signal — GDB signal number to report (5 = SIGTRAP, 11 = SIGSEGV …).
 */
void wire_debug_loop(const wire_regs_t *regs, int halt_signal);

/* ── Live debug (Cortex-M3/M4/M7/M33, requires WIRE_LIVE_DEBUG) ─────────── */
#ifdef WIRE_ARCH_CORTEX_M

/* Enable FPB and the DebugMonitor exception so that Z1 hardware breakpoints
 * and 's' single-step can fire.  Called automatically from wire_init() when
 * WIRE_LIVE_DEBUG is defined; call manually if you opt out of WIRE_LIVE_DEBUG
 * but still want live debug capability.
 *
 * Supports FPBv1 (Cortex-M3/M4) and FPBv2 (Cortex-M7/M33/M35P) — revision
 * is detected at runtime from FPB_CTRL.REV [31:28].
 *
 * Note: firmware CMakeLists must add
 *   target_compile_definitions(my_fw PRIVATE WIRE_LIVE_DEBUG=1)
 * to enable automatic setup.  DebugMonitor priority must not be masked by
 * BASEPRI/PRIMASK in the application — set it to the lowest priority.
 */
void wire_enable_debug_monitor(void);

/* ── Break-in: halt a running MCU from the host ──────────────────────────── */

/* Non-blocking UART read hook for break-in detection.
 * Return 1 and fill *byte if a byte is immediately available; return 0 if
 * the UART RX buffer is empty.  Default (weak) implementation always returns 0.
 *
 * Override in your BSP to enable wire_poll_break_in():
 *   int wire_uart_try_read(uint8_t *byte) {
 *       if (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_RXNE)) {
 *           *byte = (uint8_t)huart2.Instance->RDR;
 *           return 1;
 *       }
 *       return 0;
 *   }
 */
int wire_uart_try_read(uint8_t *byte);

/* Poll for a break-in request (Ctrl-C / 0x03) from the host.
 * When 0x03 is detected via wire_uart_try_read(), pends DebugMonitor so the
 * CPU halts at the next opportunity.
 *
 * Call from your main loop or RTOS idle task:
 *   for (;;) {
 *       wire_poll_break_in();
 *       do_work();
 *   }
 *
 * Requires wire_uart_try_read() to be overridden (default does nothing).
 * No ISR is installed — break-in is purely poll-based, avoiding conflicts
 * with your existing UART interrupt driver.
 */
void wire_poll_break_in(void);

#endif /* WIRE_ARCH_CORTEX_M */

#endif /* WIRE_H */
