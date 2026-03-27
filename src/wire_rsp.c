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
 * Phase 2 live-debug commands (Cortex-M3/M4 only, requires WIRE_LIVE_DEBUG):
 *
 *   s              single-step via DEMCR.MON_STEP — NO immediate reply;
 *                  stop-reply S05 arrives when DebugMonitor re-enters.
 *   Z1,addr,4      set FPBv1 hardware breakpoint at addr
 *   z1,addr,4      clear FPBv1 hardware breakpoint at addr
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

/* ── Cortex-M live debug: DEMCR + FPBv1 registers ───────────────────────── */

#ifdef WIRE_ARCH_CORTEX_M

/* Debug Exception and Monitor Control Register */
#define DEMCR          (*(volatile uint32_t *)0xE000EDFCu)
#define DEMCR_MON_EN   (1u << 16)  /* enable DebugMonitor exception */
#define DEMCR_MON_STEP (1u << 18)  /* single-step on DebugMonitor return */

/* Flash Patch and Breakpoint — FPBv1 (Cortex-M3/M4 only) */
#define FPB_CTRL  (*(volatile uint32_t *)0xE0002000u)
#define FPB_COMP0 ((volatile uint32_t *)0xE0002008u)
#define FPB_CTRL_ENABLE (1u << 0)

/* FPBv1 comparator word: REPLACE=11 (match either halfword), word-aligned addr, ENABLE.
 * Address 0 is used as the "free slot" sentinel — not a valid breakpoint target. */
#define FPB_COMP_WORD(addr) ((3u << 30) | ((uint32_t)(addr) & 0x1FFFFFFCu) | 1u)

/* FPBv1 maximum comparators (hardware may have fewer — query FPB_CTRL.NUM_CODE). */
#define FPB_MAX_COMP 8u

/* Active comparator addresses; 0 means the slot is free. */
static uint32_t s_fpb_addr[FPB_MAX_COMP];

/* Data Watchpoint and Trace — DWT (Cortex-M3/M4) */
#define DWT_CTRL        (*(volatile uint32_t *)0xE0001000u)
#define DWT_COMP(n)     (*(volatile uint32_t *)(0xE0001020u + (uint32_t)(n) * 0x10u))
#define DWT_MASK(n)     (*(volatile uint32_t *)(0xE0001024u + (uint32_t)(n) * 0x10u))
#define DWT_FUNCTION(n) (*(volatile uint32_t *)(0xE0001028u + (uint32_t)(n) * 0x10u))
/* DWT_FUNCTION bits [3:0]: 4=read, 5=write, 6=read+write */
#define DWT_MAX_COMP 4u

/* Occupied DWT slots; 0 means free. Stored as addr|1 to distinguish addr 0. */
static uint32_t s_dwt_slot[DWT_MAX_COMP];

static uint8_t dwt_num_comp(void)
{
    return (uint8_t)((DWT_CTRL >> 28) & 0xFu);
}

static uint8_t fpb_num_comp(void)
{
    return (uint8_t)((FPB_CTRL >> 4) & 0xFu);
}

/* Enable FPB and DebugMonitor exception.
 * Must be called before any Z1 breakpoint can fire DebugMonitor.
 * Called automatically from wire_init() when WIRE_LIVE_DEBUG is defined. */
void wire_enable_debug_monitor(void)
{
    FPB_CTRL |= FPB_CTRL_ENABLE;
    DEMCR    |= DEMCR_MON_EN;
    /* DEMCR.MON_STEP is left clear — set transiently only during 's'. */
}

#endif /* WIRE_ARCH_CORTEX_M */

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
        /* Acknowledge and exit the debug loop.
         * The caller is responsible for restoring registers and resuming. */
        rsp_send_str("S00");   /* signal 0 = no signal, just continuing */
        return 1;              /* exit debug loop */

    /* ── s — single-step (Cortex-M DebugMonitor, WIRE_LIVE_DEBUG) ────────── */
    case 's':
#ifdef WIRE_ARCH_CORTEX_M
        /* RSP spec: 's' sends NO immediate reply.
         * Arm DEMCR.MON_STEP; the CPU executes one instruction after the ISR
         * returns, then DebugMonitor fires and wire_debug_loop re-enters
         * sending S05 (SIGTRAP) in response to the host's '?' query. */
        DEMCR |= DEMCR_MON_STEP;
        return 1;  /* exit debug loop; ISR returns; MON_STEP fires next */
#else
        rsp_send_empty();
        break;
