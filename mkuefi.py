#!/usr/bin/env python3
"""mkuefi.py - build the NexusOS UEFI boot media (no external tools).

    python -I mkuefi.py BOOTX64.EFI kernel.bin out.img [--cmdline "noioapic ..."]
                        [--efiboot efiboot.img] [--size-mib 64]

out.img      raw GPT disk: protective MBR, primary/backup GPT, one EFI System Partition (FAT32,
             starting at LBA 2048) holding
                 EFI/BOOT/BOOTX64.EFI     the loader (removable-media default path)
                 EFI/NEXUS/KERNEL.BIN     the flat kernel image
                 EFI/NEXUS/CMDLINE.TXT    kernel options (only with --cmdline)
                 STARTUP.NSH              UEFI shell fallback that starts the loader
             It is a plain block image: `dd if=out.img of=/dev/sdX bs=4M` (or Rufus "DD mode")
             makes a bootable USB stick for a PC with UEFI firmware.
efiboot.img  the same files on a small FAT16 volume, for the El Torito EFI entry of the ISO
             (mkiso.py third argument).
"""
import argparse
import struct
import sys
import uuid
import zlib
from pathlib import Path

SECTOR = 512
ESP_TYPE = uuid.UUID('C12A7328-F81F-11D2-BA4B-00A0C93EC93B')
NAMESPACE = uuid.UUID('6E657875-736F-532D-9165-666962756C64')   # fixed: reproducible images


def short_name(name):
    base, _, ext = name.upper().partition('.')
    if not base or len(base) > 8 or len(ext) > 3:
        raise ValueError('not an 8.3 name: ' + name)
    return (base.ljust(8) + ext.ljust(3)).encode('ascii')


def dir_entry(name11, attr, cluster, size):
    # 2026-01-01 00:00 as a fixed timestamp keeps the image reproducible
    date = ((2026 - 1980) << 9) | (1 << 5) | 1
    return struct.pack('<11sBBBHHHHHHHI', name11, attr, 0, 0, 0, date, date, cluster >> 16, 0, date, cluster & 0xFFFF, size)


