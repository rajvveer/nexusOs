/* ============================================================================
 * NexusOS — Interactive Shell (Header)
 * ============================================================================ */

#ifndef SHELL_H
#define SHELL_H

/* Start the interactive shell (infinite loop) */
void shell_run(void);

/* Execute a single command line (used by the scripting engine's `run`). */
void shell_exec_line(const char* line);

#endif /* SHELL_H */
