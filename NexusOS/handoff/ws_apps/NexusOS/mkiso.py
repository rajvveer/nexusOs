#!/usr/bin/env python3
"""
mkiso.py — build a bootable ISO9660 image of NexusOS (Phase 50).

Produces nexus.iso with an El Torito boot catalog using **floppy emulation**
(2.88/1.44 MB), pointing at the 1.44 MB nexus.img floppy image. A BIOS that
boots the ISO will emulate it as drive A: and run the exact same Stage-1
bootloader the floppy uses — so no separate boot path is needed.

This is a minimal, dependency-free ISO9660 writer: a Primary Volume Descriptor,
a Boot Record Volume Descriptor (El Torito), the boot catalog, a tiny root
directory, and the boot image stored as a file. It is intentionally small and
readable rather than a full ISO9660 implementation.

Usage:  python mkiso.py [boot_image] [output.iso]
        defaults: nexus.img -> nexus.iso
"""

import sys, os, struct

SECTOR = 2048

def lsb_msb_u32(v):
    return struct.pack('<I', v) + struct.pack('>I', v)

def lsb_msb_u16(v):
    return struct.pack('<H', v) + struct.pack('>H', v)

def both_endian_dir_u32(v):
    return struct.pack('<I', v) + struct.pack('>I', v)

def pad_to_sector(data):
    rem = len(data) % SECTOR
    if rem:
        data += b'\x00' * (SECTOR - rem)
    return data

