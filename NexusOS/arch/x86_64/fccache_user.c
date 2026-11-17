/* desk_install.py fccache: init of a one-off boot that builds the system fontconfig cache. fc-cache writes its caches
 * to the first writable <cachedir>: a configuration that names /home/nexus/.fccache-build first and then includes the
 * system's /etc/fonts/fonts.conf (on a FAT root /var/cache is not writable). The host copies the files it lists to
 * /var/cache/fontconfig of the image afterwards; the font directories are not touched, so the caches stay valid. */
#define _GNU_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    mkdir("/home/nexus/.fccache-build", 0755);
    FILE* conf = fopen("/home/nexus/.fccache-build/fonts.conf", "w");
    if (!conf) { perror("fccache conf"); return 1; }
    fputs("<?xml version=\"1.0\"?>\n<!DOCTYPE fontconfig SYSTEM \"urn:fontconfig:fonts.dtd\">\n<fontconfig>\n"
          "  <cachedir>/home/nexus/.fccache-build</cachedir>\n  <include ignore_missing=\"no\">/etc/fonts/fonts.conf</include>\n</fontconfig>\n", conf);
    fclose(conf);
    pid_t child = fork();
    if (!child) {
        setenv("FONTCONFIG_FILE", "/home/nexus/.fccache-build/fonts.conf", 1); setenv("HOME", "/home/nexus", 1);
        execl("/usr/bin/fc-cache", "fc-cache", "-f", "-v", (char*)NULL); perror("fc-cache"); _exit(127);
    }
    int status = 0; waitpid(child, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) { fprintf(stderr, "[64] FCCACHE fc-cache failed status=%d\n", status); return 1; }
    DIR* dir = opendir("/home/nexus/.fccache-build"); struct dirent* e; unsigned n = 0;
    while (dir && (e = readdir(dir))) if (strstr(e->d_name, ".cache-") || !strcmp(e->d_name, "CACHEDIR.TAG")) { printf("[64] FCCACHE FILE %s\n", e->d_name); n++; }
    if (dir) closedir(dir);
    printf("[64] FCCACHE DONE files=%u\n", n); fflush(stdout);
    return n ? 0 : 1;
}
