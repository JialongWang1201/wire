/* wire_rsp.c — GDB Remote Serial Protocol (RSP) implementation
 *
 * Implements the minimal command set required for Phase 1:
 *
 *   ?              halt reason (SIGTRAP / SIGSEGV …)
 *   g              read all registers
 *   G XX…          write all registers
 *   m addr,len     read memory
 *   M addr,len:XX… write memory
 *   c              continue (resume execution)
 *   qSupported     feature negotiation
 *   qXfer:features:read:target.xml   target description XML
 *   vMustReplyEmpty  (and all other unknown packets → empty reply)
 *
 * Phase 1 explicitly DOES NOT support: breakpoints, single-step,
 * watchpoints, or multi-threading.
 *
 * Protocol reference: https://sourceware.org/gdb/current/onlinedocs/gdb/Remote-Protocol.html
 *
 * SPDX-License-Identifier: MIT
 */

#include "../include/wire.h"
#include "../include/wire_regs.h"
#include "../include/wire_arch.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ── Configuration ───────────────────────────────────────────────────────── */

#ifndef WIRE_PKT_BUF
#  define WIRE_PKT_BUF 512  /* bytes — covers Cortex-M 'g' response (136 chars) */
#endif

/* Stringify helper — must appear before first use in rsp_dispatch(). */
#define WIRE_STR(x)      #x
#define WIRE_XSTR(x)     WIRE_STR(x)
#define WIRE_PKT_BUF_STR WIRE_XSTR(WIRE_PKT_BUF)

/* ── Target description XML (Cortex-M, arm-m-profile) ──────────────────── */

#ifdef WIRE_ARCH_CORTEX_M
static const char s_target_xml[] =
    "<?xml version=\"1.0\"?>"
    "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
    "<target version=\"1.0\">"
      "<architecture>arm</architecture>"
      "<feature name=\"org.gnu.gdb.arm.m-profile\">"
        "<reg name=\"r0\"   bitsize=\"32\" type=\"uint32\"/>"
        "<reg name=\"r1\"   bitsize=\"32\" type=\"uint32\"/>"
        "<reg name=\"r2\"   bitsize=\"32\" type=\"uint32\"/>"
        "<reg name=\"r3\"   bitsize=\"32\" type=\"uint32\"/>"
        "<reg name=\"r4\"   bitsize=\"32\" type=\"uint32\"/>"
        "<reg name=\"r5\"   bitsize=\"32\" type=\"uint32\"/>"
        "<reg name=\"r6\"   bitsize=\"32\" type=\"uint32\"/>"
        "<reg name=\"r7\"   bitsize=\"32\" type=\"uint32\"/>"
        "<reg name=\"r8\"   bitsize=\"32\" type=\"uint32\"/>"
        "<reg name=\"r9\"   bitsize=\"32\" type=\"uint32\"/>"
        "<reg name=\"r10\"  bitsize=\"32\" type=\"uint32\"/>"
        "<reg name=\"r11\"  bitsize=\"32\" type=\"uint32\"/>"
        "<reg name=\"r12\"  bitsize=\"32\" type=\"uint32\"/>"
        "<reg name=\"sp\"   bitsize=\"32\" type=\"data_ptr\"/>"
        "<reg name=\"lr\"   bitsize=\"32\"/>"
        "<reg name=\"pc\"   bitsize=\"32\" type=\"code_ptr\"/>"
        "<reg name=\"xpsr\" bitsize=\"32\"/>"
      "</feature>"
    "</target>";
#endif

/* ── Module state ────────────────────────────────────────────────────────── */

static wire_regs_t  s_regs;
static int          s_signal;
static uint32_t     s_ram_start;
static uint32_t     s_ram_end;

/* ── Hex utilities ───────────────────────────────────────────────────────── */

static const char s_hex[] = "0123456789abcdef";

static uint8_t hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
    return 0;
}

static uint8_t hex_byte(const char *s)
{
    return (uint8_t)((hex_nibble(s[0]) << 4) | hex_nibble(s[1]));
}

/* ── RSP packet I/O ──────────────────────────────────────────────────────── */

/* Read one byte from UART (calls user hook). */
static uint8_t rsp_getc(void)
{
    uint8_t b;
    while (wire_uart_read(&b) != 0) {}
    return b;
}

/* Send raw bytes to UART. */
static void rsp_putbuf(const uint8_t *buf, size_t len)
{
    wire_uart_write(buf, len);
}

/* Receive one RSP packet into buf[0..WIRE_PKT_BUF-1].
 * Returns the data length (without '$', '#', checksum).
 * Sends '+' ACK on good checksum, '-' NACK and retries on bad. */
