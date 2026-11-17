/* ============================================================================
 * NexusOS — User Processes: isolation & multitasking — Phase 54 (impl)
 * ============================================================================
 * See uproc.h for the design and the scheduling model. This file owns:
 *   - the loaders (ELF / PE32 / Mach-O) targeting a per-process address space,
 *   - the Win32 import stub-page builder,
 *   - the cooperative + timer-driven scheduler core,
 *   - process lifecycle (spawn / foreground-wait / exit / kill / reap),
 *   - the exception->kill path for user faults,
 *   - installing the built-in demo binaries into the RAM filesystem.
 * ============================================================================ */

#include "uproc.h"
#include "paging.h"
#include "memory.h"
#include "process.h"
#include "scheduler.h"      /* context_switch */
#include "gdt.h"            /* tss_set_kernel_stack */
#include "heap.h"
#include "string.h"
#include "vga.h"
#include "vfs.h"
#include "ramfs.h"
#include "elf.h"
#include "pe.h"
#include "macho.h"
#include "users.h"
#include "usertests.h"

extern volatile uint32_t system_ticks;

/* win32.c: map an import (dll,func) to a shim id + stdcall arg byte count. */
extern int win32_lookup_shim(const char* dll, const char* func, uint32_t* argbytes_out);

volatile int uproc_in_syscall = 0;

/* --------------------------------------------------------------------------
 * Scheduler state
 * -------------------------------------------------------------------------- */
static int      sched_enabled = 0;
static uint32_t quantum = 0;
#define QUANTUM_TICKS 2        /* switch after ~2 timer ticks (~110ms) in ring3 */

/* --------------------------------------------------------------------------
 * Small serial + console helpers (segfault message goes to both)
 * -------------------------------------------------------------------------- */
#include "port.h"
static void ser_putc(char c) { while ((port_byte_in(0x3FD) & 0x20) == 0); port_byte_out(0x3F8, c); }
static void ser_str(const char* s) { for (; *s; s++) ser_putc(*s); }
static void ser_hex(uint32_t v) {
    const char* h = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4) ser_putc(h[(v >> i) & 0xF]);
}

/* --------------------------------------------------------------------------
 * ring-3 bootstrap: the first thing a freshly-created user process runs (as
 * the context_switch "return" target). Loads its address space, points the
 * TSS at its kernel stack, and irets to ring 3. Never returns.
 * -------------------------------------------------------------------------- */
static void uproc_bootstrap(void) {
    process_t* p = process_get_current();
    tss_set_kernel_stack(p->stack_top);
    paging_switch(p->cr3);
    uint32_t entry  = p->eip;
    uint32_t ustack = p->user_stack;
    __asm__ volatile(
        "cli\n"
        "mov $0x23, %%ax\n"
        "mov %%ax, %%ds\n mov %%ax, %%es\n mov %%ax, %%fs\n mov %%ax, %%gs\n"
        "push $0x23\n"               /* SS  = user data */
        "push %0\n"                  /* ESP = user stack */
        "pushf\n pop %%eax\n or $0x200, %%eax\n push %%eax\n"  /* EFLAGS, IF=1 */
        "push $0x1B\n"               /* CS  = user code (RPL 3) */
        "push %1\n"                  /* EIP = entry */
        "iret\n"
        :: "r"(ustack), "r"(entry) : "eax", "memory");
    __builtin_unreachable();
}

/* --------------------------------------------------------------------------
 * Scheduler core
 * -------------------------------------------------------------------------- */
static int slot_of(process_t* p) {
    process_t* t = process_get_table();
    return (int)(p - t);
}

/* Round-robin after `cur`. user_only excludes process 0 (used by the timer so
 * the kernel thread is never entered involuntarily). */
static process_t* pick_next(process_t* cur, int user_only) {
    process_t* t = process_get_table();
    int start = slot_of(cur);
    for (int k = 1; k <= MAX_PROCESSES; k++) {
        process_t* p = &t[(start + k) % MAX_PROCESSES];
        if (p == cur) continue;
        if (p->state != PROC_READY) continue;
        if (user_only && !p->is_user) continue;
        return p;
    }
    return NULL;
}

static void wake_sleepers(void) {
    process_t* t = process_get_table();
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (t[i].state == PROC_BLOCKED && t[i].wake_tick &&
            system_ticks >= t[i].wake_tick) {
            t[i].state = PROC_READY;
            t[i].wake_tick = 0;
        }
    }
}

