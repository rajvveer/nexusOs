/* ============================================================================
 * NexusOS — user/kernel ABI shared with the userland libc — Phase 57 "apps"
 * ============================================================================
 * This header is SELF-CONTAINED (no kernel includes) because the userland
 * build copies it to userland/libc/include/nexus/abi.h. Everything a C library
 * needs to talk to the kernel through `int 0x80` is defined here:
 *
 *   eax = syscall number, ebx/ecx/edx = arguments, result in eax.
 *   Phase 57 calls (24+) return >= 0 on success and -errno on failure.
 *
 * Numbers 0..23 are the historical NexusOS calls (see syscall.h); the ones a
 * libc uses are SYS_EXIT (5), SYS_GETPID (6), SYS_YIELD (7), SYS_KILL (15),
 * SYS_SETENV (18), SYS_SBRK (23). SYS_WRITE/SYS_READ/SYS_OPEN (0/1/2) are the
 * legacy "print a string / read a line / return a node pointer" calls used by
 * the asm demos; file-descriptor I/O uses the FD_* calls below.
 * ============================================================================ */
#ifndef NEXUS_USYS_ABI_H
#define NEXUS_USYS_ABI_H

/* ---- legacy numbers a libc still uses ---------------------------------- */
#define NX_SYS_EXIT          5
#define NX_SYS_GETPID        6
#define NX_SYS_YIELD         7
#define NX_SYS_KILL          15
#define NX_SYS_SETENV        18
#define NX_SYS_SLEEP         22
#define NX_SYS_SBRK          23

/* ---- Phase 57 POSIX-ish calls -------------------------------------------- */
#define NX_SYS_LSEEK         24   /* (fd, off, whence) -> new pos           */
#define NX_SYS_FSTAT         25   /* (fd, struct nx_stat*)                   */
#define NX_SYS_STAT          26   /* (path, struct nx_stat*)                 */
#define NX_SYS_UNLINK        27   /* (path)                                  */
#define NX_SYS_RENAME        28   /* (old, new)   -EXDEV if not same dir/FS  */
#define NX_SYS_MKDIR         29   /* (path, mode)                            */
#define NX_SYS_RMDIR         30   /* (path)                                  */
#define NX_SYS_GETDENTS      31   /* (fd, struct nx_dirent*, bytes) -> bytes */
#define NX_SYS_CHDIR         32   /* (path)                                  */
#define NX_SYS_GETCWD        33   /* (buf, size) -> len                      */
#define NX_SYS_DUP           34   /* (fd) -> newfd                           */
#define NX_SYS_DUP2          35   /* (fd, newfd) -> newfd                    */
#define NX_SYS_SPAWN         36   /* (path, argv, envp) -> pid               */
#define NX_SYS_WAITPID       37   /* (pid, int* status, options) -> pid      */
#define NX_SYS_CLOCK_GETTIME 38   /* (clk, struct nx_timespec*)              */
#define NX_SYS_NANOSLEEP     39   /* (struct nx_timespec* req, rem)          */
#define NX_SYS_ISATTY        40   /* (fd) -> 1/0                             */
#define NX_SYS_IOCTL         41   /* (fd, req, arg)                          */
#define NX_SYS_UNAME         42   /* (struct nx_utsname*)                    */
#define NX_SYS_ACCESS        43   /* (path, mode)                            */
#define NX_SYS_FTRUNCATE     44   /* (fd, length)                            */
#define NX_SYS_PIPE2         45   /* (int fds[2]) -> 0                       */
#define NX_SYS_FD_OPEN       46   /* (path, flags, mode) -> fd               */
#define NX_SYS_FD_READ       47   /* (fd, buf, n) -> bytes (0 = EOF)         */
#define NX_SYS_FD_WRITE      48   /* (fd, buf, n) -> bytes                   */
#define NX_SYS_FD_CLOSE      49   /* (fd)                                    */
#define NX_SYS_MMAP_ANON     50   /* (length) -> addr  (anonymous, rw)       */
#define NX_SYS_MUNMAP        51   /* (addr, length) -> 0                     */
#define NX_SYS_FCNTL         52   /* (fd, cmd, arg)  F_GETFL/F_SETFL/F_DUPFD */
#define NX_SYS_TTYNAME       53   /* (fd, buf, n) -> 0  ("/dev/tty")         */
#define NX_SYS_GETPPID       54   /* () -> parent pid                        */
#define NX_SYS_COUNT         55

/* ---- open(2) flags (Linux values) ---------------------------------------- */
#define NX_O_RDONLY     0x0000
#define NX_O_WRONLY     0x0001
#define NX_O_RDWR       0x0002
#define NX_O_ACCMODE    0x0003
#define NX_O_CREAT      0x0040
#define NX_O_EXCL       0x0080
#define NX_O_TRUNC      0x0200
#define NX_O_APPEND     0x0400
#define NX_O_NONBLOCK   0x0800
#define NX_O_DIRECTORY  0x10000

#define NX_SEEK_SET 0
#define NX_SEEK_CUR 1
#define NX_SEEK_END 2

