/* ============================================================================
 * NexusOS — ELF Dynamic Linker (Implementation) — Phase 32
 * ============================================================================
 * Loads ELF shared objects (.so), resolves symbols, applies relocations,
 * and provides dlopen/dlsym/dlclose API.
 * ============================================================================ */

#include "dynlink.h"
#include "elf.h"
#include "vga.h"
#include "string.h"
#include "memory.h"
#include "paging.h"
#include "heap.h"
#include "vfs.h"
#include "ramfs.h"

/* ---- Library registry ---- */
static loaded_lib_t libraries[DYNLINK_MAX_LIBS];
static int next_load_slot = 0;

/* ---- Built-in symbol table ---- */
static builtin_sym_t builtins[DYNLINK_MAX_SYMBOLS];
static int builtin_count = 0;

/* --------------------------------------------------------------------------
 * dynlink_register_builtin: Add a kernel symbol to the built-in table
 * -------------------------------------------------------------------------- */
void dynlink_register_builtin(const char* name, uint32_t addr) {
    if (builtin_count >= DYNLINK_MAX_SYMBOLS) return;
    builtins[builtin_count].name = name;
    builtins[builtin_count].addr = addr;
    builtin_count++;
}

/* --------------------------------------------------------------------------
 * resolve_builtin: Look up a symbol in the built-in table
 * -------------------------------------------------------------------------- */
