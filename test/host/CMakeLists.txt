cmake_minimum_required(VERSION 3.20)
project(wire_crash_tests C)

enable_testing()
add_executable(wire_crash_test
  wire_crash_test.c
  ../../host/wire_crash.c
)
set_target_properties(wire_crash_test PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF
)
target_include_directories(wire_crash_test PRIVATE ../../host)
if(MSVC)
  target_compile_options(wire_crash_test PRIVATE /W4 /WX /utf-8)
  target_compile_definitions(wire_crash_test PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(wire_crash_test PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME wire_crash COMMAND wire_crash_test)
add_test(NAME wire_crash_stdout COMMAND wire_crash_test --stdout)
set_tests_properties(wire_crash_stdout PROPERTIES
  PASS_REGULAR_EXPRESSION "\"stack_frames\": \\[\"0x08000101\"\\]"
)
