/* User segmentation for 32-bit code in 64-bit processes (Wine's WoW64), with Linux x86-64 semantics.
 *
 * - Every CPU runs on its own copy of the GDT (Linux layout, native.h): 0x23 compatibility-mode code,
 *   0x2b data, 0x33 64-bit code, 0x50 the CPU's LDT, 0x63/0x6b/0x73 the running thread's TLS slots.
 *   Wine 10 (dlls/ntdll/unix/signal_x86_64.c alloc_fs_sel) gets its 32-bit %fs selector from the IA-32
 *   set_thread_area call through int $0x80 and keeps one TLS slot per thread, so the slots are written
 *   into the CPU's GDT at every context switch, before the selectors are reloaded.
 * - modify_ldt: one 8192-entry LDT per address space (allocated on the first write, updated in place,
 *   shared by threads, copied by fork, dropped by exec). A CPU loads it (lldt) when a task of that
 *   space is switched in or returns to ring 3; a write bumps ldt_generation and IPIs every CPU that runs
 *   the space (TLB shootdown) so that, as on Linux, the LDT and cached DS/ES are current on all CPUs
 *   before modify_ldt returns.
 * - Context switches save DS/ES/FS/GS and the FS/GS bases; a selector > 3 supplies its own base,
 *   otherwise the saved base is written (Linux load_seg_legacy). arch_prctl(ARCH_SET_FS/GS) loads a
 *   null selector first.
 * - int $0x80 runs a small IA-32 table (set/get_thread_area, modify_ldt, exit, exit_group, getpid,
 *   gettid; everything else -ENOSYS). SYSCALL from compatibility mode returns -ENOSYS through sysretl
 *   (Linux without IA-32 emulation, AMD; Intel CPUs raise #UD -> SIGILL); SYSENTER is disabled
 *   (SYSENTER_CS = 0: #GP -> SIGSEGV on Intel, #UD -> SIGILL in long mode on AMD).
 * - A kernel load of a stale user selector retries with a null selector (Linux loadsegment; FS/GS via
 *   USER_DS first, so their base is zero on AMD as well); an iretq
 *   that faults on a stale user CS/SS becomes a fault of the user context (Linux bad-IRET fixup).
 * 16-bit LDT/TLS segments are refused with EINVAL (Linux with CONFIG_X86_16BIT=n: there is no espfix64). */
#include "native.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

#define LDT_ENTRIES 8192
#define LDT_BYTES (LDT_ENTRIES * 8ULL)
#define LDT_SELECTOR 0x50
#define TLS_MIN 12
#define TLS_MAX 14
#define TASK_SIZE_MAX 0x7FFFFFFFF000ULL
#define MSR_FS_BASE 0xC0000100
#define MSR_GS_BASE 0xC0000101
#define BASE_UNKNOWN (~0ULL) /* cpu->loaded_fs/gs after a selector load supplied the base */

struct user_desc { uint32_t entry_number, base_addr, limit, flags; };
enum { UD_32BIT = 1, UD_READ_EXEC_ONLY = 8, UD_LIMIT_PAGES = 16, UD_NOT_PRESENT = 32, UD_USEABLE = 64, UD_LM = 128 };
_Static_assert(sizeof(struct user_desc) == 16, "struct user_desc ABI");

extern uint64_t gdt64[];
extern uint8_t user_iretq[];
static uint64_t gdt[MAX_CPUS][GDT_ENTRIES] __attribute__((aligned(64)));
struct ldt_table { struct ldt_table* next; struct address_space* space; uint64_t* entries; };
static struct ldt_table* ldt_tables;          /* address spaces that have an LDT */
static volatile int ldt_lock;                 /* ldt_tables (leaf spinlock) */
static volatile uint64_t ldt_generation = 1;  /* bumped by every LDT write, creation and release */
static struct { struct address_space* space; uint64_t generation; const uint64_t* loaded; } cpu_ldt[MAX_CPUS];

