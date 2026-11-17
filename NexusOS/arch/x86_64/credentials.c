/* IDs are stored in the initial namespace; namespace root never implies host privilege. */
#include "native.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

#define ALL_CAPS ((1ULL << 41) - 1)
#define OVERFLOW_ID 65534U
struct id_range { uint32_t inside, kernel, count; };
struct id_map { unsigned count; struct id_range ranges[340]; };
struct user_namespace {
    struct user_namespace* parent; struct id_map *uid, *gid;
    unsigned references, depth; uint32_t owner; uint64_t id;
    bool groups_allowed, parent_setfcap;
};
struct group_list { unsigned references, count; uint32_t ids[]; };
struct credentials {
    struct user_namespace* space;
    struct group_list* groups;
    uint32_t uid[4], gid[4];
    uint64_t permitted, effective, inheritable, bounding, ambient;
    unsigned securebits; bool no_new_privs;
};
static struct user_namespace initial_namespace = {.groups_allowed = true};
static struct credentials initial_credentials = {
    .space = &initial_namespace, .uid = {1000,1000,1000,1000}, .gid = {1000,1000,1000,1000},
    .bounding = ALL_CAPS
};
static struct credentials* active = &initial_credentials;
static uint64_t next_namespace;
static void groups_release(struct group_list* groups) {
    if (!groups) return;
    if (!groups->references) panic("unowned supplementary groups");
    if (!--groups->references) arena_free((uint64_t)groups, sizeof(*groups) + (uint64_t)groups->count * 4);
}
_Static_assert(sizeof(struct id_map) <= 4096 && sizeof(struct credentials) <= 4096, "credential allocation size");
struct credentials* credentials_current(void) { return active; }
bool credentials_no_new_privs(void) { return active->no_new_privs; }
void credentials_mark_no_new_privs(struct credentials* c) { if (c) c->no_new_privs = true; }
/* Boot sessions started by the kernel (task_run) take the initial credentials: the desktop user (1000, no
 * capabilities) unless the disk asks for a root init (/nexus/init-as-root, test disks; only the host can write it). */
