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
#include "wire_rsp_parser.h"

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

/* ── Cortex-M live debug: DEMCR + FPB registers (v1 and v2) ─────────────── */

#ifdef WIRE_ARCH_CORTEX_M

/* Debug Exception and Monitor Control Register */
#define DEMCR           (*(volatile uint32_t *)0xE000EDFCu)
#define DEMCR_MON_EN    (1u << 16)  /* enable DebugMonitor exception */
#define DEMCR_MON_PEND  (1u << 17)  /* pend DebugMonitor (soft-halt break-in) */
#define DEMCR_MON_STEP  (1u << 18)  /* single-step on DebugMonitor return */

/* Flash Patch and Breakpoint — base registers (same address for v1 and v2) */
#define FPB_CTRL  (*(volatile uint32_t *)0xE0002000u)
#define FPB_COMP0 ((volatile uint32_t *)0xE0002008u)

/* FPB_CTRL bits */
#define FPB_CTRL_ENABLE (1u << 0)
#define FPB_CTRL_KEY    (1u << 1)  /* FPBv2: write-enable; must be set with ENABLE */
/* FPB_CTRL.REV field [31:28]: 0 = FPBv1 (M3/M4), 1 = FPBv2 (M7/M33/M35P) */
#define FPB_REV(ctrl)   (((ctrl) >> 28) & 0xFu)

/* FPBv1 REPLACE selects the addressed halfword of a 32-bit word. */
#define FPB_COMP_WORD_V1(addr) ((((addr) & 2u) ? 2u : 1u) << 30 | \
                                ((uint32_t)(addr) & 0x1FFFFFFCu) | 1u)

/* FPBv2 comparator word: address in bits [31:1] (2-byte aligned), ENABLE in bit [0].
 * No REPLACE field; the FPB always generates a DebugMonitor event. */
#define FPB_COMP_WORD_V2(addr) ((uint32_t)(addr) | 1u)

/* Maximum comparators tracked (hardware reports actual count via FPB_CTRL.NUM_CODE). */
#define FPB_MAX_COMP 8u

/* Active comparator addresses; 0 means the slot is free. */
static uint32_t s_fpb_addr[FPB_MAX_COMP];

/* Data Watchpoint and Trace — DWT (same registers on M3/M4/M7/M33) */
#define DWT_CTRL        (*(volatile uint32_t *)0xE0001000u)
#define DWT_COMP(n)     (*(volatile uint32_t *)(0xE0001020u + (uint32_t)(n) * 0x10u))
#define DWT_MASK(n)     (*(volatile uint32_t *)(0xE0001024u + (uint32_t)(n) * 0x10u))
#define DWT_FUNCTION(n) (*(volatile uint32_t *)(0xE0001028u + (uint32_t)(n) * 0x10u))
/* DWT_FUNCTION bits [3:0]: 5=read, 6=write, 7=read+write */
#define DWT_MAX_COMP 4u

/* Occupancy is separate because address zero and odd addresses are valid. */
static uint32_t s_dwt_slot[DWT_MAX_COMP];
static uint8_t s_dwt_used[DWT_MAX_COMP];
static uint8_t s_fpb_used[FPB_MAX_COMP];

static uint8_t dwt_num_comp(void)
{
    return (uint8_t)((DWT_CTRL >> 28) & 0xFu);
}

static uint8_t fpb_num_comp(void)
{
    /* NUM_CODE field is bits [7:4] in both FPBv1 and FPBv2. */
    return (uint8_t)((FPB_CTRL >> 4) & 0xFu);
}

static uint32_t dwt_watch_function(char type)
{
    switch (type) {
    case '2': return 6u; /* write */
    case '3': return 5u; /* read */
    default:  return 7u; /* read/write */
    }
}

/* Enable FPB and DebugMonitor exception.
 * Detects FPB revision at runtime: FPBv2 (M7/M33) requires KEY=1 alongside
 * ENABLE=1 in the same write; a read-modify-write is not sufficient.
 * Must be called before any Z1 breakpoint can fire DebugMonitor.
 * Called automatically from wire_init() when WIRE_LIVE_DEBUG is defined. */
void wire_enable_debug_monitor(void)
{
    if (FPB_REV(FPB_CTRL) != 0u) {
        /* FPBv2: write KEY=1 and ENABLE=1 in a single store. */
        FPB_CTRL = FPB_CTRL_KEY | FPB_CTRL_ENABLE;
    } else {
        /* FPBv1: simple OR-enable. */
        FPB_CTRL |= FPB_CTRL_ENABLE;
    }
    DEMCR |= DEMCR_MON_EN;
    /* DEMCR.MON_STEP is left clear — set transiently only during 's'. */
}

