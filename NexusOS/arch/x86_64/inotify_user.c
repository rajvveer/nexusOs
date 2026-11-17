/* inotify(7) checks on the FAT root (run as /bin/inotify64 by the apps campaign, mutating /home/nexus). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) { dprintf(2, "[64] INOTIFY FAIL line=%d errno=%d\n", __LINE__, errno); _exit(99); } } while (0)
#define DIR_PATH "/home/nexus/inotify-test"
struct seen { int wd; uint32_t mask, cookie; char name[64]; };
static struct seen events[64]; static int count;
static int collect(int fd) {
    char buffer[4096] __attribute__((aligned(8))); count = 0;
    ssize_t got = read(fd, buffer, sizeof(buffer));
    if (got < 0) return errno == EAGAIN ? 0 : -1;
    for (char* p = buffer; p < buffer + got && count < 64;) {
        struct inotify_event* e = (void*)p;
        events[count] = (struct seen){e->wd, e->mask, e->cookie, ""};
        if (e->len) snprintf(events[count].name, sizeof(events[count].name), "%s", e->name);
        count++; p += sizeof(*e) + e->len;
    }
    return count;
}
static int find(int wd, uint32_t mask, const char* name) {
    for (int i = 0; i < count; i++)
        if (events[i].wd == wd && events[i].mask == mask && !strcmp(events[i].name, name ? name : "")) return i;
    return -1;
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (getuid() == 0) { CHECK(setresgid(1000, 1000, 1000) == 0 && setresuid(1000, 1000, 1000) == 0); }
    mkdir(DIR_PATH, 0700);
    unlink(DIR_PATH "/a"); unlink(DIR_PATH "/b"); unlink(DIR_PATH "/c"); rmdir(DIR_PATH "/sub");
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC); CHECK(fd >= 0);
    CHECK(fcntl(fd, F_GETFD) == FD_CLOEXEC && (fcntl(fd, F_GETFL) & O_NONBLOCK));
    CHECK(inotify_init1(0x1234) == -1 && errno == EINVAL);
    CHECK(inotify_add_watch(fd, DIR_PATH, 0) == -1 && errno == EINVAL);
    CHECK(inotify_add_watch(fd, "/home/nexus/missing-dir", IN_ALL_EVENTS) == -1 && errno == ENOENT);
    CHECK(inotify_add_watch(1, DIR_PATH, IN_ALL_EVENTS) == -1 && errno == EINVAL);
    int dir = inotify_add_watch(fd, DIR_PATH, IN_ALL_EVENTS); CHECK(dir > 0);
    CHECK(inotify_add_watch(fd, DIR_PATH, IN_CREATE | IN_MASK_CREATE) == -1 && errno == EEXIST);
    CHECK(inotify_add_watch(fd, DIR_PATH, IN_ALL_EVENTS) == dir);
    CHECK(read(fd, events, sizeof(events)) == -1 && errno == EAGAIN);
    struct pollfd item = {fd, POLLIN, 0}; CHECK(poll(&item, 1, 0) == 0);
    /* create, write, close */
    int file = open(DIR_PATH "/a", O_CREAT | O_WRONLY, 0600); CHECK(file >= 0);
    CHECK(write(file, "data", 4) == 4 && close(file) == 0);
    CHECK(poll(&item, 1, 0) == 1 && (item.revents & POLLIN));
    int queued; CHECK(ioctl(fd, FIONREAD, &queued) == 0 && queued >= 4 * 32);
    CHECK(collect(fd) == 4);
    CHECK(find(dir, IN_CREATE, "a") == 0 && find(dir, IN_OPEN, "a") == 1 && find(dir, IN_MODIFY, "a") == 2 && find(dir, IN_CLOSE_WRITE, "a") == 3);
    /* reads and identical-event merging */
    file = open(DIR_PATH "/a", O_RDONLY); char data[8]; CHECK(file >= 0 && read(file, data, 2) == 2 && read(file, data, 2) == 2 && close(file) == 0);
    CHECK(collect(fd) == 3 && find(dir, IN_OPEN, "a") == 0 && find(dir, IN_ACCESS, "a") == 1 && find(dir, IN_CLOSE_NOWRITE, "a") == 2);
    /* watch on the file itself: modify, attrib, then delete-self and ignored */
    int self = inotify_add_watch(fd, DIR_PATH "/a", IN_MODIFY | IN_ATTRIB | IN_DELETE_SELF); CHECK(self > 0 && self != dir);
    CHECK(inotify_add_watch(fd, DIR_PATH "/a", IN_MODIFY | IN_ONLYDIR) == -1 && errno == ENOTDIR);
    CHECK(truncate(DIR_PATH "/a", 1) == 0);
    CHECK(chmod(DIR_PATH "/a", 0700) == 0);
    collect(fd);
    CHECK(find(self, IN_MODIFY, NULL) >= 0 && find(dir, IN_MODIFY, "a") >= 0 && find(self, IN_ATTRIB, NULL) >= 0 && find(dir, IN_ATTRIB, "a") >= 0);
    /* rename pairs share a cookie; the moved inode reports move-self */
    int moved = inotify_add_watch(fd, DIR_PATH "/a", IN_MOVE_SELF | IN_MASK_ADD); CHECK(moved == self);
    CHECK(rename(DIR_PATH "/a", DIR_PATH "/b") == 0);
    CHECK(collect(fd) == 3);
    int from = find(dir, IN_MOVED_FROM, "a"), to = find(dir, IN_MOVED_TO, "b");
    CHECK(from >= 0 && to >= 0 && events[from].cookie && events[from].cookie == events[to].cookie && find(self, IN_MOVE_SELF, NULL) >= 0);
    /* unlink: parent delete, then delete-self and ignored for the file watch */
    CHECK(unlink(DIR_PATH "/b") == 0);
    collect(fd);
    CHECK(find(dir, IN_DELETE, "b") >= 0 && find(self, IN_DELETE_SELF, NULL) >= 0 && find(self, IN_IGNORED, NULL) >= 0);
    CHECK(inotify_rm_watch(fd, self) == -1 && errno == EINVAL);
    /* directories carry IN_ISDIR */
    CHECK(mkdir(DIR_PATH "/sub", 0700) == 0 && rmdir(DIR_PATH "/sub") == 0);
    collect(fd);
    CHECK(find(dir, IN_CREATE | IN_ISDIR, "sub") >= 0 && find(dir, IN_DELETE | IN_ISDIR, "sub") >= 0);
    /* symlinks and one-shot watches */
    CHECK(symlink("target", DIR_PATH "/c") == 0);
    collect(fd); CHECK(find(dir, IN_CREATE, "c") >= 0);
    int oneshot = inotify_add_watch(fd, "/home/nexus", IN_CREATE | IN_ONESHOT); CHECK(oneshot > 0);
    CHECK(mkdir("/home/nexus/inotify-once", 0700) == 0 && rmdir("/home/nexus/inotify-once") == 0);
    collect(fd); CHECK(find(oneshot, IN_CREATE | IN_ISDIR, "inotify-once") >= 0 && find(oneshot, IN_IGNORED, NULL) >= 0);
    CHECK(unlink(DIR_PATH "/c") == 0); collect(fd);
    /* a too-small buffer is rejected; a blocked reader is woken */
    CHECK(close(open(DIR_PATH "/a", O_CREAT | O_WRONLY, 0600)) == 0);
    char tiny[8]; CHECK(read(fd, tiny, sizeof(tiny)) == -1 && errno == EINVAL);
    collect(fd);
    int blocking = inotify_init(); CHECK(blocking >= 0);
    CHECK(inotify_add_watch(blocking, DIR_PATH, IN_DELETE) > 0);
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) { usleep(200000); _exit(unlink(DIR_PATH "/a") ? 1 : 0); }
    CHECK(collect(blocking) == 1 && events[0].mask == IN_DELETE && !strcmp(events[0].name, "a"));
    int status; CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    /* removing a watch queues IN_IGNORED; closing releases everything */
    collect(fd);
    CHECK(inotify_rm_watch(fd, dir) == 0 && collect(fd) == 1 && events[0].mask == IN_IGNORED && events[0].wd == dir);
    CHECK(close(blocking) == 0 && close(fd) == 0 && rmdir(DIR_PATH) == 0);
    puts("[64] INOTIFY ALL PASSED: create/open/modify/close, access merge, attrib, rename cookies, move/delete-self, "
         "IN_ISDIR, one-shot, mask flags, FIONREAD/poll, buffer sizing, blocking wakeup and watch removal");
    return 0;
}
