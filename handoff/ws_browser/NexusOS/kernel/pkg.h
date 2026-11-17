/* ============================================================================
 * NexusOS — Package Manager (Header) — Phase 35
 * ============================================================================
 * Implements the .npk (NexusOS Package) archive format, a bundled package
 * repository, recursive dependency resolution, and an install/remove engine
 * that extracts package payloads into the virtual filesystem.
 *
 * Pipeline for `npkg install <pkg>`:
 *   repo definition --[npk_serialize]--> .npk archive (in memory, "downloaded")
 *                   --[npk_validate]---> CRC32 integrity check
 *                   --[extract]--------> files written into the VFS
 *                   --[register]-------> recorded in the installed database
 * ============================================================================ */

#ifndef PKG_H
#define PKG_H

#include "types.h"
#include "vfs.h" /* FS_NAME_MAX */

/* ============================================================================
 * Limits
 * ============================================================================ */
#define PKG_NAME_MAX        32
#define PKG_VER_MAX         16
#define PKG_DESC_MAX        64
#define PKG_AUTHOR_MAX      32
#define PKG_MAX_DEPS        8                /* dependencies per package      */
#define PKG_MAX_FILES       8                /* payload files per package     */
#define PKG_FILE_NAME_MAX   FS_NAME_MAX      /* matches VFS name limit (32)   */
#define PKG_MAX_INSTALLED   32               /* installed-database capacity   */

/* ============================================================================
 * .npk Archive Format (on-"disk" layout)
 * ----------------------------------------------------------------------------
 *   [ npk_header_t ]
 *   [ npk_dep_t  x dep_count  ]
 *   [ npk_file_t x file_count ]
 *   [ payload: concatenated file bytes (payload_size total) ]
 *
 * header_crc is a CRC32 over every byte that follows the header (the dep
 * table, file table, and payload). Each file additionally carries its own
 * CRC32 so a single corrupt member can be pinpointed.
 * ============================================================================ */

#define NPK_MAGIC        0x314B504E          /* 'N','P','K','1' little-endian */
#define NPK_FORMAT_VER   1

typedef struct __attribute__((packed)) {
    uint32_t magic;                          /* NPK_MAGIC                     */
    uint16_t format_version;                 /* NPK_FORMAT_VER                */
    uint16_t flags;                          /* reserved, must be 0           */
    char     name[PKG_NAME_MAX];
    char     version[PKG_VER_MAX];
    char     description[PKG_DESC_MAX];
    char     author[PKG_AUTHOR_MAX];
    uint16_t dep_count;
    uint16_t file_count;
    uint32_t payload_size;                   /* total bytes of all file data  */
    uint32_t header_crc;                     /* CRC32 of all bytes after here */
} npk_header_t;

typedef struct __attribute__((packed)) {
    char     name[PKG_NAME_MAX];
    char     min_version[PKG_VER_MAX];
} npk_dep_t;

typedef struct __attribute__((packed)) {
    char     name[PKG_FILE_NAME_MAX];
    uint32_t size;                           /* file length in bytes          */
    uint32_t offset;                         /* offset within payload section */
    uint32_t crc;                            /* CRC32 of this file's bytes    */
} npk_file_t;

/* ============================================================================
 * Repository Definition (the in-kernel source of truth for the bundled repo)
 * ============================================================================ */

typedef struct {
    const char* name;
    const char* min_version;
} pkg_dep_def_t;

typedef struct {
    const char* name;
    const char* content;                     /* text payload for the file     */
} pkg_file_def_t;

typedef struct {
    const char*    name;
    const char*    version;
    const char*    description;
    const char*    author;
    pkg_dep_def_t  deps[PKG_MAX_DEPS];       /* terminated by a {NULL,...}     */
    pkg_file_def_t files[PKG_MAX_FILES];     /* terminated by a {NULL,...}     */
} pkg_def_t;

/* ============================================================================
 * Installed Database Entry
 * ============================================================================ */

typedef struct {
    bool     active;
    char     name[PKG_NAME_MAX];
    char     version[PKG_VER_MAX];
    uint16_t file_count;
    char     files[PKG_MAX_FILES][PKG_FILE_NAME_MAX];
    uint32_t install_size;
} pkg_installed_t;

/* ============================================================================
 * Result Codes
 * ============================================================================ */
#define PKG_OK                 0
#define PKG_ERR_NOT_FOUND      1             /* not present in the repository */
#define PKG_ERR_ALREADY        2             /* already installed             */
#define PKG_ERR_NOT_INSTALLED  3
#define PKG_ERR_DEP_FAILED     4             /* a dependency could not install */
#define PKG_ERR_NOMEM          5
#define PKG_ERR_CORRUPT        6             /* bad magic / CRC mismatch      */
#define PKG_ERR_FS             7             /* filesystem write failed       */
#define PKG_ERR_DB_FULL        8             /* installed database is full    */
#define PKG_ERR_CYCLE          9             /* circular dependency           */
#define PKG_ERR_CONFLICT       10            /* installed dep too old         */

/* ============================================================================
 * Public API
 * ============================================================================ */

/* Initialize the package manager (resets the installed database). */
void pkg_init(void);

/* --- Repository queries ----------------------------------------------------*/
int               pkg_repo_count(void);
const pkg_def_t*  pkg_repo_get(int index);
const pkg_def_t*  pkg_repo_find(const char* name);
uint32_t          pkg_repo_revision(void);

/* --- Installed-database queries -------------------------------------------*/
int                     pkg_installed_count(void);
const pkg_installed_t*  pkg_installed_get(int index);
const pkg_installed_t*  pkg_installed_find(const char* name);
bool                    pkg_is_installed(const char* name);

/* --- Operations (print human-readable progress, return a PKG_* code) ------*/
int  pkg_install(const char* name);
int  pkg_remove(const char* name);
void pkg_update(void);                       /* refresh the repository index  */

/* --- Helpers ---------------------------------------------------------------*/
const char* pkg_strerror(int code);
int         pkg_version_cmp(const char* a, const char* b);   /* -1 / 0 / 1    */
uint32_t    pkg_crc32(const void* data, uint32_t len);

/* --- .npk codec (exposed for the installer and for inspection) ------------*/
/* Serialize a repository definition into a freshly kmalloc'd .npk archive.
 * On PKG_OK the caller owns *out_buf and must kfree() it.                    */
int  npk_serialize(const pkg_def_t* def, uint8_t** out_buf, uint32_t* out_size);
/* Validate magic, version, sizes, header CRC, and every per-file CRC.       */
int  npk_validate(const uint8_t* buf, uint32_t size);

#endif /* PKG_H */
