/* Storage device nodes, block ioctls, kernel uevents and /sys/block (see blockdev.h).
 *
 * Nodes: every listed disk (whole-disk view) and partition of the block layer gets a slot; FD_DEVICE numbers are
 * DEVICE_STORAGE | generation << 10 | slot, so a descriptor of a removed device never reaches a device that
 * reuses its slot (I/O on it fails with EIO, ioctls with ENODEV, close releases its reference). Slots appear
 * lazily (boot disks) or on block-layer "add" events (hot-plug, loop, device mapper) and retire on "remove".
 * Fixed slots: /dev/loop-control, /dev/mapper(/control), /dev/fuse, /dev/disk/by-*. Block nodes are root:disk 0660;
 * hot-plugged removable disks belong to the desktop user (uid 1000), as logind's uaccess gives the active
 * session its removable media. read/write/pread/pwrite go through block_part_read/write in 64 KiB pieces
 * (unaligned heads and tails read-modify-written), O_DIRECT requires logical-block alignment of the offset,
 * length and buffer, O_EXCL opens are exclusive against mounts and other exclusive opens (EBUSY).
 * Ioctls: BLKGETSIZE(64), BLKSSZGET, BLKPBSZGET, BLKBSZGET/SET, BLKIOMIN/OPT, BLKALIGNOFF, BLKROGET/SET, BLKRRPART,
 * BLKPG (add/delete/resize partition), BLKFLSBUF, BLKDISCARD, BLKZEROOUT, BLKSECDISCARD (EOPNOTSUPP),
 * BLKDISCARDZEROES, BLKROTATIONAL, BLKRAGET/SET, BLKFRAGET/SET, BLKSECTGET, BLKGETDISKSEQ, HDIO_GETGEO, CDROMEJECT;
 * driver ioctls (LOOP_*) through dev->extra->ioctl.
 * uevents: "add@/devices/virtual/block/sdb" ACTION/DEVPATH/SUBSYSTEM=block/MAJOR/MINOR/DEVNAME/DEVTYPE/PARTN/
 * PARTNAME/DISKSEQ/SEQNUM to NETLINK_KOBJECT_UEVENT group 1 (netlink.c). /sys: /sys/block/X, /sys/class/block/X and
 * /sys/dev/block/M:m link to /sys/devices/virtual/block/X[/Xn] with dev, size, ro, removable, uevent, stat,
 * capability, range, partition, start, queue/{logical,physical}_block_size..., device/{vendor,model,rev,state},
 * holders/, slaves/, dm/{name,uuid,suspended}, loop/{backing_file,offset,sizelimit,autoclear,partscan,dio}.
 * LVM: /dev/<vg>/<lv> -> ../dm-N for every active LV (dm uuid "LVM-" + 64 characters, no -real/-cow layer suffix,
 * name <vg>-<lv> with '-' doubled), what udev's lvm rules make; lvm2 run without udev unlinks and re-creates its links:
 * unlink/symlink of these are no-ops (kernel.c), mkdir of /dev/<vg> says EEXIST. */
#include "native.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"
#include "storage_util.h"

#define SLOTS 1024   /* node numbers keep the slot in 10 bits */
#define PIECE (64U * 1024U)
enum { N_NONE, N_LOOP_CONTROL, N_MAPPER_DIR, N_MAPPER_CONTROL, N_FUSE, N_DISK_DIR, N_BY_UUID, N_BY_LABEL, N_BY_PARTUUID,
       N_VG_FIRST, N_VG_LAST = N_VG_FIRST + 6, N_SPECIAL = 16 };   /* N_VG_*: /dev/<vg> of LVM volume groups (below) */
struct node {
    struct block_device* dev; struct block_partition* part;
    uint16_t gen; unsigned opens; bool live, alias;
    char name[136];                /* below /dev: "sda1", "mapper/<dm name, up to 127>" */
};
static struct node* nodes;
static uint64_t seqnum, diskseq;
const struct storage_char_ops* storage_loop_control_ops;
const struct storage_char_ops* storage_mapper_control_ops;
const struct storage_char_ops* storage_fuse_ops;
struct storage_link { char name[72]; char target[40]; unsigned kind; };   /* /dev/disk/by-* (fsprobe.c fills) */
int fsprobe_links(struct storage_link* out, unsigned capacity);          /* weak default below */
__attribute__((weak)) int fsprobe_links(struct storage_link* out, unsigned capacity) { (void)out; (void)capacity; return 0; }

uint64_t storage_linux_dev(uint32_t major, uint32_t minor) {
    return (uint64_t)(minor & 0xFF) | ((uint64_t)(major & 0xFFF) << 8) | ((uint64_t)(minor & ~0xFFU) << 12) | ((uint64_t)(major & ~0xFFFU) << 32);
}
static unsigned number_of(unsigned slot) { return DEVICE_STORAGE | ((unsigned)nodes[slot].gen << 10) | slot; }
static bool special(unsigned slot) { return slot < N_SPECIAL; }
static void block_event(struct block_device* dev, struct block_partition* part, const char* action);
__attribute__((weak)) void loop_init(void) {}
__attribute__((weak)) bool dm_device_suspended(const struct block_device* dev) { (void)dev; return false; }
__attribute__((weak)) void dm_init(void) {}
__attribute__((weak)) void md_init(void) {}
__attribute__((weak)) void fuse_init(void) {}
void storage_nodes_init(void) {
    if (nodes) return;
    nodes = (void*)arena_alloc(SLOTS * sizeof(struct node));
    if (!nodes) { vga_print("[64] STORAGE no memory for device nodes\n"); return; }
    memset(nodes, 0, SLOTS * sizeof(struct node));
    block_event_hook = block_event;
    loop_init(); dm_init(); fuse_init();   /* /dev/loop-control, /dev/mapper/control, /dev/fuse (weak: absent drivers) */
    md_init();                             /* /dev/md0../dev/md3 (md.c) */
}
/* ---- slots ---- */
static int find_part(const struct block_partition* part, bool alias) {
    for (unsigned s = N_SPECIAL; s < SLOTS; s++) if (nodes[s].live && nodes[s].part == part && nodes[s].alias == alias) return (int)s;
    return -1;
}
static int claim(struct block_device* dev, struct block_partition* part, const char* name, bool alias) {
    for (unsigned s = N_SPECIAL; s < SLOTS; s++) {
        struct node* n = &nodes[s];
        if (n->live || n->opens) continue;
        n->gen = (uint16_t)((n->gen + 1) & 0x1FFF); n->dev = dev; n->part = part; n->live = true; n->alias = alias;
        strncpy(n->name, name, sizeof(n->name) - 1); n->name[sizeof(n->name) - 1] = 0;
        return (int)s;
    }
    vga_print("[64] STORAGE node table full\n");
    return -1;
}
static void retire(unsigned s) {
    struct node* n = &nodes[s];
    n->live = false;
    if (!n->opens) { n->dev = NULL; n->part = NULL; }
}
static void ensure(struct block_device* dev, struct block_partition* part) {
    if (!part || find_part(part, false) >= 0) return;
    claim(dev, part, part->linux_name, false);
}
static void sync_nodes(void) {
    if (!nodes) return;
    for (unsigned i = 0; i < block_device_count(); i++) {
        struct block_device* dev = block_device_get(i);
        if (dev && !dev->removed) ensure(dev, block_whole(dev));
    }
    for (unsigned i = 0; i < block_partition_count(); i++) {
        struct block_partition* p = (struct block_partition*)block_partition_get(i);
        if (p && p->number && !p->dev->removed) ensure(p->dev, p);
    }
}
int storage_node_alias(struct block_device* dev, const char* name) {
    if (!nodes) return -12;
    char full[136] = "mapper/"; s_strncat(full, name, sizeof(full) - 8);
    return claim(dev, block_whole(dev), full, true) < 0 ? -12 : 0;
}
void storage_node_unalias(struct block_device* dev) {
    if (!nodes) return;
    for (unsigned s = N_SPECIAL; s < SLOTS; s++) if (nodes[s].live && nodes[s].alias && nodes[s].dev == dev) retire(s);
}
static bool link_number(unsigned device) { return device >= (DEVICE_STORAGE | 0x400000U) && device < (DEVICE_STORAGE | 0x800000U); }
/* ---- LVM names ---- */
bool dm_sysfs_attr(struct block_device* dev, unsigned index, char* text);   /* dm.c: 0 name, 1 uuid */
static char vg_dirs[N_VG_LAST - N_VG_FIRST + 1][64];
static bool lvm_names(const struct node* n, char* vg, char* lv) {   /* vg[64], lv[128] of an active LV's mapper alias */
    if (!n->live || !n->alias || !n->dev || n->dev->removed || strncmp(n->name, "mapper/", 7)) return false;
    char uuid[160] = "";
    if (!dm_sysfs_attr(n->dev, 1, uuid) || strncmp(uuid, "LVM-", 4) || strlen(uuid) != 68) return false;
    unsigned v = 0, l = 0; bool second = false;
    for (const char* p = n->name + 7; *p; p++) {
        char c = *p;
        if (c == '-' && p[1] == '-') p++;
        else if (c == '-' && !second) { second = true; continue; }
        if (second ? l + 1 >= 128 : v + 1 >= 64) return false;
        if (second) lv[l++] = c; else vg[v++] = c;
    }
    vg[v] = 0; lv[l] = 0;
    return second && v && l;
}
static int vg_slot(const char* vg) {   /* the directory slot of an active VG (assigned the first time it is seen) */
    char a[64], b[128]; bool active = false;
    for (unsigned s = N_SPECIAL; s < SLOTS && !active; s++) if (lvm_names(&nodes[s], a, b) && !strcmp(a, vg)) active = true;
    if (!active) return -1;
    for (unsigned i = 0; i <= N_VG_LAST - N_VG_FIRST; i++) if (!strcmp(vg_dirs[i], vg)) return (int)(N_VG_FIRST + i);
    for (unsigned i = 0; i <= N_VG_LAST - N_VG_FIRST; i++) {
        bool used = false; if (vg_dirs[i][0]) for (unsigned s = N_SPECIAL; s < SLOTS && !used; s++) if (lvm_names(&nodes[s], a, b) && !strcmp(a, vg_dirs[i])) used = true;
        if (!used) { strcpy(vg_dirs[i], vg); return (int)(N_VG_FIRST + i); }
    }
    return -1;
}
static int lvm_link(const char* name) {   /* "<vg>/<lv>" -> its link number, -1 */
    const char* slash = s_strchr(name, '/'); if (!slash || s_strchr(slash + 1, '/') || slash - name >= 64) return -1;
    char want[64]; memcpy(want, name, (unsigned)(slash - name)); want[slash - name] = 0;
    for (unsigned s = N_SPECIAL; s < SLOTS; s++) {
        char vg[64], lv[128];
        if (lvm_names(&nodes[s], vg, lv) && !strcmp(vg, want) && !strcmp(lv, slash + 1)) return (int)(DEVICE_STORAGE | 0x400000U | (3U << 16) | s);
    }
    return -1;
}
bool storage_kernel_link(const char* path) {   /* kernel.c: unlink/symlink of /dev/<vg>/<lv> leave the kernel's link */
    const char* name = !strncmp(path, "/dev/", 5) ? path + 5 : NULL;
    return name && nodes && lvm_link(name) >= 0;
}
/* device number -> slot; live: only nodes still present */
static struct node* node_of(unsigned device, bool live, unsigned* slot_out) {
    if (!nodes || device < DEVICE_STORAGE || device >= DEVICE_STORAGE + 0x400000U) return NULL;
    unsigned slot = device & 0x3FF, gen = (device >> 10) & 0x1FFF;
    if (slot >= SLOTS) return NULL;
    if (slot_out) *slot_out = slot;
    if (special(slot)) return &nodes[slot];
    struct node* n = &nodes[slot];
    if (n->gen != gen || !n->dev || (live && (!n->live || n->dev->removed))) return NULL;
    return n;
}

