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
    /* Kernel starts at 0x100000 (1MB). Phase 57: the heap moved to 4MB, so the
     * image may grow through the old 2-3MB heap range up to the 3MB guard band. */
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

    /* Image end. boot2.asm loads at most KERNEL_SECTORS*512 = 2816*512 =
     * 1441792 bytes (to 0x260000; Phase 57 raised it from 1984 sectors /
     * 1015808 bytes once the heap left 2MB). The image must stay below the
     * 16KB guard band at 0x300000 (memory.h KERNEL_IMAGE_LIMIT = 0x2C0000);
     * build.bat / Makefile enforce the byte limit, pmm_init reserves 0-4MB. */
    _kernel_end = .;

    /* Unwind tables (~57KB) and compiler idents: nothing in a freestanding
     * kernel uses them, and they would otherwise sit inside the loaded image. */
    /DISCARD/ : { *(.eh_frame) *(.comment) }

    /* Place .bss at 8MB, clear of the kernel image, boot stack (below 4MB) and
     * the 4MB heap (4-8MB). .bss is uninitialized so it is not written to the binary;
     * only the symbols _bss_start/_bss_end matter. kernel_entry.asm zeroes it at
     * boot and pmm_init reserves [_bss_start, _bss_end). */
    . = 0x800000;
    .bss (NOLOAD) : {
        _bss_start = .;
        *(COMMON)
        *(.bss)
        _bss_end = .;
    }

    /* Phase 57: the legacy single-address-space loaders (elf.h) overlay fixed
     * identity pages at 0xC80000-0xD00000 with their 16KB user stacks just
     * below (0xC6C000-0xC80000). .bss grows upward from 8MB towards them, so
     * fail the link loudly instead of letting a bigger static buffer silently
     * end up underneath a user image/stack. */
    ASSERT(_bss_end <= 0x00C60000,
           ".bss reaches the legacy user stacks/load window (elf.h USER_LOAD_BASE): shrink static buffers or move the window")

    /* Browser arenas have their own reserved supervisor-only region. */
    . = 0x02000000;
    .app_bss (NOLOAD) : {
        _app_bss_start = .;
        *(.app_bss)
        _app_bss_end = .;
    }
    ASSERT(_app_bss_end <= 0x02800000, "app buffers exceed the 40MB identity map")
}