class Fat:
    """A tiny FAT16/FAT32 writer: 8.3 names, directories and files, laid out contiguously."""

    def __init__(self, sectors, fat32, label, hidden=0):
        self.sectors, self.fat32, self.hidden = sectors, fat32, hidden
        self.reserved = 32 if fat32 else 4
        self.root_entries = 0 if fat32 else 512
        self.cluster_sectors = 1
        root_sectors = self.root_entries * 32 // SECTOR
        fat_sectors = 1
        while True:
            data = sectors - self.reserved - 2 * fat_sectors - root_sectors
            clusters = data // self.cluster_sectors
            need = ((clusters + 2) * (4 if fat32 else 2) + SECTOR - 1) // SECTOR
            if need <= fat_sectors:
                break
            fat_sectors = need
        if fat32 and clusters < 65525 or not fat32 and not 4085 <= clusters < 65525:
            raise ValueError(f'{sectors} sectors give {clusters} clusters: wrong FAT type')
        self.fat_sectors, self.clusters, self.root_sectors = fat_sectors, clusters, root_sectors
        self.data_start = self.reserved + 2 * fat_sectors + root_sectors
        self.image = bytearray(sectors * SECTOR)
        self.fat = [0] * (clusters + 2)
        self.fat[0] = 0x0FFFFFF8 if fat32 else 0xFFF8
        self.fat[1] = 0x0FFFFFFF if fat32 else 0xFFFF
        self.next_cluster = 2
        self.label = label.upper().ljust(11)[:11].encode('ascii')
        self.dirs = {(): []}            # path tuple -> list of entries (bytes)
        self.dir_cluster = {}
        if fat32:
            self.dir_cluster[()] = self.allocate(SECTOR * self.cluster_sectors)
        self.dirs[()].append(dir_entry(self.label, 0x08, 0, 0))

    def allocate(self, size):
        count = max(1, (size + self.cluster_sectors * SECTOR - 1) // (self.cluster_sectors * SECTOR))
        first = self.next_cluster
        if first + count > self.clusters + 2:
            raise ValueError('FAT volume full')
        end_mark = 0x0FFFFFFF if self.fat32 else 0xFFFF
        for c in range(first, first + count):
            self.fat[c] = c + 1 if c + 1 < first + count else end_mark
        self.next_cluster += count
        return first

    def cluster_offset(self, cluster):
        return (self.data_start + (cluster - 2) * self.cluster_sectors) * SECTOR

    def mkdir(self, path):
        parts = tuple(p for p in path.split('/') if p)
        for i in range(1, len(parts) + 1):
            sub = parts[:i]
            if sub in self.dirs:
                continue
            self.dirs[sub] = []
            # directories hold at most one cluster of entries here (16 per 512-byte cluster)
            self.dir_cluster[sub] = self.allocate(SECTOR * self.cluster_sectors)
            self.dirs[sub[:-1]].append(dir_entry(short_name(sub[-1]), 0x10, self.dir_cluster[sub], 0))

    def put(self, path, data):
        parts = tuple(p for p in path.split('/') if p)
        self.mkdir('/'.join(parts[:-1]))
        cluster = self.allocate(len(data)) if data else 0
        if data:
            offset = self.cluster_offset(cluster)
            self.image[offset:offset + len(data)] = data
        self.dirs[parts[:-1]].append(dir_entry(short_name(parts[-1]), 0x20, cluster, len(data)))

    def finish(self):
        for path, entries in self.dirs.items():
            if path:
                # '..' of a first-level directory names the root as cluster 0 (FAT16 and FAT32)
                parent = self.dir_cluster[path[:-1]] if path[:-1] else 0
                entries =[dir_entry(b'.          ', 0x10, self.dir_cluster[path], 0),
                           dir_entry(b'..         ', 0x10, parent, 0)] + entries
                capacity = self.cluster_sectors * SECTOR // 32
                offset = self.cluster_offset(self.dir_cluster[path])
            elif self.fat32:
                capacity = self.cluster_sectors * SECTOR // 32
                offset = self.cluster_offset(self.dir_cluster[()])
            else:
                capacity = self.root_entries
                offset = (self.reserved + 2 * self.fat_sectors) * SECTOR
            if len(entries) > capacity:
                raise ValueError('directory too large: /' + '/'.join(path))
            blob = b''.join(entries)
            self.image[offset:offset + len(blob)] = blob
        b = bytearray(SECTOR)
        b[0:3] = b'\xEB\x58\x90' if self.fat32 else b'\xEB\x3C\x90'
        b[3:11] = b'NEXUSOS '
        struct.pack_into('<HBHBHHBHHHII', b, 11, SECTOR, self.cluster_sectors, self.reserved, 2, self.root_entries,
                         self.sectors if self.sectors < 0x10000 and not self.fat32 else 0, 0xF8,
                         0 if self.fat32 else self.fat_sectors, 63, 255, self.hidden,
                         self.sectors if self.sectors >= 0x10000 or self.fat32 else 0)
        if self.fat32:
            struct.pack_into('<IHHIHH', b, 36, self.fat_sectors, 0, 0, self.dir_cluster[()], 1, 6)
            struct.pack_into('<BBBI11s8s', b, 64, 0x80, 0, 0x29, 0x4E455855, self.label, b'FAT32   ')
        else:
            struct.pack_into('<BBBI11s8s', b, 36, 0x80, 0, 0x29, 0x4E455855, self.label, b'FAT16   ')
        b[510:512] = b'\x55\xAA'
        self.image[0:SECTOR] = b
        if self.fat32:
            info = bytearray(SECTOR)
            struct.pack_into('<I', info, 0, 0x41615252)
            struct.pack_into('<III', info, 484, 0x61417272, self.clusters + 2 - self.next_cluster, self.next_cluster)
            info[510:512] = b'\x55\xAA'
            self.image[SECTOR:2 * SECTOR] = info
            self.image[6 * SECTOR:7 * SECTOR] = b
            self.image[7 * SECTOR:8 * SECTOR] = info
        fat = struct.pack(f'<{len(self.fat)}' + ('I' if self.fat32 else 'H'), *self.fat)
        for copy in range(2):
            offset = (self.reserved + copy * self.fat_sectors) * SECTOR
            self.image[offset:offset + len(fat)] = fat
        return bytes(self.image)


def fill(volume, loader, kernel, cmdline, kernel_sig=None):
    volume.put('EFI/BOOT/BOOTX64.EFI', loader)
    volume.put('EFI/NEXUS/KERNEL.BIN', kernel)
    if kernel_sig is not None:
        volume.put('EFI/NEXUS/KERNEL.SIG', kernel_sig)   # detached RSA-2048/SHA-256 signature (uefi/secureboot.py)
    if cmdline:
        volume.put('EFI/NEXUS/CMDLINE.TXT', cmdline.encode('ascii') + b'\n')
    volume.put('STARTUP.NSH', b'\\EFI\\BOOT\\BOOTX64.EFI\r\n')
    return volume.finish()


def gpt_disk(esp, total):
    first = 2048
    last = total - 34
    if first + len(esp) // SECTOR - 1 > last:
        raise ValueError('ESP does not fit the disk')
    disk_guid = uuid.uuid5(NAMESPACE, 'disk')
    part_guid = uuid.uuid5(NAMESPACE, 'esp')
    entries = bytearray(128 * 128)
    name = 'EFI System Partition'.encode('utf-16-le')
    struct.pack_into('<16s16sQQQ72s', entries, 0, ESP_TYPE.bytes_le, part_guid.bytes_le, first, last, 0, name)
    entries_crc = zlib.crc32(entries) & 0xFFFFFFFF

    def header(current, backup, entries_lba):
        h = bytearray(92)
        struct.pack_into('<8sIIIIQQQQ16sQIII', h, 0, b'EFI PART', 0x00010000, 92, 0, 0, current, backup,
                         34, last, disk_guid.bytes_le, entries_lba, 128, 128, entries_crc)
        struct.pack_into('<I', h, 16, zlib.crc32(h) & 0xFFFFFFFF)
        return bytes(h).ljust(SECTOR, b'\0')

    mbr = bytearray(SECTOR)
    struct.pack_into('<B3sB3sII', mbr, 446, 0, b'\x00\x02\x00', 0xEE, b'\xFF\xFF\xFF', 1, min(total - 1, 0xFFFFFFFF))
    mbr[510:512] = b'\x55\xAA'
    image = bytearray(total * SECTOR)
    image[0:SECTOR] = mbr
    image[SECTOR:2 * SECTOR] = header(1, total - 1, 2)
    image[2 * SECTOR:34 * SECTOR] = entries
    image[first * SECTOR:first * SECTOR + len(esp)] = esp
    image[(total - 33) * SECTOR:(total - 1) * SECTOR] = entries
    image[(total - 1) * SECTOR:total * SECTOR] = header(total - 1, 1, total - 33)
    return bytes(image)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('loader', type=Path)
    parser.add_argument('kernel', type=Path)
    parser.add_argument('image', type=Path)
    parser.add_argument('--cmdline', default='')
    parser.add_argument('--efiboot', type=Path)
    parser.add_argument('--size-mib', type=int, default=64)
    parser.add_argument('--kernel-sig', type=Path, help='detached KERNEL.SIG to stage on the ESP (secure boot)')
    args = parser.parse_args(argv)
    loader, kernel = args.loader.read_bytes(), args.kernel.read_bytes()
    if loader[:2] != b'MZ':
        raise SystemExit('loader is not a PE image')
    kernel_sig = args.kernel_sig.read_bytes() if args.kernel_sig else None
    total = args.size_mib * 1024 * 1024 // SECTOR
    esp_sectors = total - 34 - 2048 + 1
    esp = fill(Fat(esp_sectors, True, 'NEXUS ESP', hidden=2048), loader, kernel, args.cmdline, kernel_sig)
    args.image.write_bytes(gpt_disk(esp, total))
    print(f'[mkuefi] wrote {args.image}: GPT, ESP FAT32 {esp_sectors * SECTOR >> 20} MiB at LBA 2048'
          + (f', cmdline "{args.cmdline}"' if args.cmdline else ''))
    if args.efiboot:
        args.efiboot.write_bytes(fill(Fat(8192, False, 'NEXUS EFI'), loader, kernel, args.cmdline))
        print(f'[mkuefi] wrote {args.efiboot}: FAT16 El Torito EFI image')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
