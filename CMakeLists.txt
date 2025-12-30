cmake_minimum_required(VERSION 3.16)
project(wire C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── Host-only targets (skipped when cross-compiling for ARM) ─────────────
if(NOT CMAKE_CROSSCOMPILING)

add_executable(wire_host
  host/wire_host.c
  host/wire_serial.c
  host/wire_rsp_client.c
  host/wire_crash.c
)
target_compile_options(wire_host PRIVATE -Wall -Wextra -Werror -O2)
set_target_properties(wire_host PROPERTIES OUTPUT_NAME "wire-host")

enable_testing()

endif()

# ── Firmware library (cross-compiled for target) ─────────────────────────
# When included from MicroKernel-MPU:
#   add_subdirectory(tools/wire)
#   target_link_libraries(${PROJECT_NAME}.elf PRIVATE wire_firmware)
#   target_include_directories(${PROJECT_NAME}.elf PRIVATE tools/wire/include)

add_library(wire_firmware OBJECT
  src/wire_rsp.c
  src/wire_regs_cm.c
  src/wire_exception.c
)
target_include_directories(wire_firmware PUBLIC
  ${CMAKE_CURRENT_LIST_DIR}/include
)
target_compile_options(wire_firmware PRIVATE
  -Wall -Wextra -O2
)
