/* Local APIC register access in xAPIC (MMIO at 0xFEE00000) or x2APIC (MSRs 0x800 + offset/16) mode (apic.c).
 * irq.c, smp.c and pm.c reach the local APIC only through these. Register numbers are the xAPIC MMIO offsets. */
#ifndef NEXUS_APIC_H
#define NEXUS_APIC_H
#include "native.h"

extern bool apic_x2;                      /* every CPU runs its local APIC in x2APIC mode */
/* BSP, first call from irq_init: choose the mode (x2APIC when the CPU has it and the firmware handed over in x2APIC
 * mode or the kernel command line says "x2apic"; "nox2apic" forces xAPIC) and enter it. False: no usable local APIC. */
bool apic_select(void);
/* Any CPU (APs at start-up, the BSP after S3/hibernation): enter the chosen mode from whatever state the firmware or
 * INIT left. False: no usable local APIC. */
bool apic_enter_mode(void);
uint32_t apic_read(unsigned reg);
void apic_write(unsigned reg, uint32_t value);
void apic_icr(uint32_t destination, uint32_t command);   /* destination: APIC id (xAPIC: 8 bits, x2APIC: 32 bits) */
uint32_t apic_id_self(void);
#endif