/* fcntl */
#define NX_F_DUPFD 0
#define NX_F_GETFD 1
#define NX_F_SETFD 2
#define NX_F_GETFL 3
#define NX_F_SETFL 4

/* access */
#define NX_F_OK 0
#define NX_X_OK 1
#define NX_W_OK 2
#define NX_R_OK 4

/* waitpid */
#define NX_WNOHANG 1

/* clocks */
#define NX_CLOCK_REALTIME  0
#define NX_CLOCK_MONOTONIC 1

/* ---- errno values (Linux numbering) -------------------------------------- */
#define NX_EPERM        1
#define NX_ENOENT       2
#define NX_ESRCH        3
#define NX_EINTR        4
#define NX_EIO          5
#define NX_EBADF        9
#define NX_ECHILD      10
#define NX_EAGAIN      11
#define NX_ENOMEM      12
#define NX_EACCES      13
#define NX_EFAULT      14
#define NX_EBUSY       16
#define NX_EEXIST      17
#define NX_EXDEV       18
#define NX_ENODEV      19
#define NX_ENOTDIR     20
#define NX_EISDIR      21
#define NX_EINVAL      22
#define NX_ENFILE      23
#define NX_EMFILE      24
#define NX_ENOTTY      25
#define NX_EFBIG       27
#define NX_ENOSPC      28
#define NX_ESPIPE      29
#define NX_EROFS       30
#define NX_EPIPE       32
#define NX_ERANGE      34
#define NX_ENAMETOOLONG 36
#define NX_ENOSYS      38
#define NX_ENOTEMPTY   39
#define NX_ENOEXEC      8
#define NX_E2BIG        7

/* ---- stat --------------------------------------------------------------- */
#define NX_S_IFMT   0170000
#define NX_S_IFREG  0100000
#define NX_S_IFDIR  0040000
#define NX_S_IFCHR  0020000
#define NX_S_IFIFO  0010000

struct nx_stat {
    unsigned int st_mode;      /* type | permission bits                     */
    unsigned int st_size;
    unsigned int st_mtime;     /* seconds since 1970 (FAT32) or 0 (ramfs)     */
    unsigned int st_ino;
    unsigned int st_nlink;
    unsigned int st_uid;
    unsigned int st_dev;       /* 1 = ramfs, 2 = FAT32 (/disk), 3 = other    */
    unsigned int st_blksize;
};

/* ---- getdents records (fixed 48 bytes) ----------------------------------- */
#define NX_DT_UNKNOWN 0
#define NX_DT_FIFO    1
#define NX_DT_CHR     2
#define NX_DT_DIR     4
#define NX_DT_REG     8
struct nx_dirent {
    unsigned int   d_ino;
    unsigned int   d_size;
    unsigned short d_type;
    char           d_name[38];
};

/* ---- time ----------------------------------------------------------------- */
struct nx_timespec {
    unsigned int tv_sec;
    unsigned int tv_nsec;
};

/* ---- termios-lite (Linux bit values so ported code compiles unchanged) --- */
#define NX_TCGETS     0x5401
#define NX_TCSETS     0x5402
#define NX_TCSETSW    0x5403
#define NX_TCSETSF    0x5404
#define NX_TIOCGWINSZ 0x5413

/* c_iflag */
#define NX_BRKINT  0x0002
#define NX_ICRNL   0x0100
#define NX_INPCK   0x0010
#define NX_ISTRIP  0x0020
#define NX_IXON    0x0400
/* c_oflag */
#define NX_OPOST   0x0001
#define NX_ONLCR   0x0004
/* c_cflag */
#define NX_CS8     0x0030
/* c_lflag */
#define NX_ISIG    0x0001
#define NX_ICANON  0x0002
#define NX_ECHO    0x0008
#define NX_ECHOE   0x0010
#define NX_ECHOK   0x0020
#define NX_ECHONL  0x0040
#define NX_IEXTEN  0x8000
/* c_cc indices */
#define NX_VINTR   0
#define NX_VEOF    4
#define NX_VTIME   5
#define NX_VMIN    6
#define NX_NCCS    16

struct nx_termios {
    unsigned int  c_iflag;
    unsigned int  c_oflag;
    unsigned int  c_cflag;
    unsigned int  c_lflag;
    unsigned char c_cc[NX_NCCS];
};

struct nx_winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

/* ---- uname ---------------------------------------------------------------- */
struct nx_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
};

/* ---- process entry ----------------------------------------------------------
 * At _start the user stack holds: [esp] = argc, [esp+4] = argv (char**),
 * [esp+8] = envp (char**). argv/envp are NULL-terminated arrays whose strings
 * also live on the stack. The stack is UPROC_USTACK_PAGES * 4 KiB.          */
#define NX_USTACK_PAGES 64
#define NX_MMAP_BASE    0x20000000u

/* ---- signals (only the numbers; delivery is "kill the process") ---------- */
#define NX_SIGINT  2
#define NX_SIGKILL 9
#define NX_SIGSEGV 11
#define NX_SIGTERM 15

#endif /* NEXUS_USYS_ABI_H */