/* Switch to `next`. Sets its address space + kernel stack first, then swaps
 * kernel stacks via context_switch. Returns (into `cur`) only when something
 * later switches back to cur. */
static void do_switch(process_t* next) {
    process_t* cur = process_get_current();
    if (next == cur) return;
    if (cur->state == PROC_RUNNING) cur->state = PROC_READY;
    next->state = PROC_RUNNING;
    next->ticks++;
    process_set_current(next);
    paging_switch(next->cr3);
    tss_set_kernel_stack(next->stack_top);
    context_switch(&cur->esp, next->esp);
}

void sched_set_enabled(int on) { sched_enabled = on; }

void sched_yield(void) {
    if (!sched_enabled) return;
    wake_sleepers();
    process_t* cur = process_get_current();
    if (!cur) return;
    process_t* next = pick_next(cur, 0);   /* may return to the kernel thread */
    if (next) do_switch(next);
}

void uproc_timer_tick(struct registers* regs) {
    if (!sched_enabled) return;
    if ((regs->cs & 3) != 3) return;       /* only pre-empt ring-3 (user) code */
    if (++quantum < QUANTUM_TICKS) return;
    quantum = 0;
    wake_sleepers();
    process_t* cur = process_get_current();
    process_t* next = pick_next(cur, 1);   /* user processes only */
    if (next) do_switch(next);
}

/* --------------------------------------------------------------------------
 * Exit / fault / kill / reap
 * -------------------------------------------------------------------------- */
static process_t* kernel_thread(void) {
    process_t* t = process_get_table();
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (t[i].state != PROC_UNUSED && !t[i].is_user) return &t[i];
    return NULL;
}

/* Mark current user process dead and switch away forever. */
static void die_and_switch(uint32_t code) {
    process_t* cur = process_get_current();
    cur->exit_code = code;
    cur->state = PROC_TERMINATED;
    cur->needs_reap = true;
    uproc_in_syscall = 0;
    process_t* next = pick_next(cur, 0);
    if (!next) next = kernel_thread();
    if (next) do_switch(next);
    /* No runnable task at all: should never happen (the kernel thread is
     * always resumable). Halt defensively. */
    for (;;) __asm__ volatile("hlt");
}

int uproc_sys_exit(uint32_t code) {
    process_t* cur = process_get_current();
    if (cur && cur->is_user) die_and_switch(code);   /* never returns */
    return 0;
}

bool uproc_fault(struct registers* regs, uint32_t fault_addr) {
    bool from_user = (regs->cs & 3) == 3;
    process_t* cur = process_get_current();
    bool kern_for_user = (!from_user) && uproc_in_syscall > 0 && cur && cur->is_user;
    if (!from_user && !kern_for_user) return false;  /* genuine kernel fault */
    if (!cur || !cur->is_user) return false;

    /* Re-enable interrupts before we resume the (voluntarily-parked) kernel
     * thread: this handler was entered through an interrupt gate (IF=0) and
     * the kernel thread must come back with interrupts on. */
    __asm__ volatile("sti");

    ser_str("\nSegmentation fault (pid "); ser_hex(cur->pid);
    ser_str(", addr 0x"); ser_hex(fault_addr); ser_str(")\n");

    vga_print_color("\n  Segmentation fault", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
    vga_print(" (pid ");
    char b[12]; int_to_str((int)cur->pid, b); vga_print(b);
    vga_print(", addr ");
    hex_to_str(fault_addr, b); vga_print(b);   /* hex_to_str adds its own 0x */
    vga_print(")\n");

    die_and_switch(0xEE);     /* never returns */
    return true;
}

int uproc_kill(uint32_t pid, uint32_t caller_uid) {
    process_t* t = process_get_table();
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (t[i].pid == pid && t[i].state != PROC_UNUSED &&
            t[i].state != PROC_TERMINATED) {
            if (!t[i].is_user) return -2;                 /* not a user process */
            if (caller_uid != UID_ROOT && t[i].uid != caller_uid) return -2;
            if (&t[i] == process_get_current()) die_and_switch(9);  /* self-kill */
            t[i].state = PROC_TERMINATED;
            t[i].needs_reap = true;
            t[i].exit_code = 9;
            return 0;
        }
    }
    return -1;
}