static size_t rsp_recv(char *buf)
{
    for (;;) {
        /* Skip until '$' */
        uint8_t c;
        do { c = rsp_getc(); } while (c != '$');

        /* Accumulate packet data until '#' */
        size_t   len = 0;
        uint8_t  sum = 0;
        while (1) {
            c = rsp_getc();
            if (c == '#') break;
            if (len < WIRE_PKT_BUF - 1) {
                buf[len++] = (char)c;
                sum += c;
            }
        }
        buf[len] = '\0';

        /* Read 2-char checksum */
        char cs[2];
        cs[0] = (char)rsp_getc();
        cs[1] = (char)rsp_getc();
        uint8_t expected = (uint8_t)((hex_nibble(cs[0]) << 4) | hex_nibble(cs[1]));

        if (sum == expected) {
            uint8_t ack = '+';
            rsp_putbuf(&ack, 1);
            return len;
        } else {
            uint8_t nak = '-';
            rsp_putbuf(&nak, 1);
            /* loop: receive again */
        }
    }
}

/* Send one RSP packet: $data#checksum */
static void rsp_send(const char *data, size_t len)
{
    uint8_t  buf[WIRE_PKT_BUF + 4];
    uint8_t  sum = 0;

    buf[0] = '$';
    size_t i;
    for (i = 0; i < len && i < WIRE_PKT_BUF; i++) {
        buf[1 + i] = (uint8_t)data[i];
        sum += (uint8_t)data[i];
    }
    buf[1 + i]     = '#';
    buf[2 + i]     = (uint8_t)s_hex[sum >> 4];
    buf[3 + i]     = (uint8_t)s_hex[sum & 0xf];

    /* Retry until ACK '+' */
    for (;;) {
        rsp_putbuf(buf, 4 + i);
        uint8_t ack = rsp_getc();
        if (ack == '+') break;
        /* '-' or garbage: retransmit */
    }
}

static void rsp_send_str(const char *s)
{
    rsp_send(s, strlen(s));
}

static void rsp_send_empty(void)
{
    rsp_send("", 0);
}

static void rsp_send_ok(void)
{
    rsp_send_str("OK");
}

static void rsp_send_error(int code)
{
    char e[4] = { 'E', s_hex[(code >> 4) & 0xf], s_hex[code & 0xf], '\0' };
    rsp_send_str(e);
}

/* ── Memory access helpers ───────────────────────────────────────────────── */

/* Parse hex address from packet, advance *p past it. */
static uint32_t parse_hex_u32(const char **p)
{
    uint32_t v = 0;
    while (**p && **p != ',' && **p != ':' && **p != '#') {
        v = (v << 4) | hex_nibble(*(*p)++);
    }
    return v;
}

static int mem_read_safe(uint32_t addr, size_t len, char *out)
{
    /* Bounds check: only allow reads within [ram_start, ram_end) */
    if (s_ram_start != 0 || s_ram_end != 0) {
        if (addr < s_ram_start || (addr + len) > s_ram_end)
            return -1;
    }
    const uint8_t *src = (const uint8_t *)(uintptr_t)addr;
    for (size_t i = 0; i < len; i++) {
        *out++ = s_hex[src[i] >> 4];
        *out++ = s_hex[src[i] & 0xf];
    }
    return 0;
}

static int mem_write_safe(uint32_t addr, size_t len, const char *hex)
{
    if (s_ram_start != 0 || s_ram_end != 0) {
        if (addr < s_ram_start || (addr + len) > s_ram_end)
            return -1;
    }
    uint8_t *dst = (uint8_t *)(uintptr_t)addr;
    for (size_t i = 0; i < len; i++)
        dst[i] = hex_byte(hex + i * 2);
    return 0;
}

/* ── qXfer: target.xml ───────────────────────────────────────────────────── */

static void handle_qxfer_features(const char *annex, uint32_t offset, uint32_t length)
{
#ifdef WIRE_ARCH_CORTEX_M
    if (strcmp(annex, "target.xml") != 0) { rsp_send_empty(); return; }
    size_t total = sizeof(s_target_xml) - 1;
    if (offset >= total) { rsp_send_str("l"); return; }
    size_t avail = total - offset;
    if (avail > length) avail = length;

    char buf[WIRE_PKT_BUF];
    buf[0] = (avail + offset < total) ? 'm' : 'l';   /* 'm'=more, 'l'=last */
    memcpy(buf + 1, s_target_xml + offset, avail);
    rsp_send(buf, 1 + avail);
#else
    (void)annex; (void)offset; (void)length;
    rsp_send_empty();
#endif
}

