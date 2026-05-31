/* ============================================================================
 * NexusOS — Registry Emulation (Implementation) — Phase 34
 * ============================================================================
 * Implements a flat key-value store simulating the Windows Registry.
 * ============================================================================ */

#include "registry.h"
#include "vga.h"
#include "string.h"
#include "heap.h"

#define REG_MAX_KEYS 64

static reg_entry_t registry_store[REG_MAX_KEYS];
static int registry_count = 0;

/* Virtual handle mapping for open keys (we just use an index + offset as HANDLE) */
#define HKEY_MAGIC 0x52454700 // "REG"

typedef struct {
    bool     active;
    HKEY     handle;
    uint32_t root;
    char     path[REG_KEY_LEN];
} open_key_t;

#define MAX_OPEN_KEYS 16
static open_key_t open_keys[MAX_OPEN_KEYS];
static uint32_t next_hkey_id = 1;

/* --------------------------------------------------------------------------
 * Internal Helpers
 * -------------------------------------------------------------------------- */

static reg_entry_t* find_entry(uint32_t root, const char* path, const char* value_name) {
    if (!path) path = "";
    if (!value_name) value_name = "";
    
    for (int i = 0; i < REG_MAX_KEYS; i++) {
        if (registry_store[i].active && 
            registry_store[i].hkey_root == root &&
            strcmp(registry_store[i].path, path) == 0 &&
            strcmp(registry_store[i].value_name, value_name) == 0) {
            return &registry_store[i];
        }
    }
    return NULL;
}

static reg_entry_t* alloc_entry(void) {
    for (int i = 0; i < REG_MAX_KEYS; i++) {
        if (!registry_store[i].active) {
            registry_store[i].active = true;
            registry_count++;
            return &registry_store[i];
        }
    }
    return NULL;
}

static open_key_t* find_open_key(HKEY h) {
    for (int i = 0; i < MAX_OPEN_KEYS; i++) {
        if (open_keys[i].active && open_keys[i].handle == h)
            return &open_keys[i];
    }
    return NULL;
}

/* --------------------------------------------------------------------------
 * Populate default entries
 * -------------------------------------------------------------------------- */

static void set_default_string(uint32_t root, const char* path, const char* name, const char* value) {
    reg_entry_t* e = alloc_entry();
    if (!e) return;
    
    e->hkey_root = root;
    strncpy(e->path, path, REG_KEY_LEN - 1);
    e->path[REG_KEY_LEN - 1] = '\0';
    
    strncpy(e->value_name, name, 63);
    e->value_name[63] = '\0';
    
    e->type = REG_SZ;
    uint32_t len = strlen(value) + 1; /* include null terminator */
    if (len > REG_VALUE_LEN) len = REG_VALUE_LEN;
    
    memcpy(e->data, value, len);
    e->data_size = len;
}

/* --------------------------------------------------------------------------
 * API Implementation
 * -------------------------------------------------------------------------- */

LONG RegOpenKeyExA(HKEY hKey, LPCSTR lpSubKey, DWORD ulOptions, DWORD samDesired, HKEY* phkResult) {
    (void)ulOptions; (void)samDesired;
    if (!phkResult) return ERROR_INVALID_PARAMETER;
    
    uint32_t root = 0;
    char base_path[REG_KEY_LEN] = {0};
    
    /* Determine root and base path */
    if (hKey == HKEY_CLASSES_ROOT || hKey == HKEY_CURRENT_USER || 
        hKey == HKEY_LOCAL_MACHINE || hKey == HKEY_USERS) {
        root = (uint32_t)hKey;
    } else {
        open_key_t* ok = find_open_key(hKey);
        if (!ok) return ERROR_INVALID_PARAMETER;
        root = ok->root;
        strncpy(base_path, ok->path, REG_KEY_LEN - 1);
    }
    
    /* Append subkey */
    if (lpSubKey && *lpSubKey) {
        if (base_path[0] != '\0') {
            strcat(base_path, "\\");
        }
        strcat(base_path, lpSubKey);
    }
    
    /* Open the key */
    int slot = -1;
    for (int i = 0; i < MAX_OPEN_KEYS; i++) {
        if (!open_keys[i].active) { slot = i; break; }
    }
    if (slot < 0) return ERROR_MORE_DATA; /* No free handles */
    
    open_key_t* ok = &open_keys[slot];
    ok->active = true;
    ok->handle = (HKEY)(HKEY_MAGIC | next_hkey_id++);
    ok->root = root;
    strncpy(ok->path, base_path, REG_KEY_LEN - 1);
    ok->path[REG_KEY_LEN - 1] = '\0';
    
    *phkResult = ok->handle;
    return ERROR_SUCCESS;
}