void uproc_reap(void) {
    process_t* t = process_get_table();
    process_t* cur = process_get_current();
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (t[i].state != PROC_TERMINATED || !t[i].needs_reap) continue;
        if (&t[i] == cur) continue;                        /* never reap self */
        if (t[i].cr3 && t[i].cr3 != paging_kernel_cr3()) as_destroy(t[i].cr3);
        t[i].cr3 = 0;
        if (t[i].stack_base) { kfree((void*)t[i].stack_base); t[i].stack_base = 0; }
        t[i].needs_reap = false;
        t[i].state = PROC_UNUSED;
    }
}

/* --------------------------------------------------------------------------
 * Loaders (into a per-process address space; CR3 == target `cr3` on entry)
 * -------------------------------------------------------------------------- */

/* Map `pages` fresh high-pool frames at [vbase, vbase + pages*4KB) and zero
 * them. CR3 is the target space, so we can memset through the user vaddr. */
static bool map_user_range(uint32_t cr3, uint32_t vbase, uint32_t pages, uint32_t flags) {
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t va = vbase + i * PAGE_SIZE;
        if (as_check_user(cr3, va, 1)) continue;   /* already mapped */
        uint32_t ph = upmm_alloc_frame();
        if (!ph) return false;
        as_map_page(cr3, va, ph, flags);
        memset((void*)va, 0, PAGE_SIZE);
    }
    return true;
}

static bool load_elf(uint32_t cr3, const uint8_t* data, uint32_t size, uint32_t* entry_out) {
    if (!elf_validate(data, size)) return false;
    const elf32_ehdr_t* h = (const elf32_ehdr_t*)data;
    const elf32_phdr_t* ph = (const elf32_phdr_t*)(data + h->e_phoff);

    if (!paging_user_window_ok(h->e_entry, 1)) return false;
    for (int i = 0; i < h->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) continue;
        if (ph[i].p_offset > size || ph[i].p_filesz > size - ph[i].p_offset ||
            ph[i].p_filesz > ph[i].p_memsz ||
            !paging_user_window_ok(ph[i].p_vaddr, ph[i].p_memsz)) return false;
    }
    for (int i = 0; i < h->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) continue;
        uint32_t va = ph[i].p_vaddr;
        uint32_t pstart = va & 0xFFFFF000;
        uint32_t pend   = (va + ph[i].p_memsz + 0xFFF) & 0xFFFFF000;
        if (!map_user_range(cr3, pstart, (pend - pstart) / PAGE_SIZE,
                            PAGE_WRITABLE | PAGE_USER)) return false;
        if (ph[i].p_filesz) memcpy((void*)va, data + ph[i].p_offset, ph[i].p_filesz);
    }
    *entry_out = h->e_entry;
    return true;
}

static bool load_macho(uint32_t cr3, const uint8_t* data, uint32_t size, uint32_t* entry_out) {
    macho_info_t info;
    if (!macho_get_info(data, size, &info) || info.entry == 0) return false;
    if (!paging_user_window_ok(info.entry, 1)) return false;
    const mach_header_t* h = (const mach_header_t*)data;
    uint32_t off = sizeof(mach_header_t);
    for (uint32_t i = 0; i < h->ncmds; i++) {
        if (size - off < sizeof(load_command_t)) break;
        const load_command_t* lc = (const load_command_t*)(data + off);
        if (lc->cmdsize < sizeof(load_command_t) || lc->cmdsize > size - off) break;
        uint32_t this_off = off;
        off += lc->cmdsize;
        if (lc->cmd != LC_SEGMENT || lc->cmdsize < sizeof(segment_command_t)) continue;
        const segment_command_t* seg = (const segment_command_t*)lc;
        if (strncmp(seg->segname, "__PAGEZERO", 16) == 0 || seg->vmsize == 0) continue;
        if (!paging_user_window_ok(seg->vmaddr, seg->vmsize)) return false;
        uint32_t pstart = seg->vmaddr & 0xFFFFF000;
        uint32_t pend   = (seg->vmaddr + seg->vmsize + 0xFFF) & 0xFFFFF000;
        if (!map_user_range(cr3, pstart, (pend - pstart) / PAGE_SIZE,
                            PAGE_WRITABLE | PAGE_USER)) return false;
        uint32_t fsize = seg->filesize;
        if (fsize > seg->vmsize) fsize = seg->vmsize;
        if (fsize && seg->fileoff <= size && fsize <= size - seg->fileoff)
            memcpy((void*)seg->vmaddr, data + seg->fileoff, fsize);
        (void)this_off;
    }
    *entry_out = info.entry;
    return true;
}

static bool pe_in_image(uint32_t rva, uint32_t len, uint32_t img_size) {
    return rva <= img_size && len <= img_size - rva;
}

