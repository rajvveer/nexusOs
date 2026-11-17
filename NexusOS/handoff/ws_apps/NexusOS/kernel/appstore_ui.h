/* ============================================================================
 * NexusOS — App Store window (Header) — Phase 57 "apps"
 * ============================================================================
 * A desktop window listing the userland programs installed on /disk/bin
 * (Lua, kilo, 2048, tetris, coreutils...) with real metadata — version, size
 * read from the FAT32 volume, licence, source URL — and an "Open" button that
 * launches the program as the Terminal's foreground job.
 * ============================================================================ */
#ifndef APPSTORE_UI_H
#define APPSTORE_UI_H

#include "types.h"

void appstore_ui_open(void);

/* Provided by desktop.c: run `cmd` in the GUI Terminal (opens one if needed). */
void desktop_terminal_run(const char* cmd);

/* Catalog shared with pkg.c / appstore.c */
typedef struct {
    const char* name;       /* display name            */
    const char* bin;        /* /disk/bin/<bin>         */
    const char* pkg;        /* package name            */
    const char* version;
    const char* licence;
    const char* url;        /* upstream source         */
    const char* desc;
} store_app_t;
const store_app_t* appstore_ui_catalog(int* count);
/* Size of the installed binary on /disk/bin, or 0 if not installed. */
uint32_t appstore_ui_installed_size(const store_app_t* a);

#endif /* APPSTORE_UI_H */
