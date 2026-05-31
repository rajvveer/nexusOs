/* ============================================================================
 * NexusOS — Package Manager (Implementation) — Phase 35
 * ============================================================================
 * A real package pipeline running entirely on bare metal:
 *
 *   1. Packages live in a bundled repository (pkg_repo[]) as definitions.
 *   2. `npkg install` serializes the definition into a binary .npk archive
 *      (npk_serialize) — this is the same blob a remote mirror would ship.
 *   3. The archive is verified with CRC32 (npk_validate) before anything is
 *      written, so a corrupt download is rejected.
 *   4. The payload is extracted into the VFS, and the package is recorded in
 *      the installed database so it can be cleanly removed later.
 *
 * Dependencies are resolved recursively (depth-first, deps installed first)
 * with cycle detection and minimum-version checking.
 * ============================================================================ */

#include "pkg.h"
#include "ramfs.h"
#include "heap.h"
#include "string.h"
#include "vga.h"
#include "http.h"

/* Remote mirror probed by `npkg update`. Reaching it is best-effort; the
 * bundled repository below is the authoritative local mirror either way. */
#define PKG_REPO_URL "http://repo.nexusos.org/index.npk"

/* ============================================================================
 * Bundled Repository
 * ----------------------------------------------------------------------------
 * Dependency graph exercised by the resolver:
 *     doom  -> sdl-shim -> libnx
 *     doom  -> libnx
 *     nano  -> libnx
 *     nxedit-> libnx
 * Installing `doom` therefore pulls in sdl-shim and libnx automatically.
 * ============================================================================ */

static const pkg_def_t pkg_repo[] = {
    {
        "libnx", "1.0.0", "NexusOS base runtime shared library", "NexusOS Core",
        { {NULL, NULL} },
        {
            { "libnx.info",
              "libnx 1.0.0\n"
              "Base runtime library for NexusOS packages.\n"
              "Provides the shared symbols every npkg app links against.\n" },
            { NULL, NULL }
        }
    },
    {
        "coreutils", "1.2.0", "Essential command-line utilities", "NexusOS Core",
        { {NULL, NULL} },
        {
            { "coreutils.info",
              "coreutils 1.2.0\n"
              "Bundle of essential userland tools: ls, cat, grep, wc.\n" },
            { NULL, NULL }
        }
    },
    {
        "hello", "1.0.0", "Classic hello-world demo package", "NexusOS Labs",
        { {NULL, NULL} },
        {
            { "hello.txt",
              "Hello from the NexusOS package manager!\n"
              "This file was installed by `npkg install hello`.\n"
              "Run `npkg remove hello` to take it back out.\n" },
            { NULL, NULL }
        }
    },
    {
        "fetch", "1.1.0", "System information banner tool", "NexusOS Labs",
        { {NULL, NULL} },
        {
            { "fetch.info",
              "fetch 1.1.0\n"
              "OS:     NexusOS\n"
              "Kernel: hybrid x86 (i386)\n"
              "Shell:  NexusOS Shell\n"
              "WM:     NexusOS Window Manager\n" },
            { NULL, NULL }
        }
    },
    {
        "nano", "2.1.0", "Lightweight console text editor", "NexusOS Apps",
        { { "libnx", "1.0.0" }, {NULL, NULL} },
        {
            { "nano.info",
              "nano 2.1.0\n"
              "A small, friendly text editor for NexusOS.\n"
              "Depends on libnx >= 1.0.0.\n" },
            { NULL, NULL }
        }
    },
    {
        "nxedit", "3.0.0", "Advanced NexusOS code editor", "NexusOS Apps",
        { { "libnx", "1.0.0" }, {NULL, NULL} },
        {
            { "nxedit.info",
              "nxedit 3.0.0\n"
              "Syntax-highlighting editor with multi-buffer support.\n"
              "Depends on libnx >= 1.0.0.\n" },
            { NULL, NULL }
        }
    },
    {
        "sdl-shim", "0.9.0", "SDL-compatible multimedia shim", "NexusOS Games",
        { { "libnx", "1.0.0" }, {NULL, NULL} },
        {
            { "sdl-shim.info",
              "sdl-shim 0.9.0\n"
              "Provides an SDL-like surface/event/audio API for games.\n"
              "Depends on libnx >= 1.0.0.\n" },
            { NULL, NULL }
        }
    },
    {
        "doom", "1.6.6", "The classic DOOM port (preview)", "id Software / port",
        { { "sdl-shim", "0.9.0" }, { "libnx", "1.0.0" }, {NULL, NULL} },
        {
            { "doom.info",
              "DOOM 1.666 (NexusOS preview)\n"
              "The true OS benchmark. Requires sdl-shim and libnx.\n"
              "Full playable port arrives with the Phase 42 gaming framework.\n" },
            { NULL, NULL }
        }
    },
};

