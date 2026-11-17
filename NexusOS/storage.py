"""Grow disk.img safely, moving FAT beyond the fixed NPFS/persistence region.

Usage: python -I storage.py [size-in-MiB=1024] [disk.img]
Existing files are migrated; a nonempty or corrupt filesystem is never formatted.
"""
from pathlib import Path
import ctypes
import hashlib
import importlib.util
import os
import struct
import sys
import tempfile

ROOT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('fatput', ROOT / 'userland/fatput.py')
fatput = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fatput)
BASE = fatput.DATA_LBA * 512


def sparse(file):
    if os.name == 'nt':
        import msvcrt
        returned = ctypes.c_ulong()
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.DeviceIoControl.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p,
                                          ctypes.c_ulong, ctypes.c_void_p, ctypes.c_ulong,
                                          ctypes.c_void_p, ctypes.c_void_p]
        if not kernel.DeviceIoControl(msvcrt.get_osfhandle(file.fileno()), 0x900C4,
                                      None, 0, None, 0, ctypes.byref(returned), None):
            raise ctypes.WinError(ctypes.get_last_error())


def digest(path):
    with open(path, 'rb') as file:
        return hashlib.file_digest(file, 'sha256').digest()


def migrate(src, dst, source_dir, target_dir):
    for name, _, _, entry, _ in src.named_entries(source_dir):
        if entry[0] == 0: break
        if entry[0] == 0xE5 or entry[11] == 0x0F or entry[11] & 8: continue
        if entry[:1] == b'.': continue
        first = (struct.unpack_from('<H', entry, 20)[0] << 16) | struct.unpack_from('<H', entry, 26)[0]
        if entry[11] & 0x10:
            child = dst.mkdir_name(target_dir, name, entry)
            migrate(src, dst, first, child)
            continue
        size = struct.unpack_from('<I', entry, 28)[0]
        target_first = dst.alloc((size + dst.cl_bytes - 1) // dst.cl_bytes)
        source_chain = iter(src.chain(first)) if size else iter(())
        pending = bytearray()
        remaining = size
        for cluster in dst.chain(target_first) if size else ():
            count = min(remaining, dst.cl_bytes)
            while len(pending) < count:
                try: source_cluster = next(source_chain)
                except StopIteration: raise ValueError('source file has a truncated cluster chain')
                pending += src.rd(src.cl_off(source_cluster), src.cl_bytes)
            dst.wr(dst.cl_off(cluster), pending[:count])
            del pending[:count]
            remaining -= count
        dst.create_entry(target_dir, name, entry[11], target_first, size, entry)
    dst.flush_fat()


def grow(path, size_mib):
    size = size_mib * 1024 * 1024
    if size_mib < 64 or size > 128 * 1024**3:
        raise ValueError('Choose 64..131072 MiB (the ATA LBA28 address range)')
    path = Path(path).resolve()
    if size <= path.stat().st_size:
        raise ValueError('New size must be larger than the existing disk')
    original_hash = digest(path)
    src = fatput.Fat32(path)
    if src.base == 0 and path.stat().st_size != BASE:
        src.close(); raise ValueError('Legacy disks must have the known 16 MiB NexusOS layout')
    if src.base and (src.base != BASE or src.base + src.total * 512 != path.stat().st_size):
        src.close(); raise ValueError('Unsupported partition layout; refusing to discard unknown disk regions')
    if src.base:
        with open(path, 'rb') as image:
            image.seek(462)
            if any(image.read(48)):
                src.close(); raise ValueError('Additional partitions must be migrated separately')
    fd, temp = tempfile.mkstemp(prefix='.storage-', suffix='.img', dir=path.parent)
    os.close(fd)
    dst = None
    try:
        with open(path, 'rb') as old, open(temp, 'w+b') as new:
            sparse(new)
            new.truncate(size)
            prefix = old.read(BASE)
            if len(prefix) != BASE: raise ValueError('NexusOS disk must contain the reserved 16 MiB prefix')
            new.seek(0); new.write(prefix)
            mbr = bytearray(512)
            mbr[446 + 4] = 0x0C
            sectors = (size - BASE) // 512
            struct.pack_into('<II', mbr, 446 + 8, fatput.DATA_LBA, sectors)
            mbr[510:512] = b'\x55\xaa'
            new.seek(0); new.write(mbr)
            formatter = fatput.Fat32.__new__(fatput.Fat32)
            formatter.f, formatter.base, formatter.limit = new, BASE, size - BASE
            formatter.format(sectors)
        dst = fatput.Fat32(temp)
        migrate(src, dst, src.rootc, dst.rootc)
        if not dst.find_name(dst.rootc, 'Downloads'):
            dst.mkdir_name(dst.rootc, 'Downloads')
            dst.flush_fat()
        dst.close(); dst = None
        src.close()
        with open(temp, 'rb') as new:
            new.seek(fatput.LIMIT)
            if new.read(BASE - fatput.LIMIT) != prefix[fatput.LIMIT:]:
                raise ValueError('Protected NPFS/persistence bytes changed')
        if digest(path) != original_hash:
            raise ValueError('Disk changed while growing; stop QEMU and retry')
        os.replace(temp, path)
        print(f'[OK] {path}: {size_mib} MiB; existing files migrated, NPFS/persistence preserved')
    finally:
        src.close()
        if dst: dst.close()
        if os.path.exists(temp): os.remove(temp)


def ensure(path):
    path = Path(path)
    volume = fatput.Fat32(path)
    expanded = volume.base >= BASE
    volume.close()
    if not expanded:
        grow(path, 1024)


if __name__ == '__main__' and len(sys.argv) > 1 and sys.argv[1] == '--ensure':
    ensure(sys.argv[2] if len(sys.argv) > 2 else ROOT / 'disk.img')
elif __name__ == '__main__':
    grow(sys.argv[2] if len(sys.argv) > 2 else ROOT / 'disk.img',
         int(sys.argv[1]) if len(sys.argv) > 1 else 1024)