static inline uint64_t rdmsr(uint32_t msr) { uint32_t lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr)); return (uint64_t)hi << 32 | lo; }
static inline void wrmsr(uint32_t msr, uint64_t v) { __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)) : "memory"); }
static inline uint16_t read_ds(void) { uint16_t v; __asm__ volatile("mov %%ds,%0" : "=r"(v)); return v; }
static inline uint16_t read_es(void) { uint16_t v; __asm__ volatile("mov %%es,%0" : "=r"(v)); return v; }
static inline uint16_t read_fs(void) { uint16_t v; __asm__ volatile("mov %%fs,%0" : "=r"(v)); return v; }
static inline uint16_t read_gs(void) { uint16_t v; __asm__ volatile("mov %%gs,%0" : "=r"(v)); return v; }
/* Selector loads that may fault (#GP/#NP on a deleted LDT or TLS entry): segments_fixup restarts the
 * instruction with a null selector. syscall32_ignore is CSTAR (SYSCALL in compatibility mode). */
void segment_load_ds(uint32_t selector);
void segment_load_es(uint32_t selector);
void segment_load_fs(uint32_t selector);
void segment_load_gs(uint32_t selector);
void syscall32_ignore(void);
__asm__(".text\n"
        ".globl segment_load_ds, segment_load_es, segment_load_fs, segment_load_gs, syscall32_ignore\n"
        "segment_load_ds: mov %edi,%ds\n\tret\n"
        "segment_load_es: mov %edi,%es\n\tret\n"
        "segment_load_fs: mov %edi,%fs\n\tret\n"
        "segment_load_gs: mov %edi,%gs\n\tret\n"
        "syscall32_ignore: movl $-38,%eax\n\tsysretl\n");

