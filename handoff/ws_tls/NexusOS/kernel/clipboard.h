/* ============================================================================
 * NexusOS — Clipboard (Header)
 * ============================================================================ */

#ifndef CLIPBOARD_H
#define CLIPBOARD_H

#include "types.h"

#define CLIPBOARD_MAX 256

/* Copy text to the clipboard */
void clipboard_copy(const char* text);

/* Get pointer to clipboard contents (or NULL if empty) */
const char* clipboard_paste(void);

/* Clear the clipboard */
void clipboard_clear(void);

/* Check if clipboard has content */
bool clipboard_has_content(void);

#endif /* CLIPBOARD_H */
