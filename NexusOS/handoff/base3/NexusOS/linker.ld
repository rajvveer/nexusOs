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

    /* Phase 55 (kernel W^X): .text and .rodata are write-protected at run time
     * by paging_protect_kernel(), one 4KB page at a time, so the read-only
     * image [_text_start, _data_start) must not share a page with .data. The
     * ALIGN(4096) before .data costs at most 4KB of padding in kernel.bin
     * (boot2 loads a flat image, so the padding is harmless). */
    _text_start = .;

    /* Code section */
    .text : {
        *(.text .text.*)
    }
    _text_end = .;

    /* Read-only data */
    .rodata : {
        *(.rodata*)
    }
    _rodata_end = .;

    /* Initialized data — first writable byte of the image, page-aligned */
    . = ALIGN(4096);
    _data_start = .;
    .data : {
        *(.data .data.*)
    }

    /* Image end. boot2.asm loads at most KERNEL_SECTORS*512 = 1015808 bytes
     * (to 0x1F8000, below the heap at 0x200000); pmm_init reserves up to here. */
    _kernel_end = .;

    /* Unwind tables (~57KB) and compiler idents: nothing in a freestanding
     * kernel uses them, and they would otherwise sit inside the loaded image. */
    /DISCARD/ : { *(.eh_frame) *(.comment) }

    /* Place .bss at 8MB, clear of the kernel image, heap (2-3MB) and boot stack
     * (below 4MB). .bss is uninitialized so it is not written to the binary;
     * only the symbols _bss_start/_bss_end matter. kernel_entry.asm zeroes it at
     * boot and pmm_init reserves [_bss_start, _bss_end). */
    . = 0x800000;
    .bss (NOLOAD) : {
        _bss_start = .;
        *(COMMON)
        *(.bss)
        _bss_end = .;
    }
}
