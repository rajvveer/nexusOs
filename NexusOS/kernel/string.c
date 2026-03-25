/* ============================================================================
 * NexusOS — String Utilities (Implementation)
 * ============================================================================
 * Custom implementations of common string/memory functions.
 * We can't use libc, so we write everything from scratch.
 * ============================================================================ */

#include "string.h"

/* --------------------------------------------------------------------------
 * strlen: Return length of null-terminated string
 * -------------------------------------------------------------------------- */
size_t strlen(const char* str) {
    size_t len = 0;
    while (str[len] != '\0') {
        len++;
    }
    return len;
}

/* --------------------------------------------------------------------------
 * strcmp: Compare two strings. Returns 0 if equal.
 * -------------------------------------------------------------------------- */
int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

/* --------------------------------------------------------------------------
 * strncmp: Compare up to n characters
 * -------------------------------------------------------------------------- */
int strncmp(const char* s1, const char* s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

/* --------------------------------------------------------------------------
 * strcpy: Copy string src to dest
 * -------------------------------------------------------------------------- */
char* strcpy(char* dest, const char* src) {
    char* d = dest;
    while ((*d++ = *src++) != '\0');
    return dest;
}

/* --------------------------------------------------------------------------
 * strncpy: Copy up to n characters
 * -------------------------------------------------------------------------- */
char* strncpy(char* dest, const char* src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}

/* --------------------------------------------------------------------------
 * strcat: Append src to dest
 * -------------------------------------------------------------------------- */
char* strcat(char* dest, const char* src) {
    char* d = dest;
    while (*d) d++;
    while ((*d++ = *src++) != '\0');
    return dest;
}

/* --------------------------------------------------------------------------
 * memcpy: Copy n bytes from src to dest
 * -------------------------------------------------------------------------- */
void* memcpy(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

/* --------------------------------------------------------------------------
 * memset: Fill n bytes of dest with value val
 * -------------------------------------------------------------------------- */
void* memset(void* dest, int val, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    while (n--) {
        *d++ = (uint8_t)val;
    }
    return dest;
}

/* --------------------------------------------------------------------------
 * memcmp: Compare n bytes of memory
 * -------------------------------------------------------------------------- */
int memcmp(const void* s1, const void* s2, size_t n) {
    const uint8_t* a = (const uint8_t*)s1;
    const uint8_t* b = (const uint8_t*)s2;
    while (n--) {
        if (*a != *b) return *a - *b;
        a++;
        b++;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * int_to_str: Convert integer to decimal string
 * -------------------------------------------------------------------------- */
void int_to_str(int value, char* buffer) {
    char temp[32];
    int i = 0;
    int is_negative = 0;

    if (value == 0) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return;
    }

    if (value < 0) {
        is_negative = 1;
        value = -value;
    }

    while (value > 0) {
        temp[i++] = '0' + (value % 10);
        value /= 10;
    }

    int j = 0;
    if (is_negative) {
        buffer[j++] = '-';
    }
    while (i > 0) {
        buffer[j++] = temp[--i];
    }
    buffer[j] = '\0';
}

/* --------------------------------------------------------------------------
 * hex_to_str: Convert uint32 to hex string (with 0x prefix)
 * -------------------------------------------------------------------------- */
void hex_to_str(uint32_t value, char* buffer) {
    const char hex_chars[] = "0123456789ABCDEF";
    buffer[0] = '0';
    buffer[1] = 'x';
    
    for (int i = 7; i >= 0; i--) {
        buffer[2 + (7 - i)] = hex_chars[(value >> (i * 4)) & 0xF];
    }
    buffer[10] = '\0';
}

/* --------------------------------------------------------------------------
 * int_to_hex: Alias for hex_to_str
 * -------------------------------------------------------------------------- */
void int_to_hex(uint32_t value, char* buffer) {
    hex_to_str(value, buffer);
}

/* --------------------------------------------------------------------------
 * strstr: Find first occurrence of needle in haystack
 * -------------------------------------------------------------------------- */
char* strstr(const char* haystack, const char* needle) {
    if (!*needle) return (char*)haystack;
    for (; *haystack; haystack++) {
        const char* h = haystack;
        const char* n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char*)haystack;
    }
    return NULL;
}
