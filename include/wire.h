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

/* ── Live debug (Cortex-M3/M4, requires WIRE_LIVE_DEBUG) ────────────────── */
#ifdef WIRE_ARCH_CORTEX_M

/* Enable FPB and the DebugMonitor exception so that Z1 hardware breakpoints
 * and 's' single-step can fire.  Called automatically from wire_init() when
 * WIRE_LIVE_DEBUG is defined; call manually if you opt out of WIRE_LIVE_DEBUG
 * but still want live debug capability.
 *
 * Note: firmware CMakeLists must add
 *   target_compile_definitions(my_fw PRIVATE WIRE_LIVE_DEBUG=1)
 * to enable automatic setup.  DebugMonitor priority must not be masked by
 * BASEPRI/PRIMASK in the application — set it to the lowest priority.
 */
void wire_enable_debug_monitor(void);

#endif /* WIRE_ARCH_CORTEX_M */

#endif /* WIRE_H */
