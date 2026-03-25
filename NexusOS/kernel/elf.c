/* ============================================================================
 * NexusOS — ELF32 Binary Loader (Implementation) — Phase 20
 * ============================================================================
 * Parses ELF32 executables and shared objects, loads PT_LOAD segments into
 * user-mode pages, sets up user stack, and executes via iret to ring 3.
 * Phase 32: Added ET_DYN support for shared library loading.
 * ============================================================================ */

#include "elf.h"
#include "vga.h"
#include "string.h"
#include "memory.h"
#include "paging.h"
#include "process.h"
#include "gdt.h"

/* User-mode memory layout */
#define USER_STACK_TOP    0x00C00000  /* 12MB — user stack top */
#define USER_STACK_PAGES  4           /* 16KB user stack */
#define USER_LOAD_BASE    0x00400000  /* 4MB — default ELF load base */

/* --------------------------------------------------------------------------
 * elf_validate: Check if data is a valid ELF32 i386 executable
 * -------------------------------------------------------------------------- */
bool elf_validate(const uint8_t* data, uint32_t size) {
    if (!data || size < sizeof(elf32_ehdr_t)) return false;

    const elf32_ehdr_t* hdr = (const elf32_ehdr_t*)data;

    /* Check magic */
    if (hdr->e_ident_magic != ELF_MAGIC) return false;

    /* Check 32-bit */
    if (hdr->e_ident_class != ELFCLASS32) return false;

    /* Check executable or shared object */
    if (hdr->e_type != ET_EXEC && hdr->e_type != ET_DYN) return false;

    /* Check i386 */
    if (hdr->e_machine != EM_386) return false;

    /* Check program headers exist */
    if (hdr->e_phnum == 0 || hdr->e_phoff == 0) return false;

    /* Check program headers fit in file */
    if (hdr->e_phoff + hdr->e_phnum * sizeof(elf32_phdr_t) > size) return false;

    return true;
}

/* --------------------------------------------------------------------------
 * elf_exec: Load and execute an ELF32 binary
 * -------------------------------------------------------------------------- */
int elf_exec(const uint8_t* data, uint32_t size, const char* name) {
    if (!elf_validate(data, size)) {
        vga_print_color("[!!] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print("ELF: invalid binary\n");
        return -1;
    }

    const elf32_ehdr_t* hdr = (const elf32_ehdr_t*)data;
    const elf32_phdr_t* phdrs = (const elf32_phdr_t*)(data + hdr->e_phoff);

    /* Load PT_LOAD segments into memory */
    for (int i = 0; i < hdr->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        if (phdrs[i].p_memsz == 0) continue;

        uint32_t vaddr = phdrs[i].p_vaddr;
        uint32_t memsz = phdrs[i].p_memsz;
        uint32_t filesz = phdrs[i].p_filesz;
        uint32_t offset = phdrs[i].p_offset;

        /* Validate file offset */
        if (offset + filesz > size) return -1;

        /* Allocate and map pages for this segment */
        uint32_t page_start = vaddr & 0xFFFFF000;
        uint32_t page_end = (vaddr + memsz + 0xFFF) & 0xFFFFF000;
        uint32_t flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;

        for (uint32_t page = page_start; page < page_end; page += 4096) {
            uint32_t phys = pmm_alloc_page();
            if (phys == 0) return -1;
            paging_map_page(page, phys, flags);
        }

        /* Copy file data */
        if (filesz > 0) {
            memcpy((void*)vaddr, data + offset, filesz);
        }

        /* Zero BSS (memsz - filesz) */
        if (memsz > filesz) {
            memset((void*)(vaddr + filesz), 0, memsz - filesz);
        }
    }

    /* Allocate user stack */
    for (int p = 0; p < USER_STACK_PAGES; p++) {
        uint32_t stack_page = USER_STACK_TOP - (p + 1) * 4096;
        uint32_t phys = pmm_alloc_page();
        if (phys == 0) return -1;
        paging_map_page(stack_page, phys, PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
    }

    /* Create process */
    process_t* proc = process_create(name, NULL);
    if (!proc) return -1;

    proc->eip = hdr->e_entry;
    proc->is_user = true;
    proc->user_stack = USER_STACK_TOP;

    /* Set TSS kernel stack for ring transition */
    tss_set_kernel_stack(proc->stack_top);

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("ELF: loaded \"");
    vga_print(name);
    vga_print("\" entry=0x");
    char buf[12];
    int_to_str(hdr->e_entry, buf);
    vga_print(buf);
    vga_print("\n");

    /* Jump to user mode via iret:
     * Push: SS, ESP, EFLAGS (with IF), CS, EIP
     * User code segment = 0x1B (GDT entry 3, RPL=3)
     * User data segment = 0x23 (GDT entry 4, RPL=3) */
    __asm__ volatile(
        "cli\n"
        "mov $0x23, %%ax\n"    /* User data selector */
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "push $0x23\n"          /* SS = user data */
        "push %0\n"             /* ESP = user stack top */
        "pushf\n"
        "pop %%eax\n"
        "or $0x200, %%eax\n"   /* Set IF (interrupts enabled) */
        "push %%eax\n"          /* EFLAGS */
        "push $0x1B\n"          /* CS = user code (RPL=3) */
        "push %1\n"             /* EIP = entry point */
        "iret\n"
        :
        : "r"(USER_STACK_TOP), "r"(hdr->e_entry)
        : "eax"
    );

    return 0; /* Never reached */
}

/* --------------------------------------------------------------------------
 * elf_init: Initialize ELF subsystem
 * -------------------------------------------------------------------------- */
void elf_init(void) {
    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("ELF loader initialized\n");
}