#define PKG_REPO_COUNT ((int)(sizeof(pkg_repo) / sizeof(pkg_repo[0])))

/* ============================================================================
 * State
 * ============================================================================ */

static pkg_installed_t installed_db[PKG_MAX_INSTALLED];
static uint32_t        repo_rev = 1;          /* bumped by `npkg update`       */

/* Names currently being resolved — used for cycle detection. */
#define PKG_MAX_RESOLVE 16
static char resolving[PKG_MAX_RESOLVE][PKG_NAME_MAX];
static int  resolving_depth = 0;

/* ============================================================================
 * Small output helpers (keep the install log tidy and consistent)
 * ============================================================================ */

static void pr(const char* s)               { vga_print(s); }
static void prc(const char* s, uint8_t col)  { vga_print_color(s, col); }

static void pr_num(uint32_t n) {
    char buf[12];
    int_to_str((int)n, buf);
    vga_print(buf);
}

/* ============================================================================
 * CRC32 (IEEE 802.3 polynomial, table-free)
 * ============================================================================ */

uint32_t pkg_crc32(const void* data, uint32_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = (crc & 1u) ? 0xEDB88320u : 0u;
            crc = (crc >> 1) ^ mask;
        }
    }
    return ~crc;
}

/* ============================================================================
 * Version comparison (dotted numeric, up to three components)
 * ============================================================================ */

static int ver_next(const char** p) {
    int v = 0;
    while (**p >= '0' && **p <= '9') {
        v = v * 10 + (**p - '0');
        (*p)++;
    }
    if (**p == '.') (*p)++;
    return v;
}

int pkg_version_cmp(const char* a, const char* b) {
    if (!a) a = "0";
    if (!b) b = "0";
    for (int i = 0; i < 3; i++) {
        int va = ver_next(&a);
        int vb = ver_next(&b);
        if (va < vb) return -1;
        if (va > vb) return 1;
    }
    return 0;
}

/* ============================================================================
 * Field helpers
 * ============================================================================ */

