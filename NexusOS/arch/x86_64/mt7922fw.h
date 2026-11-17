/* MT7922 firmware image layout (mt7922fw.c) - groundwork for a future Wi-Fi driver; not linked into the kernel. */
#ifndef NEXUS_MT7922FW_H
#define NEXUS_MT7922FW_H
#include "../../kernel/types.h"

#define MT7922_PATCH_REGIONS 8
#define MT7922_RAM_REGIONS 16
enum { MT7922_FW_TRUNCATED = -1, MT7922_FW_REGIONS = -2, MT7922_FW_SECTION_TYPE = -3, MT7922_FW_BOUNDS = -4, MT7922_FW_CHIP = -5 };
struct mt7922_patch_info {
    char build[17], platform[5]; uint32_t hw_sw_ver, patch_ver, regions;
    struct { uint32_t addr, len, offset, sec_info; } region[MT7922_PATCH_REGIONS];
};
struct mt7922_ram_info {
    uint8_t chip, eco, regions, format; char version[11], build[16]; uint64_t payload;
    struct { uint32_t addr, len, offset; uint8_t feature; } region[MT7922_RAM_REGIONS];   /* feature bit 6: not downloaded */
};
int mt7922_patch_parse(const uint8_t* data, uint64_t size, struct mt7922_patch_info* out);   /* 0 or MT7922_FW_* */
int mt7922_ram_parse(const uint8_t* data, uint64_t size, struct mt7922_ram_info* out);
#endif