def main():
    boot_img = sys.argv[1] if len(sys.argv) > 1 else 'nexus.img'
    out_iso  = sys.argv[2] if len(sys.argv) > 2 else 'nexus.iso'

    if not os.path.exists(boot_img):
        print(f"[mkiso] boot image not found: {boot_img}")
        return 1

    with open(boot_img, 'rb') as f:
        boot_data = f.read()

    boot_sectors_2048 = (len(boot_data) + SECTOR - 1) // SECTOR

    # Choose El Torito floppy emulation type from the image size.
    n512 = len(boot_data) // 512
    if n512 <= 2880:
        media_type = 0x02            # 1.44 MB floppy emulation
    elif n512 <= 5760:
        media_type = 0x03            # 2.88 MB floppy emulation
    else:
        media_type = 0x00            # no emulation (fallback)

    # ---- Fixed sector layout -------------------------------------------------
    # 0..15  : system area (zeros)
    # 16     : Primary Volume Descriptor
    # 17     : Boot Record Volume Descriptor (El Torito)
    # 18     : Volume Descriptor Set Terminator
    # 19     : root directory
    # 20     : boot catalog
    # 21..   : boot image
    LBA_PVD   = 16
    LBA_BRVD  = 17
    LBA_TERM  = 18
    LBA_ROOT  = 19
    LBA_CAT   = 20
    LBA_BOOT  = 21
    total_sectors = LBA_BOOT + boot_sectors_2048

    def vd_header(vtype, ident=b'CD001', ver=1):
        return bytes([vtype]) + ident + bytes([ver])

    # ---- Root directory record (used inside the PVD) ------------------------
    def dir_record(lba, length, flags, name):
        rec = bytearray()
        rec += b'\x00'                       # length placeholder
        rec += b'\x00'                       # ext attr length
        rec += both_endian_dir_u32(lba)
        rec += both_endian_dir_u32(length)
        rec += bytes(7)                      # date/time (zeroed)
        rec += bytes([flags])
        rec += b'\x00\x00'                   # unit size / interleave
        rec += lsb_msb_u16(1)                # volume sequence number
        rec += bytes([len(name)])
        rec += name
        if len(rec) % 2:                     # pad to even
            rec += b'\x00'
        rec[0] = len(rec)
        return bytes(rec)

    root_dir_record = dir_record(LBA_ROOT, SECTOR, 0x02, b'\x00')

    # ---- Primary Volume Descriptor (sector 16) -----------------------------
    pvd = bytearray(SECTOR)
    pvd[0:7] = vd_header(1)
    def putstr(buf, off, s, n):
        s = s.encode('ascii')[:n].ljust(n, b' ')
        buf[off:off+n] = s
    putstr(pvd, 8, '', 32)                    # system id
    putstr(pvd, 40, 'NEXUSOS', 32)            # volume id
    pvd[80:88]  = lsb_msb_u32(total_sectors)  # volume space size
    pvd[120:124] = lsb_msb_u16(1)             # volume set size
    pvd[124:128] = lsb_msb_u16(1)             # volume sequence number
    pvd[128:132] = lsb_msb_u16(SECTOR)        # logical block size
    # path table size + locations left zero (minimal reader uses the root record)
    pvd[156:156+len(root_dir_record)] = root_dir_record
    putstr(pvd, 318, 'NEXUSOS', 128)          # publisher
    putstr(pvd, 446, 'MKISO.PY', 128)         # data preparer
    pvd[881] = 1                              # file structure version

    # ---- Boot Record Volume Descriptor / El Torito (sector 17) -------------
    brvd = bytearray(SECTOR)
    brvd[0:7] = vd_header(0)
    brvd[7:7+len(b'EL TORITO SPECIFICATION')] = b'EL TORITO SPECIFICATION'
    brvd[0x47:0x4B] = struct.pack('<I', LBA_CAT)   # boot catalog LBA

    # ---- Volume Descriptor Set Terminator (sector 18) ----------------------
    term = bytearray(SECTOR)
    term[0:7] = vd_header(0xFF)

    # ---- Root directory (sector 19): '.' and '..' --------------------------
    root = bytearray(SECTOR)
    r1 = dir_record(LBA_ROOT, SECTOR, 0x02, b'\x00')          # '.'
    r2 = dir_record(LBA_ROOT, SECTOR, 0x02, b'\x01')          # '..'
    boot_file = dir_record(LBA_BOOT, len(boot_data), 0x00, b'BOOT.IMG;1')
    off = 0
    for r in (r1, r2, boot_file):
        root[off:off+len(r)] = r
        off += len(r)

    # ---- Boot catalog (sector 20) ------------------------------------------
    cat = bytearray(SECTOR)
    # Validation entry
    val = bytearray(32)
    val[0] = 0x01                 # header id
    val[1] = 0x00                 # platform: 80x86
    val[28:30] = b'\x00\x00'      # key bytes go at 30..31
    val[30] = 0x55
    val[31] = 0xAA
    # checksum: 16-bit words sum to 0
    s = 0
    for i in range(0, 32, 2):
        s += val[i] | (val[i+1] << 8)
    checksum = (-s) & 0xFFFF
    val[28] = checksum & 0xFF
    val[29] = (checksum >> 8) & 0xFF
    cat[0:32] = val
    # Initial/default entry
    ent = bytearray(32)
    ent[0] = 0x88                 # bootable
    ent[1] = media_type          # emulation type
    ent[2:4] = struct.pack('<H', 0)            # load segment (0 = default 0x7C0)
    ent[4] = 0                    # system type
    ent[6:8] = struct.pack('<H', 1)            # sector count (virtual 512 sectors)
    ent[8:12] = struct.pack('<I', LBA_BOOT)    # boot image LBA (2048-byte units)
    cat[32:64] = ent

    # ---- Assemble ----------------------------------------------------------
    img = bytearray(SECTOR * LBA_BOOT)
    img[SECTOR*LBA_PVD :SECTOR*LBA_PVD +SECTOR] = pvd
    img[SECTOR*LBA_BRVD:SECTOR*LBA_BRVD+SECTOR] = brvd
    img[SECTOR*LBA_TERM:SECTOR*LBA_TERM+SECTOR] = term
    img[SECTOR*LBA_ROOT:SECTOR*LBA_ROOT+SECTOR] = root
    img[SECTOR*LBA_CAT :SECTOR*LBA_CAT +SECTOR] = cat
    img += pad_to_sector(bytearray(boot_data))

    with open(out_iso, 'wb') as f:
        f.write(img)

    print(f"[mkiso] wrote {out_iso}: {len(img)} bytes "
          f"({total_sectors} x 2048), El Torito media 0x{media_type:02X}, "
          f"boot image {boot_img} ({len(boot_data)} bytes)")
    return 0

if __name__ == '__main__':
    sys.exit(main())
