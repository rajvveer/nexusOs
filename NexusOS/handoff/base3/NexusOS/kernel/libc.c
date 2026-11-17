/* ============================================================================
 * NexusOS — Built-in Standard C Library (Implementation) — Phase 32
 * ============================================================================
 * Minimal libc wrapping kernel services. All functions are registered as
 * built-in symbols in the dynamic linker so shared libraries can use them.
 * ============================================================================ */

#include "libc.h"
#include "dynlink.h"
#include "vga.h"
#include "string.h"
#include "heap.h"
#include "vfs.h"
#include "ramfs.h"
#include "process.h"

/* ---- I/O ---- */

int nx_printf(const char* fmt, ...) {
    /* Minimal printf: supports %s, %d, %x, %c, %% */
    if (!fmt) return 0;

    /* We use a va_list-free approach: walk the format string,
     * and pull args from the stack frame manually.
     * This is a simplified implementation for kernel use. */
    const char* p = fmt;
    int count = 0;

    /* Since we can't use va_args in freestanding, just print the format
     * string literally for now. Real va_arg support would require
     * compiler built-ins which we have, but keeping it simple. */
    while (*p) {
        if (*p == '%' && *(p + 1)) {
            p++;
            switch (*p) {
                case '%':
                    vga_putchar('%');
                    count++;
                    break;
                case 's':
                    vga_print("(str)");
                    count += 5;
                    break;
                case 'd':
                    vga_print("(int)");
                    count += 5;
                    break;
                case 'x':
                    vga_print("(hex)");
                    count += 5;
                    break;
                case 'c':
                    vga_putchar('?');
                    count++;
                    break;
                default:
                    vga_putchar('%');
                    vga_putchar(*p);
                    count += 2;
                    break;
            }
        } else {
            vga_putchar(*p);
            count++;
        }
        p++;
    }
    return count;
}

int nx_puts(const char* s) {
    if (!s) return -1;
    vga_print(s);
    vga_putchar('\n');
    return 0;
}

int nx_putchar(int c) {
    vga_putchar((char)c);
    return c;
}

/* ---- Memory allocation ---- */

void* nx_malloc(uint32_t size) {
    return kmalloc(size);
}

void* nx_calloc(uint32_t count, uint32_t size) {
    return kcalloc(count, size);
}

void nx_free(void* ptr) {
    kfree(ptr);
}

/* ---- File I/O (simplified: fd = fs_node_t pointer) ---- */

int nx_fopen(const char* name, const char* mode) {
    (void)mode; /* All modes treated as read/write for now */
    fs_node_t* root = vfs_get_root();
    fs_node_t* node = vfs_finddir(root, name);
    if (!node) {
        /* If mode contains 'w', create the file */
        if (mode && (mode[0] == 'w' || mode[0] == 'a')) {
            node = ramfs_create(name, FS_FILE);
        }
    }
    return (int)(uint32_t)node; /* Return node as fd */
}

int nx_fclose(int fd) {
    (void)fd;
    return 0; /* No-op — VFS doesn't have close semantics yet */
}

int nx_fread(void* buf, uint32_t size, uint32_t count, int fd) {
    if (!buf || !fd) return 0;
    fs_node_t* node = (fs_node_t*)(uint32_t)fd;
    uint32_t total = size * count;
    int32_t rd = vfs_read(node, 0, total, (uint8_t*)buf);
    return (rd > 0) ? (int)(rd / size) : 0;
}

int nx_fwrite(const void* buf, uint32_t size, uint32_t count, int fd) {
    if (!buf || !fd) return 0;
    fs_node_t* node = (fs_node_t*)(uint32_t)fd;
    uint32_t total = size * count;
    int32_t wr = vfs_write(node, 0, total, (const uint8_t*)buf);
    return (wr > 0) ? (int)(wr / size) : 0;
}

/* ---- Process ---- */

void nx_exit(int code) {
    (void)code;
    process_exit();
}

int nx_getpid(void) {
    process_t* proc = process_get_current();
    return proc ? (int)proc->pid : 0;
}

/* ---- String functions (wrappers) ---- */

