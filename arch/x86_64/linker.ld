OUTPUT_FORMAT(elf64-x86-64)
ENTRY(start32)
/* Higher-half kernel (-mcmodel=kernel): linked at KERNEL_VMA + physical, loaded at physical 0x100000.
 * entry.asm's 32-bit boot code uses physical addresses (PA()). Keep in sync with native.h/entry.asm. */
KERNEL_VMA = 0xFFFFFFFF80000000;
SECTIONS {
    . = KERNEL_VMA + 0x100000;
    __text_start = .;
    .text : AT(ADDR(.text) - KERNEL_VMA) { KEEP(*(.entry)) *(.text .text.*) }
    . = ALIGN(4096);
    __text_end = .;
    __rodata_start = .;
    .rodata : AT(ADDR(.rodata) - KERNEL_VMA) { *(.rodata .rodata.*) }
    . = ALIGN(4096);
    __rodata_end = .;
    .data : AT(ADDR(.data) - KERNEL_VMA) { *(.data .data.*) }
    . = ALIGN(4096);
    /* /proc/kallsyms table (procext.c; gen_kallsyms.py). Placed after .data so adding it never moves a text/rodata/
     * data symbol; the only symbols it shifts are in .bss (by a page-aligned, known amount the generator corrects). */
    __kallsyms_start = .;
    .kallsyms : AT(ADDR(.kallsyms) - KERNEL_VMA) { KEEP(*(.kallsyms)) }
    . = ALIGN(4096);
    __kallsyms_end = .;
    __bss_start = .;
    .bss (NOLOAD) : AT(ADDR(.bss) - KERNEL_VMA) { *(.bss .bss.*) *(COMMON) }
    . = ALIGN(4096);
    __bss_end = .;
    /* protect_kernel() gives text/rodata, data/BSS and the boot-stack guard 4 KiB permissions through the
     * two page tables covering the first 4 MiB; text and rodata must lie there. BSS may continue up to 6 MiB:
     * physical 4-6 MiB is boot_pd[2], one 2 MiB page that protect_kernel leaves writable and NX (the right
     * permissions for BSS) in both the direct map and the kernel image map. Below 16 MiB nothing else uses RAM
     * (the page pool starts there); the UEFI loader reserves [0x100000, __bss_end - KERNEL_VMA) and keeps its
     * own buffers out of [0x100000, KERNEL_LIMIT = 6 MiB) (uefi/bootx64.c), build.py checks the same bound. */
    ASSERT(__rodata_end - KERNEL_VMA <= 0x400000, "kernel text/rodata exceed the 4 KiB-protected first 4 MiB")
    ASSERT(kernel_stack_guard - KERNEL_VMA < 0x400000, "boot stack guard page must lie in the first 4 MiB")
    ASSERT(__bss_end - KERNEL_VMA <= 0x600000, "kernel image and BSS exceed 6 MiB (uefi/bootx64.c KERNEL_LIMIT)")
    /DISCARD/ : { *(.eh_frame*) *(.comment*) *(.note*) }
}
