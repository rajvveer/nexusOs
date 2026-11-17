/* Aurora input methods (aurora_ime.c): zwp_text_input_v3 for applications, zwp_input_method_v2 (+ keyboard grab and
 * candidate popup) for one input-method program (an IME framework such as fcitx5), zwp_virtual_keyboard_v1 for that
 * program only (keys it does not use go back to the application). Other clients do not even see those two globals.
 *
 * Flow: the keyboard-focused client's text input is entered; when it enables itself and commits, the input method gets
 * activate + surrounding text + content type + done; while the input method holds a keyboard grab, physical keys go
 * to it instead of the application; its commit(serial) delivers preedit / commit_string / delete_surrounding_text to
 * the application followed by done. Its popup surface is drawn below the application's cursor rectangle. */
#ifndef AURORA_IME_H
#define AURORA_IME_H
#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

/* ---- services of desktop.c ---- */
struct wl_resource* aurora_host_focus_resource(void);                        /* keyboard focus wl_surface, or NULL */
bool aurora_host_surface_origin(struct wl_resource* surface, int* x, int* y);  /* desktop position of a wl_surface */
void aurora_host_inject_key(uint32_t time, uint32_t key, uint32_t state);    /* to the focused client, past the grab */
void aurora_host_inject_modifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);
bool aurora_host_keymap_fd(int* fd, uint32_t* size);
bool aurora_host_claim_ime_popup(struct wl_resource* surface);               /* role check: false if it has another role */
void aurora_host_ime_redraw(void);

/* ---- aurora_ime.c ----
 * The input-method program is named in AURORA_IME_CONF ("program=/usr/bin/fcitx5 -D", up to 8 words); Aurora starts it
 * once the session is up, over a socketpair (WAYLAND_SOCKET=3), restarts it up to AURORA_IME_RESTARTS times, and shows
 * zwp_input_method_manager_v2 / zwp_virtual_keyboard_manager_v1 to that client only (aurora_ime_init installs the
 * display's global filter: a module that needs one too must extend global_filter in aurora_ime.c). */
#define AURORA_IME_CONF "/home/nexus/.aurora/input-method.conf"
#define AURORA_IME_RESTARTS 5
bool aurora_ime_init(struct wl_display* display);
void aurora_ime_restarts(bool allowed);   /* false while the session shuts down; true again starts it if it died */
void aurora_ime_focus(struct wl_resource* surface);         /* keyboard focus moved (NULL: nobody) */
bool aurora_ime_key(uint32_t time, uint32_t key, uint32_t state);   /* true: the input method's grab took it */
void aurora_ime_modifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);
void aurora_ime_keymap_changed(void);
struct wl_resource* aurora_ime_popup(int* x, int* y);       /* the candidate popup to draw and where (desktop) */
#endif
