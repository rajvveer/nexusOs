/* LUKS1/LUKS2 unlock and LUKS2 format (luks.c) for the encrypted root (cryptroot.c) and the installer (osinstall.c). */
#ifndef NEXUS_LUKS_H
#define NEXUS_LUKS_H
#include "blockdev.h"

struct luks_volume {
    int version, keyslot;                     /* 1 or 2; the keyslot that opened */
    uint8_t key[64]; unsigned key_bytes;      /* the volume key (wipe after activation) */
    uint64_t offset_sectors, size_sectors;    /* the data segment, 512-byte sectors */
    uint32_t sector_size; uint64_t iv_tweak;  /* dm-crypt data unit and IV offset */
    char cipher[64], uuid[40];
};
struct luks_format_options { uint32_t time, memory_kib, lanes, digest_iterations; };   /* 0: defaults (4, 256 MiB, 1, 200000) */

int luks_probe(const struct block_partition* part);   /* 1, 2 (LUKS version) or 0 */
/* 0 opened; -1 no keyslot accepts the passphrase; -61 not LUKS / no valid header; -95 unsupported (cipher, KDF,
 * hash, integrity); -22 corrupt metadata; -12 not enough memory (Argon2); -5 I/O */
int luks_unlock(const struct block_partition* part, const char* passphrase, unsigned passphrase_bytes, struct luks_volume* out);
int luks2_format(const struct block_partition* part, const char* passphrase, unsigned passphrase_bytes,
                 const struct luks_format_options* options, struct luks_volume* out);
int luks_activate(const struct block_partition* part, const struct luks_volume* volume, const char* name, struct block_device** out);
/* flags: LUKS_DISCARD (dm-crypt allow_discards), LUKS_READONLY */
enum { LUKS_DISCARD = 1, LUKS_READONLY = 2 };
int luks_activate_flags(const struct block_partition* part, const struct luks_volume* volume, const char* name, unsigned flags, struct block_device** out);
#endif
