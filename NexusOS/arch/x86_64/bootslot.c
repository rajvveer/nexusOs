/* A/B core update slots, kernel side (the loader half is uefi/abslot.c, the system half services/nexus-update).
 * The UEFI loader appends nexus.slot=a|b (the last one counts) to the command line; the booted slot's own Aurora,
 * /boot/nexus/slot-X/init, then runs as init instead of /sbin/init when it exists. /proc/cmdline shows the
 * command line, which nexus-update's boot check reads (nexus.slot, nexus.trial, nexus.rollback). */
#include "native.h"
#include "boot_info.h"
#include "../../kernel/string.h"

const char* boot_cmdline(void) { return boot_info.cmdline; }

/* "/boot/nexus/slot-X/init" for the last nexus.slot=X (X = a or b), else NULL. */
const char* boot_slot_init(char* out, unsigned size) {
    char slot = 0;
    for (const char* p = boot_info.cmdline; *p; p++) {
        if (p != boot_info.cmdline && p[-1] != ' ') continue;
        if (!strncmp(p, "nexus.slot=", 11) && (p[11] == 'a' || p[11] == 'b') && (!p[12] || p[12] == ' ')) slot = p[11];
    }
    if (!slot || size < 24) return NULL;
    memcpy(out, "/boot/nexus/slot-x/init", 24); out[17] = slot;
    return out;
}
