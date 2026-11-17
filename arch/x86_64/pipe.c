/* Anonymous byte pipes with bounded storage and atomic PIPE_BUF writes.
 * The ring is kept in single pages allocated as data first reaches them (Linux pipe buffers): no physically contiguous
 * 64 KiB allocation, which failed under memory pressure (pipe2 ENOMEM killed Chrome's zygote), and no memory for pipes
 * that never carry much. */
#include "native.h"
#include "../../kernel/string.h"

#define PIPE_BUF 4096U
#define PIPE_PAGE 4096U
#define PIPE_MAX_PAGES 256U   /* Linux /proc/sys/fs/pipe-max-size: 1 MiB */
struct pipe;
struct pipe_end { struct pipe* pipe; bool write, closed; unsigned users; };   /* users: open file descriptions */
struct pipe {
    struct pipe_end ends[2];
    uint8_t* pages[PIPE_MAX_PAGES];   /* the ring, page by page; NULL until first written */
    unsigned capacity, head, used;
    uint64_t id;
    unsigned writers_seen;   /* named pipes (fifo.c): write ends opened so far (Linux pipe->w_counter) */
};
_Static_assert(sizeof(struct pipe) <= 4096, "pipe record size");
static uint64_t next_id;
static void pages_free(uint8_t** pages, unsigned count) {
    for (unsigned i = 0; i < count; i++) if (pages[i]) { page_free((uint64_t)pages[i]); pages[i] = NULL; }
}
/* The pages a write of `bytes` at `position` will use (false: no memory for one of them). */
static bool ring_reserve(struct pipe* pipe, unsigned position, unsigned bytes) {
    while (bytes) {
        unsigned index = position / PIPE_PAGE, part = PIPE_PAGE - position % PIPE_PAGE; if (part > bytes) part = bytes;
        if (!pipe->pages[index] && !(pipe->pages[index] = (void*)page_alloc())) return false;
        bytes -= part; position = (position + part) % pipe->capacity;
    }
    return true;
}
/* Copies `bytes` between the ring at `position` (wrapping, pages present) and the iovecs at `done`. */
static void ring_copy(struct pipe* pipe, unsigned position, const struct iovec64* vectors, unsigned count, uint64_t done,
                      unsigned bytes, bool write) {
    while (bytes) {
        unsigned index = position / PIPE_PAGE, offset = position % PIPE_PAGE, part = PIPE_PAGE - offset;
        if (part > bytes) part = bytes;
        if (!pipe->pages[index]) panic("pipe ring page missing");
        iov_copy(vectors, count, done, pipe->pages[index] + offset, part, write);
        done += part; bytes -= part; position = (position + part) % pipe->capacity;
    }
}
uint64_t pipe_identity(struct pipe_end* end) { return end->pipe->id; }
unsigned pipe_available(struct pipe_end* end) { return end->pipe->used; }
uint32_t pipe_ready(struct pipe_end* end) {
    struct pipe* pipe = end->pipe;
    return end->write ? (pipe->used < pipe->capacity ? 4U : 0) | (pipe->ends[0].closed ? 8U : 0) :
                        (pipe->used ? 1U : 0) | (pipe->ends[1].closed ? 16U : 0);
}
void pipe_close(struct pipe_end* end) {
    if (end->closed || !end->users) panic("pipe endpoint closed twice");
    if (--end->users) return;   /* another description (a reopen through /proc/PID/fd/N) still uses this end */
    struct pipe* pipe = end->pipe; end->closed = true;
    task_wake_channel(pipe);
    event_source_changed(&pipe->ends[0], 0); event_source_changed(&pipe->ends[1], 0);
    if (pipe->ends[0].closed && pipe->ends[1].closed) {
        pages_free(pipe->pages, PIPE_MAX_PAGES); page_free((uint64_t)pipe);
    }
}
/* open(2) through /proc/PID/fd/N of a pipe (Linux fifo_open on the pipe inode): the read or write end by the requested
 * access mode, whichever end N is - e.g. open("/dev/stdin") when stdin is a pipe, bash process substitution. An end
 * whose last user closed comes back (a new reader or writer of the same pipe). */
