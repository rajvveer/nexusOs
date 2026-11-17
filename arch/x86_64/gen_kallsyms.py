"""Generate the /proc/kallsyms table (procext.c) from a linked kernel.elf.

Reads the ELF symbol table and writes a compact blob (out/kallsyms.bin):
    u32 count
    count x { u64 address, u8 type, u8 namelen, char name[namelen] }   (sorted by address)
then NUL padding to a 4 KiB boundary.

The blob lives in a loadable .kallsyms section placed after .data (linker.ld), so text/rodata/data symbol
addresses are identical with and without it; only .bss shifts up by the blob's page-aligned size. This script is run
on a FIRST link that has no .kallsyms section, so it adds that known padded size to every symbol at or above
__bss_start, making every stored address exact for the SECOND (final) link that embeds the blob.

usage: python -I gen_kallsyms.py <kernel.elf without .kallsyms> <out kallsyms.bin>
"""
import struct, sys
from pathlib import Path

elf = Path(sys.argv[1]).read_bytes()
out = Path(sys.argv[2])
e_shoff, = struct.unpack_from('<Q', elf, 0x28)
e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', elf, 0x3a)
secs = [struct.unpack_from('<IIQQQQIIQQ', elf, e_shoff + i * e_shentsize) for i in range(e_shnum)]
symtab = next((s for s in secs if s[1] == 2), None)   # SHT_SYMTAB
if not symtab:
    raise SystemExit('gen_kallsyms: kernel.elf has no symbol table')
off, size, link, entsz = symtab[4], symtab[5], symtab[6], symtab[9]
stroff = secs[link][4]
# __bss_start from the symbol table (the first link places it right after .data, no .kallsyms yet)
def sym_value(name):
    for i in range(size // entsz):
        nameoff, info, other, shndx, value, sz = struct.unpack_from('<IBBHQQ', elf, off + i * entsz)
        if nameoff:
            end = elf.index(b'\0', stroff + nameoff)
            if elf[stroff + nameoff:end] == name:
                return value
    return None
bss_start = sym_value(b'__bss_start') or 0

TYPE = {1: 'd', 2: 't'}   # STT_OBJECT, STT_FUNC (lower-case; local/global case is applied below)
entries = []
names_bytes = 0
for i in range(size // entsz):
    nameoff, info, other, shndx, value, sz = struct.unpack_from('<IBBHQQ', elf, off + i * entsz)
    typ = info & 0xf
    bind = info >> 4
    if shndx == 0 or nameoff == 0 or typ not in (1, 2):
        continue
    end = elf.index(b'\0', stroff + nameoff)
    name = elf[stroff + nameoff:end]
    if len(name) > 255 or not name:
        continue
    t = TYPE[typ]
    if bind != 0:   # not LOCAL -> upper-case (global/weak), like Linux kallsyms
        t = t.upper()
    entries.append((value, ord(t), name))
    names_bytes += len(name) + 1

# padded blob size: header(4) + per-entry(10 + namelen), rounded up to a page
raw = 4 + sum(10 + len(n) for _, _, n in entries)
padded = (raw + 4095) & ~4095
# the final link inserts `padded` bytes before .bss: correct every symbol at or above __bss_start
entries = [((v + padded) if v >= bss_start and bss_start else v, t, n) for v, t, n in entries]
entries.sort(key=lambda e: e[0])

buf = bytearray(struct.pack('<I', len(entries)))
for value, t, name in entries:
    buf += struct.pack('<QBB', value, t, len(name)) + name
buf += b'\0' * (padded - len(buf))
out.write_bytes(buf)
print(f'[kallsyms] {len(entries)} symbols, {padded} bytes (bss fixup +{padded} from {hex(bss_start)})', flush=True)
