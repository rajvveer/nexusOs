/* Dynamic sysfs objects of the hot-plug hardware classes (input, video4linux): what libudev's enumerator reads.
 *
 * An object is /sys/devices/virtual/<class>/<name> (a top object) or <parent path>/<name> (a child, e.g. event3 and js0
 * below input7). It holds the attributes its owner lists (a name with one '/' lives in a subdirectory: id/vendor,
 * capabilities/key), a "subsystem" link to /sys/class/<class>, children, and for children a "device" link to the
 * parent. /sys/class/<class>/<name> links to every object, /sys/bus/input/devices/<name> to the top input objects.
 * Attribute text comes from the owner's read callback at read time. sysfs.c delegates every path below the roots and
 * every node number >= HW_SYSFS_BASE here. Objects change under the kernel lock (USB worker, uinput ioctls). */
#include "hwnode.h"
#include "uaccess.h"
int sysfs_add(const char* path, unsigned type, unsigned id, const char* target);   /* sysfs.c: static directory */
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

#define OBJECTS 96U
#define ATTRIBUTES 40U
enum { K_ROOT = 1, K_OBJECT, K_ATTR, K_SUBDIR, K_LINK_SUBSYSTEM, K_LINK_DEVICE, K_LINK_CLASS, K_LINK_BUS };
struct object {
    bool used; char class_name[16], name[24]; int parent; unsigned generation;
    const char* const* attributes; unsigned attribute_count; hwsys_read read; void* context;
};
static struct object objects[OBJECTS];
struct root { const char* path; const char* class_name; unsigned kind; };   /* kind: 0 devices, 1 class, 2 bus dir, 3 bus devices */
static const struct root ROOTS[] = {
    {"/sys/devices/virtual/input", "input", 0}, {"/sys/class/input", "input", 1}, {"/sys/bus/input", "input", 2}, {"/sys/bus/input/devices", "input", 3},
    {"/sys/devices/virtual/video4linux", "video4linux", 0}, {"/sys/class/video4linux", "video4linux", 1},
};
#define ROOT_COUNT (sizeof(ROOTS) / sizeof(ROOTS[0]))
static bool static_dirs;

static uint32_t make(unsigned kind, unsigned object, unsigned index) { return HW_SYSFS_BASE | (uint32_t)kind << 20 | (uint32_t)object << 8 | index; }
static unsigned kind_of(uint32_t node) { return (node >> 20) & 15; }
static unsigned object_of(uint32_t node) { return (node >> 8) & 0xFFF; }
static unsigned index_of(uint32_t node) { return node & 0xFF; }
static bool live(unsigned object) { return object < OBJECTS && objects[object].used; }

