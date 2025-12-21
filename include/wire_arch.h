/* wire_arch.h — Architecture detection and portability macros
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WIRE_ARCH_H
#define WIRE_ARCH_H

#if defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__)
#  define WIRE_ARCH_CORTEX_M   1
#  define WIRE_ARCH_NAME       "arm"
#  define WIRE_REG_COUNT       17   /* r0-r15, xpsr */
#  define WIRE_REG_BYTES       4
#elif defined(__aarch64__)
#  define WIRE_ARCH_AARCH64    1
#  define WIRE_ARCH_NAME       "aarch64"
#  define WIRE_REG_COUNT       34   /* x0-x30, sp, pc, pstate */
#  define WIRE_REG_BYTES       8
#elif defined(__x86_64__)
#  define WIRE_ARCH_X86_64     1
#  define WIRE_ARCH_NAME       "x86_64"
#  define WIRE_REG_COUNT       17   /* rax..r15, rip */
#  define WIRE_REG_BYTES       8
#elif defined(__riscv)
#  define WIRE_ARCH_RISCV      1
#  define WIRE_ARCH_NAME       "riscv"
#  define WIRE_REG_COUNT       33   /* x0-x31, pc */
#  define WIRE_REG_BYTES       (__riscv_xlen / 8)
#else
#  error "wire: unsupported architecture"
#endif

/* Total byte size of the register file sent in RSP 'g' response. */
#define WIRE_REG_BUF_BYTES  (WIRE_REG_COUNT * WIRE_REG_BYTES)
/* RSP hex encoding: 2 chars per byte. */
#define WIRE_REG_HEX_CHARS  (WIRE_REG_BUF_BYTES * 2)

#endif /* WIRE_ARCH_H */