/* ---- uevents ---- */
static unsigned append(char* buffer, unsigned at, unsigned capacity, const char* text) {
    unsigned n = strlen(text);
    if (at + n + 1 > capacity) return at;
    memcpy(buffer + at, text, n + 1); return at + n + 1;   /* keeps the NUL separator */
}
static void devpath_of(const struct block_device* dev, const struct block_partition* part, char* out) {
    strcpy(out, "/devices/virtual/block/"); strcat(out, dev->linux_name);
    if (part && part->number) { strcat(out, "/"); strcat(out, part->linux_name); }
}
void storage_uevent(const char* action, const char* devpath, const char* const* pairs, unsigned count) {
    char buffer[1024]; unsigned at = 0; char line[160];
    strcpy(line, action); strcat(line, "@"); s_strncat(line, devpath, 140); at = append(buffer, at, sizeof(buffer), line);
    strcpy(line, "ACTION="); strcat(line, action); at = append(buffer, at, sizeof(buffer), line);
    strcpy(line, "DEVPATH="); s_strncat(line, devpath, 140); at = append(buffer, at, sizeof(buffer), line);
    for (unsigned i = 0; i < count; i++) at = append(buffer, at, sizeof(buffer), pairs[i]);
    char number[24]; number64(++seqnum, number); strcpy(line, "SEQNUM="); strcat(line, number);
    at = append(buffer, at, sizeof(buffer), line);
    netlink_uevent(buffer, at);
}
void fsprobe_invalidate(const struct block_partition* part);
unsigned fsprobe_uevent_pairs(struct block_partition* p, char storage[6][96]);
void mount_device_removed(struct block_device* dev);
static void block_event(struct block_device* dev, struct block_partition* part, const char* action) {
    if (!nodes) return;
    bool removing = !strcmp(action, "remove");
    struct block_partition* view = part ? part : block_whole(dev);
    fsprobe_invalidate(view);
    if (removing && !part && dev->removed) mount_device_removed(dev);
    if (removing) {
        for (unsigned s = N_SPECIAL; s < SLOTS; s++) {
            struct node* n = &nodes[s];
            if (n->live && n->dev == dev && (part ? n->part == part : (!n->part || !n->part->number || n->alias))) retire(s);
        }
    } else if (!strcmp(action, "add")) { if (!part && !dev->generation) dev->generation = (unsigned)++diskseq; ensure(dev, view); }
    if (!view) return;
    char devpath[96], name[48], major[24], minor[24], partn[24], partname[64], seq[32];
    devpath_of(dev, part, devpath);
    strcpy(name, "DEVNAME="); strcat(name, view->linux_name);
    strcpy(major, "MAJOR="); char digits[16]; uint_to_str(view->major, digits); strcat(major, digits);
    strcpy(minor, "MINOR="); uint_to_str(view->minor, digits); strcat(minor, digits);
    strcpy(seq, "DISKSEQ="); uint_to_str(dev->generation, digits); strcat(seq, digits);
    const char* pairs[16]; unsigned count = 0;
    pairs[count++] = "SUBSYSTEM=block"; pairs[count++] = major; pairs[count++] = minor; pairs[count++] = name;
    pairs[count++] = part && part->number ? "DEVTYPE=partition" : "DEVTYPE=disk";
    if (part && part->number) {
        strcpy(partn, "PARTN="); uint_to_str(part->number, digits); strcat(partn, digits); pairs[count++] = partn;
        if (part->name[0]) { strcpy(partname, "PARTNAME="); s_strncat(partname, part->name, 50); pairs[count++] = partname; }
    }
    pairs[count++] = seq;
    /* No udev runs here: the kernel adds what udev's blkid builtin would (ID_FS_TYPE/USAGE/UUID/LABEL, ID_PART_ENTRY_*),
     * and ID_BUS=usb for USB drives, so a desktop automounter can act on the kernel event alone. */
    static char identity[6][96];
    /* not on a suspended dm device: the read would wait for the resume its caller (lvm2 renaming it) has yet to do */
    if (!removing && !dm_device_suspended(dev)) { unsigned k = fsprobe_uevent_pairs(view, identity); for (unsigned i = 0; i < k; i++) pairs[count++] = identity[i]; }
    if (dev->kind && (!strcmp(dev->kind, "USB") || !strcmp(dev->kind, "UAS"))) pairs[count++] = "ID_BUS=usb";
    storage_uevent(action, devpath, pairs, count);
}

