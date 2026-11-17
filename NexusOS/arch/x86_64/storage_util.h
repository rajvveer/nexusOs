/* Small string/number helpers shared by the storage files (kernel/string.h has no strncat/strchr/strtoul). */
#ifndef NEXUS_STORAGE_UTIL_H
#define NEXUS_STORAGE_UTIL_H
#include "../../kernel/types.h"
#include "../../kernel/string.h"

/* Storage objects (hot-plugged disks, mounts, loop/dm devices, FUSE connections) outlive the process that made
 * them: their pages are counted (blockdev.c) so the kernel's per-process allocation audit (kernel.c run_image)
 * leaves them out, as it does for disk caches and network buffers. Every storage file allocates through these. */
uint64_t storage_alloc(uint64_t bytes);
uint64_t storage_dma32_alloc(uint64_t bytes);
void storage_free(uint64_t address, uint64_t bytes);
uint64_t storage_background_pages(void);
#ifndef STORAGE_ALLOCATOR
#define arena_alloc storage_alloc
#define dma32_alloc storage_dma32_alloc
#define arena_free storage_free
#endif

static inline char* s_strncat(char* dest, const char* src, size_t n) {
    size_t length = strlen(dest), i = 0;
    for (; i < n && src[i]; i++) dest[length + i] = src[i];
    dest[length + i] = 0;
    return dest;
}
static inline char* s_strchr(const char* s, int c) {
    for (;; s++) { if (*s == (char)c) return (char*)s; if (!*s) return NULL; }
}
static inline char* s_strrchr(const char* s, int c) {
    const char* last = NULL;
    for (;; s++) { if (*s == (char)c) last = s; if (!*s) return (char*)last; }
}
/* Decimal or 0x-hex unsigned number; *end set past the digits; false on no digits or overflow. */
static inline bool parse_u64(const char* s, uint64_t* out, const char** end) {
    uint64_t v = 0; unsigned base = 10, digits = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
    for (;; s++) {
        unsigned d; char c = *s;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (base == 16 && c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else if (base == 16 && c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else break;
        if (v > (~0ULL - d) / base) return false;
        v = v * base + d; digits++;
    }
    if (end) *end = s;
    *out = v;
    return digits != 0;
}
static inline void append_u64(char* text, uint64_t value) {
    char reverse[24]; unsigned n = 0;
    do { reverse[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    size_t length = strlen(text);
    while (n) text[length++] = reverse[--n];
    text[length] = 0;
}
static inline void append_hex(char* text, uint64_t value, unsigned digits) {
    size_t length = strlen(text);
    for (unsigned i = 0; i < digits; i++) text[length + i] = "0123456789abcdef"[(value >> (4 * (digits - 1 - i))) & 15];
    text[length + digits] = 0;
}
static inline uint16_t get_le16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t get_le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint64_t get_le64(const uint8_t* p) { return get_le32(p) | (uint64_t)get_le32(p + 4) << 32; }
static inline void put_le16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void put_le32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static inline void put_le64(uint8_t* p, uint64_t v) { put_le32(p, (uint32_t)v); put_le32(p + 4, (uint32_t)(v >> 32)); }
static inline uint16_t get_be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t get_be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline uint64_t get_be64(const uint8_t* p) { return (uint64_t)get_be32(p) << 32 | get_be32(p + 4); }
static inline void put_be16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void put_be32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static inline void put_be64(uint8_t* p, uint64_t v) { put_be32(p, (uint32_t)(v >> 32)); put_be32(p + 4, (uint32_t)v); }
#endif
