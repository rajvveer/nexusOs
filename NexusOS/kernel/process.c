/* ============================================================================
 * NexusOS — Process Manager (Implementation) — Phase 22
 * ============================================================================
 * Manages processes with signals, process groups, and lifecycle.
 * ============================================================================ */

#include "process.h"
#include "heap.h"
#include "string.h"
#include "vga.h"

/* Process table */
static process_t proc_table[MAX_PROCESSES];
static process_t* current_process = NULL;
static uint32_t next_pid = 1;

/* --------------------------------------------------------------------------
 * process_init: Initialize the process manager
 * -------------------------------------------------------------------------- */
void process_init(void) {
    memset(proc_table, 0, sizeof(proc_table));
    current_process = NULL;
    next_pid = 1;

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Process manager (signals, groups)\n");
}

/* --------------------------------------------------------------------------
 * process_create: Create a new process
 * -------------------------------------------------------------------------- */
process_t* process_create(const char* name, void (*entry)(void)) {
    int slot = -1;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proc_table[i].state == PROC_UNUSED || proc_table[i].state == PROC_TERMINATED) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        vga_print_color("[ERROR] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print("Process table full!\n");
        return NULL;
    }

    process_t* proc = &proc_table[slot];
    memset(proc, 0, sizeof(process_t));

    proc->pid = next_pid++;
    strncpy(proc->name, name, PROC_NAME_MAX - 1);
    proc->name[PROC_NAME_MAX - 1] = '\0';

    /* Allocate kernel stack */
    proc->stack_base = (uint32_t)kmalloc(PROC_STACK_SIZE);
    if (proc->stack_base == 0) {
        vga_print_color("[ERROR] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print("Failed to allocate process stack!\n");
        return NULL;
    }
    proc->stack_top = proc->stack_base + PROC_STACK_SIZE;

    /* Set up initial stack frame */
    uint32_t* stack_ptr = (uint32_t*)proc->stack_top;
    *(--stack_ptr) = (uint32_t)entry;     /* EIP */
    *(--stack_ptr) = 0;                    /* Dummy return */
    *(--stack_ptr) = 0;                    /* EBP */
    *(--stack_ptr) = 0;                    /* EBX */
    *(--stack_ptr) = 0;                    /* ESI */
    *(--stack_ptr) = 0;                    /* EDI */

    proc->esp = (uint32_t)stack_ptr;
    proc->ebp = 0;
    proc->eip = (uint32_t)entry;
    proc->state = PROC_READY;
    proc->ticks = 0;

    /* Phase 22: Initialize signals */
    proc->pending_signals = 0;
    proc->pgid = proc->pid; /* Default: own process group */
    proc->ppid = current_process ? current_process->pid : 0;
    for (int i = 0; i < MAX_SIGNALS; i++) {
        proc->signal_handlers[i] = SIG_DFL;
    }

    return proc;
}

/* --------------------------------------------------------------------------
 * process_exit: Terminate the currently running process
 * -------------------------------------------------------------------------- */
void process_exit(void) {
    if (current_process != NULL) {
        current_process->state = PROC_TERMINATED;
        if (current_process->stack_base != 0) {
            kfree((void*)current_process->stack_base);
            current_process->stack_base = 0;
        }
        __asm__ volatile("int $0x20");
    }
}

/* --------------------------------------------------------------------------
 * process_terminate: Kill a process by PID
 * -------------------------------------------------------------------------- */
void process_terminate(uint32_t pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proc_table[i].pid == pid && proc_table[i].state != PROC_UNUSED) {
            proc_table[i].state = PROC_TERMINATED;
            if (proc_table[i].stack_base != 0) {
                kfree((void*)proc_table[i].stack_base);
                proc_table[i].stack_base = 0;
            }
            return;
        }
    }
}

/* --------------------------------------------------------------------------
 * Signal Management
 * -------------------------------------------------------------------------- */

int process_send_signal(uint32_t pid, int sig) {
    if (sig < 0 || sig >= MAX_SIGNALS) return -1;

    process_t* proc = process_get_by_pid(pid);
    if (!proc) return -1;

    /* SIGKILL and SIGSTOP cannot be caught */
    if (sig == SIGKILL) {
        process_terminate(pid);
        return 0;
    }

    if (sig == SIGSTOP) {
        proc->state = PROC_STOPPED;
        return 0;
    }

    if (sig == SIGCONT) {
        if (proc->state == PROC_STOPPED) {
            proc->state = PROC_READY;
        }
        return 0;
    }

    /* Set pending signal bit */
    proc->pending_signals |= (1U << sig);
    return 0;
}

int process_kill_group(uint32_t pgid, int sig) {
    int count = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proc_table[i].state != PROC_UNUSED &&
            proc_table[i].state != PROC_TERMINATED &&
            proc_table[i].pgid == pgid) {
            process_send_signal(proc_table[i].pid, sig);
            count++;
        }
    }
    return count;
}

void process_set_group(uint32_t pid, uint32_t pgid) {
    process_t* proc = process_get_by_pid(pid);
    if (proc) proc->pgid = pgid;
}

void process_check_signals(process_t* proc) {
    if (!proc || proc->pending_signals == 0) return;

    for (int sig = 1; sig < MAX_SIGNALS; sig++) {
        if (!(proc->pending_signals & (1U << sig))) continue;

        /* Clear pending bit */
        proc->pending_signals &= ~(1U << sig);

        sig_handler_t handler = proc->signal_handlers[sig];

        if (handler == SIG_IGN) {
            continue; /* Ignored */
        }

        if (handler == SIG_DFL) {
            /* Default action depends on signal */
            switch (sig) {
                case SIGTERM:
                case SIGINT:
                case SIGQUIT:
                case SIGABRT:
                case SIGSEGV:
                case SIGFPE:
                case SIGILL:
                case SIGPIPE:
                    process_terminate(proc->pid);
                    return;
                case SIGTSTP:
                    proc->state = PROC_STOPPED;
                    return;
                case SIGCHLD:
                case SIGUSR1:
                case SIGUSR2:
                    break; /* Default: ignore */
                default:
                    break;
            }
        } else {
            /* Call custom handler */
            handler(sig);
        }
    }
}

sig_handler_t process_set_signal_handler(int sig, sig_handler_t handler) {
    if (!current_process || sig < 1 || sig >= MAX_SIGNALS) return SIG_DFL;

    /* SIGKILL and SIGSTOP cannot have custom handlers */
    if (sig == SIGKILL || sig == SIGSTOP) return SIG_DFL;

    sig_handler_t old = current_process->signal_handlers[sig];
    current_process->signal_handlers[sig] = handler;
    return old;
}

/* --------------------------------------------------------------------------
 * Accessors
 * -------------------------------------------------------------------------- */
process_t* process_get_current(void) { return current_process; }

void process_set_current(process_t* proc) { current_process = proc; }

process_t* process_get_by_pid(uint32_t pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proc_table[i].pid == pid &&
            proc_table[i].state != PROC_UNUSED &&
            proc_table[i].state != PROC_TERMINATED) {
            return &proc_table[i];
        }
    }
    return NULL;
}

uint32_t process_count(void) {
    uint32_t count = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proc_table[i].state == PROC_READY || proc_table[i].state == PROC_RUNNING) {
            count++;
        }
    }
    return count;
}

process_t* process_get_table(void) { return proc_table; }