LONG RegCreateKeyExA(HKEY hKey, LPCSTR lpSubKey, DWORD Reserved, LPSTR lpClass, 
                     DWORD dwOptions, DWORD samDesired, void* lpSecurityAttributes, 
                     HKEY* phkResult, DWORD* lpdwDisposition) {
    (void)Reserved; (void)lpClass; (void)dwOptions; (void)samDesired; (void)lpSecurityAttributes;
    
    LONG res = RegOpenKeyExA(hKey, lpSubKey, 0, 0, phkResult);
    if (lpdwDisposition) {
        *lpdwDisposition = 1; /* REG_CREATED_NEW_KEY (dummy) */
    }
    return res;
}

LONG RegCloseKey(HKEY hKey) {
    if (hKey == HKEY_CLASSES_ROOT || hKey == HKEY_CURRENT_USER || 
        hKey == HKEY_LOCAL_MACHINE || hKey == HKEY_USERS) {
        return ERROR_SUCCESS; /* Can't close root keys */
    }
    
    open_key_t* ok = find_open_key(hKey);
    if (!ok) return ERROR_INVALID_PARAMETER;
    
    ok->active = false;
    return ERROR_SUCCESS;
}

LONG RegQueryValueExA(HKEY hKey, LPCSTR lpValueName, DWORD* lpReserved, 
                      DWORD* lpType, BYTE* lpData, DWORD* lpcbData) {
    (void)lpReserved;
    open_key_t* ok = find_open_key(hKey);
    if (!ok) return ERROR_INVALID_PARAMETER;
    
    reg_entry_t* e = find_entry(ok->root, ok->path, lpValueName);
    if (!e) return ERROR_FILE_NOT_FOUND;
    
    if (lpType) *lpType = e->type;
    
    if (lpData && lpcbData) {
        if (*lpcbData < e->data_size) {
            *lpcbData = e->data_size;
            return ERROR_MORE_DATA;
        }
        memcpy(lpData, e->data, e->data_size);
        *lpcbData = e->data_size;
    } else if (lpcbData) {
        *lpcbData = e->data_size;
    }
    
    return ERROR_SUCCESS;
}

LONG RegSetValueExA(HKEY hKey, LPCSTR lpValueName, DWORD Reserved, 
                    DWORD dwType, const BYTE* lpData, DWORD cbData) {
    (void)Reserved;
    open_key_t* ok = find_open_key(hKey);
    if (!ok) return ERROR_INVALID_PARAMETER;
    if (!lpData) return ERROR_INVALID_PARAMETER;
    
    reg_entry_t* e = find_entry(ok->root, ok->path, lpValueName);
    if (!e) {
        e = alloc_entry();
        if (!e) return ERROR_MORE_DATA; /* No space */
        e->hkey_root = ok->root;
        strncpy(e->path, ok->path, REG_KEY_LEN - 1);
        e->path[REG_KEY_LEN - 1] = '\0';
        if (lpValueName) {
            strncpy(e->value_name, lpValueName, 63);
            e->value_name[63] = '\0';
        } else {
            e->value_name[0] = '\0';
        }
    }
    
    e->type = dwType;
    uint32_t len = cbData;
    if (len > REG_VALUE_LEN) len = REG_VALUE_LEN;
    memcpy(e->data, lpData, len);
    e->data_size = len;
    
    return ERROR_SUCCESS;
}

/* --------------------------------------------------------------------------
 * Subsystem
 * -------------------------------------------------------------------------- */

void registry_init(void) {
    memset(registry_store, 0, sizeof(registry_store));
    memset(open_keys, 0, sizeof(open_keys));
    registry_count = 0;
    next_hkey_id = 1;

    /* Pre-populate some keys typical apps look for */
    set_default_string((uint32_t)HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "ProductName", "NexusOS Win32 Shield");
    set_default_string((uint32_t)HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "CurrentVersion", "10.0");
    
    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Registry initialized (");
    char buf[12];
    int_to_str(registry_count, buf);
    vga_print(buf);
    vga_print(" default entries)\n");
}

int registry_get_entry_count(void) {
    return registry_count;
}

const reg_entry_t* registry_get_entries(void) {
    return registry_store;
}
