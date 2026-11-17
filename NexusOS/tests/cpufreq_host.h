/* Host build of arch/x86_64/cpufreq.c (tests/cpufreq_host_test.c): the kernel services it uses, on the C library. */
#ifndef CPUFREQ_HOST_H
#define CPUFREQ_HOST_H
#define NEXUS_HOSTED 1
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "../arch/x86_64/power.h"

static inline void vga_print(const char* s) { fputs(s, stdout); }
static inline void number64(uint64_t value, char* text) { snprintf(text, 24, "%llu", (unsigned long long)value); }
static inline void uint_to_str(uint32_t value, char* text) { snprintf(text, 12, "%u", value); }
int sysfs_add(const char* path, unsigned type, unsigned id, const char* target);   /* recorded by the test */
#endif