void credentials_boot_root(bool root) {
    for (unsigned i = 0; i < 4; i++) initial_credentials.uid[i] = initial_credentials.gid[i] = root ? 0 : 1000;
    initial_credentials.permitted = initial_credentials.effective = root ? ALL_CAPS : 0;
}
void credentials_activate(struct credentials* credentials) { active = credentials ? credentials : &initial_credentials; }
struct user_namespace* credentials_namespace(struct credentials* credentials) { return credentials->space; }
uint64_t namespace_id(struct user_namespace* space) { return space->id; }
bool namespace_groups_allowed(struct user_namespace* space) { return space->groups_allowed; }
void namespace_retain(struct user_namespace* space) {
    if (space == &initial_namespace) return;
    if (!space->references || space->references == 0xFFFFFFFFU) panic("user namespace reference overflow");
    space->references++;
}
void namespace_release(struct user_namespace* space) {
    while (space != &initial_namespace) {
        if (!space || !space->references) panic("unowned user namespace");
        if (--space->references) return;
        struct user_namespace* parent = space->parent;
        if (space->uid) page_free((uint64_t)space->uid);
        if (space->gid) page_free((uint64_t)space->gid);
        page_free((uint64_t)space); space = parent;
    }
}
static bool map_id(struct user_namespace* space, bool group, uint32_t value, bool to_kernel, uint32_t* result) {
    if (value == 0xFFFFFFFFU) return false;
    if (space == &initial_namespace) { *result = value; return true; }
    struct id_map* map = group ? space->gid : space->uid;
    for (unsigned i = 0; map && i < map->count; i++) {
        struct id_range* range = &map->ranges[i]; uint32_t from = to_kernel ? range->inside : range->kernel;
        if (value >= from && value - from < range->count) {
            *result = (to_kernel ? range->kernel : range->inside) + value - from; return true;
        }
    }
    return false;
}
uint32_t credentials_kernel_id(struct credentials* credentials, bool group, unsigned which) {
    if (which > 3) panic("invalid credential ID selector");
    return group ? credentials->gid[which] : credentials->uid[which];
}
uint32_t credentials_id(struct credentials* credentials, bool group, unsigned which) {
    uint32_t value;
    return map_id(credentials->space, group, credentials_kernel_id(credentials, group, which), false, &value) ? value : OVERFLOW_ID;
}
uint32_t credentials_visible_id(uint32_t id, bool group) {
    uint32_t value; return map_id(active->space, group, id, false, &value) ? value : OVERFLOW_ID;
}
bool credentials_capable(struct credentials* credentials, struct user_namespace* space, unsigned capability) {
    if (capability > 40) return false;
    if (!space) space = &initial_namespace;
    for (;;) {
        if (space == credentials->space) return (credentials->effective & (1ULL << capability)) != 0;
        if (!space->parent || space->depth <= credentials->space->depth) return false;
        if (space->parent == credentials->space && space->owner == credentials->uid[1]) return true;
        space = space->parent;
    }
}
bool credentials_signal_allowed(struct credentials* target) {
    return active->uid[0] == target->uid[0] || active->uid[0] == target->uid[2] ||
           active->uid[1] == target->uid[0] || active->uid[1] == target->uid[2] || credentials_capable(active, target->space, 5);
}
bool credentials_limits_allowed(struct credentials* target) {
    if (credentials_capable(active, target->space, 24)) return true;
    for (unsigned i = 0; i < 3; i++) if (target->uid[i] != active->uid[0] || target->gid[i] != active->gid[0]) return false;
    return true;
}
bool credentials_inspect_allowed(struct credentials* target, bool dumpable) {
    if (credentials_capable(active, target->space, 19)) return true;
    return dumpable && active->space == target->space && !(target->permitted & ~active->effective) &&
           active->uid[3] == target->uid[0] && active->uid[3] == target->uid[1] && active->uid[3] == target->uid[2] &&
           active->gid[3] == target->gid[0] && active->gid[3] == target->gid[1] && active->gid[3] == target->gid[2];
}
struct credentials* credentials_clone(struct credentials* source, bool new_user_namespace, int* error) {
    if (!source) source = &initial_credentials;
    *error = -12;
    struct credentials* copy = (void*)page_alloc(); if (!copy) return NULL;
    *copy = *source; namespace_retain(copy->space);
    if (copy->groups) {
        if (copy->groups->references == 0xFFFFFFFFU) panic("supplementary group reference overflow");
        copy->groups->references++;
    }
    if (new_user_namespace) {
        uint32_t mapped;
        if (!map_id(source->space, false, source->uid[1], false, &mapped) || !map_id(source->space, true, source->gid[1], false, &mapped)) {
            *error = -1; goto failed;
        }
        if (source->space->depth >= 32) { *error = -87; goto failed; }
        struct user_namespace* space = (void*)page_alloc(); if (!space) goto failed;
        space->parent = copy->space; space->references = 1; space->depth = source->space->depth + 1;
        space->owner = source->uid[1]; space->id = ++next_namespace;
        space->groups_allowed = source->space->groups_allowed;
        space->parent_setfcap = credentials_capable(source, source->space, 31);
        copy->space = space; copy->permitted = copy->effective = copy->bounding = ALL_CAPS;
        copy->inheritable = copy->ambient = 0; copy->securebits = 0;
    }
    *error = 0; return copy;
failed:
    groups_release(copy->groups); namespace_release(copy->space); page_free((uint64_t)copy); return NULL;
}
/* setns(CLONE_NEWUSER) (Linux userns_install + set_cred_user_ns): new credentials in `target` with the full
 * capability set there. Re-entering the current namespace is EINVAL; CAP_SYS_ADMIN in the target is required. */
struct credentials* credentials_enter(struct credentials* source, struct user_namespace* target, int* error) {
    if (target == source->space) { *error = -22; return NULL; }
    if (!credentials_capable(source, target, 21)) { *error = -1; return NULL; }
    struct credentials* copy = credentials_clone(source, false, error); if (!copy) return NULL;
    namespace_retain(target); namespace_release(copy->space); copy->space = target;
    copy->permitted = copy->effective = copy->bounding = ALL_CAPS;
    copy->inheritable = copy->ambient = 0; copy->securebits = 0;
    return copy;
}
void credentials_destroy(struct credentials* credentials) {
    if (!credentials || credentials == active || credentials == &initial_credentials) panic("invalid credential release");
    groups_release(credentials->groups); namespace_release(credentials->space); page_free((uint64_t)credentials);
}
/* POSIX file credentials (vfs.c) */
bool credentials_in_group(struct credentials* c, uint32_t gid, bool real) {
    if ((real ? c->gid[0] : c->gid[3]) == gid) return true;
    for (unsigned i = 0; c->groups && i < c->groups->count; i++) if (c->groups->ids[i] == gid) return true;
    return false;
}
bool credentials_access_capable(struct credentials* c, unsigned capability) {
    /* access(2) checks with the real IDs: capabilities count only for a real root */
    return capability <= 40 && c->space == &initial_namespace && !c->uid[0] && ((c->permitted >> capability) & 1);
}
bool credentials_kernel_from_visible(uint32_t id, bool group, uint32_t* result) { return map_id(active->space, group, id, true, result); }
static struct { bool uid_set, gid_set; uint32_t uid, gid; } exec_ids; /* set-ID file of the execve being committed */
void credentials_exec_setid(bool set_uid, uint32_t uid, bool set_gid, uint32_t gid) {
    exec_ids.uid_set = set_uid; exec_ids.uid = uid; exec_ids.gid_set = set_gid; exec_ids.gid = gid;
}
/* AT_SECURE of the image being built (Linux bprm->secureexec): the effective IDs differ from the real ones now, or the
 * file's set-ID bits change them (file_setid: disk_exec_secure; conservatively also when no_new_privs refuses them). */
