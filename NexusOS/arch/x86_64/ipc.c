/* Native AF_UNIX streams/records/datagrams and descriptor/credential transfer.
 * SOCK_DGRAM (syslog's /dev/log, sd_notify, chronyc): a socket()-made datagram socket is connectionless; sendto/sendmsg
 * with an address (or the default destination of connect) queue one packet on the bound receiver (EPROTOTYPE for a
 * non-datagram receiver, ECONNREFUSED without one, at most UNIX_DGRAM_QLEN queued: EAGAIN or wait), recvfrom/recvmsg
 * return one datagram (MSG_TRUNC semantics) with the sender's address and SCM_CREDENTIALS when SO_PASSCRED is set.
 * socketpair(SOCK_DGRAM) ends are connected to each other and keep message boundaries like SOCK_SEQPACKET. */
#include "native.h"
#include "../../kernel/string.h"

#define PAGE 4096
#define MAX_IOV 1024
#define MAX_RIGHTS 253
#define DONTWAIT 0x40ULL
#define NOSIGNAL 0x4000ULL
#define PEEK 2ULL
#define TRUNC 0x20U
#define CTRUNC 8U
struct control64 { uint64_t size; int32_t level, type; };
struct chunk { struct chunk* next; uint32_t used, pad; uint8_t data[4080]; };
struct rights_owner { struct rights_owner* next; uint64_t count; uint32_t uid; };
static struct rights_owner* rights_owners;
struct packet {
    struct packet* next; struct chunk* data; uint64_t size, consumed;
    struct pid_identity* sender;
    struct rights_owner* owner;
    unsigned rights_count; struct open_file* rights[MAX_RIGHTS];
    uint32_t credentials[3];
    unsigned source_length; uint8_t source[109];   /* datagram sender's address */
};
#define UNIX_DGRAM_QLEN 512   /* Linux net.unix.max_dgram_qlen */
static int reserve_rights(unsigned count, struct rights_owner** owner) {
    if (!count) return 0;
    uint32_t uid = task_kernel_uid(); struct rights_owner* account;
    for (account = rights_owners; account && account->uid != uid; account = account->next) {}
    /* Linux checks the outstanding count before accepting another SCM batch. */
    if (account && account->count > task_resource_soft(7) &&
        !credentials_capable(credentials_current(), NULL, 24) && !credentials_capable(credentials_current(), NULL, 21)) return -109;
    if (!account) {
        account = (void*)page_alloc(); if (!account) return -12;
        account->uid = uid; account->next = rights_owners; rights_owners = account;
    }
    account->count += count; *owner = account; return 0;
}
static void release_rights(struct packet* packet) {
    for (unsigned i = 0; i < packet->rights_count; i++) fd_release(packet->rights[i]);
    if (packet->owner) {
        struct rights_owner* account = packet->owner;
        if (account->count < packet->rights_count) panic("SCM_RIGHTS accounting underflow");
        account->count -= packet->rights_count;
        if (!account->count) {
            struct rights_owner** link = &rights_owners; while (*link && *link != account) link = &(*link)->next;
            if (!*link) panic("unowned SCM_RIGHTS account");
            *link = account->next; page_free((uint64_t)account);
        }
        packet->owner = NULL;
    }
    packet->rights_count = 0;
}
struct socket_pair;
struct binding;
struct unix_address { unsigned length; uint8_t name[109]; };
enum socket_state { UNCONNECTED, LISTENING, CONNECTED };
struct unix_socket {
    struct socket_pair* pair; struct unix_socket* peer; unsigned type; enum socket_state state;
    bool closed, read_closed, write_closed, pass_credentials, have_peer_credentials;
    uint32_t peer_credentials[3], listen_credentials[3];
    struct pid_identity *peer_pid, *listen_pid;
    struct unix_address local_address, peer_address;
    struct binding* binding;
    struct unix_socket *pending_head, *pending_tail, *pending_next;
    unsigned pending_count, backlog;
    int error;
    struct packet *head, *tail;
    struct unix_socket* closing_next;
    unsigned queued; bool dgram_connected; struct unix_address dgram_peer;   /* unconnected SOCK_DGRAM */
};
struct socket_pair { struct unix_socket ends[2]; unsigned changing, closing_count; };
struct binding { struct binding* next; struct unix_socket* socket; struct unix_address address; uint64_t inode; };
static struct binding* bindings;
static uint32_t autobind_sequence;
static unsigned live_pairs;
static struct unix_socket* closing;
static bool draining;
static void binding_remove(struct binding* binding) {
    struct binding** link = &bindings; while (*link && *link != binding) link = &(*link)->next;
    if (!*link) panic("unowned Unix socket name");
    *link = binding->next;
    if (binding->socket->binding == binding) binding->socket->binding = NULL;
    page_free((uint64_t)binding);
}
void ipc_path_unlinked(uint64_t inode) {
    for (struct binding* b = bindings; b;) {
        struct binding* next = b->next;
        if (b->address.length && b->address.name[0] && b->inode == inode) binding_remove(b);
        b = next;
    }
}
_Static_assert(sizeof(struct message64) == 56 && sizeof(struct chunk) == PAGE && sizeof(struct packet) <= PAGE && sizeof(struct socket_pair) <= PAGE, "IPC ABI/allocations");
unsigned ipc_live_pairs(void) { return live_pairs; }
bool ipc_rights_inflight(void) { return rights_owners != NULL; }
uint32_t ipc_ready(struct unix_socket* socket) {
    if (socket->state == LISTENING) return socket->pending_head ? 1 : 0;
    if (socket->type == 2 && socket->state == UNCONNECTED) return 4 | (socket->head || socket->read_closed ? 1 : 0) | (socket->error ? 8 : 0);
    if (socket->state == UNCONNECTED) return 4 | 16;
    struct unix_socket* peer = socket->peer;
    uint32_t ready = 4;
    if (socket->head || socket->read_closed || !peer || peer->write_closed) ready |= 1;
    if (socket->read_closed || !peer || peer->write_closed) ready |= 0x2000;
    if (!peer || (socket->read_closed && socket->write_closed)) ready |= 16;
    if (socket->error) ready |= 8;
    return ready;
}
/* FIONREAD/SIOCINQ: queued bytes (stream/seqpacket) or the next datagram; Chrome's SyncSocket::Peek relies on it. */
int64_t ipc_available(struct unix_socket* socket) {
    if (socket->state == LISTENING) return -22;
    uint64_t total = 0;
    for (struct packet* p = socket->head; p; p = p->next) { total += p->size - p->consumed; if (socket->type == 2) break; }
    return total > 0x7FFFFFFF ? 0x7FFFFFFF : (int64_t)total;
}
static void packet_free(struct packet* packet) {
    while (packet->data) { struct chunk* next = packet->data->next; page_free((uint64_t)packet->data); packet->data = next; }
    release_rights(packet);
    pid_release(packet->sender);
    page_free((uint64_t)packet);
}
static void reap(struct socket_pair* pair) {
    if (!pair->changing && !pair->closing_count && pair->ends[0].closed && pair->ends[1].closed) {
        for (unsigned i = 0; i < 2; i++) { pid_release(pair->ends[i].peer_pid); pid_release(pair->ends[i].listen_pid); }
        live_pairs--; page_free((uint64_t)pair);
    }
}
void ipc_discard(struct unix_socket* socket) {
    struct socket_pair* pair = socket->pair; pair->changing++;
    struct unix_socket* pending = socket->pending_head;
    socket->pending_head = socket->pending_tail = NULL; socket->pending_count = 0;
    while (pending) { struct unix_socket* next = pending->pending_next; ipc_close(pending); pending = next; }
    struct packet* p = socket->head; socket->head = socket->tail = NULL; socket->queued = 0;
    while (p) { struct packet* next = p->next; packet_free(p); p = next; }
    pair->changing--; reap(pair);
}
void ipc_close(struct unix_socket* socket) {
    if (socket->closed) return;
    if (socket->binding) binding_remove(socket->binding);
    struct unix_socket* peer = socket->peer;
    if (peer) {
        if (socket->head) peer->error = 104;
        peer->peer = NULL; socket->peer = NULL;
        task_wake_channel(peer); event_source_changed(peer, 0);
    }
    socket->closed = socket->read_closed = socket->write_closed = true;
    task_wake_channel(socket); event_source_changed(socket, 0);
    socket->pair->closing_count++; socket->closing_next = closing; closing = socket;
    if (draining) return;
    draining = true;
    while (closing) {
        struct unix_socket* next = closing; closing = next->closing_next;
        struct socket_pair* pair = next->pair;
        ipc_discard(next); pair->closing_count--; reap(pair);
    }
    draining = false;
}
void ipc_mark(struct unix_socket* socket) {
    for (struct packet* p = socket->head; p; p = p->next)
        for (unsigned i = 0; i < p->rights_count; i++) fd_mark(p->rights[i]);
    for (struct unix_socket* child = socket->pending_head; child; child = child->pending_next)
        for (struct packet* p = child->head; p; p = p->next)
            for (unsigned i = 0; i < p->rights_count; i++) fd_mark(p->rights[i]);
}
static int vectors_load(const struct message64* message, struct iovec64* vectors, uint64_t* total) {
    if (message->count > MAX_IOV) return -22;
    if (message->count && !accessible(message->vectors, message->count * sizeof(*vectors), false)) return -14;
    if (message->count && copy_from_user(vectors, (void*)message->vectors, message->count * sizeof(*vectors))) return -14;
    *total = 0;
    for (uint64_t i = 0; i < message->count; i++) {
        if (vectors[i].size > 0x7FFFFFFFFFFFFFFFULL - *total) return -22;
        *total += vectors[i].size;
    }
    return 0;
}
static int controls_load(const struct message64* message, struct open_file** rights, unsigned* count) {
    *count = 0;
    if (message->control_size > 65536) return -105;
    if (message->control_size && !accessible(message->control, message->control_size, false)) return -14;
    uint64_t offset = 0;
    while (offset + sizeof(struct control64) <= message->control_size) {
        struct control64 control; copy_from_user(&control, (void*)(message->control + offset), sizeof(control));
        if (control.size < sizeof(control) || control.size > message->control_size - offset || control.level != 1) return -22;
        uint64_t bytes = control.size - sizeof(control);
        const void* data = (void*)(message->control + offset + sizeof(control));
        if (control.type == 1) {
            if ((bytes & 3) || bytes / 4 > MAX_RIGHTS - *count) return -22;
            for (uint64_t i = 0; i < bytes / 4; i++) {
                int fd; copy_from_user(&fd, (const uint8_t*)data + i * 4, 4);
                struct open_file* description = fd_lookup((uint64_t)(int64_t)fd); if (!description) return -9;
                rights[(*count)++] = description;
            }
        } else if (control.type == 2) {
            if (bytes != 12) return -22;
            uint32_t credentials[3]; copy_from_user(credentials, data, sizeof(credentials));
            if (credentials[0] != task_visible_pid(task_pid()) || credentials[1] != task_uid() || credentials[2] != task_gid()) return -1;
        } else return -22;
        offset += (control.size + 7) & ~7ULL;
    }
    return 0;
}
static int64_t send_vectors(struct unix_socket* socket, const struct iovec64* vectors, uint64_t count,
                            uint64_t total, struct open_file** rights, unsigned rights_count, uint64_t flags) {
    if (flags & 0x1ULL) return -95; /* Linux unix sockets reject only MSG_OOB; other send flags are ignored */
    if (socket->state != CONNECTED) return -107;
    struct unix_socket* peer = socket->peer;
    if (socket->write_closed || !peer || peer->read_closed) {
        if (!(flags & NOSIGNAL)) signals_enqueue(signals_current(), 13, 0, task_visible_pid(task_pid()), task_kernel_uid(), 0, false);
        return -32;
    }
    for (uint64_t i = 0; i < count; i++) if (vectors[i].size && !accessible(vectors[i].address, vectors[i].size, false)) return -14;
    if (!total && socket->type == 1) return 0;
    struct packet* packet = (void*)page_alloc(); if (!packet) return -12;
    int accounting = reserve_rights(rights_count, &packet->owner);
    if (accounting) { packet_free(packet); return accounting; }
    packet->size = total; packet->rights_count = rights_count;
    packet->credentials[0] = task_pid(); packet->credentials[1] = task_kernel_uid();
    packet->sender = pid_lookup(task_pid(), pid_namespace_root()); pid_retain(packet->sender);
    packet->credentials[2] = credentials_kernel_id(credentials_current(), true, 0);
    for (unsigned i = 0; i < rights_count; i++) { packet->rights[i] = rights[i]; fd_reference(rights[i]); }
    struct chunk* tail = NULL;
    for (uint64_t i = 0; i < count; i++) {
        uint64_t done = 0;
        while (done < vectors[i].size) {
            if (!tail || tail->used == sizeof(tail->data)) {
                struct chunk* next = (void*)page_alloc();
                if (!next) { packet_free(packet); return -12; }
                if (tail) tail->next = next; else packet->data = next;
                tail = next;
            }
            uint64_t bytes = sizeof(tail->data) - tail->used;
            if (bytes > vectors[i].size - done) bytes = vectors[i].size - done;
            copy_from_user(tail->data + tail->used, (void*)(vectors[i].address + done), bytes);
            tail->used += bytes; done += bytes;
        }
    }
    if (peer->tail) peer->tail->next = packet; else peer->head = packet;
    peer->tail = packet; task_wake_channel(peer); event_source_changed(peer, 1); return total;
}
static void packet_copy(const struct packet* p, uint64_t offset, void* output, uint64_t size) {
    struct chunk* chunk = p->data;
    while (chunk && offset >= chunk->used) { offset -= chunk->used; chunk = chunk->next; }
    uint8_t* out = output;
    while (size && chunk) {
        uint64_t part = chunk->used - offset; if (part > size) part = size;
        copy_to_user(out, chunk->data + offset, part); out += part; size -= part; offset = 0; chunk = chunk->next;
    }
}
static int controls_write(struct unix_socket* socket, const struct packet* packet, struct message64* message, uint64_t flags) {
    uint64_t capacity = message->control_size, written = 0;
    if (socket->pass_credentials) {
        if (capacity >= 32) {
            if (!accessible(message->control, 32, true)) return -14;
            struct control64 c = {28, 1, 2};
            copy_to_user((void*)message->control, &c, sizeof(c));
            uint32_t credentials[3] = {pid_number(packet->sender, task_pid_namespace()), credentials_visible_id(packet->credentials[1], false), credentials_visible_id(packet->credentials[2], true)};
            copy_to_user((void*)(message->control + sizeof(c)), credentials, sizeof(credentials));
            clear_user(message->control + 28, 4); written = 32;
        } else message->flags |= CTRUNC;
    }
    if (packet->rights_count) {
        uint64_t available = capacity - written;
        uint64_t room = available >= 24 ? (available - 16) / 4 : 0;
        unsigned count = room < packet->rights_count ? (unsigned)room : packet->rights_count;
        if (count > packet->rights_count) count = packet->rights_count;
        while (count && ((16ULL + count * 4 + 7) & ~7ULL) > available) count--;
        uint64_t bytes = (16ULL + count * 4 + 7) & ~7ULL;
        if (count && !accessible(message->control + written, bytes, true)) return -14;
        unsigned installed = 0;
        for (; installed < count; installed++) {
            int fd = fd_install(packet->rights[installed], 0, (flags & 0x40000000ULL) != 0);
            if (fd < 0) break;
            copy_to_user((void*)(message->control + written + 16 + installed * 4), &fd, 4);
        }
        if (installed) {
            struct control64 c = {16ULL + installed * 4, 1, 1};
            copy_to_user((void*)(message->control + written), &c, sizeof(c));
            uint64_t aligned = (c.size + 7) & ~7ULL;
            clear_user(message->control + written + c.size, aligned - c.size); written += aligned;
        }
        if (installed < packet->rights_count) message->flags |= CTRUNC;
    }
    message->control_size = written; return 0;
}
static int64_t receive_vectors(struct unix_socket* socket, const struct iovec64* vectors, uint64_t count,
                               uint64_t capacity, struct message64* message, uint64_t flags) {
    if (flags & ~(DONTWAIT | PEEK | (uint64_t)TRUNC | 0x100ULL | 0x40000000ULL)) return -95;
    if (socket->type == 1 && (flags & 0x100)) return -95;
    message->flags = 0; message->name_size = 0;
    if (socket->state != CONNECTED) return -107;
    for (;;) {
        struct unix_socket* peer = socket->peer;
        if (!socket->head && socket->error) {
            int error = socket->error; socket->error = 0; event_source_changed(socket, 0); return -error;
        }
        if (socket->read_closed || (!socket->head && (!peer || peer->write_closed))) { message->control_size = 0; return 0; }
        if (!capacity && socket->type == 1) { message->control_size = 0; return 0; }
        if (socket->head) break;
        if (flags & DONTWAIT) return -11;
        int error = task_wait_channel(socket); if (error) return error;
    }
    struct packet* packet = socket->head;
    uint64_t remaining = packet->size - packet->consumed;
    uint64_t wanted = capacity < remaining ? capacity : remaining, check = wanted;
    for (uint64_t i = 0; i < count && check; i++) {
        uint64_t size = vectors[i].size < check ? vectors[i].size : check;
        if (size && !accessible(vectors[i].address, size, true)) return -14;
        check -= size;
    }
    int error = controls_write(socket, packet, message, flags); if (error) return error;
    uint64_t done = 0;
    for (uint64_t i = 0; i < count && done < wanted; i++) {
        uint64_t bytes = vectors[i].size < wanted - done ? vectors[i].size : wanted - done;
        packet_copy(packet, packet->consumed + done, (void*)vectors[i].address, bytes); done += bytes;
    }
    if (socket->type != 1 && wanted < remaining) message->flags |= TRUNC;
    if (!(flags & PEEK)) {
        if (socket->type != 1 || wanted == remaining) {
            socket->head = packet->next; if (!socket->head) socket->tail = NULL;
            if (socket->queued) socket->queued--;
            packet_free(packet); task_wake_channel(socket);   /* a datagram sender may wait for queue room */
        } else {
            packet->consumed += wanted;
            release_rights(packet);
        }
    }
    event_source_changed(socket, 0);
    return socket->type != 1 && (flags & TRUNC) ? (int64_t)remaining : (int64_t)done;
}
/* ---- connectionless SOCK_DGRAM ---- */
static struct binding* abstract_binding(const struct unix_address* address);
static struct unix_socket* dgram_target(const struct unix_address* address, int* error) {
    struct binding* binding = NULL; *error = 0;
    if (!address->length) { *error = -22; return NULL; }
    if (!address->name[0]) binding = abstract_binding(address);
    else {
        uint64_t inode; *error = unix_path_inode((const char*)address->name, false, &inode); if (*error) return NULL;
        for (struct binding* b = bindings; b; b = b->next) if (b->address.name[0] && b->inode == inode) { binding = b; break; }
    }
    if (!binding || binding->socket->closed || binding->socket->read_closed) { *error = -111; return NULL; }
    if (binding->socket->type != 2) { *error = -91; return NULL; }
    return binding->socket;
}
static int64_t dgram_send(struct unix_socket* socket, const struct unix_address* to, const struct iovec64* vectors, uint64_t count,
                          uint64_t total, struct open_file** rights, unsigned rights_count, uint64_t flags) {
    if (flags & 0x1ULL) return -95;
    if (socket->write_closed) { if (!(flags & NOSIGNAL)) signals_enqueue(signals_current(), 13, 0, task_visible_pid(task_pid()), task_kernel_uid(), 0, false); return -32; }
    if (!to) { if (!socket->dgram_connected) return -107; to = &socket->dgram_peer; }
    if (total > 212992) return -90;                                             /* EMSGSIZE: larger than the socket buffer */
    for (uint64_t i = 0; i < count; i++) if (vectors[i].size && !accessible(vectors[i].address, vectors[i].size, false)) return -14;
    struct unix_address target_address = *to; int error;
    struct unix_socket* target;
    for (;;) {   /* the target is looked up again after every wait: it may have closed meanwhile */
        target = dgram_target(&target_address, &error); if (!target) return error;
        if (target->queued < UNIX_DGRAM_QLEN) break;
        if (flags & DONTWAIT) return -11;
        if ((error = task_wait_channel(target))) return error;
    }
    struct packet* packet = (void*)page_alloc(); if (!packet) return -12;
    int accounting = reserve_rights(rights_count, &packet->owner);
    if (accounting) { packet_free(packet); return accounting; }
    packet->size = total; packet->rights_count = rights_count;
    packet->credentials[0] = task_pid(); packet->credentials[1] = task_kernel_uid();
    packet->sender = pid_lookup(task_pid(), pid_namespace_root()); pid_retain(packet->sender);
    packet->credentials[2] = credentials_kernel_id(credentials_current(), true, 0);
    packet->source_length = socket->local_address.length; memcpy(packet->source, socket->local_address.name, socket->local_address.length);
    for (unsigned i = 0; i < rights_count; i++) { packet->rights[i] = rights[i]; fd_reference(rights[i]); }
    struct chunk* tail = NULL;
    for (uint64_t i = 0; i < count; i++) {
        for (uint64_t done = 0; done < vectors[i].size;) {
            if (!tail || tail->used == sizeof(tail->data)) {
                struct chunk* next = (void*)page_alloc(); if (!next) { packet_free(packet); return -12; }
                if (tail) tail->next = next; else packet->data = next;
                tail = next;
            }
            uint64_t bytes = sizeof(tail->data) - tail->used; if (bytes > vectors[i].size - done) bytes = vectors[i].size - done;
            copy_from_user(tail->data + tail->used, (void*)(vectors[i].address + done), bytes); tail->used += bytes; done += bytes;
        }
    }
    if (target->tail) target->tail->next = packet; else target->head = packet;
    target->tail = packet; target->queued++; task_wake_channel(target); event_source_changed(target, 1);
    return (int64_t)total;
}
/* one datagram from the socket's own queue; *from gets the sender's address */
static int64_t dgram_receive(struct unix_socket* socket, const struct iovec64* vectors, uint64_t count, uint64_t capacity,
                             struct message64* message, uint64_t flags, struct unix_address* from) {
    if (flags & ~(DONTWAIT | PEEK | (uint64_t)TRUNC | 0x100ULL | 0x40000000ULL)) return -95;
    message->flags = 0; message->name_size = 0; from->length = 0;
    for (;;) {
        if (!socket->head && socket->error) { int error = socket->error; socket->error = 0; return -error; }
        if (socket->head) break;
        if (socket->read_closed) { message->control_size = 0; return 0; }
        if (flags & DONTWAIT) return -11;
        int error = task_wait_channel(socket); if (error) return error;
    }
    struct packet* packet = socket->head;
    from->length = packet->source_length; memcpy(from->name, packet->source, packet->source_length);
    uint64_t wanted = capacity < packet->size ? capacity : packet->size, check = wanted;
    for (uint64_t i = 0; i < count && check; i++) {
        uint64_t size = vectors[i].size < check ? vectors[i].size : check;
        if (size && !accessible(vectors[i].address, size, true)) return -14;
        check -= size;
    }
    int error = controls_write(socket, packet, message, flags); if (error) return error;
    uint64_t done = 0;
    for (uint64_t i = 0; i < count && done < wanted; i++) {
        uint64_t bytes = vectors[i].size < wanted - done ? vectors[i].size : wanted - done;
        packet_copy(packet, done, (void*)vectors[i].address, bytes); done += bytes;
    }
    uint64_t size = packet->size;
    if (wanted < size) message->flags |= TRUNC;
    if (!(flags & PEEK)) {
        socket->head = packet->next; if (!socket->head) socket->tail = NULL;
        if (socket->queued) socket->queued--;
        packet_free(packet); task_wake_channel(socket);
    }
    event_source_changed(socket, 0);
    return (flags & TRUNC) ? (int64_t)size : (int64_t)done;
}
static bool dgram_unconnected(const struct unix_socket* socket) { return socket->type == 2 && socket->state == UNCONNECTED; }
int64_t ipc_write(struct unix_socket* socket, uint64_t address, uint64_t count, uint64_t flags) {
    if (count > 0x7FFFFFFFFFFFFFFFULL) return -22;
    struct iovec64 vector = {address, count};
    if (dgram_unconnected(socket)) return dgram_send(socket, NULL, &vector, 1, count, NULL, 0, flags);
    return send_vectors(socket, &vector, 1, count, NULL, 0, flags);
}
int64_t ipc_vectors(struct unix_socket* socket, const struct iovec64* vectors, unsigned count, uint64_t total, bool write, uint64_t flags) {
    struct message64 message = {0}; struct unix_address from;
    if (dgram_unconnected(socket)) return write ? dgram_send(socket, NULL, vectors, count, total, NULL, 0, flags) :
                                                  dgram_receive(socket, vectors, count, total, &message, flags, &from);
    return write ? send_vectors(socket, vectors, count, total, NULL, 0, flags) :
                   receive_vectors(socket, vectors, count, total, &message, flags);
}
int64_t ipc_read(struct unix_socket* socket, uint64_t address, uint64_t count, uint64_t flags) {
    struct iovec64 vector = {address, count}; struct message64 message = {0}; struct unix_address from;
    if (dgram_unconnected(socket)) return dgram_receive(socket, &vector, 1, count, &message, flags, &from);
    return receive_vectors(socket, &vector, 1, count, &message, flags);
}
static int socket_type(uint64_t domain, uint64_t type, uint64_t protocol) {
    if (domain != 1) return -97;
    if (protocol || (type & ~(0x80000ULL | 0x800ULL | 15ULL))) return -22;
    unsigned kind = type & 15; if (kind != 1 && kind != 2 && kind != 5) return -94;
    return kind;
}
static struct pid_identity* current_credentials(uint32_t credentials[3]) {
    credentials[0] = task_pid(); credentials[1] = credentials_kernel_id(credentials_current(), false, 1);
    credentials[2] = credentials_kernel_id(credentials_current(), true, 1);
    struct pid_identity* pid = pid_lookup(task_pid(), pid_namespace_root()); pid_retain(pid); return pid;
}
static struct socket_pair* pair_create(unsigned type) {
    struct socket_pair* pair = (void*)page_alloc(); if (!pair) return NULL;
    live_pairs++;
    for (unsigned i = 0; i < 2; i++) { pair->ends[i].pair = pair; pair->ends[i].type = type; pair->ends[i].closed = true; }
    return pair;
}
static int socket_create(uint64_t domain, uint64_t type, uint64_t protocol) {
    int kind = socket_type(domain, type, protocol); if (kind < 0) return kind;
    struct socket_pair* pair = pair_create(kind); if (!pair) return -12;
    pair->ends[0].closed = false;
    int fd = fd_create_socket(&pair->ends[0], type);
    if (fd < 0) { live_pairs--; page_free((uint64_t)pair); }
    return fd;
}
static int address_load(uint64_t pointer, uint64_t size, struct unix_address* address) {
    if (size < 2 || size > 110) return -22;
    if (!accessible(pointer, size, false)) return -14;
    uint16_t family; copy_from_user(&family, (void*)pointer, 2); if (family != 1) return -22;
    memset(address, 0, sizeof(*address)); address->length = size - 2;
    if (address->length) copy_from_user(address->name, (void*)(pointer + 2), address->length);
    if (address->length && address->name[0]) {
        unsigned length = 0; while (length < address->length && address->name[length]) length++;
        address->name[length] = 0; address->length = length + 1;
    }
    return 0;
}
static int address_output(uint64_t pointer, uint64_t size_pointer, const struct unix_address* address, bool copy) {
    if (!accessible(size_pointer, 4, true)) return -14;
    uint32_t actual = address->length + 2, amount = uget32(size_pointer); if (amount > actual) amount = actual;
    if (amount && !accessible(pointer, amount, true)) return -14;
    if (copy) {
        uint8_t result[111] = {1, 0}; memcpy(result + 2, address->name, address->length);
        if (amount) copy_to_user((void*)pointer, result, amount);
        uput32(size_pointer, actual);
    }
    return 0;
}
static struct binding* abstract_binding(const struct unix_address* address) {
    for (struct binding* b = bindings; b; b = b->next)
        if (!b->address.name[0] && b->address.length == address->length && !memcmp(b->address.name, address->name, address->length)) return b;
    return NULL;
}
static int bind_address(struct unix_socket* socket, struct unix_address* address) {
    if (socket->state != UNCONNECTED || socket->local_address.length) return -22;
    if (!address->length) {
        address->length = 6; address->name[0] = 0;
        unsigned attempts = 0;
        do {
            uint32_t value = ++autobind_sequence & 0xFFFFF;
            for (unsigned i = 0; i < 5; i++) address->name[i + 1] = "0123456789abcdef"[(value >> ((4 - i) * 4)) & 15];
            if (++attempts > 0x100000) return -98;
        } while (abstract_binding(address));
    }
    if (!address->name[0] && abstract_binding(address)) return -98;
    struct binding* binding = (void*)page_alloc(); if (!binding) return -12;
    if (address->name[0]) {
        int error = unix_path_inode((const char*)address->name, true, &binding->inode);
        if (error) { page_free((uint64_t)binding); return error; }
    }
    binding->socket = socket; binding->address = *address; binding->next = bindings; bindings = binding;
    socket->binding = binding; socket->local_address = *address; return 0;
}
static int socket_bind(struct unix_socket* socket, uint64_t pointer, uint64_t size) {
    struct unix_address address; int error = address_load(pointer, size, &address);
    return error ? error : bind_address(socket, &address);
}
static int socket_listen(struct unix_socket* socket, int backlog) {
    if (socket->state == CONNECTED || !socket->local_address.length) return -22;
    socket->backlog = backlog < 0 || backlog > 4096 ? 4096 : (unsigned)backlog;
    socket->state = LISTENING;
    pid_release(socket->listen_pid); socket->listen_pid = current_credentials(socket->listen_credentials);
    task_wake_channel(socket); event_source_changed(socket, 0); return 0;
}
static int socket_connect(struct unix_socket* socket, uint64_t pointer, uint64_t size, bool nonblock) {
    if (socket->state == CONNECTED) return -106;
    if (socket->state != UNCONNECTED) return -22;
    struct unix_address address; int error = address_load(pointer, size, &address); if (error) return error;
    if (!address.length) return -22;
    if (socket->pass_credentials && !socket->local_address.length) {
        struct unix_address local = {0}; error = bind_address(socket, &local); if (error) return error;
    }
    struct binding* binding = NULL;
    if (!address.name[0]) binding = abstract_binding(&address);
    else {
        uint64_t inode; error = unix_path_inode((const char*)address.name, false, &inode); if (error) return error;
        for (struct binding* b = bindings; b; b = b->next) if (b->address.name[0] && b->inode == inode) { binding = b; break; }
    }
    if (!binding) return -111;
    struct unix_socket* listener = binding->socket;
    if (listener->type != socket->type) return -91;
    struct socket_pair* held = listener->pair; held->changing++;
    for (;;) {
        if (socket->state != UNCONNECTED) { error = -106; break; }
        if (listener->closed || listener->state != LISTENING || listener->read_closed) { error = -111; break; }
        if (listener->pending_count <= listener->backlog) {
            struct socket_pair* pair = pair_create(socket->type); if (!pair) { error = -12; break; }
            struct unix_socket* child = &pair->ends[0]; child->closed = false; child->state = CONNECTED;
            child->peer = socket; socket->peer = child; socket->state = CONNECTED;
            child->local_address = listener->local_address; child->peer_address = socket->local_address;
            socket->peer_address = child->local_address; child->pass_credentials = listener->pass_credentials;
            child->peer_pid = current_credentials(child->peer_credentials); child->have_peer_credentials = true;
            memcpy(socket->peer_credentials, listener->listen_credentials, sizeof(socket->peer_credentials)); socket->have_peer_credentials = true;
            socket->peer_pid = listener->listen_pid; pid_retain(socket->peer_pid);
            if (listener->pending_tail) listener->pending_tail->pending_next = child; else listener->pending_head = child;
            listener->pending_tail = child; listener->pending_count++;
            task_wake_channel(listener); event_source_changed(listener, 1); event_source_changed(socket, 4); error = 0; break;
        }
        if (nonblock) { error = -11; break; }
        error = task_wait_channel(listener); if (error) break;
    }
    held->changing--; reap(held); return error;
}
static int socket_accept(struct unix_socket* listener, uint64_t address, uint64_t size_pointer, uint64_t flags, bool nonblock) {
    if (flags & ~(0x80000ULL | 0x800ULL)) return -22;
    for (;;) {
        if (listener->state != LISTENING || listener->read_closed) return -22;
        if (listener->pending_head) break;
        if (nonblock) return -11;
        int error = task_wait_channel(listener); if (error) return error;
    }
    struct unix_socket* child = listener->pending_head;
    if (address) { int error = address_output(address, size_pointer, &child->peer_address, false); if (error) return error; }
    int fd = fd_create_socket(child, flags); if (fd < 0) return fd;
    listener->pending_head = child->pending_next; child->pending_next = NULL;
    if (!listener->pending_head) listener->pending_tail = NULL;
    listener->pending_count--; task_wake_channel(listener); event_source_changed(listener, 0);
    if (address) { int error = address_output(address, size_pointer, &child->peer_address, true); if (error) { fd_close(fd); return error; } }
    return fd;
}
static int socketpair_create(uint64_t domain, uint64_t type, uint64_t protocol, uint64_t address) {
    int kind = socket_type(domain, type, protocol); if (kind < 0) return kind;
    if (!accessible(address, 8, true)) return -14;
    struct socket_pair* pair = pair_create(kind); if (!pair) return -12;
    for (unsigned i = 0; i < 2; i++) {
        pair->ends[i].peer = &pair->ends[i ^ 1]; pair->ends[i].state = CONNECTED;
        pair->ends[i].peer_pid = current_credentials(pair->ends[i].peer_credentials); pair->ends[i].have_peer_credentials = true;
    }
    pair->ends[0].closed = false;
    int a = fd_create_socket(&pair->ends[0], type);
    if (a < 0) { pair->ends[0].closed = true; reap(pair); return a; }
    pair->ends[1].closed = false;
    int b = fd_create_socket(&pair->ends[1], type);
    if (b < 0) { pair->ends[1].closed = true; fd_close(a); return b; }
    int ends[2] = {a, b}; if (copy_to_user((void*)address, ends, sizeof(ends))) { fd_close(a); fd_close(b); return -14; } return 0;
}
static int socket_option(struct unix_socket* socket, struct registers* r, bool get) {
    if (r->rsi != 1) return -92;
    if (!get) {
        if (r->rdx != 16) return -92;
        if (r->r8 < 4) return -22;
        if (!accessible(r->r10, 4, false)) return -14;
        socket->pass_credentials = uget32(r->r10) != 0; return 0;
    }
    if (!accessible(r->r8, 4, true)) return -14;
    uint32_t value[3] = {0}; uint32_t size = 4;
    switch (r->rdx) {
    case 3: value[0] = socket->type; break;
    case 4: value[0] = socket->error; break;
    case 16: value[0] = socket->pass_credentials; break;
    case 17:
        value[0] = socket->have_peer_credentials ? pid_number(socket->peer_pid, task_pid_namespace()) : 0;
        value[1] = socket->have_peer_credentials ? credentials_visible_id(socket->peer_credentials[1], false) : 0xFFFFFFFFU;
        value[2] = socket->have_peer_credentials ? credentials_visible_id(socket->peer_credentials[2], true) : 0xFFFFFFFFU;
        size = 12; break;
    case 30: value[0] = socket->state == LISTENING; break;
    case 7: case 8: value[0] = 212992; break; /* SO_SNDBUF/SO_RCVBUF: Linux default; queues here are not byte-limited */
    case 38: break;
    case 39: value[0] = 1; break;
    default: return -92;
    }
    { uint32_t capacity = uget32(r->r8); if (size > capacity) size = capacity; }
    if (size && !accessible(r->r10, size, true)) return -14;
    copy_to_user((void*)r->r10, value, size); uput32(r->r8, size);
    if (r->rdx == 4) { socket->error = 0; event_source_changed(socket, 0); }
    return 0;
}
int64_t ipc_socket_call(struct unix_socket* socket, uint64_t status_flags, struct registers* r) {
    uint64_t flags = r->rax == 46 || r->rax == 47 ? r->rdx : r->r10;
    if (status_flags & 0x800) flags |= DONTWAIT;
    if (r->rax == 49) return socket_bind(socket, r->rsi, r->rdx);
    if (r->rax == 50) return socket->type == 2 ? -95 : socket_listen(socket, (int32_t)r->rsi);
    if (r->rax == 42 && dgram_unconnected(socket)) {   /* datagram connect: the default destination (AF_UNSPEC dissolves it) */
        if (r->rdx >= 2 && accessible(r->rsi, 2, false) && uget16(r->rsi) == 0) { socket->dgram_connected = false; return 0; }
        struct unix_address address; int error = address_load(r->rsi, r->rdx, &address); if (error) return error;
        if (!dgram_target(&address, &error)) return error;
        socket->dgram_peer = address; socket->dgram_connected = true; return 0;
    }
    if (r->rax == 42) return socket_connect(socket, r->rsi, r->rdx, (status_flags & 0x800) != 0);
    if (r->rax == 43 || r->rax == 288) return socket_accept(socket, r->rsi, r->rdx, r->rax == 288 ? r->r10 : 0, (status_flags & 0x800) != 0);
    if (r->rax == 54 || r->rax == 55) return socket_option(socket, r, r->rax == 55);
    if (r->rax == 48) {
        if (r->rsi > 2) return -22;
        if (socket->state == UNCONNECTED) return -107;
        if (r->rsi != 1) { socket->read_closed = true; ipc_discard(socket); }
        if (r->rsi != 0) socket->write_closed = true;
        task_wake_channel(socket); event_source_changed(socket, 0);
        if (socket->peer) { task_wake_channel(socket->peer); event_source_changed(socket->peer, 0); }
        return 0;
    }
    if (r->rax == 52 && dgram_unconnected(socket)) return socket->dgram_connected ? address_output(r->rsi, r->rdx, &socket->dgram_peer, true) : -107;
    if (r->rax == 51 || r->rax == 52) {
        if (r->rax == 52 && socket->state != CONNECTED) return -107;
        return address_output(r->rsi, r->rdx, r->rax == 51 ? &socket->local_address : &socket->peer_address, true);
    }
    if (r->rax == 44) {
        if (dgram_unconnected(socket) && r->r8) {
            struct unix_address to; int error = address_load(r->r8, r->r9, &to); if (error) return error;
            if (r->rdx > 0x7FFFFFFFFFFFFFFFULL) return -22;
            struct iovec64 vector = {r->rsi, r->rdx};
            return dgram_send(socket, &to, &vector, 1, r->rdx, NULL, 0, flags);
        }
        if (r->r8 || r->r9) return socket->type == 2 ? -106 : -106;
        return ipc_write(socket, r->rsi, r->rdx, flags);
    }
    if (r->rax == 45) {
        if (dgram_unconnected(socket)) {
            struct iovec64 vector = {r->rsi, r->rdx}; struct message64 message = {0}; struct unix_address from;
            int64_t result = dgram_receive(socket, &vector, 1, r->rdx, &message, flags, &from);
            if (result >= 0 && r->r8) { int error = address_output(r->r8, r->r9, &from, true); if (error) return error; }
            return result;
        }
        if (r->r8) {
            if (!accessible(r->r9, 4, true)) return -14;
            uput32(r->r9, 0);
        }
        return ipc_read(socket, r->rsi, r->rdx, flags);
    }
    if (!accessible(r->rsi, sizeof(struct message64), r->rax == 47)) return -14;
    struct message64 message; copy_from_user(&message, (void*)r->rsi, sizeof(message));
    struct iovec64 vectors[MAX_IOV]; uint64_t total;
    int error = vectors_load(&message, vectors, &total); if (error) return error;
    if (r->rax == 46) {
        struct unix_address to; bool named = dgram_unconnected(socket) && message.name;
        if (named) { error = address_load(message.name, message.name_size, &to); if (error) return error; }
        else if (message.name || message.name_size) return -106;
        struct open_file* rights[MAX_RIGHTS]; unsigned count;
        error = controls_load(&message, rights, &count); if (error) return error;
        if (dgram_unconnected(socket)) return dgram_send(socket, named ? &to : NULL, vectors, message.count, total, rights, count, flags);
        return send_vectors(socket, vectors, message.count, total, rights, count, flags);
    }
    if (dgram_unconnected(socket)) {
        uint64_t name = message.name; uint32_t name_capacity = message.name_size; struct unix_address from;
        int64_t result = dgram_receive(socket, vectors, message.count, total, &message, flags, &from);
        if (result < 0) return result;
        if (name && name_capacity) {
            uint32_t actual = from.length + 2, amount = name_capacity < actual ? name_capacity : actual;
            if (!accessible(name, amount, true)) return -14;
            uint8_t encoded[111] = {1, 0}; memcpy(encoded + 2, from.name, from.length); copy_to_user((void*)name, encoded, amount);
            message.name_size = actual;
        }
        if (!accessible(r->rsi, sizeof(message), true)) return -14;
        copy_to_user((void*)r->rsi, &message, sizeof(message));
        return result;
    }
    int64_t result = receive_vectors(socket, vectors, message.count, total, &message, flags);
    if (result >= 0) {
        if (!accessible(r->rsi, sizeof(message), true)) return -14;
        copy_to_user((void*)r->rsi, &message, sizeof(message));
    }
    return result;
}
int64_t ipc_call(struct registers* r) {
    if (r->rax == 41) return socket_create(r->rdi, r->rsi, r->rdx);
    if (r->rax == 53) return socketpair_create(r->rdi, r->rsi, r->rdx, r->r10);
    struct open_file* description = fd_lookup(r->rdi); if (!description) return -9;
    uint64_t flags; struct unix_socket* socket = fd_socket(r->rdi, &flags); if (!socket) return -88;
    fd_pin(description); int64_t result = ipc_socket_call(socket, flags, r); fd_unpin(description); return result;
}
