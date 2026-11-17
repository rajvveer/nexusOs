/* ============================================================================
 * NexusOS — App Store & Ecosystem (Implementation) — Phase 49 (Era 7 opener)
 * ============================================================================
 * The storefront, update engine, developer SDK, and cooperative sandbox — all
 * layered over the Phase-35 package manager (pkg.c). Every install/remove is
 * delegated to the existing, already-reviewed pkg engine so dependency
 * resolution, CRC integrity, and the installed database stay single-sourced.
 *
 * Honest scoping (see SESSION_CONTEXT §3):
 *   - "Sandboxing" is COOPERATIVE (uid/perm trust tiers), not memory
 *     isolation. The kernel is one shared address space, ring 0. The profile
 *     records the trust tier so UX and callers can reason about it; real
 *     isolation waits on the deferred Phase-44 address-space rewrite.
 *   - "Automatic updates" is a version-check over the bundled/refreshable repo
 *     (pkg_repo_revision / pkg_version_cmp), upgrading through pkg_install.
 *   - The "App Store" reaches the SAME bundled mirror the package manager does;
 *     there is no live internet store offline.
 * ============================================================================ */

#include "appstore.h"
#include "pkg.h"
#include "ramfs.h"
#include "vfs.h"
#include "string.h"
#include "vga.h"
#include "users.h"

/* ============================================================================
 * Output helpers (match pkg.c's house style)
 * ============================================================================ */
static void pr(const char* s)                { vga_print(s); }
static void prc(const char* s, uint8_t col)  { vga_print_color(s, col); }
static void pr_num(uint32_t n) { char b[12]; int_to_str((int)n, b); vga_print(b); }

static void pr_stars(uint8_t stars) {
    if (stars > 5) stars = 5;
    for (int i = 0; i < 5; i++)
        prc(i < stars ? "*" : "-",
            i < stars ? VGA_COLOR(VGA_YELLOW, VGA_BLACK)
                      : VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
}

/* ============================================================================
 * Curated storefront catalog
 * ----------------------------------------------------------------------------
 * Each row decorates a package that already exists in pkg_repo[] (pkg.c). The
 * version, description, and dependencies live there — we only add storefront
 * metadata (category / rating / featured / trust tier / tagline). A listing
 * whose `pkg` is not in the repo is silently skipped by the views, so the two
 * stay loosely coupled.
 * ============================================================================ */
static const app_listing_t catalog[] = {
    { "libnx",     APP_CAT_SYSTEM,       5, false, APP_TRUST_CORE,
      "Base runtime every NexusOS app links against" },
    { "coreutils", APP_CAT_SYSTEM,       4, true,  APP_TRUST_CORE,
      "Essential userland: ls, cat, grep, wc" },
    { "fetch",     APP_CAT_SYSTEM,       4, false, APP_TRUST_TRUSTED,
      "Pretty system-info banner" },
    { "hello",     APP_CAT_OTHER,        3, false, APP_TRUST_SANDBOXED,
      "The classic hello-world demo" },
    { "nano",      APP_CAT_PRODUCTIVITY, 4, true,  APP_TRUST_TRUSTED,
      "Small, friendly console editor" },
    { "nxedit",    APP_CAT_PRODUCTIVITY, 5, true,  APP_TRUST_TRUSTED,
      "Syntax-highlighting multi-buffer editor" },
    { "sdl-shim",  APP_CAT_GAMES,        4, false, APP_TRUST_TRUSTED,
      "SDL-like surface/event/audio shim" },
    { "doom",      APP_CAT_GAMES,        5, true,  APP_TRUST_SANDBOXED,
      "The classic DOOM port — the true OS benchmark" },
};
#define CATALOG_COUNT ((int)(sizeof(catalog) / sizeof(catalog[0])))

/* ============================================================================
 * Catalog queries
 * ============================================================================ */
int appstore_count(void) { return CATALOG_COUNT; }

const app_listing_t* appstore_get(int index) {
    if (index < 0 || index >= CATALOG_COUNT) return NULL;
    return &catalog[index];
}

const app_listing_t* appstore_find(const char* pkg) {
    if (!pkg) return NULL;
    for (int i = 0; i < CATALOG_COUNT; i++)
        if (strcmp(catalog[i].pkg, pkg) == 0) return &catalog[i];
    return NULL;
}

const char* appstore_category_name(app_category_t c) {
    switch (c) {
        case APP_CAT_SYSTEM:       return "System";
        case APP_CAT_PRODUCTIVITY: return "Productivity";
        case APP_CAT_GAMES:        return "Games";
        default:                   return "Other";
    }
}

const char* appstore_trust_name(app_trust_t t) {
    switch (t) {
        case APP_TRUST_CORE:    return "core (full access)";
        case APP_TRUST_TRUSTED: return "trusted (first-party)";
        default:                return "sandboxed (cooperative)";
    }
}

/* ============================================================================
 * Storefront views
 * ============================================================================ */

/* One catalog row: "  [x] name 1.0.0  ***--  tagline". `installed` marker. */
static void print_listing_row(const app_listing_t* L) {
    const pkg_def_t* def = pkg_repo_find(L->pkg);
    if (!def) return;                       /* listing without a repo package  */

    bool have = pkg_is_installed(L->pkg);
    prc(have ? "  [installed] " : "  [        ]  ",
        have ? VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK)
             : VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));

    prc(def->name, VGA_COLOR(VGA_WHITE, VGA_BLACK));
    pr(" ");
    prc(def->version, VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    pr("  ");
    pr_stars(L->stars);
    pr("  ");
    pr(L->tagline);
    pr("\n");
}

