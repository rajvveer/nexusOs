/* ============================================================================
 * NexusOS — kguard: in-kernel crash containment (implementation) — Phase 55
 * ============================================================================
 * See kguard.h for the model. This file owns the guard stack, the recovery
 * decision, the "!KR:" serial log line, the reset of stuck global state and
 * the double-fault task (TSS + task gate) used for kernel-stack overflows.
 * ============================================================================ */

#include "kguard.h"
#include "gdt.h"
#include "paging.h"
#include "port.h"
#include "process.h"
#include "string.h"
#include "syslog.h"
#include "posix.h"
#include "net.h"
#include "vga.h"

extern volatile int uproc_in_syscall;                 /* uproc.c */
extern void exception_fatal(uint32_t int_no, uint32_t err, uint32_t eip);  /* idt.c */
extern void kguard_longjmp(const kguard_ctx_t* ctx, int code) __attribute__((noreturn));
extern void kguard_df_entry(void);

/* --------------------------------------------------------------------------
 * Guard stack
 * -------------------------------------------------------------------------- */
#define KGUARD_DEPTH 8

typedef struct { kguard_ctx_t* ctx; const char* what; } kguard_slot_t;
static kguard_slot_t slots[KGUARD_DEPTH];
static volatile int depth = 0;           /* pushes outstanding (may exceed DEPTH) */
static volatile int irq_depth = 0;       /* kernel-thread IRQ nesting */

static int      last_int  = -1;
static uint32_t last_eip  = 0;
static uint32_t last_addr = 0;
static uint32_t recovered = 0;

/* Called by kguard_try (asm) with the snapshot already taken. */
void kguard_push(kguard_ctx_t* ctx, const char* what) {
    if (depth < KGUARD_DEPTH) {
        slots[depth].ctx  = ctx;
        slots[depth].what = what;
    }
    depth++;
}

void kguard_end(void) {
    if (depth > 0) depth--;
}

bool kguard_active(void) { return depth > 0; }

void kguard_irq_enter(void) {
    /* Only the kernel thread's IRQs are counted: the timer never switches
     * away from ring-0 code, so the count can't be carried across a context
     * switch. User processes are preempted inside IRQs and would drift it. */
    process_t* cur = process_get_current();
    if (!cur || !cur->is_user) irq_depth++;
}

void kguard_irq_leave(void) {
    process_t* cur = process_get_current();
    if ((!cur || !cur->is_user) && irq_depth > 0) irq_depth--;
}

int      kguard_last_int(void)        { return last_int; }
uint32_t kguard_last_eip(void)        { return last_eip; }
uint32_t kguard_last_addr(void)       { return last_addr; }
uint32_t kguard_recovered_count(void) { return recovered; }

/* --------------------------------------------------------------------------
 * Names
 * -------------------------------------------------------------------------- */
static const char* const exc_long[20] = {
    "Divide error", "Debug trap", "NMI", "Breakpoint", "Overflow",
    "Bound range", "Invalid opcode", "No FPU", "Double fault", "Coproc overrun",
    "Invalid TSS", "Segment missing", "Stack fault", "GPF", "Page fault",
    "Reserved", "FPU error", "Alignment check", "Machine check", "SIMD error"
};
static const char* const exc_short[20] = {
    "#DE", "#DB", "NMI", "#BP", "#OF", "#BR", "#UD", "#NM", "#DF", "#MF9",
    "#TS", "#NP", "#SS", "#GP", "#PF", "#15", "#MF", "#AC", "#MC", "#XM"
};

const char* kguard_exc_name(int code) {
    int n = code - 1;
    return (n >= 0 && n < 20) ? exc_long[n] : "Exception";
}

const char* kguard_exc_short(int code) {
    int n = code - 1;
    return (n >= 0 && n < 20) ? exc_short[n] : "#??";
}

static void sappend(char* buf, int n, const char* s) {
    int len = (int)strlen(buf);
    while (*s && len < n - 1) buf[len++] = *s++;
    buf[len] = '\0';
}

void kguard_describe(int code, char* buf, int n) {
    char h[12];
    if (n <= 0) return;
    buf[0] = '\0';
    sappend(buf, n, kguard_exc_name(code));
    sappend(buf, n, " at ");
    hex_to_str(last_eip, h); sappend(buf, n, h);
    if (code == 1 + 14 || code == 1 + 8) {
        sappend(buf, n, " (addr ");
        hex_to_str(last_addr, h); sappend(buf, n, h);
        sappend(buf, n, ")");
    }
}

/* --------------------------------------------------------------------------
 * Serial (COM1) helpers — the !KR: line must never depend on the console
 * -------------------------------------------------------------------------- */
static void ser_putc(char c) { while ((port_byte_in(0x3FD) & 0x20) == 0); port_byte_out(0x3F8, c); }
static void ser_str(const char* s) { for (; *s; s++) ser_putc(*s); }
static void ser_hex(uint32_t v) {
    const char* h = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4) ser_putc(h[(v >> i) & 0xF]);
}

/* --------------------------------------------------------------------------
 * Recovery decision
 * -------------------------------------------------------------------------- */
static bool recoverable(uint32_t cs) {
    if (depth <= 0) return false;                 /* nothing armed */
    if ((cs & 3) != 0) return false;              /* ring-3 faults belong to uproc_fault */
    if (uproc_in_syscall > 0) return false;       /* kernel running for a user process */
    if (irq_depth > 0) return false;              /* inside an IRQ handler: fatal */
    process_t* cur = process_get_current();
    if (cur && cur->is_user) return false;        /* a user process is current */
    return true;
}

