/* Small process-based shell and everyday file tools for NexusOS. */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <nexus.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int copy_file(const char *src, const char *dst) {
    int in = open(src, O_RDONLY);
    if (in < 0) { perror(src); return 1; }
    int out = open(dst, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (out < 0) { perror(dst); close(in); return 1; }
    char b[4096]; ssize_t n; int failed = 0;
    while ((n = read(in, b, sizeof(b))) > 0) {
        ssize_t off = 0;
        while (off < n) { ssize_t w = write(out, b + off, n - off); if (w <= 0) { failed = 1; break; } off += w; }
        if (failed) break;
    }
    if (n < 0) failed = 1;
    close(in); if (close(out)) failed = 1;
    if (failed) { perror(dst); unlink(dst); }
    return failed;
}
static int cmp_line(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }
static int filter(const char *name, FILE *in, const char *arg, int limit) {
    char *line = NULL; size_t cap = 0; ssize_t n; unsigned long lines = 0, words = 0, bytes = 0;
    char **all = NULL; size_t used = 0;
    bool inword = false; char *previous = NULL;
    while ((n = getline(&line, &cap, in)) >= 0) {
        lines += n && line[n - 1] == '\n'; bytes += n;
        for (int i = 0; i < n; i++) { bool w = !isspace((unsigned char)line[i]); if (w && !inword) words++; inword = w; }
        if (!strcmp(name, "grep")) { if (strstr(line, arg)) fputs(line, stdout); }
        else if (!strcmp(name, "head")) { if (limit-- > 0) fputs(line, stdout); else break; }
        else if (!strcmp(name, "uniq")) {
            if (!previous || strcmp(previous, line)) fputs(line, stdout);
            free(previous); previous = strdup(line); if (!previous) goto fail;
        } else if (!strcmp(name, "sort") || !strcmp(name, "tail")) {
            char **next = realloc(all, (used + 1) * sizeof(*all)); if (!next) goto fail;
            all = next; all[used] = strdup(line); if (!all[used]) goto fail; used++;
        }
    }
    if (!strcmp(name, "wc")) printf("%lu %lu %lu\n", lines, words, bytes);
    if (!strcmp(name, "sort")) qsort(all, used, sizeof(*all), cmp_line);
    size_t start = !strcmp(name, "tail") && used > (size_t)limit ? used - limit : 0;
    for (size_t i = start; i < used; i++) fputs(all[i], stdout);
    for (size_t i = 0; i < used; i++) free(all[i]);
    free(all); free(previous); free(line);
    return ferror(in) || ferror(stdout);
fail:
    for (size_t i = 0; i < used; i++) free(all[i]);
    free(all); free(previous); free(line); perror("filter"); return 1;
}

enum { WORD, PIPE, INPUT, OUTPUT, APPEND, SEMI };
typedef struct { char *s; int kind; } token;
typedef struct { char *argv[32]; int argc; char *input, *output; bool append; } command;
static bool exiting;

static int tokenize(const char *line, char *store, token *tokens) {
    int n = 0, used = 0;
    while (*line) {
        while (*line == ' ' || *line == '\t' || *line == '\n') line++;
        if (!*line) break;
        if (n >= 127) return -1;
        token *t = &tokens[n++]; t->s = store + used; t->kind = WORD;
        if (*line == '|' || *line == '<' || *line == '>' || *line == ';') {
            char c = *line++;
            t->kind = c == '|' ? PIPE : c == '<' ? INPUT : c == ';' ? SEMI : OUTPUT;
            if (c == '>' && *line == '>') { t->kind = APPEND; line++; }
        } else {
            char quote = 0;
            while (*line && (quote || !strchr(" \t\n|<>;", *line))) {
                char c = *line++;
                if ((c == '\'' || c == '"') && (!quote || quote == c)) { quote = quote ? 0 : c; continue; }
                if (c == '\\' && quote != '\'' && *line) c = *line++;
                if (used >= 2046) return -1;
                store[used++] = c;
            }
            if (quote) return -1;
        }
        if (used >= 2047) return -1;
        store[used++] = 0;
    }
    return n;
}
static int pipeline(token *tok, int n) {
    command cmds[8] = {0}; int count = 1;
    for (int i = 0; i < n; i++) {
        command *c = &cmds[count - 1];
        if (tok[i].kind == PIPE) { if (!c->argc || count == 8) goto syntax; count++; }
        else if (tok[i].kind == WORD) { if (c->argc == 31) goto syntax; c->argv[c->argc++] = tok[i].s; }
        else {
            int kind = tok[i].kind;
            if (++i == n || tok[i].kind != WORD) goto syntax;
            if (kind == INPUT) c->input = tok[i].s;
            else { c->output = tok[i].s; c->append = kind == APPEND; }
        }
    }
    if (!cmds[count - 1].argc) goto syntax;
    if (count == 1 && !strcmp(cmds[0].argv[0], "cd")) {
        const char *path = cmds[0].argc > 1 ? cmds[0].argv[1] : getenv("HOME");
        if (chdir(path ? path : "/")) { perror("cd"); return 1; } return 0;
    }
    if (count == 1 && !strcmp(cmds[0].argv[0], "exit")) { exiting = true; return cmds[0].argc > 1 ? atoi(cmds[0].argv[1]) : 0; }
    fflush(stdout); fflush(stderr);
    int saved[2] = {dup(0), dup(1)}, prev = -1, pids[8], started = 0, status = 0, failed = 0;
    if (saved[0] < 0 || saved[1] < 0) { if (saved[0] >= 0) close(saved[0]); if (saved[1] >= 0) close(saved[1]); perror("dup"); return 1; }
    fcntl(saved[0], F_SETFD, FD_CLOEXEC); fcntl(saved[1], F_SETFD, FD_CLOEXEC);
    for (int i = 0; i < count; i++) {
        command *c = &cmds[i]; int pipes[2] = {-1, -1}, in = -1, out = -1;
        if (i + 1 < count && pipe(pipes)) { failed = 1; break; }
        for (int k = 0; k < 2; k++) if (pipes[k] >= 0) fcntl(pipes[k], F_SETFD, FD_CLOEXEC);
        if (c->input) in = open(c->input, O_RDONLY);
        if (c->output) out = open(c->output, O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC), 0666);
        if ((c->input && in < 0) || (c->output && out < 0)) failed = 1;
        if (in >= 0) fcntl(in, F_SETFD, FD_CLOEXEC);
        if (out >= 0) fcntl(out, F_SETFD, FD_CLOEXEC);
        if (!failed && (dup2(in >= 0 ? in : prev >= 0 ? prev : saved[0], 0) < 0 ||
                        dup2(out >= 0 ? out : pipes[1] >= 0 ? pipes[1] : saved[1], 1) < 0)) failed = 1;
        int pid = failed ? -1 : spawn(c->argv[0], c->argv, NULL);
        dup2(saved[0], 0); dup2(saved[1], 1);
        if (in >= 0) close(in);
        if (out >= 0) close(out);
        if (prev >= 0) close(prev);
        if (pipes[1] >= 0) close(pipes[1]);
        prev = pipes[0];
        if (pid < 0) { perror(c->argv[0]); failed = 1; break; }
        pids[started++] = pid;
    }
    if (prev >= 0) close(prev);
    close(saved[0]); close(saved[1]);
    for (int i = 0; i < started; i++) if (waitpid(pids[i], &status, 0) < 0) failed = 1;
    return failed ? 1 : WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
syntax:
    fputs("sh: invalid or oversized command\n", stderr); return 2;
}
static int shell_line(const char *line) {
    char store[2048]; token tokens[128]; int n = tokenize(line, store, tokens), start = 0, rc = 0;
    if (n < 0) { fputs("sh: unmatched quote or command too long\n", stderr); return 2; }
    for (int i = 0; i <= n && !exiting; i++) if (i == n || tokens[i].kind == SEMI) {
        if (i > start) rc = pipeline(tokens + start, i - start);
        start = i + 1;
    }
    return rc;
}
int main(int argc, char **argv) {
    const char *name = strrchr(argv[0], '/'); name = name ? name + 1 : argv[0];
    if (!strcmp(name, "core") || !strcmp(name, "sh")) {
        if (argc > 2 && !strcmp(argv[1], "-c")) return shell_line(argv[2]);
        char line[1024], cwd[160]; int rc = 0;
        if (isatty(0)) puts("Nexus sh: quotes, pipes, < > >>, cd, exit");
        while (!exiting) {
            if (isatty(0)) { printf("%s $ ", getcwd(cwd, sizeof(cwd)) ? cwd : "?"); fflush(stdout); }
            if (!fgets(line, sizeof(line), stdin)) break;
            rc = shell_line(line);
        }
        return rc;
    }
    if (!strcmp(name, "echo")) { for (int i = 1; i < argc; i++) printf("%s%s", i > 1 ? " " : "", argv[i]); putchar('\n'); return 0; }
    if (!strcmp(name, "pwd")) { char cwd[160]; if (!getcwd(cwd, sizeof(cwd))) return 1; puts(cwd); return 0; }
    if (!strcmp(name, "ls")) {
        const char *path = argc > 1 ? argv[1] : ".";
        DIR *d = opendir(path); if (!d) { perror(path); return 1; }
        struct dirent *e; while ((e = readdir(d))) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) printf("%s%s\n", e->d_name, e->d_type == DT_DIR ? "/" : "");
        closedir(d); return 0;
    }
    if (!strcmp(name, "cp") || !strcmp(name, "mv")) {
        if (argc != 3) { fprintf(stderr, "Usage: %s source destination\n", name); return 2; }
        if (!strcmp(name, "mv") && rename(argv[1], argv[2]) == 0) return 0;
        if (copy_file(argv[1], argv[2])) return 1;
        return !strcmp(name, "mv") && unlink(argv[1]) < 0;
    }
    if (!strcmp(name, "rm") || !strcmp(name, "mkdir")) {
        if (argc < 2) return 2;
        int rc = 0;
        for (int i = 1; i < argc; i++) if ((!strcmp(name, "rm") ? unlink(argv[i]) : mkdir(argv[i], 0755)) < 0) { perror(argv[i]); rc = 1; }
        return rc;
    }
    if (!strcmp(name, "tr")) {
        if (argc != 3 || !argv[2][0]) return 2;
        int c; size_t n = strlen(argv[2]);
        while ((c = getchar()) != EOF) { const char *p = strchr(argv[1], c); if (p && c) { size_t k = p - argv[1]; c = argv[2][k < n ? k : n - 1]; } putchar(c); }
        return ferror(stdin) || ferror(stdout);
    }
    int first = 1, limit = 10; const char *pattern = "";
    if (!strcmp(name, "grep")) { if (argc < 2) return 2; pattern = argv[first++]; }
    if ((!strcmp(name, "head") || !strcmp(name, "tail")) && argc > 2 && !strcmp(argv[1], "-n")) {
        char *end; long value = strtol(argv[2], &end, 10); if (*end || value < 0 || value > 1000000) return 2;
        limit = value; first = 3;
    }
    int rc = 0;
    do {
        FILE *in = first < argc ? fopen(argv[first++], "r") : stdin;
        if (!in) { perror(argv[first - 1]); rc = 1; continue; }
        if (!strcmp(name, "cat") || !strcmp(name, "hexdump")) {
            unsigned char b[4096]; size_t n; unsigned long off = 0;
            while ((n = fread(b, 1, !strcmp(name, "hexdump") ? 16 : sizeof(b), in))) {
                if (!strcmp(name, "cat")) { if (fwrite(b, 1, n, stdout) != n) { rc = 1; break; } }
                else { printf("%08lx  ", off); for (size_t i = 0; i < n; i++) printf("%02x ", b[i]); putchar('\n'); off += n; }
            }
            rc |= ferror(in);
        } else rc |= filter(name, in, pattern, limit);
        if (in != stdin) fclose(in);
    } while (first < argc);
    return rc || ferror(stdout);
}
