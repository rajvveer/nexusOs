/* UMIP (User-Mode Instruction Prevention) with Linux-compatible emulation.
 *
 * With CR4.UMIP set, SGDT/SIDT/SLDT/SMSW/STR executed in ring 3 raise #GP instead of leaking kernel addresses.
 * Linux keeps userspace working (notably some Windows programs under Wine) by emulating them and returning fixed
 * dummy values (arch/x86/kernel/umip.c): SGDT/SIDT store a dummy base+limit, SLDT/STR store 0, SMSW stores CR0's
 * low word. umip_emulate() is called from the ring-3 #GP path; it decodes the faulting instruction, performs the
 * store, advances RIP past it, and returns 1 so no SIGSEGV is delivered. Anything it does not recognise returns 0
 * and takes the normal #GP -> SIGSEGV path. 64-bit addressing only (what long-mode user code generates). */
#include "native.h"
#include "uaccess.h"

#define UMIP_DUMMY_GDT_BASE 0xfffffffffffe0000ULL
#define UMIP_DUMMY_IDT_BASE 0xffffffffffff0000ULL

bool umip_enabled;   /* set by hardening.c when CR4.UMIP was turned on */

/* struct registers field order (native.h): r15..r8, rbp, rdi, rsi, rdx, rcx, rbx, rax, vector/error/rip/.../rsp. */
static uint64_t* gpr(struct registers* r, unsigned n) {
    switch (n & 15) {
    case 0: return &r->rax; case 1: return &r->rcx; case 2: return &r->rdx; case 3: return &r->rbx;
    case 4: return &r->rsp; case 5: return &r->rbp; case 6: return &r->rsi; case 7: return &r->rdi;
    case 8: return &r->r8; case 9: return &r->r9; case 10: return &r->r10; case 11: return &r->r11;
    case 12: return &r->r12; case 13: return &r->r13; case 14: return &r->r14; default: return &r->r15;
    }
}
static int32_t disp32(const uint8_t* p) { return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24); }
static uint64_t rdmsr(uint32_t msr) { uint32_t lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr)); return (uint64_t)hi << 32 | lo; }

int umip_emulate(struct registers* r) {
    if (!umip_enabled) return 0;
    uint8_t p[16];
    for (unsigned k = 0; k < 16; k++) {
        if (!accessible(r->rip + k, 1, false)) { if (k < 4) return 0; p[k] = 0; continue; }
        uaccess_begin(); p[k] = *(const uint8_t*)(r->rip + k); uaccess_end();
    }
    unsigned i = 0; uint8_t rex = 0; bool opsize = false; uint64_t seg_base = 0;
    for (; i < 15; i++) {
        uint8_t b = p[i];
        if (b == 0x66) { opsize = true; continue; }
        if (b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3) continue;
        if (b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26) { seg_base = 0; continue; }
        if (b == 0x64) { seg_base = rdmsr(0xC0000100); continue; }   /* FS */
        if (b == 0x65) { seg_base = rdmsr(0xC0000101); continue; }   /* GS */
        if ((b & 0xF0) == 0x40) { rex = b; continue; }                  /* REX */
        break;
    }
    if (i + 1 >= 15 || p[i] != 0x0F || (p[i + 1] != 0x00 && p[i + 1] != 0x01)) return 0;
    uint8_t op = p[i + 1], modrm = p[i + 2]; unsigned reg = (modrm >> 3) & 7, mod = modrm >> 6, rm = modrm & 7;
    enum { SGDT, SIDT, SLDT, STR_, SMSW, NONE } which = NONE;
    if (op == 0x01 && reg == 0) which = SGDT;
    else if (op == 0x01 && reg == 1) which = SIDT;
    else if (op == 0x00 && reg == 0) which = SLDT;
    else if (op == 0x00 && reg == 1) which = STR_;
    else if (op == 0x01 && reg == 4) which = SMSW;
    if (which == NONE) return 0;

    /* operand: register (mod==3) or a memory effective address */
    bool is_mem = mod != 3; uint64_t ea = 0; unsigned at = i + 3;   /* first byte after ModRM */
    if (is_mem) {
        uint64_t addr = 0;
        if (rm == 4) {   /* SIB */
            uint8_t sib = p[at++]; unsigned scale = sib >> 6, index = ((sib >> 3) & 7), base = sib & 7;
            if (!(index == 4 && !(rex & 2))) addr += *gpr(r, index | ((rex & 2) << 2)) << scale;
            if (base == 5 && mod == 0) { addr += disp32(p + at); at += 4; }
            else addr += *gpr(r, base | ((rex & 1) << 3));
            if (mod == 1) { addr += (int8_t)p[at]; at += 1; }
            else if (mod == 2) { addr += disp32(p + at); at += 4; }
        } else if (mod == 0 && rm == 5) {   /* RIP-relative */
            int32_t d = disp32(p + at); at += 4; addr = r->rip + at + (int64_t)d;   /* rip-relative off end of insn */
        } else {
            addr += *gpr(r, rm | ((rex & 1) << 3));
            if (mod == 1) { addr += (int8_t)p[at]; at += 1; }
            else if (mod == 2) { addr += disp32(p + at); at += 4; }
        }
        ea = addr + seg_base;
    }
    if (!is_mem && (which == SGDT || which == SIDT)) return 0;   /* require memory (else genuine #UD) */
    unsigned insn_len = at;

    if (which == SGDT || which == SIDT) {
        uint8_t desc[10] = {0}; uint64_t base = which == SGDT ? UMIP_DUMMY_GDT_BASE : UMIP_DUMMY_IDT_BASE;
        for (int k = 0; k < 8; k++) desc[2 + k] = (uint8_t)(base >> (8 * k));   /* limit 0, 64-bit dummy base */
        if (!accessible(ea, 10, true)) return 0;
        copy_to_user((void*)ea, desc, 10);
    } else {
        uint64_t value = 0;
        if (which == SMSW) { uint64_t cr0; __asm__ volatile("mov %%cr0,%0" : "=r"(cr0)); value = cr0 & 0xFFFF; }
        /* SLDT / STR return 0 (hide the real selectors), as Linux does. */
        if (is_mem) {   /* SLDT/STR/SMSW store 16 bits to memory regardless of operand size */
            uint16_t w = (uint16_t)value; if (!accessible(ea, 2, true)) return 0; copy_to_user((void*)ea, &w, 2);
        } else {
            uint64_t* dst = gpr(r, rm | ((rex & 1) << 3));
            unsigned bytes = opsize ? 2 : (rex & 8) ? 8 : 4;
            if (bytes == 2) *dst = (*dst & ~0xFFFFULL) | (value & 0xFFFF);
            else if (bytes == 4) *dst = value & 0xFFFFFFFFULL;   /* 32-bit op zero-extends the full 64-bit reg */
            else *dst = value;
        }
    }
    r->rip += insn_len;
    return 1;
}