#endif

    /* ── Z — set breakpoint / watchpoint ─────────────────────────────────── */
    case 'Z':
        if (pkt[1] == '1') {
#ifdef WIRE_ARCH_CORTEX_M
            /* Z1,addr,kind — FPBv1 hardware breakpoint (kind ignored). */
            const char *p = pkt + 2;
            if (*p == ',') p++;
            uint32_t addr = parse_hex_u32(&p);
            uint8_t  n    = fpb_num_comp();
            uint8_t  slot;
            for (slot = 0; slot < n && slot < FPB_MAX_COMP; slot++) {
                if (s_fpb_addr[slot] == 0) break;
            }
            if (slot >= n || slot >= FPB_MAX_COMP) {
                rsp_send_error(0x0e);  /* E0e: no free FPB comparator */
                break;
            }
            s_fpb_addr[slot] = addr;
            FPB_COMP0[slot]  = FPB_COMP_WORD(addr);
            rsp_send_ok();
#else
            rsp_send_empty();
#endif
        } else if (pkt[1] == '2' || pkt[1] == '3' || pkt[1] == '4') {
#ifdef WIRE_ARCH_CORTEX_M
            /* Z2=write, Z3=read, Z4=access — DWT hardware watchpoint. */
            const char *p = pkt + 2;
            if (*p == ',') p++;
            uint32_t addr = parse_hex_u32(&p);
            /* DWT_FUNCTION bits [3:0]: 5=write, 4=read, 6=read+write */
            uint32_t func = (pkt[1] == '2') ? 5u :
                            (pkt[1] == '3') ? 4u : 6u;
            uint8_t  n    = dwt_num_comp();
            uint8_t  slot;
            for (slot = 0; slot < n && slot < DWT_MAX_COMP; slot++) {
                if (s_dwt_slot[slot] == 0) break;
            }
            if (slot >= n || slot >= DWT_MAX_COMP) {
                rsp_send_error(0x0e);
                break;
            }
            s_dwt_slot[slot]   = addr | 1u;  /* bit0=occupied sentinel */
            DWT_COMP(slot)     = addr;
            DWT_MASK(slot)     = 0;           /* exact address match */
            DWT_FUNCTION(slot) = func;
            rsp_send_ok();
#else
            rsp_send_empty();
#endif
        } else {
            rsp_send_empty();  /* Z0 not supported */
        }
        break;

    /* ── z — clear breakpoint / watchpoint ───────────────────────────────── */
    case 'z':
        if (pkt[1] == '1') {
#ifdef WIRE_ARCH_CORTEX_M
            /* z1,addr,kind — clear FPBv1 hardware breakpoint. */
            const char *p = pkt + 2;
            if (*p == ',') p++;
            uint32_t addr = parse_hex_u32(&p);
            uint8_t  n    = fpb_num_comp();
            uint8_t  slot;
            for (slot = 0; slot < n && slot < FPB_MAX_COMP; slot++) {
                if (s_fpb_addr[slot] == addr) break;
            }
            if (slot < n && slot < FPB_MAX_COMP) {
                FPB_COMP0[slot]  = 0;
                s_fpb_addr[slot] = 0;
                rsp_send_ok();
            } else {
                rsp_send_error(0x0e);  /* E0e: address not in active BPs */
            }
#else
            rsp_send_empty();
#endif
        } else if (pkt[1] == '2' || pkt[1] == '3' || pkt[1] == '4') {
#ifdef WIRE_ARCH_CORTEX_M
            /* z2/z3/z4,addr — clear DWT watchpoint. */
            const char *p = pkt + 2;
            if (*p == ',') p++;
            uint32_t addr = parse_hex_u32(&p);
            uint8_t  n    = dwt_num_comp();
            uint8_t  slot;
            for (slot = 0; slot < n && slot < DWT_MAX_COMP; slot++) {
                if ((s_dwt_slot[slot] & ~1u) == addr) break;
            }
            if (slot < n && slot < DWT_MAX_COMP) {
                DWT_FUNCTION(slot) = 0;
                DWT_COMP(slot)     = 0;
                s_dwt_slot[slot]   = 0;
                rsp_send_ok();
            } else {
                rsp_send_error(0x0e);
            }
#else
            rsp_send_empty();
#endif
        } else {
            rsp_send_empty();  /* z0 not supported */
        }
        break;

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

    /* Send stop reply immediately so the host's rsp_wait_for_stop() returns
     * without polling.  For crash analysis (host connects after the fact),
     * this packet may be missed — the '?' handler still works as a fallback. */
    if (s_signal != 0) {
        char reply[4];
        reply[0] = 'S';
        reply[1] = s_hex[(s_signal >> 4) & 0xf];
        reply[2] = s_hex[s_signal & 0xf];
        reply[3] = '\0';
        rsp_send_str(reply);
    }

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
#if defined(WIRE_LIVE_DEBUG) && defined(WIRE_ARCH_CORTEX_M)
    wire_enable_debug_monitor();
#endif
}