/* resolves an import's by-name string for the unresolved-import diagnostic */
static const char* fn_safe(uint32_t base, uint32_t ent, uint32_t imgsz) {
    if (!(ent & 0x80000000) && pe_in_image(ent, 3, imgsz)) return (const char*)(base + ent + 2);
    return "<ordinal>";
}

static bool load_pe(uint32_t cr3, const uint8_t* data, uint32_t size, uint32_t* entry_out) {
    if (!pe_validate(data, size)) return false;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)data;
    const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)(data + dos->e_lfanew);
    const IMAGE_OPTIONAL_HEADER32* oh = &nt->OptionalHeader;
    uint32_t base = oh->ImageBase, imgsz = oh->SizeOfImage;
    uint32_t nsec = nt->FileHeader.NumberOfSections;
    uint32_t sec_off = dos->e_lfanew + 4 + sizeof(IMAGE_FILE_HEADER) +
                       nt->FileHeader.SizeOfOptionalHeader;

    if (!paging_user_window_ok(base, imgsz) || oh->SizeOfHeaders > imgsz ||
        oh->SizeOfHeaders > size || sec_off > size ||
        nsec * sizeof(IMAGE_SECTION_HEADER) > size - sec_off ||
        oh->AddressOfEntryPoint >= imgsz) return false;

    uint32_t pstart = base & 0xFFFFF000;
    uint32_t pend   = (base + imgsz + 0xFFF) & 0xFFFFF000;
    if (!map_user_range(cr3, pstart, (pend - pstart) / PAGE_SIZE,
                        PAGE_WRITABLE | PAGE_USER)) return false;

    memcpy((void*)base, data, oh->SizeOfHeaders);
    const IMAGE_SECTION_HEADER* sec = (const IMAGE_SECTION_HEADER*)(data + sec_off);
    for (uint32_t i = 0; i < nsec; i++) {
        uint32_t rva = sec[i].VirtualAddress, fsz = sec[i].SizeOfRawData;
        uint32_t fptr = sec[i].PointerToRawData, vsz = sec[i].Misc.VirtualSize;
        uint32_t csz = (fsz > vsz && vsz) ? vsz : fsz;
        if (csz && fptr <= size && csz <= size - fptr && pe_in_image(rva, csz, imgsz))
            memcpy((void*)(base + rva), data + fptr, csz);
    }

    /* Build the per-process Win32 import stub page. Each IAT entry points at a
     * tiny read-only, user-accessible stub: mov eax,(0x8000|id); int 0x80;
     * ret <argbytes>. The kernel dispatches id to the win32.c shim (syscall.c).*/
    uint32_t stub_va = UPROC_STUB_BASE;
    if (!map_user_range(cr3, stub_va, 1, PAGE_WRITABLE | PAGE_USER)) return false;
    uint8_t* stub = (uint8_t*)stub_va;
    uint32_t stub_idx = 0;                       /* 16 bytes per stub, 256 max */

    if (oh->NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IMPORT) {
        uint32_t drva = oh->DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
        for (; drva && pe_in_image(drva, sizeof(IMAGE_IMPORT_DESCRIPTOR), imgsz);
             drva += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
            const IMAGE_IMPORT_DESCRIPTOR* d = (const IMAGE_IMPORT_DESCRIPTOR*)(base + drva);
            if (d->Name == 0) break;
            if (d->Name >= imgsz) continue;
            const char* dll = (const char*)(base + d->Name);
            uint32_t lt = d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk;
            for (uint32_t at = d->FirstThunk;
                 pe_in_image(lt, 4, imgsz) && pe_in_image(at, 4, imgsz); lt += 4, at += 4) {
                uint32_t ent = *(const uint32_t*)(base + lt);
                if (ent == 0) break;
                uint32_t target = 0;
                if (!(ent & 0x80000000) && pe_in_image(ent, 3, imgsz) && stub_idx < 256) {
                    const char* fn = (const char*)(base + ent + 2);
                    uint32_t argbytes = 0;
                    int id = win32_lookup_shim(dll, fn, &argbytes);
                    if (id >= 0) {
                        uint8_t* s = stub + stub_idx * 16;
                        uint32_t eax = 0x8000u | (uint32_t)id;
                        s[0] = 0xB8;                                   /* mov eax, imm32 */
                        s[1] = (uint8_t)eax; s[2] = (uint8_t)(eax >> 8);
                        s[3] = (uint8_t)(eax >> 16); s[4] = (uint8_t)(eax >> 24);
                        s[5] = 0xCD; s[6] = 0x80;                      /* int 0x80 */
                        s[7] = 0xC2;                                   /* ret imm16 */
                        s[8] = (uint8_t)argbytes; s[9] = (uint8_t)(argbytes >> 8);
                        for (int k = 10; k < 16; k++) s[k] = 0x90;     /* nop pad */
                        target = stub_va + stub_idx * 16;
                        stub_idx++;
                    }
                }
                if (target == 0) {
                    vga_print_color("  [win32] unresolved import ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
                    vga_print((char*)dll); vga_print("!"); vga_print((char*)fn_safe(base, ent, imgsz)); vga_print("\n");
                }
                *(uint32_t*)(base + at) = target;
            }
        }
    }
    /* stub page is executable data the program must not modify: make read-only. */
    as_set_flags(cr3, stub_va, PAGE_USER);

    *entry_out = base + oh->AddressOfEntryPoint;
    return true;
}

