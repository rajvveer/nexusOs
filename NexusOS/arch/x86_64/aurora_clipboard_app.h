/* aurora-app clipboard: the clipboard history picker (Super+V; aurora_clipboard.h). */
#ifndef AURORA_CLIPBOARD_APP_H
#define AURORA_CLIPBOARD_APP_H
#include <stdbool.h>
int aurora_clipboard_open(void);
bool aurora_clipboard_tick(void);
#endif
