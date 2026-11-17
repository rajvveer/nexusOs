"""Bootable NexusOS USB stick / disk image for BIOS and UEFI PCs (build.py usb-image).

    python -I arch/x86_64/build.py usb-image OUT.img [--size 8G] [--root-from IMAGE] [--cmdline "..."]
    python -I arch/x86_64/usbimage.py OUT.img [same options]          (uses the existing out/ build)

OUT.img is a raw GPT disk (512-byte sectors), written sparse:
  LBA 0        MBR whose boot code is usbboot.asm: E820 map, then stage 2 by INT 13h LBA reads; by default a hybrid
               MBR (0xEE over the GPT, ESP 0xEF, root active) so BIOSes that want an active partition boot it too
               (--mbr hybrid|active|protective, see mbr_entries)
  LBA 1-33     primary GPT (new random disk and partition GUIDs per image)
  p1 LBA 2048  "BIOS boot" partition, 8 MiB: boot/boot2.asm (kernel_lba and the NXCMDLINE command line patched)
               followed by kernel.bin; its last sector is the "NXBOOTD1" descriptor (where CMDLINE.TXT lives)
  p2 LBA 18432 EFI System Partition, FAT32 64 MiB: EFI/BOOT/BOOTX64.EFI, EFI/NEXUS/KERNEL.BIN,
               EFI/NEXUS/CMDLINE.TXT (one 512-byte sector), STARTUP.NSH
  p3           "NexusOS" root (type 7D1B4F2C-3A86-4E59-9C0D-4E45585553F1): the root filesystem of --root-from,
               copied block for block (FAT32 hidden-sectors fields updated); FAT32 or ext4
  end          backup GPT; space beyond p3 (--size) stays unpartitioned
Both boot paths pass the kernel "root=PARTUUID=<p3> rootwait" (+ --cmdline): the kernel picks exactly this
partition, waiting for USB enumeration (blockdev.c block_root_late, fsprobe.c block_root_wanted).
--root-from: a disk image whose first FAT32/ext4 partition (MBR or GPT) or whole-disk filesystem becomes the
root; default out/desktop64.img (the installed Aurora desktop). Tests use out/apps64.img.

Writing it to a stick: Linux `sudo dd if=OUT.img of=/dev/sdX bs=4M conv=fsync status=progress` (sdX = the whole
stick, not a partition); Windows: Rufus, select the image, "DD Image" mode when asked; macOS: `sudo dd
if=OUT.img of=/dev/rdiskN bs=4m`. Booting the stick, then `nexus-os-install nvme0n1` (or the disk shown by it)
and a restart copy the system to the internal disk (arch/x86_64/osinstall.c).
"""
from pathlib import Path
import argparse
import importlib.util
import os
import struct
import subprocess
import sys
import uuid
import zlib

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = HERE / 'out'
SECTOR = 512
BIOS_BOOT = uuid.UUID('21686148-6449-6E6F-744E-656564454649')
ESP = uuid.UUID('C12A7328-F81F-11D2-BA4B-00A0C93EC93B')
NEXUS_ROOT = uuid.UUID('7D1B4F2C-3A86-4E59-9C0D-4E45585553F1')
LINUX_DATA = uuid.UUID('0FC63DAF-8483-4772-8E79-3D69D8477DE4')
BIOS_START, BIOS_SECTORS = 2048, 16384   # 8 MiB: stage 2 + kernel.bin (<= linker.ld's 6 MiB image) + descriptor (was 2 MiB)
ESP_START, ESP_SECTORS = BIOS_START + BIOS_SECTORS, 131072
ROOT_START = ESP_START + ESP_SECTORS


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value); return value


def size_value(text):
    units = {'K': 1 << 10, 'M': 1 << 20, 'G': 1 << 30, 'T': 1 << 40}
    text = text.strip().upper().rstrip('B').rstrip('I')
    return int(float(text[:-1]) * units[text[-1]]) if text and text[-1] in units else int(text)


