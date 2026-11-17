/* Static System V ELF64 fixture using the Linux x86-64 syscall convention. */
typedef unsigned long long u64;
static long call(u64 n, u64 a, u64 b, u64 c) {
    long result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return result;
}
static long call4(u64 n, u64 a, u64 b, u64 c, u64 d) {
    register u64 fourth __asm__("r10") = d; long result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(fourth) : "rcx", "r11", "memory");
    return result;
}
/* reboot(2) from ring 3 ('r' power-off, 'R' restart): argument checks, EPERM inside a new user
 * namespace (forked child), then the real request, which must not return. */
static int reboot_test(int restart) {
    const u64 magic1 = 0xFEE1DEADULL, magic2 = 672274793;
    if (call4(169, 0, magic2, 0x4321FEDC, 0) != -22 || call4(169, magic1, 12345, 0x4321FEDC, 0) != -22) return 40;
    if (call4(169, magic1, magic2, 0x12345678, 0) != -22 || call4(169, magic1, magic2, 0, 0) != 0) return 41;
    long child = call(57, 0, 0, 0);
    if (child == 0) {
        long denied = call(272, 0x10000000, 0, 0) == 0 && call4(169, magic1, magic2, 0x4321FEDC, 0) == -1;
        call(231, denied ? 0 : 1, 0, 0);
    }
    int status = -1;
    if (child < 0 || call4(61, (u64)child, (u64)&status, 0, 0) != child || status != 0) return 42;
    const char message[] = "[64] USER REBOOT PASS: magic/command validation, EPERM in a user namespace; requesting ";
    call(1, 1, (u64)message, sizeof(message) - 1);
    call(1, 1, (u64)(restart ? "restart\n" : "power-off\n"), restart ? 8 : 10);
    call4(169, magic1, magic2, restart ? 0x01234567 : 0x4321FEDC, 0);
    return 43;
}
extern void _start(void);
static volatile unsigned char nx_code[] = {0xC3};
static volatile u64 zero_bss[1024];
#ifdef DISK_PROGRAM
static const volatile unsigned char disk_payload[3 * 1024 * 1024 + 17] = {
    [0] = 0xA7, [4095] = 0x18, [4096] = 0x42, [3 * 1024 * 1024 + 16] = 0xDA
};
#endif
struct utsname { char system[65], node[65], release[65], version[65], machine[65], domain[65]; };
static int equal(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
int user_main(int argc, char** argv) {
    unsigned short cs; __asm__ volatile("mov %%cs,%0" : "=r"(cs));
    if ((cs & 3) != 3 || argc != 2) return 1;
#ifdef DISK_PROGRAM
    if (!equal(argv[0], "/bin/selftest64")) return 1;
    for (unsigned i = 0; i < sizeof(disk_payload); i++) {
        unsigned expected = i == 0 ? 0xA7 : i == 4095 ? 0x18 : i == 4096 ? 0x42 : i == sizeof(disk_payload) - 1 ? 0xDA : 0;
        if (disk_payload[i] != expected) return 15;
    }
#else
    if (!equal(argv[0], "selftest")) return 1;
#endif
    if ((u64)argv <= 0xFFFFFFFF || (u64)_start <= 0xFFFFFFFF) return 2;
    switch (argv[1][0]) {
    case '1': return *(volatile unsigned char*)0xFFFFFFFF80100000ULL; /* kernel image (KERNEL_VMA) */
    case '2': return *(volatile unsigned char*)0;
    case '3': *(volatile unsigned char*)_start = 0; return 3;
    case '4': ((void(*)(void))(u64)nx_code)(); return 4;
    case '5': __asm__ volatile("ud2"); return 5;
    case '6': __asm__ volatile("cli"); return 6;
    case 'r': case 'R': return reboot_test(argv[1][0] == 'R');
    }
    for (unsigned i = 0; i < 1024; i++) if (zero_bss[i]) return 7;
    zero_bss[1023] = 0x123456789ABCDEF0ULL;
    if (zero_bss[1023] != 0x123456789ABCDEF0ULL) return 8;
    if (call(39, 0, 0, 0) != 1 || call(0xFFFF, 0, 0, 0) != -38) return 9;
    struct utsname info;
    if (call(63, (u64)&info, 0, 0) || !equal(info.system, "Linux") || !equal(info.machine, "x86_64")) return 10; /* Linux ABI */
    if (call(63, (u64)_start, 0, 0) != -14) return 11;
    if (call(1, 1, 0x100000, 1) != -14 || call(1, 1, 0x800000000000ULL, 1) != -14 || call(1, 1, 0xFFFFFFFF80100000ULL, 1) != -14) return 12;
    if (call(1, 1, ~0ULL - 16, 64) != -14 || call(1, 9, (u64)argv[0], 1) != -9) return 13;
    const char message[] = "[64] USER PASS: CPL3, argc/argv, BSS, uname, write, syscall errors\n";
    if (call(1, 1, (u64)message, sizeof(message) - 1) != sizeof(message) - 1) return 14;
#ifdef DISK_PROGRAM
    const char path[] = "/etc/native64.dat";
    unsigned char buffer[4099];
    long fd = call4(257, (u64)-100, (u64)path, 0, 0);
    if (fd < 3) return 20;
    u64 stat[18];
    if (call(5, fd, (u64)stat, 0) || stat[6] != 10003) return 21;
    if (call4(262, (u64)-100, (u64)path, (u64)stat, 0)) return 22;
    if (call(0, fd, 0x100000, 1) != -14) return 23;
    if (call(0, fd, (u64)buffer, 4097) != 4097) return 24;
    for (unsigned i = 0; i < 4097; i++) if (buffer[i] != (unsigned char)(i * 37 + 11)) return 25;
    if (call(8, fd, (u64)-3, 2) != 10000 || call(0, fd, (u64)buffer, 8) != 3) return 26;
    for (unsigned i = 0; i < 3; i++) if (buffer[i] != (unsigned char)((i + 10000) * 37 + 11)) return 27;
    if (call(0, fd, (u64)buffer, 1) != 0) return 28;
    if (call4(17, fd, (u64)buffer, 31, 42) != 31 || call(8, fd, 0, 1) != 10003) return 29;
    for (unsigned i = 0; i < 31; i++) if (buffer[i] != (unsigned char)((i + 42) * 37 + 11)) return 30;
    if (call(8, fd, (u64)-1, 0) != -22 || call(3, fd, 0, 0) || call(0, fd, (u64)buffer, 1) != -9) return 31;
    u64 volume[15]; /* FAT32 refuses writes outside the user's areas (EROFS); ext4 enforces the 0644 mode (EACCES) */
    long denied = call(137, (u64)"/", (u64)volume, 0) || volume[0] != 0xEF53 ? -30 : -13;
    if (call(2, (u64)"/missing-file", 0, 0) != -2 || call(2, (u64)path, 2, 0) != denied) return 32;
    const char file_message[] = "[64] FILE API PASS: openat, read, pread, seek, stat, close, errors\n";
    if (call(1, 1, (u64)file_message, sizeof(file_message) - 1) != sizeof(file_message) - 1) return 33;
    const char disk_message[] = "[64] DISK USER PASS: 3 MiB payload, BSS and 64-bit execution\n";
    if (call(1, 1, (u64)disk_message, sizeof(disk_message) - 1) != sizeof(disk_message) - 1) return 16;
#endif
    return 0;
}
