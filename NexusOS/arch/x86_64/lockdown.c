/* Kernel lockdown (Linux security/lockdown semantics). When Secure Boot is active the kernel starts in integrity
 * lockdown by default; the `lockdown=` boot option overrides. Lockdown closes the paths by which user space (even a
 * privileged one) could change the running kernel or read its secrets:
 *   integrity (level 1): no unsigned kernel image through the update/kexec paths, no /dev/mem, /dev/port or raw I/O
 *     port access, no unauthenticated hibernation image, kernel pointers and debug interfaces restricted.
 *   confidentiality (level 2): everything in integrity, plus reads of kernel memory are refused too.
 * The level can only be raised, never lowered, at run time (Linux lock_kernel_down): a one-way ratchet.
 *
 * NexusOS already lacks several of the attack surfaces Linux must gate: there is no /dev/mem, /dev/port, ioperm/iopl,
 * kexec (reboot(2) LINUX_REBOOT_CMD_KEXEC returns EINVAL) or module loading, so those are refused regardless. What
 * lockdown adds here is: (1) kernel_lockdown_level(), the value power2 checks before writing a hibernation image
 * (the image is CRC-checked, not authenticated, so hibernation is refused under integrity lockdown); (2) the policy
 * for the update path and kptr/dmesg restriction; (3) a single enforcement helper, kernel_lockdown_denied().
 *
 * power2 declares `int kernel_lockdown_level(void)` weak and calls it; this file provides the strong definition. */
#include "native.h"
#include "boot_info.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

enum { LOCKDOWN_NONE = 0, LOCKDOWN_INTEGRITY = 1, LOCKDOWN_CONFIDENTIALITY = 2 };
static int lockdown_level;
static bool lockdown_locked_by_secureboot;

/* Reasons, for the audit line and /sys write side (Linux lockdown_reason order where they overlap). */
static const char* const reason_name[] = {
    "none", "modules", "/dev/mem", "/dev/port", "ioport", "hibernation", "pci_access", "kexec",
    "kernel_image_load", "kprobes", "bpf_write", "debugfs", "kcore", "confidential",
};

int kernel_lockdown_level(void) { return lockdown_level; }
bool kernel_lockdown_active(void) { return lockdown_level >= LOCKDOWN_INTEGRITY; }
bool kernel_lockdown_confidential(void) { return lockdown_level >= LOCKDOWN_CONFIDENTIALITY; }

static unsigned denials;
/* True when `reason` is denied at the current level (and logs it, rate-limited). `confidential` marks a reason that
 * only applies at the confidentiality level (reading kernel state), as Linux's two lockdown_reason bands do. */
bool kernel_lockdown_denied(const char* reason, bool confidential) {
    int need = confidential ? LOCKDOWN_CONFIDENTIALITY : LOCKDOWN_INTEGRITY;
    if (lockdown_level < need) return false;
    if (denials < 32) {
        denials++;
        vga_print("[64] Lockdown: blocked "); vga_print(reason ? reason : "operation");
        vga_print(lockdown_level >= LOCKDOWN_CONFIDENTIALITY ? " (confidentiality)\n" : " (integrity)\n");
    }
    return true;
}

/* A signed/authenticated kernel image may still load under lockdown; an unsigned one may not (Linux
 * LOCKDOWN_KEXEC / kernel_image_load). The update path (nexus-update) always verifies the bundle signature and the
 * UEFI loader verifies each slot kernel against its kernel.sig, so this guards the in-kernel decision points. */
bool kernel_lockdown_allows_kernel_image(bool authenticated) {
    if (authenticated) return true;
    return !kernel_lockdown_denied(reason_name[8], false);
}

/* Set the level (one-way: can only rise). Returns the new level, or -1 if a lower level was requested. */
int kernel_lockdown_set(int level) {
    if (level < LOCKDOWN_NONE || level > LOCKDOWN_CONFIDENTIALITY) return -1;
    if (level < lockdown_level) return -1;   /* Linux: lockdown is a ratchet */
    lockdown_level = level;
    return lockdown_level;
}