/* ── Break-in: soft-halt a running MCU from the host ────────────────────── */

/* Weak default: non-blocking UART read.  Override in your BSP to enable
 * wire_poll_break_in().  The default always returns 0 (no byte available). */
__attribute__((weak)) int wire_uart_try_read(uint8_t *byte)
{
    (void)byte;
    return 0;
}

/* Poll for a Ctrl-C (0x03) break-in byte from the host.
 * When detected, pends DebugMonitor so the CPU halts at the next opportunity.
 * Call from your main loop or RTOS idle task.
 * Requires wire_uart_try_read() to be overridden (default does nothing). */
void wire_poll_break_in(void)
{
    uint8_t b;
    if (wire_uart_try_read(&b) && b == 0x03u) {
        DEMCR |= DEMCR_MON_EN | DEMCR_MON_PEND;
    }
}

#endif /* WIRE_ARCH_CORTEX_M */

/* ── Module state ────────────────────────────────────────────────────────── */

static wire_regs_t  s_regs;
static int          s_signal;
static int          s_resume_enabled;
static uint32_t     s_initial_sp;
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
        int      overflow = 0;
        while (1) {
            c = rsp_getc();
            if (c == '#') break;
            if (len < WIRE_PKT_BUF - 1) {
                buf[len++] = (char)c;
            } else {
                overflow = 1;
            }
            sum += c;
        }
        buf[len] = '\0';

        /* Read 2-char checksum */
        char cs[2];
        cs[0] = (char)rsp_getc();
        cs[1] = (char)rsp_getc();
        uint8_t expected = (uint8_t)((hex_nibble(cs[0]) << 4) | hex_nibble(cs[1]));

        if (!overflow && sum == expected) {
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
    if (!wire_rsp_range_is_allowed(s_ram_start, s_ram_end, addr, len))
        return -1;
    const uint8_t *src = (const uint8_t *)(uintptr_t)addr;
    for (size_t i = 0; i < len; i++) {
        *out++ = s_hex[src[i] >> 4];
        *out++ = s_hex[src[i] & 0xf];
    }
    return 0;
}