/* ---- lookup ---- */
static const char* below_dev(const char* path) { return !strncmp(path, "/dev/", 5) ? path + 5 : NULL; }
static int special_number(unsigned slot) { return (int)(DEVICE_STORAGE | slot); }
struct user_node { char name[48]; uint32_t rdev; };   /* names mknod made in /dev (storage_mknod, below) */
static struct user_node user_nodes[32];
bool storage_rdev_path(uint32_t rdev, char* out, unsigned capacity);
int storage_lookup(const char* path) {
    const char* name = below_dev(path);
    if (!name || !nodes) return -1;
    if (!strcmp(name, "loop-control")) return special_number(N_LOOP_CONTROL);
    if (!strcmp(name, "mapper")) return special_number(N_MAPPER_DIR);
    if (!strcmp(name, "mapper/control")) return special_number(N_MAPPER_CONTROL);
    if (!strcmp(name, "fuse")) return special_number(N_FUSE);
    if (!strcmp(name, "disk")) return special_number(N_DISK_DIR);
    if (!strcmp(name, "disk/by-uuid")) return special_number(N_BY_UUID);
    if (!strcmp(name, "disk/by-label")) return special_number(N_BY_LABEL);
    if (!strcmp(name, "disk/by-partuuid")) return special_number(N_BY_PARTUUID);
    if (!strncmp(name, "disk/by-", 8)) {   /* links: DEVICE_STORAGE | 0x400000 | kind << 16 | index (kind 3: LVM, below) */
        static const char* const kinds[3] = {"disk/by-uuid/", "disk/by-label/", "disk/by-partuuid/"};
        struct storage_link links[32]; int count = fsprobe_links(links, 32);
        for (unsigned k = 0; k < 3; k++) {
            unsigned prefix = strlen(kinds[k]);
            if (strncmp(name, kinds[k], prefix)) continue;
            for (int i = 0; i < count; i++) if (links[i].kind == k && !strcmp(links[i].name, name + prefix)) return (int)(DEVICE_STORAGE | 0x400000U | (k << 16) | (unsigned)i);
        }
        return -1;
    }
    sync_nodes();
    for (unsigned s = N_SPECIAL; s < SLOTS; s++) if (nodes[s].live && !nodes[s].dev->removed && !strcmp(nodes[s].name, name)) return (int)number_of(s);
    for (unsigned i = 0; i < 32; i++) if (user_nodes[i].name[0] && !strcmp(user_nodes[i].name, name)) {   /* mknod's names */
        char real[64]; return storage_rdev_path(user_nodes[i].rdev, real, sizeof(real)) ? storage_lookup(real) : -1;
    }
    if (s_strchr(name, '/')) return lvm_link(name);
    int vg = vg_slot(name);
    return vg >= 0 ? special_number((unsigned)vg) : -1;
}
/* kernel.c: a block special file on disk (mdadm mknods /tmp/.tmp.md.<pid>:9:0 and opens it) -> /dev/<node> */
bool storage_rdev_path(uint32_t rdev, char* out, unsigned capacity) {
    uint32_t major = (rdev >> 8) & 0xFFF, minor = (rdev & 0xFF) | ((rdev >> 12) & 0xFFF00);
    if (!nodes) return false;
    sync_nodes();
    for (unsigned s = N_SPECIAL; s < SLOTS; s++) {
        struct node* n = &nodes[s];
        if (!n->live || n->alias || !n->part || n->dev->removed || n->part->major != major || n->part->minor != minor) continue;
        if (strlen(n->name) + 6 > capacity) return false;
        strcpy(out, "/dev/"); strcat(out, n->name); return true;
    }
    return false;
}
/* mknod in /dev (devtmpfs: mdadm makes /dev/.tmp.md.<pid>:9:N and opens it): a name for an existing block device */
__attribute__((weak)) bool md_probe(unsigned minor) { (void)minor; return false; }
int storage_mknod(const char* path, uint32_t rdev) {
    const char* name = below_dev(path); char real[64];
    if (!name || !*name || s_strchr(name, '/') || strlen(name) >= sizeof(user_nodes[0].name)) return -1;
    if (!storage_rdev_path(rdev, real, sizeof(real)) && !(((rdev >> 8) & 0xFFF) == 9 && md_probe((rdev & 0xFF) | ((rdev >> 12) & 0xFFF00)) && storage_rdev_path(rdev, real, sizeof(real)))) return -6;   /* md: made on first use */
    for (unsigned i = 0; i < 32; i++) if (user_nodes[i].name[0] && !strcmp(user_nodes[i].name, name)) return -17;
    for (unsigned i = 0; i < 32; i++) if (!user_nodes[i].name[0]) { strcpy(user_nodes[i].name, name); user_nodes[i].rdev = rdev; return 0; }
    return -28;
}
bool storage_unlink_node(const char* path) {
    const char* name = below_dev(path);
    if (name) for (unsigned i = 0; i < 32; i++) if (user_nodes[i].name[0] && !strcmp(user_nodes[i].name, name)) { user_nodes[i].name[0] = 0; return true; }
    return false;
}
int storage_readlink(unsigned device, char* target, unsigned capacity) {
    if (!link_number(device)) return -22;
    if (((device >> 16) & 3) == 3) {   /* /dev/<vg>/<lv> -> ../dm-N */
        unsigned s = device & 0xFFFF; char vg[64], lv[128];
        if (s >= SLOTS || !lvm_names(&nodes[s], vg, lv)) return -2;
        char text[64] = "../"; strcat(text, nodes[s].dev->linux_name);
        unsigned n = strlen(text); if (n >= capacity) return -36;
        memcpy(target, text, n + 1); return (int)n;
    }
    struct storage_link links[32]; int count = fsprobe_links(links, 32);
    unsigned index = device & 0xFFFF, kind = (device >> 16) & 3;
    if ((int)index >= count || links[index].kind != kind) return -2;
    char text[64] = "../../"; strcat(text, links[index].target);
    unsigned n = strlen(text); if (n >= capacity) return -36;
    memcpy(target, text, n + 1); return (int)n;
}

/* ---- permissions ---- */
static bool capable_now(unsigned capability) { return credentials_capable(credentials_current(), NULL, capability); }
bool credentials_in_group(struct credentials* credentials, uint32_t gid, bool real);
/* USB drives (removable media or not) belong to the desktop session's user, as udisks/logind treat hot-plug drives. */
static uint32_t owner_of(const struct block_device* dev) { return dev->hotplug && dev->kind && (!strcmp(dev->kind, "USB") || !strcmp(dev->kind, "UAS")) ? 1000 : 0; }
static int block_access(const struct block_device* dev, uint64_t flags) {
    uint32_t uid = credentials_kernel_id(credentials_current(), false, 3);
    unsigned want = (flags & 3) == 0 ? 4 : (flags & 3) == 1 ? 2 : 6;
    if (uid == owner_of(dev) || credentials_in_group(credentials_current(), 6, false)) return 0;   /* 0660 owner:disk */
    if (capable_now(1)) return 0;                                                                  /* CAP_DAC_OVERRIDE */
    (void)want; return -13;
}

/* ---- open / close ---- */
static const struct storage_char_ops* char_ops(unsigned slot) {
    return slot == N_LOOP_CONTROL ? storage_loop_control_ops : slot == N_MAPPER_CONTROL ? storage_mapper_control_ops : slot == N_FUSE ? storage_fuse_ops : NULL;
}
static bool directory_slot(unsigned slot) { return slot == N_MAPPER_DIR || slot == N_DISK_DIR || slot == N_BY_UUID || slot == N_BY_LABEL || slot == N_BY_PARTUUID || (slot >= N_VG_FIRST && slot <= N_VG_LAST); }
static bool partitions_busy(const struct block_device* dev) {
    for (unsigned i = 0; i < block_partition_count(); i++) {
        const struct block_partition* p = block_partition_get(i);
        if (p && p->dev == dev && p->holders) return true;
    }
    return false;
}
int storage_open_check(unsigned device, uint64_t flags) {
    if (link_number(device)) return -40;
    unsigned slot; struct node* n = node_of(device, true, &slot);
    if (!n) return -6;   /* ENXIO: the device went away */
    if ((flags & 3) == 3) return -22;
    if (special(slot)) {
        if (directory_slot(slot)) return flags & (3 | 0x200) ? -21 : 0;
        if (flags & 0x10000) return -20;
        const struct storage_char_ops* ops = char_ops(slot);
        if (!ops) return -19;
        if (slot == N_MAPPER_CONTROL && !capable_now(21)) return -13;    /* root 0600 (CAP_SYS_ADMIN) */
        return 0;
    }
    if (flags & 0x10000) return -20;
    int error = block_access(n->dev, flags); if (error) return error;
    if ((flags & 0x80) && !(flags & 0x40)) {   /* O_EXCL without O_CREAT: exclusive against mounts and other O_EXCL */
        if (n->part->holders || (!n->part->number && partitions_busy(n->dev))) return -16;
    }
    return 0;
}
void storage_open(unsigned device, uint64_t flags, struct input_reader* reader) {
    unsigned slot; struct node* n = node_of(device, true, &slot);
    reader->sequence = 0; reader->report = false;
    if (!n) return;
    if (special(slot)) { const struct storage_char_ops* ops = char_ops(slot); if (ops && ops->open) reader->clock = ops->open(flags, reader); return; }
    n->opens++; n->part->opens++; block_get(n->dev);
    if ((flags & 0x80) && !(flags & 0x40)) { n->part->holders++; reader->report = true; }
}
void storage_close(unsigned device, uint64_t flags, struct input_reader* reader) {
    unsigned slot; struct node* n = node_of(device, false, &slot);
    if (!n) return;
    if (special(slot)) { const struct storage_char_ops* ops = char_ops(slot); if (ops && ops->close && !reader->clock) ops->close(reader); return; }
    if (!n->opens) return;
    /* A writer closed the node: re-identify it and tell user space (udev's watch rule does this on Linux). */
    if ((flags & 3) && n->live && !n->dev->removed) block_event(n->dev, n->part && n->part->number ? n->part : NULL, "change");
    if (reader->report && n->part->holders) n->part->holders--;
    if (n->part->opens) n->part->opens--;
    struct block_device* dev = n->dev;
    if (!--n->opens && !n->live) { n->dev = NULL; n->part = NULL; }
    block_put(dev);
}

/* writev as one message for char drivers that frame their writes (FUSE replies: header + payload vectors). */
bool storage_message_device(unsigned device) {
    unsigned slot; struct node* n = node_of(device, true, &slot);
    if (!n || !special(slot)) return false;
    const struct storage_char_ops* ops = char_ops(slot);
    return ops && ops->write_message;
}
int64_t storage_write_vectors(unsigned device, const struct iovec64* vectors, unsigned count, uint64_t total, struct input_reader* reader) {
    unsigned slot; struct node* n = node_of(device, true, &slot);
    if (!n || !special(slot)) return -9;
    const struct storage_char_ops* ops = char_ops(slot);
    if (!ops || !ops->write_message) return -22;
    if (total > (2u << 20)) return -22;
    uint64_t size = (total + 4095) / 4096 * 4096;
    uint8_t* data = (void*)arena_alloc(size); if (!data) return -12;
    uint64_t at = 0; int64_t result = 0;
    for (unsigned i = 0; i < count; i++) {
        if (!vectors[i].size) continue;
        if (!accessible(vectors[i].address, vectors[i].size, false)) { result = -14; break; }
        if (copy_from_user(data + at, (const void*)vectors[i].address, vectors[i].size)) { result = -14; break; } at += vectors[i].size;
    }
    if (!result) result = ops->write_message(reader, data, at);
    arena_free((uint64_t)data, size);
    return result;
}

