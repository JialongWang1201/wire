#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Exercise the firmware handler with a host UART stub. */
#define __ARM_ARCH_7M__ 1
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
#include "../src/wire_rsp.c"
#pragma GCC diagnostic pop

static uint8_t sent[WIRE_PKT_BUF + 4u];
static size_t sent_len;

void wire_uart_write(const uint8_t *buf, size_t len)
{
    assert(len <= sizeof(sent));
    memcpy(sent, buf, len);
    sent_len = len;
}

int wire_uart_read(uint8_t *byte)
{
    *byte = '+';
    return 0;
}

void wire_regs_to_hex(const wire_regs_t *regs, char *hex)
{
    (void)regs;
    (void)hex;
}

void wire_regs_from_hex(const char *hex, wire_regs_t *regs)
{
    (void)hex;
    (void)regs;
}

static void check_chunk(uint32_t offset, uint32_t request, size_t payload,
                        char marker)
{
    sent_len = 0;
    handle_qxfer_features("target.xml", offset, request);
    assert(sent_len == payload + 5u);
    assert(sent[0] == '$');
    assert(sent[1] == (uint8_t)marker);
    assert(memcmp(sent + 2, s_target_xml + offset, payload) == 0);
    assert(sent[payload + 2u] == '#');
}

int main(void)
{
    check_chunk(0u, 511u, 511u, 'm');
    check_chunk(0u, 512u, 511u, 'm');
    check_chunk(0u, UINT32_MAX, 511u, 'm');
    check_chunk(511u, UINT32_MAX, sizeof(s_target_xml) - 1u - 511u, 'l');
    return 0;
}
