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

    _kernel_end = .;

    /* Place .bss far away from VGA text buffer (0xB8000) and fb_back (0x500000).
     * .bss is uninitialized so it is not written to the binary; only the
     * symbols _bss_start/_bss_end matter. kernel_entry.asm zeroes it at boot. */
    . = 0x800000;
    .bss (NOLOAD) : {
        _bss_start = .;
        *(COMMON)
        *(.bss)
        _bss_end = .;
    }
}
