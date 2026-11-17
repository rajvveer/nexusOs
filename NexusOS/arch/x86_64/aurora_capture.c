/* Screen capture for the desktop portal (xdg-desktop-portal-aurora: Screenshot, ScreenCast), over the control socket.
 *   screenshot [OUTPUT]   + one descriptor (SCM_RIGHTS, a file or memfd opened for writing by the requester)
 *   frame [OUTPUT]          the same, for screen-cast frames
 * Aurora writes "NXFRAME 1 <width> <height> <stride bytes> <x> <y>\n" and the display's composed picture (XRGB8888,
 * the last presented frame: only the active session's windows are ever composed, aurora_session.h) into it. The
 * control socket only runs commands of the active session's user (and root), so one user cannot capture another's
 * screen; a locked screen is captured as the lock screen. OUTPUT is a display name (Settings > Displays), default the
 * primary display. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "aurora_capture.h"
#include "aurora_control.h"
#include "aurora_outputs.h"

static bool write_all(int fd, const void* data, size_t size) {
    const char* p = data;
    while (size) { ssize_t n = write(fd, p, size); if (n < 0 && errno == EINTR) continue; if (n <= 0) return false; p += n; size -= (size_t)n; }
    return true;
}
static bool capture(const char* args, struct aurora_control_reply* reply, char* error, size_t size, const char* what) {
    int fd = aurora_control_peer_fd();
    if (fd < 0) { snprintf(error, size, "%s needs a descriptor (SCM_RIGHTS)", what); return false; }
    struct aurora_output* o = NULL; char name[32] = "";
    if (sscanf(args, "%31s", name) == 1) {
        for (int i = 0; i < aurora_outputs_count() && !o; i++) { struct aurora_output* c = aurora_output_slot(i); if (c && c->enabled && !strcmp(c->name, name)) o = c; }
        if (!o) { close(fd); snprintf(error, size, "no display %.31s", name); return false; }
    } else o = aurora_output_primary();
    if (o && o->mirror >= 0) o = aurora_output_slot(o->mirror);
    if (!o || !o->canvas || o->mode_w <= 0 || o->mode_h <= 0) { close(fd); snprintf(error, size, "nothing to capture"); return false; }
    char head[96]; int n = snprintf(head, sizeof(head), "NXFRAME 1 %d %d %d %d %d\n", o->mode_w, o->mode_h, o->mode_w * 4, o->x, o->y);
    bool ok = write_all(fd, head, (size_t)n) && write_all(fd, o->canvas, (size_t)o->mode_w * (size_t)o->mode_h * 4);
    close(fd);
    if (!ok) { snprintf(error, size, "write: %s", strerror(errno)); return false; }
    aurora_control_print(reply, "%s %s %dx%d\n", what, o->name, o->mode_w, o->mode_h);
    return true;
}
static bool control_screenshot(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)body; bool ok = capture(args, reply, error, size, "screenshot");
    if (ok) { printf("[64] AURORA SCREENSHOT uid=%u\n", (unsigned)aurora_control_peer_uid()); fflush(stdout); }
    return ok;
}
static bool control_frame(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)body; return capture(args, reply, error, size, "frame");
}
void aurora_capture_init(void) {
    aurora_control_register("screenshot", false, control_screenshot);
    aurora_control_register("frame", false, control_frame);
}
