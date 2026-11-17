#ifndef IRQFLAGS_H
#define IRQFLAGS_H
#include "types.h"
static inline uintptr_t irq_save(void) {
    uintptr_t flags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}
static inline void irq_restore(uintptr_t flags) {
    __asm__ volatile("push %0; popf" :: "r"(flags) : "memory", "cc");
}
static inline bool irqs_enabled(void) {
    uintptr_t flags; __asm__ volatile("pushf; pop %0" : "=r"(flags));
    return (flags & 0x200) != 0;
}
#endif
