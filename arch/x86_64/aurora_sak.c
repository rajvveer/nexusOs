/* The secure attention key (aurora_sak.h). */
#define _GNU_SOURCE
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <wayland-server-core.h>
#include "aurora_sak.h"
#include "aurora_lock.h"
#include "aurora_session.h"

static bool held;   /* the Escape press was ours: so is its release */

/* argv of a process from /proc/PID/cmdline: count, with the fields NUL-separated in buffer */
static int command_line(pid_t pid, char* buffer, size_t size, const char** argv, int max) {
    char path[64]; snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) return 0;
    ssize_t n = read(fd, buffer, size - 1); close(fd); if (n <= 0) return 0;
    buffer[n] = 0; int count = 0;
    for (ssize_t i = 0; i < n && count < max; i += (ssize_t)strlen(buffer + i) + 1) argv[count++] = buffer + i;
    return count;
}
/* code loaded into the process by the dynamic loader on the environment's say */
static bool injected(pid_t pid) {
    char path[64], env[16384]; snprintf(path, sizeof(path), "/proc/%d/environ", (int)pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) return true;
    ssize_t n = read(fd, env, sizeof(env) - 1); close(fd); if (n < 0) return true;
    env[n] = 0;
    for (ssize_t i = 0; i < n; i += (ssize_t)strlen(env + i) + 1)
        if (!strncmp(env + i, "LD_PRELOAD=", 11) || !strncmp(env + i, "LD_LIBRARY_PATH=", 16) || !strncmp(env + i, "LD_AUDIT=", 9)) return true;
    return false;
}
/* the genuine dialog: the process image is Aurora's aurora-app (same file as /bin/aurora-app), it runs "authenticate",
 * and the loader was not told to add code to it */
static bool genuine_dialog(pid_t pid, char* app, size_t app_size, char* action, size_t action_size) {
    char exe[64], image[PATH_MAX], real[PATH_MAX]; snprintf(exe, sizeof(exe), "/proc/%d/exe", (int)pid);
    ssize_t n = readlink(exe, image, sizeof(image) - 1); if (n <= 0) return false; image[n] = 0;
    if (!realpath("/bin/aurora-app", real) || strcmp(image, real) || injected(pid)) return false;
    char buffer[4096]; const char* argv[64]; int argc = command_line(pid, buffer, sizeof(buffer), argv, 64);
    if (argc < 3 || strcmp(argv[1], "authenticate")) return false;
    snprintf(action, action_size, "%s", argv[2]); snprintf(app, app_size, "an application");
    for (int i = 3; i + 1 < argc; i++) if (!strcmp(argv[i], "--app")) snprintf(app, app_size, "%s", argv[i + 1]); else if (!strcmp(argv[i], "--")) break;
    return true;
}
/* The dialog's Wayland connection is judged when it is made, not when the key is pressed: a process that connected
 * first and then ran /bin/aurora-app (keeping the connection in a child) does not pass. */
#define DIALOGS 8
struct dialog { struct wl_listener gone; struct wl_client* client; pid_t pid; char app[96], action[128]; };
static struct dialog dialogs[DIALOGS];
static struct wl_listener created;
static void dialog_gone(struct wl_listener* listener, void* data) {
    (void)data; struct dialog* d = wl_container_of(listener, d, gone); wl_list_remove(&d->gone.link); d->client = NULL;
}
static void client_created(struct wl_listener* listener, void* data) {
    (void)listener; struct wl_client* client = data; pid_t pid = 0; wl_client_get_credentials(client, &pid, NULL, NULL);
    char app[96], action[128]; if (pid <= 0 || !genuine_dialog(pid, app, sizeof(app), action, sizeof(action))) return;
    struct dialog* d = NULL; for (int i = 0; i < DIALOGS && !d; i++) if (!dialogs[i].client) d = &dialogs[i];
    if (!d) return;
    d->client = client; d->pid = pid; snprintf(d->app, sizeof(d->app), "%s", app); snprintf(d->action, sizeof(d->action), "%s", action);
    d->gone.notify = dialog_gone; wl_client_add_destroy_listener(client, &d->gone);
    printf("[64] AURORA SAK dialog connected pid=%d action=%s\n", (int)pid, action); fflush(stdout);
}
void aurora_sak_init(struct wl_display* display) { created.notify = client_created; wl_display_add_client_created_listener(display, &created); }
bool aurora_sak_key(unsigned key, bool pressed, bool super_held) {
    if (key != KEY_ESC) return false;
    if (!pressed) { bool was = held; held = false; return was; }
    if (!super_held) return false;
    held = true;
    int count, chosen; const char* status;
    if (aurora_lock_a11y(&count, &chosen, &status)) {
        aurora_lock_sak("Secure attention key: this is the NexusOS lock screen.");
        puts("[64] AURORA SAK lock-screen genuine=1"); fflush(stdout); return true;
    }
    char app_id[96] = ""; struct wl_client* client = NULL; pid_t pid = aurora_host_focus_pid(app_id, sizeof(app_id), &client);
    const struct dialog* d = NULL; for (int i = 0; i < DIALOGS && !d; i++) if (client && dialogs[i].client == client) d = &dialogs[i];
    if (d) {
        char text[300]; snprintf(text, sizeof(text), "Genuine NexusOS authentication: %s asks for %s.", d->app, d->action);
        aurora_shell_notice(text);
        printf("[64] AURORA SAK genuine=1 pid=%d action=%s app=%s\n", (int)pid, d->action, d->app); fflush(stdout); return true;
    }
    printf("[64] AURORA SAK genuine=0 pid=%d app_id=%s -> lock\n", (int)pid, app_id[0] ? app_id : "-"); fflush(stdout);
    aurora_lock_now("secure-attention");
    aurora_lock_sak(pid > 0 ? "Secure attention key: the window you were using is not a NexusOS dialog. This is the real lock screen."
                            : "Secure attention key: this is the NexusOS lock screen.");
    return true;
}
