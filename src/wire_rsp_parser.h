/* wire_rsp_parser.h — bounded parsers for RSP memory packets
 *
 * Internal header shared by the firmware stub and host-side tests.
 * SPDX-License-Identifier: MIT
 */
#ifndef WIRE_RSP_PARSER_H
#define WIRE_RSP_PARSER_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t address;
    uint32_t length;
    const char *data;
    size_t data_length;
} wire_rsp_memory_request_t;

static inline int wire_rsp_hex_value(char ch, uint8_t *value)
{
    if (ch >= '0' && ch <= '9') {
        *value = (uint8_t)(ch - '0');
        return 0;
    }
    if (ch >= 'a' && ch <= 'f') {
        *value = (uint8_t)(ch - 'a' + 10);
        return 0;
    }
    if (ch >= 'A' && ch <= 'F') {
        *value = (uint8_t)(ch - 'A' + 10);
        return 0;
    }
    return -1;
}

static inline int wire_rsp_parse_hex_u32(const char **cursor, const char *end,
                                         uint32_t *value)
{
    const char *p = *cursor;
    uint32_t result = 0;
    size_t digits = 0;

    while (p < end) {
        uint8_t digit;
        if (wire_rsp_hex_value(*p, &digit) != 0)
            break;
        if (result > (UINT32_MAX - digit) / 16u)
            return -1;
        result = result * 16u + digit;
        p++;
        digits++;
    }
    if (digits == 0)
        return -1;

    *cursor = p;
    *value = result;
    return 0;
}

static inline int wire_rsp_parse_memory_read(const char *packet, size_t length,
                                             wire_rsp_memory_request_t *request)
{
    const char *p;
    const char *end;

    if (!packet || !request || length == 0 || packet[0] != 'm')
        return -1;

    p = packet + 1;
    end = packet + length;
    if (wire_rsp_parse_hex_u32(&p, end, &request->address) != 0 ||
        p == end || *p++ != ',' ||
        wire_rsp_parse_hex_u32(&p, end, &request->length) != 0 || p != end)
        return -1;

    request->data = NULL;
    request->data_length = 0;
    return 0;
}

static inline int wire_rsp_parse_memory_write(const char *packet, size_t length,
                                              wire_rsp_memory_request_t *request)
{
    const char *p;
    const char *end;
    size_t i;

    if (!packet || !request || length == 0 || packet[0] != 'M')
        return -1;

    p = packet + 1;
    end = packet + length;
    if (wire_rsp_parse_hex_u32(&p, end, &request->address) != 0 ||
        p == end || *p++ != ',' ||
        wire_rsp_parse_hex_u32(&p, end, &request->length) != 0 ||
        p == end || *p++ != ':')
        return -1;

    request->data = p;
    request->data_length = (size_t)(end - p);
    if ((request->data_length & 1u) != 0 ||
        request->length != request->data_length / 2u)
        return -1;

    for (i = 0; i < request->data_length; i++) {
        uint8_t digit;
        if (wire_rsp_hex_value(request->data[i], &digit) != 0)
            return -1;
    }
    return 0;
}

static inline int wire_rsp_range_is_allowed(uint32_t start, uint32_t end,
                                            uint32_t address, size_t length)
{
    if (start == 0 && end == 0)
        return 1;
    if (start > end || address < start || address > end)
        return 0;
    return length <= (size_t)(end - address);
}

#endif /* WIRE_RSP_PARSER_H */
