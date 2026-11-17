/* ============================================================================
 * NexusOS — Registry Emulation (Header) — Phase 34
 * ============================================================================
 * Simple in-memory flat key-value store simulating the Windows Registry.
 * ============================================================================ */

#ifndef REGISTRY_H
#define REGISTRY_H

#include "types.h"
#include "win32.h" /* For BOOL, DWORD, types */

/* ============================================================================
 * Registry Types and Constants
 * ============================================================================ */

typedef HANDLE HKEY;

/* Root Keys */
#define HKEY_CLASSES_ROOT   ((HKEY)0x80000000)
#define HKEY_CURRENT_USER   ((HKEY)0x80000001)
#define HKEY_LOCAL_MACHINE  ((HKEY)0x80000002)
#define HKEY_USERS          ((HKEY)0x80000003)

/* Value Types */
#define REG_NONE            0
#define REG_SZ              1
#define REG_EXPAND_SZ       2
#define REG_BINARY          3
#define REG_DWORD           4
#define REG_DWORD_LITTLE_ENDIAN 4
#define REG_DWORD_BIG_ENDIAN 5
#define REG_LINK            6
#define REG_MULTI_SZ        7

/* Error Codes */
#define ERROR_SUCCESS       0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_ACCESS_DENIED 5
#define ERROR_INVALID_PARAMETER 87
#define ERROR_MORE_DATA     234

/* ============================================================================
 * Internal Entry Structure
 * ============================================================================ */
#define REG_KEY_LEN     128
#define REG_VALUE_LEN   256

typedef struct {
    bool     active;
    uint32_t hkey_root;
    char     path[REG_KEY_LEN];
    char     value_name[64];
    uint32_t type;
    uint8_t  data[REG_VALUE_LEN];
    uint32_t data_size;
} reg_entry_t;

/* ============================================================================
 * Registry API
 * ============================================================================ */

LONG RegOpenKeyExA(HKEY hKey, LPCSTR lpSubKey, DWORD ulOptions, DWORD samDesired, HKEY* phkResult);
LONG RegCreateKeyExA(HKEY hKey, LPCSTR lpSubKey, DWORD Reserved, LPSTR lpClass, DWORD dwOptions, DWORD samDesired, void* lpSecurityAttributes, HKEY* phkResult, DWORD* lpdwDisposition);
LONG RegCloseKey(HKEY hKey);
LONG RegQueryValueExA(HKEY hKey, LPCSTR lpValueName, DWORD* lpReserved, DWORD* lpType, BYTE* lpData, DWORD* lpcbData);
LONG RegSetValueExA(HKEY hKey, LPCSTR lpValueName, DWORD Reserved, DWORD dwType, const BYTE* lpData, DWORD cbData);

/* ============================================================================
 * Internal Subsystem
 * ============================================================================ */

void registry_init(void);

/* Used by shell command to show all entries */
int registry_get_entry_count(void);
const reg_entry_t* registry_get_entries(void);

#endif /* REGISTRY_H */
