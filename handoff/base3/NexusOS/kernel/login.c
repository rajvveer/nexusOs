/* ============================================================================
 * NexusOS — Login Screen (Implementation)
 * ============================================================================
 * Full-screen animated boot splash → username/password → desktop.
 * Uses VGA text mode directly (not the GUI back buffer).
 * ============================================================================ */

#include "login.h"
#include "vga.h"
#include "keyboard.h"
#include "string.h"
#include "port.h"
#include "speaker.h"
#include "users.h"
#include "env.h"
#include "macui.h"
#include "framebuffer.h"
#include "mouse.h"
#include "power.h"
#include "rtc.h"
#include "usb_hid.h"

/* System tick counter */
extern volatile uint32_t system_ticks;

/* --------------------------------------------------------------------------
 * delay_ticks: Busy-wait for N timer ticks (~55ms each)
 * -------------------------------------------------------------------------- */
static void delay_ticks(uint32_t n) {
    uint32_t start = system_ticks;
    while (system_ticks - start < n) {
        vga_flush();
        __asm__ volatile("hlt");
    }
}

/* --------------------------------------------------------------------------
 * draw_centered: Draw text centered on a row
 * -------------------------------------------------------------------------- */
static void draw_centered(int row, const char* text, uint8_t color) {
    int len = strlen(text);
    int col = (80 - len) / 2;
    if (col < 0) col = 0;
    vga_set_cursor(row, col);
    vga_print_color(text, color);
}

/* --------------------------------------------------------------------------
 * draw_progress_bar: Animated loading bar
 * -------------------------------------------------------------------------- */
