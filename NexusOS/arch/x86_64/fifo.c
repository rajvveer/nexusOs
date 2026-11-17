/* Named pipes (mkfifo, systemd ListenFIFO=): open(2) of a FIFO inode of the ext4 root or of a mount (tmpfs) gives a
 * description of the one pipe of that inode (Linux fifo_open):
 *  - O_RDONLY waits for a writer and O_WRONLY for a reader, unless O_NONBLOCK: then O_RDONLY returns at once and
 *    O_WRONLY without a reader fails with ENXIO;
 *  - O_RDWR never waits and is a reader and a writer (reads never see end-of-file through it);
 *  - the pipe and its data live while any description is open (pipe.c frees it when both ends are closed): the inode
 *    only names it, nothing is stored on disk.
 * A reader sees POLLHUP only after a writer that came after its open went away (Linux pipe->w_counter). */
#include "native.h"
#include "../../kernel/string.h"

struct fifo_node { uint64_t identity; struct pipe_end* any; unsigned opens; struct fifo_node* next; };
struct fifo_file { struct fifo_node* node; struct pipe_end *reader, *writer; unsigned writers_at_open; char path[1024]; };
_Static_assert(sizeof(struct fifo_file) <= 4096, "fifo description record");
static struct fifo_node* nodes;
static const struct object_operations fifo_operations;

static void node_put(struct fifo_node* node) {
    if (--node->opens) return;
    for (struct fifo_node** p = &nodes; *p; p = &(*p)->next) if (*p == node) { *p = node->next; break; }
    page_free((uint64_t)node);   /* its pipe was freed by the last pipe_close */
}
static void file_release(void* object) {
    struct fifo_file* f = object;
    if (f->reader) pipe_close(f->reader);
    if (f->writer) pipe_close(f->writer);
    node_put(f->node); page_free((uint64_t)f);
}
static int64_t file_io(void* object, const struct iovec64* vectors, unsigned count, uint64_t total, bool write, uint64_t flags) {
    struct fifo_file* f = object; struct pipe_end* end = write ? f->writer : f->reader;
    if (!end) return -9;
    return pipe_io(end, vectors, count, total, write, (flags & 0x800) != 0);
}
static int file_ioctl(void* object, uint64_t command, uint64_t argument, uint64_t flags) {
    (void)flags; struct fifo_file* f = object;
    if (command == 0x541B) {   /* FIONREAD */
        if (!accessible(argument, 4, true)) return -14;
        return uput32(argument, (uint32_t)(int)pipe_available(f->reader ? f->reader : f->writer));   /* SMAP: fixup-safe store */
    }
    return -25;
}
static uint32_t file_ready(void* object) {
    struct fifo_file* f = object; uint32_t ready = 0;
    if (f->reader) {
        ready |= pipe_ready(f->reader);
        if (f->writer || pipe_writers_seen(f->reader) == f->writers_at_open) ready &= ~16U;   /* no POLLHUP without a writer that left */
    }
    if (f->writer) ready |= pipe_ready(f->writer) & (f->reader ? ~8U : ~0U);   /* O_RDWR: never POLLERR (it is its own reader) */
    return ready;
}
static void* file_source(void* object) { struct fifo_file* f = object; return f->reader ? (void*)f->reader : (void*)f->writer; }
static void file_stat(void* object, struct linux_stat* stat) {
    struct fifo_file* f = object;
    if (disk_path_stat(f->path, stat)) { memset(stat, 0, sizeof(*stat)); stat->mode = 0010600; stat->ino = f->node->identity; stat->nlink = 0; }
    stat->size = (int64_t)pipe_available(f->reader ? f->reader : f->writer); stat->block_size = 4096;
}
static const struct object_operations fifo_operations = {file_io, file_ioctl, file_ready, file_source, file_release, file_stat};

bool fifo_description(struct open_file* description) { return fd_object(description, &fifo_operations) != NULL; }

int fifo_open(uint64_t identity, uint64_t flags, const char* path) {
    unsigned access = flags & 3; bool nonblock = (flags & 0x800) != 0;
    if (access == 3) return -22;
    struct fifo_node* node = nodes;
    while (node && node->identity != identity) node = node->next;
    if (!node && access == 1 && nonblock) return -6;   /* O_WRONLY|O_NONBLOCK and nobody reads: ENXIO */
    if (node && access == 1 && nonblock && !pipe_end_open(node->any, false)) return -6;
    struct fifo_file* f = (void*)page_alloc(); if (!f) return -12;
    if (!node) {
        node = (void*)page_alloc(); if (!node) { page_free((uint64_t)f); return -12; }
        node->any = pipe_create_named(); if (!node->any) { page_free((uint64_t)node); page_free((uint64_t)f); return -12; }
        node->identity = identity; node->opens = 0; node->next = nodes; nodes = node;
    }
    node->opens++; f->node = node; f->reader = f->writer = NULL;
    size_t length = strlen(path); if (length >= sizeof(f->path)) length = sizeof(f->path) - 1;
    memcpy(f->path, path, length); f->path[length] = 0;
    f->writers_at_open = pipe_writers_seen(node->any);
    if (access == 0 || access == 2) f->reader = pipe_reopen(node->any, false);
    if (access == 1 || access == 2) f->writer = pipe_reopen(node->any, true);
    if ((access != 1 && !f->reader) || (access != 0 && !f->writer)) { file_release(f); return -23; }
    if (access != 2 && !nonblock) {   /* wait for the other side */
        while (!pipe_end_open(node->any, access == 0) && (access == 1 || pipe_writers_seen(node->any) == f->writers_at_open)) {   /* a reader also returns when a writer came and went */
            int error = pipe_wait(node->any);
            if (error) { file_release(f); return error; }
        }
    }
    int fd = fd_create_object(&fifo_operations, f, flags & ~0x200ULL, path);   /* O_TRUNC means nothing on a FIFO */
    if (fd < 0) file_release(f);
    return fd;
}