static uint32_t resolve_builtin(const char* name) {
    for (int i = 0; i < builtin_count; i++) {
        if (strcmp(builtins[i].name, name) == 0) {
            return builtins[i].addr;
        }
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * resolve_in_library: Look up a symbol within a specific loaded library
 * -------------------------------------------------------------------------- */
static uint32_t resolve_in_library(const loaded_lib_t* lib, const char* name) {
    if (!lib || !lib->active || !lib->symtab || !lib->strtab) return 0;

    for (uint32_t i = 0; i < lib->sym_count; i++) {
        const elf32_sym_t* sym = &lib->symtab[i];

        /* Skip undefined or local symbols */
        if (sym->st_shndx == 0) continue;  /* SHN_UNDEF */
        if (ELF32_ST_BIND(sym->st_info) == STB_LOCAL) continue;

        /* Get symbol name from string table */
        if (sym->st_name == 0) continue;
        if (sym->st_name >= lib->strtab_size) continue;

        const char* sym_name = lib->strtab + sym->st_name;
        if (strcmp(sym_name, name) == 0) {
            return lib->load_base + sym->st_value;
        }
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * dynlink_resolve_symbol: Search all libraries + builtins for a symbol
 * -------------------------------------------------------------------------- */
uint32_t dynlink_resolve_symbol(const char* name) {
    if (!name || !*name) return 0;

    /* 1. Search built-in symbols first */
    uint32_t addr = resolve_builtin(name);
    if (addr) return addr;

    /* 2. Search loaded libraries */
    for (int i = 0; i < DYNLINK_MAX_LIBS; i++) {
        if (!libraries[i].active) continue;
        addr = resolve_in_library(&libraries[i], name);
        if (addr) return addr;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * dynlink_lib_symbol: Find symbol in a specific library by handle
 * -------------------------------------------------------------------------- */
uint32_t dynlink_lib_symbol(uint32_t handle, const char* name) {
    if (handle == 0 || handle > DYNLINK_MAX_LIBS) return 0;
    loaded_lib_t* lib = &libraries[handle - 1];
    if (!lib->active) return 0;
    return resolve_in_library(lib, name);
}

/* --------------------------------------------------------------------------
 * apply_relocations: Process REL relocations for a loaded library
 * -------------------------------------------------------------------------- */
static void apply_relocations(loaded_lib_t* lib, elf32_rel_t* rels,
                               uint32_t size) {
    if (!rels || size == 0) return;

    uint32_t count = size / sizeof(elf32_rel_t);

    for (uint32_t i = 0; i < count; i++) {
        elf32_rel_t* rel = &rels[i];
        uint32_t sym_idx = ELF32_R_SYM(rel->r_info);
        uint32_t type    = ELF32_R_TYPE(rel->r_info);

        /* Compute the address to patch */
        uint32_t* target = (uint32_t*)(lib->load_base + rel->r_offset);

        /* Resolve symbol if needed */
        uint32_t sym_addr = 0;
        if (sym_idx != 0 && lib->symtab && lib->strtab) {
            if (sym_idx < lib->sym_count) {
                const elf32_sym_t* sym = &lib->symtab[sym_idx];
                if (sym->st_name > 0 && sym->st_name < lib->strtab_size) {
                    const char* sym_name = lib->strtab + sym->st_name;
                    /* Try to resolve in other libraries and builtins */
                    sym_addr = dynlink_resolve_symbol(sym_name);
                    /* If not found elsewhere, check if defined in this lib */
                    if (!sym_addr && sym->st_shndx != 0) {
                        sym_addr = lib->load_base + sym->st_value;
                    }
                }
            }
        }

        /* Apply relocation based on type */
        switch (type) {
            case R_386_NONE:
                break;

            case R_386_32:
                /* S + A: add symbol address to existing value */
                *target += sym_addr;
                break;

            case R_386_PC32:
                /* S + A - P: PC-relative */
                *target += sym_addr - (uint32_t)target;
                break;

            case R_386_GLOB_DAT:
            case R_386_JMP_SLOT:
                /* S: set to symbol address */
                *target = sym_addr;
                break;

            case R_386_RELATIVE:
                /* B + A: base + existing value */
                *target += lib->load_base;
                break;

            default:
                /* Unknown relocation type — skip */
                break;
        }
    }
}

/* --------------------------------------------------------------------------
 * parse_dynamic_section: Extract info from PT_DYNAMIC segment
 * -------------------------------------------------------------------------- */
static void parse_dynamic_section(loaded_lib_t* lib, uint32_t dyn_vaddr,
                                   uint32_t dyn_size) {
    elf32_dyn_t* dyn = (elf32_dyn_t*)(lib->load_base + dyn_vaddr);
    uint32_t count = dyn_size / sizeof(elf32_dyn_t);

    uint32_t symtab_addr = 0, strtab_addr = 0, strsz = 0;
    uint32_t rel_addr = 0, relsz = 0;
    uint32_t jmprel_addr = 0, pltrelsz = 0;
    uint32_t hash_addr = 0;
    uint32_t syment = sizeof(elf32_sym_t);

    for (uint32_t i = 0; i < count; i++) {
        if (dyn[i].d_tag == DT_NULL) break;

        switch (dyn[i].d_tag) {
            case DT_SYMTAB:   symtab_addr = dyn[i].d_val;  break;
            case DT_STRTAB:   strtab_addr = dyn[i].d_val;  break;
            case DT_STRSZ:    strsz = dyn[i].d_val;        break;
            case DT_REL:      rel_addr = dyn[i].d_val;     break;
            case DT_RELSZ:    relsz = dyn[i].d_val;        break;
            case DT_JMPREL:   jmprel_addr = dyn[i].d_val;  break;
            case DT_PLTRELSZ: pltrelsz = dyn[i].d_val;     break;
            case DT_HASH:     hash_addr = dyn[i].d_val;    break;
            case DT_SYMENT:   syment = dyn[i].d_val;       break;
            default: break;
        }
    }

    /* Map addresses relative to load base */
    if (symtab_addr) {
        lib->symtab = (elf32_sym_t*)(lib->load_base + symtab_addr);
    }
    if (strtab_addr) {
        lib->strtab = (const char*)(lib->load_base + strtab_addr);
        lib->strtab_size = strsz;
    }

    /* Estimate symbol count from hash table or strtab/symtab gap */
    if (hash_addr) {
        uint32_t* hash = (uint32_t*)(lib->load_base + hash_addr);
        lib->sym_count = hash[1]; /* nchain = number of symbols */
    } else if (strtab_addr > symtab_addr && syment > 0) {
        lib->sym_count = (strtab_addr - symtab_addr) / syment;
    } else {
        lib->sym_count = 64; /* Conservative fallback */
    }

    /* REL relocations */
    if (rel_addr) {
        lib->rel = (elf32_rel_t*)(lib->load_base + rel_addr);
        lib->relsz = relsz;
    }

    /* PLT relocations */
    if (jmprel_addr) {
        lib->pltrel = (elf32_rel_t*)(lib->load_base + jmprel_addr);
        lib->pltrelsz = pltrelsz;
    }
}

/* --------------------------------------------------------------------------
 * dynlink_load_library: Load an ELF shared object into memory
 * -------------------------------------------------------------------------- */
uint32_t dynlink_load_library(const uint8_t* data, uint32_t size,
                               const char* name) {
    if (!data || size < sizeof(elf32_ehdr_t) || !name) return 0;

    /* Check if already loaded */
    for (int i = 0; i < DYNLINK_MAX_LIBS; i++) {
        if (libraries[i].active && strcmp(libraries[i].name, name) == 0) {
            libraries[i].ref_count++;
            return (uint32_t)(i + 1);
        }
    }

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < DYNLINK_MAX_LIBS; i++) {
        if (!libraries[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        vga_print_color("[!!] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print("dynlink: max libraries reached\n");
        return 0;
    }

    /* Validate ELF header */
    const elf32_ehdr_t* hdr = (const elf32_ehdr_t*)data;
    if (hdr->e_ident_magic != ELF_MAGIC) {
        vga_print_color("[!!] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print("dynlink: bad ELF magic\n");
        return 0;
    }
    if (hdr->e_ident_class != ELFCLASS32 || hdr->e_machine != EM_386) {
        vga_print_color("[!!] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print("dynlink: not i386 ELF32\n");
        return 0;
    }
    /* Accept both ET_DYN (shared object) and ET_EXEC */
    if (hdr->e_type != ET_DYN && hdr->e_type != ET_EXEC) {
        vga_print_color("[!!] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print("dynlink: not a shared object\n");
        return 0;
    }

    /* Determine load base address */
    uint32_t load_base = DYNLINK_SO_BASE + (uint32_t)next_load_slot * DYNLINK_SO_STRIDE;
    next_load_slot++;

    /* Initialize library descriptor */
    loaded_lib_t* lib = &libraries[slot];
    memset(lib, 0, sizeof(loaded_lib_t));
    strncpy(lib->name, name, DYNLINK_LIB_NAME_MAX - 1);
    lib->name[DYNLINK_LIB_NAME_MAX - 1] = '\0';
    lib->load_base = load_base;
    lib->ref_count = 1;

    /* Load PT_LOAD segments */
    const elf32_phdr_t* phdrs = (const elf32_phdr_t*)(data + hdr->e_phoff);
    uint32_t dyn_vaddr = 0, dyn_size = 0;
    uint32_t max_addr = 0;

    for (int i = 0; i < hdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_LOAD) {
            uint32_t vaddr = load_base + phdrs[i].p_vaddr;
            uint32_t memsz = phdrs[i].p_memsz;
            uint32_t filesz = phdrs[i].p_filesz;
            uint32_t offset = phdrs[i].p_offset;

            /* Validate */
            if (offset + filesz > size) {
                vga_print_color("[!!] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
                vga_print("dynlink: segment out of bounds\n");
                return 0;
            }

            /* Allocate and map pages */
            uint32_t page_start = vaddr & 0xFFFFF000;
            uint32_t page_end = (vaddr + memsz + 0xFFF) & 0xFFFFF000;
            uint32_t flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;

            for (uint32_t page = page_start; page < page_end; page += 4096) {
                uint32_t phys = pmm_alloc_page();
                if (phys == 0) {
                    vga_print_color("[!!] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
                    vga_print("dynlink: out of memory\n");
                    return 0;
                }
                paging_map_page(page, phys, flags);
                /* Zero the page */
                memset((void*)page, 0, 4096);
            }

            /* Copy file data */
            if (filesz > 0) {
                memcpy((void*)vaddr, data + offset, filesz);
            }

            /* Track highest address for load_size */
            uint32_t end = phdrs[i].p_vaddr + memsz;
            if (end > max_addr) max_addr = end;

        } else if (phdrs[i].p_type == PT_DYNAMIC) {
            dyn_vaddr = phdrs[i].p_vaddr;
            dyn_size = phdrs[i].p_filesz;
        }
    }

    lib->load_size = max_addr;

    /* Parse .dynamic section if present */
    if (dyn_vaddr && dyn_size) {
        parse_dynamic_section(lib, dyn_vaddr, dyn_size);
    }

    /* Apply relocations */
    if (lib->rel && lib->relsz > 0) {
        apply_relocations(lib, lib->rel, lib->relsz);
    }
    if (lib->pltrel && lib->pltrelsz > 0) {
        apply_relocations(lib, lib->pltrel, lib->pltrelsz);
    }

    /* Mark active */
    lib->active = true;

    /* Log */
    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("dynlink: loaded \"");
    vga_print(name);
    vga_print("\" at 0x");
    char buf[12];
    hex_to_str(load_base, buf);
    vga_print(buf);
    vga_print(" (");
    int_to_str(max_addr, buf);
    vga_print(buf);
    vga_print(" bytes)\n");

    return (uint32_t)(slot + 1);
}

/* --------------------------------------------------------------------------
 * dynlink_unload_library: Unload a shared library by handle
 * -------------------------------------------------------------------------- */
int dynlink_unload_library(uint32_t handle) {
    if (handle == 0 || handle > DYNLINK_MAX_LIBS) return -1;
    loaded_lib_t* lib = &libraries[handle - 1];
    if (!lib->active) return -1;

    lib->ref_count--;
    if (lib->ref_count > 0) return 0; /* Still referenced */

    /* Unmap pages */
    if (lib->load_size > 0) {
        uint32_t page_start = lib->load_base & 0xFFFFF000;
        uint32_t page_end = (lib->load_base + lib->load_size + 0xFFF) & 0xFFFFF000;
        for (uint32_t page = page_start; page < page_end; page += 4096) {
            paging_unmap_page(page);
        }
    }

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("dynlink: unloaded \"");
    vga_print(lib->name);
    vga_print("\"\n");

    memset(lib, 0, sizeof(loaded_lib_t));
    return 0;
}

/* --------------------------------------------------------------------------
 * Accessor functions
 * -------------------------------------------------------------------------- */
int dynlink_get_lib_count(void) {
    int count = 0;
    for (int i = 0; i < DYNLINK_MAX_LIBS; i++) {
        if (libraries[i].active) count++;
    }
    return count;
}

const loaded_lib_t* dynlink_get_libs(void) {
    return libraries;
}

int dynlink_get_builtin_count(void) {
    return builtin_count;
}

const builtin_sym_t* dynlink_get_builtins(void) {
    return builtins;
}

/* --------------------------------------------------------------------------
 * dl_open: POSIX-like dlopen — load library from VFS
 * -------------------------------------------------------------------------- */
uint32_t dl_open(const char* name, int flags) {
    (void)flags;
    if (!name) return 0;

    /* Look up file in VFS */
    fs_node_t* root = vfs_get_root();
    fs_node_t* node = vfs_finddir(root, name);
    if (!node || node->size == 0) {
        vga_print_color("[!!] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print("dlopen: file not found: ");
        vga_print(name);
        vga_print("\n");
        return 0;
    }

    /* Read file into buffer */
    uint8_t* buf = (uint8_t*)kmalloc(node->size);
    if (!buf) return 0;

    int32_t rd = vfs_read(node, 0, node->size, buf);
    if (rd <= 0) {
        kfree(buf);
        return 0;
    }

    /* Load the library */
    uint32_t handle = dynlink_load_library(buf, (uint32_t)rd, name);
    kfree(buf);
    return handle;
}

/* --------------------------------------------------------------------------
 * dl_sym: POSIX-like dlsym
 * -------------------------------------------------------------------------- */
uint32_t dl_sym(uint32_t handle, const char* name) {
    if (handle == 0) {
        /* Handle 0 = search all */
        return dynlink_resolve_symbol(name);
    }
    return dynlink_lib_symbol(handle, name);
}

/* --------------------------------------------------------------------------
 * dl_close: POSIX-like dlclose
 * -------------------------------------------------------------------------- */
int dl_close(uint32_t handle) {
    return dynlink_unload_library(handle);
}

/* --------------------------------------------------------------------------
 * dynlink_init: Initialize the dynamic linker subsystem
 * -------------------------------------------------------------------------- */
void dynlink_init(void) {
    memset(libraries, 0, sizeof(libraries));
    next_load_slot = 0;
    builtin_count = 0;

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Dynamic linker initialized (");
    char buf[12];
    int_to_str(DYNLINK_MAX_LIBS, buf);
    vga_print(buf);
    vga_print(" slots, dlopen/dlsym/dlclose)\n");
}
