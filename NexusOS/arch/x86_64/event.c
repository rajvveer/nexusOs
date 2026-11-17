/* Readiness is driven by resource changes; watched descriptors are weak references. */
#include "native.h"
#include "../../kernel/string.h"

#define EDGE 0x80000000U
#define ONESHOT 0x40000000U
#define READY_BITS 0x23DFU
struct event { uint32_t events; uint64_t data; } __attribute__((packed));
struct event_poll;
struct watch {
    struct watch* next; struct open_file* description; int fd;
    struct event interest; uint32_t last, pending; bool armed;
    void* source; struct event_poll* owner; struct watch *s_next, *s_prev; unsigned s_bucket;
};
struct event_poll { struct event_poll* next; struct watch *watches, *cursor; unsigned count; };
struct event_counter { uint64_t value; bool semaphore; int id; };   /* id: fdinfo eventfd-id */
struct pollfd64 { int32_t fd; uint16_t events, returned; };
/* poll()/ppoll() callers hook each polled source so that only they are woken. */
struct poll_hook { struct poll_hook *s_next, *s_prev; void* source; void* channel; unsigned s_bucket; bool linked; };
struct poll_item { struct pollfd64 request; struct open_file* description; struct poll_hook hook; };
_Static_assert(sizeof(struct event) == 12 && sizeof(struct pollfd64) == 8, "event ABI layout");
static struct event_poll* polls;
#define SOURCE_BUCKETS 512
static struct watch* watch_buckets[SOURCE_BUCKETS];
static struct poll_hook* hook_buckets[SOURCE_BUCKETS];
static unsigned source_hash(const void* source) {
    uint64_t v = (uint64_t)(uintptr_t)source; v ^= v >> 21; v *= 0x9E3779B97F4A7C15ULL;
    return (unsigned)(v >> 55) & (SOURCE_BUCKETS - 1);
}
static void watch_index(struct watch* w) {
    w->s_bucket = source_hash(w->source); w->s_prev = NULL; w->s_next = watch_buckets[w->s_bucket];
    if (w->s_next) w->s_next->s_prev = w;
    watch_buckets[w->s_bucket] = w;
}
static void watch_unindex(struct watch* w) {
    if (w->s_prev) w->s_prev->s_next = w->s_next; else watch_buckets[w->s_bucket] = w->s_next;
    if (w->s_next) w->s_next->s_prev = w->s_prev;
    w->s_next = w->s_prev = NULL;
}
static void hook_index(struct poll_hook* h) {
    h->s_bucket = source_hash(h->source); h->s_prev = NULL; h->s_next = hook_buckets[h->s_bucket];
    if (h->s_next) h->s_next->s_prev = h;
    hook_buckets[h->s_bucket] = h; h->linked = true;
}
static void hook_unindex(struct poll_hook* h) {
    if (!h->linked) return;
    if (h->s_prev) h->s_prev->s_next = h->s_next; else hook_buckets[h->s_bucket] = h->s_next;
    if (h->s_next) h->s_next->s_prev = h->s_prev;
    h->linked = false;
}
static void wake_hooks(void* source) {
    for (struct poll_hook* h = hook_buckets[source_hash(source)]; h; h = h->s_next) if (h->source == source) task_wake_channel(h->channel);
}
/* Something about an epoll set changed: its epoll_wait callers, pollers of its descriptor and epoll sets
 * that watch it (nested epoll, bounded by the depth check in control()) re-check. */