/* ---- data ---- */
static uint64_t partition_bytes(const struct block_partition* p) { return block_part_sectors(p) * 512; }
int64_t storage_io(unsigned device, uint64_t buffer, uint64_t count, bool write, uint64_t* position, uint64_t flags, struct input_reader* reader) {
    unsigned slot; struct node* n = node_of(device, false, &slot);
    if (!n) return -5;
    if (special(slot)) {
        if (directory_slot(slot)) return -21;
        const struct storage_char_ops* ops = char_ops(slot);
        return ops && ops->io ? ops->io(reader, buffer, count, write, flags) : -22;
    }
    if (!n->live || n->dev->removed) return -5;
    struct block_partition* p = n->part; struct block_device* dev = n->dev;
    if (write && (dev->read_only)) return -1;
    if (count > 0x7FFFF000) count = 0x7FFFF000;
    uint64_t pos = *position, size = partition_bytes(p);
    if (flags & 0x4000) {   /* O_DIRECT: logical-block aligned offset, length and memory */
        uint64_t mask = dev->block_size - 1;
        if ((pos & mask) || (count & mask) || (buffer & mask)) return -22;
    }
    if (!count) return 0;
    if (pos >= size) return write ? -28 : 0;
    if (count > size - pos) count = size - pos;
    if (!accessible(buffer, count, !write)) return -14;
    uint8_t* bounce = (void*)arena_alloc(PIECE + 512);
    if (!bounce) return -12;
    block_get(dev);
    uint64_t done = 0; int error = 0;
    while (done < count && !error) {
        uint64_t at = pos + done, sector = at / 512; unsigned offset = (unsigned)(at % 512);
        uint64_t piece = count - done; if (piece > PIECE - offset) piece = PIECE - offset;
        uint32_t sectors = (uint32_t)((offset + piece + 511) / 512);
        if (write) {
            if ((offset && block_part_read(p, sector, 1, bounce) < 0) ||
                ((offset + piece) % 512 && (sectors > 1 || !offset) && block_part_read(p, sector + sectors - 1, 1, bounce + (uint64_t)(sectors - 1) * 512) < 0)) { error = -5; break; }
            if (!accessible(buffer + done, piece, false)) { error = -14; break; }
            if (copy_from_user(bounce + offset, (const void*)(buffer + done), piece)) { error = -14; break; }
            if (block_part_write(p, sector, sectors, bounce) < 0) { error = -5; break; }
        } else {
            if (block_part_read(p, sector, sectors, bounce) < 0) { error = -5; break; }
            if (!accessible(buffer + done, piece, true)) { error = -14; break; }
            if (copy_to_user((void*)(buffer + done), bounce + offset, piece)) { error = -14; break; }
        }
        done += piece;
    }
    arena_free((uint64_t)bounce, PIECE + 512);
    if (!error && write && (flags & 0x1000) && block_flush(dev) < 0) error = -5;   /* O_SYNC / O_DSYNC */
    block_put(dev);
    if (done) { *position = pos + done; return (int64_t)done; }
    return error;
}
int64_t storage_seek(unsigned device, uint64_t* position, int64_t offset, uint64_t whence) {
    unsigned slot; struct node* n = node_of(device, false, &slot);
    if (!n) return -5;
    if (special(slot)) {
        if (!directory_slot(slot)) return -29;
        if (whence != 0 || offset < 0) return -22;
        *position = (uint64_t)offset; return offset;
    }
    if (whence > 2) return -22;
    int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)*position : (n->live && !n->dev->removed ? (int64_t)partition_bytes(n->part) : 0), result;
    if (__builtin_add_overflow(base, offset, &result) || result < 0) return -22;
    *position = (uint64_t)result; return result;
}
int storage_fsync(unsigned device) {
    unsigned slot; struct node* n = node_of(device, false, &slot);
    if (!n || special(slot)) return n ? 0 : -5;
    if (!n->live || n->dev->removed) return -5;
    return block_flush(n->dev) < 0 ? -5 : 0;
}

/* ---- ioctls ---- */
struct blkpg_partition { int64_t start, length; int32_t pno; char devname[64], volname[64]; };
struct blkpg_arg { int32_t op, flags, datalen, pad; uint64_t data; };
static int put_value(uint64_t address, const void* value, unsigned size) {
    if (!accessible(address, size, true)) return -14;
    return copy_to_user((void*)address, value, size) ? -14 : 0;
}
static int get_value(uint64_t address, void* value, unsigned size) {
    if (!accessible(address, size, false)) return -14;
    return copy_from_user(value, (const void*)address, size) ? -14 : 0;
}
static uint32_t soft_block_size(const struct block_partition* p) {
    uint32_t size = p->dev->block_size; uint64_t bytes = partition_bytes(p);
    while (size < 4096 && !(bytes & ((uint64_t)size * 2 - 1))) size *= 2;
    return size;
}
static int zero_range(struct block_partition* p, uint64_t start, uint64_t length) {
    uint8_t* zeros = (void*)arena_alloc(PIECE); if (!zeros) return -12;
    memset(zeros, 0, PIECE);
    int error = 0;
    for (uint64_t at = 0; at < length && !error; at += PIECE) {
        uint64_t piece = length - at < PIECE ? length - at : PIECE;
        if (block_part_write(p, (start + at) / 512, (uint32_t)(piece / 512), zeros) < 0) error = -5;
    }
    arena_free((uint64_t)zeros, PIECE);
    return error;
}
int partition_table_edit(struct block_device* dev, int op, int64_t start, int64_t length, int number); /* blockdev.c BLKPG */
static int blkpg(struct node* n, uint64_t argument) {
    if (!capable_now(21)) return -13;
    struct blkpg_arg a; if (get_value(argument, &a, sizeof(a))) return -14;
    if (n->part->number) return -22;    /* only on the whole disk */
    struct blkpg_partition bp; if (a.datalen < (int)sizeof(bp) || get_value(a.data, &bp, sizeof(bp))) return a.datalen < (int)sizeof(bp) ? -22 : -14;
    if (a.op < 1 || a.op > 3 || bp.pno <= 0 || bp.pno > 256) return -22;
    if (bp.pno > 15) return a.op == 1 ? -22 : -6;   /* beyond the 15 a disk carries here: none such (Linux: ENXIO) */
    if ((a.op != 2) && (bp.start < 0 || bp.length <= 0 || (bp.start % 512) || (bp.length % 512))) return -22;
    return partition_table_edit(n->dev, a.op, bp.start, bp.length, bp.pno);
}
int storage_ioctl(unsigned device, uint64_t command, uint64_t argument, struct input_reader* reader, uint64_t flags) {
    unsigned slot; struct node* n = node_of(device, false, &slot);
    if (!n) return -19;
    uint32_t cmd = (uint32_t)command;
    if (special(slot)) {
        const struct storage_char_ops* ops = char_ops(slot);
        return ops && ops->ioctl ? ops->ioctl(reader, command, argument, flags) : -25;
    }
    if (!n->live || n->dev->removed) return -19;
    struct block_device* dev = n->dev; struct block_partition* p = n->part;
    if (dev->extra && dev->extra->ioctl) { int r = dev->extra->ioctl(dev, command, argument); if (r != -25) return r; }
    switch (cmd) {
    case 0x1260: { uint64_t sectors = block_part_sectors(p); return put_value(argument, &sectors, 8); }      /* BLKGETSIZE */
    case 0x80081272: { uint64_t bytes = partition_bytes(p); return put_value(argument, &bytes, 8); }       /* BLKGETSIZE64 */
    case 0x1268: { int32_t v = (int32_t)dev->block_size; return put_value(argument, &v, 4); }             /* BLKSSZGET */
    case 0x127B: case 0x1278: { uint32_t v = dev->physical_block_size ? dev->physical_block_size : dev->block_size; return put_value(argument, &v, 4); } /* BLKPBSZGET, BLKIOMIN */
    case 0x1279: case 0x127A: case 0x127C: { uint32_t v = 0; return put_value(argument, &v, 4); }       /* BLKIOOPT, BLKALIGNOFF, BLKDISCARDZEROES */
    case 0x80081270: { uint32_t v = soft_block_size(p); return put_value(argument, &v, 4); }             /* BLKBSZGET */
    case 0x40081271: { int32_t v; if (get_value(argument, &v, 4)) return -14; return v < 512 || v > 4096 || (v & (v - 1)) || (uint32_t)v < dev->block_size ? -22 : 0; } /* BLKBSZSET */
    case 0x125E: { int32_t v = dev->read_only; return put_value(argument, &v, 4); }                      /* BLKROGET */
    case 0x125D: { int32_t v; if (!capable_now(21)) return -13; if (get_value(argument, &v, 4)) return -14; dev->read_only = v != 0; return 0; } /* BLKROSET */
    case 0x127E: { uint16_t v = dev->rotational; return put_value(argument, &v, 2); }                    /* BLKROTATIONAL */
    case 0x1263: case 0x1265: { uint64_t v = 256; return put_value(argument, &v, 8); }                    /* BLKRAGET, BLKFRAGET */
    case 0x1262: case 0x1264: return capable_now(21) ? 0 : -13;                                           /* BLKRASET, BLKFRASET */
    case 0x1267: { uint16_t v = (uint16_t)(dev->max_blocks * (dev->block_size / 512) > 0xFFFF ? 0xFFFF : dev->max_blocks * (dev->block_size / 512)); return put_value(argument, &v, 2); } /* BLKSECTGET */
    case 0x80081280: { uint64_t v = dev->generation; return put_value(argument, &v, 8); }               /* BLKGETDISKSEQ */
    case 0x1261: if (!capable_now(21) && owner_of(dev) != credentials_kernel_id(credentials_current(), false, 3)) return -13;   /* BLKFLSBUF */
        memory_file_writeback(true, 0, ~0ULL); return block_flush(dev) < 0 ? -5 : 0;
    case 0x125F:                                                                                          /* BLKRRPART */
        if (!capable_now(21)) return -13;
        if (p->number) return -22;
        return block_rescan(dev);
    case 0x1269: return blkpg(n, argument);                                                               /* BLKPG */
    case 0x1277: case 0x127F: case 0x127D: {                                                              /* BLKDISCARD, BLKZEROOUT, BLKSECDISCARD */
        if (!(flags & 3)) return -9;
        uint64_t range[2]; if (get_value(argument, range, 16)) return -14;
        uint64_t mask = dev->block_size - 1, size = partition_bytes(p);
        if ((range[0] & mask) || (range[1] & mask) || range[0] > size || range[1] > size - range[0]) return -22;
        if (dev->read_only) return -1;
        if (cmd == 0x127D) return -95;
        if (cmd == 0x127F) return zero_range(p, range[0], range[1]);
        if (!dev->extra || !dev->extra->discard || !dev->discard_granularity) return -95;
        if (!range[1]) return 0;
        return dev->extra->discard(dev, p->start + range[0] / dev->block_size, range[1] / dev->block_size);
    }
    case 0x0301: {                                                                                        /* HDIO_GETGEO */
        struct { uint8_t heads, sectors; uint16_t cylinders; uint32_t pad; uint64_t start; } g;
        uint64_t sectors = block_part_sectors(block_whole(dev)), cylinders = sectors / (255 * 63);
        g.heads = 255; g.sectors = 63; g.cylinders = (uint16_t)(cylinders > 0xFFFF ? 0xFFFF : cylinders); g.pad = 0;
        g.start = p->start * (dev->block_size / 512);
        return put_value(argument, &g, sizeof(g));
    }
    case 0x5309:                                                                                          /* CDROMEJECT */
        if (!dev->extra || !dev->extra->eject) return -25;
        return dev->extra->eject(dev);
    }
    return -25;
}