/* /sys/kernel/security/lockdown text: the options with the active one in brackets (Linux lockdown_read). */
unsigned kernel_lockdown_text(char* out, unsigned capacity) {
    static const char* const names[] = {"none", "integrity", "confidentiality"};
    unsigned used = 0; out[0] = 0;
    for (int i = 0; i <= LOCKDOWN_CONFIDENTIALITY; i++) {
        if (used && used + 1 < capacity) out[used++] = ' ';
        bool active = i == lockdown_level;
        if (active && used + 1 < capacity) out[used++] = '[';
        unsigned n = strlen(names[i]); if (used + n < capacity) { memcpy(out + used, names[i], n); used += n; }
        if (active && used + 1 < capacity) out[used++] = ']';
    }
    if (used + 1 < capacity) out[used++] = '\n';
    out[used] = 0; return used;
}
int kernel_lockdown_write(const char* text) {   /* a name, raising the level (Linux never lowers it) */
    static const char* const names[] = {"none", "integrity", "confidentiality"};
    unsigned n = 0; while (text[n] && text[n] != '\n' && text[n] != ' ') n++;
    for (int i = 0; i <= LOCKDOWN_CONFIDENTIALITY; i++)
        if (strlen(names[i]) == n && !strncmp(text, names[i], n)) return kernel_lockdown_set(i) < 0 ? -22 : 0;
    return -22;
}

/* Self-test (tests/test_x64_lockdown.py boots once per level): invariants relative to the current boot level. */
void lockdown_selftest(void) {
    int level = kernel_lockdown_level();
    char text[64]; kernel_lockdown_text(text, sizeof(text));
    vga_print("[64] LOCKDOWN level="); { char n[4] = {(char)('0' + level), 0}; vga_print(n); }
    vga_print(" sys="); vga_print(text);   /* text ends in \n */
    bool ok = true;
    /* integrity-band denials (hibernation, unsigned kernel image, /dev/mem) match active >= integrity */
    ok &= kernel_lockdown_denied("hibernation", false) == (level >= 1);
    ok &= kernel_lockdown_allows_kernel_image(false) == (level < 1);
    ok &= kernel_lockdown_allows_kernel_image(true) == true;          /* a signed image is always allowed */
    ok &= kernel_lockdown_denied("kcore", true) == (level >= 2);      /* confidentiality band */
    ok &= kernel_lockdown_active() == (level >= 1);
    ok &= kernel_lockdown_confidential() == (level >= 2);
    /* ratchet: lowering is refused, raising within bounds works and is undone by nothing but a higher set */
    if (level > 0) ok &= kernel_lockdown_set(level - 1) < 0 && kernel_lockdown_level() == level;
    ok &= kernel_lockdown_set(level) == level;
    if (!ok) { vga_print("[64] LOCKDOWN FAIL\n"); return; }
    vga_print("[64] LOCKDOWN SELFTEST PASSED\n");
}

void lockdown_init(const char* option) {
    /* `lockdown=integrity|confidentiality|none|1|2` overrides; otherwise Secure Boot with a verified kernel locks
     * down to integrity (Linux CONFIG_LOCK_DOWN_IN_SECURE_BOOT). */
    if (option && *option) {
        if (!strcmp(option, "confidentiality") || option[0] == '2') lockdown_level = LOCKDOWN_CONFIDENTIALITY;
        else if (!strcmp(option, "integrity") || option[0] == '1') lockdown_level = LOCKDOWN_INTEGRITY;
        else lockdown_level = LOCKDOWN_NONE;   /* "none"/"0"/anything else: off */
    } else if (boot_secure_boot() && boot_kernel_verified()) {
        lockdown_level = LOCKDOWN_INTEGRITY; lockdown_locked_by_secureboot = true;
    }
    static const char* const names[] = {"none", "integrity", "confidentiality"};
    vga_print("[64] Lockdown: "); vga_print(names[lockdown_level]);
    if (lockdown_locked_by_secureboot) vga_print(" (Secure Boot)");
    vga_print("\n");
}
