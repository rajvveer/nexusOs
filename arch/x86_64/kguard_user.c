/* kguard containment trigger (tests/test_x64_kguard.py). Makes a private syscall that asks the kernel to perform a
 * stray ring-0 user access (mode '0', an SMAP violation) or overflow its own kernel stack (mode '1'). The kernel's
 * kguard path must kill this task, so the syscall never returns; if it does, containment failed and we exit non-zero
 * so the campaign sees a clean exit (a failure) instead of the expected signal death. */
#define _GNU_SOURCE
#include <unistd.h>
#include <sys/syscall.h>

int main(int argc, char** argv) {
    char mode = argc > 1 ? argv[1][0] : '0';
    volatile unsigned long canary = 0x1234567890ABCDEFUL;
    if (mode == '1') syscall(0x6B02);                 /* kernel-stack overflow */
    else syscall(0x6B01, (unsigned long)&canary);     /* stray ring-0 read of this mapped stack address */
    return 3;   /* reached only if the kernel did NOT kill us: containment failed */
}