static void draw_progress_bar(int row, int width, int filled) {
    int start_col = (80 - width - 2) / 2;
    vga_set_cursor(row, start_col);
    vga_print_color("[", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    for (int i = 0; i < width; i++) {
        if (i < filled) {
            vga_print_color("\xDB", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
        } else {
            vga_print_color("\xB0", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
        }
    }
    vga_print_color("]", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
}

/* --------------------------------------------------------------------------
 * read_input: Read a line of input (optionally masked with *)
 * -------------------------------------------------------------------------- */
static int read_input(char* buf, int max, bool masked) {
    int len = 0;
    while (len < max - 1) {
        vga_flush();
        char c = keyboard_getchar();
        if (c == '\n') {
            buf[len] = '\0';
            vga_putchar('\n');
            return len;
        }
        if (c == '\b') {
            if (len > 0) {
                len--;
                vga_backspace();
            }
            continue;
        }
        if (c >= 32 && c < 127) {
            buf[len++] = c;
            vga_putchar(masked ? '*' : c);
        }
    }
    buf[len] = '\0';
    return len;
}

/* --------------------------------------------------------------------------
 * login_run: Main login screen
 * -------------------------------------------------------------------------- */
static uint32_t login_run_text(void) {
    /* Hide cursor during splash */
    port_byte_out(0x3D4, 0x0A);
    port_byte_out(0x3D5, 0x20);

    vga_clear();

    /* === BOOT SPLASH === */
    /* Draw NexusOS logo */
    draw_centered(5, "\xDB\xDB\xDB\xDB\xDB\xDB    NexusOS    \xDB\xDB\xDB\xDB\xDB\xDB", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    draw_centered(7, "The Hybrid Operating System", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    draw_centered(8, "v0.6.0", VGA_COLOR(VGA_WHITE, VGA_BLACK));

    /* Animated loading bar */
    int bar_width = 40;
    draw_centered(11, "Loading system...", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    for (int i = 0; i <= bar_width; i++) {
        draw_progress_bar(13, bar_width, i);
        delay_ticks(1);  /* ~55ms per step */
    }

    draw_centered(15, "\xFB System ready", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    delay_ticks(10);

    /* === LOGIN PROMPT === */
    vga_clear();

    /* Top bar */
    for (int i = 0; i < 80; i++) {
        vga_set_cursor(0, i);
        vga_print_color(" ", VGA_COLOR(VGA_WHITE, VGA_BLUE));
    }
    vga_set_cursor(0, 2);
    vga_print_color(" NexusOS Login ", VGA_COLOR(VGA_WHITE, VGA_BLUE));

    /* Login box */
    draw_centered(6, "\x0F Welcome to NexusOS \x0F", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));

    /* Decorative line */
    vga_set_cursor(7, 20);
    for (int i = 0; i < 40; i++)
        vga_print_color("\xC4", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));

    draw_centered(9, "Please log in to continue", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));

    draw_centered(19, "(try root/root or guest/guest)", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));

    /* Re-enable cursor */
    port_byte_out(0x3D4, 0x0A);
    port_byte_out(0x3D5, 14);
    port_byte_out(0x3D4, 0x0B);
    port_byte_out(0x3D5, 15);

    /* Phase 44: real authentication — loop until credentials verify. */
    char username[32];
    char password[32];
    uint32_t uid = UID_INVALID;
    while (uid == UID_INVALID) {
        /* Clear any prior input/error rows */
        for (int r = 11; r <= 13; r++) {
            vga_set_cursor(r, 0);
            for (int c = 0; c < 80; c++) vga_print(" ");
        }

        vga_set_cursor(11, 25);
        vga_print_color("Username: ", VGA_COLOR(VGA_WHITE, VGA_BLACK));
        read_input(username, 32, false);

        vga_set_cursor(12, 25);
        vga_print_color("Password: ", VGA_COLOR(VGA_WHITE, VGA_BLACK));
        read_input(password, 32, true);

        uid = users_authenticate(username, password);
        if (uid == UID_INVALID) {
            play_error_sound();
            vga_set_cursor(13, 25);
            vga_print_color("Invalid username or password.",
                            VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
            /* brief pause so the message is visible */
            delay_ticks(18);
        }
    }

    /* Authenticated — make this the current user. */
    users_set_current(uid);
    env_sync_user();

    /* Welcome message */
    /* Hide cursor for animation */
    port_byte_out(0x3D4, 0x0A);
    port_byte_out(0x3D5, 0x20);

    vga_set_cursor(14, 0);
    draw_centered(14, "\xFB Login successful!", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));

    char welcome[60];
    strcpy(welcome, "Welcome, ");
    if (username[0] != '\0') {
        strcat(welcome, username);
    } else {
        strcat(welcome, "admin");
    }
    strcat(welcome, "!");
    draw_centered(15, welcome, VGA_COLOR(VGA_WHITE, VGA_BLACK));

    draw_centered(17, "Starting desktop environment...", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));

    /* Brief animation */
    for (int i = 0; i < 3; i++) {
        delay_ticks(6);
        vga_set_cursor(19, 38 + i);
        vga_print_color(".", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    }
    delay_ticks(10);

    /* Re-enable cursor */
    port_byte_out(0x3D4, 0x0A);
    port_byte_out(0x3D5, 14);
    port_byte_out(0x3D4, 0x0B);
    port_byte_out(0x3D5, 15);

    vga_clear();
    vga_flush();

    return uid;
}

/* ============================================================================
 * Aurora (VESA) login window — macOS style
 * ============================================================================
 * Big clock over the darkened wallpaper, user pictures, a pill password field
 * that shakes on a wrong password, and Restart / Shut Down at the bottom.
 * Typing a name + Enter, then a password + Enter, works exactly like the text
 * login (the headless test harness relies on that).
 * ============================================================================ */

#define LG_FIELD_W 252
#define LG_FIELD_H 34

typedef struct {
    bool     lock;            /* lock-screen mode: current user only */
    bool     pass_stage;      /* false = choosing/typing a name */
    char     name[32];
    char     pass[32];
    int      nlen, plen;
    int      shake;           /* frames of shake animation left */
    int      fails;
    const char* msg;          /* one-line status under the field */
} login_ui_t;

static const int SHAKE[8] = { 0, 5, -5, 9, -9, 12, -12, 0 };

static int lg_users(const char** names) {
    int n = 0;
    const char* r = users_name_of(UID_ROOT);
    const char* g = users_name_of(1000);
    if (r && r[0] != '?') names[n++] = r;
    if (g && g[0] != '?') names[n++] = g;
    return n;
}

/* Small user tiles shown in the name stage; returns hit index for (mx,my). */
static int lg_tiles(int draw, int mx, int my) {
    const char* names[2];
    int n = lg_users(names), sw = (int)fb_get_width(), sh = (int)fb_get_height();
    int y = sh * 58 / 100, hit = -1;
    for (int i = 0; i < n; i++) {
        int cx = sw / 2 + (i * 2 - (n - 1)) * 80;
        if (draw) {
            ui_avatar(cx, y, 40);
            ui_text_center(cx, y + 50, names[i], UF_UIB, 0xFFFFFF);
        }
        if (mx >= cx - 44 && mx <= cx + 44 && my >= y - 44 && my <= y + 70) hit = i;
    }
    return hit;
}

static void lg_power_buttons(int sw, int sh) {
    static const char* labels[2] = { "Restart", "Shut Down" };
    for (int i = 0; i < 2; i++) {
        int cx = sw / 2 + (i * 2 - 1) * 70, cy = sh - 96;
        ui_circle(cx, cy, 18, 0xFFFFFF, 40);
        if (i == 0) {
            ui_ring(cx, cy, 8, 2, 0xFFFFFF, 220);
            ui_tri(cx + 3, cy - 12, cx + 10, cy - 7, cx + 2, cy - 3, 0xFFFFFF, 240);
        } else {
            ui_ring(cx, cy + 1, 9, 2, 0xFFFFFF, 230);
            ui_rrect(cx - 3, cy - 12, 7, 9, 2, UI_RGB(70, 70, 80), 200);
            ui_rrect(cx - 1, cy - 11, 3, 10, 1, 0xFFFFFF, 255);
        }
        ui_text_center(cx, cy + 24, labels[i], UF_SMALL, UI_RGB(230, 230, 236));
    }
}

static int lg_power_hit(int mx, int my) {
    int sw = (int)fb_get_width(), sh = (int)fb_get_height();
    for (int i = 0; i < 2; i++) {
        int cx = sw / 2 + (i * 2 - 1) * 70, cy = sh - 96;
        if (mx >= cx - 30 && mx <= cx + 30 && my >= cy - 20 && my <= cy + 40) return i + 1;
    }
    return 0;
}

static void lg_draw(login_ui_t* L, int mx, int my) {
    static const char* const DAYS[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char* const SHORT[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    int sw = (int)fb_get_width(), sh = (int)fb_get_height();
    ui_wallpaper_draw();
    ui_blend_rect(0, 0, sw, sh, UI_RGB(8, 6, 20), 70);

    /* Date + big clock */
    char clk[32], date[40], hm[6];
    rtc_time_t t;
    rtc_read(&t);
    ui_format_clock(clk);                 /* "Thu 8 Oct  12:40" */
    int d = 0;
    for (int i = 0; i < 7; i++) if (!strncmp(clk, SHORT[i], 3)) d = i;
    strcpy(date, DAYS[d]);
    strcat(date, ",");
    int k = 3, dl = (int)strlen(date);
    while (clk[k] && !(clk[k] == ' ' && clk[k + 1] == ' ') && dl < 38) date[dl++] = clk[k++];
    date[dl] = 0;
    hm[0] = (char)('0' + t.hour / 10); hm[1] = (char)('0' + t.hour % 10); hm[2] = ':';
    hm[3] = (char)('0' + t.minute / 10); hm[4] = (char)('0' + t.minute % 10); hm[5] = 0;
    ui_text_center(sw / 2, sh * 9 / 100, date, UF_TITLE, UI_RGB(240, 240, 246));
    ui_text_center(sw / 2, sh * 9 / 100 + 26, hm, UF_HUGE, UI_RGB(246, 246, 250));

    int off = L->shake > 0 ? SHAKE[L->shake & 7] : 0;
    int fy;
    if (!L->pass_stage) {
        lg_tiles(1, -1, -1);
        fy = sh * 58 / 100 + 92;
        int fx = sw / 2 - LG_FIELD_W / 2 + off;
        ui_rrect(fx, fy, LG_FIELD_W, LG_FIELD_H, LG_FIELD_H / 2, 0xFFFFFF, 52);
        ui_rrect_stroke(fx, fy, LG_FIELD_W, LG_FIELD_H, LG_FIELD_H / 2, 0xFFFFFF, 60);
        int ty = fy + (LG_FIELD_H - ui_font_height(UF_UI)) / 2 + 1;
        if (L->nlen) {
            int e = ui_text(fx + 16, ty, L->name, UF_UI, 0xFFFFFF);
            ui_blend_rect(e + 1, fy + 8, 1, LG_FIELD_H - 16, 0xFFFFFF, 220);
        } else {
            ui_text(fx + 16, ty, "Name", UF_UI, UI_RGB(225, 225, 232));
        }
    } else {
        int ay = sh * 56 / 100;
        ui_avatar(sw / 2, ay, 58);
        ui_text_center(sw / 2, ay + 70, L->name, UF_TITLE, 0xFFFFFF);
        fy = ay + 112;
        int fx = sw / 2 - LG_FIELD_W / 2 + off;
        ui_rrect(fx, fy, LG_FIELD_W, LG_FIELD_H, LG_FIELD_H / 2, 0xFFFFFF, 52);
        ui_rrect_stroke(fx, fy, LG_FIELD_W, LG_FIELD_H, LG_FIELD_H / 2, 0xFFFFFF, 60);
        int ty = fy + (LG_FIELD_H - ui_font_height(UF_UI)) / 2 + 1;
        if (L->plen) {
            for (int i = 0; i < L->plen && i < 16; i++) ui_circle(fx + 22 + i * 13, fy + LG_FIELD_H / 2, 4, 0xFFFFFF, 255);
        } else {
            ui_text(fx + 16, ty, L->lock ? "Enter Password to Unlock" : "Enter Password", UF_UI, UI_RGB(225, 225, 232));
        }
        /* arrow button */
        int bx = fx + LG_FIELD_W - LG_FIELD_H / 2 - 1, by = fy + LG_FIELD_H / 2;
        int a = L->plen ? 230 : 110;
        ui_ring(bx, by, 11, 2, 0xFFFFFF, a);
        ui_line(bx - 4, by, bx + 4, by, 2, 0xFFFFFF, a);
        ui_line(bx + 1, by - 4, bx + 5, by, 2, 0xFFFFFF, a);
        ui_line(bx + 1, by + 4, bx + 5, by, 2, 0xFFFFFF, a);
    }
    if (L->msg) ui_text_center(sw / 2, fy + LG_FIELD_H + 14, L->msg, UF_UI, UI_RGB(235, 235, 240));
    if (L->fails >= 3 && L->pass_stage)
        ui_text_center(sw / 2, fy + LG_FIELD_H + 36, "Hint: the default password is the user name", UF_SMALL, UI_RGB(200, 200, 210));

    if (!L->lock) lg_power_buttons(sw, sh);
    else ui_text_center(sw / 2, sh - 90, "Locked", UF_UI, UI_RGB(220, 220, 228));
    ui_cursor_draw(mx, my);
    fb_mark_dirty_all();
    fb_flip();
}

static uint32_t lg_try(login_ui_t* L) {
    uint32_t uid = users_authenticate(L->name, L->pass);
    if (uid == UID_INVALID) {
        L->fails++;
        L->shake = 7;
        L->plen = 0; L->pass[0] = 0;
        L->msg = "Incorrect password";
        play_error_sound();
    }
    return uid;
}

static void lg_select(login_ui_t* L, const char* name) {
    strncpy(L->name, name, sizeof(L->name) - 1);
    L->name[sizeof(L->name) - 1] = 0;
    L->nlen = (int)strlen(L->name);
    L->pass_stage = true;
    L->plen = 0; L->pass[0] = 0;
    L->msg = NULL;
}

/* Fade the screen towards black before handing over to the desktop. */
static void lg_fade(void) {
    int sw = (int)fb_get_width(), sh = (int)fb_get_height();
    for (int i = 0; i < 6; i++) {
        ui_blend_rect(0, 0, sw, sh, 0, 70);
        fb_mark_dirty_all();
        fb_flip();
        delay_ticks(1);
    }
}

static uint32_t login_gui(bool lock) {
    login_ui_t L;
    memset(&L, 0, sizeof(L));
    L.lock = lock;
    if (lock) lg_select(&L, users_current_name());
    bool dirty = true, pl = false;
    int last_min = -1, pmx = -1, pmy = -1;
    uint32_t uid = UID_INVALID;
    while (keyboard_has_key()) keyboard_getchar();      /* drop stale keys */

    for (;;) {
        usb_hid_poll();   /* Phase 54: let a USB keyboard/mouse drive login too */
        mouse_state_t ms = mouse_get_state();
        bool ln = (ms.buttons & MOUSE_LEFT) != 0;
        bool click = (ln && !pl) || (ms.clicked && !ln && !pl);
        pl = ln;
        mouse_clear_events();
        if (ms.px != pmx || ms.py != pmy) { pmx = ms.px; pmy = ms.py; dirty = true; }

        if (click) {
            if (!lock && !L.pass_stage) {
                const char* names[2];
                int n = lg_users(names), hit = lg_tiles(0, ms.px, ms.py);
                if (hit >= 0 && hit < n) lg_select(&L, names[hit]);
            }
            int p = lock ? 0 : lg_power_hit(ms.px, ms.py);
            if (p == 1) power_restart();
            if (p == 2) power_shutdown();
            dirty = true;
        }

        while (keyboard_has_key()) {
            char c = keyboard_getchar();
            dirty = true;
            if (!L.pass_stage) {
                if (c == '\n') {
                    if (L.nlen && users_find(L.name)) lg_select(&L, L.name);
                    else if (L.nlen) { L.shake = 7; L.msg = "Unknown user"; play_error_sound(); }
                } else if (c == '\b') { if (L.nlen) L.name[--L.nlen] = 0; }
                else if (c >= 32 && c < 127 && L.nlen < 31) { L.name[L.nlen++] = c; L.name[L.nlen] = 0; L.msg = NULL; }
            } else {
                if (c == '\n') {
                    uid = lg_try(&L);
                    if (uid != UID_INVALID) break;
                } else if (c == 27 && !lock) {
                    L.pass_stage = false; L.plen = 0; L.pass[0] = 0; L.msg = NULL;
                } else if (c == '\b') { if (L.plen) L.pass[--L.plen] = 0; }
                else if (c >= 32 && c < 127 && L.plen < 31) { L.pass[L.plen++] = c; L.pass[L.plen] = 0; L.msg = NULL; }
            }
        }
        if (uid != UID_INVALID) break;

        rtc_time_t t;
        rtc_read(&t);
        if (t.minute != last_min) { last_min = t.minute; dirty = true; }
        if (L.shake > 0) { L.shake--; dirty = true; }
        if (dirty) { lg_draw(&L, ms.px, ms.py); dirty = false; }
        __asm__ volatile("hlt");
    }
    memset(L.pass, 0, sizeof(L.pass));
    lg_fade();
    return uid;
}

uint32_t login_run(void) {
    if (!fb_is_vesa()) return login_run_text();
    ui_splash_end();
    uint32_t uid = login_gui(false);
    users_set_current(uid);
    env_sync_user();
    return uid;
}

void login_lock_screen(void) {
    if (fb_is_vesa()) login_gui(true);
}
