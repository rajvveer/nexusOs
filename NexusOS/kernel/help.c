/* ============================================================================
 * NexusOS — Help System (Implementation)
 * ============================================================================
 * Built-in documentation viewer with multiple sections.
 * ============================================================================ */

#include "help.h"
#include "window.h"
#include "gui.h"
#include "theme.h"
#include "vga.h"
#include "string.h"

static int help_section = 0;
static int help_scroll = 0;

#define HELP_SECTIONS 5

static const char* section_names[HELP_SECTIONS] = {
    "Shortcuts", "Commands", "Apps", "About", "Changelog"
};

/* Section content - arrays of lines */
static const char* sec_shortcuts[] = {
    "Desktop Keyboard Shortcuts:",
    "",
    "Esc        Exit desktop",
    "Tab        Alt+Tab switcher",
    "Ctrl+P     Command Palette",
    "Ctrl+T     Open Terminal",
    "Ctrl+F     File Manager",
    "Ctrl+K     Calculator",
    "Ctrl+S     System Monitor",
    "Ctrl+N     Settings",
    "Ctrl+B     Notepad",
    "Ctrl+G     Task Manager",
    "Ctrl+D     Calendar",
    "Ctrl+L     Lock Screen",
    "Ctrl+W     Close Window",
    "Ctrl+V     Paste Clipboard",
    "Ctrl+I     Toggle Widgets",
    "Ctrl+Left  Snap Left",
    "Ctrl+Right Snap Right",
    "Ctrl+Up    Maximize",
    "Ctrl+Down  Restore",
    "Right-Click Context Menu",
    NULL
};

static const char* sec_commands[] = {
    "Shell Commands (v9.0):",
    "",
    "System:",
    "  help about clear reboot",
    "  uptime date mem heap",
    "",
    "Files:",
    "  ls cd mkdir cat write rm",
    "",
    "Process:",
    "  ps kill <pid> run <name>",
    "",
    "Desktop:",
    "  gui theme <n> calc files",
    "  sysmon settings notepad",
    "  taskmgr calendar",
    "",
    "Fun:",
    "  snake beep echo color",
    "  history",
    "",
    "Up/Down arrows for history",
    NULL
};

static const char* sec_apps[] = {
    "Installed Applications:",
    "",
    "\x10 Terminal    GUI terminal",
    "\x10 File Manager  Browse FS",
    "\x10 Calculator  Math tool",
    "\x10 Sys Monitor CPU/mem/proc",
    "\x10 Settings   Theme/wallpaper",
    "\x10 Notepad    Text editor",
    "\x10 Task Manager Processes",
    "\x10 Calendar   Date viewer",
    "\x10 Music Player PC speaker",
    "\x10 Paint      ASCII drawing",
    "\x10 Snake Game Classic snake",
    "\x10 Help       This viewer",
    NULL
};

static const char* sec_about[] = {
    "NexusOS v0.9.0",
    "The Hybrid Operating System",
    "",
    "Built from scratch in C and",
    "x86 Assembly. No external",
    "libraries or OS dependencies.",
    "",
    "Inspired by Windows, macOS,",
    "and Linux - combining the",
    "best of all three.",
    "",
    "Features:",
    "  Custom bootloader",
    "  Protected mode kernel",
    "  Virtual filesystem (RamFS)",
    "  Process scheduler",
    "  GUI window manager",
    "  Mouse + keyboard input",
    "  16-color VGA text mode",
    "  PC speaker audio",
    NULL
};

static const char* sec_changelog[] = {
    "Changelog:",
    "",
    "v0.9 Phase 9:",
    "  Music, help, paint, widgets",
    "  Window snap, shell history",
    "",
    "v0.8 Phase 8:",
    "  Palette, notepad, taskmgr",
    "  Calendar, system tray",
    "",
    "v0.7 Phase 7:",
    "  Alt+Tab, min/max, lock",
    "  Screensaver, clipboard",
    "",
    "v0.6 Phase 6:",
    "  Login, icons, context menu",
    "  Notifications, settings",
    "",
    "v0.5 Phase 5:",
    "  Start menu, calc, files",
    "  System monitor",
    NULL
};

static const char** sections[HELP_SECTIONS] = {
    sec_shortcuts, sec_commands, sec_apps, sec_about, sec_changelog
};

static int section_line_count(int s) {
    int c = 0;
    while (sections[s][c] != NULL) c++;
    return c;
}

static void help_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    const theme_t* t = theme_get();
    uint8_t tc = t->win_content;
    uint8_t bg = (tc >> 4) & 0x0F;
    uint8_t accent = VGA_COLOR(VGA_LIGHT_CYAN, bg);
    uint8_t dim = VGA_COLOR(VGA_DARK_GREY, bg);
    uint8_t hi = t->menu_highlight;

    int row = cy;

    /* Tab bar */
    int tx = cx;
    for (int i = 0; i < HELP_SECTIONS; i++) {
        bool sel = (i == help_section);
        uint8_t col = sel ? hi : dim;
        int len = strlen(section_names[i]);
        if (tx + len + 2 > cx + cw) break;
        gui_putchar(tx, row, ' ', col);
        gui_draw_text(tx + 1, row, section_names[i], col);
        gui_putchar(tx + 1 + len, row, ' ', col);
        tx += len + 2;
    }
    row++;

    /* Separator */
    for (int i = 0; i < cw - 1; i++) gui_putchar(cx + i, row, (char)0xC4, dim);
    row++;

    /* Content */
    int vis = ch - 3;
    int total = section_line_count(help_section);
    if (help_scroll > total - vis) help_scroll = total - vis;
    if (help_scroll < 0) help_scroll = 0;

    for (int i = 0; i < vis && (help_scroll + i) < total; i++) {
        const char* line = sections[help_section][help_scroll + i];
        int j = 0;
        while (line[j] && j < cw - 2) {
            gui_putchar(cx + j, row + i, line[j], tc); j++;
        }
    }

    /* Scroll indicator */
    if (total > vis) {
        int bar_h = vis;
        int thumb = (help_scroll * bar_h) / total;
        for (int i = 0; i < bar_h; i++) {
            gui_putchar(cx + cw - 2, row + i, i == thumb ? (char)0xDB : (char)0xB3, accent);
        }
    }

    gui_draw_text(cx, cy + ch - 1, "Tab:section Up/Dn:scroll", dim);
}

static void help_key(int id, char key) {
    (void)id;
    if (key == '\t') { help_section = (help_section + 1) % HELP_SECTIONS; help_scroll = 0; }
    if ((unsigned char)key == 0x80) { if (help_scroll > 0) help_scroll--; }
    if ((unsigned char)key == 0x81) help_scroll++;
}

int help_open(void) {
    help_section = 0; help_scroll = 0;
    return window_create("Help", 16, 1, 36, 20, help_draw, help_key);
}
