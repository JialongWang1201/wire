#include "wire_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int transaction_count;
static int fail_transaction;

static void expect(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

int rsp_transaction(int fd, const char *cmd, char *response, size_t capacity)
{
    (void)fd;
    transaction_count++;
    if (transaction_count == fail_transaction) return WIRE_ERR_TIMEOUT;
    switch (transaction_count) {
    case 1:
        expect(strcmp(cmd, "?") == 0, "query halt first");
        snprintf(response, capacity, "S05");
        break;
    case 2:
        expect(strcmp(cmd, "g") == 0, "read registers second");
        expect(capacity > 17 * 8, "register response fits");
        memset(response, '0', 17 * 8);
        memcpy(response + 13 * 8, "00000020", 8);
        memcpy(response + 15 * 8, "01000008", 8);
        response[17 * 8] = '\0';
        break;
    case 3:
        expect(strcmp(cmd, "m 20000000,100") == 0, "read stack third");
        expect(capacity > 256 * 2, "stack response fits");
        memset(response, '0', 256 * 2);
        memcpy(response, "01010008", 8);
        response[256 * 2] = '\0';
        break;
    case 4:
        expect(strcmp(cmd, "m e000ed28,4") == 0, "read CFSR last");
        snprintf(response, capacity, "01000000");
        break;
    default:
        expect(0, "no extra transactions");
    }
    return WIRE_OK;
}

static int capture(char *buffer, size_t capacity, int failure)
{
    transaction_count = 0;
    fail_transaction = failure;
    return wire_dump_crash_to_buf(0, buffer, capacity);
}

static void test_success(void)
{
    char buffer[4096];
    expect(capture(buffer, sizeof(buffer), 0) == 0, "successful crash capture");
    expect(transaction_count == 4, "all crash fields collected");
    expect(strstr(buffer, "\"halt_signal\": 5") != NULL, "halt signal emitted");
    expect(strstr(buffer, "\"halt_signal_name\": \"SIGTRAP\"") != NULL,
           "signal name emitted");
    expect(strstr(buffer, "\"sp\": \"0x20000000\"") != NULL, "SP emitted");
    expect(strstr(buffer, "\"pc\": \"0x08000001\"") != NULL, "PC emitted");
    expect(strstr(buffer, "\"cfsr\": \"0x00000001\"") != NULL, "CFSR emitted");
    expect(strstr(buffer, "IACCVIOL") != NULL, "CFSR decoded");
    expect(strstr(buffer, "\"stack_frames\": [\"0x08000101\"]") != NULL,
           "backtrace emitted");
    expect(strstr(buffer, "\"stack_dump_hex\": \"01010008") != NULL,
           "stack bytes emitted");
    expect(strstr(buffer, "\"wire_version\": \"1.0.0\"\n}\n") != NULL,
           "JSON output completed");
}

static void test_buffer_bounds(void)
{
    unsigned char guarded[4096];
    char reference[4096];
    expect(capture(reference, sizeof(reference), 0) == 0, "measure output size");
    size_t length = strlen(reference);
    size_t capacities[] = {1, 2, 32, length, length + 1};
    for (size_t i = 0; i < sizeof(capacities) / sizeof(capacities[0]); i++) {
        size_t capacity = capacities[i];
        memset(guarded, 0xa5, sizeof(guarded));
        char *buffer = (char *)guarded + 1;
        int rc = capture(buffer, capacity, 0);
        expect(guarded[0] == 0xa5, "leading guard preserved");
        for (size_t j = capacity + 1; j < sizeof(guarded); j++)
            expect(guarded[j] == 0xa5, "bytes past capacity preserved");
        if (capacity <= length) {
            expect(rc == -1, "truncation returns an error");
            expect(buffer[0] == '\0', "truncated JSON is not exposed");
        } else {
            expect(rc == 0, "exact capacity including terminator succeeds");
            expect(strlen(buffer) == length, "complete output is terminated");
        }
    }

    transaction_count = 0;
    expect(wire_dump_crash_to_buf(0, NULL, 1) == -1, "NULL buffer rejected");
    expect(wire_dump_crash_to_buf(0, reference, 0) == -1, "zero capacity rejected");
    expect(transaction_count == 0, "invalid buffers do not access the target");
}

static void test_target_failures(void)
{
    char buffer[4096];
    expect(capture(buffer, sizeof(buffer), 1) == 1, "halt timeout preserved");
    expect(strcmp(buffer, "{\"halt_signal\":0,\"timeout\":true}\n") == 0,
           "timeout JSON preserved");
    expect(capture(buffer, 1, 1) == -1, "truncated timeout JSON rejected");
    expect(buffer[0] == '\0', "truncated timeout output cleared");
    expect(capture(buffer, sizeof(buffer), 2) == 1, "register read error preserved");
    expect(buffer[0] == '\0', "failed register read leaves empty output");
    expect(capture(buffer, sizeof(buffer), 3) == 0, "optional stack read may fail");
    expect(strstr(buffer, "\"stack_frames\": []") != NULL, "failed stack is empty");
    expect(capture(buffer, sizeof(buffer), 4) == 0, "optional CFSR read may fail");
    expect(strstr(buffer, "no fault bits set") != NULL, "failed CFSR defaults to zero");

    transaction_count = 0;
    fail_transaction = 1;
    expect(wire_dump_crash(0) == 1, "stdout path preserves timeout status");
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--stdout") == 0) {
        transaction_count = 0;
        fail_transaction = 0;
        int rc = wire_dump_crash(0);
        expect(transaction_count == 4, "stdout collects all crash fields");
        return rc;
    }
    test_success();
    test_buffer_bounds();
    test_target_failures();
    puts("wire_crash_test: OK");
    return 0;
}
