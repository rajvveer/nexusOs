/* ============================================================================
 * NexusOS — Syscall Interface (Implementation) — Phase 22
 * ============================================================================
 * 22 syscalls. Ring 3 accessible via INT 0x80.
 * Phase 22: Added kill, pipe, getenv, setenv.
 * Phase 32: Added dlopen, dlsym, dlclose.
 * ============================================================================ */

#include "syscall.h"
#include "idt.h"
#include "vga.h"
#include "keyboard.h"
#include "vfs.h"
#include "ramfs.h"
#include "process.h"
#include "scheduler.h"
#include "string.h"
#include "heap.h"
#include "elf.h"
#include "vmm.h"
#include "pipe.h"
#include "env.h"
#include "dynlink.h"
#include "paging.h"
#include "memory.h"
#include "uproc.h"
#include "usys.h"        /* Phase 57 apps: POSIX-ish calls 24+ (usys_abi.h) */
#include "usys_abi.h"

/* win32.c: dispatch a Win32 shim id (from an IAT stub) using args on the user
 * stack pointed to by regs->useresp. */
extern uint32_t win32_dispatch(uint32_t id, struct registers* regs);

/* Validate that [addr, addr+len) is user-accessible in the caller's address
 * space before the kernel reads/writes it. Kernel-originated calls are trusted
 * (the kernel thread never issues int 0x80, but be defensive). */
static bool uptr_ok(uint32_t addr, uint32_t len) {
    process_t* p = process_get_current();
    if (!p || !p->is_user) return true;
    return as_check_user(p->cr3, addr, len);
}

/* --------------------------------------------------------------------------
 * Existing syscall implementations (0–14)
 * -------------------------------------------------------------------------- */

/* SYS_WRITE: print at most len bytes (stops early at a NUL). Returns the
 * number of bytes written. */
static int32_t sys_write(struct registers* regs) {
    const char* str = (const char*)regs->ebx;
    uint32_t len = regs->ecx;
    uint32_t i;
    if (str == NULL) return -1;
    if (len > 0x7FFFFFFF) return -1;
    if (!uptr_ok(regs->ebx, len)) return -1;     /* whole range must be user-mapped */
    for (i = 0; i < len && str[i]; i++) vga_putchar(str[i]);
    return (int32_t)i;
}

static int32_t sys_read(struct registers* regs) {
    char* buffer = (char*)regs->ebx;
    uint32_t max_len = regs->ecx;
    if (buffer == NULL || max_len == 0 || max_len > 0x7FFFFFFF) return -1;
    if (!uptr_ok(regs->ebx, max_len)) return -1; /* must be user-writable */
    keyboard_readline(buffer, (int)max_len);
    return (int32_t)strlen(buffer);
}

static int32_t sys_open(struct registers* regs) {
    const char* name = (const char*)regs->ebx;
    fs_node_t* root = vfs_get_root();
    fs_node_t* node = vfs_finddir(root, name);
    return (int32_t)node;
}

static int32_t sys_close(struct registers* regs) {
    (void)regs;
    return 0;
}

static int32_t sys_create(struct registers* regs) {
    const char* name = (const char*)regs->ebx;
    fs_node_t* node = ramfs_create(name, FS_FILE);
    return (node != NULL) ? 0 : -1;
}

static int32_t sys_exit(struct registers* regs) {
    uproc_sys_exit(regs->ebx);   /* exit code in ebx; never returns for a user proc */
    return 0;
}

/* Phase 54: SYS_SLEEP (ebx = ms) / SYS_SBRK (ebx = signed increment) */
static int32_t sys_sleep(struct registers* regs) {
    return uproc_sys_sleep(regs->ebx);
}
static int32_t sys_sbrk(struct registers* regs) {
    return uproc_sys_sbrk((int32_t)regs->ebx);
}

static int32_t sys_getpid(struct registers* regs) {
    (void)regs;
    process_t* proc = process_get_current();
    return (proc != NULL) ? (int32_t)proc->pid : 0;
}

static int32_t sys_yield(struct registers* regs) {
    (void)regs;
    yield();
    return 0;
}

static int32_t sys_malloc(struct registers* regs) {
    return (int32_t)kmalloc(regs->ebx);
}

/* SYS_FREE: kfree() only releases a pointer inside a live heap block and
 * ignores anything else (NULL, stray, double-free), so a bad pointer can't
 * corrupt the heap. */
static int32_t sys_free(struct registers* regs) {
    kfree((void*)regs->ebx);
    return 0;
}

static int32_t sys_exec(struct registers* regs) {
    const char* name = (const char*)regs->ebx;
    fs_node_t* root = vfs_get_root();
    fs_node_t* node = vfs_finddir(root, name);
    if (!node || node->size == 0) return -1;

    uint8_t* buf = (uint8_t*)kmalloc(node->size);
    if (!buf) return -1;

    int32_t rd = vfs_read(node, 0, node->size, buf);
    if (rd <= 0) { kfree(buf); return -1; }

    int ret = elf_exec(buf, (uint32_t)rd, name);
    kfree(buf);
    return ret;
}

static int32_t sys_fork(struct registers* regs) {
    (void)regs;
    uint32_t child_pd = vmm_fork();
    return (child_pd != 0) ? 0 : -1;
}

static int32_t sys_shmget(struct registers* regs) {
    return vmm_shmem_create(regs->ebx, regs->ecx);
}

static int32_t sys_shmat(struct registers* regs) {
    return (int32_t)vmm_shmem_attach(regs->ebx);
}

