/* Host test of arch/x86_64/mt7922fw.c: synthetic ROM-patch and RAM-code images (good, truncated, hostile region
   tables) and, when paths are given on the command line (patch, RAM), the real linux-firmware images. */
#define NEXUS_HOSTED 1
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../arch/x86_64/mt7922fw.c"

static void put_be32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void put_le32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint8_t* load(const char* path, uint64_t* size) {
    FILE* f = fopen(path, "rb"); assert(f); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* b = malloc((size_t)n); assert(b && fread(b, 1, (size_t)n, f) == (size_t)n); fclose(f); *size = (uint64_t)n; return b;
}

int main(int argc, char** argv) {
    /* ROM patch: header + 2 info sections + payloads */
    static uint8_t patch[96 + 2 * 64 + 300];
    memcpy(patch, "20260724143257a\n", 16); memcpy(patch + 16, "ALPS", 4); put_be32(patch + 20, 0x8a108a10); put_be32(patch + 24, 0xffffffff);
    put_be32(patch + 44, 2);
    for (unsigned i = 0; i < 2; i++) {
        uint8_t* s = patch + 96 + 64 * i;
        put_be32(s, 0x30002); put_be32(s + 4, 96 + 128 + 100 * i); put_be32(s + 8, 100); put_be32(s + 12, 0x900000 + 0x1000 * i); put_be32(s + 16, 100); put_be32(s + 20, i ? 0x8 : 0);
    }
    struct mt7922_patch_info p;
    assert(mt7922_patch_parse(patch, sizeof(patch), &p) == 0 && p.regions == 2 && !strcmp(p.build, "20260724143257a") && !strcmp(p.platform, "ALPS"));
    assert(p.hw_sw_ver == 0x8a108a10 && p.region[1].addr == 0x901000 && p.region[1].len == 100 && p.region[1].offset == 324 && p.region[1].sec_info == 8);
    assert(mt7922_patch_parse(patch, 95, &p) == MT7922_FW_TRUNCATED && mt7922_patch_parse(patch, 200, &p) == MT7922_FW_TRUNCATED);
    put_be32(patch + 96 + 64 + 16, 1000); assert(mt7922_patch_parse(patch, sizeof(patch), &p) == MT7922_FW_BOUNDS);          /* past the end */
    put_be32(patch + 96 + 64 + 16, 100); put_be32(patch + 96 + 4, 10); assert(mt7922_patch_parse(patch, sizeof(patch), &p) == MT7922_FW_BOUNDS);   /* inside the header */
    put_be32(patch + 96 + 4, 224); put_be32(patch + 96, 1); assert(mt7922_patch_parse(patch, sizeof(patch), &p) == MT7922_FW_SECTION_TYPE);
    put_be32(patch + 96, 2); put_be32(patch + 44, 0x10000); assert(mt7922_patch_parse(patch, sizeof(patch), &p) == MT7922_FW_REGIONS);
    puts("patch images passed");

    /* RAM code: 3 regions (one not downloaded), table, trailer */
    static uint8_t ram[600 + 3 * 40 + 36];
    uint8_t* table = ram + 600; uint8_t* trailer = table + 120;
    uint32_t lengths[3] = {200, 100, 300};
    for (unsigned i = 0; i < 3; i++) { put_le32(table + 40 * i + 16, 0x100000 * (i + 1)); put_le32(table + 40 * i + 20, lengths[i]); table[40 * i + 24] = i == 1 ? 0x40 : 0; }
    trailer[0] = 0x10; trailer[2] = 3; trailer[3] = 2; memcpy(trailer + 7, "____000000", 10); memcpy(trailer + 17, "20260724143402", 14);
    struct mt7922_ram_info r;
    assert(mt7922_ram_parse(ram, sizeof(ram), &r) == 0 && r.regions == 3 && r.payload == 600 && r.region[2].offset == 300 && r.region[1].feature == 0x40);
    assert(!strcmp(r.build, "20260724143402") && r.region[2].addr == 0x300000);
    put_le32(table + 40 * 2 + 20, 301); assert(mt7922_ram_parse(ram, sizeof(ram), &r) == MT7922_FW_BOUNDS); put_le32(table + 40 * 2 + 20, 300);
    trailer[2] = 200; assert(mt7922_ram_parse(ram, sizeof(ram), &r) == MT7922_FW_REGIONS); trailer[2] = 3;
    trailer[0] = 0x21; assert(mt7922_ram_parse(ram, sizeof(ram), &r) == MT7922_FW_CHIP); trailer[0] = 0x10;
    assert(mt7922_ram_parse(ram, 20, &r) == MT7922_FW_TRUNCATED);
    puts("RAM images passed");

    if (argc == 3) {                                                             /* the pinned linux-firmware images */
        uint64_t size; uint8_t* data = load(argv[1], &size);
        assert(mt7922_patch_parse(data, size, &p) == 0 && !strcmp(p.platform, "ALPS"));
        printf("real patch: build %s hw/sw %08x regions %u, first %u bytes at %08x\n", p.build, p.hw_sw_ver, p.regions, p.region[0].len, p.region[0].addr);
        data = load(argv[2], &size);
        assert(mt7922_ram_parse(data, size, &r) == 0 && r.payload <= size);
        printf("real RAM code: chip %02x build %s regions %u payload %llu of %llu bytes\n", r.chip, r.build, r.regions, (unsigned long long)r.payload, (unsigned long long)size);
    }
    puts("MT7922 FIRMWARE LAYOUT TESTS PASSED");
    return 0;
}
