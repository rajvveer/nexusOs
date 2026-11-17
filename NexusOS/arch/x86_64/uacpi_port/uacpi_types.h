/* uACPI platform types for the NexusOS kernel (UACPI_OVERRIDE_TYPES).
 * The kernel has no <stdint.h>/<stdbool.h>: its own kernel/types.h defines the fixed-width types, size_t,
 * NULL and an enum `bool` (used by a few uACPI locals). uacpi_bool is a real _Bool so that every value is
 * normalised to 0/1 (uACPI assigns masked 64-bit flags to it). */
#ifndef NEXUS_UACPI_TYPES_H
#define NEXUS_UACPI_TYPES_H
#include "../../../kernel/types.h"
#include <uacpi/helpers.h>

typedef uint8_t uacpi_u8;
typedef uint16_t uacpi_u16;
typedef uint32_t uacpi_u32;
typedef uint64_t uacpi_u64;
typedef int8_t uacpi_i8;
typedef int16_t uacpi_i16;
typedef int32_t uacpi_i32;
typedef int64_t uacpi_i64;

#define UACPI_TRUE 1
#define UACPI_FALSE 0
typedef _Bool uacpi_bool;
#define UACPI_NULL ((void*)0)

typedef uintptr_t uacpi_uintptr;
typedef uacpi_uintptr uacpi_virt_addr;
typedef size_t uacpi_size;

typedef __builtin_va_list uacpi_va_list;
#define uacpi_va_start __builtin_va_start
#define uacpi_va_end __builtin_va_end
#define uacpi_va_arg __builtin_va_arg

typedef char uacpi_char;
#define uacpi_offsetof __builtin_offsetof

#define UACPI_PRIu64 "llu"
#define UACPI_PRIx64 "llx"
#define UACPI_PRIX64 "llX"
#define UACPI_FMT64(val) ((unsigned long long)(val))
#endif