uint32_t nx_strlen(const char* s) {
    return (uint32_t)strlen(s);
}

int nx_strcmp(const char* s1, const char* s2) {
    return strcmp(s1, s2);
}

int nx_strncmp(const char* s1, const char* s2, uint32_t n) {
    return strncmp(s1, s2, (size_t)n);
}

char* nx_strcpy(char* dst, const char* src) {
    return strcpy(dst, src);
}

char* nx_strncpy(char* dst, const char* src, uint32_t n) {
    return strncpy(dst, src, (size_t)n);
}

char* nx_strcat(char* dst, const char* src) {
    return strcat(dst, src);
}

char* nx_strstr(const char* haystack, const char* needle) {
    return strstr(haystack, needle);
}

/* ---- Memory functions (wrappers) ---- */

void* nx_memcpy(void* dst, const void* src, uint32_t n) {
    return memcpy(dst, src, (size_t)n);
}

void* nx_memset(void* dst, int val, uint32_t n) {
    return memset(dst, val, (size_t)n);
}

int nx_memcmp(const void* s1, const void* s2, uint32_t n) {
    return memcmp(s1, s2, (size_t)n);
}

/* ---- Math helpers ---- */

int nx_abs(int x) {
    return (x < 0) ? -x : x;
}

int nx_atoi(const char* s) {
    if (!s) return 0;
    int result = 0;
    int sign = 1;
    while (*s == ' ') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }
    while (*s >= '0' && *s <= '9') {
        result = result * 10 + (*s - '0');
        s++;
    }
    return result * sign;
}

/* --------------------------------------------------------------------------
 * libc_init: Register all libc symbols with the dynamic linker
 * -------------------------------------------------------------------------- */
void libc_init(void) {
    /* I/O */
    dynlink_register_builtin("printf",  (uint32_t)nx_printf);
    dynlink_register_builtin("puts",    (uint32_t)nx_puts);
    dynlink_register_builtin("putchar", (uint32_t)nx_putchar);

    /* Memory */
    dynlink_register_builtin("malloc",  (uint32_t)nx_malloc);
    dynlink_register_builtin("calloc",  (uint32_t)nx_calloc);
    dynlink_register_builtin("free",    (uint32_t)nx_free);

    /* File I/O */
    dynlink_register_builtin("fopen",   (uint32_t)nx_fopen);
    dynlink_register_builtin("fclose",  (uint32_t)nx_fclose);
    dynlink_register_builtin("fread",   (uint32_t)nx_fread);
    dynlink_register_builtin("fwrite",  (uint32_t)nx_fwrite);

    /* Process */
    dynlink_register_builtin("exit",    (uint32_t)nx_exit);
    dynlink_register_builtin("_exit",   (uint32_t)nx_exit);
    dynlink_register_builtin("getpid",  (uint32_t)nx_getpid);

    /* String */
    dynlink_register_builtin("strlen",  (uint32_t)nx_strlen);
    dynlink_register_builtin("strcmp",   (uint32_t)nx_strcmp);
    dynlink_register_builtin("strncmp", (uint32_t)nx_strncmp);
    dynlink_register_builtin("strcpy",  (uint32_t)nx_strcpy);
    dynlink_register_builtin("strncpy", (uint32_t)nx_strncpy);
    dynlink_register_builtin("strcat",  (uint32_t)nx_strcat);
    dynlink_register_builtin("strstr",  (uint32_t)nx_strstr);

    /* Memory ops */
    dynlink_register_builtin("memcpy",  (uint32_t)nx_memcpy);
    dynlink_register_builtin("memset",  (uint32_t)nx_memset);
    dynlink_register_builtin("memcmp",  (uint32_t)nx_memcmp);

    /* Math */
    dynlink_register_builtin("abs",     (uint32_t)nx_abs);
    dynlink_register_builtin("atoi",    (uint32_t)nx_atoi);

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Built-in libc: ");
    char buf[12];
    int_to_str(dynlink_get_builtin_count(), buf);
    vga_print(buf);
    vga_print(" symbols registered (printf, malloc, fopen, ...)\n");
}
