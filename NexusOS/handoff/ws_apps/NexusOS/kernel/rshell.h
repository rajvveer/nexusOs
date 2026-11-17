/* ============================================================================
 * NexusOS — Remote Shell Header — Phase 30
 * ============================================================================
 * Telnet-style remote shell over TCP for network administration.
 * ============================================================================ */

#ifndef RSHELL_H
#define RSHELL_H

#include "types.h"

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */
#define RSHELL_PORT         2323
#define RSHELL_MAX_CMD      128

/* --------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------- */

/* Initialize remote shell */
void rshell_init(void);

/* Start listening */
void rshell_start(void);

/* Stop the shell server */
void rshell_stop(void);

/* Poll for connections and input (call periodically) */
void rshell_poll(void);

/* Check if running */
bool rshell_is_running(void);

#endif /* RSHELL_H */
