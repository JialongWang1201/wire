/* wire_crash.c — Cortex-M crash dump via RSP for wire-host --dump mode
 *
 * Execution sequence (3 RSP transactions + 1 memory read):
 *   ?             → halt signal (S<signal> or T<signal>...)
 *   g             → all 17 Cortex-M registers (r0–r12, sp, lr, pc, xpsr)
 *   m <SP>,100    → 256 bytes of stack (heuristic backtrace)
 *   m e000ed28,4  → CFSR (Configurable Fault Status Register)
 *
 * Outputs JSON to stdout; returns exit code (0=success, 1=timeout/error).
 *
 * SPDX-License-Identifier: MIT
 */

#include "wire_host.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define RSP_RESP_MAX   2048
#define STACK_BYTES    256    /* bytes to read from stack */
#define CFSR_ADDR      "e000ed28"
#define WIRE_VERSION   "1.0.0"

/* ── hex helpers ─────────────────────────────────────────────────────────── */

static int hex2(const char *s)
{
    int hi = 0, lo = 0;
    if      (s[0] >= '0' && s[0] <= '9') hi = s[0] - '0';
    else if (s[0] >= 'a' && s[0] <= 'f') hi = s[0] - 'a' + 10;
    else if (s[0] >= 'A' && s[0] <= 'F') hi = s[0] - 'A' + 10;
    else return -1;
    if      (s[1] >= '0' && s[1] <= '9') lo = s[1] - '0';
    else if (s[1] >= 'a' && s[1] <= 'f') lo = s[1] - 'a' + 10;
    else if (s[1] >= 'A' && s[1] <= 'F') lo = s[1] - 'A' + 10;
    else return -1;
    return (hi << 4) | lo;
}

/* Parse 8 hex chars in little-endian byte order into uint32_t. */
static int parse_le32(const char *hex8, uint32_t *out)
{
    uint32_t val = 0;
    for (int b = 0; b < 4; b++) {
        int byte = hex2(hex8 + b * 2);
        if (byte < 0) return -1;
        val |= (uint32_t)((unsigned)byte << (b * 8));
    }
    *out = val;
    return 0;
}

/* ── halt signal ─────────────────────────────────────────────────────────── */

static const char *signal_name(int sig)
{
    switch (sig) {
    case 1:  return "SIGHUP";
    case 2:  return "SIGINT";
    case 5:  return "SIGTRAP";
    case 6:  return "SIGABRT";
    case 7:  return "SIGBUS";
    case 8:  return "SIGFPE";
    case 11: return "SIGSEGV";
    case 15: return "SIGTERM";
    default: return "UNKNOWN";
    }
}

/*
 * Parse halt response: S<2hex> or T<2hex>...
 * Returns signal number, or -1 on parse error.
 */
static int parse_halt_signal(const char *resp)
{
    if ((resp[0] == 'S' || resp[0] == 'T') && resp[1] && resp[2]) {
        int s = hex2(resp + 1);
        if (s >= 0) return s;
    }
    return -1;
}

/* ── registers ───────────────────────────────────────────────────────────── */

/*
 * Cortex-M GDB register order:
 *   r0–r12 (0–12), sp/r13 (13), lr/r14 (14), pc/r15 (15), xpsr (16)
 * Each register: 4 bytes = 8 hex chars, little-endian.
 * Total: 17 * 8 = 136 hex chars.
 */
#define CM_NREGS 17
static const char *reg_names[CM_NREGS] = {
    "r0","r1","r2","r3","r4","r5","r6","r7",
    "r8","r9","r10","r11","r12","sp","lr","pc","xpsr"
};

static int parse_registers(const char *resp, uint32_t regs[CM_NREGS])
{
    if (strlen(resp) < CM_NREGS * 8u) return -1;
    for (int i = 0; i < CM_NREGS; i++) {
        if (parse_le32(resp + i * 8, &regs[i]) != 0) return -1;
    }
    return 0;
}

