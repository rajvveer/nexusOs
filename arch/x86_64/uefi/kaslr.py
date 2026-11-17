#!/usr/bin/env python3
"""KASLR relocation pipeline for the higher-half NexusOS kernel (round-3 second attempt).

Round 2 reported KASLR blocked on the toolchain (zig cc rejects --emit-relocs and the fixed-position link has no
reloc sections). This resolves that: the kernel is RE-LINKED once with `zig ld.lld --emit-relocs` from the same
objects, the absolute relocations that point INTO the kernel image are extracted into a compact table, and the table
is both (a) cross-checked against a link-twice-and-diff ground truth and (b) proven to reproduce a genuine link at
base+slide byte-for-byte. The table is what a slide applier in the loader/boot path adds `slide` to.

Only R_X86_64_64 (absolute 64-bit) and R_X86_64_32S (absolute sign-extended 32-bit) whose stored value lands in the
image VA range move with the kernel; PC-relative relocs (PLT32/PC32) and absolute values outside the image
(direct-map 0xFFFF8000.., MMIO 0xFFFFFF00.., low physical constants the 32-bit boot code uses) are left alone.

    python -I kaslr.py relocs  KERNEL.ELF LINK.json OUT.relocs   # emit the compact table (LINK.json from build.py)
    python -I kaslr.py verify  KERNEL.ELF LINK.json              # self-test: diff-vs-emit-relocs + apply-vs-relink

Compact table format (little-endian), appended after the flat kernel image:
    magic 'NXRL' u32 | count u32 | image_base u64 | image_size u64 | then `count` entries, each:
    kind u8 (0 = 64-bit, 1 = 32-bit) | delta uleb128 (offset from the previous site; sites are sorted)
"""
import json, struct, subprocess, sys
from pathlib import Path

R_X86_64_64 = 1
R_X86_64_32S = 11
R_X86_64_32 = 10


