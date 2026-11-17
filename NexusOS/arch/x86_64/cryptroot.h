/* Shared LUKS unlocking for the encrypted root (cryptroot.c) and /etc/crypttab (crypttab.c). */
#ifndef NEXUS_CRYPTROOT_H
#define NEXUS_CRYPTROOT_H
#include "blockdev.h"

struct crypt_request {
    const char* prefix;           /* serial marker: "[64] <prefix> PROMPT" ... ("CRYPTROOT", "CRYPTTAB data") */
    const char* heading, *intro;  /* the dialog: title, first line (" (LUKS2, sda1)." is appended) */
    const char* console_prompt;   /* without a framebuffer */
    unsigned tries;               /* passphrase attempts (0: 3) */
    bool fatal;                   /* power off when it cannot be opened (the root) */
    unsigned flags;               /* LUKS_DISCARD, LUKS_READONLY (luks.h) */
    const uint8_t* key; unsigned key_bytes;   /* a keyfile's bytes instead of the prompt */
};
int crypt_unlock(const struct block_partition* volume, const char* name, const struct crypt_request* r, struct block_device** dev);
struct block_partition* crypt_find_volume(const char* spec, unsigned* count);   /* spec NULL: the only typed LUKS volume */
extern const struct block_partition* cryptroot_volume;   /* the root's LUKS partition (NULL: not an encrypted root) */
#endif
