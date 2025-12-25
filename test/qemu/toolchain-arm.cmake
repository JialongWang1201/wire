# test/qemu/toolchain-arm.cmake — ARM Cortex-M3 cross-compile toolchain
#
# Targets mps2-an385 (Cortex-M3, no FPU, Thumb-2).
# Requires arm-none-eabi-gcc in PATH.

set(CMAKE_SYSTEM_NAME  Generic)
set(CMAKE_SYSTEM_PROCESSOR cortex-m3)

set(CMAKE_C_COMPILER   arm-none-eabi-gcc)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)
set(CMAKE_OBJCOPY      arm-none-eabi-objcopy)
set(CMAKE_SIZE         arm-none-eabi-size)

set(CMAKE_C_FLAGS_INIT
    "-mcpu=cortex-m3 -mthumb -ffunction-sections -fdata-sections"
    CACHE STRING "" FORCE)

set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-Wl,--gc-sections -specs=nosys.specs -specs=nano.specs"
    CACHE STRING "" FORCE)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
