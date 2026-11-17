/* Clipboard history of the Aurora shell (aurora_clipboard.c).
 *
 * Every time an application of the active session sets the clipboard (wl_data_device.set_selection) with text, Aurora
 * reads that text from the source (as a clipboard manager does) and keeps it, newest first, at most
 * AURORA_CLIPBOARD_ENTRIES per user and AURORA_CLIPBOARD_MAX bytes each, in memory only (never on disk); a session's
 * history goes when it logs out. Super+V opens the picker (aurora-app clipboard): Up/Down choose, Enter puts the entry
 * back on the clipboard (Aurora itself serves it), Delete removes it, "Clear History" (or C) forgets everything.
 * Privacy: nothing is kept when the copy comes from a password field - the source offers the
 * x-kde-passwordManagerHint type (KeePassXC and other password managers), or the focused text input of the copying
 * application has the password or PIN purpose or the sensitive-data hint (text-input-v3, aurora_ime.c) - nor while
 * the screen is locked. Log lines never contain the text.
 * Control commands (root or the active session's user; the active session's history only):
 *   clipboard-history           -> "entries=N" then "entry\tINDEX\tBYTES\tSECONDS-AGO\tPREVIEW" lines (newest first)
 *   clipboard-use INDEX         the entry becomes the clipboard again (and the newest)
 *   clipboard-remove INDEX      clipboard-clear */
#ifndef AURORA_CLIPBOARD_H
#define AURORA_CLIPBOARD_H
#include <stdbool.h>
#include <stddef.h>
#include <wayland-server-core.h>

#define AURORA_CLIPBOARD_ENTRIES 25
#define AURORA_CLIPBOARD_MAX (256 * 1024)
#define AURORA_ACTION_CLIPBOARD 149   /* desktop action: the picker (aurora-app clipboard) */

void aurora_clipboard_init(struct wl_event_loop* loop);
/* desktop.c, set_selection: fd is the read end of a pipe the new source writes its text into (Aurora asked for it);
 * the history reads it in the background. */
void aurora_clipboard_capture(int fd, unsigned uid);
/* desktop.c, a client receives Aurora's own selection: data goes into fd (the history's copy; fd is the history's now) */
void aurora_clipboard_serve(int fd, const char* data, size_t size);
void aurora_clipboard_forget(unsigned uid);   /* that user's session ended */

/* ---- services of desktop.c ---- */
bool aurora_host_set_text_selection(const char* data, size_t size);   /* Aurora's own source with the text types */
/* ---- services of aurora_ime.c ---- */
bool aurora_ime_secret_focus(void);   /* the focused text input is a password / PIN / sensitive field */
#endif