def find_root(image):
    """(offset in bytes, size in bytes, 'vfat'|'ext4') of the root filesystem inside a disk image file."""
    with open(image, 'rb') as f:
        mbr = f.read(SECTOR)
        candidates = []
        f.seek(SECTOR); header = f.read(SECTOR)
        if header[:8] == b'EFI PART':
            entries_lba, count, size = struct.unpack_from('<QII', header, 72)
            f.seek(entries_lba * SECTOR); table = f.read(count * size)
            for i in range(count):
                e = table[i * size:(i + 1) * size]
                first, last = struct.unpack_from('<QQ', e, 32)
                if e[:16] != bytes(16): candidates.append((first * SECTOR, (last - first + 1) * SECTOR))
        elif mbr[510:512] == b'\x55\xaa':
            for i in range(4):
                kind = mbr[446 + 16 * i + 4]; start, count = struct.unpack_from('<II', mbr, 446 + 16 * i + 8)
                if kind and kind != 0xEE and count: candidates.append((start * SECTOR, count * SECTOR))
        candidates.append((0, os.path.getsize(image)))
        for offset, size in candidates:
            f.seek(offset); boot = f.read(4096)
            if len(boot) < 4096: continue
            if boot[510:512] == b'\x55\xaa' and boot[82:87] == b'FAT32' and struct.unpack_from('<H', boot, 11)[0] == 512:
                total = struct.unpack_from('<I', boot, 32)[0]
                return offset, total * SECTOR, 'vfat'
            if boot[1024 + 56:1024 + 58] == b'\x53\xef':
                return offset, size, 'ext4'
    raise ValueError(f'{image}: no FAT32 or ext4 root filesystem found')


