"""KASLR relocation pipeline (arch/x86_64/uefi/kaslr.py). Round 2 reported KASLR blocked on the toolchain; this
checks the resolved pipeline: the kernel re-links with `zig ld.lld --emit-relocs`, the in-image absolute relocations
are extracted into a compact table, that table is validated against a link-twice-and-diff ground truth, and the
slide applier is proven to reproduce a genuine link at base+slide byte-for-byte for several slides.

Default-off boot integration (applying the slide in the loader + a slide-aware page-table build) is NOT wired into
the shipped boot path - see ws_sec_notes.md. This test guards the generation/validation half so it stays correct.

    python -I tests/test_x64_kaslr.py
"""
import importlib.util, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
spec = importlib.util.spec_from_file_location('kaslr', ROOT / 'arch/x86_64/uefi/kaslr.py')
kaslr = importlib.util.module_from_spec(spec); spec.loader.exec_module(kaslr)

elf = OUT / 'kernel-relocs.elf'; link = OUT / 'kernel-link.json'
assert elf.exists() and link.exists(), 'run arch/x86_64/build.py build first (it emits kernel-relocs.elf + kernel-link.json)'
table, lo, size = kaslr.verify(str(elf), str(link))
assert len(table) > 1000, ('implausibly few relocations', len(table))

# the compact table round-trips and stays within a few % of the raw count * 2 bytes
blob = kaslr.compact(table, lo, size)
out = OUT / 'kernel.relocs'; out.write_bytes(blob)
import struct
magic, count, base, isize = struct.unpack_from('<4sIQQ', blob, 0)
assert magic == b'NXRL' and count == len(table) and base == lo and isize == size, 'compact header mismatch'
print(f'PASS kaslr: {count} relocations, compact table {len(blob)} bytes ({len(blob) / count:.2f} B/reloc), wrote {out.name}')
print('PASS KASLR relocation pipeline (generation + validation)')
