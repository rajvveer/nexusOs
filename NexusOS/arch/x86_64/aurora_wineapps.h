#ifndef NEXUS_AURORA_WINEAPPS_H
#define NEXUS_AURORA_WINEAPPS_H
/* Windows programs installed through Wine, listed in Spotlight and Launchpad (aurora_wineapps.c). */
#include <stdbool.h>
#include "../../kernel/macui.h"

#define WINEAPPS_ACTION_BASE 900   /* shell actions 900.. launch the listed programs */
#define WINEAPPS_ICON_BASE 1000    /* ui_icon ids 1000.. draw their icons */
#define WINEAPPS_MAX 96

/* Rebuild the shell registry: the fixed apps followed by the visible .desktop entries under
 * $HOME/.local/share/applications (Wine's winemenubuilder output). Returns the number of Windows programs. */
int wineapps_refresh(const ui_app_t* fixed, int fixed_count, int dock_count);
/* Start the program behind a WINEAPPS_ACTION_BASE action; returns its pid (0: not a program action, <0: failed). */
int wineapps_launch(int action, char* name, unsigned name_size);
/* A reaped child that was a program launcher: the launcher (start.exe /exec, which waits for whatever the shortcut
 * started) may end with any status while the program keeps running, so its status says nothing about the program. */
bool wineapps_reaped(int pid);
/* The name of the program behind an action (NULL when it is not one). */
const char* wineapps_name(int action);
/* Run a Windows file with Wine (Files: .exe/.msi/.bat/.cmd/.com/.lnk); returns the pid or -errno. */
int wineapps_open_file(const char* path);
bool wineapps_is_windows_file(const char* path);   /* by suffix; .exe/.com also need the MZ header */
#endif