static void copy_field(char* dst, const char* src, int cap) {
    if (!src) src = "";
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

static int def_dep_count(const pkg_def_t* d) {
    int n = 0;
    while (n < PKG_MAX_DEPS && d->deps[n].name) n++;
    return n;
}

static int def_file_count(const pkg_def_t* d) {
    int n = 0;
    while (n < PKG_MAX_FILES && d->files[n].name) n++;
    return n;
}

/* ============================================================================
 * .npk Codec
 * ============================================================================ */

int npk_serialize(const pkg_def_t* def, uint8_t** out_buf, uint32_t* out_size) {
    if (!def || !out_buf || !out_size) return PKG_ERR_NOMEM;

    int ndeps  = def_dep_count(def);
    int nfiles = def_file_count(def);

    uint32_t payload = 0;
    for (int i = 0; i < nfiles; i++)
        payload += (uint32_t)strlen(def->files[i].content);

    uint32_t total = sizeof(npk_header_t)
                   + (uint32_t)ndeps  * sizeof(npk_dep_t)
                   + (uint32_t)nfiles * sizeof(npk_file_t)
                   + payload;

    uint8_t* buf = (uint8_t*)kmalloc(total);
    if (!buf) return PKG_ERR_NOMEM;
    memset(buf, 0, total);

    /* Header */
    npk_header_t* h = (npk_header_t*)buf;
    h->magic          = NPK_MAGIC;
    h->format_version = NPK_FORMAT_VER;
    h->flags          = 0;
    copy_field(h->name,        def->name,        PKG_NAME_MAX);
    copy_field(h->version,     def->version,     PKG_VER_MAX);
    copy_field(h->description, def->description, PKG_DESC_MAX);
    copy_field(h->author,      def->author,      PKG_AUTHOR_MAX);
    h->dep_count    = (uint16_t)ndeps;
    h->file_count   = (uint16_t)nfiles;
    h->payload_size = payload;

    /* Dependency table */
    npk_dep_t* deps = (npk_dep_t*)(buf + sizeof(npk_header_t));
    for (int i = 0; i < ndeps; i++) {
        copy_field(deps[i].name,        def->deps[i].name,        PKG_NAME_MAX);
        copy_field(deps[i].min_version, def->deps[i].min_version, PKG_VER_MAX);
    }

    /* File table + payload */
    npk_file_t* files = (npk_file_t*)(deps + ndeps);
    uint8_t*    pl    = (uint8_t*)(files + nfiles);
    uint32_t    off   = 0;
    for (int i = 0; i < nfiles; i++) {
        uint32_t flen = (uint32_t)strlen(def->files[i].content);
        copy_field(files[i].name, def->files[i].name, PKG_FILE_NAME_MAX);
        files[i].size   = flen;
        files[i].offset = off;
        memcpy(pl + off, def->files[i].content, flen);
        files[i].crc    = pkg_crc32(pl + off, flen);
        off += flen;
    }

    /* CRC over everything after the header */
    h->header_crc = pkg_crc32(buf + sizeof(npk_header_t), total - sizeof(npk_header_t));

    *out_buf  = buf;
    *out_size = total;
    return PKG_OK;
}

int npk_validate(const uint8_t* buf, uint32_t size) {
    if (!buf || size < sizeof(npk_header_t)) return PKG_ERR_CORRUPT;

    const npk_header_t* h = (const npk_header_t*)buf;
    if (h->magic != NPK_MAGIC)              return PKG_ERR_CORRUPT;
    if (h->format_version != NPK_FORMAT_VER) return PKG_ERR_CORRUPT;
    if (h->dep_count > PKG_MAX_DEPS)        return PKG_ERR_CORRUPT;
    if (h->file_count > PKG_MAX_FILES)      return PKG_ERR_CORRUPT;

    uint32_t expected = sizeof(npk_header_t)
                      + (uint32_t)h->dep_count  * sizeof(npk_dep_t)
                      + (uint32_t)h->file_count * sizeof(npk_file_t)
                      + h->payload_size;
    if (expected != size) return PKG_ERR_CORRUPT;

    /* Header CRC covers every byte after the header. */
    if (pkg_crc32(buf + sizeof(npk_header_t), size - sizeof(npk_header_t)) != h->header_crc)
        return PKG_ERR_CORRUPT;

    /* Per-file bounds and CRC. */
    const npk_file_t* files = (const npk_file_t*)(buf + sizeof(npk_header_t)
                                                 + (uint32_t)h->dep_count * sizeof(npk_dep_t));
    const uint8_t* payload = (const uint8_t*)(files + h->file_count);
    for (int i = 0; i < h->file_count; i++) {
        uint32_t end = files[i].offset + files[i].size;
        if (end < files[i].offset || end > h->payload_size) return PKG_ERR_CORRUPT;
        if (pkg_crc32(payload + files[i].offset, files[i].size) != files[i].crc)
            return PKG_ERR_CORRUPT;
    }
    return PKG_OK;
}

/* ============================================================================
 * Repository / installed-database queries
 * ============================================================================ */

int pkg_repo_count(void) { return PKG_REPO_COUNT; }

const pkg_def_t* pkg_repo_get(int index) {
    if (index < 0 || index >= PKG_REPO_COUNT) return NULL;
    return &pkg_repo[index];
}

const pkg_def_t* pkg_repo_find(const char* name) {
    if (!name) return NULL;
    for (int i = 0; i < PKG_REPO_COUNT; i++)
        if (strcmp(pkg_repo[i].name, name) == 0) return &pkg_repo[i];
    return NULL;
}

uint32_t pkg_repo_revision(void) { return repo_rev; }

int pkg_installed_count(void) {
    int n = 0;
    for (int i = 0; i < PKG_MAX_INSTALLED; i++)
        if (installed_db[i].active) n++;
    return n;
}

const pkg_installed_t* pkg_installed_get(int index) {
    int n = 0;
    for (int i = 0; i < PKG_MAX_INSTALLED; i++) {
        if (!installed_db[i].active) continue;
        if (n == index) return &installed_db[i];
        n++;
    }
    return NULL;
}

const pkg_installed_t* pkg_installed_find(const char* name) {
    if (!name) return NULL;
    for (int i = 0; i < PKG_MAX_INSTALLED; i++)
        if (installed_db[i].active && strcmp(installed_db[i].name, name) == 0)
            return &installed_db[i];
    return NULL;
}

bool pkg_is_installed(const char* name) {
    return pkg_installed_find(name) != NULL;
}

static pkg_installed_t* installed_alloc(void) {
    for (int i = 0; i < PKG_MAX_INSTALLED; i++)
        if (!installed_db[i].active) return &installed_db[i];
    return NULL;
}

/* ============================================================================
 * Cycle-detection stack
 * ============================================================================ */

static bool is_resolving(const char* name) {
    for (int i = 0; i < resolving_depth; i++)
        if (strcmp(resolving[i], name) == 0) return true;
    return false;
}

static bool resolving_push(const char* name) {
    if (resolving_depth >= PKG_MAX_RESOLVE) return false;
    copy_field(resolving[resolving_depth], name, PKG_NAME_MAX);
    resolving_depth++;
    return true;
}

static void resolving_pop(void) {
    if (resolving_depth > 0) resolving_depth--;
}

/* ============================================================================
 * Install engine
 * ============================================================================ */

/* Extract a validated .npk into the VFS and record it in the installed DB.
 * On any filesystem failure, files already written for THIS package are
 * rolled back so the operation is all-or-nothing for the package itself. */
static int extract_and_register(const uint8_t* buf) {
    const npk_header_t* h = (const npk_header_t*)buf;
    const npk_file_t* files = (const npk_file_t*)(buf + sizeof(npk_header_t)
                                                 + (uint32_t)h->dep_count * sizeof(npk_dep_t));
    const uint8_t* payload = (const uint8_t*)(files + h->file_count);

    pkg_installed_t* inst = installed_alloc();
    if (!inst) return PKG_ERR_DB_FULL;

    memset(inst, 0, sizeof(*inst));
    inst->active = true;
    copy_field(inst->name,    h->name,    PKG_NAME_MAX);
    copy_field(inst->version, h->version, PKG_VER_MAX);

    prc("    Extracting ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    pr_num(h->file_count);
    pr(" file(s):\n");

    fs_node_t* root = vfs_get_root();
    for (int i = 0; i < h->file_count; i++) {
        const char* fname = files[i].name;

        /* Reuse an existing node, otherwise create one. */
        fs_node_t* node = vfs_finddir(root, fname);
        if (!node) node = ramfs_create(fname, FS_FILE);
        if (!node) {
            /* Roll back the files we created for this package. */
            for (int j = 0; j < inst->file_count; j++)
                ramfs_delete(inst->files[j]);
            inst->active = false;
            return PKG_ERR_FS;
        }

        int32_t wrote = vfs_write(node, 0, files[i].size, payload + files[i].offset);
        if (wrote != (int32_t)files[i].size) {
            ramfs_delete(fname);
            for (int j = 0; j < inst->file_count; j++)
                ramfs_delete(inst->files[j]);
            inst->active = false;
            return PKG_ERR_FS;
        }

        copy_field(inst->files[inst->file_count], fname, PKG_FILE_NAME_MAX);
        inst->file_count++;
        inst->install_size += files[i].size;

        prc("       + ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        pr(fname);
        pr(" (");
        pr_num(files[i].size);
        pr(" bytes)\n");
    }
    return PKG_OK;
}

static int install_internal(const char* name) {
    /* Already present → dependency is satisfied. */
    if (pkg_is_installed(name)) return PKG_OK;

    const pkg_def_t* def = pkg_repo_find(name);
    if (!def) {
        prc("  [error] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        pr("package not in repository: ");
        pr(name); pr("\n");
        return PKG_ERR_NOT_FOUND;
    }

    if (is_resolving(name)) {
        prc("  [error] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        pr("circular dependency on ");
        pr(name); pr("\n");
        return PKG_ERR_CYCLE;
    }
    if (!resolving_push(name)) return PKG_ERR_DEP_FAILED;

    /* Header line for this package. */
    prc("==> ", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    pr(def->name); pr(" "); pr(def->version); pr("\n");

    /* Resolve dependencies first (depth-first). */
    int ndeps = def_dep_count(def);
    for (int i = 0; i < ndeps; i++) {
        const char* dname = def->deps[i].name;
        const char* dmin  = def->deps[i].min_version;

        const pkg_installed_t* have = pkg_installed_find(dname);
        if (have) {
            if (pkg_version_cmp(have->version, dmin) < 0) {
                prc("    [dep] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
                pr(dname); pr(" "); pr(have->version);
                pr(" is older than required "); pr(dmin); pr("\n");
                resolving_pop();
                return PKG_ERR_CONFLICT;
            }
            prc("    [dep] ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
            pr(dname); pr(" already satisfied ("); pr(have->version); pr(")\n");
            continue;
        }

        prc("    [dep] ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        pr("pulling in "); pr(dname); pr(" >= "); pr(dmin); pr("\n");

        int dr = install_internal(dname);
        if (dr != PKG_OK) {
            resolving_pop();
            return PKG_ERR_DEP_FAILED;
        }
        /* Confirm the freshly installed dependency meets the requirement. */
        have = pkg_installed_find(dname);
        if (!have || pkg_version_cmp(have->version, dmin) < 0) {
            resolving_pop();
            return PKG_ERR_CONFLICT;
        }
    }

    /* Build the .npk archive (what a mirror would have shipped). */
    uint8_t* archive = NULL;
    uint32_t archive_size = 0;
    int r = npk_serialize(def, &archive, &archive_size);
    if (r != PKG_OK) { resolving_pop(); return r; }

    prc("    Built ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    pr(def->name); pr(".npk (");
    pr_num(archive_size);
    pr(" bytes)\n");

    /* Verify integrity before touching the filesystem. */
    r = npk_validate(archive, archive_size);
    if (r != PKG_OK) {
        prc("    [error] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        pr("integrity check failed (corrupt archive)\n");
        kfree(archive);
        resolving_pop();
        return PKG_ERR_CORRUPT;
    }
    prc("    Integrity OK ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    pr("(CRC32 verified)\n");

    /* Extract and register. */
    r = extract_and_register(archive);
    kfree(archive);
    if (r != PKG_OK) {
        prc("    [error] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        pr(pkg_strerror(r)); pr("\n");
        resolving_pop();
        return r;
    }

    prc("==> ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    pr(def->name); pr(" installed.\n");

    resolving_pop();
    return PKG_OK;
}

int pkg_install(const char* name) {
    if (!name || !*name) return PKG_ERR_NOT_FOUND;

    if (pkg_is_installed(name)) {
        prc("  Package '", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        pr(name);
        pr("' is already installed.\n");
        return PKG_ERR_ALREADY;
    }
    if (!pkg_repo_find(name)) {
        prc("  Package '", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        pr(name);
        pr("' not found. Try 'npkg list'.\n");
        return PKG_ERR_NOT_FOUND;
    }

    resolving_depth = 0;
    int r = install_internal(name);

    if (r == PKG_OK) {
        const pkg_installed_t* inst = pkg_installed_find(name);
        prc("\n  Done. ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        if (inst) {
            pr(name); pr(" "); pr(inst->version);
            pr(" and its dependencies are installed.\n\n");
        } else {
            pr("Installation complete.\n\n");
        }
    } else {
        prc("\n  Installation failed: ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        pr(pkg_strerror(r)); pr("\n\n");
    }
    return r;
}

/* ============================================================================
 * Remove engine
 * ============================================================================ */

/* Return true if `target` is listed as a dependency of any other installed
 * package — used to warn before removing something still in use. */
static bool is_required_by_other(const char* target, char* dependent_out) {
    for (int i = 0; i < PKG_MAX_INSTALLED; i++) {
        if (!installed_db[i].active) continue;
        if (strcmp(installed_db[i].name, target) == 0) continue;

        const pkg_def_t* def = pkg_repo_find(installed_db[i].name);
        if (!def) continue;
        int nd = def_dep_count(def);
        for (int d = 0; d < nd; d++) {
            if (strcmp(def->deps[d].name, target) == 0) {
                if (dependent_out) copy_field(dependent_out, installed_db[i].name, PKG_NAME_MAX);
                return true;
            }
        }
    }
    return false;
}

int pkg_remove(const char* name) {
    if (!name || !*name) return PKG_ERR_NOT_INSTALLED;

    pkg_installed_t* inst = NULL;
    for (int i = 0; i < PKG_MAX_INSTALLED; i++) {
        if (installed_db[i].active && strcmp(installed_db[i].name, name) == 0) {
            inst = &installed_db[i];
            break;
        }
    }
    if (!inst) {
        prc("  Package '", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        pr(name);
        pr("' is not installed.\n");
        return PKG_ERR_NOT_INSTALLED;
    }

    /* Warn (but proceed) if another installed package depends on this one. */
    char dependent[PKG_NAME_MAX];
    if (is_required_by_other(name, dependent)) {
        prc("  [warn] ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        pr(dependent);
        pr(" depends on "); pr(name); pr("; removing anyway.\n");
    }

    prc("==> Removing ", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    pr(inst->name); pr(" "); pr(inst->version); pr("\n");

    for (int i = 0; i < inst->file_count; i++) {
        if (ramfs_delete(inst->files[i]) == 0) {
            prc("       - ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
            pr(inst->files[i]); pr("\n");
        }
    }

    inst->active = false;
    prc("==> ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    pr(name); pr(" removed.\n\n");
    return PKG_OK;
}

/* ============================================================================
 * Repository update (best-effort remote probe)
 * ============================================================================ */

void pkg_update(void) {
    prc("  Refreshing package index...\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    pr("  Mirror: "); pr(PKG_REPO_URL); pr("\n");

    http_response_t resp = http_get(PKG_REPO_URL);
    if (resp.success) {
        prc("  [ok] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        pr("remote mirror reachable (HTTP ");
        pr_num((uint32_t)resp.status_code);
        pr(").\n");
    } else {
        prc("  [info] ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        pr("remote mirror unreachable; using bundled local mirror.\n");
    }

    repo_rev++;
    prc("  Index updated. ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    pr_num((uint32_t)PKG_REPO_COUNT);
    pr(" packages available (rev ");
    pr_num(repo_rev);
    pr(").\n\n");
}

/* ============================================================================
 * Misc
 * ============================================================================ */

const char* pkg_strerror(int code) {
    switch (code) {
        case PKG_OK:                return "success";
        case PKG_ERR_NOT_FOUND:     return "package not found";
        case PKG_ERR_ALREADY:       return "already installed";
        case PKG_ERR_NOT_INSTALLED: return "package not installed";
        case PKG_ERR_DEP_FAILED:    return "dependency installation failed";
        case PKG_ERR_NOMEM:         return "out of memory";
        case PKG_ERR_CORRUPT:       return "corrupt package archive";
        case PKG_ERR_FS:            return "filesystem write failed";
        case PKG_ERR_DB_FULL:       return "installed database full";
        case PKG_ERR_CYCLE:         return "circular dependency";
        case PKG_ERR_CONFLICT:      return "dependency version conflict";
        default:                    return "unknown error";
    }
}

void pkg_init(void) {
    memset(installed_db, 0, sizeof(installed_db));
    resolving_depth = 0;
    repo_rev = 1;

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Package manager ready (");
    pr_num((uint32_t)PKG_REPO_COUNT);
    vga_print(" packages in repository)\n");
}
