#!/usr/bin/env python3
"""
mkfat32.py — Create a minimal FAT32 disk image for NexusOS
Generates a 16MB raw disk image with valid FAT32 structures.
"""
import struct, sys, os

SECTOR = 512
DISK_SIZE = 16 * 1024 * 1024  # 16 MB
TOTAL_SECTORS = DISK_SIZE // SECTOR
SECTORS_PER_CLUSTER = 8  # 4KB clusters
RESERVED_SECTORS = 32
NUM_FATS = 2
ROOT_CLUSTER = 2

# Calculate FAT size
data_sectors = TOTAL_SECTORS - RESERVED_SECTORS
clusters = data_sectors // SECTORS_PER_CLUSTER
fat_entries_per_sector = SECTOR // 4  # 128
fat_sectors = (clusters + fat_entries_per_sector - 1) // fat_entries_per_sector
data_sectors = TOTAL_SECTORS - RESERVED_SECTORS - (NUM_FATS * fat_sectors)
clusters = data_sectors // SECTORS_PER_CLUSTER

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'disk.img')
img = bytearray(DISK_SIZE)

# --- BPB (Boot Sector) ---
bpb = bytearray(SECTOR)
bpb[0:3] = b'\xEB\x58\x90'          # Jump + NOP
bpb[3:11] = b'NEXUSOS '             # OEM name
struct.pack_into('<H', bpb, 11, SECTOR)           # bytes_per_sector
bpb[13] = SECTORS_PER_CLUSTER                      # sectors_per_cluster
struct.pack_into('<H', bpb, 14, RESERVED_SECTORS)  # reserved_sectors
bpb[16] = NUM_FATS                                 # num_fats
struct.pack_into('<H', bpb, 17, 0)                 # root_entry_count (0 for FAT32)
struct.pack_into('<H', bpb, 19, 0)                 # total_sectors_16 (0 for FAT32)
bpb[21] = 0xF8                                     # media_type (hard disk)
struct.pack_into('<H', bpb, 22, 0)                 # fat_size_16 (0 for FAT32)
struct.pack_into('<H', bpb, 24, 63)                # sectors_per_track
struct.pack_into('<H', bpb, 26, 16)                # num_heads
struct.pack_into('<I', bpb, 28, 0)                 # hidden_sectors
struct.pack_into('<I', bpb, 32, TOTAL_SECTORS)     # total_sectors_32

# FAT32 extended BPB
struct.pack_into('<I', bpb, 36, fat_sectors)       # fat_size_32
struct.pack_into('<H', bpb, 40, 0)                 # ext_flags
struct.pack_into('<H', bpb, 42, 0)                 # fs_version
struct.pack_into('<I', bpb, 44, ROOT_CLUSTER)      # root_cluster
struct.pack_into('<H', bpb, 48, 1)                 # fs_info sector
struct.pack_into('<H', bpb, 50, 6)                 # backup_boot sector
bpb[64] = 0x80                                     # drive_number
bpb[66] = 0x29                                     # boot_sig
struct.pack_into('<I', bpb, 67, 0x12345678)        # volume_id
bpb[71:82] = b'NEXUSOS    '                        # volume_label (11 bytes)
bpb[82:90] = b'FAT32   '                           # fs_type
bpb[510] = 0x55                                    # signature
bpb[511] = 0xAA

img[0:SECTOR] = bpb

# --- FSInfo sector (sector 1) ---
fsinfo = bytearray(SECTOR)
struct.pack_into('<I', fsinfo, 0, 0x41615252)      # lead_sig
struct.pack_into('<I', fsinfo, 484, 0x61417272)     # struc_sig
struct.pack_into('<I', fsinfo, 488, clusters - 1)    # free_count
struct.pack_into('<I', fsinfo, 492, 3)              # next_free
fsinfo[510] = 0x55
fsinfo[511] = 0xAA
img[SECTOR:2*SECTOR] = fsinfo

# --- Backup boot sector (sector 6) ---
img[6*SECTOR:7*SECTOR] = bpb

# --- FAT tables ---
fat_offset_1 = RESERVED_SECTORS * SECTOR
fat_offset_2 = fat_offset_1 + fat_sectors * SECTOR

# FAT32 first 3 entries
# Entry 0: media type, Entry 1: EOC, Entry 2: EOC (root dir cluster)
fat_entries = bytearray(SECTOR)
struct.pack_into('<I', fat_entries, 0, 0x0FFFFFF8)   # Entry 0: media
struct.pack_into('<I', fat_entries, 4, 0x0FFFFFFF)   # Entry 1: EOC
struct.pack_into('<I', fat_entries, 8, 0x0FFFFFFF)   # Entry 2: root dir EOC

img[fat_offset_1:fat_offset_1+SECTOR] = fat_entries
img[fat_offset_2:fat_offset_2+SECTOR] = fat_entries

# --- Root directory (cluster 2) ---
data_start = (RESERVED_SECTORS + NUM_FATS * fat_sectors) * SECTOR
root_offset = data_start  # Cluster 2 is the first data cluster

# Create a volume label entry
vol_entry = bytearray(32)
vol_entry[0:11] = b'NEXUSOS    '
vol_entry[11] = 0x08  # ATTR_VOLUME_ID

# Create a welcome file entry
welcome = bytearray(32)
welcome[0:11] = b'README  TXT'  # 8.3 name
welcome[11] = 0x20  # ATTR_ARCHIVE
welcome_cluster = 3
struct.pack_into('<H', welcome, 20, (welcome_cluster >> 16) & 0xFFFF)
struct.pack_into('<H', welcome, 26, welcome_cluster & 0xFFFF)
welcome_text = b'Welcome to NexusOS!\nThis is the FAT32 persistent disk.\nFiles saved here survive reboots.\n'
struct.pack_into('<I', welcome, 28, len(welcome_text))

# Write directory entries
img[root_offset:root_offset+32] = vol_entry
img[root_offset+32:root_offset+64] = welcome

# Allocate cluster 3 for the welcome file in FAT
struct.pack_into('<I', fat_entries, 12, 0x0FFFFFFF)  # Entry 3: EOC
img[fat_offset_1:fat_offset_1+SECTOR] = fat_entries
img[fat_offset_2:fat_offset_2+SECTOR] = fat_entries

# Write welcome file content to cluster 3
welcome_offset = data_start + SECTORS_PER_CLUSTER * SECTOR  # cluster 3
img[welcome_offset:welcome_offset+len(welcome_text)] = welcome_text

# Write output
with open(out, 'wb') as f:
    f.write(img)

print(f'[OK] Created FAT32 disk image: {out} ({DISK_SIZE // 1024 // 1024} MB, {clusters} clusters)')
