/* Shared helpers for the native PCIe Ethernet drivers (netdrv.h): MSI-X -> MSI -> INTx -> polled interrupt
 * setup, delays, pinned firmware files and link reporting. */
#include "netdrv.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

bool netdrv_booting;

void netdrv_print_hex(uint64_t value, unsigned digits) {
    char text[20]; unsigned n = 0;
    if (digits > 16) digits = 16;
    for (unsigned i = 0; i < digits; i++) text[n++] = "0123456789abcdef"[(value >> (4 * (digits - 1 - i))) & 15];
    text[n] = 0; vga_print(text);
}
void netdrv_print_dec(uint64_t value) {
    char digits[24]; unsigned n = 0;
    do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value && n < 23);
    char text[24]; unsigned k = 0; while (n) text[k++] = digits[--n]; text[k] = 0; vga_print(text);
}

/* ---- interrupts ---- */
static void netdrv_vector(unsigned vector, void* context) { (void)context; native_nic_irq(vector); }
const char* netdrv_irq_name(unsigned kind) {
    return kind == NETDRV_MSIX ? "MSI-X" : kind == NETDRV_MSI ? "MSI" : kind == NETDRV_INTX ? "INTx" : "polled";
}
void netdrv_irq_setup(pci_device_t* device, struct netdrv_irq* irq, bool allow_msix, bool allow_msi) {
    irq->vector = 0; irq->kind = NETDRV_POLLED; irq->line = 0;
    for (unsigned attempt = 0; attempt < 2; attempt++) {
        bool msix = attempt == 0;
        if (msix ? (!allow_msix || pci_msix_count(device) <= 0) : (!allow_msi || !pci_find_capability(device, 0x05))) continue;
        int vector = irq_alloc_vector(0);
        if (vector < 0) break;
        if (!irq_register((unsigned)vector, netdrv_vector, NULL)) {
            int error = msix ? pci_enable_msix(device, 0, (unsigned)vector, 0) : pci_enable_msi(device, (unsigned)vector, 0);
            if (!error) { irq->vector = (unsigned)vector; irq->kind = msix ? NETDRV_MSIX : NETDRV_MSI; return; }
            if (msix) pci_disable_msix(device); else pci_disable_msi(device);
        }
        irq_free_vector((unsigned)vector);
    }
    /* INTx: re-enable the line (MSI setup sets the INTx-disable command bit), level-triggered through irq.c. */
    uint16_t command = pci_read16(device->bus, device->device, device->function, PCI_COMMAND);
    pci_write32(device->bus, device->device, device->function, PCI_COMMAND, command & ~0x400U);
    unsigned line = device->irq_line;
    if (line < 3 || line >= 16 || line == 8 || line == 13) return;
    int vector = irq_attach_isa(line, IRQ_PCI_INTX, netdrv_vector, NULL);
    if (vector < 0) return;
    irq->vector = (unsigned)vector; irq->kind = NETDRV_INTX; irq->line = line;
}
void netdrv_irq_release(pci_device_t* device, struct netdrv_irq* irq) {
    if (irq->kind == NETDRV_MSIX) pci_disable_msix(device);
    if (irq->kind == NETDRV_MSI) pci_disable_msi(device);
    if (irq->kind == NETDRV_MSIX || irq->kind == NETDRV_MSI) irq_free_vector(irq->vector);
    if (irq->kind == NETDRV_INTX) irq_unregister(irq->vector, netdrv_vector, NULL);  /* the line may be shared: not masked */
    irq->vector = 0; irq->kind = NETDRV_POLLED; irq->line = 0;
}
bool netdrv_irq_restore(pci_device_t* device, const struct netdrv_irq* irq) {
    if (irq->kind == NETDRV_MSIX) return pci_enable_msix(device, 0, irq->vector, 0) == 0;
    if (irq->kind == NETDRV_MSI) return pci_enable_msi(device, irq->vector, 0) == 0;
    if (irq->kind == NETDRV_INTX) {
        uint16_t command = pci_read16(device->bus, device->device, device->function, PCI_COMMAND);
        pci_write32(device->bus, device->device, device->function, PCI_COMMAND, command & ~0x400U);
    }
    return true;
}

/* ---- delays ---- */
void netdrv_udelay(uint64_t microseconds) {
    uint64_t deadline = native_clock_ns() + microseconds * 1000;
    while (native_clock_ns() < deadline) cpu_pause();
}
void netdrv_mdelay(uint64_t milliseconds) {
    uint64_t deadline = native_clock_ns() + milliseconds * 1000000ULL;
    while (native_clock_ns() < deadline) { if (netdrv_booting) boot_wait(); else cpu_pause(); }
}

