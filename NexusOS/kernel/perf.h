/* ============================================================================
 * NexusOS — Performance / Profiling (Header) — Phase 48
 * ============================================================================
 * Era 6 finale. Honest, measurable performance work for a single-CPU QEMU
 * i386 target:
 *
 *   - Boot-time measurement: capture the tick at which automated init finished
 *     (start -> "all systems go", before the interactive login), so we can
 *     report how long the kernel takes to come up. PIT is 18.2 Hz (≈55 ms/tick),
 *     so this is ±55 ms resolution — accurate enough to confirm a sub-second
 *     boot and to catch regressions.
 *
 *   - A consolidated `perf` benchmark: times the rep-string memcpy/memset
 *     (Phase 48 optimized them), a function-call / loop baseline, and the
 *     cooperative-yield path, reporting throughput in MB/s and ops/s using the
 *     existing tick-boundary timing trick.
 *
 *   - Honest status for the two big roadmap items that are NOT real on this
 *     target: SMP (single CPU here) and kernel preemption (the scheduler exists
 *     but is intentionally left cooperative — enabling it risks the known boot
 *     double-fault). `perf` / `smp` report what a real implementation needs
 *     rather than faking it.
 * ============================================================================ */

#ifndef PERF_H
#define PERF_H

#include "types.h"

/* Record that automated boot/init has finished (call once, right after the
 * last _init in kernel_main, before login). Stamps the boot tick count. */
void perf_mark_boot_done(void);

/* Ticks the kernel took to initialize (0 if not yet marked). */
uint32_t perf_boot_ticks(void);

/* Run the Phase 48 micro-benchmarks and print a report (memcpy/memset MB/s,
 * loop ops/s, yield/s, plus boot time and the SMP/preemption status). */
void perf_run(void);

/* Print just the SMP / multi-core honest status. */
void perf_smp_status(void);

/* Print just the preemption / scheduler honest status. */
void perf_preempt_status(void);

#endif /* PERF_H */
