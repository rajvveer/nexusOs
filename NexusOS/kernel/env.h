/* ============================================================================
 * NexusOS — Environment Variables (Header) — Phase 22
 * ============================================================================
 * Global environment variable table shared across processes.
 * ============================================================================ */

#ifndef ENV_H
#define ENV_H

#include "types.h"

/* Environment limits */
#define ENV_MAX_VARS    32
#define ENV_KEY_MAX     32
#define ENV_VALUE_MAX   128

/* Environment entry */
typedef struct {
    char key[ENV_KEY_MAX];
    char value[ENV_VALUE_MAX];
    bool active;
} env_var_t;

/* Initialize environment with defaults */
void env_init(void);

/* Set a variable (creates or updates) */
int env_set(const char* key, const char* value);

/* Get a variable value, returns NULL if not found */
const char* env_get(const char* key);

/* Unset (delete) a variable */
int env_unset(const char* key);

/* List all variables. Returns count. Calls callback for each. */
int env_list(void (*callback)(const char* key, const char* value));

/* Get count of active variables */
int env_count(void);

#endif /* ENV_H */