/* ── Command dispatcher ──────────────────────────────────────────────────── */

/* Returns 1 if the debug loop should exit (resume execution). */
static int rsp_dispatch(const char *pkt, size_t len)
{
    (void)len;

    switch (pkt[0]) {

    /* ── ? — halt reason ─────────────────────────────────────────────────── */
    case '?': {
        char reply[8];
        reply[0] = 'S';
        reply[1] = s_hex[(s_signal >> 4) & 0xf];
        reply[2] = s_hex[s_signal & 0xf];
        reply[3] = '\0';
        rsp_send_str(reply);
        break;
    }

    /* ── g — read all registers ──────────────────────────────────────────── */
    case 'g': {
        char hex[WIRE_REG_HEX_CHARS + 1];
        wire_regs_to_hex(&s_regs, hex);
        hex[WIRE_REG_HEX_CHARS] = '\0';
        rsp_send(hex, WIRE_REG_HEX_CHARS);
        break;
    }

    /* ── G — write all registers ─────────────────────────────────────────── */
    case 'G':
        wire_regs_from_hex(pkt + 1, &s_regs);
        rsp_send_ok();
        break;

    /* ── m addr,length — read memory ─────────────────────────────────────── */
    case 'm': {
        const char *p = pkt + 1;
        uint32_t addr = parse_hex_u32(&p);
        if (*p == ',') p++;
        uint32_t rlen = parse_hex_u32(&p);
        if (rlen > (WIRE_PKT_BUF / 2)) rlen = WIRE_PKT_BUF / 2;

        char out[WIRE_PKT_BUF];
        if (mem_read_safe(addr, rlen, out) != 0)
            rsp_send_error(0x0e);  /* EFAULT */
        else
            rsp_send(out, rlen * 2);
        break;
    }

    /* ── M addr,length:data — write memory ───────────────────────────────── */
    case 'M': {
        const char *p = pkt + 1;
        uint32_t addr = parse_hex_u32(&p);
        if (*p == ',') p++;
        uint32_t wlen = parse_hex_u32(&p);
        if (*p == ':') p++;
        if (mem_write_safe(addr, wlen, p) != 0)
            rsp_send_error(0x0e);
        else
            rsp_send_ok();
        break;
    }

    /* ── c — continue ────────────────────────────────────────────────────── */
    case 'c':
        /* Phase 1: just acknowledge and exit the debug loop.
         * The caller is responsible for restoring registers and resuming. */
        rsp_send_str("S00");   /* signal 0 = no signal, just continuing */
        return 1;              /* exit debug loop */

    /* ── q — queries ─────────────────────────────────────────────────────── */
    case 'q':
        if (strncmp(pkt, "qSupported", 10) == 0) {
            rsp_send_str("PacketSize=" WIRE_PKT_BUF_STR
                         ";qXfer:features:read+");
        } else if (strncmp(pkt, "qXfer:features:read:", 20) == 0) {
            /* qXfer:features:read:annex:offset,length */
            const char *annex = pkt + 20;
            const char *colon = strchr(annex, ':');
            if (!colon) { rsp_send_empty(); break; }
            char annex_buf[64];
            size_t al = (size_t)(colon - annex);
            if (al >= sizeof(annex_buf)) al = sizeof(annex_buf) - 1;
            memcpy(annex_buf, annex, al);
            annex_buf[al] = '\0';
            const char *p = colon + 1;
            uint32_t off = parse_hex_u32(&p);
            if (*p == ',') p++;
            uint32_t mlen = parse_hex_u32(&p);
            handle_qxfer_features(annex_buf, off, mlen);
        } else {
            rsp_send_empty();
        }
        break;

    /* ── v — v-packets ───────────────────────────────────────────────────── */
    case 'v':
        /* vMustReplyEmpty and all others: empty reply */
        rsp_send_empty();
        break;

    /* ── Everything else → empty reply (GDB-compatible) ─────────────────── */
    default:
        rsp_send_empty();
        break;
    }

    return 0;
}

/* ── Public entry point ──────────────────────────────────────────────────── */

void wire_debug_loop(const wire_regs_t *regs, int signal)
{
    s_regs   = *regs;
    s_signal = signal;

    char pkt[WIRE_PKT_BUF];

    for (;;) {
        size_t len = rsp_recv(pkt);
        if (rsp_dispatch(pkt, len))
            break;
    }
}

void wire_init(uint32_t ram_start, uint32_t ram_end)
{
    s_ram_start = ram_start;
    s_ram_end   = ram_end;
    /* Exception handlers are installed via weak symbol overrides in
     * wire_exception.c — no dynamic registration needed. */
}
