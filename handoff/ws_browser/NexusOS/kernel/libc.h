/* ============================================================================
 * NexusOS — Built-in Standard C Library (Header) — Phase 32
 * ============================================================================
 * Provides a minimal libc for dynamically-linked programs.
 * Functions are registered as built-in symbols so .so files can link to them.
 * ============================================================================ */

#ifndef LIBC_H
#define LIBC_H

#include "types.h"

/* Initialize the built-in libc (registers symbols with dynamic linker) */
void libc_init(void);

/* ---- I/O functions ---- */
int   nx_printf(const char* fmt, ...);
int   nx_puts(const char* s);
int   nx_putchar(int c);

/* ---- Memory allocation ---- */
void* nx_malloc(uint32_t size);
void* nx_calloc(uint32_t count, uint32_t size);
void  nx_free(void* ptr);

/* ---- File I/O ---- */
int   nx_fopen(const char* name, const char* mode);
int   nx_fclose(int fd);
int   nx_fread(void* buf, uint32_t size, uint32_t count, int fd);
int   nx_fwrite(const void* buf, uint32_t size, uint32_t count, int fd);

/* ---- Process ---- */
void  nx_exit(int code);
int   nx_getpid(void);

/* ---- String functions ---- */
uint32_t nx_strlen(const char* s);
int   nx_strcmp(const char* s1, const char* s2);
int   nx_strncmp(const char* s1, const char* s2, uint32_t n);
char* nx_strcpy(char* dst, const char* src);
char* nx_strncpy(char* dst, const char* src, uint32_t n);
char* nx_strcat(char* dst, const char* src);
char* nx_strstr(const char* haystack, const char* needle);

/* ---- Memory functions ---- */
void* nx_memcpy(void* dst, const void* src, uint32_t n);
void* nx_memset(void* dst, int val, uint32_t n);
int   nx_memcmp(const void* s1, const void* s2, uint32_t n);

/* ---- Math helpers ---- */
int   nx_abs(int x);
int   nx_atoi(const char* s);

#endif /* LIBC_H */