bool credentials_exec_secure(bool file_setid) {
    return file_setid || active->uid[1] != active->uid[0] || active->gid[1] != active->gid[0];
}
#define CAP_FS_MASK ((1ULL << 0) | (1ULL << 1) | (1ULL << 2) | (1ULL << 3) | (1ULL << 4) | (1ULL << 9) | (1ULL << 27) | (1ULL << 32))
/* setuid/setgid/setreuid/setregid/setresuid/setresgid/setfsuid/setfsgid with Linux capability fixups. */
static int64_t set_ids(struct registers* r) {
    bool group = r->rax == 106 || r->rax == 114 || r->rax == 119 || r->rax == 123;
    uint32_t* ids = group ? active->gid : active->uid;
    bool privileged = credentials_capable(active, active->space, group ? 6 : 7);
    uint32_t old[4], next[4], k[3], args[3] = {(uint32_t)r->rdi, (uint32_t)r->rsi, (uint32_t)r->rdx};
    for (int i = 0; i < 4; i++) old[i] = next[i] = ids[i];
    for (int i = 0; i < 3; i++) {
        k[i] = 0xFFFFFFFFU;
        if (args[i] != 0xFFFFFFFFU && !map_id(active->space, group, args[i], true, &k[i])) {
            if (r->rax == 122 || r->rax == 123) { uint32_t v; return map_id(active->space, group, old[3], false, &v) ? v : OVERFLOW_ID; }
            return -22;
        }
    }
    switch (r->rax) {
    case 105: case 106:
        if (k[0] == 0xFFFFFFFFU) return -22;
        if (privileged) next[0] = next[1] = next[2] = next[3] = k[0];
        else if (k[0] == old[0] || k[0] == old[2]) next[1] = next[3] = k[0];
        else return -1;
        break;
    case 113: case 114:
        if (k[0] != 0xFFFFFFFFU) { if (!privileged && k[0] != old[0] && k[0] != old[1]) return -1; next[0] = k[0]; }
        if (k[1] != 0xFFFFFFFFU) { if (!privileged && k[1] != old[0] && k[1] != old[1] && k[1] != old[2]) return -1; next[1] = k[1]; }
        if (k[0] != 0xFFFFFFFFU || (k[1] != 0xFFFFFFFFU && k[1] != old[0])) next[2] = next[1];
        next[3] = next[1];
        break;
    case 117: case 119:
        for (int i = 0; i < 3; i++) if (k[i] != 0xFFFFFFFFU) {
            if (!privileged && k[i] != old[0] && k[i] != old[1] && k[i] != old[2]) return -1;
            next[i] = k[i];
        }
        next[3] = next[1];
        break;
    default: /* 122, 123: setfsuid/setfsgid return the previous value */
        if (k[0] != 0xFFFFFFFFU && (privileged || k[0] == old[0] || k[0] == old[1] || k[0] == old[2] || k[0] == old[3])) next[3] = k[0];
        break;
    }
    for (int i = 0; i < 4; i++) ids[i] = next[i];
    if (old[1] != next[1] || old[3] != next[3]) space_set_dumpable(space_current(), false);
    if (!group && !(active->securebits & 4)) {
        uint32_t root = 0; map_id(active->space, false, 0, true, &root);
        bool had_root = old[0] == root || old[1] == root || old[2] == root;
        bool no_root = next[0] != root && next[1] != root && next[2] != root;
        if (had_root && no_root && !(active->securebits & 16)) active->permitted = active->effective = active->ambient = 0;
        if (old[1] == root && next[1] != root) active->effective = 0;
        if (old[1] != root && next[1] == root) active->effective = active->permitted;
        if (old[3] == root && next[3] != root) active->effective &= ~CAP_FS_MASK;
        if (old[3] != root && next[3] == root) active->effective |= active->permitted & CAP_FS_MASK;
    }
    if (r->rax == 122 || r->rax == 123) { uint32_t v; return map_id(active->space, group, old[3], false, &v) ? v : OVERFLOW_ID; }
    return 0;
}
/* `tracer`: the credentials recorded when the ptrace link was made (Linux ptracer_cred), NULL when untraced. An
 * unsafe exec (no_new_privs, or a tracer without CAP_SYS_PTRACE over the tracee's namespace: LSM_UNSAFE_PTRACE)
 * gains no capability, and its effective IDs fall back to the real ones unless the caller has CAP_SETUID (never
 * under no_new_privs; Linux cap_bprm_creds_from_file). A set-ID or capability-raising exec makes `space`
 * non-dumpable (Linux begin_new_exec). */
