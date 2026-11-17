/* uACPI libc mapping for the NexusOS kernel (UACPI_OVERRIDE_LIBC): the kernel's string routines
 * (kernel/string.c) plus a memmove from acpi_os.c. snprintf/vsnprintf/strnlen stay uACPI's own. */
#ifndef NEXUS_UACPI_LIBC_H
#define NEXUS_UACPI_LIBC_H
void* memcpy(void* dest, const void* src, __SIZE_TYPE__ n);
void* memset(void* dest, int val, __SIZE_TYPE__ n);
int memcmp(const void* s1, const void* s2, __SIZE_TYPE__ n);
__SIZE_TYPE__ strlen(const char* str);
int strcmp(const char* s1, const char* s2);
void* acpi_os_memmove(void* dest, const void* src, __SIZE_TYPE__ n);
#define uacpi_memcpy memcpy
#define uacpi_memset memset
#define uacpi_memcmp memcmp
#define uacpi_memmove acpi_os_memmove
#define uacpi_strlen strlen
#define uacpi_strcmp strcmp
#endif