void appstore_show_featured(void) {
    prc("\n  NexusOS App Store — Featured\n",
        VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    prc("  ============================\n\n",
        VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    int shown = 0;
    for (int i = 0; i < CATALOG_COUNT; i++) {
        if (!catalog[i].featured) continue;
        print_listing_row(&catalog[i]);
        shown++;
    }
    if (!shown) pr("  (no featured apps)\n");
    pr("\n  Browse: 'store list' | 'store category games' | 'store info <app>'\n\n");
}

void appstore_show_category(app_category_t c) {
    prc("\n  App Store — ", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    prc(appstore_category_name(c), VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    pr("\n\n");
    int shown = 0;
    for (int i = 0; i < CATALOG_COUNT; i++) {
        if (catalog[i].category != c) continue;
        print_listing_row(&catalog[i]);
        shown++;
    }
    if (!shown) pr("  (no apps in this category)\n");
    pr("\n");
}

void appstore_show_all(void) {
    prc("\n  NexusOS App Store — All Apps (", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    pr_num((uint32_t)CATALOG_COUNT);
    prc(")\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    prc("  ==============================\n\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));

    /* Group by category for a tidy storefront layout. */
    for (int cat = 0; cat < APP_CAT_COUNT; cat++) {
        int shown = 0;
        for (int i = 0; i < CATALOG_COUNT; i++) {
            if (catalog[i].category != (app_category_t)cat) continue;
            if (!shown) {
                prc("  --- ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
                prc(appstore_category_name((app_category_t)cat),
                    VGA_COLOR(VGA_LIGHT_MAGENTA, VGA_BLACK));
                prc(" ---\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
            }
            print_listing_row(&catalog[i]);
            shown++;
        }
        if (shown) pr("\n");
    }
}

void appstore_show_detail(const char* pkg) {
    const app_listing_t* L = appstore_find(pkg);
    const pkg_def_t*     def = pkg_repo_find(pkg);
    if (!def) {
        prc("  App '", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        pr(pkg ? pkg : "");
        pr("' is not in the store. Try 'store list'.\n");
        return;
    }

    prc("\n  ", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    prc(def->name, VGA_COLOR(VGA_WHITE, VGA_BLACK));
    pr("  ");
    prc(def->version, VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    pr("\n  ");
    if (L) { pr_stars(L->stars); pr("   "); }
    prc(appstore_category_name(L ? L->category : APP_CAT_OTHER),
        VGA_COLOR(VGA_LIGHT_MAGENTA, VGA_BLACK));
    pr("\n\n");

    pr("  "); pr(def->description); pr("\n");
    prc("  Author:  ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK)); pr(def->author); pr("\n");
    prc("  Sandbox: ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    pr(appstore_trust_name(L ? L->trust : APP_TRUST_SANDBOXED)); pr("\n");

    /* Dependencies (read straight from the repo definition). */
    int nd = 0;
    while (nd < PKG_MAX_DEPS && def->deps[nd].name) nd++;
    prc("  Requires:", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    if (nd == 0) pr(" (none)\n");
    else {
        for (int i = 0; i < nd; i++) {
            pr(" "); pr(def->deps[i].name);
            pr(">="); pr(def->deps[i].min_version);
        }
        pr("\n");
    }

    prc("  Status:  ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    const pkg_installed_t* inst = pkg_installed_find(pkg);
    if (inst) {
        prc("installed ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        pr("("); pr(inst->version); pr(")");
        if (pkg_version_cmp(inst->version, def->version) < 0) {
            prc("  — update available: ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
            pr(def->version);
        }
        pr("\n");
    } else {
        pr("not installed\n");
    }

    pr("\n  Install: 'store install "); pr(def->name); pr("'\n\n");
}

void appstore_search(const char* term) {
    if (!term || !*term) { appstore_show_all(); return; }
    prc("\n  App Store search: '", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    pr(term); pr("'\n\n");

    int hits = 0;
    for (int i = 0; i < CATALOG_COUNT; i++) {
        const pkg_def_t* def = pkg_repo_find(catalog[i].pkg);
        if (!def) continue;
        if (strstr(def->name, term) || strstr(def->description, term)
            || strstr(catalog[i].tagline, term)) {
            print_listing_row(&catalog[i]);
            hits++;
        }
    }
    if (!hits) pr("  No matching apps.\n");
    pr("\n");
}

/* ============================================================================
 * Install via the storefront (delegates to the pkg engine)
 * ============================================================================ */
int appstore_install(const char* pkg) {
    const app_listing_t* L = appstore_find(pkg);
    const pkg_def_t*     def = pkg_repo_find(pkg);
    if (!def) {
        prc("  App '", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        pr(pkg ? pkg : "");
        pr("' is not in the store. Try 'store list'.\n");
        return PKG_ERR_NOT_FOUND;
    }

    prc("==> App Store: installing ", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    pr(def->name); pr(" "); pr(def->version);
    prc("  [sandbox: ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    pr(appstore_trust_name(L ? L->trust : APP_TRUST_SANDBOXED));
    prc("]\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));

    int r = pkg_install(pkg);              /* full dep resolution + integrity */
    if (r == PKG_OK) {
        /* Apply the cooperative sandbox profile. With one shared address space
         * the only enforceable "sandbox" today is uid/perm scoping (Phase 44):
         * record the trust tier for the user; full memory isolation is the
         * deferred address-space rewrite. */
        prc("  Sandbox profile applied: ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        pr(appstore_trust_name(L ? L->trust : APP_TRUST_SANDBOXED));
        pr("\n");
        if (L && L->trust == APP_TRUST_SANDBOXED) {
            prc("  Note: ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
            pr("cooperative sandbox (uid/perm scoping); not memory isolation.\n");
        }
    }
    return r;
}

/* ============================================================================
 * Update engine
 * ============================================================================ */
int appstore_outdated_count(void) {
    int n = 0;
    int ic = pkg_installed_count();
    for (int i = 0; i < ic; i++) {
        const pkg_installed_t* inst = pkg_installed_get(i);
        if (!inst) continue;
        const pkg_def_t* def = pkg_repo_find(inst->name);
        if (def && pkg_version_cmp(inst->version, def->version) < 0) n++;
    }
    return n;
}

void appstore_check_updates(void) {
    prc("\n  Checking for app updates...\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    pkg_update();                          /* re-probe mirror, bump revision  */

    int ic = pkg_installed_count();
    if (ic == 0) {
        pr("  No apps installed.\n\n");
        return;
    }

    prc("  Installed apps (rev ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    pr_num(pkg_repo_revision());
    prc("):\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));

    int outdated = 0;
    for (int i = 0; i < ic; i++) {
        const pkg_installed_t* inst = pkg_installed_get(i);
        if (!inst) continue;
        const pkg_def_t* def = pkg_repo_find(inst->name);
        pr("    "); pr(inst->name); pr(" "); pr(inst->version);
        if (def && pkg_version_cmp(inst->version, def->version) < 0) {
            prc("  -> ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
            prc(def->version, VGA_COLOR(VGA_YELLOW, VGA_BLACK));
            prc("  (update available)", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
            outdated++;
        } else {
            prc("  (up to date)", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        }
        pr("\n");
    }

    if (outdated) {
        prc("\n  ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        pr_num((uint32_t)outdated);
        pr(" update(s) available. Run 'store upgrade' to install them.\n\n");
    } else {
        prc("\n  All apps are up to date.\n\n", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    }
}

int appstore_upgrade_all(void) {
    int ic = pkg_installed_count();
    if (ic == 0) { pr("  No apps installed.\n"); return PKG_OK; }

    /* Snapshot outdated names first — pkg_remove/install below would otherwise
     * shift the installed-DB indices we are iterating. */
    char outdated[PKG_MAX_INSTALLED][PKG_NAME_MAX];
    int  count = 0;
    for (int i = 0; i < ic && count < PKG_MAX_INSTALLED; i++) {
        const pkg_installed_t* inst = pkg_installed_get(i);
        if (!inst) continue;
        const pkg_def_t* def = pkg_repo_find(inst->name);
        if (def && pkg_version_cmp(inst->version, def->version) < 0) {
            strncpy(outdated[count], inst->name, PKG_NAME_MAX - 1);
            outdated[count][PKG_NAME_MAX - 1] = '\0';
            count++;
        }
    }

    if (count == 0) {
        prc("  All apps are up to date.\n\n", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        return PKG_OK;
    }

    prc("\n  Upgrading ", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    pr_num((uint32_t)count);
    pr(" app(s)...\n\n");

    int failed = 0;
    for (int i = 0; i < count; i++) {
        prc("==> Upgrading ", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
        pr(outdated[i]); pr("\n");
        /* Remove the old version, then reinstall at the repo version. The pkg
         * engine re-resolves dependencies, so an upgrade that newly requires a
         * dep pulls it in. */
        pkg_remove(outdated[i]);
        int r = pkg_install(outdated[i]);
        if (r != PKG_OK) {
            prc("  [error] upgrade failed: ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
            pr(pkg_strerror(r)); pr("\n");
            failed++;
        }
    }

    if (failed) {
        prc("  ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        pr_num((uint32_t)failed); pr(" upgrade(s) failed.\n\n");
        return PKG_ERR_DEP_FAILED;
    }
    prc("  All apps upgraded.\n\n", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    return PKG_OK;
}

/* ============================================================================
 * Developer SDK
 * ============================================================================ */

/* Write one text file into the RAM-FS, reusing an existing node if present. */
static bool sdk_write_file(const char* name, const char* content) {
    fs_node_t* root = vfs_get_root();
    fs_node_t* node = vfs_finddir(root, name);
    if (!node) node = ramfs_create(name, FS_FILE);
    if (!node) return false;
    uint32_t len = (uint32_t)strlen(content);
    return vfs_write(node, 0, len, (const uint8_t*)content) == (int32_t)len;
}

static const char* SDK_README =
    "NexusOS Developer SDK (Phase 49)\n"
    "================================\n"
    "Build an app, package it as .npk, and ship it through the App Store.\n"
    "\n"
    "Files in this SDK:\n"
    "  sdk-readme.txt    this file\n"
    "  sdk-api.txt       the kernel API surface an app may call\n"
    "  sdk-manifest.txt  the package manifest (name/version/deps/files)\n"
    "  sdk-sample.c      a buildable hello-world app template\n"
    "\n"
    "Quick start:\n"
    "  1. Edit sdk-sample.c and sdk-manifest.txt.\n"
    "  2. 'store sdk' prints the API quick-reference.\n"
    "  3. Package with the .npk codec (see pkg.h npk_serialize).\n"
    "  4. Publish: add an app_listing_t row in appstore.c's catalog[].\n"
    "\n"
    "Sandbox model: apps run in the shared address space (ring 0, cooperative).\n"
    "Honor the uid/permission scoping from your trust tier; do NOT assume memory\n"
    "isolation (it is the deferred address-space rewrite).\n";

static const char* SDK_API =
    "NexusOS App API Quick Reference\n"
    "===============================\n"
    "Console:   vga_print(s)  vga_print_color(s, color)  vga_putchar(c)\n"
    "Files:     vfs_get_root()  ramfs_create(name,FS_FILE)  vfs_read/vfs_write\n"
    "Strings:   strlen strcpy strcmp strncpy strcat strstr int_to_str hex_to_str\n"
    "Memory:    kmalloc(n)  kfree(p)   (no libc; freestanding, integer-only)\n"
    "Users:     users_current_uid()  users_current_name()  users_current_is_root()\n"
    "Graphics:  fb_is_vesa()  gfx_*  (VESA 1024x768x32 framebuffer)\n"
    "Audio:     audio_play_pcm(...)  tone(...)   (AC'97 via the audio core)\n"
    "\n"
    "Constraints: no 64-bit multiply/divide (no libgcc); RAM-FS files <= 4 KB;\n"
    "keep kmalloc modest (heap/stack/fb overlap). Integer math only.\n";

static const char* SDK_MANIFEST =
    "# NexusOS package manifest — edit, then build with the .npk codec.\n"
    "name = myapp\n"
    "version = 1.0.0\n"
    "description = My first NexusOS app\n"
    "author = Me\n"
    "deps = libnx>=1.0.0\n"
    "files = myapp.txt\n";

static const char* SDK_SAMPLE =
    "/* NexusOS sample app — Phase 49 SDK template.\n"
    " * A console app entry point; link against libnx. */\n"
    "#include \"vga.h\"\n"
    "#include \"users.h\"\n"
    "\n"
    "void app_main(void) {\n"
    "    vga_print(\"Hello from a NexusOS app!\\n\");\n"
    "    vga_print(\"Running as: \");\n"
    "    vga_print(users_current_name());\n"
    "    vga_print(\"\\n\");\n"
    "}\n";

void appstore_sdk_install(void) {
    prc("\n  Installing NexusOS Developer SDK...\n",
        VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));

    struct { const char* name; const char* body; } files[] = {
        { "sdk-readme.txt",   SDK_README   },
        { "sdk-api.txt",      SDK_API      },
        { "sdk-manifest.txt", SDK_MANIFEST },
        { "sdk-sample.c",     SDK_SAMPLE   },
    };
    int n = (int)(sizeof(files) / sizeof(files[0]));
    int ok = 0;
    for (int i = 0; i < n; i++) {
        if (sdk_write_file(files[i].name, files[i].body)) {
            prc("    + ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
            pr(files[i].name); pr("\n");
            ok++;
        } else {
            prc("    [error] could not write ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
            pr(files[i].name); pr("\n");
        }
    }

    prc("\n  SDK ready: ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    pr_num((uint32_t)ok); pr("/"); pr_num((uint32_t)n);
    pr(" files in the filesystem.\n");
    pr("  Read them with 'cat sdk-readme.txt'. See 'store sdk' for the API.\n\n");
}

void appstore_sdk_docs(void) {
    prc("\n  NexusOS Developer SDK — Quick Reference\n",
        VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    prc("  =======================================\n",
        VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    pr(SDK_API);
    pr("\n  Scaffold the full SDK into files with 'store sdk install'.\n\n");
}

/* ============================================================================
 * Sandbox
 * ============================================================================ */
void appstore_sandbox_info(const char* pkg) {
    const app_listing_t* L = appstore_find(pkg);
    if (!L) {
        prc("  App '", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        pr(pkg ? pkg : "");
        pr("' is not in the store.\n");
        return;
    }
    prc("\n  Sandbox profile — ", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    pr(L->pkg); pr("\n\n");
    prc("    Trust tier: ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    pr(appstore_trust_name(L->trust)); pr("\n");
    prc("    Model:      ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    pr("cooperative (uid/permission scoping, Phase 44)\n");
    prc("    Address sp: ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    pr("shared, ring 0 — NOT memory-isolated\n");
    prc("    Enforced:   ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    pr("file permissions at the VFS read/write choke points\n");
    prc("    Deferred:   ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    pr("per-process page dirs + ring-3 (true isolation)\n\n");
}

/* ============================================================================
 * Init
 * ============================================================================ */
void appstore_init(void) {
    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("App Store ready (");
    pr_num((uint32_t)CATALOG_COUNT);
    vga_print(" apps in catalog)\n");
}
