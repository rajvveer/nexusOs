/* ============================================================================
 * NexusOS — Basic Type Definitions
 * ============================================================================
 * Since we can't use stdlib, we define our own fixed-width types.
 * ============================================================================ */

#ifndef TYPES_H
#define TYPES_H

#ifdef NEXUS_HOSTED
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#else

/* Unsigned integers */
typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long long  uint64_t;

/* Signed integers */
typedef signed char         int8_t;
typedef signed short        int16_t;
typedef signed int          int32_t;
typedef signed long long    int64_t;

/* Size type */
typedef __SIZE_TYPE__       size_t;
typedef __UINTPTR_TYPE__    uintptr_t;

/* Boolean */
typedef enum { false = 0, true = 1 } bool;

/* NULL */
#define NULL ((void*)0)

#endif /* NEXUS_HOSTED */

#endif /* TYPES_H */
