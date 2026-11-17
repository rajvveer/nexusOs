/* ============================================================================
 * NexusOS — PE32 Binary Loader (Implementation) — Phase 34
 * ============================================================================
 * Parses Windows PE32 executables, maps the image into memory, 
 * resolves imports to local Win32 shim functions, sets up the user stack, 
 * and transitions to ring 3 execution.
 * ============================================================================ */

#include "pe.h"
#include "elf.h"     /* user_range_ok(): shared user load window */
#include "vga.h"
#include "string.h"
#include "memory.h"
#include "paging.h"
#include "process.h"
#include "gdt.h"

/* Reference to win32 layer's import resolver */
uint32_t win32_resolve_import(const char* dll_name, const char* func_name);

/* User-mode memory layout (similar to elf.c but different base to avoid conflict) */
#define USER_STACK_TOP    0x00C00000  /* 12MB — user stack top */
#define USER_STACK_PAGES  4           /* 16KB user stack */

/* Load base for PE if the default ImageBase (0x400000) conflicts.
   For now, we honor PE ImageBase, so we map exactly where it asks to be —
   provided the whole image fits the user load window (elf.h). */

/* True if [rva, rva+len) lies inside an image of img_size bytes (overflow-safe). */
static bool pe_in_image(uint32_t rva, uint32_t len, uint32_t img_size) {
    return rva <= img_size && len <= img_size - rva;
}

/* --------------------------------------------------------------------------
 * pe_validate: Check if data is a valid i386 PE32 executable
 * -------------------------------------------------------------------------- */
bool pe_validate(const uint8_t* data, uint32_t size) {
    if (!data || size < sizeof(IMAGE_DOS_HEADER)) return false;

    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)data;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

    if (dos->e_lfanew > size || sizeof(IMAGE_NT_HEADERS32) > size - dos->e_lfanew) return false;

    const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)(data + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386) return false;
    
    if (!(nt->FileHeader.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE)) return false;
    if (!(nt->FileHeader.Characteristics & IMAGE_FILE_32BIT_MACHINE)) return false;

    return true;
}

/* --------------------------------------------------------------------------
 * pe_exec: Load and execute a PE32 binary
 * -------------------------------------------------------------------------- */