static void wake_poll(struct event_poll* poll) { task_wake_channel(poll); wake_hooks(poll); event_source_changed(poll, 1); }
void counter_destroy(struct event_counter* counter) { page_free((uint64_t)counter); }
uint32_t counter_ready(struct event_counter* counter) { return (counter->value ? 1U : 0) | (counter->value < ~0ULL - 1 ? 4U : 0); }
static int create_counter(uint32_t value, uint64_t flags) {
    if (flags & ~(0x80000ULL | 0x800ULL | 1ULL)) return -22;
    struct event_counter* counter = (void*)page_alloc(); if (!counter) return -12;
    static int next_counter_id; counter->id = next_counter_id++ & 0x7FFFFFFF;
    counter->value = value; counter->semaphore = (flags & 1) != 0;
    int fd = fd_create_counter(counter, flags); if (fd < 0) page_free((uint64_t)counter);
    return fd;
}
int64_t counter_io(struct event_counter* counter, const struct iovec64* vectors, unsigned count, uint64_t total, bool write, bool nonblock) {
    if (total < 8) return -22;
    uint64_t value = 0;
    if (write) {
        if (!iov_access(vectors, count, 0, 8, false)) return -14;
        iov_copy(vectors, count, 0, (void*)&value, 8, true);
        if (value == ~0ULL) return -22;
    }
    while (write ? value > ~0ULL - 1 - counter->value : !counter->value) {
        if (nonblock) return -11;
        int error = task_wait_channel(counter); if (error) return error;
    }
    if (write) counter->value += value;
    else {
        if (!iov_access(vectors, count, 0, 8, true)) return -14;
        value = counter->semaphore ? 1 : counter->value;
        iov_copy(vectors, count, 0, (void*)&value, 8, false); counter->value -= value;
    }
    task_wake_channel(counter); event_source_changed(counter, write ? 1 : 4); return 8;
}
static uint32_t refresh(struct watch* watch, uint32_t triggers) {
    uint32_t ready = fd_ready(watch->description) & ((watch->interest.events & READY_BITS) | 24);
    if (triggers & 1) triggers |= 0x40;
    if (triggers & 4) triggers |= 0x100;
    if (watch->armed) watch->pending |= (ready & ~watch->last) | (ready & triggers);
    watch->last = ready; watch->pending &= ready;
    return !watch->armed ? 0 : watch->interest.events & EDGE ? watch->pending : ready;
}
uint32_t event_ready(struct event_poll* poll) {
    for (struct watch* w = poll->watches; w; w = w->next) if (refresh(w, 0)) return 1;
    return 0;
}
void event_source_changed(void* source, uint32_t events) {
    for (struct watch *w = watch_buckets[source_hash(source)], *next; w; w = next) {
        next = w->s_next;
        if (w->source == source && refresh(w, events)) wake_poll(w->owner);
    }
    wake_hooks(source);
}
static void remove_watch(struct event_poll* poll, struct watch** link) {
    struct watch* w = *link; *link = w->next;
    watch_unindex(w);
    if (poll->cursor == w) poll->cursor = w->next ? w->next : poll->watches;
    poll->count--; page_free((uint64_t)w);
}
void event_forget(struct open_file* description) {
    /* Watches of a description are indexed under its event source. */
    void* source = fd_event_source(description);
    for (struct watch *w = watch_buckets[source_hash(source)], *next; w; w = next) {
        next = w->s_next;
        if (w->description != description) continue;
        struct event_poll* owner = w->owner; struct watch** link = &owner->watches;
        while (*link && *link != w) link = &(*link)->next;
        if (!*link) panic("epoll watch index out of sync");
        remove_watch(owner, link); wake_poll(owner);
    }
}
void event_destroy(struct event_poll* poll) {
    struct event_poll** link = &polls; while (*link && *link != poll) link = &(*link)->next;
    if (!*link) panic("unowned epoll instance");
    *link = poll->next;
    while (poll->watches) remove_watch(poll, &poll->watches);
    wake_poll(poll); page_free((uint64_t)poll);
}
static int create(uint64_t flags) {
    if (flags & ~0x80000ULL) return -22;
    struct event_poll* poll = (void*)page_alloc(); if (!poll) return -12;
    int fd = fd_create_epoll(poll, flags != 0);
    if (fd < 0) { page_free((uint64_t)poll); return fd; }
    poll->next = polls; polls = poll; return fd;
}
/* Nesting: the longest chain of epoll sets below `poll` (0: it watches no epoll set). */
static unsigned depth_below(struct event_poll* poll, unsigned limit) {
    unsigned deepest = 0;
    if (!limit) return 6;
    for (struct watch* w = poll->watches; w; w = w->next) {
        struct event_poll* inner = fd_epoll(w->description);
        if (inner) { unsigned d = 1 + depth_below(inner, limit - 1); if (d > deepest) deepest = d; }
    }
    return deepest;
}
static bool reaches(struct event_poll* from, struct event_poll* to, unsigned limit) {
    if (from == to) return true;
    if (!limit) return true;
    for (struct watch* w = from->watches; w; w = w->next) {
        struct event_poll* inner = fd_epoll(w->description);
        if (inner && reaches(inner, to, limit - 1)) return true;
    }
    return false;
}
static unsigned depth_above(struct event_poll* poll, unsigned limit) {
    unsigned deepest = 0;
    if (!limit) return 6;
    for (struct event_poll* outer = polls; outer; outer = outer->next)
        for (struct watch* w = outer->watches; w; w = w->next)
            if (fd_epoll(w->description) == poll) { unsigned d = 1 + depth_above(outer, limit - 1); if (d > deepest) deepest = d; }
    return deepest;
}
static int control(struct event_poll* poll, int operation, int fd, uint64_t address) {
    struct open_file* target = fd_lookup((uint64_t)(int64_t)fd); if (!target) return -9;
    if (fd_epoll(target) == poll) return -22;
    /* Linux EPOLL_MAX_NESTS: at most 4 epoll sets below the outermost one, and no cycles (ELOOP). */
    if (operation == 1 && fd_epoll(target) &&
        (reaches(fd_epoll(target), poll, 8) || depth_above(poll, 6) + 1 + depth_below(fd_epoll(target), 6) > 4)) return -40;
    if (!fd_pollable(target)) return -1;
    if (operation < 1 || operation > 3) return -22;
    struct event event = {0};
    if (operation != 2) {
        if (!accessible(address, sizeof(event), false)) return -14;
        copy_from_user(&event, (void*)address, sizeof(event));
        if (event.events & ~(READY_BITS | EDGE | ONESHOT | 0x20000000U)) return -22;
        event.events &= ~0x20000000U; /* EPOLLWAKEUP has no effect without suspend-block privilege. */
    }
    struct watch** link = &poll->watches;
    while (*link && ((*link)->fd != fd || (*link)->description != target)) link = &(*link)->next;
    if (operation == 1 && *link) return -17;
    if (operation != 1 && !*link) return -2;
    if (operation == 2) remove_watch(poll, link);
    else {
        if (!*link) {
            *link = (void*)page_alloc(); if (!*link) return -12;
            (*link)->fd = fd; (*link)->description = target; poll->count++;
            (*link)->source = fd_event_source(target); (*link)->owner = poll; watch_index(*link);
        }
        struct watch* w = *link; w->interest = event; w->armed = true; w->last = w->pending = 0; refresh(w, 0);
    }
    wake_poll(poll); return 0;
}
static uint64_t deadline_ms(int32_t timeout) { return timeout > 0 ? native_clock_ns() + (uint64_t)timeout * 1000000 : 0; }
static int wait_events(struct event_poll* poll, uint64_t address, int maximum, int32_t timeout) {
    if (maximum <= 0 || maximum > 0x7FFFFFFF / (int)sizeof(struct event)) return -22;
    if (!user_range(address, (uint64_t)maximum * sizeof(struct event))) return -14;
    uint64_t deadline = deadline_ms(timeout);
    for (;;) {
        int count = 0; unsigned visits = poll->count;
        struct watch* w = poll->cursor ? poll->cursor : poll->watches;
        while (visits-- && w && count < maximum) {
            struct watch* next = w->next ? w->next : poll->watches;
            uint32_t ready = refresh(w, 0);
            if (ready) {
                uint64_t output = address + (uint64_t)count * sizeof(struct event);
                if (!accessible(output, sizeof(struct event), true)) return count ? count : -14;
                struct event event = {ready, w->interest.data}; if (copy_to_user((void*)output, &event, sizeof(event))) return count ? count : -14; count++;
                w->pending = 0; if (w->interest.events & ONESHOT) w->armed = false;
                poll->cursor = next;
            }
            w = next;
        }
        if (count || !timeout) return count;
        int error = task_wait_until(poll, deadline);
        if (error) return error == -110 ? 0 : error;
    }
}
/* Wait on `count` requests (user array at `address`, or a kernel array `requests` for select). */
static int poll_requests(uint64_t address, struct pollfd64* requests, uint64_t count, uint64_t deadline, bool immediate) {
    if (count > 0x7FFFFFFF / sizeof(struct poll_item)) return -22;
    if (count && !requests && !accessible(address, count * sizeof(struct pollfd64), true)) return -14;
    struct poll_item* items = count ? (void*)arena_alloc(count * sizeof(*items)) : NULL;
    if (count && !items) return -12;
    for (uint64_t i = 0; i < count; i++) {
        if (requests) memcpy(&items[i].request, &requests[i], sizeof(struct pollfd64));
        else copy_from_user(&items[i].request, (void*)(address + i * sizeof(struct pollfd64)), sizeof(struct pollfd64));
        if (items[i].request.fd >= 0) items[i].description = fd_lookup((uint64_t)items[i].request.fd);
        if (items[i].description) {
            fd_pin(items[i].description);
            items[i].hook.source = fd_event_source(items[i].description); items[i].hook.channel = items; hook_index(&items[i].hook);
        }
    }
    int result;
    for (;;) {
        result = 0;
        for (uint64_t i = 0; i < count; i++) {
            struct poll_item* item = &items[i];
            item->request.returned = item->request.fd < 0 ? 0 : !item->description ? 32 :
                fd_ready(item->description) & (item->request.events | 24);
            if (item->request.returned) result++;
        }
        if (result || immediate) break;
        int error = task_wait_until(items ? (void*)items : (void*)&items, deadline);
        if (error) { result = error == -110 ? 0 : error; break; }
    }
    if (result >= 0 && count && requests) for (uint64_t i = 0; i < count; i++) requests[i].returned = items[i].request.returned;
    else if (result >= 0 && count) {
        if (!accessible(address, count * sizeof(struct pollfd64), true)) result = -14;
        else for (uint64_t i = 0; i < count; i++)
            copy_to_user((void*)(address + i * sizeof(struct pollfd64) + 6), &items[i].request.returned, 2);
    }
    for (uint64_t i = 0; i < count; i++) if (items[i].description) { hook_unindex(&items[i].hook); fd_unpin(items[i].description); }
    arena_free((uint64_t)items, count * sizeof(*items)); return result;
}
static int poll_files(uint64_t address, uint64_t count, uint64_t deadline, bool immediate) { return poll_requests(address, NULL, count, deadline, immediate); }
/* select(2)/pselect6(2): fd_set bitmaps mapped onto poll requests (Linux POLLIN_SET/POLLOUT_SET/POLLEX_SET). */
static bool fdset_test(uint64_t set, int fd) { return uget64(set + (uint64_t)(fd / 64) * 8) >> (fd % 64) & 1; }   /* validated user fd_set */
static void fdset_set(uint64_t set, int fd) { uint64_t at = set + (uint64_t)(fd / 64) * 8; uput64(at, uget64(at) | 1ULL << (fd % 64)); }
static int64_t select_call(struct registers* r) {
    int nfds = (int)r->rdi; uint64_t sets[3] = {r->rsi, r->rdx, r->r10}, timeout = r->r8;
    if (nfds < 0 || (uint64_t)nfds > task_resource_soft(7) || nfds > 0x100000) return -22;
    unsigned bytes = ((unsigned)nfds + 63) / 64 * 8;
    for (unsigned k = 0; k < 3; k++) if (sets[k] && bytes && !accessible(sets[k], bytes, true)) return -14;
    uint64_t deadline = 0, now = native_clock_ns(); bool immediate = false;
    if (timeout) {
        if (!accessible(timeout, 16, true)) return -14;
        int64_t value[2]; copy_from_user(value, (void*)timeout, 16);
        uint64_t unit = r->rax == 23 ? 1000 : 1;
        if (value[0] < 0 || value[1] < 0 || (uint64_t)value[1] >= 1000000000ULL / unit) return -22;
        uint64_t seconds = (uint64_t)value[0], ns = (uint64_t)value[1] * unit;
        uint64_t duration = seconds > (~0ULL - ns) / 1000000000 ? ~0ULL : seconds * 1000000000 + ns;
        immediate = !duration; deadline = duration > ~0ULL - now ? ~0ULL : now + duration;
    }
    if (r->rax == 270 && r->r9) {
        if (!accessible(r->r9, 16, false)) return -14;
        uint64_t mask[2]; copy_from_user(mask, (void*)r->r9, 16);
        if (mask[0]) { int error = signals_wait_mask(mask[0], mask[1]); if (error) return error; }
    }
    unsigned count = 0;
    for (int fd = 0; fd < nfds; fd++) for (unsigned k = 0; k < 3; k++)
        if (sets[k] && fdset_test(sets[k], fd)) { count++; break; }
    struct pollfd64* requests = count ? (void*)arena_alloc(count * sizeof(*requests)) : NULL;
    if (count && !requests) return -12;
    unsigned used = 0;
    for (int fd = 0; fd < nfds; fd++) {
        uint16_t events = 0;
        if (sets[0] && fdset_test(sets[0], fd)) events |= 0x41;   /* POLLIN | POLLRDNORM */
        if (sets[1] && fdset_test(sets[1], fd)) events |= 0x104;  /* POLLOUT | POLLWRNORM */
        if (sets[2] && fdset_test(sets[2], fd)) events |= 0x2;    /* POLLPRI */
        if (!events) continue;
        if (!fd_lookup((uint64_t)fd)) { arena_free((uint64_t)requests, count * sizeof(*requests)); return -9; }
        requests[used++] = (struct pollfd64){fd, events, 0};
    }
    int result = poll_requests(0, requests, count, deadline, immediate);
    if (result >= 0) {
        for (unsigned k = 0; k < 3; k++) if (sets[k] && bytes) clear_user(sets[k], bytes);
        result = 0;
        for (unsigned i = 0; i < count; i++) {
            int fd = requests[i].fd; uint16_t ready = requests[i].returned;
            bool in = (requests[i].events & 1) && (ready & (0x1 | 0x40 | 0x80 | 0x10 | 0x8));
            bool out = (requests[i].events & 4) && (ready & (0x4 | 0x100 | 0x200 | 0x8));
            bool ex = (requests[i].events & 2) && (ready & 0x2);
            if (in) { fdset_set(sets[0], fd); result++; }
            if (out) { fdset_set(sets[1], fd); result++; }
            if (ex) { fdset_set(sets[2], fd); result++; }
        }
    }
    if (count) arena_free((uint64_t)requests, count * sizeof(*requests));
    if (timeout && result != -14 && accessible(timeout, 16, true)) {
        uint64_t later = native_clock_ns(), remaining = deadline > later ? deadline - later : 0;
        int64_t value[2] = {(int64_t)(remaining / 1000000000), (int64_t)(remaining % 1000000000 / (r->rax == 23 ? 1000 : 1))};
        copy_to_user((void*)timeout, value, 16);
    }
    return result;
}
int64_t event_call(struct registers* r) {
    if (r->rax == 23 || r->rax == 270) return select_call(r);
    if (r->rax == 7) return poll_files(r->rdi, r->rsi, deadline_ms((int32_t)r->rdx), (int32_t)r->rdx == 0);
    if (r->rax == 271) {
        uint64_t deadline = 0; bool immediate = false;
        if (r->rdx) {
            if (!accessible(r->rdx, 16, true)) return -14;
            int64_t timeout[2]; copy_from_user(timeout, (void*)r->rdx, sizeof(timeout));
            if (timeout[0] < 0 || timeout[1] < 0 || timeout[1] >= 1000000000) return -22;
            uint64_t seconds = timeout[0], ns = timeout[1], now = native_clock_ns();
            uint64_t duration = seconds > (~0ULL - ns) / 1000000000 ? ~0ULL : seconds * 1000000000 + ns;
            immediate = !duration; deadline = duration > ~0ULL - now ? ~0ULL : now + duration;
        }
        int result = signals_wait_mask(r->r10, r->r8); if (result) return result;
        result = poll_files(r->rdi, r->rsi, deadline, immediate);
        if (r->rdx) {
            if (!accessible(r->rdx, 16, true)) return -14;
            uint64_t now = native_clock_ns(), remaining = deadline > now ? deadline - now : 0;
            uint64_t timeout[2] = {remaining / 1000000000, remaining % 1000000000}; copy_to_user((void*)r->rdx, timeout, sizeof(timeout));
        }
        return result;
    }
    if (r->rax == 291) return create(r->rdi);
    if (r->rax == 284 || r->rax == 290) return create_counter((uint32_t)r->rdi, r->rax == 290 ? r->rsi : 0);
    if (r->rax == 213) return (int32_t)r->rdi <= 0 ? -22 : create(0);
    struct open_file* description = fd_lookup(r->rdi); if (!description) return -9;
    struct event_poll* poll = fd_epoll(description); if (!poll) return -22;
    fd_pin(description); int result;
    if (r->rax == 233) result = control(poll, (int32_t)r->rsi, (int32_t)r->rdx, r->r10);
    else {
        result = r->rax == 281 ? signals_wait_mask(r->r8, r->r9) : 0;
        if (!result) result = wait_events(poll, r->rsi, (int32_t)r->rdx, (int32_t)r->r10);
    }
    fd_unpin(description); return result;
}
/* fdinfo.c (/proc/PID/fdinfo of eventfd and epoll descriptions) */
uint64_t counter_fdinfo(struct event_counter* counter, bool* semaphore, int* id) { *semaphore = counter->semaphore; *id = counter->id; return counter->value; }
bool event_poll_watch(struct event_poll* poll, unsigned index, int* fd, uint32_t* events, uint64_t* data, struct open_file** description) {
    struct watch* w = poll->watches;
    while (w && index--) w = w->next;
    if (!w) return false;
    *fd = w->fd; *events = w->interest.events; *data = w->interest.data; *description = w->description; return true;
}