/* ---- stat / directories ---- */
static void char_stat(unsigned slot, struct linux_stat* st) {
    st->mode = slot == N_MAPPER_CONTROL ? 0020600 : slot == N_FUSE ? 0020666 : 0020660;
    st->rdev = storage_linux_dev(10, slot == N_LOOP_CONTROL ? 237 : slot == N_MAPPER_CONTROL ? 236 : 229);
    st->gid = credentials_visible_id(slot == N_LOOP_CONTROL ? 6 : 0, true);
}
void storage_stat(unsigned device, struct linux_stat* st) {
    memset(st, 0, sizeof(*st)); st->dev = 9; st->ino = 0x100000 + (device & 0x3FF) + ((uint64_t)(device >> 10 & 0x1FFF) << 10);
    st->nlink = 1; st->block_size = 4096; st->uid = credentials_visible_id(0, false); st->gid = credentials_visible_id(0, true);
    if (link_number(device)) { st->mode = 0120777; st->size = 16; return; }
    unsigned slot; struct node* n = node_of(device, false, &slot);
    if (!n) { st->mode = 0060000; return; }
    if (special(slot)) {
        if (directory_slot(slot)) { st->mode = 0040755; st->nlink = 2; return; }
        char_stat(slot, st); return;
    }
    if (!n->part) { st->mode = 0060000; return; }
    st->mode = 0060660; st->rdev = storage_linux_dev(n->part->major, n->part->minor);
    st->uid = credentials_visible_id(owner_of(n->dev), false); st->gid = credentials_visible_id(6, true);
}
static int fill(struct directory_item* item, const char* name, uint64_t inode, uint8_t type) {
    strncpy(item->name, name, sizeof(item->name) - 1); item->name[sizeof(item->name) - 1] = 0; item->inode = inode; item->type = type; return 1;
}
int storage_root_entry(uint64_t index, struct directory_item* item) {
    if (!nodes) return 0;
    static const char* const fixed[4] = {"loop-control", "mapper", "fuse", "disk"};
    static const uint8_t types[4] = {2, 4, 2, 4};
    if (index < 4) return fill(item, fixed[index], 0x100000 + index + 1, types[index]);
    index -= 4; sync_nodes();
    for (unsigned s = N_SPECIAL; s < SLOTS; s++) {
        if (!nodes[s].live || nodes[s].alias || nodes[s].dev->removed || s_strchr(nodes[s].name, '/')) continue;
        if (!index--) return fill(item, nodes[s].name, 0x100000 + s, 6);
    }
    for (unsigned s = N_SPECIAL; s < SLOTS; s++) {   /* /dev/<vg> once per active volume group */
        char vg[64], lv[128], other[64], ignored[128]; bool first = true;
        if (!lvm_names(&nodes[s], vg, lv)) continue;
        for (unsigned t = N_SPECIAL; t < s && first; t++) if (lvm_names(&nodes[t], other, ignored) && !strcmp(other, vg)) first = false;
        if (!first) continue;
        int slot = vg_slot(vg); if (slot < 0) continue;
        if (!index--) return fill(item, vg, 0x100000 + (unsigned)slot, 4);
    }
    return 0;
}
int storage_directory(unsigned device, uint64_t position, struct directory_item* item) {
    unsigned slot; struct node* n = node_of(device, true, &slot);
    if (!n || !special(slot) || !directory_slot(slot)) return -20;
    memset(item, 0, sizeof(*item)); item->next = position + 1;
    if (position < 2) return fill(item, position ? ".." : ".", position ? 1 : 0x100000 + slot, 4);
    uint64_t index = position - 2;
    if (slot == N_MAPPER_DIR) {
        if (!index) return fill(item, "control", 0x100000 + N_MAPPER_CONTROL, 2);
        index--;
        for (unsigned s = N_SPECIAL; s < SLOTS; s++) {
            if (!nodes[s].live || !nodes[s].alias || nodes[s].dev->removed) continue;
            if (!index--) return fill(item, nodes[s].name + 7, 0x100000 + s, 6);
        }
        return 0;
    }
    if (slot == N_DISK_DIR) {
        static const char* const subdirs[3] = {"by-uuid", "by-label", "by-partuuid"};
        return index < 3 ? fill(item, subdirs[index], 0x100000 + N_BY_UUID + index, 4) : 0;
    }
    if (slot >= N_VG_FIRST && slot <= N_VG_LAST) {   /* the LVs of one volume group */
        for (unsigned s = N_SPECIAL; s < SLOTS; s++) {
            char vg[64], lv[128];
            if (lvm_names(&nodes[s], vg, lv) && !strcmp(vg, vg_dirs[slot - N_VG_FIRST]) && !index--) return fill(item, lv, 0x300000 + s, 10);
        }
        return 0;
    }
    unsigned kind = slot - N_BY_UUID;
    struct storage_link links[32]; int count = fsprobe_links(links, 32);
    for (int i = 0; i < count; i++) if (links[i].kind == kind && !index--) return fill(item, links[i].name, 0x200000 + (uint64_t)i, 10);
    return 0;
}
uint32_t storage_ready(unsigned device, const struct input_reader* reader) {
    unsigned slot; struct node* n = node_of(device, false, &slot);
    if (n && special(slot)) { const struct storage_char_ops* ops = char_ops(slot); if (ops && ops->ready) return ops->ready(reader); }
    return 5;
}
void* storage_event_source(unsigned device, const struct input_reader* reader) {
    unsigned slot; struct node* n = node_of(device, false, &slot);
    if (n && special(slot)) { const struct storage_char_ops* ops = char_ops(slot); if (ops && ops->source) return ops->source(reader); }
    return NULL;
}
bool storage_pollable(unsigned device) { return (device & 0x3FF) == N_FUSE; }
struct block_partition* storage_partition(unsigned device) {
    unsigned slot; struct node* n = node_of(device, true, &slot);
    return n && !special(slot) ? n->part : NULL;
}

/* ---- /proc/partitions ---- */
unsigned storage_proc_partitions(char* out, unsigned capacity) {
    unsigned used = 0; out[0] = 0;
    #define EMIT(text) do { const char* t_ = (text); unsigned l_ = strlen(t_); if (used + l_ < capacity) { memcpy(out + used, t_, l_ + 1); used += l_; } } while (0)
    EMIT("major minor  #blocks  name\n\n");
    sync_nodes();
    for (unsigned s = N_SPECIAL; s < SLOTS; s++) {
        struct node* n = &nodes[s];
        if (!n->live || n->alias || n->dev->removed || !n->part) continue;
        char line[96], number[24]; line[0] = 0;
        uint_to_str(n->part->major, number); for (unsigned k = strlen(number); k < 4; k++) strcat(line, " "); strcat(line, number);
        uint_to_str(n->part->minor, number); for (unsigned k = strlen(number); k < 8; k++) strcat(line, " "); strcat(line, number);
        number64(block_part_sectors(n->part) / 2, number); for (unsigned k = strlen(number); k < 11; k++) strcat(line, " "); strcat(line, number);
        strcat(line, " "); strcat(line, n->part->linux_name); strcat(line, "\n");
        EMIT(line);
    }
    #undef EMIT
    return used;
}

/* ---- /sys/block ----
 * Node numbers: BLK_SYSFS_BASE | kind << 22 | slot << 12 | item. */
enum { K_ROOT = 0, K_DISK = 1, K_PART = 2, K_ATTR = 3, K_DIR = 4, K_LINK = 5 };
enum { R_BLOCK = 1, R_CLASS, R_DEV, R_VIRTUAL };
enum { D_QUEUE = 1, D_DEVICE, D_HOLDERS, D_SLAVES, D_DM, D_LOOP, D_MD, D_MDDEV };   /* D_MDDEV item: member << 4 | attribute + 1 */
enum { L_BLOCK = 1, L_CLASS, L_DEV, L_SUBSYSTEM, L_HOLDER, L_SLAVE, L_MDBLOCK };
static const char* const disk_attrs[] = {"dev", "size", "ro", "removable", "uevent", "stat", "inflight", "capability", "range", "ext_range",
                                         "hidden", "alignment_offset", "discard_alignment", "diskseq", "events", "events_async", "events_poll_msecs"};
