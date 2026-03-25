/* ============================================================================
 * NexusOS — Process Manager Header — Phase 22
 * ============================================================================ */

#ifndef PROCESS_H
#define PROCESS_H

#include "types.h"

/* Process limits */
#define MAX_PROCESSES   16
#define PROC_STACK_SIZE 4096    /* 4KB kernel stack per process */
#define PROC_MAX_LIBS   8       /* Max shared libraries per process */
#define PROC_NAME_MAX   32

/* Signal definitions (POSIX-like) */
#define SIGHUP     1
#define SIGINT     2
#define SIGQUIT    3
#define SIGILL     4
#define SIGABRT    6
#define SIGFPE     8
#define SIGKILL    9    /* Cannot be caught or ignored */
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15    /* Polite kill */
#define SIGCHLD   17
#define SIGCONT   18    /* Continue stopped process */
#define SIGSTOP   19    /* Cannot be caught or ignored */
#define SIGTSTP   20
#define MAX_SIGNALS 32

/* Signal handler types */
#define SIG_DFL  ((sig_handler_t)0)   /* Default action */
#define SIG_IGN  ((sig_handler_t)1)   /* Ignore signal */

typedef void (*sig_handler_t)(int);

/* Process states */
typedef enum {
    PROC_UNUSED = 0,       /* Slot is empty */
    PROC_READY,            /* Ready to run */
    PROC_RUNNING,          /* Currently executing */
    PROC_BLOCKED,          /* Waiting for something */
    PROC_STOPPED,          /* Stopped by SIGSTOP/SIGTSTP */
    PROC_TERMINATED        /* Finished execution */
} proc_state_t;

/* Process Control Block */
typedef struct process {
    uint32_t      pid;
    char          name[PROC_NAME_MAX];
    proc_state_t  state;

    /* Saved context */
    uint32_t      esp;          /* Saved stack pointer */
    uint32_t      ebp;          /* Saved base pointer */
    uint32_t      eip;          /* Entry point (for initial setup) */

    /* Stack */
    uint32_t      stack_base;   /* Bottom of allocated stack */
    uint32_t      stack_top;    /* Top of stack (stack_base + PROC_STACK_SIZE) */

    /* Phase 20: User-mode fields */
    bool          is_user;      /* True if ring 3 process */
    uint32_t      user_stack;   /* User-mode stack top */

    /* Phase 22: Signals */
    uint32_t      pending_signals;              /* Bitmask of pending signals */
    sig_handler_t signal_handlers[MAX_SIGNALS]; /* Per-signal handlers */
    uint32_t      pgid;                         /* Process group ID */
    uint32_t      ppid;                         /* Parent process ID */

    /* Phase 32: Loaded shared libraries */
    uint32_t      loaded_libs[PROC_MAX_LIBS];    /* Library handles */
    uint32_t      num_libs;                      /* Number of loaded libs */

    /* Statistics */
    uint32_t      ticks;        /* CPU ticks consumed */
} process_t;

/* Process management */
void        process_init(void);
process_t*  process_create(const char* name, void (*entry)(void));
void        process_exit(void);
void        process_terminate(uint32_t pid);
process_t*  process_get_current(void);
void        process_set_current(process_t* proc);
process_t*  process_get_by_pid(uint32_t pid);
uint32_t    process_count(void);
process_t*  process_get_table(void);

/* Signal management */
int         process_send_signal(uint32_t pid, int sig);
int         process_kill_group(uint32_t pgid, int sig);
void        process_set_group(uint32_t pid, uint32_t pgid);
void        process_check_signals(process_t* proc);
sig_handler_t process_set_signal_handler(int sig, sig_handler_t handler);

#endif /* PROCESS_H */
