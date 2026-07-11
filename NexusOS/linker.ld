/* ============================================================================
 * NexusOS Linker Script
 * ============================================================================
 * Defines the memory layout of the kernel binary.
 * The kernel is loaded at 0x100000 (1MB) by the Stage 2 bootloader, which
 * bounce-buffers BIOS reads below 640KB and copies chunks high in brief
 * protected-mode hops (the old in-place load at 0x10000 capped the kernel
 * at 576KB — the VGA hole at 0xA0000).
 * ============================================================================ */

ENTRY(_start)

SECTIONS
{
    /* Kernel starts at 0x100000 (1MB; heap begins at 0x200000) */
    . = 0x100000;

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
