/* ============================================================================
 * NexusOS — Clipboard (Implementation)
 * ============================================================================
 * System-wide text clipboard for copy/paste between apps.
 * ============================================================================ */

#include "clipboard.h"
#include "string.h"

/* Clipboard buffer */
static char clip_buf[CLIPBOARD_MAX];
static bool clip_has = false;

/* --------------------------------------------------------------------------
 * clipboard_copy: Store text in clipboard
 * -------------------------------------------------------------------------- */
void clipboard_copy(const char* text) {
    if (text) {
        strncpy(clip_buf, text, CLIPBOARD_MAX - 1);
        clip_buf[CLIPBOARD_MAX - 1] = '\0';
        clip_has = true;
    }
}

/* --------------------------------------------------------------------------
 * clipboard_paste: Get clipboard contents
 * -------------------------------------------------------------------------- */
const char* clipboard_paste(void) {
    if (clip_has) return clip_buf;
    return NULL;
}

/* --------------------------------------------------------------------------
 * clipboard_clear: Empty the clipboard
 * -------------------------------------------------------------------------- */
void clipboard_clear(void) {
    clip_buf[0] = '\0';
    clip_has = false;
}

/* --------------------------------------------------------------------------
 * clipboard_has_content: Check if clipboard has data
 * -------------------------------------------------------------------------- */
bool clipboard_has_content(void) {
    return clip_has;
}
