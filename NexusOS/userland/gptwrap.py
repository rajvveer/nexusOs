#!/usr/bin/env python3
"""gptwrap.py -- wrap the FAT32 volume of a NexusOS disk image in a GPT-partitioned disk.

    python -I gptwrap.py <source.img> <dest.img> [--start LBA] [--esp MIB] [--type nexus|basic]
                         [--sector 512|4096] [--name NAME] [--damage-primary]

The source is an MBR image (FAT32 partition of type 0x0B/0x0C, e.g. apps64.img/desktop64.img)
or a partitionless FAT32 image. The destination gets a protective MBR, primary and backup GPT
headers with CRC32-checked entry arrays (128 entries), an optional EFI system partition placed
first (a decoy: the kernel never mounts it) and the FAT32 volume copied to --start (in logical
sectors of --sector bytes). --type nexus uses the NexusOS root type GUID (blockdev.h), basic the
Microsoft basic-data GUID. Zero chunks are not written, so the copy stays sparse.
--damage-primary corrupts the primary header CRC to exercise the backup-header path.
The source is never modified; the destination must not exist.
"""
import argparse, os, struct, sys, uuid, zlib

NEXUS_ROOT = uuid.UUID('7D1B4F2C-3A86-4E59-9C0D-4E45585553F1')
BASIC_DATA = uuid.UUID('EBD0A0A2-B9E5-4433-87C0-68B6B72699C7')
EFI_SYSTEM = uuid.UUID('C12A7328-F81F-11D2-BA4B-00A0C93EC93B')
ENTRIES, ENTRY_SIZE = 128, 128
CHUNK = 1 << 20


def fat32_volume(path):
    """Return (byte offset, byte length) of the FAT32 volume in an image."""
    with open(path, 'rb') as f:
        mbr = f.read(512)
        f.seek(0, 2); size = f.tell()
    def bpb_ok(b):
        return b[510:512] == b'\x55\xaa' and struct.unpack_from('<H', b, 11)[0] == 512 and struct.unpack_from('<I', b, 36)[0] and b[13]
    if bpb_ok(mbr):
        return 0, struct.unpack_from('<I', mbr, 32)[0] * 512
    if mbr[510:512] != b'\x55\xaa':
        raise SystemExit('source has neither an MBR nor a FAT32 boot sector')
    for off in range(446, 510, 16):
        start, count = struct.unpack_from('<II', mbr, off + 8)
        if mbr[off + 4] in (0x0B, 0x0C) and count and (start + count) * 512 <= size:
            return start * 512, count * 512
    raise SystemExit('no FAT32 partition in the source MBR')


def entry(type_guid, unique, first, last, name):
    label = name.encode('utf-16-le')[:72]
    return type_guid.bytes_le + unique.bytes_le + struct.pack('<QQQ', first, last, 0) + label.ljust(72, b'\0')


def header(sector, my, alternate, first_usable, last_usable, disk_guid, entries_lba, entries_crc):
    h = bytearray(struct.pack('<8sIIIIQQQQ16sQIII', b'EFI PART', 0x00010000, 92, 0, 0, my, alternate,
                              first_usable, last_usable, disk_guid.bytes_le, entries_lba, ENTRIES, ENTRY_SIZE, entries_crc))
    struct.pack_into('<I', h, 16, zlib.crc32(bytes(h[:92])) & 0xFFFFFFFF)
    return bytes(h).ljust(sector, b'\0')


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('source'); ap.add_argument('dest')
    ap.add_argument('--start', type=int, default=None, help='first LBA of the FAT32 partition (default: 1 MiB aligned, after the ESP)')
    ap.add_argument('--esp', type=int, default=0, help='size in MiB of a leading EFI system partition (0: none)')
    ap.add_argument('--type', choices=('nexus', 'basic'), default='nexus')
    ap.add_argument('--sector', type=int, choices=(512, 4096), default=512)
    ap.add_argument('--name', default='NEXUSROOT')
    ap.add_argument('--damage-primary', action='store_true')
    args = ap.parse_args(argv)
    if os.path.exists(args.dest):
        raise SystemExit('destination exists: ' + args.dest)
    if os.path.abspath(args.source) == os.path.abspath(args.dest):
        raise SystemExit('source and destination are the same file')
    sector = args.sector
    offset, length = fat32_volume(args.source)
    volume_blocks = (length + sector - 1) // sector
    table_blocks = ENTRIES * ENTRY_SIZE // sector
    first_usable = 2 + table_blocks
    align = (1 << 20) // sector
    esp_blocks = args.esp * align
    esp_first = align if esp_blocks else 0
    start = args.start if args.start is not None else (esp_first + esp_blocks if esp_blocks else align)
    if start < first_usable or (esp_blocks and start < esp_first + esp_blocks):
        raise SystemExit('partition start overlaps the GPT or the ESP')
    last_usable = start + volume_blocks - 1
    total = last_usable + 1 + table_blocks + 1
    disk_guid = uuid.uuid5(uuid.NAMESPACE_URL, 'nexusos-gptwrap-disk-%d-%d' % (sector, start))
    rows = []
    if esp_blocks:
        rows.append(entry(EFI_SYSTEM, uuid.uuid5(disk_guid, 'esp'), esp_first, esp_first + esp_blocks - 1, 'EFI system'))
    rows.append(entry(NEXUS_ROOT if args.type == 'nexus' else BASIC_DATA, uuid.uuid5(disk_guid, 'root'), start, last_usable, args.name))
    table = b''.join(rows).ljust(ENTRIES * ENTRY_SIZE, b'\0')
    table_crc = zlib.crc32(table) & 0xFFFFFFFF
    mbr = bytearray(sector)
    struct.pack_into('<B3sB3sII', mbr, 446, 0, b'\x00\x02\x00', 0xEE, b'\xff\xff\xff', 1, min(total - 1, 0xFFFFFFFF))
    mbr[510:512] = b'\x55\xaa'
    primary = bytearray(header(sector, 1, total - 1, first_usable, last_usable, disk_guid, 2, table_crc))
    backup = header(sector, total - 1, 1, first_usable, last_usable, disk_guid, total - 1 - table_blocks, table_crc)
    if args.damage_primary:
        primary[16] ^= 0xFF
    with open(args.dest, 'xb') as out:
        if os.name == 'nt':
            import ctypes, msvcrt
            returned = ctypes.c_ulong()
            ctypes.windll.kernel32.DeviceIoControl(ctypes.c_void_p(msvcrt.get_osfhandle(out.fileno())), 0x900C4, None, 0, None, 0, ctypes.byref(returned), None)
        out.truncate(total * sector)
        out.seek(0); out.write(mbr); out.write(primary); out.write(table)
        out.seek((total - 1 - table_blocks) * sector); out.write(table); out.write(backup)
        with open(args.source, 'rb') as src:
            src.seek(offset)
            done = 0
            while done < length:
                block = src.read(min(CHUNK, length - done))
                if not block:
                    raise SystemExit('source volume truncated')
                if block.count(0) != len(block):
                    out.seek(start * sector + done); out.write(block)
                done += len(block)
    print('[gptwrap] %s: %d-byte sectors, %s FAT32 partition at LBA %d (%d blocks)%s, %d MiB total%s' % (
        args.dest, sector, args.type, start, volume_blocks, ', ESP first' if esp_blocks else '', total * sector >> 20,
        ', primary header damaged' if args.damage_primary else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
