/* Advisory inode locks: process ranges, open-description ranges and flock. */
#include "native.h"
#include "../../kernel/string.h"

enum lock_kind { PROCESS_LOCK, DESCRIPTION_LOCK, FLOCK_LOCK };
struct held_lock {
    struct held_lock* next;
    struct open_file* description;
    uint64_t device, inode, begin, end;
    uint32_t pid;
    enum lock_kind kind;
    unsigned type;
};
struct waiting_lock { struct waiting_lock* next; struct held_lock request; bool reachable; };
struct flock64 { int16_t type, whence; uint32_t pad; int64_t start, length; int32_t pid; uint32_t pad2; };
_Static_assert(sizeof(struct flock64) == 32 && __builtin_offsetof(struct flock64, start) == 8, "fcntl lock ABI");
_Static_assert(sizeof(struct held_lock) <= 4096, "file lock allocation");
static struct held_lock* held;
static struct waiting_lock* waiting;
static unsigned char changed;
#define LOCK_END (1ULL << 63)
static bool same_inode(const struct held_lock* a, const struct held_lock* b) { return a->device == b->device && a->inode == b->inode; }
static bool same_owner(const struct held_lock* a, const struct held_lock* b) {
    return a->kind == b->kind && (a->kind == PROCESS_LOCK ? a->pid == b->pid : a->description == b->description);
}
static bool conflicts(const struct held_lock* a, const struct held_lock* b) {
    return same_inode(a, b) && ((a->kind == FLOCK_LOCK) == (b->kind == FLOCK_LOCK)) && !same_owner(a, b) &&
           a->begin < b->end && b->begin < a->end && (a->type == 1 || b->type == 1);
}
static struct held_lock* conflict(const struct held_lock* request) {
    struct held_lock* result = NULL;
    for (struct held_lock* h = held; h; h = h->next)
        if (conflicts(request, h) && (!result || h->begin < result->begin)) result = h;
    return result;
}
static void remove_lock(struct held_lock** link) {
    struct held_lock* old = *link; *link = old->next; page_free((uint64_t)old);
}
static bool deadlock(const struct held_lock* request) {
    if (request->kind != PROCESS_LOCK) return false;
    for (struct waiting_lock* w = waiting; w; w = w->next) w->reachable = false;
    for (struct held_lock* h = held; h; h = h->next) if (h->kind == PROCESS_LOCK && conflicts(request, h))
        for (struct waiting_lock* w = waiting; w; w = w->next)
            if (w->request.kind == PROCESS_LOCK && w->request.pid == h->pid) w->reachable = true;
    bool progress;
    do {
        progress = false;
        for (struct waiting_lock* w = waiting; w; w = w->next) if (w->reachable)
            for (struct held_lock* h = held; h; h = h->next) if (h->kind == PROCESS_LOCK && conflicts(&w->request, h)) {
                if (h->pid == request->pid) return true;
                for (struct waiting_lock* next = waiting; next; next = next->next)
                    if (!next->reachable && next->request.kind == PROCESS_LOCK && next->request.pid == h->pid) {
                        next->reachable = true; progress = true;
                    }
            }
    } while (progress);
    return false;
}
static int replace_range(const struct held_lock* request) {
    bool split = false;
    for (struct held_lock* h = held; h; h = h->next)
        if (same_inode(h, request) && same_owner(h, request) && h->begin < request->begin && h->end > request->end) split = true;
    struct held_lock* added = request->type == 2 ? NULL : (void*)page_alloc();
    struct held_lock* spare = split ? (void*)page_alloc() : NULL;
    if ((request->type != 2 && !added) || (split && !spare)) {
        if (added) page_free((uint64_t)added); if (spare) page_free((uint64_t)spare); return -37;
    }
    struct held_lock** link = &held;
    while (*link) {
        struct held_lock* h = *link;
        if (!same_inode(h, request) || !same_owner(h, request) || h->end <= request->begin || h->begin >= request->end) { link = &h->next; continue; }
        if (h->begin < request->begin && h->end > request->end) {
            if (!spare) panic("overlapping lock ranges for one owner");
            *spare = *h; spare->begin = request->end; h->end = request->begin;
            h->next = spare; link = &spare->next; spare = NULL;
        } else if (h->begin < request->begin) { h->end = request->begin; link = &h->next; }
        else if (h->end > request->end) { h->begin = request->end; link = &h->next; }
        else remove_lock(link);
    }
    if (spare) page_free((uint64_t)spare);
    if (added) {
        *added = *request; added->next = held; held = added;
        bool merged;
        do {
            merged = false; link = &held;
            while (*link) {
                struct held_lock* h = *link;
                if (h != added && same_inode(h, added) && same_owner(h, added) && h->type == added->type && h->end >= added->begin && h->begin <= added->end) {
                    if (h->begin < added->begin) added->begin = h->begin;
                    if (h->end > added->end) added->end = h->end;
                    remove_lock(link); merged = true;
                } else link = &h->next;
            }
        } while (merged);
    }
    task_wake_channel(&changed); return 0;
}
static int change_lock(struct held_lock* request, uint32_t fd, bool block) {
    if (request->type == 2) return replace_range(request);
    if (request->kind == FLOCK_LOCK) {
        for (struct held_lock* h = held; h; h = h->next) if (same_inode(h, request) && same_owner(h, request)) {
            if (h->type == request->type) return 0;
            struct held_lock unlock = *request; unlock.type = 2;
            int error = replace_range(&unlock); if (error) return error; break;
        }
    }
    struct waiting_lock waiter = {.request = *request, .next = waiting}; waiting = &waiter;
    int error;
    for (;;) {
        if (request->kind == PROCESS_LOCK && fd_lookup(fd) != request->description) { error = -9; break; }
        if (!conflict(request)) { error = replace_range(request); break; }
        if (!block) { error = -11; break; }
        if (deadlock(request)) { error = -35; break; }
        error = task_wait_channel(&changed); if (error) break;
    }
    struct waiting_lock** link = &waiting; while (*link && *link != &waiter) link = &(*link)->next;
    if (!*link) panic("unowned lock waiter");
    *link = waiter.next; return error;
}
int64_t file_lock_call(struct open_file* file, uint32_t fd, uint32_t command, uint64_t argument, bool whole_file) {
    struct lock_file_info info; int error = fd_lock_info(file, &info); if (error) return error;
    struct held_lock request = {.description = file, .device = info.device, .inode = info.inode, .pid = task_pid(), .end = LOCK_END};
    if (whole_file) {
        uint32_t operation = (uint32_t)argument;
        if ((operation & ~15u) || ((operation & ~4u) != 1 && (operation & ~4u) != 2 && (operation & ~4u) != 8)) return -22;
        request.kind = FLOCK_LOCK; request.type = (operation & 8) ? 2 : (operation & 2) ? 1 : 0;
        return change_lock(&request, fd, !(operation & 4));
    }
    bool description = command >= 36, query = command == 5 || command == 36;
    if (command != 5 && command != 6 && command != 7 && command != 36 && command != 37 && command != 38) return -22;
    if (!accessible(argument, sizeof(struct flock64), query)) return -14;
    struct flock64 input; copy_from_user(&input, (void*)argument, sizeof(input));
    if (input.type < 0 || input.type > 2 || input.whence < 0 || input.whence > 2 || (description && input.pid)) return -22;
    int64_t base = input.whence == 1 ? (int64_t)info.position : input.whence == 2 ? (int64_t)info.size : 0, start;
    if (__builtin_add_overflow(base, input.start, &start)) return -75;
    if (start < 0) return -22;
    request.begin = start; request.kind = description ? DESCRIPTION_LOCK : PROCESS_LOCK; request.type = input.type;
    if (input.length > 0) {
        if ((uint64_t)input.length > LOCK_END - request.begin) return -75;
        request.end = request.begin + input.length;
    } else if (input.length < 0) {
        if (__builtin_add_overflow(start, input.length, &start) || start < 0) return -22;
        request.end = request.begin; request.begin = start;
    }
    if (query) {
        if (request.type == 2) return -22;
        struct held_lock* found = conflict(&request);
        if (!found) input.type = 2;
        else {
            input.type = found->type; input.whence = 0; input.start = found->begin;
            input.length = found->end == LOCK_END ? 0 : found->end - found->begin;
            input.pid = found->kind == PROCESS_LOCK ? (int32_t)task_visible_pid(found->pid) : -1;
        }
        return copy_to_user((void*)argument, &input, sizeof(input)) ? -14 : 0;
    }
    if ((request.type == 0 && (info.flags & 3) == 1) || (request.type == 1 && !(info.flags & 3))) return -9;
    return change_lock(&request, fd, command == 7 || command == 38);
}
void file_locks_close(struct open_file* file, uint32_t pid) {
    struct lock_file_info info; if (fd_lock_info(file, &info)) return;
    struct held_lock** link = &held;
    while (*link) {
        struct held_lock* h = *link;
        if (h->kind == PROCESS_LOCK && h->pid == pid && h->device == info.device && h->inode == info.inode) remove_lock(link);
        else link = &h->next;
    }
    task_wake_channel(&changed);
}
void file_locks_release(struct open_file* file) {
    struct held_lock** link = &held;
    while (*link) {
        struct held_lock* h = *link;
        if (h->kind != PROCESS_LOCK && h->description == file) remove_lock(link);
        else link = &h->next;
    }
    task_wake_channel(&changed);
}
void file_locks_exit(uint32_t pid) {
    struct held_lock** link = &held;
    while (*link) {
        struct held_lock* h = *link;
        if (h->kind == PROCESS_LOCK && h->pid == pid) remove_lock(link);
        else link = &h->next;
    }
    task_wake_channel(&changed);
}