static int mem_write_safe(uint32_t addr, size_t len, const char *hex)
{
    if (!wire_rsp_range_is_allowed(s_ram_start, s_ram_end, addr, len))
        return -1;
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
    if (avail > sizeof(buf) - 1u) avail = sizeof(buf) - 1u;
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
    case 'G': {
        if (len != 1u + WIRE_REG_HEX_CHARS) {
            rsp_send_error(0x01);
            break;
        }
        for (size_t i = 1; i < len; i++) {
            uint8_t digit;
            if (wire_rsp_hex_value(pkt[i], &digit) != 0) {
                rsp_send_error(0x01);
                return 0;
            }
        }
        wire_regs_t updated;
        wire_regs_from_hex(pkt + 1, &updated);
        if (s_resume_enabled && updated.r[WIRE_REG_SP] != s_initial_sp) {
            rsp_send_error(0x01); /* SP cannot change through exception return. */
            break;
        }
        s_regs = updated;
        rsp_send_ok();
        break;
    }

    /* ── P reg=value — write one register ──────────────────────────────── */
    case 'P': {
        const char *cursor = pkt + 1;
        const char *end = pkt + len;
        uint32_t reg;
        if (wire_rsp_parse_hex_u32(&cursor, end, &reg) != 0 ||
            cursor == end || *cursor++ != '=' ||
            reg >= WIRE_REG_COUNT || (size_t)(end - cursor) != 8u) {
            rsp_send_error(0x01);
            break;
        }
        uint32_t value = 0;
        for (unsigned byte = 0; byte < 4u; byte++) {
            uint8_t hi, lo;
            if (wire_rsp_hex_value(cursor[byte * 2u], &hi) != 0 ||
                wire_rsp_hex_value(cursor[byte * 2u + 1u], &lo) != 0) {
                rsp_send_error(0x01);
                return 0;
            }
            value |= (uint32_t)((hi << 4) | lo) << (byte * 8u);
        }
        if (s_resume_enabled && reg == WIRE_REG_SP && value != s_initial_sp) {
            rsp_send_error(0x01);
            break;
        }
        if (reg == 16u) s_regs.xpsr = value;
        else s_regs.r[reg] = value;
        rsp_send_ok();
        break;
    }

    /* ── m addr,length — read memory ─────────────────────────────────────── */
    case 'm': {
        wire_rsp_memory_request_t request;
        if (wire_rsp_parse_memory_read(pkt, len, &request) != 0 ||
            request.length > (WIRE_PKT_BUF / 2u)) {
            rsp_send_error(0x01);
            break;
        }

        char out[WIRE_PKT_BUF];
        if (mem_read_safe(request.address, request.length, out) != 0)
            rsp_send_error(0x0e);  /* EFAULT */
        else
            rsp_send(out, (size_t)request.length * 2u);
        break;
    }

    /* ── M addr,length:data — write memory ───────────────────────────────── */
    case 'M': {
        wire_rsp_memory_request_t request;
        if (wire_rsp_parse_memory_write(pkt, len, &request) != 0) {
            rsp_send_error(0x01);
            break;
        }
        if (mem_write_safe(request.address, request.length, request.data) != 0)
            rsp_send_error(0x0e);
        else
            rsp_send_ok();
        break;
    }

    /* ── c — continue ────────────────────────────────────────────────────── */
    case 'c':
        /* GDB waits for the next asynchronous stop reply. */
        return 1;

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
            /* Z1,addr,kind — FPB hardware breakpoint (kind ignored).
             * Selects FPBv1 or FPBv2 comparator word format at runtime. */
            const char *p = pkt + 2;
            if (*p == ',') p++;
            uint32_t addr = parse_hex_u32(&p);
            uint8_t  n    = fpb_num_comp();
            uint8_t  slot;
            for (slot = 0; slot < n && slot < FPB_MAX_COMP; slot++) {
                if (!s_fpb_used[slot]) break;
            }
            if (slot >= n || slot >= FPB_MAX_COMP) {
                rsp_send_error(0x0e);  /* E0e: no free FPB comparator */
                break;
            }
            s_fpb_addr[slot] = addr;
            s_fpb_used[slot] = 1;
            FPB_COMP0[slot]  = (FPB_REV(FPB_CTRL) != 0u)
                               ? FPB_COMP_WORD_V2(addr)
                               : FPB_COMP_WORD_V1(addr);
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
            uint32_t func = dwt_watch_function(pkt[1]);
            uint8_t  n    = dwt_num_comp();
            uint8_t  slot;
            for (slot = 0; slot < n && slot < DWT_MAX_COMP; slot++) {
                if (!s_dwt_used[slot]) break;
            }
            if (slot >= n || slot >= DWT_MAX_COMP) {
                rsp_send_error(0x0e);
                break;
            }
            s_dwt_slot[slot]   = addr;
            s_dwt_used[slot]   = 1;
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
                if (s_fpb_used[slot] && s_fpb_addr[slot] == addr) break;
            }
            if (slot < n && slot < FPB_MAX_COMP) {
                FPB_COMP0[slot]  = 0;
                s_fpb_addr[slot] = 0;
                s_fpb_used[slot] = 0;
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
                if (s_dwt_used[slot] && s_dwt_slot[slot] == addr) break;
            }
            if (slot < n && slot < DWT_MAX_COMP) {
                DWT_FUNCTION(slot) = 0;
                DWT_COMP(slot)     = 0;
                s_dwt_slot[slot]   = 0;
                s_dwt_used[slot]   = 0;
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

    /* ── R — reset (software system reset via SCB->AIRCR) ───────────────── */
    case 'R':
        /* RSP 'R' packet: software reset.  MCU resets immediately — no reply
         * is sent.  The host must use rsp_send_packet() (not rsp_transaction)
         * so it does not block waiting for an ACK that never arrives. */
#ifdef WIRE_ARCH_CORTEX_M
        {
            volatile uint32_t *AIRCR = (volatile uint32_t *)0xE000ED0Cu;
            /* VECTKEY (0x5FA << 16) | SYSRESETRQ (bit 2) */
            *AIRCR = (0x5FAu << 16) | (1u << 2);
            for (;;) {}  /* does not return — MCU resets */
        }
#else
        rsp_send_empty();
#endif
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

static void wire_debug_loop_impl(const wire_regs_t *regs, int signal,
                                 wire_regs_t *resume)
{
    s_regs   = *regs;
    s_signal = signal;
    s_resume_enabled = resume != NULL;
    s_initial_sp = regs->r[WIRE_REG_SP];

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
    if (resume)
        *resume = s_regs;
    s_resume_enabled = 0;
}

void wire_debug_loop(const wire_regs_t *regs, int signal)
{
    wire_debug_loop_impl(regs, signal, NULL);
}

void wire_debug_loop_resume(wire_regs_t *regs, int signal)
{
    wire_debug_loop_impl(regs, signal, regs);
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