/* ── CFSR decode ─────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t    bit;
    const char *name;
    const char *desc;
} CfsrBit;

static const CfsrBit cfsr_bits[] = {
    /* MemManage fault (bits 0–7) */
    { 1u << 0,  "IACCVIOL",   "instruction access MPU violation" },
    { 1u << 1,  "DACCVIOL",   "data access MPU violation" },
    { 1u << 3,  "MUNSTKERR",  "MemManage fault on unstacking" },
    { 1u << 4,  "MSTKERR",    "MemManage fault on stacking" },
    { 1u << 5,  "MLSPERR",    "MemManage fault on lazy FP save" },
    /* BusFault (bits 8–15) */
    { 1u << 8,  "IBUSERR",    "instruction bus error" },
    { 1u << 9,  "PRECISERR",  "precise data bus error" },
    { 1u << 10, "IMPRECISERR","imprecise data bus error" },
    { 1u << 11, "UNSTKERR",   "bus fault on unstacking" },
    { 1u << 12, "STKERR",     "bus fault on stacking" },
    { 1u << 13, "LSPERR",     "bus fault on lazy FP save" },
    /* UsageFault (bits 16–31) */
    { 1u << 16, "UNDEFINSTR", "undefined instruction" },
    { 1u << 17, "INVSTATE",   "invalid execution state (EPSR)" },
    { 1u << 18, "INVPC",      "integrity check violation on EXC_RETURN" },
    { 1u << 19, "NOCP",       "coprocessor not available" },
    { 1u << 24, "UNALIGNED",  "unaligned memory access" },
    { 1u << 25, "DIVBYZERO",  "divide by zero" },
};
#define CFSR_NBITS ((int)(sizeof(cfsr_bits) / sizeof(cfsr_bits[0])))

/* Build a human-readable string into out (capacity out_size). */
static void cfsr_decode(uint32_t cfsr, char *out, size_t out_size)
{
    if (out_size == 0) return;
    out[0] = '\0';
    if (cfsr == 0) {
        snprintf(out, out_size, "no fault bits set");
        return;
    }
    size_t pos = 0;
    int    first = 1;
    for (int i = 0; i < CFSR_NBITS; i++) {
        if (!(cfsr & cfsr_bits[i].bit)) continue;
        int n = snprintf(out + pos, out_size - pos,
                         "%s%s — %s",
                         first ? "" : "; ",
                         cfsr_bits[i].name,
                         cfsr_bits[i].desc);
        if (n < 0 || (size_t)n >= out_size - pos) break;
        pos  += (size_t)n;
        first = 0;
    }
}

/* ── heuristic backtrace ─────────────────────────────────────────────────── */

/*
 * Scan stack_bytes for Thumb return addresses in STM32 flash range.
 * Criteria: word-aligned in buffer, value in 0x08000001–0x08FFFFFF, bit0=1.
 * Collect up to max_frames frames in frames[]; returns count.
 */
static int heuristic_backtrace(const uint8_t *stack_bytes, int stack_len,
                                uint32_t *frames, int max_frames)
{
    int count = 0;
    for (int off = 0; off + 3 < stack_len && count < max_frames; off += 4) {
        uint32_t val = (uint32_t)stack_bytes[off]
                     | ((uint32_t)stack_bytes[off+1] << 8)
                     | ((uint32_t)stack_bytes[off+2] << 16)
                     | ((uint32_t)stack_bytes[off+3] << 24);
        /* Thumb bit set, flash range, not the very base address */
        if ((val & 1u) && val >= 0x08000001u && val <= 0x08FFFFFFu)
            frames[count++] = val;
    }
    return count;
}

/* ── JSON output helpers ─────────────────────────────────────────────────── */

/* Escape a string for JSON (no control chars expected in our strings). */
static void json_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        if      (*s == '"')  fputs("\\\"", f);
        else if (*s == '\\') fputs("\\\\", f);
        else                 fputc(*s, f);
    }
    fputc('"', f);
}

/* ── public API ──────────────────────────────────────────────────────────── */

/*
 * Execute the crash dump sequence on uart_fd.
 * Prints JSON to stdout.  Returns 0 on success, 1 on timeout/error.
 */