bool kguard_should_recover(struct registers* regs) {
    return recoverable(regs->cs);
}

/* Log + bookkeeping shared by the ISR path and the #DF task path. Returns the
 * context to resume into and the fault code. */
static kguard_ctx_t* prepare_recovery(int int_no, uint32_t err, uint32_t eip,
                                      uint32_t addr, int* code_out) {
    int top = depth < KGUARD_DEPTH ? depth : KGUARD_DEPTH;
    kguard_slot_t* s = &slots[top - 1];

    last_int  = int_no;
    last_eip  = eip;
    last_addr = addr;
    recovered++;

    /* ONE serial line: !KR:<int> <err> <eip> <addr> <what>  (NOT !EX:, so the
     * test harness keeps treating !EX: as a real crash). */
    ser_str("!KR:"); ser_hex((uint32_t)int_no); ser_putc(' ');
    ser_hex(err); ser_putc(' '); ser_hex(eip); ser_putc(' '); ser_hex(addr);
    ser_putc(' '); ser_str(s->what ? s->what : "?"); ser_putc('\n');

    {
        char m[SLOG_MSG_MAX]; char h[12];
        m[0] = '\0';
        sappend(m, SLOG_MSG_MAX, "KR ");
        sappend(m, SLOG_MSG_MAX, kguard_exc_short(1 + int_no));
        sappend(m, SLOG_MSG_MAX, " ");
        hex_to_str(eip, h); sappend(m, SLOG_MSG_MAX, h);
        sappend(m, SLOG_MSG_MAX, " ");
        sappend(m, SLOG_MSG_MAX, s->what ? s->what : "?");
        syslog_add(m);
    }

    /* Reset global "in progress" state the abandoned frames would have left
     * stuck. The kernel has no locks; these are the flags that gate work. */
    if (posix_is_capturing()) { posix_capture_stop(); posix_capture_clear(); }
    net_clear_busy();
    uproc_in_syscall = 0;

    depth = top - 1;                              /* pop this guard (and any overflow) */
    *code_out = 1 + int_no;
    return s->ctx;
}

void kguard_recover(struct registers* regs, int int_no, uint32_t addr) {
    int code;
    kguard_ctx_t* ctx = prepare_recovery(int_no, regs->err_code, regs->eip, addr, &code);
    kguard_longjmp(ctx, code);
}

/* --------------------------------------------------------------------------
 * Double-fault task
 * --------------------------------------------------------------------------
 * A kernel-stack overflow into the unmapped guard pages (paging_protect_kernel)
 * raises #PF, but the CPU cannot push the #PF frame on that same stack, so it
 * escalates to #DF. Delivered through an interrupt gate, that #DF would fail
 * the same way (triple fault -> reset). A task gate makes the CPU save the
 * interrupted state into the current TSS and load this task's own stack, so
 * the handler runs. If a guard is armed it rewrites the saved state in the
 * main TSS to the recovery context and irets (NT=1) back into it.
 * -------------------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint32_t prev_tss;
    uint32_t esp0, ss0, esp1, ss1, esp2, ss2;
    uint32_t cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap, iomap_base;
} kg_tss_t;

static kg_tss_t df_tss __attribute__((aligned(16)));
static uint8_t  df_stack[4096] __attribute__((aligned(16)));

void kguard_df_handler(void) {
    kg_tss_t* m = (kg_tss_t*)tss_main();          /* interrupted task's state */
    uint32_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    if (recoverable(m->cs)) {
        int code;
        kguard_ctx_t* ctx = prepare_recovery(8, 0, m->eip, cr2, &code);
        m->eip = ctx->eip;
        m->esp = ctx->esp;
        m->ebx = ctx->ebx; m->esi = ctx->esi; m->edi = ctx->edi; m->ebp = ctx->ebp;
        m->eax = (uint32_t)code;
        m->eflags = (m->eflags | 0x200) & ~0x4000u;  /* IF=1, NT=0 in the resumed task */
        /* A task switch LOADS CR3 from the incoming TSS but never saves it on
         * the way out, and the main TSS has only ever served esp0 — its cr3
         * field is 0. Resuming with CR3=0 triple-faults (seen as a reboot
         * right after the !KR: line). Use the directory the kernel thread
         * actually had loaded (paging_switch tracks it). */
        m->cr3 = paging_current_cr3();
        return;                                   /* asm: add esp,4 ; iret */
    }

    exception_fatal(8, 0, m->eip);                /* !EX: + red bar + halt */
}

void kguard_init(void) {
    memset(&df_tss, 0, sizeof(df_tss));
    df_tss.cr3    = paging_kernel_cr3();
    df_tss.eip    = (uint32_t)kguard_df_entry;
    df_tss.eflags = 0x2;                          /* IF=0 inside the #DF task */
    df_tss.esp    = (uint32_t)df_stack + sizeof(df_stack);
    df_tss.esp0   = df_tss.esp;
    df_tss.ss0    = 0x10;
    df_tss.cs     = 0x08;
    df_tss.ss = df_tss.ds = df_tss.es = df_tss.fs = df_tss.gs = 0x10;
    df_tss.iomap_base = sizeof(kg_tss_t);

    gdt_set_df_tss((uint32_t)&df_tss, sizeof(kg_tss_t) - 1);   /* GDT entry 6 */
    idt_set_gate(8, 0, GDT_DF_TSS_SEL, 0x85);                   /* task gate, DPL 0 */

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("kguard: ring-0 fault recovery + #DF task gate (stack overflow)\n");
}
