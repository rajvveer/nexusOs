/* ============================================================================
 * NexusOS — String Utilities (Header)
 * ============================================================================ */

#ifndef STRING_H
#define STRING_H

#include "types.h"

/* String functions */
size_t strlen(const char* str);
int strcmp(const char* s1, const char* s2);
int strncmp(const char* s1, const char* s2, size_t n);
char* strcpy(char* dest, const char* src);
char* strncpy(char* dest, const char* src, size_t n);
char* strcat(char* dest, const char* src);
char* strstr(const char* haystack, const char* needle);

/* Memory functions */
void* memcpy(void* dest, const void* src, size_t n);
void* memset(void* dest, int val, size_t n);
int memcmp(const void* s1, const void* s2, size_t n);

/* Extra utilities */
void int_to_str(int value, char* buffer);
void hex_to_str(uint32_t value, char* buffer);
void int_to_hex(uint32_t value, char* buffer);

#endif /* STRING_H */
