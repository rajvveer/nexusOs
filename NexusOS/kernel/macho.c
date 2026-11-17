/* ============================================================================
 * NexusOS — Mach-O Binary Loader (Implementation) — Phase 37
 * ============================================================================
 * Parses 32-bit i386 Mach-O executables, maps their segments into memory,
 * locates the entry point, and transitions to ring-3 execution. The macOS
 * analogue of pe.c (Phase 34). Dynamic linking via dyld is out of scope, so
 * this loads static/simple executables; the Cocoa/Core Foundation API the
 * binaries call lives in cocoa.c.
 * ============================================================================ */

#include "macho.h"
#include "elf.h"     /* user_range_ok(): shared user load window */
#include "vga.h"
#include "string.h"
#include "memory.h"
#include "paging.h"
#include "process.h"
#include "gdt.h"

/* User-mode layout — distinct stack top from elf.c/pe.c to avoid clashes. */
#define USER_STACK_TOP    0x00C70000   /* Phase 57: 16KB stack just under the ELF/PE stack
                                          (0xC80000) and the 12.5MB load window; linker.ld
                                          asserts .bss ends below 0xC60000 */
#define USER_STACK_PAGES  4            /* 16KB user stack */

static int macho_loaded_count = 0;     /* successful launches, for status */

/* --------------------------------------------------------------------------
 * macho_validate: 32-bit i386 Mach-O executable?
 * -------------------------------------------------------------------------- */
bool macho_validate(const uint8_t* data, uint32_t size) {
    if (!data || size < sizeof(mach_header_t)) return false;
    const mach_header_t* h = (const mach_header_t*)data;

    /* Host (little-endian) 32-bit only: 64-bit is unsupported, and byte-swapped
     * MH_CIGAM files would need every field swapped, which we don't do. */
    if (h->magic != MH_MAGIC) return false;
    if (h->cputype != CPU_TYPE_X86) return false;
    if (h->filetype != MH_EXECUTE) return false;
    /* Load commands must fit within the file. */
    if ((uint64_t)sizeof(mach_header_t) + h->sizeofcmds > size) return false;
    return true;
}

/* --------------------------------------------------------------------------
 * macho_get_info: walk load commands and summarize
 * -------------------------------------------------------------------------- */
bool macho_get_info(const uint8_t* data, uint32_t size, macho_info_t* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!macho_validate(data, size)) return false;

    const mach_header_t* h = (const mach_header_t*)data;
    out->valid    = true;
    out->cputype  = h->cputype;
    out->filetype = h->filetype;
    out->ncmds    = h->ncmds;

    uint32_t text_vmaddr = 0;
    uint32_t off = sizeof(mach_header_t);

    for (uint32_t i = 0; i < h->ncmds; i++) {
        if (size - off < sizeof(load_command_t)) break;      /* off <= size holds */
        const load_command_t* lc = (const load_command_t*)(data + off);
        uint32_t cmdsize = lc->cmdsize;
        if (cmdsize < sizeof(load_command_t) || cmdsize > size - off) break;

        switch (lc->cmd) {
            case LC_SEGMENT: {
                if (cmdsize >= sizeof(segment_command_t)) {
                    const segment_command_t* seg = (const segment_command_t*)lc;
                    out->nsegments++;
                    if (strncmp(seg->segname, "__TEXT", 16) == 0)
                        text_vmaddr = seg->vmaddr;
                }
                break;
            }
            case LC_UNIXTHREAD: {
                /* i386 thread state: registers begin at +16, eip is reg #10. */
                if (cmdsize >= 16 + 11 * 4)
                    out->entry = *(const uint32_t*)(data + off + 16 + 10 * 4);
                break;
            }
            case LC_MAIN: {
                if (cmdsize >= sizeof(entry_point_command_t)) {
                    const entry_point_command_t* ec = (const entry_point_command_t*)lc;
                    out->entry = text_vmaddr + (uint32_t)ec->entryoff;
                }
                break;
            }
            case LC_LOAD_DYLIB: {
                out->ndylibs++;
                if (out->first_dylib[0] == '\0' && cmdsize >= sizeof(dylib_command_t)) {
                    const dylib_command_t* dl = (const dylib_command_t*)lc;
                    if (dl->name_offset < cmdsize) {
                        const char* nm = (const char*)(data + off + dl->name_offset);
                        uint32_t maxn = cmdsize - dl->name_offset;
                        uint32_t n = 0;
                        while (n < maxn && n < sizeof(out->first_dylib) - 1 && nm[n]) {
                            out->first_dylib[n] = nm[n]; n++;
                        }
                        out->first_dylib[n] = '\0';
                    }
                }
                break;
            }
            default: break;
        }
        off += cmdsize;
    }
    return true;
}

/* --------------------------------------------------------------------------
 * macho_exec: map segments and jump to the entry point in ring 3
 * -------------------------------------------------------------------------- */