static const char* const part_attrs[] = {"dev", "size", "ro", "uevent", "stat", "inflight", "partition", "start", "alignment_offset", "discard_alignment"};
static const char* const queue_attrs[] = {"logical_block_size", "physical_block_size", "hw_sector_size", "minimum_io_size", "optimal_io_size",
                                          "rotational", "max_sectors_kb", "max_hw_sectors_kb", "discard_granularity", "discard_max_bytes",
                                          "discard_max_hw_bytes", "discard_zeroes_data", "read_ahead_kb", "nr_requests", "scheduler", "add_random",
                                          "nomerges", "dax", "zoned", "write_cache", "max_segments", "max_segment_size"};
static const char* const device_attrs[] = {"vendor", "model", "rev", "type", "state", "serial", "queue_depth", "scsi_level"};
static const char* const dm_attrs[] = {"name", "uuid", "suspended", "use_blk_mq"};
static const char* const md_attrs[] = {"level", "raid_disks", "chunk_size", "layout", "component_size", "metadata_version", "array_state",
                                      "array_size", "degraded", "sync_action", "sync_completed", "resync_start", "uuid", "consistency_policy"};
static const char* const md_dev_attrs[] = {"slot", "state", "offset", "size", "errors", "recovery_start"};   /* md/dev-<member>/ (+ block link) */
static const char* const loop_attrs[] = {"backing_file", "offset", "sizelimit", "autoclear", "partscan", "dio"};
#define COUNT(a) (sizeof(a) / sizeof(a[0]))
/* dm.c / loop.c fill their attribute directories (weak defaults: absent). */
__attribute__((weak)) bool dm_sysfs_attr(struct block_device* dev, unsigned index, char* text) { (void)dev; (void)index; (void)text; return false; }
__attribute__((weak)) bool loop_sysfs_attr(struct block_device* dev, unsigned index, char* text) { (void)dev; (void)index; (void)text; return false; }
__attribute__((weak)) bool md_sysfs_attr(struct block_device* dev, unsigned index, char* text) { (void)dev; (void)index; (void)text; return false; }
__attribute__((weak)) unsigned md_sysfs_members(struct block_device* dev, struct block_partition** out, unsigned capacity) { (void)dev; (void)out; (void)capacity; return 0; }
__attribute__((weak)) bool md_sysfs_member_attr(struct block_device* dev, unsigned member, unsigned index, char* text) { (void)dev; (void)member; (void)index; (void)text; return false; }
__attribute__((weak)) unsigned dm_sysfs_slaves(struct block_device* dev, struct block_device** out, unsigned capacity) { (void)dev; (void)out; (void)capacity; return 0; }
__attribute__((weak)) unsigned storage_holders(struct block_device* dev, struct block_device** out, unsigned capacity) { (void)dev; (void)out; (void)capacity; return 0; }
static bool is_dm(const struct block_device* d) { return d->kind && !strcmp(d->kind, "DM"); }
static bool is_loop(const struct block_device* d) { return d->kind && !strcmp(d->kind, "loop"); }
static bool is_md(const struct block_device* d) { return d->kind && !strcmp(d->kind, "MD"); }
/* disks with a device/ directory (vendor, model, serial, ...): SCSI-like ones and NVMe namespaces (Linux: the controller) */
static bool scsi_like(const struct block_device* d) { return d->kind && (!strcmp(d->kind, "USB") || !strcmp(d->kind, "UAS") || !strcmp(d->kind, "AHCI") || !strcmp(d->kind, "IDE") || !strcmp(d->kind, "NVMe")); }
static uint32_t make(unsigned kind, unsigned slot, unsigned item) { return BLK_SYSFS_BASE | kind << 22 | slot << 12 | item; }
bool blk_sysfs_path(const char* path) {
    static const char* const roots[4] = {"/sys/block", "/sys/class/block", "/sys/dev/block", "/sys/devices/virtual/block"};
    for (unsigned i = 0; i < 4; i++) { unsigned n = strlen(roots[i]); if (!strncmp(path, roots[i], n) && (!path[n] || path[n] == '/')) return true; }
    return false;
}
static int disk_slot_named(const char* name, unsigned length) {
    sync_nodes();
    for (unsigned s = N_SPECIAL; s < SLOTS; s++) {
        struct node* n = &nodes[s];
        if (n->live && !n->alias && !n->dev->removed && n->part && strlen(n->name) == length && !strncmp(n->name, name, length)) return (int)s;
    }
    return -1;
}
static int devnum_slot(const char* text, unsigned length) {
    sync_nodes();
    for (unsigned s = N_SPECIAL; s < SLOTS; s++) {
        struct node* n = &nodes[s];
        if (!n->live || n->alias || n->dev->removed || !n->part) continue;
        char want[24], number[12]; uint_to_str(n->part->major, number); strcpy(want, number); strcat(want, ":"); uint_to_str(n->part->minor, number); strcat(want, number);
        if (strlen(want) == length && !strncmp(want, text, length)) return (int)s;
    }
    return -1;
}
static void target_of(unsigned slot, char* out) {
    struct node* n = &nodes[slot];
    strcpy(out, "/sys/devices/virtual/block/"); strcat(out, n->dev->linux_name);
    if (n->part && n->part->number) { strcat(out, "/"); strcat(out, n->part->linux_name); }
}
static int index_of(const char* const* list, unsigned count, const char* name, unsigned length) {
    for (unsigned i = 0; i < count; i++) if (strlen(list[i]) == length && !strncmp(list[i], name, length)) return (int)i;
    return -1;
}
/* Resolve within a disk or partition directory: component(s) after /sys/devices/virtual/block/<disk>. */
static int resolve_inside(unsigned slot, const char* rest, bool follow, uint32_t* node, unsigned hops);
static int part_slot(const struct block_partition* p) {
    for (unsigned s = N_SPECIAL; s < SLOTS; s++) if (nodes[s].live && !nodes[s].alias && nodes[s].part == p && !nodes[s].dev->removed) return (int)s;
    return -1;
}
/* /sys/block/mdN/md/dev-<member>[/attr | /block[/...]] (mdadm's sysfs_read GET_DEVS) */
static int resolve_md_member(unsigned slot, const char* name, unsigned length, const char* rest, bool follow, uint32_t* node, unsigned hops) {
    struct block_partition* parts[16]; unsigned m = md_sysfs_members(nodes[slot].dev, parts, 16);
    for (unsigned k = 0; k < m; k++) {
        if (strlen(parts[k]->linux_name) != length || strncmp(parts[k]->linux_name, name, length)) continue;
        if (!*rest) { *node = make(K_DIR, slot, D_MDDEV << 8 | k << 4); return 0; }
        const char* leaf = rest + 1; const char* stop = leaf; while (*stop && *stop != '/') stop++;
        unsigned n = (unsigned)(stop - leaf); int i;
        if ((i = index_of(md_dev_attrs, COUNT(md_dev_attrs), leaf, n)) >= 0) { if (*stop) return -20; *node = make(K_ATTR, slot, D_MDDEV << 8 | k << 4 | (unsigned)(i + 1)); return 0; }
        if (n != 5 || strncmp(leaf, "block", 5)) return -2;
        if (!*stop && !follow) { *node = make(K_LINK, slot, L_MDBLOCK | k << 4); return 0; }
        int target = part_slot(parts[k]);
        return target < 0 ? -2 : resolve_inside((unsigned)target, stop, follow, node, hops + 1);
    }
    return -2;
}
int blk_sysfs_resolve(const char* path, bool follow, uint32_t* node) {
    if (!nodes || !blk_sysfs_path(path)) return -2;
    char work[256]; if (strlen(path) >= sizeof(work)) return -36;
    strcpy(work, path); unsigned length = strlen(work); while (length > 4 && work[length - 1] == '/') work[--length] = 0;
    struct { const char* prefix; unsigned root, link; } roots[4] = {{"/sys/block", R_BLOCK, L_BLOCK}, {"/sys/class/block", R_CLASS, L_CLASS},
                                                                   {"/sys/dev/block", R_DEV, L_DEV}, {"/sys/devices/virtual/block", R_VIRTUAL, 0}};
    for (unsigned r = 0; r < 4; r++) {
        unsigned n = strlen(roots[r].prefix);
        if (strncmp(work, roots[r].prefix, n) || (work[n] && work[n] != '/')) continue;
        if (!work[n]) { *node = make(K_ROOT, 0, roots[r].root); return 0; }
        const char* name = work + n + 1; const char* end = name; while (*end && *end != '/') end++;
        int slot = roots[r].root == R_DEV ? devnum_slot(name, (unsigned)(end - name)) : disk_slot_named(name, (unsigned)(end - name));
        if (slot < 0) return -2;
        if (roots[r].root == R_BLOCK && nodes[slot].part->number) return -2;   /* /sys/block lists disks only */
        if (roots[r].root == R_VIRTUAL && nodes[slot].part->number) return -2; /* partitions live under their disk */
        if (roots[r].link && !*end && !follow) { *node = make(K_LINK, (unsigned)slot, roots[r].link); return 0; }
        return resolve_inside((unsigned)slot, end, follow, node, 0);
    }
    return -2;
}
static int resolve_inside(unsigned slot, const char* rest, bool follow, uint32_t* node, unsigned hops) {
    if (hops > 8) return -40;
    struct node* n = &nodes[slot]; bool partition = n->part->number != 0;
    if (!*rest) { *node = make(partition ? K_PART : K_DISK, slot, 0); return 0; }
    const char* name = rest + 1; const char* end = name; while (*end && *end != '/') end++;
    unsigned length = (unsigned)(end - name); int i;
    const char* const* attrs = partition ? part_attrs : disk_attrs; unsigned count = partition ? COUNT(part_attrs) : COUNT(disk_attrs);
    if ((i = index_of(attrs, count, name, length)) >= 0) { if (*end) return -20; *node = make(K_ATTR, slot, (unsigned)i); return 0; }
    if (length == 9 && !strncmp(name, "subsystem", 9)) { if (*end) return -2; *node = make(K_LINK, slot, L_SUBSYSTEM); return 0; }
    unsigned dir = 0; const char* const* list = NULL; unsigned list_count = 0;
    if (length == 7 && !strncmp(name, "holders", 7)) dir = D_HOLDERS;
    else if (length == 6 && !strncmp(name, "slaves", 6)) dir = D_SLAVES;
    else if (!partition && length == 5 && !strncmp(name, "queue", 5)) { dir = D_QUEUE; list = queue_attrs; list_count = COUNT(queue_attrs); }
    else if (!partition && length == 6 && !strncmp(name, "device", 6) && scsi_like(n->dev)) { dir = D_DEVICE; list = device_attrs; list_count = COUNT(device_attrs); }
    else if (!partition && length == 2 && !strncmp(name, "dm", 2) && is_dm(n->dev)) { dir = D_DM; list = dm_attrs; list_count = COUNT(dm_attrs); }
    else if (!partition && length == 4 && !strncmp(name, "loop", 4) && is_loop(n->dev)) { dir = D_LOOP; list = loop_attrs; list_count = COUNT(loop_attrs); }
    else if (!partition && length == 2 && !strncmp(name, "md", 2) && is_md(n->dev)) { dir = D_MD; list = md_attrs; list_count = COUNT(md_attrs); }
    if (dir) {
        if (!*end) { *node = make(K_DIR, slot, dir << 8); return 0; }
        const char* leaf = end + 1; const char* stop = leaf; while (*stop && *stop != '/') stop++;
        if (dir == D_MD && !strncmp(leaf, "dev-", 4)) return resolve_md_member(slot, leaf + 4, (unsigned)(stop - leaf - 4), stop, follow, node, hops);
        if (*stop) return -20;
        if (list && (i = index_of(list, list_count, leaf, (unsigned)(stop - leaf))) >= 0) { *node = make(K_ATTR, slot, dir << 8 | (unsigned)(i + 1)); return 0; }
        if (dir == D_HOLDERS || dir == D_SLAVES) {
            struct block_device* devs[8]; unsigned m = dir == D_HOLDERS ? storage_holders(n->dev, devs, 8) : dm_sysfs_slaves(n->dev, devs, 8);
            for (unsigned k = 0; k < m; k++) if (strlen(devs[k]->linux_name) == (unsigned)(stop - leaf) && !strncmp(devs[k]->linux_name, leaf, (unsigned)(stop - leaf))) {
                *node = make(K_LINK, slot, (dir == D_HOLDERS ? L_HOLDER : L_SLAVE) | k << 4); return 0;
            }
        }
        return -2;
    }
    if (!partition) {   /* partition subdirectory */
        for (unsigned s = N_SPECIAL; s < SLOTS; s++) {
            struct node* p = &nodes[s];
            if (p->live && !p->alias && p->dev == n->dev && p->part && p->part->number && strlen(p->name) == length && !strncmp(p->name, name, length))
                return resolve_inside(s, end, follow, node, hops + 1);
        }
    }
    return -2;
}
static bool blk_node_valid(uint32_t id, unsigned* kind, unsigned* slot, unsigned* item) {
    if (id < BLK_SYSFS_BASE || !nodes) return false;
    *kind = (id >> 22) & 0xFF; *slot = (id >> 12) & 0x3FF; *item = id & 0xFFF;
    if (*kind == K_ROOT) return true;
    return *slot < SLOTS && nodes[*slot].live && nodes[*slot].part && !nodes[*slot].dev->removed;
}
bool blk_sysfs_directory(uint32_t id) { unsigned k, s, i; return blk_node_valid(id, &k, &s, &i) && (k == K_ROOT || k == K_DISK || k == K_PART || k == K_DIR); }
bool blk_sysfs_link(uint32_t id) { unsigned k, s, i; return blk_node_valid(id, &k, &s, &i) && k == K_LINK; }
int blk_sysfs_stat(uint32_t id, struct linux_stat* st) {
    unsigned k, s, i; if (!blk_node_valid(id, &k, &s, &i)) return -2;
    memset(st, 0, sizeof(*st)); st->dev = 12; st->ino = id; st->block_size = 4096;
    st->uid = credentials_visible_id(0, false); st->gid = credentials_visible_id(0, true);
    if (k == K_LINK) { st->mode = 0120777; st->nlink = 1; st->size = 64; }
    else if (k == K_ATTR) { st->mode = 0100444; st->nlink = 1; st->size = 4096; }
    else { st->mode = 0040755; st->nlink = 2; }
    return 0;
}
static void link_target(unsigned slot, unsigned item, char* out) {
    struct node* n = &nodes[slot]; out[0] = 0;
    unsigned kind = item & 15;
    if (kind == L_BLOCK || kind == L_CLASS || kind == L_DEV) { target_of(slot, out); return; }
    if (kind == L_SUBSYSTEM) { strcpy(out, "/sys/class/block"); return; }
    if (kind == L_MDBLOCK) {
        struct block_partition* parts[16]; unsigned m = md_sysfs_members(n->dev, parts, 16); int t = (item >> 4) < m ? part_slot(parts[item >> 4]) : -1;
        if (t >= 0) target_of((unsigned)t, out);
        return;
    }
    struct block_device* devs[8]; unsigned m = kind == L_HOLDER ? storage_holders(n->dev, devs, 8) : dm_sysfs_slaves(n->dev, devs, 8);
    unsigned k = item >> 4;
    if (k < m) { strcpy(out, "/sys/devices/virtual/block/"); strcat(out, devs[k]->linux_name); }
}
int blk_sysfs_readlink(uint32_t id, char* result, unsigned capacity) {
    unsigned k, s, i; if (!blk_node_valid(id, &k, &s, &i) || k != K_LINK) return -22;
    char target[128]; link_target(s, i, target);
    unsigned n = strlen(target); if (!n) return -2; if (n >= capacity) return -36;
    memcpy(result, target, n + 1); return (int)n;
}
static void cat_number(char* text, uint64_t value) { char n[24]; number64(value, n); strcat(text, n); }
static void attribute_text(unsigned slot, unsigned item, char* text) {
    struct node* n = &nodes[slot]; struct block_device* d = n->dev; struct block_partition* p = n->part;
    unsigned dir = item >> 8, index = item & 0xFF; text[0] = 0;
    uint32_t physical = d->physical_block_size ? d->physical_block_size : d->block_size;
    if (!dir) {
        const char* name = p->number ? part_attrs[index] : disk_attrs[index];
        if (!strcmp(name, "dev")) { cat_number(text, p->major); strcat(text, ":"); cat_number(text, p->minor); }
        else if (!strcmp(name, "size")) cat_number(text, block_part_sectors(p));
        else if (!strcmp(name, "ro")) strcat(text, d->read_only ? "1" : "0");
        else if (!strcmp(name, "removable")) strcat(text, d->removable ? "1" : "0");
        else if (!strcmp(name, "uevent")) {
            strcat(text, "MAJOR="); cat_number(text, p->major); strcat(text, "\nMINOR="); cat_number(text, p->minor);
            strcat(text, "\nDEVNAME="); strcat(text, p->linux_name); strcat(text, p->number ? "\nDEVTYPE=partition" : "\nDEVTYPE=disk");
            if (p->number) { strcat(text, "\nPARTN="); cat_number(text, p->number); if (p->name[0]) { strcat(text, "\nPARTNAME="); strcat(text, p->name); } }
            else { strcat(text, "\nDISKSEQ="); cat_number(text, d->generation); }
            /* + what udev's database would hold (fsprobe.c): libudev reads it here; never probe a suspended dm device */
            { void fsprobe_udev_properties(struct block_partition* part, char* out, unsigned capacity);
              if (!dm_device_suspended(d)) fsprobe_udev_properties(p, text, 510);   /* blk_sysfs_read's 512-byte buffer, final "\n" */
              if (d->kind && (!strcmp(d->kind, "USB") || !strcmp(d->kind, "UAS")) && strlen(text) < 498) strcat(text, "\nID_BUS=usb"); }
        } else if (!strcmp(name, "stat")) {
            const struct block_stats* s = &d->stats; uint64_t ms = block_tsc_hz() ? s->busy_ticks * 1000 / block_tsc_hz() : 0;
            uint64_t v[17] = {s->read_cmds, 0, s->read_sectors, ms, s->write_cmds, 0, s->write_sectors, 0, 0, ms, ms, 0, 0, 0, 0, s->flushes, 0};
            for (unsigned k = 0; k < 17; k++) { char f[24]; number64(v[k], f); for (unsigned w = strlen(f); w < 8; w++) strcat(text, " "); strcat(text, f); strcat(text, " "); }
        } else if (!strcmp(name, "inflight")) strcat(text, "       0        0");
        else if (!strcmp(name, "capability")) strcat(text, d->removable ? "1" : "0");
        else if (!strcmp(name, "range")) strcat(text, d->major == 8 ? "16" : "1");
        else if (!strcmp(name, "ext_range")) {   /* partitions the disk may carry (libparted skips BLKPG when 1) */
            char scan[8] = "";
            bool parts = !is_dm(d) && (!is_loop(d) || (loop_sysfs_attr(d, 4, scan) && scan[0] == '1'));
            strcat(text, parts ? "16" : "1");
        }
        else if (!strcmp(name, "partition")) cat_number(text, p->number);
        else if (!strcmp(name, "start")) cat_number(text, p->start * (d->block_size / 512));
        else if (!strcmp(name, "diskseq")) cat_number(text, d->generation);
        else if (!strcmp(name, "events") || !strcmp(name, "events_async")) { if (d->removable) strcat(text, "media_change"); }
        else if (!strcmp(name, "events_poll_msecs")) strcat(text, d->removable ? "2000" : "-1");
        else strcat(text, "0");   /* hidden, alignment_offset, discard_alignment */
        strcat(text, "\n"); return;
    }
    if (dir == D_MDDEV) { if ((index & 15) && md_sysfs_member_attr(d, index >> 4, (index & 15) - 1, text)) strcat(text, "\n"); return; }
    if (!index) return;
    index--;
    if (dir == D_QUEUE) {
        const char* name = queue_attrs[index];
        uint64_t max_kb = (uint64_t)d->max_blocks * d->block_size / 1024;
        if (!strcmp(name, "logical_block_size") || !strcmp(name, "hw_sector_size")) cat_number(text, d->block_size);
        else if (!strcmp(name, "physical_block_size") || !strcmp(name, "minimum_io_size")) cat_number(text, physical);
        else if (!strcmp(name, "rotational")) strcat(text, d->rotational ? "1" : "0");
        else if (!strcmp(name, "max_sectors_kb") || !strcmp(name, "max_hw_sectors_kb")) cat_number(text, max_kb ? max_kb : 64);
        else if (!strcmp(name, "discard_granularity")) cat_number(text, d->discard_granularity);
        else if (!strcmp(name, "discard_max_bytes") || !strcmp(name, "discard_max_hw_bytes")) cat_number(text, d->discard_granularity ? 0xFFFFFFFFULL * 512 : 0);
        else if (!strcmp(name, "read_ahead_kb")) strcat(text, "128");
        else if (!strcmp(name, "nr_requests")) strcat(text, "64");
        else if (!strcmp(name, "scheduler")) strcat(text, "[none]");
        else if (!strcmp(name, "zoned")) strcat(text, "none");
        else if (!strcmp(name, "write_cache")) strcat(text, "write back");
        else if (!strcmp(name, "max_segments")) strcat(text, "128");
        else if (!strcmp(name, "max_segment_size")) strcat(text, "65536");
        else strcat(text, "0");
    } else if (dir == D_DEVICE) {
        const char* name = device_attrs[index];
        if (!strcmp(name, "vendor")) strcat(text, d->vendor[0] ? d->vendor : "ATA");
        else if (!strcmp(name, "model")) { const char* m = d->model; unsigned vn = strlen(d->vendor); if (vn && !strncmp(m, d->vendor, vn) && m[vn] == ' ') m += vn + 1; strcat(text, m); }
        else if (!strcmp(name, "rev")) strcat(text, "1.0");
        else if (!strcmp(name, "type")) strcat(text, "0");
        else if (!strcmp(name, "state")) strcat(text, "running");
        else if (!strcmp(name, "serial")) strcat(text, d->serial);
        else if (!strcmp(name, "queue_depth")) strcat(text, "1");
        else strcat(text, "7");
    } else if (dir == D_DM) { if (!dm_sysfs_attr(d, index, text)) return; }
    else if (dir == D_LOOP) { if (!loop_sysfs_attr(d, index, text)) return; }
    else if (dir == D_MD) { if (!md_sysfs_attr(d, index, text)) return; }
    strcat(text, "\n");
}
int64_t blk_sysfs_read(uint32_t id, uint64_t address, uint64_t count, uint64_t offset) {
    unsigned k, s, i; if (!blk_node_valid(id, &k, &s, &i)) return -2;
    if (k != K_ATTR) return k == K_LINK ? -22 : -21;
    char text[512]; attribute_text(s, i, text);
    uint64_t length = strlen(text);
    if (offset >= length || !count) return 0;
    uint64_t n = length - offset < count ? length - offset : count;
    if (!accessible(address, n, true)) return -14;
    return copy_to_user((void*)address, text + offset, n) ? -14 : (int64_t)n;
}
int blk_sysfs_entry(uint32_t id, uint64_t position, struct directory_item* item) {
    unsigned k, s, i; if (!blk_node_valid(id, &k, &s, &i) || !blk_sysfs_directory(id)) return -20;
    memset(item, 0, sizeof(*item)); item->next = position + 1;
    if (position < 2) return fill(item, position ? ".." : ".", position ? 1 : id, 4);
    uint64_t index = position - 2;
    if (k == K_ROOT) {
        sync_nodes();
        for (unsigned t = N_SPECIAL; t < SLOTS; t++) {
            struct node* n = &nodes[t];
            if (!n->live || n->alias || n->dev->removed || !n->part) continue;
            bool partition = n->part->number != 0;
            if ((i == R_BLOCK || i == R_VIRTUAL) && partition) continue;
            if (index--) continue;
            if (i == R_DEV) { char name[24], number[12]; uint_to_str(n->part->major, number); strcpy(name, number); strcat(name, ":"); uint_to_str(n->part->minor, number); strcat(name, number); return fill(item, name, make(K_LINK, t, L_DEV), 10); }
            return fill(item, n->name, i == R_VIRTUAL ? make(K_DISK, t, 0) : make(K_LINK, t, i == R_BLOCK ? L_BLOCK : L_CLASS), i == R_VIRTUAL ? 4 : 10);
        }
        return 0;
    }
    struct node* n = &nodes[s];
    if (k == K_DISK || k == K_PART) {
        bool partition = k == K_PART;
        const char* const* attrs = partition ? part_attrs : disk_attrs; unsigned count = partition ? COUNT(part_attrs) : COUNT(disk_attrs);
        if (index < count) return fill(item, attrs[index], make(K_ATTR, s, (unsigned)index), 8);
        index -= count;
        const char* dirs[6]; unsigned codes[6], m = 0;
        dirs[m] = "holders"; codes[m++] = D_HOLDERS; dirs[m] = "slaves"; codes[m++] = D_SLAVES;
        if (!partition) { dirs[m] = "queue"; codes[m++] = D_QUEUE; if (scsi_like(n->dev)) { dirs[m] = "device"; codes[m++] = D_DEVICE; }
                          if (is_dm(n->dev)) { dirs[m] = "dm"; codes[m++] = D_DM; } if (is_loop(n->dev)) { dirs[m] = "loop"; codes[m++] = D_LOOP; }
                          if (is_md(n->dev)) { dirs[m] = "md"; codes[m++] = D_MD; } }
        if (index < m) return fill(item, dirs[index], make(K_DIR, s, codes[index] << 8), 4);
        index -= m;
        if (!index) return fill(item, "subsystem", make(K_LINK, s, L_SUBSYSTEM), 10);
        index--;
        if (!partition) for (unsigned t = N_SPECIAL; t < SLOTS; t++) {
            struct node* p = &nodes[t];
            if (!p->live || p->alias || p->dev != n->dev || !p->part || !p->part->number) continue;
            if (!index--) return fill(item, p->name, make(K_PART, t, 0), 4);
        }
        return 0;
    }
    unsigned dir = i >> 8;
    const char* const* list = dir == D_QUEUE ? queue_attrs : dir == D_DEVICE ? device_attrs : dir == D_DM ? dm_attrs : dir == D_LOOP ? loop_attrs : dir == D_MD ? md_attrs : NULL;
    unsigned count = dir == D_QUEUE ? COUNT(queue_attrs) : dir == D_DEVICE ? COUNT(device_attrs) : dir == D_DM ? COUNT(dm_attrs) : dir == D_LOOP ? COUNT(loop_attrs) : dir == D_MD ? COUNT(md_attrs) : 0;
    if (dir == D_MDDEV) {
        unsigned k = (i >> 4) & 15;
        if (index < COUNT(md_dev_attrs)) return fill(item, md_dev_attrs[index], make(K_ATTR, s, D_MDDEV << 8 | k << 4 | (unsigned)(index + 1)), 8);
        return index == COUNT(md_dev_attrs) ? fill(item, "block", make(K_LINK, s, L_MDBLOCK | k << 4), 10) : 0;
    }
    if (dir == D_MD && index >= count) {
        struct block_partition* parts[16]; unsigned m = md_sysfs_members(n->dev, parts, 16), k = (unsigned)(index - count);
        if (k >= m) return 0;
        char name[48] = "dev-"; strcat(name, parts[k]->linux_name); return fill(item, name, make(K_DIR, s, D_MDDEV << 8 | k << 4), 4);
    }
    if (list) return index < count ? fill(item, list[index], make(K_ATTR, s, dir << 8 | (unsigned)(index + 1)), 8) : 0;
    struct block_device* devs[8]; unsigned m = dir == D_HOLDERS ? storage_holders(n->dev, devs, 8) : dm_sysfs_slaves(n->dev, devs, 8);
    if (index < m) return fill(item, devs[index]->linux_name, make(K_LINK, s, (dir == D_HOLDERS ? L_HOLDER : L_SLAVE) | (unsigned)index << 4), 10);
    return 0;
}
int blk_sysfs_node_path(uint32_t id, char* result, unsigned capacity) {
    unsigned k, s, i; if (!blk_node_valid(id, &k, &s, &i)) return -2;
    char text[160];
    if (k == K_ROOT) strcpy(text, i == R_BLOCK ? "/sys/block" : i == R_CLASS ? "/sys/class/block" : i == R_DEV ? "/sys/dev/block" : "/sys/devices/virtual/block");
    else target_of(s, text);
    unsigned n = strlen(text); if (n >= capacity) return -36;
    memcpy(result, text, n + 1); return (int)n;
}