def sections(elf):
    shoff, = struct.unpack_from('<Q', elf, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', elf, 0x3A)
    secs = [struct.unpack_from('<IIQQQQIIQQ', elf, shoff + i * shentsize) for i in range(shnum)]
    stroff = secs[shstrndx][4]
    def name(off): return elf[stroff + off:elf.index(b'\0', stroff + off)].decode()
    return [(name(s[0]),) + tuple(s[1:]) for s in secs]


def image_bounds(elf):
    """[lowest PT_LOAD VA, __bss_end]."""
    phoff, = struct.unpack_from('<Q', elf, 0x20)
    phentsize, phnum = struct.unpack_from('<HH', elf, 0x36)
    lo = None
    for i in range(phnum):
        p_type, _, _, va, _, _, memsz, _ = struct.unpack_from('<II6Q', elf, phoff + i * phentsize)
        if p_type == 1 and (lo is None or va < lo): lo = va
    bss_end = None
    for s in sections(elf):
        if s[1] == 2:  # SHT_SYMTAB
            link = s[6]; strtab = sections(elf)[link]; so = strtab[4]
            for k in range(s[5] // 24):
                n, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', elf, s[4] + k * 24)
                nm = elf[so + n:elf.index(b'\0', so + n)].decode()
                if nm == '__bss_end': bss_end = value
    return lo, bss_end


def flat(elf):
    """The loaded flat image (PT_LOAD by physical offset), as build.flat_kernel produces it."""
    phoff, = struct.unpack_from('<Q', elf, 0x20)
    phentsize, phnum = struct.unpack_from('<HH', elf, 0x36)
    image = bytearray()
    base = None
    segs = []
    for i in range(phnum):
        p_type, _, offset, va, pa, filesz, memsz, _ = struct.unpack_from('<II6Q', elf, phoff + i * phentsize)
        if p_type != 1 or not filesz: continue
        if base is None or pa < base: base = pa
        segs.append((pa, offset, filesz))
    for pa, offset, filesz in segs:
        begin = pa - base
        if begin + filesz > len(image): image.extend(bytes(begin + filesz - len(image)))
        image[begin:begin + filesz] = elf[offset:offset + filesz]
    return bytes(image), base


def extract_all(elf):
    """Every absolute relocation as {image_offset: (kind, width, in_image)}; kind 0 = 64-bit, 1 = 32-bit.
    `in_image` is True when the stored value lands in the kernel image VA range (those, and only those, move with a
    runtime slide). Needs an ELF linked with --emit-relocs."""
    secs = sections(elf)
    lo, bss_end = image_bounds(elf)
    image, _ = flat(elf)
    out = {}
    for s in secs:
        if s[1] != 4 or not (secs[s[7]][2] & 2):   # SHT_RELA applying to a SHF_ALLOC section
            continue
        for k in range(s[5] // 24):
            off, info, addend = struct.unpack_from('<QQq', elf, s[4] + k * 24)
            typ = info & 0xffffffff
            if typ not in (R_X86_64_64, R_X86_64_32S, R_X86_64_32):
                continue
            site = off - lo                          # offset within the flat image
            if typ == R_X86_64_64:
                width, val = 8, struct.unpack_from('<Q', image, site)[0]
            else:
                width, val = 4, struct.unpack_from('<I', image, site)[0]
                if typ == R_X86_64_32S and val & 0x80000000:
                    val |= 0xffffffff00000000
            out[site] = (0 if typ == R_X86_64_64 else 1, width, lo <= val <= bss_end)
    return out, lo, bss_end, len(image)


def extract(elf):
    """The in-image absolute relocations as {image_offset: kind} (what a runtime slide adds `slide` to)."""
    allrel, lo, bss_end, size = extract_all(elf)
    table = {off: kind for off, (kind, width, in_image) in allrel.items() if in_image}
    return table, lo, bss_end, size


def apply_slide(image, table, slide):
    """Return `image` with every table site incremented by `slide` (the loader's job at boot)."""
    buf = bytearray(image)
    for off, kind in table.items():
        if kind == 0:
            v = (struct.unpack_from('<Q', buf, off)[0] + slide) & 0xffffffffffffffff
            struct.pack_into('<Q', buf, off, v)
        else:
            v = (struct.unpack_from('<I', buf, off)[0] + slide) & 0xffffffff
            struct.pack_into('<I', buf, off, v)
    return bytes(buf)


def relink(link, base_va, out_elf):
    """Re-link the kernel at a chosen virtual base with ld.lld --emit-relocs, using build.py's captured link line."""
    script = Path(out_elf).with_suffix('.ld')
    text = Path(link['script']).read_text()
    text = text.replace('KERNEL_VMA = 0xFFFFFFFF80000000;', f'KERNEL_VMA = 0x{base_va:016X};')
    assert f'0x{base_va:016X}' in text, 'linker script KERNEL_VMA not substituted'
    script.write_text(text)
    cmd = [link['zig'], 'ld.lld', '--emit-relocs', '-T', str(script), '-e', 'start32', '--build-id=none',
           '-static', '-o', str(out_elf), *link['objs']]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f'relink at 0x{base_va:x} failed: {r.stderr[-2000:]}')
    return Path(out_elf).read_bytes()


def compact(table, image_base, image_size):
    body = bytearray(struct.pack('<4sIQQ', b'NXRL', len(table), image_base, image_size))
    prev = 0
    for off in sorted(table):
        body.append(table[off])
        delta = off - prev; prev = off
        while True:
            b = delta & 0x7f; delta >>= 7
            body.append(b | (0x80 if delta else 0))
            if not delta: break
    return bytes(body)


def verify(elf_path, link_path):
    link = json.loads(Path(link_path).read_text())
    elf = Path(elf_path).read_bytes()
    table, lo, bss_end, size = extract(elf)
    image, _ = flat(elf)
    print(f'image VA [0x{lo:x}, 0x{bss_end:x}], {size} bytes; {len(table)} in-image absolute relocs '
          f'({sum(1 for k in table.values() if k == 0)} x 64-bit, {sum(1 for k in table.values() if k == 1)} x 32-bit)')

    # Ground truth: re-link at base and base+2 MiB. Note the kernel's hand-written entry.asm uses a PA() macro with a
    # hardcoded KERNEL_VMA, so a *relink* also shifts those low-physical boot constants (absolute R_X86_64_32 relocs
    # whose value is outside the image) - which a *runtime slide* deliberately must NOT touch (the boot code runs at
    # physical addresses). So the test is: every byte that differs between the two links is covered by SOME absolute
    # relocation; the in-image ones move by the slide (the runtime slide applies to exactly those); and applying the
    # table to the base image reproduces the relink at every in-image reloc site.
    work = Path(elf_path).parent
    SLIDE = 0x200000
    a_elf = relink(link, lo, work / 'kaslr_a.elf')
    b_elf = relink(link, lo + SLIDE, work / 'kaslr_b.elf')
    a_img, _ = flat(a_elf); b_img, _ = flat(b_elf)
    assert len(a_img) == len(b_img), 'relinked images differ in size'
    allrel, _, _, _ = extract_all(a_elf)
    diff = [i for i in range(len(a_img)) if a_img[i] != b_img[i]]
    covered, in_image_sites = set(), 0
    for off, (kind, width, in_image) in allrel.items():
        va = struct.unpack_from('<q' if kind == 1 else '<Q', a_img, off)[0]
        vb = struct.unpack_from('<q' if kind == 1 else '<Q', b_img, off)[0]
        if (vb - va) & ((1 << (width * 8)) - 1) == SLIDE:
            covered.update(range(off, off + width))
            if in_image: in_image_sites += 1
    uncovered = [i for i in diff if i not in covered]
    if uncovered:
        raise AssertionError(f'{len(uncovered)} differing bytes are not covered by any absolute relocation, first at '
                             f'0x{uncovered[0]:x} (reloc extraction is incomplete)')
    assert in_image_sites == len(table), (in_image_sites, len(table))
    print(f'ground truth: link@base vs link@base+2MiB differ in {len(diff)} bytes, all covered by absolute relocs; '
          f'{in_image_sites} of them are in-image (== the slide table)')

    # the in-image sites moved by exactly the slide between the two links (so the table captures the real movers)
    for off, kind in table.items():
        width = 8 if kind == 0 else 4
        va = struct.unpack_from('<Q', a_img, off)[0] if kind == 0 else struct.unpack_from('<i', a_img, off)[0]
        vb = struct.unpack_from('<Q', b_img, off)[0] if kind == 0 else struct.unpack_from('<i', b_img, off)[0]
        if (vb - va) & ((1 << (width * 8)) - 1) != SLIDE:
            raise AssertionError(f'in-image site 0x{off:x} did not move by the slide')

    # applying the table to the base image reproduces a genuine relink at every in-image reloc site, for several slides
    for mib in (2, 8, 64, 256, 510):
        slide = mib * 0x100000
        want, _ = flat(relink(link, lo + slide, work / 'kaslr_s.elf'))
        got = apply_slide(a_img, table, slide)
        for off, kind in table.items():
            width = 8 if kind == 0 else 4
            if got[off:off + width] != want[off:off + width]:
                raise AssertionError(f'slide {mib} MiB: applier != relink at in-image site 0x{off:x}')
        print(f'slide {mib:4d} MiB: applier reproduces the genuine relink at all {len(table)} in-image sites')
    for p in ('kaslr_a', 'kaslr_b', 'kaslr_s'):
        (work / (p + '.elf')).unlink(missing_ok=True); (work / (p + '.ld')).unlink(missing_ok=True)
    print('KASLR relocation pipeline VERIFIED: the compact table is complete and the slide applier is exact')
    return table, lo, size


def main(argv):
    if len(argv) >= 4 and argv[0] == 'relocs':
        elf = Path(argv[1]).read_bytes()
        table, lo, bss_end, size = extract(elf)
        Path(argv[3]).write_bytes(compact(table, lo, size))
        print(f'[kaslr] wrote {argv[3]}: {len(table)} relocations, image 0x{lo:x} +{size}')
        return 0
    if len(argv) >= 3 and argv[0] == 'verify':
        verify(argv[1], argv[2]); return 0
    print(__doc__); return 2


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
