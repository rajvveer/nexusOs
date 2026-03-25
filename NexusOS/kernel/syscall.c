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

/* --------------------------------------------------------------------------
 * Existing syscall implementations (0–14)
 * -------------------------------------------------------------------------- */

static int32_t sys_write(struct registers* regs) {
    char* str = (char*)regs->ebx;
    uint32_t len = regs->ecx;
    (void)len;
    vga_print(str);
    return 0;
}

static int32_t sys_read(struct registers* regs) {
    char* buffer = (char*)regs->ebx;
    uint32_t max_len = regs->ecx;
    keyboard_readline(buffer, max_len);
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
    (void)regs;
    process_exit();
    return 0;
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

#define NUM_SYSCALLS_TOTAL 22

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
};

/* --------------------------------------------------------------------------
 * syscall_handler: INT 0x80 handler
 * -------------------------------------------------------------------------- */
static void syscall_handler(struct registers* regs) {
    uint32_t syscall_num = regs->eax;

    if (syscall_num < NUM_SYSCALLS_TOTAL && syscall_table[syscall_num] != NULL) {
        int32_t ret = syscall_table[syscall_num](regs);
        regs->eax = (uint32_t)ret;
    } else {
        regs->eax = (uint32_t)-1;
    }
}

/* --------------------------------------------------------------------------
 * syscall_init: Register INT 0x80 handler (ring 3 accessible)
 * -------------------------------------------------------------------------- */
void syscall_init(void) {
    idt_set_gate(0x80, 0, 0x08, 0xEE);
    register_interrupt_handler(0x80, syscall_handler);

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Syscall interface: 22 calls (ring 3)\n");
}
