/* In-guest half of the application install (arch/x86_64/apps_postinst.py): regenerate the caches that
 * Debian maintainer triggers build. System paths are read-only to applications, so results are written
 * below /home/nexus/.nexus-postinst and the host installs them into place afterwards. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define OUT "/home/nexus/.nexus-postinst"
static int failures;
static int run(const char* output, char* const argv[], char* const env[]) {
    pid_t child = fork();
    if (child < 0) { perror("fork"); return -1; }
    if (!child) {
        if (output) {
            int fd = open(output, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0 || dup2(fd, 1) < 0) _exit(126);
        }
        execve(argv[0], argv, env); _exit(127);
    }
    int status = 0; waitpid(child, &status, 0);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    printf("[64] POSTINST %s status=%d\n", argv[0], code); fflush(stdout);
    if (code) failures++;
    return code;
}
static int copy_tree(const char* from, const char* to) {
    if (mkdir(to, 0700) && errno != EEXIST) return -1;
    DIR* d = opendir(from); if (!d) return -1;
    struct dirent* e; char a[1024], b[1024], buffer[65536];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(a, sizeof(a), "%s/%s", from, e->d_name); snprintf(b, sizeof(b), "%s/%s", to, e->d_name);
        struct stat st; if (stat(a, &st)) continue;
        if (S_ISDIR(st.st_mode)) { copy_tree(a, b); continue; }
        int in = open(a, O_RDONLY), out = open(b, O_WRONLY | O_CREAT | O_TRUNC, 0644); ssize_t n;
        while (in >= 0 && out >= 0 && (n = read(in, buffer, sizeof(buffer))) > 0) if (write(out, buffer, (size_t)n) != n) break;
        if (in >= 0) close(in);
        if (out >= 0) close(out);
    }
    closedir(d); return 0;
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    char* env[] = {"HOME=/home/nexus", "LANG=C.UTF-8", "PATH=/usr/bin:/bin", "XDG_CACHE_HOME=" OUT "/xdg-cache", NULL};
    mkdir(OUT, 0700); mkdir(OUT "/xdg-cache", 0700);
    run(NULL, (char*[]){"/usr/lib/x86_64-linux-gnu/glib-2.0/glib-compile-schemas", "--targetdir=" OUT, "/usr/share/glib-2.0/schemas", NULL}, env);
    run(OUT "/loaders.cache", (char*[]){"/usr/lib/x86_64-linux-gnu/gdk-pixbuf-2.0/gdk-pixbuf-query-loaders", NULL}, env);
    if (copy_tree("/usr/share/mime", OUT "/mime")) { puts("[64] POSTINST mime copy failed"); failures++; }
    else run(NULL, (char*[]){"/usr/bin/update-mime-database", OUT "/mime", NULL}, env);
    run(NULL, (char*[]){"/usr/bin/fc-cache", "-f", NULL}, env);
    sync();
    printf("[64] APPS POSTINST DONE failures=%d\n", failures);
    return failures ? 1 : 0;
}
