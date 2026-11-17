/* Hardware facts (hwinfo.c): fault-tolerant MSR reads, MTRR decoding, display report. */
#ifndef NEXUS_HWINFO_H
#define NEXUS_HWINFO_H
#include "../../kernel/types.h"

enum { MEMTYPE_UC = 0, MEMTYPE_WC = 1, MEMTYPE_WT = 4, MEMTYPE_WP = 5, MEMTYPE_WB = 6, MEMTYPE_CONFLICT = -2, MEMTYPE_UNKNOWN = -1 };
#define HW_MTRR_MAX 32
struct hw_mtrr { bool present; unsigned count; uint64_t cap, def_type, physical_mask, base[HW_MTRR_MAX], mask[HW_MTRR_MAX]; };

int hw_rdmsr_safe(uint32_t msr, uint64_t* value);      /* 0, or -5 when the MSR faults (#GP fixed up by trap64) */
void hw_cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4]);
bool hw_mtrr_read(struct hw_mtrr* m);
int hw_mtrr_type(const struct hw_mtrr* m, uint64_t physical);
const char* hw_memtype_name(int type);
const uint8_t* hw_acpi_table(const char* signature, unsigned nth, uint64_t* physical);   /* NULL signature: any */
uint64_t hw_tsc_calibrate(bool pm_first, const char** source);   /* HPET / ACPI PM timer; 0 if neither works */
void hwinfo_display_report(uint64_t physical, uint32_t width, uint32_t height, uint32_t stride, uint32_t format, bool write_combining);
#endif
