cmake_minimum_required(VERSION 3.20)
project(wire_qemu_test C ASM)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(WIRE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")

# ── wire firmware objects (RSP + register capture; no exception shim needed) ─
# The test calls wire_debug_loop() directly so wire_exception.c (which installs
# Cortex-M fault handlers via naked assembly) is not required here.
add_library(wire_core OBJECT
    ${WIRE_ROOT}/src/wire_rsp.c
    ${WIRE_ROOT}/src/wire_regs_cm.c
)
target_include_directories(wire_core PUBLIC
    ${WIRE_ROOT}/include
)
target_compile_options(wire_core PRIVATE
    -Wall -Wextra -O2
)

# ── Test firmware ELF ────────────────────────────────────────────────────────
set(LINKER_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/link.ld")

add_executable(wire_qemu_test.elf
    "${CMAKE_CURRENT_LIST_DIR}/startup.c"
    "${CMAKE_CURRENT_LIST_DIR}/main.c"
    $<TARGET_OBJECTS:wire_core>
)
target_include_directories(wire_qemu_test.elf PRIVATE
    ${WIRE_ROOT}/include
)
target_compile_options(wire_qemu_test.elf PRIVATE
    -Wall -Wextra -O2
)
target_link_options(wire_qemu_test.elf PRIVATE
    -T${LINKER_SCRIPT}
    -Wl,-Map=wire_qemu_test.map
)
set_target_properties(wire_qemu_test.elf PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

add_custom_command(TARGET wire_qemu_test.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary
            $<TARGET_FILE:wire_qemu_test.elf>
            "${CMAKE_BINARY_DIR}/wire_qemu_test.bin"
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:wire_qemu_test.elf>
)
