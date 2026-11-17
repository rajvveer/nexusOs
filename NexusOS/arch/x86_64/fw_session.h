#ifndef NEXUS_FW_SESSION_H
#define NEXUS_FW_SESSION_H
#include "../../kernel/types.h"

/* QEMU fw_cfg file directory: https://www.qemu.org/docs/master/specs/fw_cfg.html */
static bool fw_chrome_session(void (*select_item)(uint16_t), uint8_t (*read_byte)(void)) {
    static const char signature[] = "QEMU", name[] = "opt/nexus/session", value[] = "chrome";
    select_item(0);
    for (unsigned i = 0; i < 4; i++) if (read_byte() != signature[i]) return false;
    select_item(0x19);
    uint32_t count = 0;
    for (unsigned i = 0; i < 4; i++) count = (count << 8) | read_byte();
    if (count > 0x4000 - 0x20) return false;
    uint16_t selector = 0; uint32_t size = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t entry[64];
        for (unsigned j = 0; j < sizeof(entry); j++) entry[j] = read_byte();
        unsigned j = 0;
        while (j < sizeof(name) && entry[8 + j] == (uint8_t)name[j]) j++;
        if (j != sizeof(name)) continue;
        if (selector || entry[6] || entry[7]) return false;
        size = ((uint32_t)entry[0] << 24) | ((uint32_t)entry[1] << 16) | ((uint32_t)entry[2] << 8) | entry[3];
        selector = ((uint16_t)entry[4] << 8) | entry[5];
        if (selector < 0x20 || selector >= 0x4000 || (size != 6 && size != 7)) return false;
    }
    if (!selector) return false;
    select_item(selector);
    for (unsigned i = 0; i < 6; i++) if (read_byte() != value[i]) return false;
    return size == 6 || read_byte() == 0;
}
#endif
