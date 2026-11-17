/* Portal dialogs inside aurora-app (aurora_dialogs.c): file chooser and permission prompt, parameters in the environment. */
#ifndef AURORA_DIALOGS_H
#define AURORA_DIALOGS_H
int aurora_dialog_open(const char* kind);   /* "filechooser" | "prompt": the window id; exits 2 on bad parameters */
#endif