static int32_t sys_mmap(struct registers* regs) {
    return (int32_t)vmm_mmap(regs->ebx, regs->ecx, regs->edx);
}

/* --------------------------------------------------------------------------
 * Phase 22 syscalls (15–18)
 * -------------------------------------------------------------------------- */

/* SYS_KILL: Send signal to process */
static int32_t sys_kill(struct registers* regs) {
    uint32_t pid = regs->ebx;
    int sig = (int)regs->ecx;
    /* Phase 57 apps: a live user process is killed through the uproc path so
     * its address space, fds and waiting parent are handled; -1000 = not one. */
    { int32_t r = usys_kill(pid, sig); if (r != -1000) return r; }
    return process_send_signal(pid, sig);
}

/* SYS_PIPE: Create a pipe, returns pipe ID */
static int32_t sys_pipe(struct registers* regs) {
    (void)regs;
    return pipe_create();
}

/* SYS_GETENV: Get environment variable */
static int32_t sys_getenv(struct registers* regs) {
    const char* key = (const char*)regs->ebx;
    const char* val = env_get(key);
    return (int32_t)val; /* Returns pointer or NULL */
}

/* SYS_SETENV: Set environment variable */
static int32_t sys_setenv(struct registers* regs) {
    const char* key = (const char*)regs->ebx;
    const char* val = (const char*)regs->ecx;
    return env_set(key, val);
}

/* --------------------------------------------------------------------------
 * Phase 32 syscalls (19–21): Dynamic linking
 * -------------------------------------------------------------------------- */

/* SYS_DLOPEN: Load a shared library */
static int32_t sys_dlopen(struct registers* regs) {
    const char* name = (const char*)regs->ebx;
    int flags = (int)regs->ecx;
    return (int32_t)dl_open(name, flags);
}

/* SYS_DLSYM: Look up symbol in shared library */
static int32_t sys_dlsym(struct registers* regs) {
    uint32_t handle = regs->ebx;
    const char* name = (const char*)regs->ecx;
    return (int32_t)dl_sym(handle, name);
}

/* SYS_DLCLOSE: Close a shared library */
static int32_t sys_dlclose(struct registers* regs) {
    uint32_t handle = regs->ebx;
    return (int32_t)dl_close(handle);
}

/* --------------------------------------------------------------------------
 * Syscall table
 * -------------------------------------------------------------------------- */
typedef int32_t (*syscall_fn)(struct registers*);

#define NUM_SYSCALLS_TOTAL 24

static syscall_fn syscall_table[NUM_SYSCALLS_TOTAL] = {
    sys_write,      /* 0  */
    sys_read,       /* 1  */
    sys_open,       /* 2  */
    sys_close,      /* 3  */
    sys_create,     /* 4  */
    sys_exit,       /* 5  */
    sys_getpid,     /* 6  */
    sys_yield,      /* 7  */
    sys_malloc,     /* 8  */
    sys_free,       /* 9  */
    sys_exec,       /* 10 */
    sys_fork,       /* 11 */
    sys_shmget,     /* 12 */
    sys_shmat,      /* 13 */
    sys_mmap,       /* 14 */
    sys_kill,       /* 15 */
    sys_pipe,       /* 16 */
    sys_getenv,     /* 17 */
    sys_setenv,     /* 18 */
    sys_dlopen,     /* 19 */
    sys_dlsym,      /* 20 */
    sys_dlclose,    /* 21 */
    sys_sleep,      /* 22 */
    sys_sbrk,       /* 23 */
};

/* --------------------------------------------------------------------------
 * syscall_handler: INT 0x80 handler
 * -------------------------------------------------------------------------- */
static void syscall_handler(struct registers* regs) {
    uint32_t syscall_num = regs->eax;

    /* Mark that the kernel is now running on behalf of a user syscall, so that
     * a bad user pointer dereferenced below faults into a clean process kill
     * (uproc_fault) instead of halting the machine. */
    uproc_in_syscall++;

    if (syscall_num & 0x8000) {
        /* Win32 shim call issued by a per-process IAT stub. */
        regs->eax = win32_dispatch(syscall_num & 0x7FFF, regs);
    } else if (syscall_num < NUM_SYSCALLS_TOTAL && syscall_table[syscall_num] != NULL) {
        regs->eax = (uint32_t)syscall_table[syscall_num](regs);
    } else if (syscall_num < NX_SYS_COUNT) {
        /* Phase 57 apps: POSIX-ish calls 24+ (fd I/O, spawn/waitpid, termios,
         * dirs, time...) live in usys.c; they return -errno on failure. */
        regs->eax = (uint32_t)usys_syscall(syscall_num, regs);
    } else {
        regs->eax = (uint32_t)-1;
    }

    uproc_in_syscall--;
}

/* INT 0x80 entry stub (isr_asm.asm): standard ISR frame -> isr_handler ->
 * syscall_handler below. */
extern void isr128(void);

/* --------------------------------------------------------------------------
 * syscall_init: Register INT 0x80 handler (ring 3 accessible)
 * DPL=3 so ring 3 may `int 0x80`; a trap gate (0xEF, not 0xEE) so IF stays
 * set — sys_read waits for keyboard IRQs with hlt and would hang forever
 * behind an interrupt gate. Kernel code runs with IF=1 everywhere else too.
 * -------------------------------------------------------------------------- */
void syscall_init(void) {
    idt_set_gate(0x80, (uint32_t)isr128, 0x08, 0xEF);
    register_interrupt_handler(0x80, syscall_handler);

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Syscall interface: 24 calls + Win32 dispatch (ring 3)\n");
}