def root_extents(source, offset, sectors, kind):
    """(first sector, count) runs of the root filesystem worth copying: for FAT32 the reserved area, the FATs and
    the allocated clusters (free clusters stay unwritten, so the image is smaller and faster to write to a stick)."""
    if kind != 'vfat':
        yield 0, sectors; return
    source.seek(offset); bs = source.read(SECTOR)
    per, reserved, fats = bs[13], struct.unpack_from('<H', bs, 14)[0], bs[16]
    total, fat_size = struct.unpack_from('<I', bs, 32)[0], struct.unpack_from('<I', bs, 36)[0]
    data = reserved + fats * fat_size
    yield 0, data
    source.seek(offset + reserved * SECTOR); table = source.read(fat_size * SECTOR)
    clusters = (total - data) // per
    entries = struct.unpack_from('<%dI' % min(clusters + 2, len(table) // 4), table)
    run = None
    for c in range(2, min(clusters + 2, len(entries))):
        if entries[c] & 0x0FFFFFFF:
            if run and run[0] + run[1] == c and run[1] * per < 2048: run[1] += 1; continue
            if run: yield data + (run[0] - 2) * per, run[1] * per
            run = [c, 1]
        elif run: yield data + (run[0] - 2) * per, run[1] * per; run = None
    if run: yield data + (run[0] - 2) * per, run[1] * per


def sparse_file(path, size):
    if path.exists(): path.unlink()
    path.parent.mkdir(parents=True, exist_ok=True); path.touch()
    if os.name == 'nt': subprocess.run(['fsutil', 'sparse', 'setflag', str(path)], check=True, stdout=subprocess.DEVNULL)
    with path.open('r+b') as f: f.truncate(size)


def gpt(total, parts, disk_guid):
    """Primary (LBA 1..33) and backup GPT for parts = [(type uuid, unique uuid, first, last, name)]."""
    entries = bytearray(128 * 128)
    for i, (kind, unique, first, last, name) in enumerate(parts):
        struct.pack_into('<16s16sQQQ72s', entries, i * 128, kind.bytes_le, unique.bytes_le, first, last, 0, name.encode('utf-16-le'))
    crc = zlib.crc32(entries) & 0xFFFFFFFF

    def header(current, backup, table):
        h = bytearray(92)
        struct.pack_into('<8sIIIIQQQQ16sQIII', h, 0, b'EFI PART', 0x00010000, 92, 0, 0, current, backup, 34, total - 34,
                         disk_guid.bytes_le, table, 128, 128, crc)
        struct.pack_into('<I', h, 16, zlib.crc32(h) & 0xFFFFFFFF)
        return bytes(h).ljust(SECTOR, b'\0')
    return header(1, total - 1, 2), bytes(entries), header(total - 1, 1, total - 33)


def installer_program():
    """out/osinstall_user.elf (nexus-os-install), compiled with the pinned toolchain when missing or stale."""
    source, elf = HERE / 'osinstall_user.c', OUT / 'osinstall_user.elf'
    if not elf.is_file() or elf.stat().st_mtime < source.stat().st_mtime:
        build = module('usbimage_build', HERE / 'build.py')
        build.run(build.compiler(), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror', source, '-o', elf)
    return elf


MBR_MODES = ('hybrid', 'active', 'protective')
def mbr_entries(mode, total, root_sectors, root_type):
    """The MBR partition entries (status, type, first LBA, sectors). The GPT stays authoritative in every mode: an 0xEE
    entry starting at LBA 1 is what UEFI firmware (EDK2 PartitionDxe), Linux, Windows and NexusOS look for.
      hybrid      (default) 0xEE over LBA 1..2047 (the GPT), the ESP as 0xEF and the root (0x0C FAT32 / 0x83) marked
                  active: what BIOSes that only boot a disk with an active MBR partition need (the isohybrid / Rufus
                  convention); the boot code never reads the entries
      active      the single protective entry, marked active (0x80)
      protective  the single protective entry as the UEFI specification writes it (some BIOSes skip such a stick)"""
    if mode == 'protective': return [(0x00, 0xEE, 1, total - 1)]
    if mode == 'active': return [(0x80, 0xEE, 1, total - 1)]
    if mode != 'hybrid': raise ValueError('mbr mode ' + mode)
    return [(0x00, 0xEE, 1, BIOS_START - 1), (0x00, 0xEF, ESP_START, ESP_SECTORS),
            (0x80, 0x0C if root_type == 'vfat' else 0x83, ROOT_START, root_sectors)]


def build_image(output, size=None, root_from=None, cmdline='', boot_code=None, stage2=None, kernel=None, loader=None, mbr_mode='hybrid'):
    output = Path(output)
    boot_code = Path(boot_code or OUT / 'usbboot.bin').read_bytes()
    stage2 = bytearray(Path(stage2 or OUT / 'boot2.bin').read_bytes())
    kernel = Path(kernel or OUT / 'kernel.bin').read_bytes()
    loader = Path(loader or OUT / 'BOOTX64.EFI').read_bytes()
    root_from = Path(root_from or OUT / 'desktop64.img')
    if not root_from.is_file(): raise SystemExit(f'{root_from} not found: pass --root-from (an installed desktop64.img, or out/apps64.img)')
    assert len(boot_code) == 440 and len(stage2) == 2048 and loader[:2] == b'MZ'
    if len(kernel) > (BIOS_SECTORS - 5) * SECTOR: raise ValueError('kernel.bin does not fit the BIOS boot partition')
    root_offset, root_bytes, root_type = find_root(root_from)
    root_sectors = (root_bytes + SECTOR - 1) // SECTOR
    minimum = ROOT_START + root_sectors + 33 + 1
    total = max(minimum, (size_value(size) if size else 0) // SECTOR)
    total = (total + 2047) // 2048 * 2048
    root_part = uuid.uuid4()
    line = ('root=PARTUUID=' + str(root_part) + ' rootwait ' + cmdline).strip()
    if len(line) > 229: raise ValueError('command line too long')
    # stage 2: kernel right after it in the BIOS boot partition; command line at 0x700
    struct.pack_into('<I', stage2, 0x7F0, BIOS_START + 4)
    stage2[0x700:0x7F0] = (b'NXCMDLINE=' + line.encode('ascii')).ljust(0xF0, b'\0')
    # ESP: CMDLINE.TXT is exactly one data sector (the installer rewrites it in place)
    mkuefi = module('usbimage_mkuefi', ROOT / 'mkuefi.py')
    esp = mkuefi.Fat(ESP_SECTORS, True, 'NEXUS ESP', hidden=ESP_START)
    cmdline_data = (line.encode('ascii') + b' ' * 512)[:511] + b'\n'
    esp.put('EFI/BOOT/BOOTX64.EFI', loader)
    esp.put('EFI/NEXUS/KERNEL.BIN', kernel)
    esp.put('EFI/NEXUS/CMDLINE.TXT', cmdline_data)
    esp.put('STARTUP.NSH', b'\\EFI\\BOOT\\BOOTX64.EFI\r\n')
    esp_bytes = esp.finish()
    found = [i for i in range(0, len(esp_bytes), SECTOR) if esp_bytes[i:i + SECTOR] == cmdline_data]
    if len(found) != 1: raise ValueError('CMDLINE.TXT sector not unique in the ESP')
    descriptor = bytearray(SECTOR)
    struct.pack_into('<8sII', descriptor, 0, b'NXBOOTD1', 1, found[0] // SECTOR)
    parts = [(BIOS_BOOT, uuid.uuid4(), BIOS_START, BIOS_START + BIOS_SECTORS - 1, 'BIOS boot'),
             (ESP, uuid.uuid4(), ESP_START, ESP_START + ESP_SECTORS - 1, 'EFI System Partition'),
             (NEXUS_ROOT, root_part, ROOT_START, ROOT_START + root_sectors - 1, 'NexusOS')]
    primary, entries, backup = gpt(total, parts, uuid.uuid4())
    mbr = bytearray(SECTOR); mbr[:440] = boot_code
    struct.pack_into('<I', mbr, 0x1A8, BIOS_START)   # usbboot.asm stage2_lba (disk address packet)
    mbr[440:444] = os.urandom(4)
    for i, (status, kind, first, count) in enumerate(mbr_entries(mbr_mode, total, root_sectors, root_type)):
        struct.pack_into('<B3sB3sII', mbr, 446 + 16 * i, status, b'\x00\x02\x00' if kind == 0xEE else b'\xfe\xff\xff', kind, b'\xff\xff\xff',
                         first, min(count, 0xFFFFFFFF))
    mbr[510:512] = b'\x55\xaa'
    sparse_file(output, total * SECTOR)
    with output.open('r+b') as f, root_from.open('rb') as source:
        f.write(mbr); f.write(primary); f.write(entries)
        f.seek(BIOS_START * SECTOR); f.write(bytes(stage2)); f.write(kernel)
        f.seek((BIOS_START + BIOS_SECTORS - 1) * SECTOR); f.write(descriptor)
        f.seek(ESP_START * SECTOR); f.write(esp_bytes)
        for start, count in root_extents(source, root_offset, root_sectors, root_type):
            source.seek(root_offset + start * SECTOR); at = (ROOT_START + start) * SECTOR; left = count * SECTOR
            while left:
                block = source.read(min(left, 1 << 20))
                if not block: break
                if at == ROOT_START * SECTOR and root_type == 'vfat':   # hidden sectors (BPB 28) in the boot sector and its backup
                    block = bytearray(block); backup_sector = struct.unpack_from('<H', block, 50)[0]
                    struct.pack_into('<I', block, 28, ROOT_START)
                    if backup_sector and backup_sector * SECTOR + 32 <= len(block): struct.pack_into('<I', block, backup_sector * SECTOR + 28, ROOT_START)
                    block = bytes(block)
                if block.strip(b'\0'): f.seek(at); f.write(block)
                at += len(block); left -= len(block)
        f.seek((total - 33) * SECTOR); f.write(entries); f.write(backup)
    if root_type == 'vfat':   # the installer command on the stick's system (FAT32 roots; fatput finds the GPT root)
        fatput = module('usbimage_fatput', ROOT / 'userland/fatput.py')
        if fatput.edit(['fatput', str(output), 'put', str(installer_program()), 'usr/bin/nexus-os-install']) != 0:
            raise ValueError('could not add /usr/bin/nexus-os-install')
    print(f'[usb-image] wrote {output}: {total * SECTOR / (1 << 30):.2f} GiB GPT, BIOS boot + ESP 64 MiB + root {root_type} '
          f'{root_sectors * SECTOR >> 20} MiB from {root_from.name}; kernel "{line}"', flush=True)
    return {'image': str(output), 'sectors': total, 'root_partuuid': str(root_part), 'cmdline': line, 'root_type': root_type,
            'root_start': ROOT_START, 'root_sectors': root_sectors}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('output', type=Path)
    parser.add_argument('--size', help='image size (e.g. 8G); default: just large enough')
    parser.add_argument('--root-from', type=Path, help='disk image holding the root filesystem (default out/desktop64.img)')
    parser.add_argument('--cmdline', default='', help='extra kernel options')
    parser.add_argument('--mbr', default='hybrid', choices=MBR_MODES, help='MBR partition entries (see mbr_entries)')
    args = parser.parse_args(argv)
    build_image(args.output, args.size, args.root_from, args.cmdline, mbr_mode=args.mbr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