int pe_exec(const uint8_t* data, uint32_t size, const char* name) {
    if (!pe_validate(data, size)) {
        vga_print_color("[!!] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print("PE: invalid executable\n");
        return -1;
    }

    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)data;
    const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)(data + dos->e_lfanew);

    const IMAGE_OPTIONAL_HEADER32* oh = &nt->OptionalHeader;
    uint32_t img_base = oh->ImageBase;
    uint32_t img_size = oh->SizeOfImage;
    uint32_t nsec     = nt->FileHeader.NumberOfSections;
    uint32_t sec_off  = dos->e_lfanew + 4 + sizeof(IMAGE_FILE_HEADER) +
                        nt->FileHeader.SizeOfOptionalHeader;   /* no wrap: e_lfanew validated */

    /* Validate before mapping anything (all overflow-safe): the whole image in
     * the user load window (so it can't overlay kernel/heap/stack/.bss/back
     * buffer), headers within both file and image, the section table within
     * the file, and the entry point inside the image. */
    if (!user_range_ok(img_base, img_size) || oh->SizeOfHeaders > img_size ||
        oh->SizeOfHeaders > size || sec_off > size ||
        nsec * sizeof(IMAGE_SECTION_HEADER) > size - sec_off ||
        oh->AddressOfEntryPoint >= img_size) {
        vga_print_color("[!!] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print("PE: image out of bounds (user window 4MB-8MB)\n");
        return -1;
    }

    /* Map pages for the entire image in one go, backing with physical pages */
    uint32_t page_start = img_base & 0xFFFFF000;
    uint32_t page_end = (img_base + img_size + 0xFFF) & 0xFFFFF000;
    uint32_t flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;

    for (uint32_t page = page_start; page < page_end; page += 4096) {
        uint32_t phys = pmm_alloc_page();
        if (phys == 0) {
            vga_print_color("[!!] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
            vga_print("PE: out of memory allocating image\n");
            return -1;
        }
        paging_map_page(page, phys, flags);
    }
    
    /* Zero out the entire mapped image area first */
    memset((void*)page_start, 0, page_end - page_start);

    /* Copy PE headers to Image Base */
    memcpy((void*)img_base, data, oh->SizeOfHeaders);

    /* Load sections (a section whose file or image range is out of bounds is
     * skipped; its pages stay zeroed) */
    const IMAGE_SECTION_HEADER* sections = (const IMAGE_SECTION_HEADER*)(data + sec_off);

    for (uint32_t i = 0; i < nsec; i++) {
        uint32_t rva   = sections[i].VirtualAddress;
        uint32_t fsize = sections[i].SizeOfRawData;
        uint32_t fptr  = sections[i].PointerToRawData;
        uint32_t vsize = sections[i].Misc.VirtualSize;
        uint32_t copy_size = (fsize > vsize) ? vsize : fsize;

        if (copy_size && fptr <= size && copy_size <= size - fptr &&
            pe_in_image(rva, copy_size, img_size))
            memcpy((void*)(img_base + rva), data + fptr, copy_size);
        /* BSS (uninitialized data) is implicitly 0 because we memset the mapped region. */
    }

    /* Process Import Address Table (IAT). Every RVA (descriptor, name, thunk
     * tables, hint/name entries) is bounds-checked against the image. */
    if (oh->NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IMPORT) {
        uint32_t drva = oh->DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
        for (; drva && pe_in_image(drva, sizeof(IMAGE_IMPORT_DESCRIPTOR), img_size);
             drva += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
            const IMAGE_IMPORT_DESCRIPTOR* desc = (const IMAGE_IMPORT_DESCRIPTOR*)(img_base + drva);
            if (desc->Name == 0) break;
            if (desc->Name >= img_size) continue;
            const char* dll_name = (const char*)(img_base + desc->Name);

            uint32_t lt = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
            for (uint32_t at = desc->FirstThunk;
                 pe_in_image(lt, 4, img_size) && pe_in_image(at, 4, img_size); lt += 4, at += 4) {
                uint32_t ent = *(const uint32_t*)(img_base + lt);
                if (ent == 0) break;
                /* Import by name (IMAGE_IMPORT_BY_NAME: 2-byte hint + name).
                 * Import by ordinal is unsupported in our simple shim. */
                if (!(ent & 0x80000000) && pe_in_image(ent, 3, img_size))
                    *(uint32_t*)(img_base + at) =
                        win32_resolve_import(dll_name, (const char*)(img_base + ent + 2));
            }
        }
    }

    /* Allocate user stack */
    for (int p = 0; p < USER_STACK_PAGES; p++) {
        uint32_t stack_page = USER_STACK_TOP - (p + 1) * 4096;
        uint32_t phys = pmm_alloc_page();
        if (phys == 0) return -1;
        paging_map_page(stack_page, phys, flags);
    }

    uint32_t entry_point = img_base + nt->OptionalHeader.AddressOfEntryPoint;

    /* Create process */
    process_t* proc = process_create(name, NULL);
    if (!proc) return -1;

    proc->eip = entry_point;
    proc->is_user = true;
    proc->user_stack = USER_STACK_TOP;

    /* Set up stack arguments for WinMain if needed? Easiest to push some dummy args.
       Typically standard is: int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) */
    uint32_t* user_esp = (uint32_t*)proc->user_stack;
    
    /* Push arguments right to left */
    *(--user_esp) = 1;     /* nCmdShow (SW_SHOWNORMAL) */
    *(--user_esp) = 0;     /* lpCmdLine (NULL) */
    *(--user_esp) = 0;     /* hPrevInstance (NULL) */
    *(--user_esp) = img_base; /* hInstance */
    *(--user_esp) = 0;     /* Return address (if it returns, it faults) */

    proc->user_stack = (uint32_t)user_esp;

    /* Set kernel stack for ring transition */
    tss_set_kernel_stack(proc->stack_top);

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("PE: loaded \"");
    vga_print(name);
    vga_print("\" entry=");
    char buf[12];
    hex_to_str(entry_point, buf);       /* adds its own 0x */
    vga_print(buf);
    vga_print("\n");

    /* Transition to Ring 3 */
    __asm__ volatile(
        "cli\n"
        "mov $0x23, %%ax\n"     /* User data selector */
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "push $0x23\n"          /* SS */
        "push %0\n"             /* ESP */
        "pushf\n"
        "pop %%eax\n"
        "or $0x200, %%eax\n"    /* Enable interrupts (IF) */
        "push %%eax\n"          /* EFLAGS */
        "push $0x1B\n"          /* CS */
        "push %1\n"             /* EIP */
        "iret\n"
        :
        : "r"(proc->user_stack), "r"(entry_point)
        : "eax"
    );

    return 0;
}

void pe_init(void) {
    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("PE loader initialized\n");
}
