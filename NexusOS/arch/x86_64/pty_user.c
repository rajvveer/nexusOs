/* Pseudo-terminal, controlling terminal and job-control checks (run as /bin/pty64 by the apps campaign). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifdef PTY_TRACE
#define TRACE() dprintf(2, "L%d ", __LINE__)
#else
#define TRACE() (void)0
#endif
#define CHECK(test) do { TRACE(); if (!(test)) { dprintf(2, "[64] PTY FAIL line=%d errno=%d\n", __LINE__, errno); _exit(99); } } while (0)
#define CHILD(test) do { TRACE(); if (!(test)) { dprintf(2, "[64] PTY CHILD FAIL line=%d errno=%d\n", __LINE__, errno); _exit(98); } } while (0)
static volatile sig_atomic_t signals[65];
static void handler(int number) { signals[number]++; }
static void catch(int number, int flags) {
    struct sigaction action = {.sa_handler = handler, .sa_flags = flags}; sigemptyset(&action.sa_mask);
    sigaction(number, &action, NULL);
}
static int drain(int master, char* text, size_t size) {
    /* Collect everything the slave side produced so far (non-blocking master). */
    size_t used = 0; struct pollfd item = {master, POLLIN, 0};
    while (used + 1 < size && poll(&item, 1, 200) == 1 && (item.revents & POLLIN)) {
        ssize_t got = read(master, text + used, size - used - 1);
        if (got <= 0) break;
        used += (size_t)got;
    }
    text[used] = 0; return (int)used;
}
static int status_of(pid_t pid, int options) { int status = -1; return waitpid(pid, &status, options) == pid ? status : -1; }
static void pair(int* master, int* slave) {
    *master = posix_openpt(O_RDWR | O_NOCTTY); CHECK(*master >= 0);
    char* name = ptsname(*master); CHECK(name && !strncmp(name, "/dev/pts/", 9));
    CHECK(open(name, O_RDWR | O_NOCTTY) == -1 && errno == EIO);          /* locked until unlockpt */
    CHECK(grantpt(*master) == 0 && unlockpt(*master) == 0);
    *slave = open(name, O_RDWR | O_NOCTTY); CHECK(*slave >= 0);
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    char text[4096]; int master, slave, number;
    /* ---- allocation, naming, nodes ---- */
    pair(&master, &slave);
    CHECK(ioctl(master, TIOCGPTN, &number) == 0);
    char expected[32]; snprintf(expected, sizeof(expected), "/dev/pts/%d", number);
    CHECK(!strcmp(ptsname(master), expected) && !strcmp(ttyname(slave), expected));
    struct stat node, opened;
    CHECK(stat(expected, &node) == 0 && S_ISCHR(node.st_mode) && major(node.st_rdev) == 136 && minor(node.st_rdev) == (unsigned)number);
    CHECK(fstat(slave, &opened) == 0 && opened.st_ino == node.st_ino && opened.st_dev == node.st_dev && opened.st_rdev == node.st_rdev);
    CHECK(stat("/dev/ptmx", &node) == 0 && S_ISCHR(node.st_mode) && node.st_rdev == makedev(5, 2));
    CHECK(fstat(master, &opened) == 0 && opened.st_rdev == makedev(5, 2));
    CHECK(isatty(slave) && isatty(master) && !isatty(open("/dev/null", O_RDONLY)));
    CHECK(ioctl(slave, TIOCGPTN, &number) == -1 && errno == ENOTTY);
    CHECK(open("/dev/pts/999", O_RDWR) == -1 && errno == ENOENT);
    int peer = ioctl(master, TIOCGPTPEER, O_RDWR | O_NOCTTY); CHECK(peer >= 0);
    CHECK(fstat(peer, &opened) == 0 && opened.st_rdev == makedev(136, number) && close(peer) == 0);
    CHECK(fcntl(master, F_SETFL, O_NONBLOCK) == 0);
    CHECK(read(master, text, 1) == -1 && errno == EAGAIN);
    /* ---- termios defaults and canonical editing with echo ---- */
    struct termios mode; CHECK(tcgetattr(slave, &mode) == 0);
    CHECK((mode.c_lflag & (ICANON | ECHO | ISIG | IEXTEN)) == (ICANON | ECHO | ISIG | IEXTEN) && (mode.c_iflag & ICRNL) && (mode.c_oflag & (OPOST | ONLCR)) == (OPOST | ONLCR));
    CHECK(mode.c_cc[VERASE] == 0177 && mode.c_cc[VINTR] == 3 && mode.c_cc[VEOF] == 4 && mode.c_cc[VMIN] == 1);
    CHECK(fcntl(slave, F_SETFL, O_NONBLOCK) == 0);
    CHECK(write(master, "hello", 5) == 5 && read(slave, text, sizeof(text)) == -1 && errno == EAGAIN);  /* no line yet */
    struct pollfd item = {slave, POLLIN, 0}; CHECK(poll(&item, 1, 0) == 0);
    fd_set readable, writable; FD_ZERO(&readable); FD_SET(slave, &readable); FD_ZERO(&writable); FD_SET(slave, &writable);
    struct timeval wait = {0, 100000};
    CHECK(select(slave + 1, &readable, &writable, NULL, &wait) == 1 && !FD_ISSET(slave, &readable) && FD_ISSET(slave, &writable));
    FD_ZERO(&readable); FD_SET(slave, &readable); wait = (struct timeval){0, 50000};
    CHECK(select(slave + 1, &readable, NULL, NULL, &wait) == 0 && !FD_ISSET(slave, &readable));
    FD_ZERO(&readable); FD_SET(999, &readable); CHECK(select(1000, &readable, NULL, NULL, &wait) == -1 && errno == EBADF);
    CHECK(write(master, "\r", 1) == 1 && poll(&item, 1, 1000) == 1 && (item.revents & POLLIN));
    sigset_t none; sigemptyset(&none); struct timespec patience = {1, 0};
    FD_ZERO(&readable); FD_SET(slave, &readable);
    CHECK(pselect(slave + 1, &readable, NULL, NULL, &patience, &none) == 1 && FD_ISSET(slave, &readable));
    CHECK(ioctl(slave, FIONREAD, &number) == 0 && number == 6);
    CHECK(read(slave, text, sizeof(text)) == 6 && !memcmp(text, "hello\n", 6));               /* ICRNL */
    CHECK(drain(master, text, sizeof(text)) == 7 && !strcmp(text, "hello\r\n"));               /* echo + ONLCR */
    CHECK(write(master, "abc\177d\n", 6) == 6 && read(slave, text, sizeof(text)) == 4 && !memcmp(text, "abd\n", 4));
    CHECK(drain(master, text, sizeof(text)) > 0 && !strcmp(text, "abc\b \bd\r\n"));
    CHECK(write(master, "one two\027\025ok\n", 12) == 12 && read(slave, text, sizeof(text)) == 3 && !memcmp(text, "ok\n", 3));
    drain(master, text, sizeof(text));
    CHECK(write(master, "x\026\025y\n", 5) == 5 && read(slave, text, sizeof(text)) == 4 && !memcmp(text, "x\025y\n", 4)); /* VLNEXT */
    drain(master, text, sizeof(text));
    CHECK(write(master, "tail\004", 5) == 5 && read(slave, text, sizeof(text)) == 4 && !memcmp(text, "tail", 4));    /* VEOF ends a line */
    CHECK(write(master, "\004", 1) == 1 && read(slave, text, sizeof(text)) == 0);                                    /* alone: end of file */
    CHECK(write(master, "abcdef\n", 7) == 7 && read(slave, text, 2) == 2 && read(slave, text + 2, 10) == 5 && !memcmp(text, "abcdef\n", 7));
    drain(master, text, sizeof(text));
    /* ---- output processing ---- */
    CHECK(write(slave, "out\n", 4) == 4 && drain(master, text, sizeof(text)) == 5 && !strcmp(text, "out\r\n"));
    mode.c_oflag &= ~OPOST; CHECK(tcsetattr(slave, TCSANOW, &mode) == 0);
    CHECK(write(slave, "raw\n", 4) == 4 && drain(master, text, sizeof(text)) == 4 && !strcmp(text, "raw\n"));
    /* ---- raw input, VMIN/VTIME ---- */
    cfmakeraw(&mode); mode.c_cc[VMIN] = 0; mode.c_cc[VTIME] = 2;
    CHECK(tcsetattr(slave, TCSAFLUSH, &mode) == 0 && fcntl(slave, F_SETFL, 0) == 0);
    struct timespec before, after; clock_gettime(CLOCK_MONOTONIC, &before);
    CHECK(read(slave, text, 10) == 0); clock_gettime(CLOCK_MONOTONIC, &after);
    long elapsed = (after.tv_sec - before.tv_sec) * 1000 + (after.tv_nsec - before.tv_nsec) / 1000000;
    CHECK(elapsed >= 150 && elapsed < 2000);
    CHECK(write(master, "\003\r\177", 3) == 3 && read(slave, text, 10) == 3 && !memcmp(text, "\003\r\177", 3)); /* no ISIG/ICRNL */
    CHECK(drain(master, text, sizeof(text)) == 0);                                            /* no echo */
    mode.c_cc[VMIN] = 3; mode.c_cc[VTIME] = 0; CHECK(tcsetattr(slave, TCSANOW, &mode) == 0);
    pid_t writer = fork(); CHECK(writer >= 0);
    if (!writer) { usleep(100000); write(master, "1", 1); usleep(100000); write(master, "23", 2); _exit(0); }
    CHECK(read(slave, text, 10) == 3 && !memcmp(text, "123", 3) && status_of(writer, 0) == 0);
    /* ---- window size and packet mode ---- */
    struct winsize size = {40, 120, 0, 0}, seen;
    CHECK(ioctl(master, TIOCSWINSZ, &size) == 0 && ioctl(slave, TIOCGWINSZ, &seen) == 0 && seen.ws_row == 40 && seen.ws_col == 120);
    int on = 1; CHECK(ioctl(master, TIOCPKT, &on) == 0);
    CHECK(write(slave, "pk", 2) == 2 && drain(master, text, sizeof(text)) == 3 && text[0] == 0 && !memcmp(text + 1, "pk", 2));
    CHECK(tcflush(slave, TCIFLUSH) == 0 && read(master, text, 10) == 1 && (text[0] & TIOCPKT_FLUSHREAD));
    on = 0; CHECK(ioctl(master, TIOCPKT, &on) == 0);
    /* ---- hangup: master close ---- */
    close(master);
    CHECK(read(slave, text, 1) == 0 && write(slave, "x", 1) == -1 && errno == EIO);
    item = (struct pollfd){slave, POLLIN, 0}; CHECK(poll(&item, 1, 0) == 1 && (item.revents & POLLHUP));
    CHECK(stat(expected, &node) == -1 && errno == ENOENT);
    close(slave);
    /* ---- slave close seen by the master: EIO after the data ---- */
    pair(&master, &slave);
    CHECK(write(slave, "bye\n", 4) == 4 && close(slave) == 0);
    CHECK(read(master, text, 10) == 5 && !memcmp(text, "bye\r\n", 5) && read(master, text, 10) == -1 && errno == EIO);
    item = (struct pollfd){master, POLLIN, 0}; CHECK(poll(&item, 1, 0) == 1 && (item.revents & POLLHUP));
    close(master);
    /* ---- controlling terminal, signals and job control in a new session ---- */
    pair(&master, &slave); close(slave);
    char name[64]; strcpy(name, ptsname(master));
    int report[2]; CHECK(pipe(report) == 0);
    pid_t leader = fork(); CHECK(leader >= 0);
    if (!leader) {
        close(report[0]); close(master); /* the session must not keep the master side open */
        CHILD(setsid() == getpid());
        CHILD(open("/dev/tty", O_RDWR) == -1 && errno == ENXIO);
        int tty = open(name, O_RDWR); CHILD(tty >= 0);                                         /* acquires the terminal */
        CHILD(tcgetpgrp(tty) == getpid() && tcgetsid(tty) == getpid());
        int again = open("/dev/tty", O_RDWR); CHILD(again >= 0);
        struct stat a, b; CHILD(fstat(again, &a) == 0 && fstat(tty, &b) == 0 && a.st_rdev == b.st_rdev && close(again) == 0);
        catch(SIGINT, 0); catch(SIGWINCH, SA_RESTART); catch(SIGCHLD, SA_RESTART);
        write(report[1], "R", 1);
        char line[64];
        CHILD(read(tty, line, sizeof(line)) == -1 && errno == EINTR && signals[SIGINT] == 1);   /* ^C from the master */
        CHILD(signals[SIGWINCH] == 1);
        /* background job: reading the terminal stops it with SIGTTIN */
        pid_t job = fork(); CHILD(job >= 0);
        if (!job) {
            setpgid(0, 0); signal(SIGINT, SIG_DFL);
            char data[16]; ssize_t got = read(tty, data, sizeof(data));
            _exit(got == 4 && !memcmp(data, "job\n", 4) ? 7 : 8);
        }
        setpgid(job, job);
        int status = status_of(job, WUNTRACED); CHILD(WIFSTOPPED(status) && WSTOPSIG(status) == SIGTTIN);
        signal(SIGTTOU, SIG_IGN); CHILD(tcsetpgrp(tty, job) == 0 && tcgetpgrp(tty) == job);
        CHILD(kill(job, SIGCONT) == 0);
        status = status_of(job, WCONTINUED); CHILD(WIFCONTINUED(status));
        write(report[1], "J", 1);                                                              /* parent types "job\n" */
        status = status_of(job, 0); CHILD(WIFEXITED(status) && WEXITSTATUS(status) == 7);
        CHILD(tcsetpgrp(tty, getpid()) == 0);
        /* foreground job stopped by ^Z, resumed by SIGCONT */
        job = fork(); CHILD(job >= 0);
        if (!job) { setpgid(0, 0); signal(SIGTSTP, SIG_DFL); write(report[1], "Z", 1); pause(); _exit(0); }
        setpgid(job, job); CHILD(tcsetpgrp(tty, job) == 0);
        status = status_of(job, WUNTRACED); CHILD(WIFSTOPPED(status) && WSTOPSIG(status) == SIGTSTP);
        CHILD(signals[SIGCHLD] >= 1);
        CHILD(kill(job, SIGTERM) == 0 && kill(job, SIGCONT) == 0);
        status = status_of(job, 0); CHILD(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
        CHILD(tcsetpgrp(tty, getpid()) == 0);
        /* waitid sees a stop as CLD_STOPPED and a continue as CLD_CONTINUED */
        job = fork(); CHILD(job >= 0);
        if (!job) { for (;;) pause(); }
        siginfo_t info; CHILD(kill(job, SIGSTOP) == 0 && waitid(P_PID, job, &info, WSTOPPED) == 0 && info.si_code == CLD_STOPPED && info.si_status == SIGSTOP);
        CHILD(kill(job, SIGCONT) == 0 && waitid(P_PID, job, &info, WCONTINUED) == 0 && info.si_code == CLD_CONTINUED);
        CHILD(kill(job, SIGKILL) == 0 && waitid(P_PID, job, &info, WEXITED) == 0 && info.si_code == CLD_KILLED && info.si_status == SIGKILL);
        /* TOSTOP: a background writer gets SIGTTOU */
        struct termios settings; CHILD(tcgetattr(tty, &settings) == 0); settings.c_lflag |= TOSTOP; CHILD(tcsetattr(tty, TCSANOW, &settings) == 0);
        job = fork(); CHILD(job >= 0);
        if (!job) { setpgid(0, 0); signal(SIGTTOU, SIG_DFL); write(tty, "bg", 2); _exit(0); }
        setpgid(job, job);
        status = status_of(job, WUNTRACED); CHILD(WIFSTOPPED(status) && WSTOPSIG(status) == SIGTTOU);
        CHILD(kill(job, SIGKILL) == 0 && WIFSIGNALED(status_of(job, 0)));
        write(report[1], "H", 1);
        catch(SIGHUP, 0);
        pause();                                                                                /* SIGHUP when the master closes */
        CHILD(signals[SIGHUP] == 1 && read(tty, line, 1) == 0);
        CHILD(open("/dev/tty", O_RDWR) == -1);
        _exit(0);
    }
    close(report[1]);
    CHECK(read(report[0], text, 1) == 1 && text[0] == 'R');
    usleep(300000);
    size.ws_col = 100; CHECK(ioctl(master, TIOCSWINSZ, &size) == 0);
    usleep(300000);
    CHECK(write(master, "\003", 1) == 1);
    CHECK(read(report[0], text, 1) == 1 && text[0] == 'J');
    usleep(100000); CHECK(write(master, "job\n", 4) == 4);
    CHECK(read(report[0], text, 1) == 1 && text[0] == 'Z');
    usleep(200000); CHECK(write(master, "\032", 1) == 1);
    CHECK(read(report[0], text, 1) == 1 && text[0] == 'H');
    usleep(200000); close(master);
    int status = status_of(leader, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    /* ---- resource usage of the waited-for children ---- */
    struct rusage usage; CHECK(getrusage(RUSAGE_CHILDREN, &usage) == 0 && usage.ru_nvcsw > 0);
    CHECK(getrusage(RUSAGE_SELF, &usage) == 0 && usage.ru_maxrss > 0 && usage.ru_minflt > 0);
    CHECK(getrusage(RUSAGE_THREAD, &usage) == 0 && getrusage(7, &usage) == -1 && errno == EINVAL);
    struct tms ticks; CHECK(times(&ticks) != (clock_t)-1);
    pid_t spinner = fork(); CHECK(spinner >= 0);
    if (!spinner) { volatile unsigned long x = 0; for (unsigned long i = 0; i < 20000000UL; i++) x += i; _exit(0); }
    CHECK(wait4(spinner, &status, 0, &usage) == spinner && WIFEXITED(status) && (usage.ru_utime.tv_sec || usage.ru_utime.tv_usec));
    puts("[64] PTY ALL PASSED: ptmx/pts naming and locking, termios canonical editing/echo/VEOF/VLNEXT, raw VMIN/VTIME, "
         "output processing, window size, packet mode, hangup, controlling terminal, SIGINT/SIGWINCH/SIGTTIN/SIGTSTP/SIGTTOU/SIGHUP, "
         "WUNTRACED/WCONTINUED/waitid job reports, getrusage/wait4/times");
    return 0;
}
