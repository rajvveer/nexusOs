/* Read-only /sys for device discovery, served through the procfs handle interface (proc.c).
 * Holds what libdrm's drmGetDevices2()/drmGetDevice2() and Mesa's loader read for the virtio-gpu
 * DRM minors: /sys/dev/char/226:{0,128} -> .../virtio0/drm/{card0,renderD128}, whose "device" link
 * leads to the virtio device ("subsystem" -> /sys/bus/virtio) under the PCI function directory with
 * vendor/device/subsystem/revision/class/config/uevent (PCI_SLOT_NAME) attributes, plus /sys/class/drm
 * and the /sys/bus views. Symbolic links carry absolute targets, which the path walker follows.
 * The base directories exist even without a GPU; other device classes add nodes with node_add(). */
#include "native.h"
#include "gpu.h"
#include "power.h"
#include "hwnode.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

/* Worst case: 11 base nodes, 7 per CPU (cpuN, topology and 5 attributes), 49 for the GPU, plus room for more classes. */
#define NODES (11U + 7U * MAX_CPUS + 49U + 32U + 512U + 48U)   /* + power/ACPI/cpufreq nodes (power.c, sysfs_add), /sys/class/net (lo, eth0: 23 nodes each) */
_Static_assert(NODES <= 0xFFFFU, "sysfs parent indexes are 16-bit");
enum { DIRECTORY, ATTRIBUTE, LINK };
enum { NONE, VENDOR, DEVICE, SUBSYSTEM_VENDOR, SUBSYSTEM_DEVICE, REVISION, CLASS, PCI_UEVENT, CONFIG, IRQ, ENABLE, BOOT_VGA,
       VIRTIO_UEVENT, VIRTIO_DEVICE, VIRTIO_VENDOR, CARD_DEV, CARD_UEVENT, RENDER_DEV, RENDER_UEVENT, DRM_VERSION, MODALIAS,
       CPU_LIST, CPU_PACKAGE, CPU_CORE, CPU_SIBLINGS, CPU_SIBLINGS_LIST, EXTERNAL, EXTERNAL_RW, NET_ATTRIBUTE = 64 /* + netdrv_sysfs_names index */,
       WIFI_ATTR = 128 /* + wifi_sysfs subid; node.cpu = phy index (wifi.c) */ };
unsigned wifi_sysfs_text(unsigned sub, unsigned phy, char* out);
extern const char* const netdrv_sysfs_names[];
unsigned netdrv_sysfs_text(unsigned attribute, unsigned index, char* text);
struct sysfs_node { char path[112], target[112]; uint16_t parent; uint8_t kind, content, cpu; uint16_t external; };
static struct sysfs_node* nodes; static unsigned node_count;
#define SYSFS_LOCKDOWN_ID 0xFF01   /* reserved external id: /sys/kernel/security/lockdown -> lockdown.c, not power.c */