/* --------------------------------------------------------------------------
 * Spawn
 * -------------------------------------------------------------------------- */
typedef enum { FMT_NONE, FMT_ELF, FMT_PE, FMT_MACHO } binfmt_t;

static binfmt_t detect_fmt(const uint8_t* d, uint32_t n) {
    if (elf_validate(d, n))   return FMT_ELF;
    if (pe_validate(d, n))    return FMT_PE;
    if (macho_validate(d, n)) return FMT_MACHO;
    return FMT_NONE;
}

static process_t* uproc_spawn(const uint8_t* data, uint32_t size, const char* name) {
    uproc_reap();                                   /* free stale slots first */

    binfmt_t fmt = detect_fmt(data, size);
    if (fmt == FMT_NONE) {
        vga_print_color("  Not a runnable binary (ELF/PE/Mach-O).\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        return NULL;
    }

    uint32_t cr3 = as_create();
    if (!cr3) { vga_print_color("  Out of memory (address space).\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK)); return NULL; }

    uint32_t saved = paging_current_cr3();
    paging_switch(cr3);

    bool ok = true;
    /* user stack */
    if (!map_user_range(cr3, UPROC_STACK_TOP - UPROC_STACK_PAGES * PAGE_SIZE,
                        UPROC_STACK_PAGES, PAGE_WRITABLE | PAGE_USER)) ok = false;

    uint32_t entry = 0;
    if (ok) {
        if (fmt == FMT_ELF)        ok = load_elf(cr3, data, size, &entry);
        else if (fmt == FMT_PE)    ok = load_pe(cr3, data, size, &entry);
        else                       ok = load_macho(cr3, data, size, &entry);
    }

    /* Set up the initial user stack frame per format (CR3 is the new space). */
    uint32_t user_esp = UPROC_STACK_TOP;
    if (ok) {
        uint32_t* sp = (uint32_t*)UPROC_STACK_TOP;
        if (fmt == FMT_PE) {
            *(--sp) = 1; *(--sp) = 0; *(--sp) = 0; *(--sp) = 0; /* WinMain args */
            *(--sp) = 0;                                        /* return addr  */
        } else if (fmt == FMT_MACHO) {
            *(--sp) = 0; *(--sp) = 0; *(--sp) = 1; *(--sp) = 0; /* envp,argv,argc,ret */
        } else {
            *(--sp) = 0;                                        /* fake return  */
        }
        user_esp = (uint32_t)sp;
    }

    paging_switch(saved);

    if (!ok) { as_destroy(cr3);
        vga_print_color("  Loader rejected the binary.\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        return NULL; }

    process_t* p = process_create(name, NULL);
    if (!p) { as_destroy(cr3); return NULL; }

    p->is_user    = true;
    p->cr3        = cr3;
    p->eip        = entry;
    p->user_stack = user_esp;
    p->brk_base   = UPROC_HEAP_BASE;
    p->brk_cur    = UPROC_HEAP_BASE;
    p->needs_reap = false;

    /* Kernel stack: initial frame so context_switch lands in uproc_bootstrap. */
    uint32_t* ksp = (uint32_t*)p->stack_top;
    *(--ksp) = (uint32_t)uproc_bootstrap;   /* ret target */
    *(--ksp) = 0;                            /* ebp */
    *(--ksp) = 0;                            /* ebx */
    *(--ksp) = 0;                            /* esi */
    *(--ksp) = 0;                            /* edi */
    p->esp = (uint32_t)ksp;
    p->state = PROC_READY;
    return p;
}

/* --------------------------------------------------------------------------
 * sleep / sbrk back-ends
 * -------------------------------------------------------------------------- */
int uproc_sys_sleep(uint32_t ms) {
    process_t* cur = process_get_current();
    if (!cur) return -1;
    uint32_t ticks = (ms * 18u) / 1000u;    /* PIT ~18.2Hz; keep ticks/18 = sec */
    if (ticks == 0) ticks = 1;
    cur->wake_tick = system_ticks + ticks;
    cur->state = PROC_BLOCKED;
    sched_yield();                          /* give up CPU until woken */
    return 0;
}

int uproc_sys_sbrk(int32_t inc) {
    process_t* cur = process_get_current();
    if (!cur || !cur->is_user) return -1;
    uint32_t old = cur->brk_cur;
    if (inc == 0) return (int)old;
    if (inc < 0) { cur->brk_cur = old; return (int)old; }  /* shrink: lazy */
    uint32_t nb = old + (uint32_t)inc;
    if (nb < old || nb > cur->brk_base + UPROC_HEAP_MAX) return -1;
    for (uint32_t va = old & 0xFFFFF000; va < nb; va += PAGE_SIZE) {
        if (as_check_user(cur->cr3, va, 1)) continue;
        uint32_t ph = upmm_alloc_frame();
        if (!ph) return -1;
        as_map_page(cur->cr3, va, ph, PAGE_WRITABLE | PAGE_USER);
        memset((void*)va, 0, PAGE_SIZE);    /* CR3 == cur's space in-syscall */
    }
    cur->brk_cur = nb;
    return (int)old;
}

/* --------------------------------------------------------------------------
 * Public run / init
 * -------------------------------------------------------------------------- */
static process_t* slot_by_pid(uint32_t pid) {
    process_t* t = process_get_table();
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (t[i].pid == pid && t[i].state != PROC_UNUSED) return &t[i];
    return NULL;
}

int uproc_run_path(const char* path, bool background) {
    fs_node_t* root = vfs_get_root();
    fs_node_t* node = path ? vfs_finddir(root, path) : NULL;
    if (!node || node->size == 0) {
        vga_print_color("  No such file: ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print(path ? path : "(null)"); vga_print("\n");
        return -1;
    }
    uint8_t* buf = (uint8_t*)kmalloc(node->size);
    if (!buf) return -1;
    int32_t rd = vfs_read(node, 0, node->size, buf);
    if (rd <= 0) { kfree(buf); return -1; }

    process_t* p = uproc_spawn(buf, (uint32_t)rd, path);
    kfree(buf);
    if (!p) return -1;

    uint32_t pid = p->pid;
    vga_print_color("  Started ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print(path); vga_print(" (pid ");
    char b[12]; int_to_str((int)pid, b); vga_print(b);
    vga_print(background ? ", background)\n" : ")\n");

    if (background) return (int)pid;

    /* Foreground: yield until the child exits, then reap and report. */
    while (1) {
        process_t* q = slot_by_pid(pid);
        if (!q || q->state == PROC_TERMINATED) break;
        sched_yield();
    }
    process_t* q = slot_by_pid(pid);
    uint32_t code = q ? q->exit_code : 0;
    uproc_reap();
    vga_print_color("  [exit ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    int_to_str((int)code, b); vga_print(b); vga_print("]\n");
    return (int)code;
}

void uproc_init(void) {
    /* Register the running context as the kernel thread (process 0). */
    process_t* kt = process_get_current();
    if (kt) {
        kt->is_user    = false;
        kt->cr3        = paging_kernel_cr3();
        kt->needs_reap = false;
        kt->state      = PROC_RUNNING;
    }

    /* Install the built-in demo binaries into the RAM filesystem. */
    fs_node_t* root = vfs_get_root();
    int n = 0;
    if (root) {
        for (int i = 0; i < USERTEST_COUNT; i++) {
            const usertest_bin_t* u = &usertest_bins[i];
            if (vfs_finddir(root, (char*)u->name)) continue;
            fs_node_t* node = ramfs_create((char*)u->name, FS_FILE);
            if (node) { vfs_write(node, 0, u->len, u->data); n++; }
        }
    }

    sched_set_enabled(1);

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("User processes: per-AS isolation + multitasking, ");
    char b[12]; int_to_str(n, b); vga_print(b);
    vga_print(" demo bins\n");
}
