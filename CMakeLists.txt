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

add_executable(wire_rsp_qxfer_test test/wire_rsp_qxfer_test.c)
target_compile_options(wire_rsp_qxfer_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME rsp_qxfer_bounds COMMAND wire_rsp_qxfer_test)

add_executable(wire_rsp_memory_test
  test/wire_rsp_memory_test.c
)
target_compile_options(wire_rsp_memory_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME rsp_memory_validation COMMAND wire_rsp_memory_test)

add_executable(wire_regs_cm_test test/wire_regs_cm_test.c src/wire_regs_cm.c)
target_compile_definitions(wire_regs_cm_test PRIVATE __ARM_ARCH_7M__=1)
target_compile_options(wire_regs_cm_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME regs_exception_frames COMMAND wire_regs_cm_test)

add_executable(wire_crash_handshake_test test/wire_crash_handshake_test.c
  host/wire_rsp_client.c host/wire_crash.c)
target_compile_options(wire_crash_handshake_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME crash_spontaneous_stop COMMAND wire_crash_handshake_test)

option(WIRE_ENABLE_SANITIZERS "Build host tests with ASan and UBSan" OFF)
if(WIRE_ENABLE_SANITIZERS)
  foreach(target wire_host wire_rsp_qxfer_test wire_rsp_memory_test
                 wire_regs_cm_test wire_crash_handshake_test)
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined
      -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
  endforeach()
endif()

option(WIRE_BUILD_FUZZERS "Build libFuzzer protocol targets" OFF)
if(WIRE_BUILD_FUZZERS)
  if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "WIRE_BUILD_FUZZERS requires upstream Clang with libFuzzer")
  endif()
  add_executable(wire_rsp_memory_fuzz
    test/wire_rsp_memory_fuzz.c
  )
  target_compile_options(wire_rsp_memory_fuzz PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-omit-frame-pointer
  )
  target_link_options(wire_rsp_memory_fuzz PRIVATE
    -fsanitize=fuzzer,address,undefined
  )
endif()

endif()

# ── Firmware library (cross-compiled for target) ─────────────────────────
# When included from MicroKernel-MPU:
#   add_subdirectory(tools/wire)
#   target_link_libraries(${PROJECT_NAME}.elf PRIVATE wire_firmware)
#   target_include_directories(${PROJECT_NAME}.elf PRIVATE tools/wire/include)

if(CMAKE_CROSSCOMPILING)
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
endif()
