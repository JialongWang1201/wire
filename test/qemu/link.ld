/* test/qemu/link.ld — Linker script for mps2-an385 (Cortex-M3)
 *
 * Memory map:
 *   FLASH  0x00000000  256 KB  (vector table + code + rodata)
 *   RAM    0x20000000  256 KB  (stack + .data + .bss)
 *
 * SPDX-License-Identifier: MIT
 */

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 256K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 256K
}

/* Stack at the top of RAM. */
_estack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
    /* ── Vector table must be at the very start of FLASH ── */
    .isr_vector :
    {
        KEEP(*(.isr_vector))
    } > FLASH

    /* ── Code and read-only data ── */
    .text :
    {
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
        _etext = .;
    } > FLASH

    /* ── Initialised data: stored in FLASH, copied to RAM at startup ── */
    _sidata = LOADADDR(.data);
    .data :
    {
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    /* ── Zero-initialised data ── */
    .bss :
    {
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    /DISCARD/ :
    {
        *(.ARM.exidx*)
        *(.ARM.extab*)
    }
}
