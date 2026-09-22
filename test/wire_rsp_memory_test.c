#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/wire_rsp_parser.h"

static int failures;

static void expect(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static void expect_invalid_read(const char *packet)
{
    wire_rsp_memory_request_t request;
    expect(wire_rsp_parse_memory_read(packet, strlen(packet), &request) != 0,
           packet);
}

static void expect_invalid_write(const char *packet)
{
    wire_rsp_memory_request_t request;
    expect(wire_rsp_parse_memory_write(packet, strlen(packet), &request) != 0,
           packet);
}

int main(void)
{
    wire_rsp_memory_request_t request;
    static const char *invalid_reads[] = {
        "m", "m,1", "m1", "m1,", "m20g0,1", "m100000000,1",
        "m20000000,100000000", "m20000000,1:00", "m20000000,1x",
        "m20000000,,1", "m20000000,0x1"
    };
    static const char *invalid_writes[] = {
        "M", "M,1:00", "M1", "M1,1", "M1,1:", "M1,1:0",
        "M1,1:0000", "M1,1:0g", "M20g0,1:00", "M100000000,1:00",
        "M20000000,100000000:00", "M1,0:00", "M1,1:00#00",
        "M1,1:0G", "M1,2:000"
    };
    size_t i;

    const char *valid_read = "m20000000,10";
    const char *valid_write = "M20000000,2:00fF";

    expect(wire_rsp_parse_memory_read(valid_read, strlen(valid_read), &request) == 0,
           "parse valid memory read");
    expect(request.address == 0x20000000u && request.length == 0x10u,
           "decode memory read fields");

    expect(wire_rsp_parse_memory_write(valid_write, strlen(valid_write), &request) == 0,
           "parse valid memory write");
    expect(request.address == 0x20000000u && request.length == 2u &&
           request.data_length == 4u,
           "decode memory write fields");

    for (i = 0; i < sizeof(invalid_reads) / sizeof(invalid_reads[0]); i++)
        expect_invalid_read(invalid_reads[i]);
    for (i = 0; i < sizeof(invalid_writes) / sizeof(invalid_writes[0]); i++)
        expect_invalid_write(invalid_writes[i]);

    {
        const char packet[] = { 'M', '1', ',', '1', ':', '0', '0', '\0', '0' };
        expect(wire_rsp_parse_memory_write(packet, sizeof(packet), &request) != 0,
               "reject embedded NUL and trailing data");
    }

    expect(wire_rsp_range_is_allowed(0x20000000u, 0x20040000u,
                                     0xffffffffu, 1u) == 0,
           "reject address-plus-length wraparound");
    expect(wire_rsp_range_is_allowed(0x20000000u, 0x20040000u,
                                     0x2003ffffu, 1u) == 1,
           "allow final byte in range");
    expect(wire_rsp_range_is_allowed(0x20000000u, 0x20040000u,
                                     0x20040000u, 0u) == 1,
           "allow empty range at end");
    expect(wire_rsp_range_is_allowed(0x20040000u, 0x20000000u,
                                     0x20020000u, 1u) == 0,
           "reject invalid configured range");
    expect(wire_rsp_range_is_allowed(0xfffffff0u, 0xffffffffu,
                                     0xfffffffeu, 2u) == 0,
           "reject crossing the 32-bit address boundary");
    expect(wire_rsp_range_is_allowed(0x20000000u, 0x20040000u,
                                     0x2003ffffu, 2u) == 0,
           "reject crossing configured end");

    return failures == 0 ? 0 : 1;
}
