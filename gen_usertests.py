#!/usr/bin/env python3
"""Assemble the Phase 54 user-mode test binaries (kernel/usertests/*.asm) with
NASM (-f bin) and emit kernel/usertests.h as committed C byte arrays, so the
kernel build needs no extra toolchain step. Run from the NexusOS directory:

    python gen_usertests.py
"""
import os, subprocess, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
NASM = os.path.join(ROOT, "tools", "nasm-2.16.01", "nasm.exe")
SRC  = os.path.join(ROOT, "kernel", "usertests")
OUT  = os.path.join(ROOT, "kernel", "usertests.h")

# (install name in ramfs, source .asm)
BINS = [
    ("hello.elf",   "hello_elf.asm"),
    ("spin.elf",    "spin_elf.asm"),
    ("segv.elf",    "segv_elf.asm"),
    ("hello.macho", "hello_macho.asm"),
    ("hello.exe",   "hello_pe.asm"),
]

def assemble(asm):
    src = os.path.join(SRC, asm)
    out = os.path.join(SRC, asm[:-4] + ".bin")
    nasm = NASM if os.path.exists(NASM) else "nasm"
    subprocess.run([nasm, "-f", "bin", src, "-o", out], check=True)
    with open(out, "rb") as f:
        return f.read()

def c_ident(name):
    return "ut_" + name.replace(".", "_")

def main():
    blobs = [(name, c_ident(name), assemble(asm)) for name, asm in BINS]
    lines = []
    lines.append("/* ============================================================================")
    lines.append(" * NexusOS - built-in user-mode test binaries (Phase 54) - GENERATED")
    lines.append(" * Do not edit by hand; regenerate with: python gen_usertests.py")
    lines.append(" * Sources: the kernel/usertests asm files (ELF/PE/Mach-O, nasm -f bin).")
    lines.append(" * ============================================================================ */")
    lines.append("#ifndef USERTESTS_H")
    lines.append("#define USERTESTS_H")
    lines.append("")
    lines.append("#include \"types.h\"")
    lines.append("")
    for name, ident, data in blobs:
        lines.append("/* %s (%d bytes) */" % (name, len(data)))
        lines.append("static const uint8_t %s[] = {" % ident)
        row = "    "
        for i, b in enumerate(data):
            row += "0x%02X," % b
            if (i % 12) == 11:
                lines.append(row)
                row = "    "
        if row.strip():
            lines.append(row)
        lines.append("};")
        lines.append("")
    lines.append("typedef struct { const char* name; const uint8_t* data; uint32_t len; } usertest_bin_t;")
    lines.append("")
    lines.append("static const usertest_bin_t usertest_bins[] = {")
    for name, ident, data in blobs:
        lines.append("    { \"%s\", %s, %d }," % (name, ident, len(data)))
    lines.append("};")
    lines.append("#define USERTEST_COUNT ((int)(sizeof(usertest_bins)/sizeof(usertest_bins[0])))")
    lines.append("")
    lines.append("#endif /* USERTESTS_H */")
    with open(OUT, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote", OUT)
    for name, ident, data in blobs:
        print("  %-12s %4d bytes" % (name, len(data)))

if __name__ == "__main__":
    sys.exit(main())
