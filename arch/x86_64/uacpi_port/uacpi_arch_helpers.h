/* uACPI architecture helpers for NexusOS x86-64 (UACPI_OVERRIDE_ARCH_HELPERS).
 * Thread ids are opaque pointers (acpi_os.c: the running task, the ACPI worker, or the CPU when no task runs);
 * interrupt state is RFLAGS. Sleep entry below S4 writes back and invalidates the caches (ACPI 7.4.2). */
#ifndef NEXUS_UACPI_ARCH_HELPERS_H
#define NEXUS_UACPI_ARCH_HELPERS_H
#include <uacpi/platform/atomic.h>

#define UACPI_ARCH_FLUSH_CPU_CACHE() __asm__ volatile("wbinvd" ::: "memory")

typedef unsigned long uacpi_cpu_flags;
typedef unsigned long uacpi_interrupt_state;
typedef void* uacpi_thread_id;

#define UACPI_ATOMIC_LOAD_THREAD_ID(ptr) ((uacpi_thread_id)uacpi_atomic_load_ptr(ptr))
#define UACPI_ATOMIC_STORE_THREAD_ID(ptr, value) uacpi_atomic_store_ptr(ptr, value)
#define UACPI_THREAD_ID_NONE ((uacpi_thread_id)-1)
#endif
