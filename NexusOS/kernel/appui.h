/* ============================================================================
 * NexusOS - App UI Helpers
 * ============================================================================
 * Shared character-cell UI helpers for modern desktop applications.
 * ============================================================================ */

#ifndef APPUI_H
#define APPUI_H

#include "types.h"

typedef struct {
    uint8_t bg;
    uint8_t panel;
    uint8_t toolbar;
    uint8_t text;
    uint8_t muted;
    uint8_t accent;
    uint8_t good;
    uint8_t warn;
    uint8_t danger;
    uint8_t selected;
    uint8_t status;
} appui_theme_t;

appui_theme_t appui_theme(void);
void appui_fill(int x, int y, int w, int h, uint8_t color);
void appui_text(int x, int y, const char* text, int max, uint8_t color);
void appui_header(int x, int y, int w, const char* title, const char* subtitle);
void appui_status(int x, int y, int w, const char* text);
void appui_hline(int x, int y, int w, uint8_t color);
void appui_row(int x, int y, int w, bool selected, uint8_t normal, uint8_t selected_color);
void appui_kv(int x, int y, const char* key, const char* value, int value_x, int max);
void appui_input(int x, int y, int w, const char* text, int len, bool cursor);

#endif /* APPUI_H */
