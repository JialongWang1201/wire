# wire

GDB RSP stub for baremetal targets. One UART wire is all you need.

No OpenOCD. No JTAG. No Python.

---

## What it does

`wire` embeds a minimal [GDB Remote Serial Protocol](https://sourceware.org/gdb/current/onlinedocs/gdb/Remote-Protocol.html) server into your firmware. On crash, the target halts and waits for a GDB session over UART.

```
[firmware crash]
       │
       ▼
wire catches the fault, halts, waits for GDB
       │
       ▼
wire-host --port /dev/ttyUSB0 --baud 115200
       │
       ▼
arm-none-eabi-gdb firmware.elf
(gdb) target remote :3333
(gdb) bt          ← call stack
(gdb) info reg    ← register state
(gdb) x/16xw $sp  ← memory
```

---

## Architecture support

| Architecture       | Phase | Status    |
|--------------------|-------|-----------|
| Cortex-M (ARMv7-M) | 1     | available |
| Cortex-A (AArch64) | 2     | planned   |
| x86\_64            | 2     | planned   |
| RISC-V 32/64       | 3     | planned   |

---

## Firmware integration

### 1. Implement two UART hooks

```c
#include "wire.h"

void wire_uart_write(const uint8_t *buf, size_t len) {
    HAL_UART_Transmit(&huart2, buf, len, HAL_MAX_DELAY);
}

int wire_uart_read(uint8_t *byte) {
    return HAL_UART_Receive(&huart2, byte, 1, HAL_MAX_DELAY) == HAL_OK ? 0 : -1;
}
```

### 2. Call `wire_init()` early

```c
int main(void) {
    HAL_Init();
    SystemClock_Config();

    // Pass your RAM region for safe GDB memory access.
    wire_init(0x20000000, 0x20000000 + 128*1024);

    // ... rest of your application
}
```

### 3. Done

`wire` installs fault handlers for `HardFault`, `MemManage`, `BusFault`, and `UsageFault`. On any crash, the CPU halts and waits for a GDB connection.

---

## Host tool

### Build

```bash
bash build_wire_host.sh
# → build/wire-host
```

### Use

```bash
# Hardware
wire-host --port /dev/ttyUSB0 --baud 115200

# QEMU
qemu-system-arm -M mps2-an385 -kernel firmware.elf -serial pty
# QEMU prints: char device redirected to /dev/pts/3
wire-host --port /dev/pts/3 --baud 0
```

Then connect GDB in another terminal:

```bash
arm-none-eabi-gdb firmware.elf
(gdb) target remote :3333
(gdb) bt
(gdb) continue
```

---

## CMake integration (as submodule)

```cmake
add_subdirectory(tools/wire)
target_link_libraries(${PROJECT_NAME}.elf PRIVATE wire_firmware)
target_include_directories(${PROJECT_NAME}.elf PRIVATE tools/wire/include)
```

---

## Resource budget (Cortex-M, Phase 1)

| Resource   | Usage   |
|------------|---------|
| RAM        | ~512 B  |
| Flash      | ~4 KB   |
| Min SRAM   | 2 KB    |

---

## Phase 1 limitations

- No breakpoints or single-step (requires DWT/FPB hardware access)
- No multi-threading support
- Cortex-M only (Cortex-A / x86 / RISC-V in Phase 2–3)
- `continue` resumes execution but does not restore register state

---

## Relationship to seam

| repo   | role                            |
|--------|---------------------------------|
| [seam](https://github.com/JialongWang1201/seam)   | post-mortem fault analysis (ring buffer → host decode) |
| wire   | live GDB debugging over UART    |

Both are support repos for [MicroKernel-MPU](https://github.com/JialongWang1201/MicroKernel-MPU).

---

## License

MIT