int macho_exec(const uint8_t* data, uint32_t size, const char* name) {
    macho_info_t info;
    if (!macho_get_info(data, size, &info)) {
        vga_print_color("[!!] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print("Mach-O: invalid executable\n");
        return -1;
    }
    if (info.entry == 0) {
        vga_print_color("[!!] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print("Mach-O: no entry point (LC_MAIN/LC_UNIXTHREAD)\n");
        return -1;
    }

    const mach_header_t* h = (const mach_header_t*)data;
    uint32_t flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;

    /* The entry must lie in the user load window (elf.h). */
    if (!user_range_ok(info.entry, 1)) goto bad;

    /* Two passes over the load commands: pass 0 validates every loadable
     * segment (whole vm range inside the user load window, so it can't
     * overlay kernel memory) before pass 1 maps and copies anything. */
    for (int pass = 0; pass < 2; pass++) {
        uint32_t off = sizeof(mach_header_t);
        for (uint32_t i = 0; i < h->ncmds; i++) {
            if (size - off < sizeof(load_command_t)) break;      /* off <= size holds */
            const load_command_t* lc = (const load_command_t*)(data + off);
            if (lc->cmdsize < sizeof(load_command_t) || lc->cmdsize > size - off) break;
            off += lc->cmdsize;

            if (lc->cmd != LC_SEGMENT || lc->cmdsize < sizeof(segment_command_t)) continue;
            const segment_command_t* seg = (const segment_command_t*)lc;
            /* Skip the null-guard segment and anything zero-sized. */
            if (strncmp(seg->segname, "__PAGEZERO", 16) == 0 || seg->vmsize == 0) continue;
            if (!user_range_ok(seg->vmaddr, seg->vmsize)) goto bad;
            if (pass == 0) continue;

            uint32_t page_start = seg->vmaddr & 0xFFFFF000;
            uint32_t page_end   = (seg->vmaddr + seg->vmsize + 0xFFF) & 0xFFFFF000;
            for (uint32_t page = page_start; page < page_end; page += 4096) {
                uint32_t phys = pmm_alloc_page();
                if (phys == 0) {
                    vga_print_color("[!!] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
                    vga_print("Mach-O: out of memory mapping segment\n");
                    return -1;
                }
                paging_map_page(page, phys, flags);
            }
            memset((void*)seg->vmaddr, 0, seg->vmsize);

            /* filesize > vmsize is clamped; the file range must not wrap. */
            uint32_t fsize = seg->filesize;
            if (fsize > seg->vmsize) fsize = seg->vmsize;
            if (fsize > 0 && seg->fileoff <= size && fsize <= size - seg->fileoff)
                memcpy((void*)seg->vmaddr, data + seg->fileoff, fsize);
        }
    }

    /* User stack. */
    for (int p = 0; p < USER_STACK_PAGES; p++) {
        uint32_t stack_page = USER_STACK_TOP - (p + 1) * 4096;
        uint32_t phys = pmm_alloc_page();
        if (phys == 0) return -1;
        paging_map_page(stack_page, phys, flags);
    }

    process_t* proc = process_create(name, NULL);
    if (!proc) return -1;
    proc->eip = info.entry;
    proc->is_user = true;

    /* BSD/Mach C runtime expects argc/argv on the stack; push a minimal frame. */
    uint32_t* user_esp = (uint32_t*)USER_STACK_TOP;
    *(--user_esp) = 0;            /* envp[0] = NULL */
    *(--user_esp) = 0;            /* argv[0] = NULL */
    *(--user_esp) = 1;            /* argc = 1 */
    *(--user_esp) = 0;            /* fake return address */
    proc->user_stack = (uint32_t)user_esp;

    tss_set_kernel_stack(proc->stack_top);
    macho_loaded_count++;

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Mach-O: loaded \"");
    vga_print(name);
    vga_print("\" entry=");
    char buf[12]; hex_to_str(info.entry, buf); vga_print(buf);   /* adds its own 0x */
    vga_print("\n");

    __asm__ volatile(
        "cli\n"
        "mov $0x23, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "push $0x23\n"          /* SS */
        "push %0\n"             /* ESP */
        "pushf\n"
        "pop %%eax\n"
        "or $0x200, %%eax\n"    /* IF */
        "push %%eax\n"          /* EFLAGS */
        "push $0x1B\n"          /* CS */
        "push %1\n"             /* EIP */
        "iret\n"
        :
        : "r"(proc->user_stack), "r"(info.entry)
        : "eax"
    );
    return 0;

bad:
    vga_print_color("[!!] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
    vga_print("Mach-O: segment/entry out of bounds (user window 0xC80000-0xD00000)\n");
    return -1;
}

void macho_init(void) {
    macho_loaded_count = 0;
    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Mach-O loader initialized (i386)\n");
}

const char* macho_get_status(void) {
    return "Mach-O i386 loader: LC_SEGMENT, LC_UNIXTHREAD/LC_MAIN, LC_LOAD_DYLIB";
}
