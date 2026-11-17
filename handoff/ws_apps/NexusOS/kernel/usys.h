/* ============================================================================
 * NexusOS — POSIX-ish process plumbing for the userland — Phase 57 "apps"
 * ============================================================================
 * usys.c owns everything a C library needs beyond the Phase 54 basics:
 *   - per-process file-descriptor tables over ramfs, FAT32 (/disk), /proc,
 *     /dev/null, pipes and the two terminals (text console, GUI Terminal);
 *   - path resolution (absolute, cwd-relative, search path /disk/bin:/bin);
 *   - the int-0x80 calls numbered 24+ (see usys_abi.h);
 *   - spawn/waitpid, exit bookkeeping (zombie table), blocking + wake-ups;
 *   - terminal line discipline (cooked/raw, echo, Ctrl+C) in front of the
 *     VT100 engine (vt.c), for both terminals;
 *   - FPU/SSE context save/restore for user processes;
 *   - launching user programs from the kernel shell and the GUI Terminal.
 * ============================================================================ */
#ifndef USYS_H
#define USYS_H

#include "types.h"
#include "idt.h"
#include "process.h"
#include "vt.h"

#define USYS_TTY_CONSOLE 0
#define USYS_TTY_GUI     1

/* Called once from uproc_init(): enables the FPU/SSE for user processes and
 * clears the tables. */
void    usys_init(void);

/* Dispatcher for syscall numbers >= 24 (syscall.c). Returns eax. */
int32_t usys_syscall(uint32_t num, struct registers* regs);

/* NX_SYS_KILL (15) hook: kill a user process through the uproc path. Returns
 * -1000 if `pid` is not a live user process (caller falls back to signals). */
int32_t usys_kill(uint32_t pid, int sig);

/* ---- hooks for uproc.c -------------------------------------------------- */
void    usys_on_exit(process_t* p, uint32_t code);  /* before TERMINATED       */
void    usys_fpu_switch(process_t* cur, process_t* next);
bool    usys_fg_active(void);                        /* a job owns a terminal   */

/* ---- launching from the kernel shell (text console) --------------------- */
/* Resolve `name` (absolute, cwd-relative or bare search-path name) to the
 * program path it would run; false if there is no such user program. */
bool    usys_find_program(const char* name, const char* cwd, char* out, int max);
/* Run `line` ("prog args... [&]") on the console. Blocks (pumping the keyboard
 * and the idle hooks) until the job exits unless it ends with '&'. *found is
 * false when no such program exists (nothing was printed). Returns the exit
 * code (0 for background launches). */
int     usys_console_run(const char* line, const char* cwd, bool* found);
void    usys_cmd_apps(void);                          /* list /disk/bin        */
void    usys_cmd_which(const char* name, const char* cwd);
bool    usys_disk_ls(const char* path);               /* true if path is /disk */
bool    usys_disk_cat(const char* path);

/* ---- GUI Terminal foreground job (desktop.c) ----------------------------- */
void    usys_gui_resize(int cols, int rows);          /* content cell size     */
bool    usys_gui_launch(const char* line, const char* cwd); /* false: no program */
bool    usys_gui_job_active(void);
void    usys_gui_key(char key);                        /* keyboard -> job stdin */
void    usys_gui_paste(const char* s);
const vt_t* usys_gui_vt(void);                         /* screen for term_draw  */
bool    usys_gui_dirty(void);                          /* and clears the flag   */
void    usys_gui_kill(void);                           /* window closed / F12   */
/* True once after the GUI job exited; *code = exit status. */
bool    usys_gui_take_exit(int* code);

#endif /* USYS_H */