int wire_dump_crash(int uart_fd)
{
    char    resp[RSP_RESP_MAX];
    uint32_t regs[CM_NREGS];
    uint8_t  stack_bytes[STACK_BYTES];
    uint32_t frames[WIRE_MAX_FRAMES];
    uint32_t cfsr_val = 0;
    char     cfsr_str[512];
    int      nframes = 0;

    /* ── 1. halt signal ── */
    if (rsp_transaction(uart_fd, "?", resp, sizeof(resp)) != WIRE_OK) {
        fprintf(stdout,
                "{\"halt_signal\":0,\"timeout\":true}\n");
        return 1;
    }
    int sig = parse_halt_signal(resp);
    if (sig < 0) sig = 0;
    if (sig == 0) {
        fprintf(stdout, "{\"halt_signal\":0,\"timeout\":true}\n");
        return 1;
    }

    /* ── 2. read all registers ── */
    if (rsp_transaction(uart_fd, "g", resp, sizeof(resp)) != WIRE_OK ||
        parse_registers(resp, regs) != 0) {
        fprintf(stderr, "wire-host: failed to read registers\n");
        return 1;
    }
    uint32_t sp_val = regs[13];  /* SP */

    /* ── 3. read stack (heuristic backtrace) ── */
    {
        char cmd[32];
        snprintf(cmd, sizeof(cmd), "m %x,%x", sp_val, STACK_BYTES);
        if (rsp_transaction(uart_fd, cmd, resp, sizeof(resp)) == WIRE_OK) {
            int ok = 1;
            size_t resp_len = strlen(resp);
            if (resp_len < STACK_BYTES * 2u) ok = 0;
            if (ok) {
                for (int i = 0; i < STACK_BYTES; i++) {
                    int b = hex2(resp + i * 2);
                    if (b < 0) { ok = 0; break; }
                    stack_bytes[i] = (uint8_t)b;
                }
            }
            if (ok)
                nframes = heuristic_backtrace(stack_bytes, STACK_BYTES,
                                              frames, WIRE_MAX_FRAMES);
        }
    }

    /* ── 4. read CFSR ── */
    {
        char cmd[32];
        snprintf(cmd, sizeof(cmd), "m %s,4", CFSR_ADDR);
        if (rsp_transaction(uart_fd, cmd, resp, sizeof(resp)) == WIRE_OK &&
            strlen(resp) >= 8) {
            parse_le32(resp, &cfsr_val);
        }
    }
    cfsr_decode(cfsr_val, cfsr_str, sizeof(cfsr_str));

    /* ── 5. timestamp ── */
    char ts[32] = "unknown";
    {
        time_t now = time(NULL);
        struct tm *t = gmtime(&now);
        if (t) strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", t);
    }

    /* ── 6. emit JSON ── */
    fprintf(stdout, "{\n");
    fprintf(stdout, "  \"halt_signal\": %d,\n", sig);
    fprintf(stdout, "  \"halt_signal_name\": \"%s\",\n", signal_name(sig));
    fprintf(stdout, "  \"registers\": {\n");
    for (int i = 0; i < CM_NREGS; i++) {
        fprintf(stdout, "    \"%s\": \"0x%08x\"%s\n",
                reg_names[i], regs[i],
                i < CM_NREGS - 1 ? "," : "");
    }
    fprintf(stdout, "  },\n");
    fprintf(stdout, "  \"cfsr\": \"0x%08x\",\n", cfsr_val);
    fprintf(stdout, "  \"cfsr_decoded\": ");
    json_string(stdout, cfsr_str);
    fprintf(stdout, ",\n");
    fprintf(stdout, "  \"stack_frames\": [");
    for (int i = 0; i < nframes; i++) {
        fprintf(stdout, "%s\"0x%08x\"", i ? ", " : "", frames[i]);
    }
    fprintf(stdout, "],\n");

    /* stack_dump_hex (first 64 bytes) */
    fprintf(stdout, "  \"stack_dump_hex\": \"");
    int dump_len = nframes > 0 ? 64 : 0;  /* only include if backtrace found */
    for (int i = 0; i < dump_len && i < STACK_BYTES; i++)
        fprintf(stdout, "%02x", stack_bytes[i]);
    fprintf(stdout, "\",\n");

    fprintf(stdout, "  \"timestamp\": \"%s\",\n", ts);
    fprintf(stdout, "  \"wire_version\": \"%s\"\n", WIRE_VERSION);
    fprintf(stdout, "}\n");

    return 0;
}