int hwsys_add(const char* class_name, const char* name, int parent, const char* const* attributes, unsigned attribute_count, hwsys_read read, void* context) {
    if (!static_dirs) {   /* listings of /sys/class etc. show the class directories */
        static_dirs = true;
        for (unsigned r = 0; r < ROOT_COUNT; r++) sysfs_add(ROOTS[r].path, 0, 0, NULL);
    }
    if (!class_name || !name || strlen(class_name) >= 16 || strlen(name) >= 24 || attribute_count > ATTRIBUTES) return -1;
    if (parent >= 0 && !live((unsigned)parent)) return -1;
    for (unsigned i = 0; i < OBJECTS; i++) if (!objects[i].used) {
        struct object* o = &objects[i]; unsigned generation = o->generation + 1;
        memset(o, 0, sizeof(*o)); o->used = true; o->generation = generation; strcpy(o->class_name, class_name); strcpy(o->name, name);
        o->parent = parent; o->attributes = attributes; o->attribute_count = attribute_count; o->read = read; o->context = context;
        return (int)i;
    }
    vga_print("[64] HWSYSFS object table full\n");
    return -1;
}
void hwsys_remove(int id) {
    if (id < 0 || !live((unsigned)id)) return;
    for (unsigned i = 0; i < OBJECTS; i++) if (objects[i].used && objects[i].parent == id) hwsys_remove((int)i);
    objects[id].used = false;
}
/* Full /sys path of an object. */
static int object_path(unsigned object, char* out, unsigned capacity) {
    const struct object* o = &objects[object];
    char tail[160] = ""; unsigned chain[8], depth = 0; int at = (int)object;
    while (at >= 0 && depth < 8) { chain[depth++] = (unsigned)at; at = objects[at].parent; }
    if (at >= 0) return -40;
    for (unsigned i = depth; i-- > 0;) { if (strlen(tail) + strlen(objects[chain[i]].name) + 2 > sizeof(tail)) return -36; strcat(tail, "/"); strcat(tail, objects[chain[i]].name); }
    char path[256] = "/sys/devices/virtual/"; strcat(path, o->class_name); (void)o;
    if (strlen(path) + strlen(tail) + 1 > capacity) return -36;
    strcpy(out, path); strcat(out, tail); return (int)strlen(out);
}
bool hw_sysfs_path(const char* path) {
    for (unsigned r = 0; r < ROOT_COUNT; r++) { unsigned n = strlen(ROOTS[r].path); if (!strncmp(path, ROOTS[r].path, n) && (!path[n] || path[n] == '/')) return true; }
    return false;
}
static bool has_subdir(const struct object* o, const char* name, unsigned length) {
    for (unsigned a = 0; a < o->attribute_count; a++) { const char* slash = strstr(o->attributes[a], "/"); if (slash && (unsigned)(slash - o->attributes[a]) == length && !strncmp(o->attributes[a], name, length)) return true; }
    return false;
}
/* Resolve below an object: rest is "" or "/x[/y...]". */
static int resolve_inside(unsigned object, const char* rest, bool follow, uint32_t* node, unsigned hops);
static int resolve_link_target(unsigned object, const char* rest, bool follow, uint32_t* node, unsigned hops) { return resolve_inside(object, rest, follow, node, hops + 1); }
static int resolve_inside(unsigned object, const char* rest, bool follow, uint32_t* node, unsigned hops) {
    if (hops > 8) return -40;
    if (!*rest) { *node = make(K_OBJECT, object, 0); return 0; }
    const char* name = rest + 1; const char* end = name; while (*end && *end != '/') end++;
    unsigned length = (unsigned)(end - name); struct object* o = &objects[object];
    for (unsigned a = 0; a < o->attribute_count; a++) if (strlen(o->attributes[a]) == length && !strncmp(o->attributes[a], name, length)) {
        if (*end) return -20; *node = make(K_ATTR, object, a); return 0;
    }
    if (has_subdir(o, name, length)) {
        if (!*end) { for (unsigned a = 0; a < o->attribute_count; a++) if (!strncmp(o->attributes[a], name, length) && o->attributes[a][length] == '/') { *node = make(K_SUBDIR, object, a); return 0; } }
        const char* leaf = end + 1;
        for (unsigned a = 0; a < o->attribute_count; a++) if (!strncmp(o->attributes[a], name, length) && o->attributes[a][length] == '/' && !strcmp(o->attributes[a] + length + 1, leaf)) { *node = make(K_ATTR, object, a); return 0; }
        return -2;
    }
    if (length == 9 && !strncmp(name, "subsystem", 9)) { if (*end) return -2; *node = make(K_LINK_SUBSYSTEM, object, 0); return 0; }
    if (length == 6 && !strncmp(name, "device", 6) && o->parent >= 0) {
        if (!*end && !follow) { *node = make(K_LINK_DEVICE, object, 0); return 0; }
        return resolve_link_target((unsigned)o->parent, end, follow, node, hops);
    }
    for (unsigned i = 0; i < OBJECTS; i++) if (objects[i].used && objects[i].parent == (int)object && strlen(objects[i].name) == length && !strncmp(objects[i].name, name, length))
        return resolve_inside(i, end, follow, node, hops);
    return -2;
}
int hw_sysfs_resolve(const char* original, bool follow, uint32_t* node) {
    char path[256]; if (strlen(original) >= sizeof(path)) return -36;
    strcpy(path, original); unsigned length = strlen(path); while (length > 4 && path[length - 1] == '/') path[--length] = 0;
    int best = -1; unsigned best_length = 0;
    for (unsigned r = 0; r < ROOT_COUNT; r++) { unsigned n = strlen(ROOTS[r].path); if (!strncmp(path, ROOTS[r].path, n) && (!path[n] || path[n] == '/') && n > best_length) { best = (int)r; best_length = n; } }
    if (best < 0) return -2;
    const struct root* root = &ROOTS[best]; const char* rest = path + best_length;
    if (!*rest) { *node = make(K_ROOT, 0, (unsigned)best); return 0; }
    const char* name = rest + 1; const char* end = name; while (*end && *end != '/') end++; unsigned n = (unsigned)(end - name);
    if (root->kind == 2) {                                               /* /sys/bus/input: only "devices" (its own root) */
        return -2;
    }
    for (unsigned i = 0; i < OBJECTS; i++) {
        struct object* o = &objects[i];
        if (!o->used || strcmp(o->class_name, root->class_name) || strlen(o->name) != n || strncmp(o->name, name, n)) continue;
        if (root->kind == 0 && o->parent >= 0) continue;                 /* /sys/devices/virtual/<class>/ lists top objects */
        if (root->kind == 3 && o->parent >= 0) continue;
        if (root->kind == 1 || root->kind == 3) {                       /* links */
            if (!*end && !follow) { *node = make(root->kind == 1 ? K_LINK_CLASS : K_LINK_BUS, i, 0); return 0; }
            return resolve_inside(i, end, follow, node, 1);
        }
        return resolve_inside(i, end, follow, node, 0);
    }
    return -2;
}
bool hw_sysfs_directory(uint32_t node) { unsigned k = kind_of(node); return k == K_ROOT || k == K_OBJECT || k == K_SUBDIR; }
bool hw_sysfs_link(uint32_t node) { unsigned k = kind_of(node); return k == K_LINK_SUBSYSTEM || k == K_LINK_DEVICE || k == K_LINK_CLASS || k == K_LINK_BUS; }
static bool node_live(uint32_t node) { return kind_of(node) == K_ROOT ? index_of(node) < ROOT_COUNT : live(object_of(node)); }
int hw_sysfs_readlink(uint32_t node, char* result, unsigned capacity) {
    if (!hw_sysfs_link(node) || !node_live(node)) return -22;
    unsigned object = object_of(node); char path[256];
    if (kind_of(node) == K_LINK_SUBSYSTEM) { strcpy(path, "/sys/class/"); strcat(path, objects[object].class_name); }
    else {
        unsigned target = kind_of(node) == K_LINK_DEVICE ? (unsigned)objects[object].parent : object;
        if (object_path(target, path, sizeof(path)) < 0) return -36;
    }
    unsigned size = strlen(path); if (size >= capacity) return -36;
    memcpy(result, path, size + 1); return (int)size;
}
int hw_sysfs_node_path(uint32_t node, char* result, unsigned capacity) {
    if (!node_live(node)) return -2;
    unsigned k = kind_of(node), object = object_of(node); char path[256];
    if (k == K_ROOT) strcpy(path, ROOTS[index_of(node)].path);
    else {
        if (object_path(object, path, sizeof(path)) < 0) return -36;
        const struct object* o = &objects[object];
        if (k == K_ATTR || k == K_SUBDIR) {
            const char* a = o->attributes[index_of(node)]; char piece[64]; unsigned n = strlen(a); if (n >= sizeof(piece)) return -36;
            strcpy(piece, a); if (k == K_SUBDIR) { char* slash = strstr(piece, "/"); if (slash) *slash = 0; }
            if (strlen(path) + strlen(piece) + 2 > sizeof(path)) return -36;
            strcat(path, "/"); strcat(path, piece);
        } else if (k == K_LINK_SUBSYSTEM) strcat(path, "/subsystem");
        else if (k == K_LINK_DEVICE) strcat(path, "/device");
        else if (k == K_LINK_CLASS) { strcpy(path, "/sys/class/"); strcat(path, o->class_name); strcat(path, "/"); strcat(path, o->name); }
        else if (k == K_LINK_BUS) { strcpy(path, "/sys/bus/input/devices/"); strcat(path, o->name); }
    }
    unsigned size = strlen(path); if (size >= capacity) return -36;
    memcpy(result, path, size + 1); return (int)size;
}
int hw_sysfs_stat(uint32_t node, struct linux_stat* stat) {
    if (!node_live(node)) return -2;
    memset(stat, 0, sizeof(*stat)); stat->dev = 12; stat->ino = (1ULL << 40) | (node & 0x0FFFFFFFU);   /* kind, object and index: unique */ stat->block_size = 4096;
    stat->uid = credentials_visible_id(0, false); stat->gid = credentials_visible_id(0, true);
    if (hw_sysfs_directory(node)) { stat->mode = 0040755; stat->nlink = 2; }
    else if (hw_sysfs_link(node)) { char target[256]; int n = hw_sysfs_readlink(node, target, sizeof(target)); stat->mode = 0120777; stat->nlink = 1; stat->size = n > 0 ? n : 0; }
    else { stat->mode = 0100444; stat->nlink = 1; stat->size = 4096; }
    return 0;
}
int64_t hw_sysfs_read(uint32_t node, uint64_t address, uint64_t count, uint64_t offset) {
    if (!node_live(node)) return -2;
    if (hw_sysfs_directory(node)) return -21;
    if (kind_of(node) != K_ATTR) return -22;
    struct object* o = &objects[object_of(node)];
    static char text[4096];                                              /* kernel lock held */
    int length = o->read ? o->read(o->context, o->attributes[index_of(node)], text, sizeof(text)) : 0;
    if (length < 0) return length;
    if ((uint64_t)length > sizeof(text)) length = sizeof(text);
    if (offset >= (uint64_t)length || !count) return 0;
    uint64_t n = (uint64_t)length - offset < count ? (uint64_t)length - offset : count;
    if (!accessible(address, n, true)) return -14;
    copy_to_user((void*)address, text + offset, n); return (int64_t)n;
}
static int item(struct directory_item* out, const char* name, uint32_t node, unsigned type) { strcpy(out->name, name); out->inode = 0x500000 + (node & 0xFFFFF); out->type = (uint8_t)type; return 1; }
int hw_sysfs_entry(uint32_t node, uint64_t position, struct directory_item* out) {
    if (!node_live(node) || !hw_sysfs_directory(node)) return -20;
    memset(out, 0, sizeof(*out)); out->next = position + 1;
    if (position < 2) { strcpy(out->name, position ? ".." : "."); out->inode = 0x500000 + (node & 0xFFFFF); out->type = 4; return 1; }
    uint64_t index = 2; unsigned k = kind_of(node);
    if (k == K_ROOT) {
        const struct root* root = &ROOTS[index_of(node)];
        if (root->kind == 2) return position == index ? item(out, "devices", make(K_ROOT, 0, 3), 4) : 0;
        for (unsigned i = 0; i < OBJECTS; i++) {
            const struct object* o = &objects[i];
            if (!o->used || strcmp(o->class_name, root->class_name) || ((root->kind == 0 || root->kind == 3) && o->parent >= 0)) continue;
            if (index++ == position) return item(out, o->name, make(root->kind == 1 ? K_LINK_CLASS : root->kind == 3 ? K_LINK_BUS : K_OBJECT, i, 0), root->kind ? 10 : 4);
        }
        return 0;
    }
    unsigned object = object_of(node); const struct object* o = &objects[object];
    if (k == K_SUBDIR) {
        const char* dir = o->attributes[index_of(node)]; unsigned n = (unsigned)(strstr(dir, "/") - dir);
        for (unsigned a = 0; a < o->attribute_count; a++) if (!strncmp(o->attributes[a], dir, n) && o->attributes[a][n] == '/' && index++ == position)
            return item(out, o->attributes[a] + n + 1, make(K_ATTR, object, a), 8);
        return 0;
    }
    for (unsigned a = 0; a < o->attribute_count; a++) {                  /* attributes, then each subdirectory once */
        const char* slash = strstr(o->attributes[a], "/");
        if (slash) {
            bool first = true; unsigned n = (unsigned)(slash - o->attributes[a]);
            for (unsigned b = 0; b < a; b++) if (!strncmp(o->attributes[b], o->attributes[a], n) && o->attributes[b][n] == '/') first = false;
            if (!first) continue;
            if (index++ == position) { char name[32]; if (n >= sizeof(name)) n = sizeof(name) - 1; memcpy(name, o->attributes[a], n); name[n] = 0; return item(out, name, make(K_SUBDIR, object, a), 4); }
        } else if (index++ == position) return item(out, o->attributes[a], make(K_ATTR, object, a), 8);
    }
    if (index++ == position) return item(out, "subsystem", make(K_LINK_SUBSYSTEM, object, 0), 10);
    if (o->parent >= 0 && index++ == position) return item(out, "device", make(K_LINK_DEVICE, object, 0), 10);
    for (unsigned i = 0; i < OBJECTS; i++) if (objects[i].used && objects[i].parent == (int)object && index++ == position) return item(out, objects[i].name, make(K_OBJECT, i, 0), 4);
    return 0;
}
