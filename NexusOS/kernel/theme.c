/* ============================================================================
 * NexusOS — Theme Engine (Implementation) — Polished
 * ============================================================================
 * 6 built-in color themes with refined color palettes.
 * ============================================================================ */

#include "theme.h"
#include "vga.h"
#include "string.h"

static int current_theme = THEME_NEXUS_DARK;

static const theme_t themes[THEME_COUNT] = {
    /* === Nexus Dark — flagship dark theme === */
    {
        .desktop_bg         = VGA_COLOR(VGA_DARK_GREY, VGA_BLACK),
        .desktop_fg         = VGA_COLOR(VGA_DARK_GREY, VGA_BLACK),
        .desktop_char       = (char)0xB0,

        .win_border         = VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK),
        .win_title_active   = VGA_COLOR(VGA_BLACK, VGA_LIGHT_CYAN),
        .win_title_inactive = VGA_COLOR(VGA_LIGHT_GREY, VGA_DARK_GREY),
        .win_content        = VGA_COLOR(VGA_WHITE, VGA_BLACK),
        .win_text           = VGA_COLOR(VGA_WHITE, VGA_BLACK),

        .taskbar_bg         = VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLUE),
        .taskbar_text       = VGA_COLOR(VGA_WHITE, VGA_BLUE),
        .taskbar_active     = VGA_COLOR(VGA_BLACK, VGA_LIGHT_CYAN),
        .taskbar_clock      = VGA_COLOR(VGA_YELLOW, VGA_BLUE),

        .menu_bg            = VGA_COLOR(VGA_WHITE, VGA_DARK_GREY),
        .menu_highlight     = VGA_COLOR(VGA_BLACK, VGA_LIGHT_CYAN),

        .cursor_color       = VGA_COLOR(VGA_BLACK, VGA_WHITE),

        .name = "dark"
    },

    /* === Nexus Light — clean bright theme === */
    {
        .desktop_bg         = VGA_COLOR(VGA_BLUE, VGA_WHITE),
        .desktop_fg         = VGA_COLOR(VGA_BLUE, VGA_WHITE),
        .desktop_char       = (char)0xB0,

        .win_border         = VGA_COLOR(VGA_BLUE, VGA_WHITE),
        .win_title_active   = VGA_COLOR(VGA_WHITE, VGA_BLUE),
        .win_title_inactive = VGA_COLOR(VGA_DARK_GREY, VGA_LIGHT_GREY),
        .win_content        = VGA_COLOR(VGA_BLACK, VGA_WHITE),
        .win_text           = VGA_COLOR(VGA_BLACK, VGA_WHITE),

        .taskbar_bg         = VGA_COLOR(VGA_WHITE, VGA_DARK_GREY),
        .taskbar_text       = VGA_COLOR(VGA_WHITE, VGA_DARK_GREY),
        .taskbar_active     = VGA_COLOR(VGA_BLACK, VGA_WHITE),
        .taskbar_clock      = VGA_COLOR(VGA_YELLOW, VGA_DARK_GREY),

        .menu_bg            = VGA_COLOR(VGA_BLACK, VGA_WHITE),
        .menu_highlight     = VGA_COLOR(VGA_WHITE, VGA_BLUE),

        .cursor_color       = VGA_COLOR(VGA_WHITE, VGA_BLACK),

        .name = "light"
    },

    /* === Retro — phosphor green terminal === */
    {
        .desktop_bg         = VGA_COLOR(VGA_GREEN, VGA_BLACK),
        .desktop_fg         = VGA_COLOR(VGA_GREEN, VGA_BLACK),
        .desktop_char       = (char)0xFA,

        .win_border         = VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK),
        .win_title_active   = VGA_COLOR(VGA_BLACK, VGA_LIGHT_GREEN),
        .win_title_inactive = VGA_COLOR(VGA_GREEN, VGA_BLACK),
        .win_content        = VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK),
        .win_text           = VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK),

        .taskbar_bg         = VGA_COLOR(VGA_BLACK, VGA_GREEN),
        .taskbar_text       = VGA_COLOR(VGA_BLACK, VGA_GREEN),
        .taskbar_active     = VGA_COLOR(VGA_BLACK, VGA_LIGHT_GREEN),
        .taskbar_clock      = VGA_COLOR(VGA_BLACK, VGA_GREEN),

        .menu_bg            = VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK),
        .menu_highlight     = VGA_COLOR(VGA_BLACK, VGA_LIGHT_GREEN),

        .cursor_color       = VGA_COLOR(VGA_BLACK, VGA_GREEN),

        .name = "retro"
    },

    /* === Ocean — deep blue tones === */
    {
        .desktop_bg         = VGA_COLOR(VGA_LIGHT_BLUE, VGA_BLUE),
        .desktop_fg         = VGA_COLOR(VGA_LIGHT_BLUE, VGA_BLUE),
        .desktop_char       = (char)0xB1,

        .win_border         = VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLUE),
        .win_title_active   = VGA_COLOR(VGA_BLUE, VGA_LIGHT_CYAN),
        .win_title_inactive = VGA_COLOR(VGA_LIGHT_GREY, VGA_BLUE),
        .win_content        = VGA_COLOR(VGA_WHITE, VGA_BLUE),
        .win_text           = VGA_COLOR(VGA_WHITE, VGA_BLUE),

        .taskbar_bg         = VGA_COLOR(VGA_LIGHT_CYAN, VGA_DARK_GREY),
        .taskbar_text       = VGA_COLOR(VGA_WHITE, VGA_DARK_GREY),
        .taskbar_active     = VGA_COLOR(VGA_BLACK, VGA_LIGHT_CYAN),
        .taskbar_clock      = VGA_COLOR(VGA_YELLOW, VGA_DARK_GREY),

        .menu_bg            = VGA_COLOR(VGA_WHITE, VGA_BLUE),
        .menu_highlight     = VGA_COLOR(VGA_BLUE, VGA_LIGHT_CYAN),

        .cursor_color       = VGA_COLOR(VGA_BLUE, VGA_WHITE),

        .name = "ocean"
    }
};

void theme_set(int id) { if (id >= 0 && id < THEME_COUNT) current_theme = id; }

bool theme_set_by_name(const char* name) {
    for (int i = 0; i < THEME_COUNT; i++) {
        if (strcmp(name, themes[i].name) == 0) { current_theme = i; return true; }
    }
    return false;
}

const theme_t* theme_get(void) { return &themes[current_theme]; }
void theme_cycle(void) { current_theme = (current_theme + 1) % THEME_COUNT; }
const char* theme_get_name(int id) { return (id >= 0 && id < THEME_COUNT) ? themes[id].name : "unknown"; }
int theme_get_index(void) { return current_theme; }