bool sysfs_path(const char* path) { return !strncmp(path, "/sys", 4) && (!path[4] || path[4] == '/'); }
static int node_add(const char* path, unsigned kind, unsigned content, const char* target) {
    if (nodes && node_count >= NODES) {   /* never silent: a missing node breaks device discovery */
        static bool reported; if (!reported) { reported = true; vga_print("[64] sysfs: node table full, nodes dropped\n"); }
        return -1;
    }
    if (!nodes || strlen(path) >= sizeof(nodes[0].path) || (target && strlen(target) >= sizeof(nodes[0].target))) return -1;
    struct sysfs_node* n = &nodes[node_count]; memset(n, 0, sizeof(*n));
    strcpy(n->path, path); if (target) strcpy(n->target, target);
    n->kind = (uint8_t)kind; n->content = (uint8_t)content;
    unsigned length = strlen(path); while (length > 1 && path[length - 1] != '/') length--;
    if (length > 1) length--;
    n->parent = 0;
    for (unsigned i = 0; i < node_count; i++) if (strlen(nodes[i].path) == length && !strncmp(nodes[i].path, path, length)) { n->parent = (uint16_t)i; break; }
    return (int)node_count++;
}
static void hex_text(char* text, uint32_t value, unsigned digits, bool prefix) {
    char* p = text + strlen(text);
    if (prefix) { *p++ = '0'; *p++ = 'x'; }
    for (unsigned i = 0; i < digits; i++) *p++ = "0123456789abcdef"[(value >> ((digits - 1 - i) * 4)) & 15];
    *p = 0;
}
static void upper_hex(char* text, uint32_t value, unsigned digits) {
    char* p = text + strlen(text);
    for (unsigned i = 0; i < digits; i++) *p++ = "0123456789ABCDEF"[(value >> ((digits - 1 - i) * 4)) & 15];
    *p = 0;
}
static void slot_name(char* text) {
    uint32_t location = virtio_gpu_pci_location(); text[0] = 0;
    strcat(text, "0000:"); hex_text(text, location >> 8, 2, false); strcat(text, ":"); hex_text(text, (location >> 3) & 31, 2, false);
    strcat(text, "."); hex_text(text, location & 7, 1, false);
}
static unsigned content_text(unsigned content, unsigned cpu, char* text) {
    text[0] = 0;
    if (content >= WIFI_ATTR) return wifi_sysfs_text(content - WIFI_ATTR, cpu, text);             /* cpu: phy index (wifi.c) */
    if (content >= NET_ATTRIBUTE) return netdrv_sysfs_text(content - NET_ATTRIBUTE, cpu, text);   /* cpu: interface index */
    if (content >= CPU_LIST) { /* CPU topology (Linux drivers/base/cpu.c, topology.c): one package, one thread per core */
        char number[12];
        if (content == CPU_LIST) { strcat(text, "0"); if (cpu_count > 1) { uint_to_str(cpu_count - 1, number); strcat(text, "-"); strcat(text, number); } }
        else if (content == CPU_PACKAGE) strcat(text, "0");
        else if (content == CPU_SIBLINGS) hex_text(text, 1U << cpu, cpu_count > 28 ? 8 : (cpu_count + 3) / 4, false);
        else { uint_to_str(cpu, number); strcat(text, number); }   /* core_id, thread_siblings_list, core_cpus_list */
        strcat(text, "\n"); return strlen(text);
    }
    uint16_t ids[5]; virtio_gpu_pci_ids(ids);
    uint8_t config[256]; uint32_t class_code = 0;
    if (content == CLASS || content == PCI_UEVENT || content == MODALIAS || content == CONFIG || content == BOOT_VGA || content == IRQ) {
        virtio_gpu_pci_config(config); class_code = (uint32_t)config[11] << 16 | (uint32_t)config[10] << 8 | config[9];
    }
    char slot[16]; slot_name(slot);
    switch (content) {
    case VENDOR: hex_text(text, ids[0], 4, true); break;
    case DEVICE: hex_text(text, ids[1], 4, true); break;
    case SUBSYSTEM_VENDOR: hex_text(text, ids[2], 4, true); break;
    case SUBSYSTEM_DEVICE: hex_text(text, ids[3], 4, true); break;
    case REVISION: hex_text(text, ids[4], 2, true); break;
    case CLASS: hex_text(text, class_code, 6, true); break;
    case CONFIG: memcpy(text, config, 64); return 64;
    case IRQ: { char number[12]; uint_to_str(config[0x3C], number); strcat(text, number); break; }
    case ENABLE: strcat(text, "1"); break;
    case BOOT_VGA: strcat(text, (class_code >> 8) == 0x0300 ? "1" : "0"); break;
    case MODALIAS: case PCI_UEVENT:
        if (content == PCI_UEVENT) {
            strcat(text, "DRIVER=virtio-pci\nPCI_CLASS="); upper_hex(text, class_code, 6);
            strcat(text, "\nPCI_ID="); upper_hex(text, ids[0], 4); strcat(text, ":"); upper_hex(text, ids[1], 4);
            strcat(text, "\nPCI_SUBSYS_ID="); upper_hex(text, ids[2], 4); strcat(text, ":"); upper_hex(text, ids[3], 4);
            strcat(text, "\nPCI_SLOT_NAME="); strcat(text, slot); strcat(text, "\nMODALIAS=");
        }
        strcat(text, "pci:v0000"); upper_hex(text, ids[0], 4); strcat(text, "d0000"); upper_hex(text, ids[1], 4);
        strcat(text, "sv0000"); upper_hex(text, ids[2], 4); strcat(text, "sd0000"); upper_hex(text, ids[3], 4);
        strcat(text, "bc"); upper_hex(text, class_code >> 16, 2); strcat(text, "sc"); upper_hex(text, (class_code >> 8) & 255, 2);
        strcat(text, "i"); upper_hex(text, class_code & 255, 2);
        break;
    case VIRTIO_UEVENT: strcat(text, "DRIVER=virtio_gpu\nMODALIAS=virtio:d00000010v00001AF4"); break;
    case VIRTIO_DEVICE: strcat(text, "0x0010"); break;
    case VIRTIO_VENDOR: strcat(text, "0x1af4"); break;
    case CARD_DEV: strcat(text, "226:0"); break;
    case RENDER_DEV: strcat(text, "226:128"); break;
    case CARD_UEVENT: strcat(text, "MAJOR=226\nMINOR=0\nDEVNAME=dri/card0\nDEVTYPE=drm_minor"); break;
    case RENDER_UEVENT: strcat(text, "MAJOR=226\nMINOR=128\nDEVNAME=dri/renderD128\nDEVTYPE=drm_minor"); break;
    case DRM_VERSION: strcat(text, "drm 1.1.0 20060810"); break;
    default: return 0;
    }
    strcat(text, "\n"); return strlen(text);
}
void sysfs_init(void) {
    if (nodes) return;
    nodes = (void*)arena_alloc((uint64_t)NODES * sizeof(struct sysfs_node)); if (!nodes) return;
    static const char* const roots[] = {"/sys", "/sys/dev", "/sys/dev/char", "/sys/devices", "/sys/bus", "/sys/class"};
    for (unsigned i = 0; i < 6; i++) node_add(roots[i], DIRECTORY, NONE, NULL);
    static const char* const block_roots[] = {"/sys/block", "/sys/class/block", "/sys/dev/block", "/sys/devices/virtual", "/sys/devices/virtual/block"};
    for (unsigned i = 0; i < 5; i++) node_add(block_roots[i], DIRECTORY, NONE, NULL);   /* listed here, served by blkdev_node.c */
    /* /sys/devices/system/cpu: online/present/possible lists and per-CPU topology (Wine's GetLogicalProcessorInformation) */
    node_add("/sys/devices/system", DIRECTORY, NONE, NULL); node_add("/sys/devices/system/cpu", DIRECTORY, NONE, NULL);
    static const char* const lists[] = {"online", "present", "possible"};
    for (unsigned i = 0; i < 3; i++) { char name[48] = "/sys/devices/system/cpu/"; strcat(name, lists[i]); node_add(name, ATTRIBUTE, CPU_LIST, NULL); }
    for (unsigned c = 0; c < cpu_count && c < MAX_CPUS; c++) {
        static const char* const files[] = {"physical_package_id", "core_id", "thread_siblings", "thread_siblings_list", "core_cpus_list"};
        static const uint8_t kinds[] = {CPU_PACKAGE, CPU_CORE, CPU_SIBLINGS, CPU_SIBLINGS_LIST, CPU_SIBLINGS_LIST};
        char dir[64] = "/sys/devices/system/cpu/cpu", number[12]; uint_to_str(c, number); strcat(dir, number);
        node_add(dir, DIRECTORY, NONE, NULL); strcat(dir, "/topology"); node_add(dir, DIRECTORY, NONE, NULL);
        for (unsigned i = 0; i < 5; i++) {
            char file[96]; strcpy(file, dir); strcat(file, "/"); strcat(file, files[i]);
            int index = node_add(file, ATTRIBUTE, kinds[i], NULL); if (index >= 0) nodes[index].cpu = (uint8_t)c;
        }
    }
    /* /sys/class/net/{lo,eth0}: carrier, operstate, speed, duplex, address, statistics (netdrv.c) */
    node_add("/sys/class/net", DIRECTORY, NONE, NULL);
    node_add("/sys/class/ieee80211", DIRECTORY, NONE, NULL);   /* wlan phys appear here as wifi.c registers them (sysfs_add_wifi) */
    node_add("/sys/devices/platform", DIRECTORY, NONE, NULL);
    for (unsigned index = 1; index <= 2 && (index == 1 || native_network_available()); index++) {
        char dir[48] = "/sys/class/net/", file[96]; strcat(dir, index == 1 ? "lo" : "eth0"); node_add(dir, DIRECTORY, NONE, NULL);
        strcpy(file, dir); strcat(file, "/statistics"); node_add(file, DIRECTORY, NONE, NULL);
        for (unsigned a = 0; netdrv_sysfs_names[a]; a++) {
            strcpy(file, dir); strcat(file, "/"); strcat(file, netdrv_sysfs_names[a]);
            int at = node_add(file, ATTRIBUTE, NET_ATTRIBUTE + a, NULL); if (at >= 0) nodes[at].cpu = (uint8_t)index;
        }
    }
    /* securityfs (/sys/kernel/security): the kernel lockdown control (lockdown.c), read by anyone, written by root. */
    node_add("/sys/kernel", DIRECTORY, NONE, NULL);
    node_add("/sys/kernel/security", DIRECTORY, NONE, NULL);
    sysfs_add("/sys/kernel/security/lockdown", 2 /* EXTERNAL_RW */, SYSFS_LOCKDOWN_ID, NULL);
    if (!drm_available()) return;
    char slot[16], pci[64], virtio[80], drm[96], card[112], render[112], path[112];
    slot_name(slot);
    strcpy(pci, "/sys/devices/pci0000:00/"); strcat(pci, slot);
    strcpy(virtio, pci); strcat(virtio, "/virtio0");
    strcpy(drm, virtio); strcat(drm, "/drm");
    strcpy(card, drm); strcat(card, "/card0"); strcpy(render, drm); strcat(render, "/renderD128");
    node_add("/sys/devices/pci0000:00", DIRECTORY, NONE, NULL);
    node_add(pci, DIRECTORY, NONE, NULL);
    static const char* const attributes[] = {"vendor", "device", "subsystem_vendor", "subsystem_device", "revision", "class", "uevent",
                                             "config", "irq", "enable", "boot_vga", "modalias"};
    static const uint8_t contents[] = {VENDOR, DEVICE, SUBSYSTEM_VENDOR, SUBSYSTEM_DEVICE, REVISION, CLASS, PCI_UEVENT, CONFIG, IRQ, ENABLE, BOOT_VGA, MODALIAS};
    for (unsigned i = 0; i < 12; i++) { strcpy(path, pci); strcat(path, "/"); strcat(path, attributes[i]); node_add(path, ATTRIBUTE, contents[i], NULL); }
    strcpy(path, pci); strcat(path, "/subsystem"); node_add(path, LINK, NONE, "/sys/bus/pci");
    strcpy(path, pci); strcat(path, "/driver"); node_add(path, LINK, NONE, "/sys/bus/pci/drivers/virtio-pci");
    node_add(virtio, DIRECTORY, NONE, NULL);
    strcpy(path, virtio); strcat(path, "/uevent"); node_add(path, ATTRIBUTE, VIRTIO_UEVENT, NULL);
    strcpy(path, virtio); strcat(path, "/device"); node_add(path, ATTRIBUTE, VIRTIO_DEVICE, NULL);
    strcpy(path, virtio); strcat(path, "/vendor"); node_add(path, ATTRIBUTE, VIRTIO_VENDOR, NULL);
    strcpy(path, virtio); strcat(path, "/subsystem"); node_add(path, LINK, NONE, "/sys/bus/virtio");
    strcpy(path, virtio); strcat(path, "/driver"); node_add(path, LINK, NONE, "/sys/bus/virtio/drivers/virtio_gpu");
    node_add(drm, DIRECTORY, NONE, NULL);
    const char* minors[2] = {card, render}; const uint8_t devs[2] = {CARD_DEV, RENDER_DEV}, events[2] = {CARD_UEVENT, RENDER_UEVENT};
    unsigned minor_count = drm_render_available() ? 2 : 1;   /* without virgl: the KMS primary node only */
    for (unsigned i = 0; i < minor_count; i++) {
        node_add(minors[i], DIRECTORY, NONE, NULL);
        strcpy(path, minors[i]); strcat(path, "/dev"); node_add(path, ATTRIBUTE, devs[i], NULL);
        strcpy(path, minors[i]); strcat(path, "/uevent"); node_add(path, ATTRIBUTE, events[i], NULL);
        strcpy(path, minors[i]); strcat(path, "/device"); node_add(path, LINK, NONE, virtio);
        strcpy(path, minors[i]); strcat(path, "/subsystem"); node_add(path, LINK, NONE, "/sys/class/drm");
    }
    node_add("/sys/dev/char/226:0", LINK, NONE, card);
    if (minor_count > 1) node_add("/sys/dev/char/226:128", LINK, NONE, render);
    node_add("/sys/class/drm", DIRECTORY, NONE, NULL);
    node_add("/sys/class/drm/card0", LINK, NONE, card);
    if (minor_count > 1) node_add("/sys/class/drm/renderD128", LINK, NONE, render);
    node_add("/sys/class/drm/version", ATTRIBUTE, DRM_VERSION, NULL);
    node_add("/sys/bus/pci", DIRECTORY, NONE, NULL);
    node_add("/sys/bus/pci/devices", DIRECTORY, NONE, NULL);
    strcpy(path, "/sys/bus/pci/devices/"); strcat(path, slot); node_add(path, LINK, NONE, pci);
    node_add("/sys/bus/pci/drivers", DIRECTORY, NONE, NULL);
    node_add("/sys/bus/pci/drivers/virtio-pci", DIRECTORY, NONE, NULL);
    node_add("/sys/bus/virtio", DIRECTORY, NONE, NULL);
    node_add("/sys/bus/virtio/devices", DIRECTORY, NONE, NULL);
    node_add("/sys/bus/virtio/devices/virtio0", LINK, NONE, virtio);
    node_add("/sys/bus/virtio/drivers", DIRECTORY, NONE, NULL);
    node_add("/sys/bus/virtio/drivers/virtio_gpu", DIRECTORY, NONE, NULL);
}
int sysfs_add_wifi(const char* path, unsigned subid, unsigned phy) {   /* a live /sys ieee80211 / net wlan attribute (wifi.c) */
    int index = node_add(path, ATTRIBUTE, WIFI_ATTR + subid, NULL); if (index >= 0) nodes[index].cpu = (uint8_t)phy; return index;
}
int sysfs_add(const char* path, unsigned type, unsigned id, const char* target) {
    if (type > 3) return -1;
    int index = node_add(path, type == 0 ? DIRECTORY : type == 3 ? LINK : ATTRIBUTE, type == 1 ? EXTERNAL : type == 2 ? EXTERNAL_RW : NONE, target);
    if (index >= 0) nodes[index].external = (uint16_t)id;
    return index;
}
bool sysfs_writable(uint32_t node) { return nodes && node < node_count && nodes[node].kind == ATTRIBUTE && nodes[node].content == EXTERNAL_RW; }
/* One store per write(2), as Linux sysfs: the whole buffer (at most a page) goes to the owner; the offset is ignored. */
int64_t sysfs_write(uint32_t node, uint64_t address, uint64_t count) {
    if (!sysfs_writable(node)) return -13;
    if (!count) return 0;
    if (count >= 4096) return -22;
    if (!accessible(address, count, false)) return -14;
    char text[4096]; copy_from_user(text, (void*)address, count); text[count] = 0;
    if (nodes[node].external == SYSFS_LOCKDOWN_ID) {   /* lockdown.c: only root may raise the level (Linux securityfs) */
        if (!credentials_capable(credentials_current(), NULL, 21 /* CAP_SYS_ADMIN */)) return -13;
        int64_t result = kernel_lockdown_write(text);
        return result < 0 ? result : (int64_t)count;
    }
    int64_t result = power_sysfs_write(nodes[node].external, text, (unsigned)count);
    return result < 0 ? result : (int64_t)count;
}
static int lookup(const char* path) {
    for (unsigned i = 0; i < node_count; i++) if (!strcmp(nodes[i].path, path)) return (int)i;
    return -1;
}
/* Resolve a path; links in the middle are always followed, a final link only with follow. */
int blk_sysfs_resolve(const char* path, bool follow, uint32_t* node);
bool blk_sysfs_directory(uint32_t node); bool blk_sysfs_link(uint32_t node); int blk_sysfs_stat(uint32_t node, struct linux_stat* stat);
int64_t blk_sysfs_read(uint32_t node, uint64_t address, uint64_t count, uint64_t offset); int blk_sysfs_entry(uint32_t node, uint64_t position, struct directory_item* item);
int blk_sysfs_readlink(uint32_t node, char* result, unsigned capacity); int blk_sysfs_node_path(uint32_t node, char* result, unsigned capacity);
int sysfs_resolve(const char* original, bool follow, uint32_t* node) {
    if (!nodes || !sysfs_path(original)) return -2;
    if (hw_sysfs_path(original)) return hw_sysfs_resolve(original, follow, node);     /* input/video4linux classes (hwsysfs.c) */
    if (blk_sysfs_path(original)) return blk_sysfs_resolve(original, follow, node);   /* /sys/block & co (blkdev_node.c) */
    char path[256]; if (strlen(original) >= sizeof(path)) return -36;
    strcpy(path, original);
    unsigned length = strlen(path); while (length > 4 && path[length - 1] == '/') path[--length] = 0;
    for (unsigned hops = 0; hops < 16; hops++) {
        unsigned end = 4; int found = lookup("/sys"); bool restarted = false;
        while (path[end]) {
            unsigned next = end + 1; while (path[next] && path[next] != '/') next++;
            char saved = path[next]; path[next] = 0; int index = lookup(path); path[next] = saved;
            if (index < 0) return -2;
            if (nodes[index].kind == LINK && (saved || follow)) {
                char rest[256]; strcpy(rest, path + next);
                if (strlen(nodes[index].target) + strlen(rest) >= sizeof(path)) return -36;
                strcpy(path, nodes[index].target); strcat(path, rest); restarted = true; break;
            }
            if (saved && nodes[index].kind != DIRECTORY) return -20;
            found = index; end = next;
        }
        if (!restarted) { if (found < 0) return -2; *node = (uint32_t)found; return 0; }
    }
    return -40;
}
bool sysfs_directory(uint32_t node) { if (node >= HW_SYSFS_BASE) return hw_sysfs_directory(node); if (node >= BLK_SYSFS_BASE) return blk_sysfs_directory(node); return nodes && node < node_count && nodes[node].kind == DIRECTORY; }
bool sysfs_link(uint32_t node) { if (node >= HW_SYSFS_BASE) return hw_sysfs_link(node); if (node >= BLK_SYSFS_BASE) return blk_sysfs_link(node); return nodes && node < node_count && nodes[node].kind == LINK; }
int sysfs_stat(uint32_t node, struct linux_stat* stat) {
    if (node >= HW_SYSFS_BASE) return hw_sysfs_stat(node, stat);
    if (node >= BLK_SYSFS_BASE) return blk_sysfs_stat(node, stat);
    if (!nodes || node >= node_count) return -2;
    memset(stat, 0, sizeof(*stat)); stat->dev = 12; stat->ino = node + 1; stat->block_size = 4096;
    stat->uid = credentials_visible_id(0, false); stat->gid = credentials_visible_id(0, true);
    if (nodes[node].kind == DIRECTORY) { stat->mode = 0040755; stat->nlink = 2; }
    else if (nodes[node].kind == LINK) { stat->mode = 0120777; stat->nlink = 1; stat->size = (int64_t)strlen(nodes[node].target); }
    else { stat->mode = nodes[node].content == CONFIG || nodes[node].content == EXTERNAL_RW ? 0100644 : 0100444; stat->nlink = 1; stat->size = 4096; stat->blocks = 0; }
    return 0;
}
int64_t sysfs_read(uint32_t node, uint64_t address, uint64_t count, uint64_t offset) {
    if (node >= HW_SYSFS_BASE) return hw_sysfs_read(node, address, count, offset);
    if (node >= BLK_SYSFS_BASE) return blk_sysfs_read(node, address, count, offset);
    if (!nodes || node >= node_count) return -2;
    if (nodes[node].kind == DIRECTORY) return -21;
    if (nodes[node].kind != ATTRIBUTE) return -22;
    char text[512]; unsigned length;
    if (nodes[node].external == SYSFS_LOCKDOWN_ID) length = kernel_lockdown_text(text, sizeof(text));   /* lockdown.c */
    else if (nodes[node].content == EXTERNAL || nodes[node].content == EXTERNAL_RW) {
        int64_t n = power_sysfs_read(nodes[node].external, text, sizeof(text)); if (n < 0) return n;
        length = (unsigned)n;
    } else length = content_text(nodes[node].content, nodes[node].cpu, text);
    if (offset >= length || !count) return 0;
    uint64_t n = length - offset < count ? length - offset : count;
    if (!accessible(address, n, true)) return -14;
    return copy_to_user((void*)address, text + offset, n) ? -14 : (int64_t)n;
}
int sysfs_entry(uint32_t node, uint64_t position, struct directory_item* item) {
    if (node >= HW_SYSFS_BASE) return hw_sysfs_entry(node, position, item);
    if (node >= BLK_SYSFS_BASE) return blk_sysfs_entry(node, position, item);
    if (!sysfs_directory(node)) return -20;
    memset(item, 0, sizeof(*item)); item->next = position + 1; item->type = 4;
    if (position < 2) { strcpy(item->name, position ? ".." : "."); item->inode = position ? nodes[node].parent + 1 : node + 1; return 1; }
    uint64_t index = 2;
    for (unsigned i = 1; i < node_count; i++) {
        if (nodes[i].parent != node) continue;
        if (index++ != position) continue;
        const char* name = nodes[i].path + strlen(nodes[i].path); while (name[-1] != '/') name--;
        strcpy(item->name, name); item->inode = i + 1;
        item->type = nodes[i].kind == DIRECTORY ? 4 : nodes[i].kind == LINK ? 10 : 8; return 1;
    }
    return 0;
}
int sysfs_readlink(uint32_t node, char* result, unsigned capacity) {
    if (node >= HW_SYSFS_BASE) return hw_sysfs_readlink(node, result, capacity);
    if (node >= BLK_SYSFS_BASE) return blk_sysfs_readlink(node, result, capacity);
    if (!sysfs_link(node)) return -22;
    unsigned size = strlen(nodes[node].target); if (size >= capacity) return -36;
    memcpy(result, nodes[node].target, size + 1); return (int)size;
}
int sysfs_node_path(uint32_t node, char* result, unsigned capacity) {
    if (node >= HW_SYSFS_BASE) return hw_sysfs_node_path(node, result, capacity);
    if (node >= BLK_SYSFS_BASE) return blk_sysfs_node_path(node, result, capacity);
    if (!nodes || node >= node_count) return -2;
    unsigned size = strlen(nodes[node].path); if (size >= capacity) return -36;
    memcpy(result, nodes[node].path, size + 1); return (int)size;
}