struct pipe_end* pipe_reopen(struct pipe_end* any, bool write) {
    struct pipe_end* end = &any->pipe->ends[write ? 1 : 0];
    if (write) any->pipe->writers_seen++;
    if (end->closed) { end->closed = false; end->users = 1; }
    else if (end->users == 0xFFFFFFFFU) return NULL;
    else end->users++;
    task_wake_channel(end->pipe); event_source_changed(&end->pipe->ends[0], 0); event_source_changed(&end->pipe->ends[1], 0);
    return end;
}
/* A named pipe (fifo.c): both ends closed until open(2) of the FIFO reopens one; freed when both close again. */
struct pipe_end* pipe_create_named(void) {
    struct pipe* pipe = (void*)page_alloc(); if (!pipe) return NULL;
    pipe->capacity = 65536; pipe->id = ++next_id;
    for (unsigned i = 0; i < 2; i++) pipe->ends[i] = (struct pipe_end){pipe, i != 0, true, 0};
    return &pipe->ends[0];
}
bool pipe_end_open(struct pipe_end* any, bool write) { return !any->pipe->ends[write ? 1 : 0].closed; }
unsigned pipe_writers_seen(struct pipe_end* any) { return any->pipe->writers_seen; }
int pipe_wait(struct pipe_end* any) { return task_wait_channel(any->pipe); }
int pipe_create(uint64_t address, uint64_t flags) {
    if (flags & ~(0x80000ULL | 0x800ULL)) return -22;
    if (!accessible(address, 8, true)) return -14;
    struct pipe* pipe = (void*)page_alloc(); if (!pipe) return -12;   /* zeroed: no ring pages yet */
    pipe->capacity = 65536;
    pipe->id = ++next_id;
    for (unsigned i = 0; i < 2; i++) pipe->ends[i] = (struct pipe_end){pipe, i != 0, false, 1};
    int read = fd_create_pipe(&pipe->ends[0], false, flags);
    if (read < 0) { page_free((uint64_t)pipe); return read; }
    int write = fd_create_pipe(&pipe->ends[1], true, flags);
    if (write < 0) { pipe->ends[1].closed = true; pipe->ends[1].users = 0; fd_close(read); return write; }
    int ends[2] = {read, write}; if (copy_to_user((void*)address, ends, sizeof(ends))) { fd_close(read); fd_close(write); return -14; } return 0;
}
int64_t pipe_io(struct pipe_end* end, const struct iovec64* vectors, unsigned count, uint64_t total, bool write, bool nonblock) {
    if (write != end->write) return -9;
    if (!total) return 0;
    if (total > 0x7FFFF000) total = 0x7FFFF000;
    struct pipe* pipe = end->pipe; uint64_t done = 0;
    for (;;) {
        if (write && pipe->ends[0].closed) {
            signals_enqueue(signals_current(), 13, 0, task_visible_pid(task_pid()), task_kernel_uid(), 0, false);
            return done ? (int64_t)done : -32;
        }
        unsigned room = write ? pipe->capacity - pipe->used : pipe->used;
        if (!write && !room && pipe->ends[1].closed) return 0;
        if (!room || (write && total <= PIPE_BUF && room < total)) {
            if (nonblock) return done ? (int64_t)done : -11;
            int error = task_wait_channel(pipe); if (error) return done ? (int64_t)done : error;
            continue;
        }
        unsigned bytes = total - done < room ? (unsigned)(total - done) : room;
        if (!iov_access(vectors, count, done, bytes, !write)) return done ? (int64_t)done : -14;
        unsigned position = write ? (pipe->head + pipe->used) % pipe->capacity : pipe->head;
        if (write && !ring_reserve(pipe, position, bytes)) return done ? (int64_t)done : -12;
        ring_copy(pipe, position, vectors, count, done, bytes, write);
        if (write) pipe->used += bytes;
        else { pipe->head = (pipe->head + bytes) % pipe->capacity; pipe->used -= bytes; }
        done += bytes; task_wake_channel(pipe);
        event_source_changed(&pipe->ends[0], write ? 1 : 0);
        event_source_changed(&pipe->ends[1], write ? 0 : 4);
        if (!write || done == total) return done;
    }
}
int64_t pipe_control(struct pipe_end* end, uint64_t command, uint64_t argument) {
    struct pipe* pipe = end->pipe;
    if (command == 1032) return pipe->capacity;
    if (command != 1031 || argument > 0x40000000) return -22;
    unsigned capacity = PIPE_BUF;
    while (capacity < argument) capacity *= 2;
    if (capacity > PIPE_MAX_PAGES * PIPE_PAGE) return -1;   /* EPERM past pipe-max-size (an unprivileged Linux task) */
    if (capacity < pipe->used) return -16;
    if (capacity == pipe->capacity) return capacity;
    /* the bytes in order into a fresh ring of the new size, page by page */
    uint8_t* fresh[PIPE_MAX_PAGES] = {0};
    unsigned need = (pipe->used + PIPE_PAGE - 1) / PIPE_PAGE;
    for (unsigned i = 0; i < need; i++) if (!(fresh[i] = (void*)page_alloc())) { pages_free(fresh, need); return -12; }
    for (unsigned moved = 0; moved < pipe->used;) {
        unsigned from = (pipe->head + moved) % pipe->capacity;
        unsigned part = PIPE_PAGE - from % PIPE_PAGE, room = PIPE_PAGE - moved % PIPE_PAGE;
        if (part > room) part = room;
        if (part > pipe->used - moved) part = pipe->used - moved;
        memcpy(fresh[moved / PIPE_PAGE] + moved % PIPE_PAGE, pipe->pages[from / PIPE_PAGE] + from % PIPE_PAGE, part);
        moved += part;
    }
    pages_free(pipe->pages, PIPE_MAX_PAGES);
    memcpy(pipe->pages, fresh, sizeof(fresh)); pipe->capacity = capacity; pipe->head = 0;
    task_wake_channel(pipe); event_source_changed(&pipe->ends[1], 4); return capacity;
}
