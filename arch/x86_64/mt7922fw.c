/* MediaTek MT7922 (Filogic 330, PCI 14c3:0616) Wi-Fi firmware image layout - GROUNDWORK ONLY.
 *
 * Not linked into the kernel: there is no MT7922 driver yet (end-to-end Wi-Fi/Bluetooth needs the hardware).
 * This file is the first, host-verifiable piece of one: it validates the two "connac2" images a driver
 * downloads to the WM MCU (layouts written from the published image format, as documented by the Linux mt76
 * driver's struct definitions; tests/test_mt7922fw.py checks it against synthetic and hostile images and,
 * with --firmware DIR, against the pinned linux-firmware files listed under "inventory" in firmware-lock.json):
 *
 *   WIFI_MT7922_patch_mcu_1_1_hdr.bin  ROM patch: 96-byte big-endian header (build date, platform "ALPS",
 *                                      hw/sw version, patch version, descriptor with n_region), then n_region
 *                                      64-byte section headers (type 2 = info: target address, length, key
 *                                      index/encryption), each section's payload at its file offset.
 *   WIFI_RAM_CODE_MT7922_1.bin         WM RAM code: payload regions back to back from offset 0, then n_region
 *                                      40-byte little-endian region descriptors (address, length, feature set:
 *                                      bit 6 = not downloaded, bit 5 = start address override), then a 36-byte
 *                                      trailer (chip id 0x10, eco, n_region, format, version, build date).
 *
 * The download itself (patch semaphore, FW_SCATTER through the WFDMA TX ring 16, start patch/RAM commands) is
 * described in docs/DEVELOPMENT_NOTES.md; it cannot be simulated faithfully without the MCU, so it is not written yet. */
#include "mt7922fw.h"

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t le32(const uint8_t* p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
static void text(char* out, const uint8_t* in, unsigned length) {
    unsigned n = 0;
    for (unsigned i = 0; i < length && in[i] && in[i] != '\n'; i++) out[n++] = in[i] >= 0x20 && in[i] < 0x7F ? (char)in[i] : '?';
    out[n] = 0;
}

#define PATCH_HEADER 96U
#define PATCH_SECTION 64U
int mt7922_patch_parse(const uint8_t* data, uint64_t size, struct mt7922_patch_info* out) {
    if (!data || size < PATCH_HEADER) return MT7922_FW_TRUNCATED;
    text(out->build, data, 16); text(out->platform, data + 16, 4);
    out->hw_sw_ver = be32(data + 20); out->patch_ver = be32(data + 24);
    out->regions = be32(data + 32 + 12);                                          /* desc.n_region */
    if (!out->regions || out->regions > MT7922_PATCH_REGIONS) return MT7922_FW_REGIONS;
    if (size < PATCH_HEADER + (uint64_t)out->regions * PATCH_SECTION) return MT7922_FW_TRUNCATED;
    for (uint32_t i = 0; i < out->regions; i++) {
        const uint8_t* s = data + PATCH_HEADER + i * PATCH_SECTION;
        if ((be32(s) & 0xFFFF) != 2) return MT7922_FW_SECTION_TYPE;               /* PATCH_SEC_TYPE_INFO */
        uint32_t offset = be32(s + 4), length = be32(s + 16);                      /* offs, info.len */
        if (offset < PATCH_HEADER + out->regions * PATCH_SECTION || offset > size || length > size - offset) return MT7922_FW_BOUNDS;
        out->region[i].addr = be32(s + 12); out->region[i].len = length; out->region[i].offset = offset;   /* info.addr */
        out->region[i].sec_info = be32(s + 20);                                    /* info.sec_key_idx */
    }
    return 0;
}

#define RAM_TRAILER 36U
#define RAM_REGION 40U
int mt7922_ram_parse(const uint8_t* data, uint64_t size, struct mt7922_ram_info* out) {
    if (!data || size < RAM_TRAILER) return MT7922_FW_TRUNCATED;
    const uint8_t* t = data + size - RAM_TRAILER;
    out->chip = t[0]; out->eco = t[1]; out->regions = t[2]; out->format = t[3];
    text(out->version, t + 7, 10); text(out->build, t + 17, 15);
    if (out->chip != 0x10) return MT7922_FW_CHIP;
    if (!out->regions || out->regions > MT7922_RAM_REGIONS) return MT7922_FW_REGIONS;
    uint64_t table = (uint64_t)out->regions * RAM_REGION;
    if (size < RAM_TRAILER + table) return MT7922_FW_TRUNCATED;
    uint64_t payload_end = size - RAM_TRAILER - table, offset = 0;
    for (unsigned i = 0; i < out->regions; i++) {
        const uint8_t* r = t - table + (uint64_t)i * RAM_REGION;
        uint32_t length = le32(r + 20);
        if (length > payload_end - offset) return MT7922_FW_BOUNDS;
        out->region[i].addr = le32(r + 16); out->region[i].len = length; out->region[i].offset = (uint32_t)offset;
        out->region[i].feature = r[24];
        offset += length;                                                         /* skipped (non-DL) regions still occupy space */
    }
    out->payload = offset;
    return 0;
}
