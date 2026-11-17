#ifndef NEXUS_AURORA_NETSTATUS_H
#define NEXUS_AURORA_NETSTATUS_H
#include <stdbool.h>
#define AURORA_ACTION_NETWORK 47                 /* desktop action: open the Network settings (aurora-app network) */
bool aurora_netstatus_poll(void);                /* true when the menu-bar item must be redrawn (samples every 2 s) */
int aurora_netstatus_draw(int right, int cy);    /* draws the item ending at `right`; returns its left edge */
void aurora_netstatus_boot(void);                /* run `aurora-app network --apply` once at desktop start */
#endif
