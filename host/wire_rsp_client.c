/* wire_rsp_client.c — Minimal RSP client for wire-host --dump mode
 *
 * Implements GDB Remote Serial Protocol packet framing:
 *   Send:    $<data>#<2-hex-checksum>
 *   Recv:    server sends + (ACK) or - (NAK, retransmit)
 *   Resp:    server sends $<data>#<2-hex-checksum>
 *   Ack:     client sends + (ACK) or - (NAK, server retransmits)
 *
 * rsp_transaction() handles the full command → ack → response → ack cycle
 * with NAK/retransmit up to RSP_MAX_RETRIES times.
 *
 * SPDX-License-Identifier: MIT
 */

#include "wire_host.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#define RSP_BUF_MAX    4096
#define RSP_MAX_RETRIES   3
#define RSP_TIMEOUT_MS 2000

/* ── helpers ─────────────────────────────────────────────────────────────── */

static uint8_t rsp_checksum(const char *data, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++)
        sum = (uint8_t)(sum + (uint8_t)data[i]);
    return sum;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Read one byte with timeout_ms; returns 1 on success, 0 on timeout, -1 on error. */
static int read_byte(int fd, uint8_t *out, int timeout_ms)
{
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);

    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int r = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (r < 0) {
        if (errno == EINTR) return read_byte(fd, out, timeout_ms);
        return -1;
    }
    if (r == 0) return 0;  /* timeout */

    ssize_t n = read(fd, out, 1);
    if (n < 0) return (errno == EAGAIN || errno == EINTR) ? 0 : -1;
    if (n == 0) return 0;
    return 1;
}

/* ── send ────────────────────────────────────────────────────────────────── */

int rsp_send_packet(int fd, const char *data)
{
    size_t  dlen = strlen(data);
    uint8_t sum  = rsp_checksum(data, dlen);
    char    buf[RSP_BUF_MAX + 8];
    int     n    = snprintf(buf, sizeof(buf), "$%s#%02x", data, sum);
    if (n < 0 || (size_t)n >= sizeof(buf)) return WIRE_ERR_OVERFLOW;

    ssize_t w = write(fd, buf, (size_t)n);
    if (w != (ssize_t)n) return WIRE_ERR_IO;
    return WIRE_OK;
}

/* ── receive ─────────────────────────────────────────────────────────────── */

/*
 * Receive one RSP response packet:
 *   - drain until '$'
 *   - read data bytes until '#'
 *   - read 2-hex checksum
 *   - verify; send '+' on OK or '-' on mismatch
 * Returns WIRE_OK or WIRE_ERR_*.
 */
static int rsp_recv_packet(int fd, char *out_buf, size_t out_size)
{
    uint8_t c;

    /* drain until '$' */
    for (;;) {
        int r = read_byte(fd, &c, RSP_TIMEOUT_MS);
        if (r < 0) return WIRE_ERR_IO;
        if (r == 0) return WIRE_ERR_TIMEOUT;
        if (c == '$') break;
        /* discard noise / ACK bytes from previous exchanges */
    }

    /* accumulate data until '#' */
    size_t  dlen        = 0;
    uint8_t running_sum = 0;
    for (;;) {
        int r = read_byte(fd, &c, RSP_TIMEOUT_MS);
        if (r < 0) return WIRE_ERR_IO;
        if (r == 0) return WIRE_ERR_TIMEOUT;
        if (c == '#') break;
        if (dlen + 1 >= out_size) {
            write(fd, "-", 1);
            return WIRE_ERR_OVERFLOW;
        }
        out_buf[dlen++] = (char)c;
        running_sum     = (uint8_t)(running_sum + c);
    }
    out_buf[dlen] = '\0';

    /* read 2-hex checksum */
    uint8_t hi, lo;
    if (read_byte(fd, &hi, RSP_TIMEOUT_MS) <= 0) return WIRE_ERR_IO;
    if (read_byte(fd, &lo, RSP_TIMEOUT_MS) <= 0) return WIRE_ERR_IO;

    int h = hex_nibble((char)hi);
    int l = hex_nibble((char)lo);
    if (h < 0 || l < 0) {
        write(fd, "-", 1);
        return WIRE_ERR_CHECKSUM;
    }

    uint8_t expected = (uint8_t)((h << 4) | l);
    if (running_sum != expected) {
        write(fd, "-", 1);  /* NAK: server will retransmit */
        return WIRE_ERR_CHECKSUM;
    }

    write(fd, "+", 1);  /* ACK */
    return WIRE_OK;
}

/* ── public API ──────────────────────────────────────────────────────────── */

/*
 * Block until the target sends a spontaneous stop reply (S or T packet).
 * Called after 'c' or 's' commands when the target does not reply immediately.
 *
 * The target sends $Sxx#cs as soon as DebugMonitor fires and wire_debug_loop
 * re-enters.  rsp_recv_packet() drains noise/ACK bytes and returns on '$'.
 *
 * Retries up to RSP_WAIT_STOP_RETRIES × RSP_TIMEOUT_MS ms total (default 60 s).
 * Returns WIRE_OK with the stop reply in out_buf, or WIRE_ERR_TIMEOUT.
 */
#define RSP_WAIT_STOP_RETRIES 30  /* 30 × 2 s = 60 s */

int rsp_wait_for_stop(int fd, char *out_buf, size_t out_size)
{
    for (int i = 0; i < RSP_WAIT_STOP_RETRIES; i++) {
        int rc = rsp_recv_packet(fd, out_buf, out_size);
        if (rc == WIRE_ERR_TIMEOUT) continue;
        if (rc != WIRE_OK) return rc;
        if (out_buf[0] == 'S' || out_buf[0] == 'T')
            return WIRE_OK;
        /* Unexpected packet (stale ACK, etc.) — keep waiting */
    }
    return WIRE_ERR_TIMEOUT;
}

/*
 * Send cmd and receive response, with NAK/retransmit on both sides.
 * Returns WIRE_OK on success.
 */
int rsp_transaction(int fd, const char *cmd,
                    char *resp_buf, size_t resp_size)
{
    for (int cmd_try = 0; cmd_try < RSP_MAX_RETRIES; cmd_try++) {
        int rc = rsp_send_packet(fd, cmd);
        if (rc != WIRE_OK) return rc;

        /* Wait for server ACK/NAK of our command */
        uint8_t ack;
        int r = read_byte(fd, &ack, RSP_TIMEOUT_MS);
        if (r < 0) return WIRE_ERR_IO;
        if (r == 0) return WIRE_ERR_TIMEOUT;
        if (ack == '-') {
            fprintf(stderr, "wire-host: RSP NAK on '%s', retrying (%d/%d)\n",
                    cmd, cmd_try + 1, RSP_MAX_RETRIES);
            continue;  /* retransmit command */
        }
        if (ack != '+') continue;  /* unexpected byte, retransmit */

        /* Server ACK'd; receive response with checksum retry */
        for (int resp_try = 0; resp_try < RSP_MAX_RETRIES; resp_try++) {
            rc = rsp_recv_packet(fd, resp_buf, resp_size);
            if (rc == WIRE_OK) return WIRE_OK;
            if (rc != WIRE_ERR_CHECKSUM) return rc;
            fprintf(stderr, "wire-host: RSP response checksum error, retry %d/%d\n",
                    resp_try + 1, RSP_MAX_RETRIES);
        }
        return WIRE_ERR_CHECKSUM;
    }
    return WIRE_ERR_CHECKSUM;
}
