#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

/* Bootstrap the official protocol generator on our kernel, not a hosted OS. */
static int generate(const char* mode, const char* xml, const char* output) {
    pid_t child = fork(); if (child < 0) { perror("protocol fork"); return 1; }
    if (!child) {
        execl("/bin/wayland-scanner", "wayland-scanner", mode, xml, output, NULL);
        perror("exec wayland-scanner"); _exit(1);
    }
    int status;
    if (waitpid(child, &status, 0) != child || status) { fprintf(stderr, "protocol generator failed: %s %s\n", mode, xml); return 1; }
    printf("[64] PROTOCOL GENERATED %s\n", output); fflush(stdout);
    return 0;
}
int main(void) {
    const char* modes[] = {"server-header", "client-header", "private-code"};
    /* /etc/protocols.list (desktop.py prepare_protocols): "<name> <xml path>" lines; the bindings are written to
     * /home/nexus/<name>-{server-protocol.h,client-protocol.h,protocol.c}. Without it, the xdg-shell bootstrap runs. */
    FILE* list = fopen("/etc/protocols.list", "r");
    if (list) {
        const char* suffix[] = {"server-protocol.h", "client-protocol.h", "protocol.c"};
        char name[96], xml[256], path[384]; unsigned count = 0;
        while (fscanf(list, "%95s %255s", name, xml) == 2) {
            for (unsigned i = 0; i < 3; i++) {
                snprintf(path, sizeof(path), "/home/nexus/%s-%s", name, suffix[i]);
                if (generate(modes[i], xml, path)) { fclose(list); return 1; }
            }
            count++;
        }
        fclose(list);
        if (!count) { fputs("empty protocol list\n", stderr); return 1; }
        printf("[64] PROTOCOL LIST GENERATED %u\n", count);
        puts("[64] PROTOCOL GENERATION PASSED"); return 0;
    }
    const char* paths[] = {"/home/nexus/xdg-shell-server-protocol.h", "/home/nexus/xdg-shell-client-protocol.h", "/home/nexus/xdg-shell-protocol.c"};
    for (unsigned i = 0; i < 3; i++) if (generate(modes[i], "/etc/xdg-shell.xml", paths[i])) return 1;
    puts("[64] PROTOCOL GENERATION PASSED"); return 0;
}