struct credentials* credentials_exec_traced(struct credentials* source, struct credentials* tracer, struct address_space* space) {
    int error; struct credentials* next = credentials_clone(source, false, &error); if (!next) return NULL;
    uint32_t visible; /* the file's owner must be representable in the caller's namespace */
    if (exec_ids.uid_set && map_id(next->space, false, exec_ids.uid, false, &visible)) next->uid[1] = exec_ids.uid;
    if (exec_ids.gid_set && map_id(next->space, true, exec_ids.gid, false, &visible)) next->gid[1] = exec_ids.gid;
    uint64_t permitted = next->ambient; bool root_effective = !credentials_id(next, false, 1);
    if (!(next->securebits & 1) && (!credentials_id(next, false, 0) || root_effective))
        permitted = next->inheritable | next->bounding;
    if (next->no_new_privs || (tracer && !credentials_capable(tracer, source->space, 19))) {
        if (next->no_new_privs || !credentials_capable(source, source->space, 7)) { next->uid[1] = next->uid[0]; next->gid[1] = next->gid[0]; }
        permitted &= source->permitted;
    }
    next->permitted = permitted; next->effective = root_effective ? permitted : next->ambient;
    next->uid[2] = next->uid[3] = next->uid[1]; next->gid[2] = next->gid[3] = next->gid[1]; next->securebits &= ~16U;
    if (space && (next->uid[1] != next->uid[0] || next->gid[1] != next->gid[0] || next->uid[1] != source->uid[1] ||
                  next->gid[1] != source->gid[1] || (next->permitted & ~source->permitted))) space_set_dumpable(space, false);
    return next;
}
struct credentials* credentials_exec(struct credentials* source) { return credentials_exec_traced(source, NULL, NULL); }
/* PTRACE_TRACEME (Linux cap_ptrace_traceme): the parent holds every capability the child has, or CAP_SYS_PTRACE over it. */
bool credentials_traceme_allowed(struct credentials* tracer, struct credentials* target) {
    return (tracer->space == target->space && !(target->permitted & ~tracer->permitted)) || credentials_capable(tracer, target->space, 19);
}
static bool range_to_kernel(struct user_namespace* space, bool group, uint32_t start, uint32_t count, uint32_t* result) {
    if (space == &initial_namespace) { *result = start; return true; }
    struct id_map* map = group ? space->gid : space->uid;
    for (unsigned i = 0; map && i < map->count; i++) {
        struct id_range* range = &map->ranges[i];
        if (start >= range->inside && (uint64_t)start - range->inside + count <= range->count) {
            *result = range->kernel + start - range->inside; return true;
        }
    }
    return false;
}
static bool horizontal(char c) { return c == ' ' || c == '\t' || c == '\r'; }
static bool number(const char* text, unsigned size, unsigned* at, uint32_t* value) {
    if (*at == size || text[*at] < '0' || text[*at] > '9') return false;
    uint64_t n = 0;
    do {
        n = n * 10 + (unsigned)(text[(*at)++] - '0'); if (n > 0xFFFFFFFFULL) return false;
    } while (*at < size && text[*at] >= '0' && text[*at] <= '9');
    *value = (uint32_t)n; return true;
}
static bool map_authorized(struct user_namespace* space, struct credentials* opener, unsigned capability) {
    return space->parent && (active->space == space || active->space == space->parent) &&
           credentials_capable(active, space, capability) && credentials_capable(opener, space, capability);
}
int namespace_write_map(struct user_namespace* space, struct credentials* opener, bool group, const char* text, unsigned size, uint64_t offset) {
    struct id_map** destination = group ? &space->gid : &space->uid;
    if (!space->parent || *destination) return -1;
    if (offset || !size || size >= 4096) return -22;
    if (!map_authorized(space, opener, group ? 6 : 7)) return -1;
    struct id_map* map = (void*)page_alloc(); if (!map) return -12;
    int error = -22; unsigned at = 0; bool maps_parent_root = false;
    while (at < size) {
        while (at < size && horizontal(text[at])) at++;
        struct id_range range; uint32_t outside;
        if (map->count == 340 || !number(text, size, &at, &range.inside) || at == size || !horizontal(text[at])) goto bad;
        while (at < size && horizontal(text[at])) at++;
        if (!number(text, size, &at, &outside) || at == size || !horizontal(text[at])) goto bad;
        while (at < size && horizontal(text[at])) at++;
        if (!number(text, size, &at, &range.count) || !range.count ||
            (uint64_t)range.inside + range.count > 0xFFFFFFFFULL || (uint64_t)outside + range.count > 0xFFFFFFFFULL) goto bad;
        while (at < size && horizontal(text[at])) at++;
        if (at < size && text[at++] != '\n') goto bad;
        if (!range_to_kernel(space->parent, group, outside, range.count, &range.kernel)) { error = -1; goto bad; }
        for (unsigned i = 0; i < map->count; i++) {
            struct id_range* previous = &map->ranges[i];
            if (((uint64_t)range.inside < (uint64_t)previous->inside + previous->count && (uint64_t)previous->inside < (uint64_t)range.inside + range.count) ||
                ((uint64_t)range.kernel < (uint64_t)previous->kernel + previous->count && (uint64_t)previous->kernel < (uint64_t)range.kernel + range.count)) goto bad;
        }
        if (!outside) maps_parent_root = true;
        map->ranges[map->count++] = range;
    }
    error = -1;
    if (!map->count) { error = -22; goto bad; }
    if (!group && maps_parent_root && (active->space == space ? !space->parent_setfcap :
        !credentials_capable(active, space->parent, 31) || !credentials_capable(opener, space->parent, 31))) goto bad;
    if (!credentials_capable(active, space->parent, group ? 6 : 7) || !credentials_capable(opener, space->parent, group ? 6 : 7)) {
        if (map->count != 1 || map->ranges[0].count != 1 || map->ranges[0].kernel != (group ? active->gid[1] : active->uid[1]) ||
            active->uid[1] != space->owner || (group && space->groups_allowed)) goto bad;
    }
    *destination = map; return (int)size;
bad:
    page_free((uint64_t)map); return error;
}
int namespace_write_groups(struct user_namespace* space, struct credentials* opener, const char* text, unsigned size, uint64_t offset) {
    unsigned original = size;
    if (offset || !size || size > 6) return -22;
    if (!map_authorized(space, opener, 21) || space->gid) return -1;
    if (text[size - 1] == '\n') size--;
    if (size == 4 && !memcmp(text, "deny", 4)) space->groups_allowed = false;
    else if (size != 5 || memcmp(text, "allow", 5)) return -22;
    else if (!space->groups_allowed) return -1;
    return (int)original;
}
bool namespace_mapping(struct user_namespace* space, struct user_namespace* reader, bool group, unsigned index, uint32_t values[3]) {
    struct id_range range;
    if (space == &initial_namespace) { if (index) return false; range = (struct id_range){0,0,0xFFFFFFFFU}; }
    else {
        struct id_map* map = group ? space->gid : space->uid;
        if (!map || index >= map->count) return false;
        range = map->ranges[index];
    }
    if (reader == space && space->parent) reader = space->parent;
    uint32_t lower = 0xFFFFFFFFU; map_id(reader, group, range.kernel, false, &lower);
    values[0] = range.inside; values[1] = lower; values[2] = range.count; return true;
}
static int apply_caps(struct credentials* c, uint64_t permitted, uint64_t effective, uint64_t inheritable) {
    if ((permitted | effective | inheritable) & ~ALL_CAPS) return -22;
    uint64_t allowed = c->inheritable | (credentials_capable(c, c->space, 8) ? c->bounding : c->permitted);
    if ((effective & ~permitted) || (permitted & ~c->permitted) || (inheritable & ~allowed)) return -1;
    c->permitted = permitted; c->effective = effective; c->inheritable = inheritable; c->ambient &= permitted & inheritable; return 0;
}
int64_t credentials_call(struct registers* r) {
    if ((r->rax >= 105 && r->rax <= 106) || r->rax == 113 || r->rax == 114 || r->rax == 117 || r->rax == 119 || r->rax == 122 || r->rax == 123) return set_ids(r);
    if (r->rax == 115 || r->rax == 116) {
        int count = (int32_t)r->rdi; if (count < 0) return -22;
        unsigned have = active->groups ? active->groups->count : 0;
        if (r->rax == 115) {
            if (!count) return have;
            if ((unsigned)count < have) return -22;
            if (have && !accessible(r->rsi, (uint64_t)have * 4, true)) return -14;
            for (unsigned i = 0; i < have; i++) uput32(r->rsi + i * 4, credentials_visible_id(active->groups->ids[i], true));
            return have;
        }
        if (count > 65536) return -22;
        if (!credentials_capable(active, active->space, 6) || !active->space->groups_allowed ||
            (active->space != &initial_namespace && !active->space->gid)) return -1;
        if (count && !accessible(r->rsi, (uint64_t)count * 4, false)) return -14;
        struct group_list* groups = count ? (void*)arena_alloc(sizeof(*groups) + (uint64_t)count * 4) : NULL;
        if (count && !groups) return -12;
        if (groups) { groups->references = 1; groups->count = count; }
        for (int i = 0; i < count; i++) if (!map_id(active->space, true, uget32(r->rsi + (uint64_t)i * 4), true, &groups->ids[i])) {
            groups_release(groups); return -22;
        }
        groups_release(active->groups); active->groups = groups; return 0;
    }
    if (r->rax == 125 || r->rax == 126) {
        struct header { uint32_t version; int32_t pid; } header;
        struct data { uint32_t effective, permitted, inheritable; } data[2] = {{0}};
        if (!accessible(r->rdi, sizeof(header), false)) return -14;
        copy_from_user(&header, (void*)r->rdi, sizeof(header));
        unsigned words = header.version == 0x19980330 ? 1 : 2;
        if (header.version != 0x19980330 && header.version != 0x20071026 && header.version != 0x20080522) {
            if (!accessible(r->rdi, 4, true)) return -14;
            uput32(r->rdi, 0x20080522); return r->rax == 125 && !r->rsi ? 0 : -22;
        }
        if (header.pid < 0) return -22;
        struct credentials* target = header.pid ? task_credentials(task_internal_pid(header.pid)) : active;
        if (!target) return -3;
        if (r->rax == 126 && header.pid && header.pid != (int32_t)task_visible_pid(task_tid())) return -1;
        if (!accessible(r->rsi, words * sizeof(*data), r->rax == 125)) return -14;
        if (r->rax == 125) {
            for (unsigned i = 0; i < words; i++) data[i] = (struct data){target->effective >> (32 * i), target->permitted >> (32 * i), target->inheritable >> (32 * i)};
            return copy_to_user((void*)r->rsi, data, words * sizeof(*data)) ? -14 : 0;
        }
        copy_from_user(data, (void*)r->rsi, words * sizeof(*data));
        return apply_caps(active, (uint64_t)data[1].permitted << 32 | data[0].permitted,
                          (uint64_t)data[1].effective << 32 | data[0].effective, (uint64_t)data[1].inheritable << 32 | data[0].inheritable);
    }
    if (r->rax == 122 || r->rax == 123) { /* setfsuid/setfsgid: always return the previous value */
        bool group = r->rax == 123; uint32_t* ids = group ? active->gid : active->uid, previous = ids[3], next;
        if (map_id(active->space, group, (uint32_t)r->rdi, true, &next) &&
            (next == ids[0] || next == ids[1] || next == ids[2] || next == ids[3] || credentials_capable(active, active->space, group ? 6 : 7)))
            ids[3] = next;
        return credentials_visible_id(previous, group);
    }
    if (r->rax == 157) switch (r->rdi) {
    case 3: return space_dumpable(space_current());
    case 4: if (r->rsi > 1) return -22; space_set_dumpable(space_current(), r->rsi != 0); return 0;
    case PR_NEXUS_SEAT: return r->r10 || r->r8 ? -22 : seat_prctl(r->rsi, r->rdx);   /* the seat's console user (seat.c) */
    case 23: return r->rsi > 40 ? -22 : (active->bounding >> r->rsi) & 1;
    case 24:
        if (r->rsi > 40) return -22;
        if (!credentials_capable(active, active->space, 8)) return -1;
        active->bounding &= ~(1ULL << r->rsi); return 0;
    case 27: return active->securebits;
    case 7: return (active->securebits & 16) != 0;                                  /* PR_GET_KEEPCAPS */
    case 8:                                                                          /* PR_SET_KEEPCAPS */
        if (r->rsi > 1) return -22;
        if (active->securebits & 32) return -1;                                      /* SECBIT_KEEP_CAPS_LOCKED */
        active->securebits = (active->securebits & ~16U) | (r->rsi ? 16U : 0); return 0;
    case 28: {                                                                       /* PR_SET_SECUREBITS */
        uint64_t bits = r->rsi;
        if (bits & ~0xFFULL) return -22;
        if (!credentials_capable(active, active->space, 8)) return -1;               /* CAP_SETPCAP */
        uint32_t locks = (active->securebits & 0xAA) >> 1;                           /* a locked bit keeps its value, locks stay */
        if (((bits ^ active->securebits) & locks & 0x55) || (active->securebits & 0xAA & ~bits)) return -1;
        active->securebits = (unsigned)bits; return 0;
    }
    case 47:                                                                         /* PR_CAP_AMBIENT */
        if (r->rdi == 47 && r->rsi == 4) { if (r->rdx || r->r10 || r->r8) return -22; active->ambient = 0; return 0; }
        if (r->r10 || r->r8 || r->rdx > 40) return -22;
        if (r->rsi == 1) return (active->ambient >> r->rdx) & 1;
        if (r->rsi == 3) { active->ambient &= ~(1ULL << r->rdx); return 0; }
        if (r->rsi == 2) {
            if (!((active->permitted & active->inheritable) >> r->rdx & 1) || (active->securebits & 64)) return -1;
            active->ambient |= 1ULL << r->rdx; return 0;
        }
        return -22;
    case 38:
        if (r->rsi != 1 || r->rdx || r->r10 || r->r8) return -22;
        active->no_new_privs = true; return 0;
    case 39: return r->rsi || r->rdx || r->r10 || r->r8 ? -22 : active->no_new_privs;
    case 22: return seccomp_set_mode_prctl(r->rsi, r->rdx); /* PR_SET_SECCOMP (Linux ignores arg4/arg5 here) */
    case 21: return r->rsi || r->rdx || r->r10 || r->r8 ? -22 : seccomp_get_mode(); /* PR_GET_SECCOMP */
    case 15: {
        char name[16];
        for (unsigned i = 0; i < 15; i++) {
            if (!accessible(r->rsi + i, 1, false)) return -14;
            name[i] = (char)uget8(r->rsi + i); if (!name[i]) break;
        }
        name[15] = 0; task_set_name(name); return 0;
    }
    case 16: if (!accessible(r->rsi, 16, true)) return -14; { char name[16]; task_get_name(name); return copy_to_user((void*)r->rsi, name, 16) ? -14 : 0; }
    case 0x53564d41: return 0; /* PR_SET_VMA: anonymous mapping names are not kept */
    case 1: return kabi_pdeath_set(r->rsi);   /* PR_SET_PDEATHSIG (kabi.c) */
    case 2: return kabi_pdeath_get(r->rsi);   /* PR_GET_PDEATHSIG */
    case 0x59616d61: task_set_ptracer((int64_t)r->rsi); return 0; /* PR_SET_PTRACER (Yama): declare an allowed tracer */
    default: {
        static unsigned unsupported_logged; if (unsupported_logged >= 3) return -38; unsupported_logged++;
        char operation[24]; number64(r->rdi, operation);
        vga_print("[64] Unsupported prctl operation="); vga_print(operation); vga_print("\n"); return -38;
    }
    }
    return -38;
}
void credentials_selftest(void) {
    uint64_t before = memory_used_pages(); struct credentials* saved = active; int error;
    struct credentials* parent = credentials_clone(NULL, false, &error);
    if (!parent) panic("credential fixture allocation");
    parent->groups = (void*)arena_alloc(sizeof(struct group_list) + 8); if (!parent->groups) panic("group fixture allocation");
    parent->groups->references = 1; parent->groups->count = 2; parent->groups->ids[0] = 1000; parent->groups->ids[1] = 1001;
    struct credentials* child = credentials_clone(parent, true, &error); if (!child) panic("user namespace allocation");
    if (child->groups != parent->groups || child->groups->references != 2) panic("supplementary groups not inherited");
    credentials_activate(child);
    if (!credentials_limits_allowed(parent)) panic("same-owner resource access denied");
    parent->uid[2] = 1001;
    if (credentials_limits_allowed(parent)) panic("saved UID mismatch bypassed resource permission");
    parent->uid[2] = 1000; parent->gid[1] = 1001;
    if (credentials_limits_allowed(parent)) panic("effective GID mismatch bypassed resource permission");
    parent->gid[1] = 1000;
    if (credentials_id(child, false, 1) != OVERFLOW_ID || !credentials_capable(child, child->space, 21) ||
        credentials_capable(child, NULL, 21) || credentials_inspect_allowed(parent, true)) panic("namespace privilege boundary");
    if (namespace_write_map(child->space, child, false, "0 0 1\n", 6, 0) != -1 ||
        namespace_write_map(child->space, child, true, "0 1000 1\n", 9, 0) != -1 ||
        namespace_write_map(child->space, child, false, "0 1000 0\n", 9, 0) != -22) panic("unsafe ID map accepted");
    if (namespace_write_groups(child->space, child, "deny", 4, 0) != 4 || namespace_groups_allowed(child->space) ||
        namespace_write_groups(child->space, child, "allow", 5, 0) != -1) panic("setgroups denial is reversible");
    if (namespace_write_map(child->space, child, false, "0 1000 1\n", 9, 0) != 9 ||
        namespace_write_map(child->space, child, true, "0 1000 1\n", 9, 0) != 9 ||
        namespace_write_map(child->space, child, false, "1 1000 1\n", 9, 0) != -1) panic("ID mapping commit/one-write rule");
    if (credentials_id(child, false, 1) || credentials_id(child, true, 1) || credentials_kernel_id(child, false, 1) != 1000 ||
        credentials_visible_id(0, false) != OVERFLOW_ID) panic("namespace IDs escaped their mapping");
    uint32_t values[3];
    if (!namespace_mapping(child->space, child->space, false, 0, values) || values[0] || values[1] != 1000 || values[2] != 1)
        panic("namespace map reader view");
    if (apply_caps(child, 0, 0, 0) || apply_caps(child, 1, 1, 0) != -1) panic("capabilities can be raised after dropping");
    child->no_new_privs = true;
    struct credentials* executed = credentials_exec(child);
    if (!executed || executed->permitted || executed->effective) panic("exec ignored no_new_privs");
    credentials_destroy(executed);
    struct credentials* nested = credentials_clone(child, true, &error);
    if (!nested || namespace_groups_allowed(nested->space)) panic("nested namespace inheritance");
    credentials_activate(nested);
    if (namespace_write_map(nested->space, nested, false, "0 0 1\n", 6, 0) != -1) panic("parent root mapping bypassed SETFCAP check");
    credentials_activate(saved); credentials_destroy(nested); credentials_destroy(child); credentials_destroy(parent);
    /* set-user-ID-root exec: untraced, under an unprivileged and a CAP_SYS_PTRACE tracer (LSM_UNSAFE_PTRACE) */
    struct credentials *user = credentials_clone(NULL, false, &error), *root = credentials_clone(NULL, false, &error);
    if (!user || !root) panic("ptrace credential fixture allocation");
    for (int i = 0; i < 4; i++) root->uid[i] = root->gid[i] = 0;
    root->permitted = root->effective = ALL_CAPS;
    credentials_exec_setid(true, 0, false, 0);
    struct credentials *plain = credentials_exec_traced(user, NULL, NULL), *watched = credentials_exec_traced(user, user, NULL);
    struct credentials* privileged = credentials_exec_traced(user, root, NULL);
    credentials_exec_setid(false, 0, false, 0);
    user->uid[1] = user->uid[2] = 1001;
    struct credentials* fallback = credentials_exec_traced(user, user, NULL);
    if (!plain || !watched || !privileged || !fallback) panic("ptrace exec fixture allocation");
    if (plain->uid[1] || plain->uid[2] || plain->effective != ALL_CAPS || watched->uid[1] != 1000 || watched->uid[2] != 1000 ||
        watched->permitted || watched->effective || privileged->uid[1] || privileged->permitted != ALL_CAPS ||
        fallback->uid[1] != 1000 || fallback->uid[2] != 1000 || fallback->permitted)
        panic("set-ID exec under ptrace");
    if (credentials_traceme_allowed(user, root) || !credentials_traceme_allowed(root, user) || !credentials_traceme_allowed(user, user))
        panic("PTRACE_TRACEME capability subset");
    credentials_destroy(plain); credentials_destroy(watched); credentials_destroy(privileged); credentials_destroy(fallback);
    credentials_destroy(user); credentials_destroy(root);
    if (memory_used_pages() != before) panic("credential/namespace ownership leak");
    vga_print("[64] PASS credential namespaces: ID maps, capability isolation, irreversible drops and ownership\n");
}