/* ---- SHA-256 (FIPS 180-4) ---- */
static const uint32_t sha_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
static uint32_t ror(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
static void sha_block(uint32_t h[8], const uint8_t* p) {
    uint32_t w[64];
    for (unsigned i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (unsigned i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (unsigned i = 0; i < 64; i++) {
        uint32_t t1 = k + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + sha_k[i] + w[i];
        uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}
void netdrv_sha256(const void* data, uint64_t size, uint8_t out[32]) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const uint8_t* p = data; uint64_t left = size;
    for (; left >= 64; left -= 64, p += 64) sha_block(h, p);
    uint8_t tail[128]; memset(tail, 0, sizeof(tail)); memcpy(tail, p, left); tail[left] = 0x80;
    unsigned blocks = left + 9 > 64 ? 2 : 1; uint64_t bits = size * 8;
    for (unsigned i = 0; i < 8; i++) tail[blocks * 64 - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (unsigned i = 0; i < blocks; i++) sha_block(h, tail + 64 * i);
    for (unsigned i = 0; i < 8; i++) { out[4 * i] = (uint8_t)(h[i] >> 24); out[4 * i + 1] = (uint8_t)(h[i] >> 16); out[4 * i + 2] = (uint8_t)(h[i] >> 8); out[4 * i + 3] = (uint8_t)h[i]; }
}

/* ---- firmware ----
 * linux-firmware tag 20260916 (git.kernel.org), also listed with URLs in arch/x86_64/firmware-lock.json and
 * installed onto a disk by arch/x86_64/firmware_install.py. Licence: LICENSE.r8169 (redistributable). */
const struct netdrv_firmware_pin netdrv_firmware_pins[] = {
    {"rtl_nic/rtl8125a-3.fw", 3456, "3f6c328af0ceff5969c729edf0f535e8bb2a9b9e05d15b6be3199a3e6d421b99"},
    {"rtl_nic/rtl8125b-2.fw", 3328, "529bf1c25c97ff52b401090d00ff89cc22351012336e5a0c9662728a3ee909ef"},
    {"rtl_nic/rtl8125d-1.fw", 17168, "bc3c82939f02df2f58a17e7354096375001a80a5c1ca3f459c555d5a7d42bf1b"},
    {"rtl_nic/rtl8125d-2.fw", 6800, "c1cb0d358ca985b0d45584612143fd93613d567064702e265bfb370e99c90e35"},
    {"rtl_nic/rtl8126a-2.fw", 29248, "2ffa201aa7dec5e3726e268a10342b175ae8de4fa9f39f535bbbbf5cb8245879"},
    {"rtl_nic/rtl8126a-3.fw", 13232, "18ffaf3b873d217bf780503a80f04fe2b03c088ba2d0c2a3d626145cb8e53b26"},
    {"rtl_nic/rtl8168g-2.fw", 4896, "f878ec719f78e0b104bc73362a9862611858bec893f58a1b3b289a0a57d71d25"},
    {"rtl_nic/rtl8168g-3.fw", 832, "e4d5e62b0a5a097af34c7aa3c55f634fd5be2c232e02c6b983ebd7e699cb27e9"},
    {"rtl_nic/rtl8168h-2.fw", 976, "0b4beab008e308f28296c13188ee23edfeff7d865e398a7ade170495c8cec7e6"},
    {"rtl_nic/rtl8168fp-3.fw", 336, "c526c06aa0ab8d9b57ba2b730bb1e40a1c3513f8591e238dceb2e0f45210ed27"},
    {NULL, 0, NULL}};
#define FIRMWARE_MAX (256U * 1024U)
static int hex_value(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }
const char* netdrv_error_name(int error) {
    switch (error) {
    case -1: return "not pinned"; case -2: return "not found"; case -5: return "read error"; case -12: return "out of memory";
    case -27: return "too large"; case -74: return "SHA-256 or size mismatch"; default: return "error";
    }
}
int netdrv_firmware_load(const char* name, uint8_t** data, uint32_t* size) {
    *data = NULL; *size = 0;
    const struct netdrv_firmware_pin* pin = netdrv_firmware_pins;
    while (pin->name && strcmp(pin->name, name)) pin++;
    if (!pin->name) return -1;
    static const char* const roots[] = {"/lib/firmware/", "/usr/lib/firmware/"};
    fat32_file_t file; bool found = false;
    for (unsigned i = 0; i < 2 && !found; i++) {
        char path[128];
        if (strlen(roots[i]) + strlen(name) >= sizeof(path)) continue;
        strcpy(path, roots[i]); strcat(path, name);
        found = fat32_lookup(path, &file) && !file.is_dir;
    }
    if (!found) return -2;
    if (file.size > FIRMWARE_MAX) return -27;
    if (file.size != pin->size) return -74;
    uint8_t* buffer = (uint8_t*)arena_alloc(pin->size);
    if (!buffer) return -12;
    if (fat32_read_file(&file, 0, pin->size, buffer) != (int32_t)pin->size) { arena_free((uint64_t)buffer, pin->size); return -5; }
    uint8_t digest[32]; netdrv_sha256(buffer, pin->size, digest);
    for (unsigned i = 0; i < 32; i++)
        if (hex_value(pin->sha256[2 * i]) != (digest[i] >> 4) || hex_value(pin->sha256[2 * i + 1]) != (digest[i] & 15)) {
            arena_free((uint64_t)buffer, pin->size); return -74;
        }
    *data = buffer; *size = pin->size;
    return 0;
}
void netdrv_firmware_free(uint8_t* data, uint32_t size) { if (data) arena_free((uint64_t)data, size); }

/* ---- /sys/class/net/{lo,eth0} attributes (sysfs.c nodes, interface index 1 = lo, 2 = the NIC) ---- */
const char* const netdrv_sysfs_names[] = {"address", "addr_len", "broadcast", "carrier", "duplex", "flags", "ifindex", "iflink",
    "mtu", "operstate", "speed", "tx_queue_len", "type", "uevent", "statistics/rx_bytes", "statistics/tx_bytes",
    "statistics/rx_packets", "statistics/tx_packets", "statistics/rx_errors", "statistics/tx_errors", "statistics/rx_dropped", NULL};
static void mac_text(char* text, const uint8_t* mac) {
    char* p = text + strlen(text);
    for (unsigned i = 0; i < 6; i++) { *p++ = "0123456789abcdef"[mac[i] >> 4]; *p++ = "0123456789abcdef"[mac[i] & 15]; if (i < 5) *p++ = ':'; }
    *p = 0;
}
static void number_text(char* text, int64_t value) {
    char digits[24]; unsigned n = 0; bool negative = value < 0; uint64_t v = negative ? (uint64_t)-value : (uint64_t)value;
    do { digits[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 22);
    char* p = text + strlen(text); if (negative) *p++ = '-'; while (n) *p++ = digits[--n]; *p = 0;
}
unsigned netdrv_sysfs_text(unsigned attribute, unsigned index, char* text) {
    text[0] = 0;
    net_device_t* device = index == 2 && native_network_available() ? net_get_device(0) : NULL;
    if (index != 1 && !device) return 0;
    struct nic_link link = {index == 1, 0, NIC_DUPLEX_UNKNOWN};
    if (device && !native_nic_link(&link)) link.up = device->link_up;
    static const uint8_t zero[6] = {0}, ones[6] = {255, 255, 255, 255, 255, 255};
    const char* name = netdrv_sysfs_names[attribute];
    if (!name) return 0;
    if (!strcmp(name, "address")) mac_text(text, device ? device->mac.addr : zero);
    else if (!strcmp(name, "addr_len")) strcat(text, "6");
    else if (!strcmp(name, "broadcast")) mac_text(text, device ? ones : zero);
    else if (!strcmp(name, "carrier")) strcat(text, link.up ? "1" : "0");
    else if (!strcmp(name, "duplex")) strcat(text, !device ? "unknown" : link.duplex == NIC_DUPLEX_FULL ? "full" : link.duplex == NIC_DUPLEX_HALF ? "half" : "unknown");
    else if (!strcmp(name, "flags")) strcat(text, device ? "0x1003" : "0x9");
    else if (!strcmp(name, "ifindex") || !strcmp(name, "iflink")) number_text(text, index);
    else if (!strcmp(name, "mtu")) number_text(text, device ? 1500 : 65536);
    else if (!strcmp(name, "operstate")) strcat(text, !device ? "unknown" : link.up ? "up" : "down");
    else if (!strcmp(name, "speed")) number_text(text, device && link.up && link.speed ? (int64_t)link.speed : -1);
    else if (!strcmp(name, "tx_queue_len")) number_text(text, device ? 1000 : 0);
    else if (!strcmp(name, "type")) number_text(text, device ? 1 : 772);
    else if (!strcmp(name, "uevent")) { strcat(text, "INTERFACE="); strcat(text, device ? (device->name ? device->name : "eth0") : "lo"); strcat(text, "\nIFINDEX="); number_text(text, index); }
    else {
        const net_stats_t* s = device ? &device->stats : NULL;
        uint64_t value = !s ? 0 : !strcmp(name, "statistics/rx_bytes") ? s->rx_bytes : !strcmp(name, "statistics/tx_bytes") ? s->tx_bytes :
                         !strcmp(name, "statistics/rx_packets") ? s->rx_packets : !strcmp(name, "statistics/tx_packets") ? s->tx_packets :
                         !strcmp(name, "statistics/rx_errors") ? s->rx_errors : !strcmp(name, "statistics/tx_errors") ? s->tx_errors : s->rx_dropped;
        number_text(text, (int64_t)value);
    }
    strcat(text, "\n");
    return strlen(text);
}

/* ---- link ---- */
void netdrv_link_report(net_device_t* device, const struct nic_link* now) {
    vga_print("[64] "); vga_print(device->name ? device->name : "eth0");
    if (now->up) {
        vga_print(": link up "); netdrv_print_dec(now->speed); vga_print(" Mb/s ");
        vga_print(now->duplex == NIC_DUPLEX_FULL ? "full duplex\n" : now->duplex == NIC_DUPLEX_HALF ? "half duplex\n" : "duplex unknown\n");
    } else vga_print(": link down\n");
    device->link_up = now->up;
    netlink_link_changed();
}
