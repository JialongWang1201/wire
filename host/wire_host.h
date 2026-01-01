/* wire_host.h — Shared declarations for wire-host
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WIRE_HOST_H
#define WIRE_HOST_H

#include <stddef.h>
#include <stdint.h>

/* ── Serial port ──────────────────────────────────────────────────────────── */

int wire_serial_open(const char *port, int baud);

/* ── RSP error codes ─────────────────────────────────────────────────────── */

#define WIRE_OK            0
#define WIRE_ERR_IO       (-1)
#define WIRE_ERR_TIMEOUT  (-2)
#define WIRE_ERR_CHECKSUM (-3)
#define WIRE_ERR_OVERFLOW (-4)
#define WIRE_ERR_PARSE    (-5)

/* Maximum stack frames captured by heuristic backtrace */
#ifndef WIRE_MAX_FRAMES
#define WIRE_MAX_FRAMES 8
#endif

/* ── RSP client (wire_rsp_client.c) ─────────────────────────────────────── */

/*
 * Send RSP command cmd and receive the server's response into resp_buf.
 * Handles NAK/retransmit on both command and response sides (max 3 retries).
 * Returns WIRE_OK on success.
 */
int rsp_transaction(int fd, const char *cmd,
                    char *resp_buf, size_t resp_size);

/* ── Crash dump (wire_crash.c) ───────────────────────────────────────────── */

/*
 * Execute crash readout sequence on uart_fd:
 *   ?  →  halt signal
 *   g  →  all Cortex-M registers
 *   m <SP>,256  →  stack bytes (heuristic backtrace)
 *   m e000ed28,4  →  CFSR
 * Prints JSON to stdout.  Returns 0 on success, 1 on timeout/error.
 */
int wire_dump_crash(int uart_fd);

/*
 * Same as wire_dump_crash() but captures JSON into buf instead of stdout.
 * buf is NUL-terminated.  Returns 0 on success, 1 on timeout/error, -1 on
 * internal error (e.g. open_memstream failure).
 */
int wire_dump_crash_to_buf(int uart_fd, char *buf, size_t buf_size);

#endif /* WIRE_HOST_H */
