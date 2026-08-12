#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "../src/wire_rsp_parser.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    wire_rsp_memory_request_t request;

    if (size == 0)
        return 0;
    if (data[0] == 'm') {
        if (wire_rsp_parse_memory_read((const char *)data, size, &request) == 0 &&
            (request.data != NULL || request.data_length != 0))
            abort();
    } else if (data[0] == 'M') {
        if (wire_rsp_parse_memory_write((const char *)data, size, &request) == 0) {
            if (request.data_length != (size_t)request.length * 2u)
                abort();
            for (size_t i = 0; i < request.data_length; i++) {
                uint8_t value;
                if (wire_rsp_hex_value(request.data[i], &value) != 0)
                    abort();
            }
        }
    }
    if (size >= 8u) {
        uint32_t address = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
                           ((uint32_t)data[2] << 8) | data[3];
        uint32_t length = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) |
                          ((uint32_t)data[6] << 8) | data[7];
        int expected = address >= 0x20000000u &&
                       (uint64_t)address + length <= 0x20040000u;
        if (wire_rsp_range_is_allowed(0x20000000u, 0x20040000u,
                                      address, length) != expected)
            abort();
    }
    return 0;
}