/* Linux fill_ldt: DPL 3, accessed, never long mode. */
static uint64_t descriptor(const struct user_desc* u) {
    uint64_t base = u->base_addr, limit = u->limit, f = u->flags;
    uint64_t type = ((((f >> 3) & 1) ^ 1) << 1) | (((f >> 1) & 3) << 2) | 1;
    return (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (type << 40) | (1ULL << 44) | (3ULL << 45) |
           ((((f >> 5) & 1) ^ 1) << 47) | (((limit >> 16) & 0xF) << 48) | (((f >> 6) & 1) << 52) |
           ((f & 1) << 54) | (((f >> 4) & 1) << 55) | ((base >> 24) << 56);
}
static bool desc_empty(const struct user_desc* u) { /* LDT_empty */
    return !u->base_addr && !u->limit && (u->flags & 0x7F) == (UD_READ_EXEC_ONLY | UD_NOT_PRESENT);
}
static bool desc_zero(const struct user_desc* u) { return !u->base_addr && !u->limit && !(u->flags & 0x7F); }
static uint64_t system_descriptor(uint64_t base, uint32_t limit, unsigned type) {
    return (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | ((uint64_t)(0x80 | type) << 40) |
           ((uint64_t)((limit >> 16) & 0xF) << 48) | ((base >> 24 & 0xFF) << 56);
}

void segments_cpu_init(unsigned index, uint64_t tss, uint32_t tss_limit) {
    uint64_t* g = gdt[index];
    memcpy(g, gdt64, 16 * 8); /* null, kernel code32/code64/data, user code32/data/code64 */
    g[16 + 2 * index] = system_descriptor(tss, tss_limit, 0x9); g[17 + 2 * index] = tss >> 32;
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) table = {sizeof(gdt[0]) - 1, (uint64_t)g};
    __asm__ volatile("lgdt %0" :: "m"(table) : "memory");
    __asm__ volatile("ltr %0" :: "r"((uint16_t)(TSS_SELECTOR + 16 * index)));
    __asm__ volatile("lldt %0" :: "r"((uint16_t)0));
    cpu_ldt[index].space = NULL; cpu_ldt[index].generation = 0; cpu_ldt[index].loaded = NULL;
    wrmsr(0xC0000083, (uint64_t)syscall32_ignore); /* CSTAR */
    uint32_t a, b, c, d; __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (d & (1U << 11)) { wrmsr(0x174, 0); wrmsr(0x175, 0); wrmsr(0x176, 0); } /* SYSENTER_CS 0: disabled */
}

/* ---- LDT ownership and per-CPU loading ---- */
static struct ldt_table* ldt_find(struct address_space* space) { /* ldt_lock held */
    for (struct ldt_table* t = ldt_tables; t; t = t->next) if (t->space == space) return t;
    return NULL;
}
static uint64_t* ldt_entries(struct address_space* space) {
    if (!__atomic_load_n(&ldt_tables, __ATOMIC_ACQUIRE)) return NULL; /* no process uses an LDT */
    spin_acquire(&ldt_lock);
    struct ldt_table* t = ldt_find(space); uint64_t* entries = t ? t->entries : NULL;
    spin_release(&ldt_lock);
    return entries;
}
static void ldt_load(unsigned index, const uint64_t* entries) {
    if (entries) {
        gdt[index][10] = system_descriptor((uint64_t)entries, LDT_BYTES - 1, 0x2); gdt[index][11] = (uint64_t)entries >> 32;
        __asm__ volatile("lldt %0" :: "r"((uint16_t)LDT_SELECTOR) : "memory");
    } else __asm__ volatile("lldt %0" :: "r"((uint16_t)0) : "memory");
    cpu_ldt[index].loaded = entries;
}
/* This CPU's LDTR follows `space`; returns whether the LDT state was re-read (generation changed). */
static bool ldt_sync(unsigned index, struct address_space* space) {
    uint64_t generation = __atomic_load_n(&ldt_generation, __ATOMIC_ACQUIRE);
    if (cpu_ldt[index].space == space && cpu_ldt[index].generation == generation) return false;
    const uint64_t* entries = ldt_entries(space);
    if (entries != cpu_ldt[index].loaded) ldt_load(index, entries);
    cpu_ldt[index].space = space; cpu_ldt[index].generation = generation;
    return true;
}
void segments_user_return(struct address_space* space) {
    if (!ldt_sync(cpu_self()->index, space)) return;
    /* Linux refresh_ldt_segments: cached DS/ES descriptors follow a changed LDT (a deleted entry: null). */
    uint16_t ds = read_ds(), es = read_es();
    if (ds & 4) segment_load_ds(ds);
    if (es & 4) segment_load_es(es);
}
bool segments_space_clone(struct address_space* source, struct address_space* child) {
    uint64_t* from = ldt_entries(source);
    if (!from) return true;
    struct ldt_table* t = (void*)page_alloc(); uint64_t* entries = (void*)arena_alloc(LDT_BYTES);
    if (!t || !entries) { if (t) page_free((uint64_t)t); if (entries) arena_free((uint64_t)entries, LDT_BYTES); return false; }
    memcpy(entries, from, LDT_BYTES); t->space = child; t->entries = entries;
    spin_acquire(&ldt_lock); t->next = ldt_tables; __atomic_store_n(&ldt_tables, t, __ATOMIC_RELEASE); spin_release(&ldt_lock);
    __atomic_add_fetch(&ldt_generation, 1, __ATOMIC_SEQ_CST);
    return true;
}
void segments_space_release(struct address_space* space) {
    spin_acquire(&ldt_lock);
    struct ldt_table** link = &ldt_tables;
    while (*link && (*link)->space != space) link = &(*link)->next;
    struct ldt_table* t = *link; if (t) *link = t->next;
    spin_release(&ldt_lock);
    if (!t) return;
    /* No CPU runs this space any more; one whose LDTR still points here reloads it before ring 3. */
    __atomic_add_fetch(&ldt_generation, 1, __ATOMIC_SEQ_CST);
    arena_free((uint64_t)t->entries, LDT_BYTES); page_free((uint64_t)t);
}

/* ---- context switches ---- */
void segments_switch_out(struct user_segments* s, uint64_t* fs_base, uint64_t* gs_base) {
    struct cpu* c = cpu_self();
    s->ds = read_ds(); s->es = read_es(); s->fs = read_fs(); s->gs = read_gs();
    c->loaded_fs = *fs_base = rdmsr(MSR_FS_BASE); c->loaded_gs = *gs_base = rdmsr(MSR_GS_BASE);
}
static void load_fs_gs(bool fs, uint16_t now, uint16_t next, uint64_t base) {
    struct cpu* c = cpu_self(); uint64_t* loaded = fs ? &c->loaded_fs : &c->loaded_gs;
    if (next > 3) { if (fs) segment_load_fs(next); else segment_load_gs(next); *loaded = BASE_UNKNOWN; return; }
    bool reloaded = (now | next) != 0; /* loading null clears the base on Intel, keeps it on AMD */
    if (reloaded) { if (fs) segment_load_fs(next); else segment_load_gs(next); }
    if (reloaded || *loaded != base) { wrmsr(fs ? MSR_FS_BASE : MSR_GS_BASE, base); *loaded = base; }
}
void segments_switch_in(const struct user_segments* s, struct address_space* space, uint64_t fs_base, uint64_t gs_base) {
    unsigned index = cpu_self()->index; uint64_t* g = gdt[index];
    for (unsigned k = 0; k < 3; k++) if (g[TLS_MIN + k] != s->tls[k]) g[TLS_MIN + k] = s->tls[k];
    ldt_sync(index, space); /* before any LDT selector is loaded */
    uint16_t now = read_ds(); if (now | s->ds) segment_load_ds(s->ds); /* Linux __switch_to */
    now = read_es(); if (now | s->es) segment_load_es(s->es);
    load_fs_gs(true, read_fs(), s->fs, fs_base);
    load_fs_gs(false, read_gs(), s->gs, gs_base);
}
void segments_clone(struct user_segments* child, const struct user_segments* parent, bool new_tls) {
    memcpy(child->tls, parent->tls, sizeof(child->tls));
    child->ds = read_ds(); child->es = read_es(); child->fs = new_tls ? 0 : read_fs(); child->gs = read_gs();
}
void segments_exec(struct user_segments* s) {
    struct cpu* c = cpu_self();
    memset(s, 0, sizeof(*s));
    for (unsigned k = 0; k < 3; k++) gdt[c->index][TLS_MIN + k] = 0;
    segment_load_ds(0); segment_load_es(0); segment_load_fs(0); segment_load_gs(0);
    wrmsr(MSR_FS_BASE, 0); wrmsr(MSR_GS_BASE, 0); c->loaded_fs = c->loaded_gs = 0;
}

/* ---- fault fixups (trap64, kernel-mode #GP/#NP/#SS) ---- */
bool segments_fixup(struct registers* r) {
    /* FS/GS: the flat USER_DS first (Linux CLEAR_FS / ZAP_GS): a null load keeps the hidden base on AMD
     * (X86_BUG_NULL_SEG), so the base of the task that ran before must not survive the null retry. */
    if (r->rip == (uint64_t)segment_load_fs) segment_load_fs(USER_DS);
    if (r->rip == (uint64_t)segment_load_gs) segment_load_gs(USER_DS);
    if (r->rip == (uint64_t)segment_load_ds || r->rip == (uint64_t)segment_load_es ||
        r->rip == (uint64_t)segment_load_fs || r->rip == (uint64_t)segment_load_gs) { r->rdi = 0; return true; }
    if (r->rip == (uint64_t)user_iretq) {
        /* iretq popped a ring-3 frame whose CS or SS no longer exists (LDT/TLS entry changed by another
         * thread): the exception frame's RSP points at that frame. Report the fault from user mode. */
        const uint64_t* frame = (const uint64_t*)r->rsp;
        if ((frame[1] & 3) != 3) return false;
        r->rip = frame[0]; r->cs = frame[1]; r->flags = frame[2]; r->rsp = frame[3]; r->ss = frame[4];
    }
    return false;
}

/* ---- arch_prctl ---- */
int64_t segments_arch_prctl(uint64_t code, uint64_t argument) {
    struct cpu* c = cpu_self();
    switch (code) {
    case 0x1001: case 0x1002: /* ARCH_SET_GS, ARCH_SET_FS: null selector, then the base */
        if (argument >= TASK_SIZE_MAX) return -1;
        if (code == 0x1002) { segment_load_fs(0); wrmsr(MSR_FS_BASE, argument); c->loaded_fs = argument; }
        else { segment_load_gs(0); wrmsr(MSR_GS_BASE, argument); c->loaded_gs = argument; }
        return 0;
    case 0x1003: case 0x1004: /* ARCH_GET_FS, ARCH_GET_GS: the base in use (a selector's base included) */
        if (!accessible(argument, 8, true)) return -14;
        return uput64(argument, rdmsr(code == 0x1003 ? MSR_FS_BASE : MSR_GS_BASE));
    case 0x1011: return 1;   /* ARCH_GET_CPUID: CPUID is never trapped */
    case 0x1012: return -19; /* ARCH_SET_CPUID: no CPUID faulting (ENODEV) */
    }
    return -22;
}

/* ---- modify_ldt ---- */
static int64_t ldt_write(uint64_t address, uint64_t bytes, bool oldmode) {
    struct user_desc u;
    if (bytes != sizeof(u)) return -22;
    if (!accessible(address, sizeof(u), false)) return -14;
    copy_from_user(&u, (const void*)address, sizeof(u));
    if (u.entry_number >= LDT_ENTRIES) return -22;
    if (((u.flags >> 1) & 3) == 3 && (oldmode || !(u.flags & UD_NOT_PRESENT))) return -22;
    uint64_t value = 0;
    if (!((oldmode && !u.base_addr && !u.limit) || desc_empty(&u))) {
        if (!(u.flags & UD_32BIT)) return -22; /* 16-bit segments: no espfix64 */
        value = descriptor(&u);
        if (oldmode) value &= ~(1ULL << 52); /* AVL */
    }
    struct address_space* space = space_current();
    spin_acquire(&ldt_lock); struct ldt_table* t = ldt_find(space); spin_release(&ldt_lock);
    if (!t) { /* the first write creates the LDT, even one that clears an entry (Linux) */
        t = (void*)page_alloc(); uint64_t* entries = (void*)arena_alloc(LDT_BYTES);
        if (!t || !entries) { if (t) page_free((uint64_t)t); if (entries) arena_free((uint64_t)entries, LDT_BYTES); return -12; }
        t->space = space; t->entries = entries;
        spin_acquire(&ldt_lock); t->next = ldt_tables; __atomic_store_n(&ldt_tables, t, __ATOMIC_RELEASE); spin_release(&ldt_lock);
    }
    __atomic_store_n(&t->entries[u.entry_number], value, __ATOMIC_RELEASE);
    __atomic_add_fetch(&ldt_generation, 1, __ATOMIC_SEQ_CST);
    /* Every other CPU running this space: reload the LDT and refresh DS/ES before we return (the
     * shootdown IPI reaches segments_user_return; a CPU in the kernel does it on its way out). */
    smp_tlb_shootdown(cpu_self()->loaded_root, false);
    return 0;
}
static int64_t ldt_read(uint64_t address, uint64_t bytes) {
    uint64_t* entries = ldt_entries(space_current());
    if (!entries || !bytes) return 0;
    if (bytes > LDT_BYTES) bytes = LDT_BYTES;
    if (!accessible(address, bytes, true)) return -14;
    if (copy_to_user((void*)address, entries, bytes)) return -14;
    return (int64_t)bytes;
}
int64_t segments_modify_ldt(uint64_t function, uint64_t address, uint64_t bytes) {
    int64_t result = -38;
    switch ((int32_t)function) {
    case 0: result = ldt_read(address, bytes); break;
    case 1: result = ldt_write(address, bytes, true); break;
    case 2: /* read_default_ldt: 128 zero bytes on x86-64 */
        if (bytes > 128) bytes = 128;
        if (bytes && !accessible(address, bytes, true)) { result = -14; break; }
        result = clear_user(address, bytes) ? -14 : (int64_t)bytes; break;
    case 0x11: result = ldt_write(address, bytes, false); break;
    }
    return (uint32_t)result; /* Linux returns an int-sized result: errors are not sign-extended */
}

/* ---- TLS slots (IA-32 set_thread_area / get_thread_area) ---- */
static int tls_store(struct user_segments* s, struct user_desc* u, bool* allocated) {
    bool clear = desc_empty(u) || desc_zero(u);
    if (!clear && (!(u->flags & UD_32BIT) || ((u->flags >> 1) & 3) > 1 || (u->flags & UD_NOT_PRESENT))) return -22;
    int32_t index = (int32_t)u->entry_number;
    *allocated = index == -1;
    if (index == -1) {
        for (index = TLS_MIN; index <= TLS_MAX && s->tls[index - TLS_MIN]; index++) {}
        if (index > TLS_MAX) return -3; /* ESRCH */
        u->entry_number = (uint32_t)index;
    }
    if (index < TLS_MIN || index > TLS_MAX) return -22;
    s->tls[index - TLS_MIN] = clear ? 0 : descriptor(u);
    return index;
}
static void tls_install(const struct user_segments* s, int index) {
    struct cpu* c = cpu_self();
    gdt[c->index][index] = s->tls[index - TLS_MIN];
    /* Linux do_set_thread_area: DS/ES/FS/GS holding the changed selector are reloaded at once. */
    uint16_t selector = (uint16_t)(index << 3 | 3);
    if (read_ds() == selector) segment_load_ds(selector);
    if (read_es() == selector) segment_load_es(selector);
    if (read_fs() == selector) { segment_load_fs(selector); c->loaded_fs = BASE_UNKNOWN; }
    if (read_gs() == selector) { segment_load_gs(selector); c->loaded_gs = BASE_UNKNOWN; }
}
static int64_t tls_set(uint64_t address) {
    struct user_segments* s = task_segments(); struct user_desc u; bool allocated;
    if (!s) return -38;
    if (!accessible(address, sizeof(u), false)) return -14;
    copy_from_user(&u, (const void*)address, sizeof(u));
    struct user_segments next = *s;
    int index = tls_store(&next, &u, &allocated);
    if (index < 0) return index;
    if (allocated) { if (!accessible(address, 4, true)) return -14; uput32(address, u.entry_number); }
    *s = next; tls_install(s, index);
    return 0;
}
static int64_t tls_get(uint64_t address) {
    struct user_segments* s = task_segments();
    if (!s) return -38;
    if (!accessible(address, 4, false)) return -14;
    int32_t index = (int32_t)uget32(address);
    if (index < TLS_MIN || index > TLS_MAX) return -22;
    uint64_t d = s->tls[index - TLS_MIN], type = d >> 40 & 15;
    struct user_desc u = {(uint32_t)index, (uint32_t)((d >> 16 & 0xFFFFFF) | (d >> 56 << 24)), (uint32_t)((d & 0xFFFF) | (d >> 48 & 15) << 16),
        (uint32_t)((d >> 54 & 1) | (type >> 2) << 1 | !(type & 2) << 3 | (d >> 55 & 1) << 4 | !(d >> 47 & 1) << 5 | (d >> 52 & 1) << 6 | (d >> 53 & 1) << 7)};
    if (!accessible(address, sizeof(u), true)) return -14;
    return copy_to_user((void*)address, &u, sizeof(u)) ? -14 : 0;
}

/* ---- int $0x80: IA-32 numbers, arguments in EBX, ECX, EDX (pointers are 32-bit) ---- */
struct registers* segments_int80(struct registers* r) {
    uint32_t number = (uint32_t)r->rax, first = (uint32_t)r->rbx;
    int64_t result;
    switch (number) {
    case 1: case 252: /* exit, exit_group */
        if (number == 252) task_exit_group(first & 255);
        user_termination_signal = 0; user_result = first & 255; return NULL;
    case 20: result = task_visible_pid(task_pid()); break;   /* getpid */
    case 224: result = task_visible_pid(task_tid()); break;  /* gettid */
    case 123: result = segments_modify_ldt(first, (uint32_t)r->rcx, (uint32_t)r->rdx); break;
    case 243: result = tls_set(first); break;
    case 244: result = tls_get(first); break;
    default: result = -38;
    }
    r->rax = (uint64_t)result;
    return signal_dispatch(r);
}

/* ---- sigreturn checks: ring-3 code/stack selectors that iretq accepts ---- */
static bool user_descriptor(uint16_t selector, uint64_t* value) {
    unsigned index = selector >> 3;
    if ((selector & 3) != 3) return false;
    if (selector & 4) {
        uint64_t* entries = ldt_entries(space_current());
        if (!entries) return false;
        *value = __atomic_load_n(&entries[index], __ATOMIC_ACQUIRE); return true;
    }
    if (index >= 4 && index <= 6) { *value = gdt64[index]; return true; } /* 0x23, 0x2b, 0x33 */
    struct user_segments* s = task_segments();
    if (s && index >= TLS_MIN && index <= TLS_MAX) { *value = s->tls[index - TLS_MIN]; return true; }
    return false;
}
bool segments_user_code(uint16_t selector) { /* present code segment, DPL 3 */
    uint64_t d; return user_descriptor(selector, &d) && (d >> 47 & 1) && (d >> 43 & 3) == 3 && (d >> 45 & 3) == 3;
}
bool segments_user_stack(uint16_t selector) { /* present writable data segment, DPL 3 */
    uint64_t d; return user_descriptor(selector, &d) && (d >> 47 & 1) && (d >> 43 & 3) == 2 && (d >> 41 & 1) && (d >> 45 & 3) == 3;
}

/* ---- boot self-test (WoW64 campaign): this CPU's tables, encodings, TLS rules, LDT loads, fixups ---- */
static void expect(bool ok, const char* what) {
    if (!ok) { vga_print("[64] WOW32 SELFTEST FAIL: "); vga_print(what); vga_print("\n"); panic("segments self-test"); }
}
void segments_selftest(void) {
    struct cpu* c = cpu_self(); unsigned index = c->index;
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) table;
    __asm__ volatile("sgdt %0" : "=m"(table));
    uint16_t tr, cs; __asm__ volatile("str %0; mov %%cs,%1" : "=r"(tr), "=r"(cs));
    expect(table.base == (uint64_t)gdt[index] && table.limit == GDT_ENTRIES * 8 - 1, "per-CPU GDT loaded");
    expect(tr == TSS_SELECTOR + 16 * index && cs == KERNEL_CS, "TSS and kernel CS selectors");
    expect(gdt[index][4] == 0x00CFFB000000FFFFULL && gdt[index][5] == 0x00CFF3000000FFFFULL && gdt[index][6] == 0x00AFFB000000FFFFULL,
           "user code32/data/code64 descriptors");
    uint64_t star = rdmsr(0xC0000081);
    expect((star >> 32 & 0xFFFF) == KERNEL_CS && star >> 48 == USER32_CS && rdmsr(0xC0000083) == (uint64_t)syscall32_ignore, "STAR/CSTAR");
    /* Wine's alloc_fs_sel request: base, limit ~0 (byte granular 1 MiB), seg_32bit | useable. */
    struct user_desc u = {0xFFFFFFFFU, 0x12345000U, 0xFFFFFFFFU, UD_32BIT | UD_USEABLE};
    struct user_segments s; memset(&s, 0, sizeof(s)); bool allocated;
    expect(tls_store(&s, &u, &allocated) == 12 && allocated && u.entry_number == 12 && s.tls[0] == 0x125FF3345000FFFFULL, "TLS slot allocation and encoding");
    struct user_desc v = u; v.entry_number = 0xFFFFFFFFU; v.flags = UD_32BIT | UD_LIMIT_PAGES; v.limit = 0xFFFFF;
    expect(tls_store(&s, &v, &allocated) == 13 && s.tls[1] == 0x12CFF3345000FFFFULL, "second TLS slot (4 GiB pages)");
    v.entry_number = 0xFFFFFFFFU; expect(tls_store(&s, &v, &allocated) == 14, "third TLS slot");
    v.entry_number = 0xFFFFFFFFU; expect(tls_store(&s, &v, &allocated) == -3, "TLS slots exhausted (ESRCH)");
    v.entry_number = 11; expect(tls_store(&s, &v, &allocated) == -22, "TLS index range");
    v.entry_number = 13; v.flags = 0; expect(tls_store(&s, &v, &allocated) == -22, "16-bit TLS refused");
    v.flags = UD_32BIT | 4; expect(tls_store(&s, &v, &allocated) == -22, "code TLS refused");
    v.flags = UD_32BIT | UD_NOT_PRESENT; expect(tls_store(&s, &v, &allocated) == -22, "not-present TLS refused");
    struct user_desc zero = {13, 0, 0, 0}; expect(tls_store(&s, &zero, &allocated) == 13 && !s.tls[1], "zeroed user_desc clears a slot");
    struct user_desc empty = {14, 0, 0, UD_READ_EXEC_ONLY | UD_NOT_PRESENT}; expect(tls_store(&s, &empty, &allocated) == 14 && !s.tls[2], "LDT_empty clears a slot");
    /* Load a TLS selector in ring 0: FS takes the descriptor base; a deleted slot loads null (fixup). */
    uint64_t saved_fs = rdmsr(MSR_FS_BASE), saved_gs = rdmsr(MSR_GS_BASE);
    uint16_t saved_fs_selector = read_fs(), saved_gs_selector = read_gs();
    gdt[index][12] = s.tls[0]; gdt[index][13] = 0;
    segment_load_fs(0x63); expect(read_fs() == 0x63 && rdmsr(MSR_FS_BASE) == 0x12345000ULL, "TLS selector supplies the FS base");
    segment_load_fs(0x6B); expect(read_fs() == 0 && !rdmsr(MSR_FS_BASE), "stale TLS selector loads null with base 0 (fault fixup)");
    for (unsigned k = 0; k < 2; k++) { /* the fixup zeroes the base itself (AMD keeps it on a null load) */
        struct registers fake; memset(&fake, 0, sizeof(fake)); fake.rip = k ? (uint64_t)segment_load_gs : (uint64_t)segment_load_fs; fake.rdi = 0x6B;
        wrmsr(k ? MSR_GS_BASE : MSR_FS_BASE, 0x5A5A5000ULL);
        expect(segments_fixup(&fake) && !fake.rdi && !rdmsr(k ? MSR_GS_BASE : MSR_FS_BASE), "FS/GS fault fixup clears the base");
    }
    /* An LDT on this CPU: data entry 3 through GS, then deleted (refresh -> null). */
    uint64_t* entries = (void*)arena_alloc(LDT_BYTES); expect(entries != NULL, "LDT allocation");
    struct user_desc data = {3, 0x00ABC000U, 0xFFFFF, UD_32BIT | UD_LIMIT_PAGES};
    entries[3] = descriptor(&data); expect(entries[3] == 0x00CFF3ABC000FFFFULL, "LDT data descriptor encoding");
    const uint64_t* previous = cpu_ldt[index].loaded;
    ldt_load(index, entries);
    segment_load_gs(0x1F); expect(read_gs() == 0x1F && rdmsr(MSR_GS_BASE) == 0xABC000ULL, "LDT selector supplies the GS base");
    entries[3] = 0; segment_load_gs(0x1F); expect(read_gs() == 0 && !rdmsr(MSR_GS_BASE), "deleted LDT entry loads null with base 0 (fault fixup)");
    data.flags = UD_32BIT | UD_LIMIT_PAGES | 4; data.entry_number = 4; expect(descriptor(&data) == 0x00CFFBABC000FFFFULL, "LDT code descriptor encoding");
    ldt_load(index, previous); cpu_ldt[index].space = NULL; cpu_ldt[index].generation = 0;
    arena_free((uint64_t)entries, LDT_BYTES);
    gdt[index][12] = gdt[index][13] = 0;
    segment_load_fs(saved_fs_selector); segment_load_gs(saved_gs_selector);
    wrmsr(MSR_FS_BASE, saved_fs); wrmsr(MSR_GS_BASE, saved_gs); c->loaded_fs = saved_fs; c->loaded_gs = saved_gs;
    vga_print("[64] PASS WOW32 SELFTEST: per-CPU GDT (Linux selectors), STAR/CSTAR, TLS slots, LDT loads, selector fault fixups\n");
}
