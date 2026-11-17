/* The seat's console user: who may open the console devices (keyboard/mouse evdev nodes, /dev/fb0, the sound nodes).
 * On Linux logind gives them to the user of the active session (udev "uaccess" ACLs, EVIOCREVOKE on session switch);
 * NexusOS has one seat and its display server is init (Aurora, pid 1), which sets the active session's user here
 * whenever it switches sessions (prctl PR_NEXUS_SEAT, below). Default 1000: the desktop user, as before.
 *   - opening a console device: root (fsuid 0 or CAP_SYS_ADMIN), pid 1, or the console user (device.c);
 *   - input (and /dev/fb0) readers opened by anyone else - also root daemons opening with a user's fsuid, as logind's
 *     TakeDevice does - are revoked when the console user changes: read/ioctl fail with ENODEV and poll reports them
 *     readable (Linux EVIOCREVOKE semantics), so a process left behind in an inactive session cannot read the keys
 *     typed in the active one; readers of pid 1 and of fsuid 0 are never revoked. (A mapping of /dev/fb0 is not.)
 * prctl(PR_NEXUS_SEAT, 0) -> the console uid; prctl(PR_NEXUS_SEAT, 1, uid) sets it (pid 1 or CAP_SYS_ADMIN, else EPERM;
 * uid -1 is EINVAL). */
#include "native.h"

#define TRUSTED 0x80000000U
static uint32_t console_uid = 1000;
static uint32_t generation;   /* bumped on every change (31 bits); readers remember the value at open */

static bool privileged(void) {
    struct credentials* who = credentials_current();
    if (credentials_capable(who, NULL, 21)) return true;   /* CAP_SYS_ADMIN in the initial namespace */
    return task_pid() == 1;                                /* the display server (internal pid 1 = the real init) */
}
uint32_t seat_console_uid(void) { return __atomic_load_n(&console_uid, __ATOMIC_ACQUIRE); }
bool seat_device_access(void) {
    uint32_t uid = credentials_kernel_id(credentials_current(), false, 3);   /* fsuid, as an ACL check */
    return !uid || uid == seat_console_uid() || privileged();
}
uint32_t seat_open_token(void) {   /* a root daemon opening for a user (setfsuid: logind TakeDevice) is revocable too */
    if (!credentials_kernel_id(credentials_current(), false, 3) || task_pid() == 1) return TRUSTED;
    return __atomic_load_n(&generation, __ATOMIC_ACQUIRE) & ~TRUSTED;
}
bool seat_revoked(uint32_t token) { return !(token & TRUSTED) && token != (__atomic_load_n(&generation, __ATOMIC_ACQUIRE) & ~TRUSTED); }
int64_t seat_prctl(uint64_t operation, uint64_t value) {
    if (operation == 0) return value ? -22 : (int64_t)seat_console_uid();
    if (operation != 1) return -22;
    if (value >= 0xFFFFFFFFULL) return -22;
    if (!privileged()) return -1;
    if ((uint32_t)value != seat_console_uid()) {   /* generation first: a reader opened after the change keeps working */
        __atomic_store_n(&generation, (__atomic_load_n(&generation, __ATOMIC_RELAXED) + 1) & ~TRUSTED, __ATOMIC_RELEASE);
        __atomic_store_n(&console_uid, (uint32_t)value, __ATOMIC_RELEASE);
    }
    return 0;
}
