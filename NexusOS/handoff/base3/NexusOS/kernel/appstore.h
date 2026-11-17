/* ============================================================================
 * NexusOS — App Store & Ecosystem (Header) — Phase 49 (Era 7 opener)
 * ============================================================================
 * A curated storefront layered OVER the Phase-35 package manager (pkg.c). The
 * App Store does not re-implement install/remove/dependency-resolution — it
 * delegates every mutation to `pkg_install`/`pkg_remove` and adds the
 * ecosystem layer on top:
 *
 *   1. Catalog   — categories, ratings, featured picks, search/browse over the
 *                  bundled package repository.
 *   2. Updates   — a version-check engine that compares each installed package
 *                  against the (refreshable) repository revision and offers an
 *                  "upgrade all" path through the existing pkg engine.
 *   3. SDK       — a developer SDK: writes API docs + a buildable sample-app
 *                  template into the VFS so a developer can scaffold an .npk.
 *   4. Sandbox   — an HONEST cooperative sandbox profile per app (uid/perm
 *                  scoping, like Phase 44). NOT memory isolation — the kernel
 *                  is one shared address space (see SESSION_CONTEXT §3). The
 *                  profile records the trust tier so callers/UX can reason
 *                  about it; real isolation waits on the address-space rewrite.
 * ============================================================================ */

#ifndef APPSTORE_H
#define APPSTORE_H

#include "types.h"

/* ============================================================================
 * Categories
 * ============================================================================ */
typedef enum {
    APP_CAT_SYSTEM = 0,     /* runtimes, core libs, utilities                  */
    APP_CAT_PRODUCTIVITY,   /* editors, tools                                  */
    APP_CAT_GAMES,          /* games + game shims                              */
    APP_CAT_OTHER,          /* anything uncategorized                          */
    APP_CAT_COUNT
} app_category_t;

/* ============================================================================
 * Sandbox trust tiers (cooperative — see header note)
 * ============================================================================ */
typedef enum {
    APP_TRUST_CORE = 0,     /* shipped by NexusOS Core; full access            */
    APP_TRUST_TRUSTED,      /* signed first-party app                          */
    APP_TRUST_SANDBOXED     /* third-party; cooperative uid/perm scoping       */
} app_trust_t;

/* A storefront listing — metadata that decorates a repo package. Resolved by
 * name against the pkg_repo[] entry, so versions/deps stay single-sourced. */
typedef struct {
    const char*    pkg;         /* package name in the repository              */
    app_category_t category;
    uint8_t        stars;       /* curated rating, 1..5                        */
    bool           featured;    /* shown in the "Featured" rail                */
    app_trust_t    trust;       /* sandbox profile applied on install          */
    const char*    tagline;     /* one-line storefront blurb                   */
} app_listing_t;

/* ============================================================================
 * Public API
 * ============================================================================ */
void appstore_init(void);

/* Catalog queries. */
int                  appstore_count(void);
const app_listing_t* appstore_get(int index);
const app_listing_t* appstore_find(const char* pkg);
const char*          appstore_category_name(app_category_t c);
const char*          appstore_trust_name(app_trust_t t);

/* Storefront views (print to the console). */
void appstore_show_featured(void);
void appstore_show_category(app_category_t c);
void appstore_show_all(void);
void appstore_show_detail(const char* pkg);
void appstore_search(const char* term);

/* Install via the storefront — delegates to pkg_install, then applies the
 * sandbox profile. Returns the PKG_* code from the underlying install. */
int  appstore_install(const char* pkg);

/* --- Updates --------------------------------------------------------------- */
/* Re-probe the repo index, then report which installed packages are outdated. */
void appstore_check_updates(void);
/* Upgrade every outdated installed package through the pkg engine. */
int  appstore_upgrade_all(void);
/* Count outdated installed packages (no output). */
int  appstore_outdated_count(void);

/* --- Developer SDK --------------------------------------------------------- */
/* Scaffold the SDK into the VFS: API reference + a sample-app template +
 * a manifest the developer can edit, then "build" into an .npk. */
void appstore_sdk_install(void);
/* Print the SDK quick-reference to the console. */
void appstore_sdk_docs(void);

/* --- Sandbox --------------------------------------------------------------- */
/* Describe the cooperative sandbox profile that applies to a package. */
void appstore_sandbox_info(const char* pkg);

#endif /* APPSTORE_H */
