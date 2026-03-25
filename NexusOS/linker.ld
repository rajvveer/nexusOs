/* ============================================================================
 * NexusOS Linker Script
 * ============================================================================
 * Defines the memory layout of the kernel binary.
 * The kernel is loaded at 0x10000 by the Stage 2 bootloader.
 * ============================================================================ */

ENTRY(_start)

SECTIONS
{
    /* Kernel starts at 0x10000 (64KB, above bootloader) */
    . = 0x10000;

    _kernel_start = .;

    /* Code section */
    .text : {
        *(.text)
    }

    /* Read-only data */
    .rodata : {
        *(.rodata*)
    }

    /* Initialized data */
    .data : {
        *(.data)
    }

    /* Uninitialized data (zeroed) */
    .bss : {
        _bss_start = .;
        *(COMMON)
        *(.bss)
        _bss_end = .;
    }

    _kernel_end = .;
}
