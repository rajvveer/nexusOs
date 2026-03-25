/* ============================================================================
 * NexusOS — Environment Variables (Implementation) — Phase 22
 * ============================================================================
 * Global environment variable table with default system variables.
 * ============================================================================ */

#include "env.h"
#include "string.h"
#include "vga.h"

/* Environment table */
static env_var_t env_table[ENV_MAX_VARS];

/* --------------------------------------------------------------------------
 * env_init: Initialize with default environment variables
 * -------------------------------------------------------------------------- */
void env_init(void) {
    memset(env_table, 0, sizeof(env_table));

    /* Set default variables */
    env_set("PATH", "/bin:/usr/bin");
    env_set("HOME", "/home/nexus");
    env_set("USER", "nexus");
    env_set("SHELL", "/bin/nsh");
    env_set("TERM", "nexus-vt");
    env_set("OS", "NexusOS");
    env_set("LANG", "en_US.UTF-8");
    env_set("PWD", "/");
    env_set("HOSTNAME", "nexus-pc");

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Environment: 9 vars set\n");
}

/* --------------------------------------------------------------------------
 * env_set: Set or update an environment variable
 * -------------------------------------------------------------------------- */
int env_set(const char* key, const char* value) {
    if (!key || !value) return -1;
    if (strlen(key) >= ENV_KEY_MAX || strlen(value) >= ENV_VALUE_MAX) return -1;

    /* Check if key already exists */
    for (int i = 0; i < ENV_MAX_VARS; i++) {
        if (env_table[i].active && strcmp(env_table[i].key, key) == 0) {
            strncpy(env_table[i].value, value, ENV_VALUE_MAX - 1);
            env_table[i].value[ENV_VALUE_MAX - 1] = '\0';
            return 0;
        }
    }

    /* Find empty slot */
    for (int i = 0; i < ENV_MAX_VARS; i++) {
        if (!env_table[i].active) {
            strncpy(env_table[i].key, key, ENV_KEY_MAX - 1);
            env_table[i].key[ENV_KEY_MAX - 1] = '\0';
            strncpy(env_table[i].value, value, ENV_VALUE_MAX - 1);
            env_table[i].value[ENV_VALUE_MAX - 1] = '\0';
            env_table[i].active = true;
            return 0;
        }
    }

    return -1; /* Table full */
}

/* --------------------------------------------------------------------------
 * env_get: Get value of an environment variable
 * -------------------------------------------------------------------------- */
const char* env_get(const char* key) {
    if (!key) return NULL;
    for (int i = 0; i < ENV_MAX_VARS; i++) {
        if (env_table[i].active && strcmp(env_table[i].key, key) == 0) {
            return env_table[i].value;
        }
    }
    return NULL;
}

/* --------------------------------------------------------------------------
 * env_unset: Remove an environment variable
 * -------------------------------------------------------------------------- */
int env_unset(const char* key) {
    if (!key) return -1;
    for (int i = 0; i < ENV_MAX_VARS; i++) {
        if (env_table[i].active && strcmp(env_table[i].key, key) == 0) {
            env_table[i].active = false;
            return 0;
        }
    }
    return -1; /* Not found */
}

/* --------------------------------------------------------------------------
 * env_list: Iterate all active variables
 * -------------------------------------------------------------------------- */
int env_list(void (*callback)(const char* key, const char* value)) {
    int count = 0;
    for (int i = 0; i < ENV_MAX_VARS; i++) {
        if (env_table[i].active) {
            if (callback) callback(env_table[i].key, env_table[i].value);
            count++;
        }
    }
    return count;
}

/* --------------------------------------------------------------------------
 * env_count: Get number of active variables
 * -------------------------------------------------------------------------- */
int env_count(void) {
    int count = 0;
    for (int i = 0; i < ENV_MAX_VARS; i++) {
        if (env_table[i].active) count++;
    }
    return count;
}
