#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "../include/wire_regs.h"

static void check_frame(uint32_t exc_return, size_t core_offset,
                        uint32_t alignment_word)
{
    uint32_t frame[27] = {0};
    uint32_t saved[8] = {0};
    uint32_t *core = frame + core_offset;
    wire_regs_t regs;

    core[0] = 0x12345678u;
    core[6] = 0x08001234u;
    core[7] = (1u << 24) | alignment_word;
    saved[0] = 0xaabbccddu;
    wire_regs_capture_cm(frame, saved, exc_return, &regs);

    assert(regs.r[0] == core[0]);
    assert(regs.r[4] == saved[0]);
    assert(regs.r[15] == core[6]);
    assert(regs.r[13] == (uint32_t)(uintptr_t)(core + 8 +
                                  (alignment_word ? 1 : 0)));

    regs.r[0] = 0xdeadbeefu;
    regs.r[4] = 0x01234567u;
    regs.r[15] = 0x08005678u;
    wire_regs_restore_cm(&regs, frame, saved, exc_return);
    assert(core[0] == 0xdeadbeefu);
    assert(saved[0] == 0x01234567u);
    assert(core[6] == 0x08005678u);
}

int main(void)
{
    check_frame(0xfffffff9u, 0u, 0u);
    check_frame(0xfffffff9u, 0u, 1u << 9);
    check_frame(0xffffffe9u, 18u, 0u);
    check_frame(0xffffffe9u, 18u, 1u << 9);
    return 0;
}
