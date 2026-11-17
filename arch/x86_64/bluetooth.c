/* Bluetooth core (bt.h): controllers, the HCI command/event engine, ACL links, /dev/vhci and the HCI sockets.
 *
 * Controllers come from transports: /dev/vhci (char 10:137, root; every open is one virtual controller - BlueZ's
 * btvirt/hciemu or a test program answers) and USB (btusb.c). The first vhci write may be {0xFF, opcode} (opcode bits
 * 0-1 type, 0 = primary; 0x40/0x80 configuration quirks accepted and ignored); the reply {0xFF, opcode, index} is read
 * first. Reads return host->controller H:4 packets (command, ACL, SCO, ISO), writes are controller->host packets.
 * Commands go through one FIFO per controller: one in flight per Num_HCI_Command_Packets credit, 2 s timeout (a
 * timed-out command completes with -ETIMEDOUT and the credit is restored). ACL data is fragmented to the controller's
 * buffer size and sent only against its buffer credits (Number Of Completed Packets returns them); received fragments
 * are reassembled into whole L2CAP frames (at most 65539 bytes; malformed or oversized frames are dropped).
 * Sockets: AF_BLUETOOTH SOCK_RAW BTPROTO_HCI with sockaddr_hci {family, dev, channel}:
 *  - HCI_CHANNEL_RAW (0): one controller's packets through HCI_FILTER (SOL_HCI 2), HCI_DATA_DIR/HCI_TIME_STAMP
 *    ancillary data; writes (CAP_NET_RAW) are queued like the kernel's own commands. dev 0xFFFF: ioctls only
 *    (HCIGETDEVLIST, HCIGETDEVINFO, HCIGETCONNLIST, HCIGETCONNINFO, HCIDEVUP/DOWN/RESET/RESTAT, HCISETSCAN).
 *  - HCI_CHANNEL_USER (1): exclusive raw access to a controller that is down (userspace stacks); the kernel stack
 *    does not touch it meanwhile (mgmt sees Index Removed).
 *  - HCI_CHANNEL_MONITOR (2, CAP_NET_RAW): btmon's view: indexes, every packet, and the mgmt traffic of every control
 *    socket (CTRL_OPEN/CLOSE/COMMAND/EVENT); SO_TIMESTAMP gives SCM_TIMESTAMP.
 *  - HCI_CHANNEL_CONTROL (3): the mgmt API (bt_mgmt.c).
 * L2CAP (BTPROTO_L2CAP 0) and RFCOMM (BTPROTO_RFCOMM 3) sockets are in bt_l2cap.c / bt_rfcomm.c; AF_ALG in
 * bt_alg.c. SCO, ISO, BNEP, HIDP, CMTP: EPROTONOSUPPORT. Everything runs under the kernel lock. */
#include "bt.h"
#include "hwnode.h"
#include "uaccess.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

/* ---------- memory, user copies, packets ---------- */
void* bt_zalloc(uint64_t bytes) { return bytes ? (void*)arena_alloc(bytes) : NULL; }
void bt_free(void* p, uint64_t bytes) { if (p) arena_free((uint64_t)p, bytes); }
int bt_from_user(void* kernel, uint64_t user, uint64_t n) { if (!n) return 0; if (!accessible(user, n, false)) return E_FAULT; copy_from_user(kernel, (const void*)user, n); return 0; }
int bt_to_user(uint64_t user, const void* kernel, uint64_t n) { if (!n) return 0; if (!accessible(user, n, true)) return E_FAULT; copy_to_user((void*)user, kernel, n); return 0; }
uint64_t bt_now(void) { return native_clock_ns(); }
void bt_wake(void* channel) { task_wake_channel(channel); event_source_changed(channel, 1U | 4U); }
bool bt_privileged(unsigned capability) { return credentials_capable(credentials_current(), NULL, capability); }
struct bt_pkt* bt_pkt_new(unsigned len) {
    uint64_t bytes = sizeof(struct bt_pkt) + len; struct bt_pkt* p = bt_zalloc(bytes);
    if (p) { p->len = len; p->alloc = (uint32_t)bytes; }
    return p;
}
void bt_pkt_free(struct bt_pkt* p) { if (p) bt_free(p, p->alloc); }
void bt_queue_put(struct bt_queue* q, struct bt_pkt* p) { p->next = NULL; if (q->tail) q->tail->next = p; else q->head = p; q->tail = p; q->count++; q->bytes += p->len; }
struct bt_pkt* bt_queue_take(struct bt_queue* q) {
    struct bt_pkt* p = q->head; if (!p) return NULL;
    q->head = p->next; if (!q->head) q->tail = NULL; q->count--; q->bytes -= p->len; p->next = NULL; return p;
}
void bt_queue_purge(struct bt_queue* q) { struct bt_pkt* p; while ((p = bt_queue_take(q))) bt_pkt_free(p); }

/* ---------- the worker: mgmt commands off the writer's context ---------- */
#define WORKER_STACK (32U * 1024U)
void usb_context_switch(uint64_t* save, uint64_t load);   /* usb.c: callee-saved registers + stack switch */
struct mreq { struct mreq* next; void* socket; bool trusted; unsigned length; uint8_t data[]; };
static struct mreq *mreq_head, *mreq_tail; static unsigned mreq_count;
static uint64_t worker_stack, worker_rsp, worker_caller_rsp, worker_deadline;
static void* worker_channel; static bool worker_running, worker_ready, worker_waiting;
static void worker_main(void);
bool bt_in_worker(void) { return worker_running; }
void bt_worker_wake(void) { worker_ready = true; }
int bt_wait(void* channel, uint64_t deadline) {
    if (!worker_running) return task_wait_until(channel, deadline);
    worker_channel = channel; worker_deadline = deadline; worker_waiting = true; worker_ready = false;
    usb_context_switch(&worker_rsp, worker_caller_rsp);                  /* back to whoever resumed the worker */
    worker_waiting = false; worker_channel = NULL;
    return deadline && bt_now() >= deadline ? E_TIMEDOUT : 0;
}
/* Resume the worker if it has something to do (new requests, a wait that may be over). Safe points only: never from
 * inside event processing, so the worker never runs over a half-updated connection or command queue. */
static void worker_run(void) {
    if (worker_running || !worker_stack) return;
    if (worker_waiting && !worker_ready && !(worker_deadline && bt_now() >= worker_deadline)) return;
    if (!worker_waiting && !mreq_head) return;
    worker_running = true; worker_ready = false;
    usb_context_switch(&worker_caller_rsp, worker_rsp);
    worker_running = false;
}
static void worker_main(void) {
    for (;;) {
        struct mreq* r = mreq_head;
        if (!r) { usb_context_switch(&worker_rsp, worker_caller_rsp); continue; }   /* idle */
        mreq_head = r->next; if (!mreq_head) mreq_tail = NULL; mreq_count--;
        void mgmt_socket_release_hold(void* socket); bool mgmt_socket_alive(void* socket);
        if (r->socket && mgmt_socket_alive(r->socket)) mgmt_socket_command(r->socket, r->data, r->length, r->trusted);   /* closed meanwhile: dropped */
        if (r->socket) mgmt_socket_release_hold(r->socket);
        bt_free(r, sizeof(*r) + r->length);
    }
}
static bool worker_init(void) {
    if (worker_stack) return true;
    worker_stack = (uint64_t)bt_zalloc(WORKER_STACK); if (!worker_stack) return false;
    uint64_t* stack = (uint64_t*)(worker_stack + WORKER_STACK);
    *--stack = 0; *--stack = (uint64_t)worker_main;                        /* the first switch "returns" into worker_main */
    for (unsigned i = 0; i < 6; i++) *--stack = 0;
    worker_rsp = (uint64_t)stack; return true;
}

/* ---------- registry ---------- */
static struct bt_dev* devs[BT_MAX_DEVICES];
static uint32_t generations, conn_ids;
static struct bt_conn* conn_table[64];     /* id -> conn for bt_conn_get (ids are unique, slots reused) */
struct bt_dev* bt_dev_by_index(int index) { return index >= 0 && index < (int)BT_MAX_DEVICES && devs[index] && devs[index]->alive ? devs[index] : NULL; }
struct bt_dev* bt_dev_get(int index, uint32_t generation) { struct bt_dev* d = bt_dev_by_index(index); return d && d->generation == generation ? d : NULL; }
struct bt_dev* bt_dev_route(const uint8_t* src, bool le) {
    for (unsigned i = 0; i < BT_MAX_DEVICES; i++) {
        struct bt_dev* d = bt_dev_by_index((int)i);
        if (!d || !(d->flags & HCI_UP) || d->user) continue;
        if (le ? !(d->settings & ST_LE) : !(d->settings & ST_BREDR)) continue;
        if (!src || bt_addr_any(src) || !memcmp(src, d->bdaddr, 6)) return d;
    }
    return NULL;
}

/* ---------- HCI sockets ---------- */
enum { CH_RAW = 0, CH_USER = 1, CH_MONITOR = 2, CH_CONTROL = 3 };
#define SOCKETS 48U
#define SOCKET_QUEUE 96U
#define MONITOR_QUEUE 1024U   /* a monitor sees every command, event and mgmt message: room for a reader that starts late */
struct hci_socket {
    bool bound; unsigned channel; int dev; uint32_t dev_gen; uint64_t id; uint32_t cookie;
    uint32_t type_mask, event_mask[2]; uint16_t opcode; bool data_dir, hci_stamp, so_stamp, monitor_open;
    struct bt_queue rx; char comm[16]; unsigned holds; bool dead;   /* holds: queued mgmt requests (the worker frees a dead one) */
};
static struct hci_socket* sockets[SOCKETS];
static uint64_t socket_ids;
static void socket_push(struct hci_socket* s, const uint8_t* a, unsigned alen, const uint8_t* b, unsigned blen, bool incoming) {
    if (s->rx.count >= (s->channel == CH_MONITOR ? MONITOR_QUEUE : SOCKET_QUEUE)) bt_pkt_free(bt_queue_take(&s->rx));   /* oldest dropped */
    struct bt_pkt* p = bt_pkt_new(alen + blen); if (!p) return;
    memcpy(p->data, a, alen); if (blen) memcpy(p->data + alen, b, blen); p->stamp = native_realtime_ns() << 1 | (incoming ? 1 : 0);
    bt_queue_put(&s->rx, p); bt_wake(s);
}
static void monitor(unsigned opcode, int index, const uint8_t* data, unsigned length) {
    uint8_t header[6]; bt_put16(header, opcode); bt_put16(header + 2, index < 0 ? 0xFFFF : (unsigned)index); bt_put16(header + 4, length);
    for (unsigned i = 0; i < SOCKETS; i++) {
        struct hci_socket* s = sockets[i];
        if (s && s->bound && s->channel == CH_MONITOR) socket_push(s, header, 6, data, length, true);
    }
}
static void monitor_new_index(struct bt_dev* d, struct hci_socket* only) {
    uint8_t info[16] = {0}; info[0] = 0; info[1] = d->driver ? (uint8_t)d->driver->bus : 0;
    memcpy(info + 2, d->bdaddr, 6); info[8] = 'h'; info[9] = 'c'; info[10] = 'i'; info[11] = (uint8_t)('0' + d->index);
    uint8_t index_info[8]; memcpy(index_info, d->bdaddr, 6); bt_put16(index_info + 6, d->manufacturer);
    if (!only) { monitor(0, d->index, info, 16); return; }
    uint8_t header[6]; bt_put16(header, 0); bt_put16(header + 2, (unsigned)d->index); bt_put16(header + 4, 16); socket_push(only, header, 6, info, 16, true);
    if (d->flags & HCI_UP) {
        bt_put16(header, 8); bt_put16(header + 4, 0); socket_push(only, header, 6, NULL, 0, true);
        bt_put16(header, 10); bt_put16(header + 4, 8); socket_push(only, header, 6, index_info, 8, true);
    }
}
/* mgmt traffic for btmon: CTRL_OPEN/CLOSE of control sockets, their commands and events (bt_mgmt.c calls this) */
static void monitor_ctrl_open(struct hci_socket* s, struct hci_socket* only) {
    uint8_t m[32] = {0}; bt_put32(m, s->cookie); bt_put16(m + 4, 2); m[6] = 1; bt_put16(m + 7, 22); bt_put32(m + 9, 1); m[13] = 16; memcpy(m + 14, s->comm, 16);
    if (!only) { monitor(14, -1, m, 30); return; }
    uint8_t header[6]; bt_put16(header, 14); bt_put16(header + 2, 0xFFFF); bt_put16(header + 4, 30); socket_push(only, header, 6, m, 30, true);
}
void mgmt_monitor_command(int index, const uint8_t* message, unsigned length, bool event, uint32_t cookie) {
    uint8_t m[BT_FRAME_MAX]; if (length < 6) return;                      /* {cookie, opcode/event, parameters} */
    if (length > sizeof(m)) length = sizeof(m);
    bt_put32(m, cookie); memcpy(m + 4, message, 2); memcpy(m + 6, message + 6, length - 6); monitor(event ? 17 : 16, index, m, length);
}
static bool filter_passes(const struct hci_socket* s, const uint8_t* packet, unsigned length) {
    unsigned type = packet[0];
    if (type == PKT_VENDOR || !((s->type_mask >> (type & 31)) & 1)) return false;
    if (type != PKT_EVENT || length < 2) return true;
    unsigned event = packet[1] & 63;
    if (!((s->event_mask[event >> 5] >> (event & 31)) & 1)) return false;
    if (s->opcode && length >= 6 && (packet[1] == 0x0E || packet[1] == 0x0F)) {
        unsigned op = packet[1] == 0x0E ? bt_get16(packet + 4) : (length >= 7 ? bt_get16(packet + 5) : 0);
        if (op != s->opcode) return false;
    }
    return true;
}
/* A packet crossed d (to_controller: host -> controller); from (a raw socket) does not get its own packet back. */
static void distribute(struct bt_dev* d, const uint8_t* packet, unsigned length, bool to_controller, const void* from) {
    unsigned type = packet[0], opcode = type == PKT_COMMAND ? 2 : type == PKT_EVENT ? 3 : type == PKT_ACL ? (to_controller ? 4 : 5) :
                    type == PKT_SCO ? (to_controller ? 6 : 7) : type == PKT_ISO ? (to_controller ? 18 : 19) : 0;
    if (opcode && length >= 1) monitor(opcode, d->index, packet + 1, length - 1);
    for (unsigned i = 0; i < SOCKETS; i++) {
        struct hci_socket* s = sockets[i];
        if (!s || !s->bound || s == from || s->dev != d->index || s->dev_gen != d->generation) continue;
        bool deliver = s->channel == CH_USER ? !to_controller : s->channel == CH_RAW && !d->user && filter_passes(s, packet, length);
        if (deliver) socket_push(s, packet, length, NULL, 0, !to_controller);
    }
}
static void to_controller(struct bt_dev* d, const uint8_t* packet, unsigned length, const void* from) {
    distribute(d, packet, length, true, from);
    if (packet[0] == PKT_COMMAND) d->stat[2]++; else if (packet[0] == PKT_ACL) d->stat[4]++; else if (packet[0] == PKT_SCO) d->stat[6]++;
    d->stat[9] += length - 1;
    if (!d->driver || d->driver->send(d, packet, length) < 0) d->stat[1]++;
}

/* ---------- commands ---------- */
int bt_hci_errno(int status) {
    if (status < 0) return status;
    switch (status) {
    case 0: return 0;
    case 0x04: return E_HOSTDOWN;            /* page timeout */
    case 0x05: case 0x06: case 0x18: return E_ACCES;   /* authentication failure, PIN or key missing, pairing not allowed */
    case 0x07: case 0x0D: return E_NOMEM;
    case 0x08: case 0x22: return E_TIMEDOUT;
    case 0x0E: case 0x0F: return E_CONNREFUSED;
    case 0x13: case 0x14: case 0x15: return E_CONNRESET;
    case 0x16: return E_CONNABORTED;
    case 0x0B: return E_ALREADY;
    case 0x0C: return E_BUSY;
    case 0x01: case 0x11: return E_OPNOTSUPP;
    case 0x12: return E_INVAL;
    default: return E_IO;
    }
}
static void command_finish(struct bt_dev* d, struct bt_cmd* c, int status, const uint8_t* rp, unsigned rlen) {
    if (c->done) c->done(d, c, status, rp, rlen);
    if (c->sync && !c->abandoned) {
        c->status = status; c->rlen = (uint16_t)(rlen < sizeof(c->rp) ? rlen : sizeof(c->rp)); if (c->rlen) memcpy(c->rp, rp, c->rlen);
        c->completed = true; if (c->worker) bt_worker_wake(); else task_wake_channel(c);
    } else bt_free(c, sizeof(*c));
}
static void cmd_kick(struct bt_dev* d) {
    while (d->alive && !d->user && !d->cmd_sent && d->cmd_cnt && d->cmd_head) {
        struct bt_cmd* c = d->cmd_head; d->cmd_head = c->next; if (!d->cmd_head) d->cmd_tail = NULL; d->cmd_queued--; c->next = NULL;
        uint8_t packet[259]; packet[0] = PKT_COMMAND; bt_put16(packet + 1, c->opcode); packet[3] = c->plen; memcpy(packet + 4, c->params, c->plen);
        d->cmd_sent = c; d->cmd_cnt--; d->cmd_deadline = bt_now() + (c->opcode == 0x0C03 ? 5000000000ULL : 2000000000ULL);
        to_controller(d, packet, 4U + c->plen, NULL);
    }
}
static struct bt_cmd* command_new(struct bt_dev* d, unsigned opcode, const void* params, unsigned plen) {
    if (!d || !d->alive || plen > 255 || d->cmd_queued >= 64) return NULL;
    struct bt_cmd* c = bt_zalloc(sizeof(*c)); if (!c) return NULL;
    c->opcode = (uint16_t)opcode; c->plen = (uint8_t)plen; if (plen) memcpy(c->params, params, plen);
    return c;
}
static void command_queue(struct bt_dev* d, struct bt_cmd* c) {
    if (d->cmd_tail) d->cmd_tail->next = c; else d->cmd_head = c; d->cmd_tail = c; d->cmd_queued++; cmd_kick(d);
}
bool bt_cmd_async(struct bt_dev* d, unsigned opcode, const void* params, unsigned plen, bt_cmd_done done, void* ctx, uint64_t arg) {
    struct bt_cmd* c = command_new(d, opcode, params, plen); if (!c) return false;
    c->done = done; c->ctx = ctx; c->arg = arg; command_queue(d, c); return true;
}
static void cmd_timeouts(struct bt_dev* d, uint64_t now) {
    if (!d->cmd_sent || now < d->cmd_deadline) return;
    struct bt_cmd* c = d->cmd_sent; d->cmd_sent = NULL; d->cmd_cnt = 1;
    char text[64] = "[64] BLUETOOTH hci"; text[18] = (char)('0' + d->index); text[19] = 0; strcat(text, " command timeout opcode 0x"); vga_print(text);
    char hexs[8]; const char* digits = "0123456789abcdef"; for (int i = 0; i < 4; i++) hexs[i] = digits[(c->opcode >> (12 - 4 * i)) & 15]; hexs[4] = '\n'; hexs[5] = 0; vga_print(hexs);
    command_finish(d, c, E_TIMEDOUT, NULL, 0); cmd_kick(d);
}
int bt_cmd_sync(struct bt_dev* d, unsigned opcode, const void* params, unsigned plen, uint8_t* rp, unsigned rmax) {
    struct bt_cmd* c = command_new(d, opcode, params, plen); if (!c) return d && d->alive ? E_NOMEM : E_NODEV;
    c->sync = true; c->worker = worker_running; int index = d->index; uint32_t generation = d->generation;
    command_queue(d, c);
    while (!c->completed) {
        int error = bt_wait(c, bt_now() + 50000000ULL);
        if (c->completed) break;
        struct bt_dev* live = bt_dev_get(index, generation);
        if (live) cmd_timeouts(live, bt_now());
        if (c->completed) break;
        if (error == E_INTR || error == E_NOSYS) { c->abandoned = true; return error == E_NOSYS ? E_NOSYS : E_INTR; }
    }
    int status = c->status;
    if (rp && rmax) { memset(rp, 0, rmax); memcpy(rp, c->rp, c->rlen < rmax ? c->rlen : rmax); }
    bt_free(c, sizeof(*c)); return status;
}

/* ---------- connections ---------- */
struct bt_conn* bt_conn_get(uint32_t id) { struct bt_conn* c = id ? conn_table[id % 64] : NULL; return c && c->id == id ? c : NULL; }
struct bt_conn* bt_conn_lookup(struct bt_dev* d, const uint8_t* addr, uint8_t link) {
    for (struct bt_conn* c = d ? d->conns : NULL; c; c = c->next) if (c->link == link && c->state != CONN_CLOSING && !memcmp(c->peer, addr, 6)) return c;
    return NULL;
}
struct bt_conn* bt_conn_by_handle(struct bt_dev* d, unsigned handle) {
    for (struct bt_conn* c = d ? d->conns : NULL; c; c = c->next) if (c->handle_valid && c->handle == handle) return c;
    return NULL;
}
struct bt_conn* bt_conn_new(struct bt_dev* d, const uint8_t* addr, uint8_t peer_type, uint8_t link, bool outgoing) {
    unsigned slot = 0; for (; slot < 64; slot++) if (!conn_table[(conn_ids + 1 + slot) % 64]) break;
    if (slot == 64) return NULL;
    struct bt_conn* c = bt_zalloc(sizeof(*c)); if (!c) return NULL;
    conn_ids += 1 + slot; c->id = conn_ids; if (!c->id) c->id = conn_ids = 64; conn_table[c->id % 64] = c;
    c->dev = d; memcpy(c->peer, addr, 6); c->peer_type = peer_type; c->link = link; c->outgoing = outgoing; c->state = CONN_CONNECTING;
    c->sec_level = SEC_LOW; c->pending_sec = SEC_LOW; c->io_cap = d->io_cap; c->remote_io_cap = 0xFF; c->ident = 0; c->disc_reason = 0x13;
    c->role = outgoing ? 0 : 1; c->auth_type = 0xFF;
    c->next = d->conns; d->conns = c; return c;
}
static void conn_free(struct bt_conn* c) {
    struct bt_dev* d = c->dev;
    for (struct bt_conn** p = &d->conns; *p; p = &(*p)->next) if (*p == c) { *p = c->next; break; }
    if (d->acl_cnt + c->sent <= d->acl_max && c->link == LINK_ACL) d->acl_cnt = (uint16_t)(d->acl_cnt + c->sent);
    else if (c->link == LINK_LE) { if (d->le_max) { if (d->le_cnt + c->sent <= d->le_max) d->le_cnt = (uint16_t)(d->le_cnt + c->sent); } else if (d->acl_cnt + c->sent <= d->acl_max) d->acl_cnt = (uint16_t)(d->acl_cnt + c->sent); }
    bt_queue_purge(&c->txq); if (c->rx) bt_free(c->rx, c->rx_alloc);
    conn_table[c->id % 64] = NULL; c->id = 0; bt_wake(&c->txq); bt_free(c, sizeof(*c));
}
/* The link ended (disconnection, failed connection, controller gone): users first, then the object. */
static void conn_closed(struct bt_conn* c, int error, uint8_t hci_reason) {
    c->state = CONN_CLOSING; c->handle_valid = false;
    l2cap_conn_down(c, error); smp_conn_closed(c); mgmt_conn_closed(c, hci_reason);
    conn_free(c);
}
const char* bt_conn_text(const struct bt_conn* c) {
    static char text[24]; const char* h = "0123456789ABCDEF"; unsigned n = 0;
    for (int i = 5; i >= 0; i--) { text[n++] = h[c->peer[i] >> 4]; text[n++] = h[c->peer[i] & 15]; if (i) text[n++] = ':'; }
    text[n] = 0; return text;
}
static void conn_log(const struct bt_conn* c, const char* what) {
    char text[96] = "[64] BLUETOOTH hci"; unsigned n = 18; text[n++] = (char)('0' + c->dev->index); text[n] = 0;
    strcat(text, c->link == LINK_LE ? " LE " : " ACL "); strcat(text, bt_conn_text(c)); strcat(text, " "); strcat(text, what); strcat(text, "\n"); vga_print(text);
}
static void connect_status(struct bt_dev* d, struct bt_cmd* cmd, int status, const uint8_t* rp, unsigned rlen) {
    (void)d; (void)rp; (void)rlen; struct bt_conn* c = bt_conn_get((uint32_t)cmd->arg);
    if (c && status && c->state == CONN_CONNECTING) { conn_log(c, "connection attempt refused by the controller"); conn_closed(c, bt_hci_errno(status), (uint8_t)(status > 0 ? status : 0x1F)); }
}
struct bt_conn* bt_connect(struct bt_dev* d, const uint8_t* addr, uint8_t peer_type, int* error) {
    uint8_t link = peer_type ? LINK_LE : LINK_ACL; *error = 0;
    struct bt_conn* c = bt_conn_lookup(d, addr, link); if (c) return c;
    if (!(d->flags & HCI_UP)) { *error = E_HOSTDOWN; return NULL; }
    if (link == LINK_LE) for (struct bt_conn* o = d->conns; o; o = o->next) if (o->link == LINK_LE && o->state == CONN_CONNECTING && o->outgoing) { *error = E_BUSY; return NULL; }
    c = bt_conn_new(d, addr, peer_type, link, true); if (!c) { *error = E_NOMEM; return NULL; }
    bool sent;
    if (link == LINK_ACL) {
        uint8_t p[13]; memcpy(p, addr, 6); bt_put16(p + 6, 0xCC18); p[8] = 0x02; p[9] = 0; bt_put16(p + 10, 0); p[12] = 0x01;
        for (unsigned i = 0; i < d->found_count; i++) if (!memcmp(d->found[i].addr, addr, 6) && !d->found[i].type) { p[8] = d->found[i].page_scan_mode; bt_put16(p + 10, d->found[i].clock_offset | 0x8000); }
        sent = bt_cmd_async(d, 0x0405, p, 13, connect_status, NULL, c->id);
    } else {
        if (d->le_scan_active) { uint8_t off[2] = {0, 0}; bt_cmd_async(d, 0x200C, off, 2, NULL, NULL, 0); d->le_scan_active = false; }
        uint8_t p[25]; bt_put16(p, 0x0060); bt_put16(p + 2, 0x0060); p[4] = 0; p[5] = peer_type == 2 ? 1 : 0; memcpy(p + 6, addr, 6); p[12] = 0;
        bt_put16(p + 13, 0x0018); bt_put16(p + 15, 0x0028); bt_put16(p + 17, 0); bt_put16(p + 19, 0x002A); bt_put16(p + 21, 0); bt_put16(p + 23, 0);
        sent = bt_cmd_async(d, 0x200D, p, 25, connect_status, NULL, c->id);
        c->idle_deadline = bt_now() + 20000000000ULL;   /* LE connection attempts are cancelled by the host after 20 s */
    }
    if (!sent) { conn_free(c); *error = E_NOMEM; return NULL; }
    conn_log(c, "connecting");
    return c;
}
void bt_disconnect(struct bt_conn* c, uint8_t reason) {
    struct bt_dev* d = c->dev;
    if (c->state == CONN_CLOSING) return;
    if (c->state == CONN_CONNECTING) {
        if (c->outgoing) { if (c->link == LINK_LE) bt_cmd_async(d, 0x200E, NULL, 0, NULL, NULL, 0); else bt_cmd_async(d, 0x0408, c->peer, 6, NULL, NULL, 0); }
        return;
    }
    uint8_t p[3]; bt_put16(p, c->handle); p[2] = reason; c->state = CONN_CLOSING; c->disc_reason = 0x16;   /* local host */
    bt_cmd_async(d, 0x0406, p, 3, NULL, NULL, 0);
}
static bool conn_in_use(struct bt_conn* c) { return c->pair_op || c->smp || c->auth_pending || c->enc_pending || c->waiters || l2cap_conn_in_use(c); }
void bt_conn_idle(struct bt_conn* c) {
    if (c->state == CONN_CONNECTED && !c->idle_deadline) c->idle_deadline = bt_now() + (c->outgoing ? 2000000000ULL : 4000000000ULL);
    else if (c->state == CONN_CONNECTING && c->outgoing && !conn_in_use(c)) bt_disconnect(c, 0x13);   /* last user gone: cancel the attempt (Linux) */
}
void bt_conn_busy(struct bt_conn* c) { if (c->state == CONN_CONNECTED) c->idle_deadline = 0; }

/* ---------- ACL ---------- */
static void acl_kick(struct bt_dev* d) {
    for (bool progress = true; progress && d->alive && !d->user;) {
        progress = false;
        for (struct bt_conn* c = d->conns; c; c = c->next) {
            if (!c->txq.count || !c->handle_valid) continue;
            uint16_t* credits = c->link == LINK_LE && d->le_max ? &d->le_cnt : &d->acl_cnt;
            if (!*credits || (d->driver->can_send && !d->driver->can_send(d, PKT_ACL))) continue;
            struct bt_pkt* p = bt_queue_take(&c->txq); (*credits)--; c->sent++;
            to_controller(d, p->data, p->len, NULL); bt_pkt_free(p); progress = true;
        }
    }
}
int bt_acl_send2(struct bt_conn* c, uint16_t cid, const uint8_t* head, unsigned hlen, const uint8_t* data, unsigned length) {
    struct bt_dev* d = c->dev;
    if (c->state != CONN_CONNECTED || !c->handle_valid) return E_NOTCONN;
    unsigned total = 4 + hlen + length; if (total > BT_ACL_MAX) return E_MSGSIZE;
    if (c->txq.count > 512) return E_AGAIN;
    unsigned mtu = c->link == LINK_LE && d->le_max ? d->le_mtu : d->acl_mtu; if (mtu < 23) mtu = 23;
    unsigned at = 0; struct bt_queue fragments = {0};
    while (at < total) {
        unsigned n = total - at < mtu ? total - at : mtu;
        struct bt_pkt* p = bt_pkt_new(5 + n); if (!p) { bt_queue_purge(&fragments); return E_NOMEM; }
        unsigned pb = at ? 0x1000 : (c->link == LINK_LE ? 0x0000 : 0x2000);
        p->data[0] = PKT_ACL; bt_put16(p->data + 1, c->handle | pb); bt_put16(p->data + 3, n);
        for (unsigned i = 0; i < n; i++) {   /* the frame is header(4) + head + data, copied piecewise */
            unsigned k = at + i; uint8_t byte;
            if (k < 4) { uint8_t h[4]; bt_put16(h, hlen + length); bt_put16(h + 2, cid); byte = h[k]; }
            else if (k < 4 + hlen) byte = head[k - 4]; else byte = data[k - 4 - hlen];
            p->data[5 + i] = byte;
        }
        bt_queue_put(&fragments, p); at += n;
    }
    while (fragments.count) bt_queue_put(&c->txq, bt_queue_take(&fragments));
    acl_kick(d); return 0;
}
int bt_acl_send(struct bt_conn* c, uint16_t cid, const uint8_t* data, unsigned length) { return bt_acl_send2(c, cid, NULL, 0, data, length); }
static void acl_receive(struct bt_dev* d, const uint8_t* p, unsigned length) {
    if (length < 4) return;
    unsigned handle = bt_get16(p) & 0x0FFF, pb = (bt_get16(p) >> 12) & 3, n = bt_get16(p + 2);
    if (n != length - 4) { d->stat[0]++; return; }
    struct bt_conn* c = bt_conn_by_handle(d, handle);
    if (!c || c->state != CONN_CONNECTED) return;
    const uint8_t* data = p + 4;
    if (pb == 1) {                                                         /* continuation */
        if (!c->rx) return;
        if (c->rx_len + n > c->rx_want) { bt_free(c->rx, c->rx_alloc); c->rx = NULL; d->stat[0]++; return; }
        memcpy(c->rx + c->rx_len, data, n); c->rx_len += n;
    } else {                                                               /* start of a frame (0b00 / 0b10) */
        if (c->rx) { bt_free(c->rx, c->rx_alloc); c->rx = NULL; d->stat[0]++; }
        if (n < 2) { d->stat[0]++; return; }
        unsigned want = bt_get16(data) + 4U;
        if (n == want) { l2cap_receive(c, data, n); return; }
        if (n > want || want > BT_ACL_MAX) { d->stat[0]++; return; }
        c->rx = bt_zalloc(want); if (!c->rx) return;
        c->rx_alloc = want; c->rx_want = want; memcpy(c->rx, data, n); c->rx_len = n;
    }
    if (c->rx && c->rx_len == c->rx_want) {
        uint8_t* frame = c->rx; unsigned size = c->rx_len, alloc = c->rx_alloc; c->rx = NULL;
        uint32_t id = c->id; l2cap_receive(c, frame, size); bt_free(frame, alloc); (void)id;
    }
}

/* ---------- security ---------- */
static uint8_t key_level(uint8_t key_type, uint8_t pin_len) {
    switch (key_type) { case 0x05: return SEC_HIGH; case 0x08: return SEC_FIPS; case 0x00: return pin_len == 16 ? SEC_HIGH : SEC_MEDIUM; default: return SEC_MEDIUM; }
}
bool bt_conn_security(struct bt_conn* c, uint8_t level, bool initiator) {
    if (level <= SEC_LOW) return true;
    if (c->state != CONN_CONNECTED) return false;
    if (c->encrypted && c->sec_level >= level) return true;
    if (c->pending_sec < level) c->pending_sec = level;
    if (c->link == LINK_LE) {
        if (c->smp || c->enc_pending) return false;
        if (c->role == 0) {
            struct bt_key* k = mgmt_find_ltk(c->dev, c->peer, c->peer_type, true);
            if (k && (k->authenticated ? SEC_HIGH : SEC_MEDIUM) >= level && bt_le_encrypt(c, k->val, k->ediv, k->rand, k->enc_size)) return false;
        }
        if (smp_start(c, level) < 0) bt_conn_security_changed(c, E_ACCES);
        return false;
    }
    if (c->auth_pending || c->enc_pending) return false;
    if (c->authenticated && c->sec_level >= level && !c->encrypted) {
        uint8_t p[3]; bt_put16(p, c->handle); p[2] = 1; c->enc_pending = bt_cmd_async(c->dev, 0x0413, p, 3, NULL, NULL, 0); return false;
    }
    (void)initiator;
    if (c->auth_type == 0xFF) c->auth_type = level >= SEC_HIGH ? 0x05 : 0x04;   /* general bonding (MITM for HIGH) */
    uint8_t p[2]; bt_put16(p, c->handle); c->auth_pending = bt_cmd_async(c->dev, 0x0411, p, 2, NULL, NULL, 0);
    return false;
}
void bt_conn_security_changed(struct bt_conn* c, int status) {
    uint32_t id = c->id;
    l2cap_security_done(c, status);
    if ((c = bt_conn_get(id)) && c->pair_op && c->link == LINK_ACL && c->authenticated) mgmt_pair_done(c, status ? 0x05 : 0);
    if ((c = bt_conn_get(id)) && !conn_in_use(c)) bt_conn_idle(c);
}
static void security_event(struct bt_conn* c, int status, bool encryption, bool enabled) {
    if (encryption) {
        c->enc_pending = false; c->encrypted = !status && enabled;
        if (c->link == LINK_LE) { smp_encrypt_changed(c, status); return; }
        if (c->encrypted && c->authenticated) c->sec_level = c->pending_sec > c->sec_level ? (c->pending_sec <= key_level(c->key_type, 0) ? c->pending_sec : key_level(c->key_type, 0)) : c->sec_level;
        if (c->encrypted && c->sec_level < SEC_MEDIUM && c->authenticated) c->sec_level = key_level(c->key_type, 0);
        if (c->encrypted) { uint8_t p[2]; bt_put16(p, c->handle); bt_cmd_async(c->dev, 0x1408, p, 2, NULL, NULL, 0); }   /* Read Encryption Key Size */
        bt_conn_security_changed(c, status ? bt_hci_errno(status) : 0); return;
    }
    c->auth_pending = false;
    if (status) { c->pending_sec = c->sec_level; bt_conn_security_changed(c, bt_hci_errno(status)); return; }
    c->authenticated = true;
    if (!c->encrypted) { uint8_t p[3]; bt_put16(p, c->handle); p[2] = 1; c->enc_pending = bt_cmd_async(c->dev, 0x0413, p, 3, NULL, NULL, 0); if (c->enc_pending) return; }
    c->sec_level = key_level(c->key_type, 0); bt_conn_security_changed(c, 0);
}

/* ---------- events ---------- */
static void event(struct bt_dev* d, const uint8_t* e, unsigned length) {
    if (length < 2 || e[1] != length - 2) { d->stat[0]++; return; }
    const uint8_t* p = e + 2; unsigned n = e[1];
    switch (e[0]) {
    case 0x0E:                                                             /* Command Complete */
        if (n < 3) break;
        d->cmd_cnt = p[0]; if (d->cmd_cnt > 8) d->cmd_cnt = 8;
        if (d->cmd_sent && d->cmd_sent->opcode == bt_get16(p + 1) && bt_get16(p + 1)) {
            struct bt_cmd* c = d->cmd_sent; d->cmd_sent = NULL; command_finish(d, c, n > 3 ? p[3] : 0, p + 3, n - 3);
        }
        cmd_kick(d); return;
    case 0x0F:                                                             /* Command Status */
        if (n < 4) break;
        d->cmd_cnt = p[1]; if (d->cmd_cnt > 8) d->cmd_cnt = 8;
        if (d->cmd_sent && d->cmd_sent->opcode == bt_get16(p + 2) && bt_get16(p + 2)) {
            struct bt_cmd* c = d->cmd_sent; d->cmd_sent = NULL; command_finish(d, c, p[0], p, 1);
        }
        cmd_kick(d);
        if (!p[0]) return;
        break;                                                             /* a failed status may matter to mgmt */
    case 0x13: {                                                           /* Number Of Completed Packets */
        if (n < 1 || n < 1U + 4U * p[0]) break;
        for (unsigned i = 0; i < p[0]; i++) {
            struct bt_conn* c = bt_conn_by_handle(d, bt_get16(p + 1 + 4 * i) & 0x0FFF); unsigned count = bt_get16(p + 3 + 4 * i);
            if (!c) continue;
            if (count > c->sent) count = c->sent;
            c->sent -= count;
            uint16_t* credits = c->link == LINK_LE && d->le_max ? &d->le_cnt : &d->acl_cnt; uint16_t max = c->link == LINK_LE && d->le_max ? d->le_max : d->acl_max;
            *credits = (uint16_t)(*credits + count > max ? max : *credits + count);
            bt_wake(&c->txq);
        }
        acl_kick(d); return;
    }
    case 0x03: {                                                           /* Connection Complete */
        if (n < 11) break;
        struct bt_conn* c = NULL;
        for (struct bt_conn* o = d->conns; o; o = o->next) if (o->link == LINK_ACL && o->state == CONN_CONNECTING && !memcmp(o->peer, p + 3, 6)) c = o;
        if (p[9] != 1) {                                                   /* SCO/eSCO: not used by this stack */
            if (!p[0]) { uint8_t q[3]; bt_put16(q, bt_get16(p + 1)); q[2] = 0x13; bt_cmd_async(d, 0x0406, q, 3, NULL, NULL, 0); }
            return;
        }
        if (!c) { if (p[0]) break; c = bt_conn_new(d, p + 3, 0, LINK_ACL, false); if (!c) { uint8_t q[3]; bt_put16(q, bt_get16(p + 1)); q[2] = 0x14; bt_cmd_async(d, 0x0406, q, 3, NULL, NULL, 0); return; } }
        if (p[0]) { conn_log(c, "connection failed"); conn_closed(c, bt_hci_errno(p[0]), p[0]); return; }
        c->handle = (uint16_t)(bt_get16(p + 1) & 0x0FFF); c->handle_valid = true; c->state = CONN_CONNECTED; c->encrypted = p[10] != 0;
        conn_log(c, "connected");
        { uint8_t q[2]; bt_put16(q, c->handle); bt_cmd_async(d, 0x041B, q, 2, NULL, NULL, 0); }   /* Read Remote Supported Features */
        uint32_t id = c->id; mgmt_conn_complete(c, 0);
        if ((c = bt_conn_get(id))) l2cap_conn_ready(c);
        if ((c = bt_conn_get(id)) && !conn_in_use(c)) bt_conn_idle(c);
        return;
    }
    case 0x04: {                                                           /* Connection Request */
        if (n < 10) break;
        bool acl = p[9] == 1, allowed = false;
        for (unsigned i = 0; i < d->accept_count; i++) if (!memcmp(d->accept_list[i], p, 6) && !d->accept_list[i][6]) allowed = true;
        for (unsigned i = 0; i < d->blocked_count; i++) if (!memcmp(d->blocked[i], p, 6) && !d->blocked[i][6]) { acl = false; allowed = false; }
        if (acl && ((d->settings & ST_CONNECTABLE) || allowed) && !bt_conn_lookup(d, p, LINK_ACL)) {
            struct bt_conn* c = bt_conn_new(d, p, 0, LINK_ACL, false);
            if (c) { memcpy(c->dev_class, p + 6, 3); uint8_t q[7]; memcpy(q, p, 6); q[6] = 0x01; bt_cmd_async(d, 0x0409, q, 7, NULL, NULL, 0); return; }
        }
        uint8_t q[7]; memcpy(q, p, 6); q[6] = acl ? 0x0F : 0x0D; bt_cmd_async(d, 0x040A, q, 7, NULL, NULL, 0);
        return;
    }
    case 0x05: {                                                           /* Disconnection Complete */
        if (n < 4 || p[0]) break;
        struct bt_conn* c = bt_conn_by_handle(d, bt_get16(p + 1) & 0x0FFF); if (!c) break;
        conn_log(c, "disconnected");
        conn_closed(c, p[3] == 0x08 ? E_TIMEDOUT : p[3] == 0x16 ? E_CONNABORTED : E_CONNRESET, c->state == CONN_CLOSING && c->disc_reason == 0x16 ? 0x16 : p[3]);
        return;
    }
    case 0x06: {                                                           /* Authentication Complete */
        if (n < 3) break;
        struct bt_conn* c = bt_conn_by_handle(d, bt_get16(p + 1) & 0x0FFF); if (!c) break;
        uint32_t id = c->id; mgmt_hci_event(d, e, length);
        if ((c = bt_conn_get(id))) security_event(c, p[0], false, false);
        return;
    }
    case 0x08: case 0x59: {                                                /* Encryption Change v1/v2 */
        if (n < 4) break;
        struct bt_conn* c = bt_conn_by_handle(d, bt_get16(p + 1) & 0x0FFF); if (!c) break;
        if (e[0] == 0x59 && n >= 5) c->enc_key_size = p[4];
        security_event(c, p[0], true, p[3] != 0); return;
    }
    case 0x30: {                                                           /* Encryption Key Refresh Complete */
        if (n < 3) break;
        struct bt_conn* c = bt_conn_by_handle(d, bt_get16(p + 1) & 0x0FFF); if (!c) break;
        if (c->link == LINK_LE) security_event(c, p[0], true, !p[0] || c->encrypted);
        return;
    }
    case 0x0B: {                                                           /* Read Remote Supported Features Complete */
        if (n < 11) break;
        struct bt_conn* c = bt_conn_by_handle(d, bt_get16(p + 1) & 0x0FFF); if (c && !p[0]) memcpy(c->remote_features, p + 3, 8);
        return;
    }
    case 0x3E: {                                                           /* LE Meta */
        if (n < 1) break;
        unsigned sub = p[0];
        if ((sub == 0x01 && n >= 19) || (sub == 0x0A && n >= 31)) {        /* LE (Enhanced) Connection Complete */
            const uint8_t* q = p + 1; unsigned status = q[0], role = q[3], peer_type = q[4] & 1; const uint8_t* peer = q + 5;
            struct bt_conn* c = NULL;
            for (struct bt_conn* o = d->conns; o; o = o->next) if (o->link == LINK_LE && o->state == CONN_CONNECTING && o->outgoing && (status || !memcmp(o->peer, peer, 6))) c = o;
            if (status) { if (c) { conn_log(c, "LE connection failed"); conn_closed(c, status == 0x02 ? E_TIMEDOUT : bt_hci_errno((int)status), (uint8_t)status); } return; }
            if (!c) { c = bt_conn_new(d, peer, (uint8_t)(peer_type + 1), LINK_LE, false); if (!c) return; }
            memcpy(c->peer, peer, 6); c->peer_type = (uint8_t)(peer_type + 1);
            c->handle = (uint16_t)(bt_get16(q + 1) & 0x0FFF); c->handle_valid = true; c->state = CONN_CONNECTED; c->role = (uint8_t)role; c->idle_deadline = 0;
            if (role == 1) d->le_adv_on = false;                           /* legacy advertising stops on a connection */
            conn_log(c, role ? "connected (peripheral)" : "connected (central)");
            uint32_t id = c->id; mgmt_conn_complete(c, 0);
            if ((c = bt_conn_get(id))) l2cap_conn_ready(c);
            if ((c = bt_conn_get(id)) && !conn_in_use(c)) bt_conn_idle(c);
            return;
        }
        if (sub == 0x05 && n >= 13) {                                      /* LE Long Term Key Request */
            struct bt_conn* c = bt_conn_by_handle(d, bt_get16(p + 1) & 0x0FFF);
            if (c) smp_ltk_request(c, (uint16_t)bt_get16(p + 11), p + 3);
            else { uint8_t q[2]; bt_put16(q, bt_get16(p + 1)); bt_cmd_async(d, 0x201B, q, 2, NULL, NULL, 0); }
            return;
        }
        if (sub == 0x06 && n >= 11) {                                      /* LE Remote Connection Parameter Request: accept */
            uint8_t q[14]; memcpy(q, p + 1, 10); bt_put16(q + 10, 0); bt_put16(q + 12, 0); bt_cmd_async(d, 0x2020, q, 14, NULL, NULL, 0); return;
        }
        break;
    }
    case 0x10: { char text[48] = "[64] BLUETOOTH hci"; text[18] = (char)('0' + d->index); text[19] = 0; strcat(text, " hardware error\n"); vga_print(text); break; }
    }
    mgmt_hci_event(d, e, length);
}
void bt_dev_receive(struct bt_dev* d, const uint8_t* packet, unsigned length) {
    if (!d || !d->alive || !length) return;
    unsigned type = packet[0];
    if (type == PKT_EVENT) d->stat[3]++; else if (type == PKT_ACL) d->stat[5]++; else if (type == PKT_SCO) d->stat[7]++;
    d->stat[8] += length - 1;
    distribute(d, packet, length, false, NULL);
    if (d->user || !d->alive) return;                                      /* the user channel owns everything */
    if (type == PKT_EVENT) event(d, packet + 1, length - 1);
    else if (type == PKT_ACL && (d->flags & (HCI_UP | HCI_INIT))) acl_receive(d, packet + 1, length - 1);
}
void bt_send_raw(struct bt_dev* d, const uint8_t* packet, unsigned length, const void* from) {
    if (packet[0] == PKT_COMMAND && !d->user) {                            /* queued like the stack's own commands */
        struct bt_cmd* c = command_new(d, bt_get16(packet + 1), packet + 4, packet[3]); if (!c) { d->stat[1]++; return; }
        command_queue(d, c); return;
    }
    to_controller(d, packet, length, from);
}

/* ---------- lifecycle ---------- */
/* Setup at registration (as Linux's HCI_SETUP stage): the controller's identity is read asynchronously, then it stays
 * down and mgmt learns of it (Index Added), so READ_INFO has the real address before anyone powers it on. */
static void setup_step(struct bt_dev* d, struct bt_cmd* c, int status, const uint8_t* rp, unsigned rlen) {
    if (!d->alive) return;
    if (!status) switch (c->opcode) {
    case 0x1003: if (rlen >= 9) memcpy(d->features[0], rp + 1, 8); break;
    case 0x1001: if (rlen >= 9) { d->hci_ver = rp[1]; d->hci_rev = (uint16_t)bt_get16(rp + 2); d->lmp_ver = rp[4]; d->manufacturer = (uint16_t)bt_get16(rp + 5); d->lmp_subver = (uint16_t)bt_get16(rp + 7); } break;
    case 0x1009: if (rlen >= 7) memcpy(d->bdaddr, rp + 1, 6); break;
    }
    if (c->arg != 1) return;                                               /* the last step */
    d->setup_done = true;
    bool bredr = !(d->features[0][4] & 0x20), le = (d->features[0][4] & 0x40) != 0;
    d->supported = ST_POWERED | ST_BONDABLE | ST_DEBUG_KEYS | ST_CONNECTABLE | ST_FAST_CONN | ST_DISCOVERABLE |
                   (bredr ? ST_BREDR | ST_LINK_SEC : 0) | (bredr && (d->features[0][6] & 0x08) ? ST_SSP : 0) | (le ? ST_LE | ST_ADVERTISING | ST_SC : 0);
    if (!bredr) d->settings &= ~(ST_BREDR | ST_SSP);
    if (!bredr && le) d->settings |= ST_LE;                                 /* LE-only: LE is always on */
    if (!le) d->settings &= ~(ST_LE | ST_ADVERTISING);
    if (!(d->supported & ST_SSP)) d->settings &= ~ST_SSP;
    uint8_t info[8]; memcpy(info, d->bdaddr, 6); bt_put16(info + 6, d->manufacturer); monitor(10, d->index, info, 8);   /* INDEX_INFO */
    if (!d->user) mgmt_index_added(d);
}
static void setup_start(struct bt_dev* d) {
    bt_cmd_async(d, 0x0C03, NULL, 0, setup_step, NULL, 0); bt_cmd_async(d, 0x1003, NULL, 0, setup_step, NULL, 0);
    bt_cmd_async(d, 0x1001, NULL, 0, setup_step, NULL, 0); bt_cmd_async(d, 0x1009, NULL, 0, setup_step, NULL, 1);
}
struct bt_dev* bt_dev_register(const struct bt_driver* driver, void* data, const char* why) {
    for (unsigned i = 0; i < BT_MAX_DEVICES; i++) if (!devs[i]) {
        struct bt_dev* d = bt_zalloc(sizeof(*d)); if (!d) return NULL;
        d->index = (int)i; d->generation = ++generations; d->alive = true; d->driver = driver; d->driver_data = data; d->cmd_cnt = 1;
        d->acl_mtu = 1021; d->acl_max = 8; d->acl_cnt = 8; d->sco_mtu = 64; d->sco_max = 8; d->io_cap = 0x03;
        strcpy(d->name, "NexusOS"); d->settings = ST_BREDR;   /* Linux's defaults: user space (bluetoothd) enables the rest */
        devs[i] = d;
        monitor_new_index(d, NULL); setup_start(d);
        char text[64] = "[64] BLUETOOTH hci"; text[18] = (char)('0' + i); text[19] = 0; strcat(text, " registered ("); strcat(text, why); strcat(text, ")\n"); vga_print(text);
        return d;
    }
    return NULL;
}
static void fail_commands(struct bt_dev* d) {
    struct bt_cmd* c = d->cmd_sent; d->cmd_sent = NULL;
    if (c) command_finish(d, c, E_NODEV, NULL, 0);
    while ((c = d->cmd_head)) { d->cmd_head = c->next; d->cmd_queued--; command_finish(d, c, E_NODEV, NULL, 0); }
    d->cmd_tail = NULL;
}
static void drop_connections(struct bt_dev* d, int error) { while (d->conns) conn_closed(d->conns, error, 0x16); }
void bt_dev_unregister(struct bt_dev* d) {
    if (!d || !d->alive) return;
    drop_connections(d, E_HOSTDOWN);
    if (d->flags & HCI_UP) mgmt_powered(d, false);
    d->alive = false; d->flags = 0;
    fail_commands(d);
    monitor(1, d->index, NULL, 0);                                         /* DEL_INDEX */
    if (!d->user && d->setup_done) mgmt_index_removed(d);
    for (unsigned i = 0; i < SOCKETS; i++) if (sockets[i] && sockets[i]->dev == d->index && sockets[i]->channel != CH_MONITOR) bt_wake(sockets[i]);
    mgmt_dev_freed(d);
    char text[48] = "[64] BLUETOOTH hci"; text[18] = (char)('0' + d->index); text[19] = 0; strcat(text, " removed\n"); vga_print(text);
    devs[d->index] = NULL; d->generation = 0; bt_free(d, sizeof(*d));
}
static const uint8_t EVENT_MASK[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0xF8, 0xBF, 0x3D};
int bt_dev_open(struct bt_dev* d) {
    if (d->flags & HCI_UP) return E_ALREADY;
    if (d->user) return E_BUSY;
    if (d->flags & HCI_INIT) return E_BUSY;
    int index = d->index; uint32_t generation = d->generation;
    d->flags |= HCI_INIT; d->cmd_cnt = 1;
    uint8_t rp[64]; int status;
#define LIVE() do { d = bt_dev_get(index, generation); if (!d) return E_NODEV; } while (0)
#define STEP(op, params, plen) (status = bt_cmd_sync(d, op, params, plen, rp, sizeof(rp)), d = bt_dev_get(index, generation), !d ? E_NODEV : status)
#define REQUIRED(op, params, plen) do { int r_ = STEP(op, params, plen); if (r_) { if (d) d->flags &= ~HCI_INIT; return r_ < 0 ? r_ : E_IO; } } while (0)
    REQUIRED(0x0C03, NULL, 0);                                             /* Reset */
    if (d->driver->setup) { int r = d->driver->setup(d); LIVE(); if (r) { d->flags &= ~HCI_INIT; return r; } }
    REQUIRED(0x1003, NULL, 0); memcpy(d->features[0], rp + 1, 8);         /* Read Local Supported Features */
    REQUIRED(0x1001, NULL, 0);                                             /* Read Local Version Information */
    d->hci_ver = rp[1]; d->hci_rev = (uint16_t)bt_get16(rp + 2); d->lmp_ver = rp[4]; d->manufacturer = (uint16_t)bt_get16(rp + 5); d->lmp_subver = (uint16_t)bt_get16(rp + 7);
    REQUIRED(0x1009, NULL, 0); memcpy(d->bdaddr, rp + 1, 6);              /* Read BD_ADDR */
    if (!STEP(0x1002, NULL, 0)) memcpy(d->commands, rp + 1, 63 < sizeof(rp) - 1 ? 63 : sizeof(rp) - 1);   /* Supported Commands (first 63) */
    if (!d) return E_NODEV;
    bool bredr = !(d->features[0][4] & 0x20), le = (d->features[0][4] & 0x40) != 0;
    d->supported = ST_POWERED | ST_BONDABLE | ST_DEBUG_KEYS | ST_CONNECTABLE | ST_FAST_CONN | ST_DISCOVERABLE;
    if (bredr) {
        d->supported |= ST_BREDR | ST_LINK_SEC;
        if (d->features[0][6] & 0x08) d->supported |= ST_SSP;
        if (!STEP(0x1005, NULL, 0)) {                                      /* Read Buffer Size */
            d->acl_mtu = (uint16_t)bt_get16(rp + 1); d->sco_mtu = rp[3]; d->acl_max = (uint16_t)bt_get16(rp + 4); d->sco_max = (uint16_t)bt_get16(rp + 6);
            if (!d->acl_max) d->acl_max = 1;
            d->acl_cnt = d->acl_max;
        }
        if (!d) return E_NODEV;
        if (d->features[0][7] & 0x80) {                                    /* Read Local Extended Features page 1, 2 */
            uint8_t page = 1;
            if (!STEP(0x1004, &page, 1)) { d->max_page = rp[2]; memcpy(d->features[1], rp + 3, 8); }
            if (!d) return E_NODEV;
            page = 2; if (d->max_page >= 2 && !STEP(0x1004, &page, 1)) memcpy(d->features[2], rp + 3, 8);
            if (!d) return E_NODEV;
        }
        if (d->features[2][1] & 0x01) d->supported |= ST_SC;               /* Secure Connections (Controller Support) */
        uint8_t clear = 0; STEP(0x0C05, &clear, 1); if (!d) return E_NODEV;   /* Set Event Filter: clear */
        uint8_t timeout[2]; bt_put16(timeout, 0x7D00); STEP(0x0C16, timeout, 2); if (!d) return E_NODEV;
        uint8_t mode = (d->features[0][6] & 0x01) ? 2 : (d->features[0][3] & 0x40) ? 1 : 0;   /* EIR / RSSI inquiry results */
        if (mode) { STEP(0x0C45, &mode, 1); if (!d) return E_NODEV; }
    }
    if (le) {
        d->supported |= ST_LE | ST_ADVERTISING | ST_SC;
        if (!STEP(0x2002, NULL, 0)) { d->le_mtu = (uint16_t)bt_get16(rp + 1); d->le_max = rp[3]; d->le_cnt = d->le_max; if (!d->le_mtu) d->le_max = d->le_cnt = 0; }
        if (!d) return E_NODEV;
        if (!STEP(0x2003, NULL, 0)) memcpy(d->le_features, rp + 1, 8);
        if (!d) return E_NODEV;
        if (!STEP(0x201C, NULL, 0)) memcpy(d->le_states, rp + 1, 8);
        if (!d) return E_NODEV;
        uint8_t le_mask[8] = {0x7F, 0, 0, 0, 0, 0, 0, 0}; STEP(0x2001, le_mask, 8); if (!d) return E_NODEV;
        STEP(0x2010, NULL, 0); if (!d) return E_NODEV;                     /* LE Clear Accept List */
        if (bredr) { uint8_t host[2] = {(d->settings & ST_LE) ? 1 : 0, 0}; STEP(0x0C6D, host, 2); if (!d) return E_NODEV; }
    }
    if (!bredr) d->settings &= ~(ST_BREDR | ST_SSP);
    if (!le) d->settings &= ~ST_LE;
    if (!(d->supported & ST_SSP)) d->settings &= ~ST_SSP;
    if (!(d->supported & ST_SC)) d->settings &= ~ST_SC;
    REQUIRED(0x0C01, EVENT_MASK, 8);                                       /* Set Event Mask */
    if (bredr) { uint8_t policy[2]; bt_put16(policy, 0x0005); STEP(0x080F, policy, 2); if (!d) return E_NODEV; }   /* role switch + sniff */
    if (bredr && (d->settings & ST_SSP)) { uint8_t on = 1; STEP(0x0C56, &on, 1); if (!d) return E_NODEV; }
    if (bredr && (d->settings & ST_SC) && (d->settings & ST_SSP)) { uint8_t on = 1; STEP(0x0C7A, &on, 1); if (!d) return E_NODEV; }
#undef LIVE
#undef STEP
#undef REQUIRED
    d->flags = (d->flags & ~HCI_INIT) | HCI_UP | HCI_RUNNING;
    monitor(8, d->index, NULL, 0);                                         /* OPEN_INDEX */
    uint8_t info[8]; memcpy(info, d->bdaddr, 6); bt_put16(info + 6, d->manufacturer); monitor(10, d->index, info, 8);   /* INDEX_INFO */
    return 0;
}
int bt_dev_close(struct bt_dev* d) {
    if (!(d->flags & HCI_UP)) return E_ALREADY;
    int index = d->index; uint32_t generation = d->generation;
    for (struct bt_conn* c = d->conns; c; c = c->next) bt_disconnect(c, 0x15);   /* power off */
    for (unsigned i = 0; i < 40 && d->conns; i++) { bt_wait(d, bt_now() + 25000000ULL); if (!(d = bt_dev_get(index, generation))) return 0; }
    drop_connections(d, E_HOSTDOWN);
    mgmt_powered(d, false);
    d->flags &= ~(HCI_UP | HCI_RUNNING);
    bt_cmd_sync(d, 0x0C03, NULL, 0, NULL, 0); if (!(d = bt_dev_get(index, generation))) return 0;
    fail_commands(d); d->acl_cnt = d->acl_max; d->le_cnt = d->le_max; d->cmd_cnt = 1;
    monitor(9, d->index, NULL, 0);                                         /* CLOSE_INDEX */
    return 0;
}

/* ---------- /dev/vhci ---------- */
struct vhci { bool used; uint64_t generation; struct bt_dev* d; int index; uint32_t dev_gen; struct bt_queue to_driver; bool replied, have_reply; uint8_t reply[4]; };
static struct vhci vhcis[BT_MAX_DEVICES];
static uint64_t vhci_generations; static int vhci_node = -1;
static struct vhci* vhci_of(const struct input_reader* reader) {
    unsigned i = reader ? (unsigned)(reader->sequence & 0xFF) : 0;
    if (!i || i > BT_MAX_DEVICES || !vhcis[i - 1].used || vhcis[i - 1].generation != reader->sequence >> 8) return NULL;
    return &vhcis[i - 1];
}
static struct bt_dev* vhci_dev(struct vhci* v) { return v->d ? bt_dev_get(v->index, v->dev_gen) : NULL; }
static int vhci_send(struct bt_dev* d, const uint8_t* packet, unsigned length) {
    struct vhci* v = d->driver_data;
    if (v->to_driver.count >= 256) return E_AGAIN;
    struct bt_pkt* p = bt_pkt_new(length); if (!p) return E_NOMEM;
    memcpy(p->data, packet, length); bt_queue_put(&v->to_driver, p); bt_wake(&v->to_driver); return 0;
}
static bool vhci_can_send(struct bt_dev* d, unsigned type) { (void)type; return ((struct vhci*)d->driver_data)->to_driver.count < 240; }
static const struct bt_driver vhci_driver = {"vhci", 0, vhci_send, vhci_can_send, NULL};
static int vhci_open_check(unsigned cookie, uint64_t flags) {
    (void)cookie; (void)flags;
    for (unsigned i = 0; i < BT_MAX_DEVICES; i++) if (!vhcis[i].used) return 0;
    return E_BUSY;
}
static void vhci_open(unsigned cookie, uint64_t flags, struct input_reader* reader) {
    (void)cookie; (void)flags;
    for (unsigned i = 0; i < BT_MAX_DEVICES; i++) if (!vhcis[i].used) {
        memset(&vhcis[i], 0, sizeof(vhcis[i])); vhcis[i].used = true; vhcis[i].generation = ++vhci_generations;
        reader->sequence = vhcis[i].generation << 8 | (i + 1); return;
    }
}
static void vhci_close(unsigned cookie, uint64_t flags, struct input_reader* reader) {
    (void)cookie; (void)flags; struct vhci* v = vhci_of(reader); if (!v) return;
    struct bt_dev* d = vhci_dev(v); if (d) bt_dev_unregister(d);
    bt_queue_purge(&v->to_driver); bt_wake(&v->to_driver); v->used = false; v->d = NULL; reader->sequence = 0;
}
static bool vhci_create(struct vhci* v, unsigned opcode) {
    if (v->d) return true;
    if (opcode & 0x3C || (opcode & 3)) return false;                       /* primary controllers only */
    struct bt_dev* d = bt_dev_register(&vhci_driver, v, "vhci"); if (!d) return false;
    v->d = d; v->index = d->index; v->dev_gen = d->generation;
    v->reply[0] = PKT_VENDOR; v->reply[1] = (uint8_t)opcode; bt_put16(v->reply + 2, (unsigned)d->index); v->have_reply = true;
    return true;
}
/* One controller -> host packet (a write, or a whole writev). */
static int64_t vhci_packet(struct vhci* v, const uint8_t* packet, uint64_t count) {
    {
        if (packet[0] == PKT_VENDOR) {
            if (count != 2 || v->d) return E_INVAL;
            return vhci_create(v, packet[1]) ? (int64_t)count : E_NFILE;
        }
        if (!v->d && !vhci_create(v, 0)) return E_NFILE;
        struct bt_dev* d = vhci_dev(v); if (!d) return E_NODEV;
        unsigned type = packet[0];
        if (type != PKT_EVENT && type != PKT_ACL && type != PKT_SCO && type != PKT_ISO) return E_INVAL;
        if ((type == PKT_EVENT && (count < 3 || count != 3U + packet[2])) || (type == PKT_ACL && (count < 5 || count != 5U + bt_get16(packet + 3))) ||
            (type == PKT_SCO && (count < 4 || count != 4U + packet[3])) || (type == PKT_ISO && (count < 5 || count != 5U + (bt_get16(packet + 3) & 0x3FFF)))) {
            d->stat[0]++; return E_INVAL;                                  /* framing must match the header */
        }
        bt_dev_receive(d, packet, (unsigned)count);
        worker_run();                                                      /* a command the worker waits for may be complete */
        return (int64_t)count;
    }
}
static int64_t vhci_write_vectors(unsigned cookie, struct input_reader* reader, const struct iovec64* vectors, unsigned count, uint64_t total, uint64_t flags) {
    (void)cookie; (void)flags; struct vhci* v = vhci_of(reader); if (!v) return E_NODEV;
    if (total < 1 || total > BT_FRAME_MAX) return E_INVAL;
    uint8_t packet[BT_FRAME_MAX]; int64_t n = bt_copy_in_vectors(vectors, count, total, packet, sizeof(packet));
    return n < 0 ? n : vhci_packet(v, packet, (uint64_t)n);
}
static int64_t vhci_io(unsigned cookie, struct input_reader* reader, uint64_t buffer, uint64_t count, bool write, uint64_t flags) {
    (void)cookie; struct vhci* v = vhci_of(reader); if (!v) return E_NODEV;
    if (write) {
        if (count < 1 || count > BT_FRAME_MAX) return E_INVAL;
        uint8_t packet[BT_FRAME_MAX]; if (bt_from_user(packet, buffer, count)) return E_FAULT;
        return vhci_packet(v, packet, count);
    }
    for (;;) {
        if (v->have_reply && !v->replied) {
            if (count < 4) return E_INVAL;
            if (bt_to_user(buffer, v->reply, 4)) return E_FAULT;
            v->replied = true; return 4;
        }
        struct bt_pkt* p = v->to_driver.head;
        if (p) {
            unsigned n = p->len < count ? p->len : (unsigned)count;
            if (bt_to_user(buffer, p->data, n)) return E_FAULT;
            bt_pkt_free(bt_queue_take(&v->to_driver));
            if (v->d) { struct bt_dev* d = vhci_dev(v); if (d) { cmd_kick(d); acl_kick(d); } }
            return n;
        }
        if (v->d && !vhci_dev(v)) return E_NODEV;
        if (flags & 0x800) return E_AGAIN;
        int error = task_wait_channel(&v->to_driver); if (error) return error;
        if (!(v = vhci_of(reader))) return E_NODEV;
    }
}
static uint32_t vhci_ready(unsigned cookie, const struct input_reader* reader) {
    (void)cookie; struct vhci* v = vhci_of(reader); if (!v) return 8U;
    bool readable = (v->have_reply && !v->replied) || v->to_driver.count;
    return (readable ? 1U | 64U : 0) | 4U | (v->d && !vhci_dev(v) ? 16U : 0);
}
static void* vhci_source(unsigned cookie, const struct input_reader* reader) { (void)cookie; struct vhci* v = vhci_of(reader); return v ? (void*)&v->to_driver : NULL; }
static int64_t vhci_ioctl(unsigned cookie, struct input_reader* reader, uint64_t command, uint64_t argument, uint64_t flags) { (void)cookie; (void)reader; (void)command; (void)argument; (void)flags; return E_NOTTY; }
static const struct hwnode_ops vhci_ops = {vhci_open_check, vhci_open, vhci_close, vhci_io, vhci_ioctl, vhci_ready, vhci_source, NULL, NULL};

/* ---------- socket helpers for the protocol files ---------- */
int bt_get_bytes_option(uint64_t value, uint64_t size_address, const void* data, unsigned n) {
    uint32_t size; if (bt_from_user(&size, size_address, 4)) return E_FAULT;
    if ((int32_t)size < 0) return E_INVAL;
    unsigned k = size < n ? size : n;
    if (bt_to_user(value, data, k)) return E_FAULT;
    uint32_t written = k; return bt_to_user(size_address, &written, 4);
}
int bt_get_int_option(uint64_t value, uint64_t size_address, uint32_t v) { return bt_get_bytes_option(value, size_address, &v, 4); }
int bt_set_int_option(uint64_t value, uint64_t size, uint32_t* out) {
    if (size < 4) { if (size >= 1) { uint8_t b; if (bt_from_user(&b, value, 1)) return E_FAULT; *out = b; return 0; } return E_INVAL; }
    return bt_from_user(out, value, 4);
}
int bt_sol_socket_get(uint64_t name, uint64_t value, uint64_t size_address, int type, int protocol, int error, unsigned rcvbuf, unsigned sndbuf) {
    switch (name) {
    case 3: return bt_get_int_option(value, size_address, (uint32_t)type);           /* SO_TYPE */
    case 4: return bt_get_int_option(value, size_address, (uint32_t)-error);         /* SO_ERROR */
    case 7: return bt_get_int_option(value, size_address, sndbuf);                   /* SO_SNDBUF */
    case 8: return bt_get_int_option(value, size_address, rcvbuf);                   /* SO_RCVBUF */
    case 38: return bt_get_int_option(value, size_address, (uint32_t)protocol);      /* SO_PROTOCOL */
    case 39: return bt_get_int_option(value, size_address, 31);                      /* SO_DOMAIN */
    case 12: return bt_get_int_option(value, size_address, 0);                       /* SO_PRIORITY */
    case 13: { uint32_t linger[2] = {0, 0}; return bt_get_bytes_option(value, size_address, linger, 8); }
    }
    return E_NOPROTOOPT;
}
int64_t bt_copy_out_vectors(const struct iovec64* v, unsigned count, const uint8_t* data, unsigned n) {
    unsigned at = 0;
    for (unsigned i = 0; i < count && at < n; i++) {
        unsigned take = v[i].size < (uint64_t)(n - at) ? (unsigned)v[i].size : n - at;
        if (take && bt_to_user(v[i].address, data + at, take)) return E_FAULT;
        at += take;
    }
    return at;
}
int64_t bt_copy_in_vectors(const struct iovec64* v, unsigned count, uint64_t total, uint8_t* data, unsigned max) {
    if (total > max) return E_MSGSIZE;
    unsigned at = 0;
    for (unsigned i = 0; i < count; i++) {
        if (!v[i].size) continue;
        if (bt_from_user(data + at, v[i].address, v[i].size)) return E_FAULT;
        at += (unsigned)v[i].size;
    }
    return at;
}
int bt_read_message(uint64_t address, struct message64* m, struct iovec64* v, unsigned* count, uint64_t* total) {
    if (bt_from_user(m, address, sizeof(*m))) return E_FAULT;
    if (m->count > 64) return E_MSGSIZE;
    if (m->count && bt_from_user(v, m->vectors, m->count * sizeof(struct iovec64))) return E_FAULT;
    *total = 0;
    for (uint64_t i = 0; i < m->count; i++) { if (v[i].size > 0x7FFFFFFFULL || *total + v[i].size > 0x7FFFFFFFULL) return E_INVAL; *total += v[i].size; }
    *count = (unsigned)m->count; return 0;
}

/* ---------- HCI socket operations ---------- */
struct sockaddr_hci { uint16_t family, dev, channel; };
static struct bt_dev* socket_dev(struct hci_socket* s) { return s->dev >= 0 && s->dev != 0xFFFF ? bt_dev_get(s->dev, s->dev_gen) : NULL; }
static int64_t hci_send(struct hci_socket* s, const uint8_t* data, unsigned length) {
    if (!s->bound) return E_NOTCONN;
    if (s->channel == CH_MONITOR) return E_OPNOTSUPP;
    if (s->channel == CH_CONTROL) {                                    /* queued for the worker; the reply follows as an event */
        if (length < 6) return E_INVAL;
        if (mreq_count >= 64) return -105;                                 /* ENOBUFS */
        struct mreq* r = bt_zalloc(sizeof(*r) + length); if (!r) return E_NOMEM;
        r->socket = s; r->trusted = bt_privileged(12); r->length = length; memcpy(r->data, data, length); s->holds++;
        if (mreq_tail) mreq_tail->next = r; else mreq_head = r; mreq_tail = r; mreq_count++;
        worker_run(); return length;
    }
    struct bt_dev* d = socket_dev(s); if (!d) return s->dev == 0xFFFF ? E_BADFD : E_NODEV;
    if (length < 4) return E_INVAL;
    unsigned type = data[0];
    if (type != PKT_COMMAND && type != PKT_ACL && type != PKT_SCO && type != PKT_ISO) return E_INVAL;
    if (s->channel == CH_RAW && d->user) return E_BUSY;
    if (s->channel == CH_RAW && !bt_privileged(13)) return E_PERM;
    if (s->channel == CH_RAW && !(d->flags & HCI_UP)) return E_NETDOWN;
    if (type == PKT_COMMAND && length != 4U + data[3]) return E_INVAL;
    if (type == PKT_ACL && length != 5U + bt_get16(data + 3)) return E_INVAL;
    if (s->channel == CH_USER) to_controller(d, data, length, s); else bt_send_raw(d, data, length, s);
    return length;
}
static int64_t hci_receive(struct hci_socket* s, uint8_t* out, unsigned capacity, uint64_t flags, bool* truncated, uint64_t* stamp) {
    for (;;) {
        if (s->bound && (s->channel == CH_RAW || s->channel == CH_USER) && s->dev != 0xFFFF && !socket_dev(s)) { bt_queue_purge(&s->rx); return E_NODEV; }   /* unplugged: the error first (Linux reports it before queued data) */
        struct bt_pkt* p = s->rx.head;
        if (p) {
            unsigned n = p->len < capacity ? p->len : capacity; *truncated = p->len > capacity; memcpy(out, p->data, n); *stamp = p->stamp;
            if (!(flags & BT_PEEK)) bt_pkt_free(bt_queue_take(&s->rx));   /* MSG_PEEK keeps it */
            return n;
        }
        if (flags & 0x800) return E_AGAIN;
        int error = task_wait_channel(s); if (error) return error;
    }
}
static int hci_bind(struct hci_socket* s, const struct sockaddr_hci* a) {
    if (s->bound) return E_INVAL;
    if (a->family != 31 || a->channel > CH_CONTROL) return E_INVAL;
    if (a->channel == CH_MONITOR || a->channel == CH_CONTROL) {
        if (a->dev != 0xFFFF) return E_INVAL;
        if (a->channel == CH_MONITOR && !bt_privileged(13)) return E_PERM;
        s->channel = a->channel; s->dev = 0xFFFF; s->bound = true;
        if (a->channel == CH_MONITOR) {
            for (unsigned i = 0; i < BT_MAX_DEVICES; i++) { struct bt_dev* d = bt_dev_by_index((int)i); if (d && !d->user) monitor_new_index(d, s); }
            for (unsigned i = 0; i < SOCKETS; i++) if (sockets[i] && sockets[i]->bound && sockets[i]->channel == CH_CONTROL) monitor_ctrl_open(sockets[i], s);
        } else {
            static uint32_t cookies; s->cookie = ++cookies;
            const char* path = task_executable(task_tid()); const char* base = path ? path : "?";
            for (const char* p = base; p && *p; p++) if (*p == '/') base = p + 1;
            unsigned k = 0; while (base[k] && k < 15) { s->comm[k] = base[k]; k++; }
            monitor_ctrl_open(s, NULL); s->monitor_open = true;
        }
        return 0;
    }
    if (a->dev == 0xFFFF) { if (a->channel != CH_RAW) return E_INVAL; s->channel = CH_RAW; s->dev = 0xFFFF; s->bound = true; return 0; }
    struct bt_dev* d = bt_dev_by_index(a->dev); if (!d) return E_NODEV;
    if (a->channel == CH_USER) {
        if (!bt_privileged(12)) return E_PERM;
        if (d->user || !d->setup_done || (d->flags & (HCI_UP | HCI_INIT))) return E_BUSY;
        d->user = s; d->flags |= HCI_USER;
        mgmt_index_removed(d);                                             /* mgmt does not see a user-owned controller */
    }
    s->channel = a->channel; s->dev = a->dev; s->dev_gen = d->generation; s->bound = true;
    return 0;
}
static void socket_unlink(struct hci_socket* s) { for (unsigned i = 0; i < SOCKETS; i++) if (sockets[i] == s) sockets[i] = NULL; }
void mgmt_socket_release_hold(void* socket);
void mgmt_socket_release_hold(void* socket) { struct hci_socket* s = socket; if (s->holds) s->holds--; if (!s->holds && s->dead) { bt_queue_purge(&s->rx); bt_free(s, sizeof(*s)); } }
bool mgmt_socket_alive(void* socket);
bool mgmt_socket_alive(void* socket) { return !((struct hci_socket*)socket)->dead; }
static void hci_release(void* object) {
    struct hci_socket* s = object;
    struct bt_dev* d = s->bound && s->channel == CH_USER ? socket_dev(s) : NULL;
    if (d && d->user == s) { d->user = NULL; d->flags &= ~(HCI_USER | HCI_UP | HCI_RUNNING); d->cmd_cnt = 1; mgmt_index_added(d); }
    if (s->bound && s->channel == CH_CONTROL) { mgmt_socket_closed(s); uint8_t c[4]; bt_put32(c, s->cookie); monitor(15, -1, c, 4); }
    socket_unlink(s); bt_queue_purge(&s->rx);
    if (s->holds) { s->dead = true; return; }                             /* the worker still holds it: freed after its request */
    bt_free(s, sizeof(*s));
}
static int64_t hci_io(void* object, const struct iovec64* vectors, unsigned count, uint64_t total, bool write, uint64_t flags) {
    struct hci_socket* s = object; uint8_t buffer[BT_FRAME_MAX];
    if (write) { int64_t n = bt_copy_in_vectors(vectors, count, total, buffer, sizeof(buffer)); return n < 0 ? n : hci_send(s, buffer, (unsigned)n); }
    bool truncated = false; uint64_t stamp; int64_t n = hci_receive(s, buffer, total > sizeof(buffer) ? sizeof(buffer) : (unsigned)total, flags, &truncated, &stamp);
    return n < 0 ? n : bt_copy_out_vectors(vectors, count, buffer, (unsigned)n);
}
static int dev_info(uint64_t argument) {
    uint8_t info[92]; if (bt_from_user(info, argument, 2)) return E_FAULT;
    struct bt_dev* d = bt_dev_by_index((int)bt_get16(info)); if (!d) return E_NODEV;
    memset(info, 0, sizeof(info)); bt_put16(info, (unsigned)d->index); info[2] = 'h'; info[3] = 'c'; info[4] = 'i'; info[5] = (char)('0' + d->index);
    memcpy(info + 10, d->bdaddr, 6); memcpy(info + 16, &d->flags, 4); info[20] = (uint8_t)(d->driver ? d->driver->bus : 0); memcpy(info + 21, d->features[0], 8);
    uint32_t mode = 0x8000; memcpy(info + 36, &mode, 4);                    /* pkt_type, link_policy, link_mode */
    bt_put16(info + 44, d->acl_mtu); bt_put16(info + 46, d->acl_max); bt_put16(info + 48, d->sco_mtu); bt_put16(info + 50, d->sco_max);
    memcpy(info + 52, d->stat, 40);
    return bt_to_user(argument, info, sizeof(info));
}
static int hci_ioctl(void* object, uint64_t command, uint64_t argument, uint64_t flags) {
    (void)flags; struct hci_socket* s = object;
    if (s->channel != CH_RAW) return E_BADFD;
    switch ((uint32_t)command) {
    case 0x800448D2: {                                                     /* HCIGETDEVLIST */
        uint16_t max; if (bt_from_user(&max, argument, 2)) return E_FAULT;
        if (!max || max > 16) return E_INVAL;
        if (!accessible(argument, 4 + 8ULL * max, true)) return E_FAULT;
        uint16_t n = 0;
        for (unsigned i = 0; i < BT_MAX_DEVICES && n < max; i++) {
            struct bt_dev* d = bt_dev_by_index((int)i); if (!d) continue;
            uint8_t entry[8] = {0}; bt_put16(entry, i); memcpy(entry + 4, &d->flags, 4); if (bt_to_user(argument + 4 + 8ULL * n, entry, 8)) return E_FAULT; n++;
        }
        return bt_to_user(argument, &n, 2);
    }
    case 0x800448D3: return dev_info(argument);                            /* HCIGETDEVINFO */
    case 0x800448D4: {                                                     /* HCIGETCONNLIST: {dev_id, conn_num, conn_info[]} */
        uint8_t request[4]; if (bt_from_user(request, argument, 4)) return E_FAULT;
        struct bt_dev* d = bt_dev_by_index((int)bt_get16(request)); if (!d) return E_NODEV;
        unsigned max = bt_get16(request + 2), n = 0; if (!max || max > 64) return E_INVAL;
        for (struct bt_conn* c = d->conns; c && n < max; c = c->next) {
            if (c->state != CONN_CONNECTED) continue;
            uint8_t ci[16] = {0}; bt_put16(ci, c->handle); memcpy(ci + 2, c->peer, 6); ci[8] = c->link == LINK_LE ? 0x80 : 1; ci[9] = c->outgoing; bt_put16(ci + 10, 1);
            uint32_t mode = (c->role ? 0 : 1) | (c->authenticated ? 2 : 0) | (c->encrypted ? 4 : 0); memcpy(ci + 12, &mode, 4);
            if (bt_to_user(argument + 4 + 16ULL * n, ci, 16)) return E_FAULT; n++;
        }
        uint16_t count = (uint16_t)n; return bt_to_user(argument + 2, &count, 2);
    }
    case 0x800448D5: {                                                     /* HCIGETCONNINFO: {bdaddr, type, conn_info} */
        uint8_t request[7]; if (bt_from_user(request, argument, 7)) return E_FAULT;
        for (unsigned i = 0; i < BT_MAX_DEVICES; i++) {
            struct bt_dev* d = bt_dev_by_index((int)i); if (!d) continue;
            struct bt_conn* c = bt_conn_lookup(d, request, request[6] == 0x80 ? LINK_LE : LINK_ACL);
            if (!c || c->state != CONN_CONNECTED) continue;
            uint8_t ci[16] = {0}; bt_put16(ci, c->handle); memcpy(ci + 2, c->peer, 6); ci[8] = request[6]; ci[9] = c->outgoing; bt_put16(ci + 10, 1);
            uint32_t mode = (c->role ? 0 : 1) | (c->authenticated ? 2 : 0) | (c->encrypted ? 4 : 0); memcpy(ci + 12, &mode, 4);
            return bt_to_user(argument + 8, ci, 16);
        }
        return E_NOENT;
    }
    case 0x400448C9: case 0x400448CA: case 0x400448CB: case 0x400448CC: case 0x400448DD: {   /* UP DOWN RESET RESTAT SETSCAN */
        if (!bt_privileged(12)) return E_PERM;
        unsigned index = (unsigned)argument; uint8_t req[8];
        if ((uint32_t)command == 0x400448DD) { if (bt_from_user(req, argument, 8)) return E_FAULT; index = bt_get16(req); }
        struct bt_dev* d = bt_dev_by_index((int)index); if (!d) return E_NODEV;
        switch ((uint32_t)command) {
        case 0x400448C9: { int r = bt_dev_open(d); if (!r && (d = bt_dev_by_index((int)index))) mgmt_powered(d, true); return r; }
        case 0x400448CA: return d->user ? E_BUSY : bt_dev_close(d);
        case 0x400448CB: if (!(d->flags & HCI_UP)) return E_NETDOWN; { int r = bt_cmd_sync(d, 0x0C03, NULL, 0, NULL, 0); return r < 0 ? r : r ? E_IO : 0; }
        case 0x400448CC: memset(d->stat, 0, sizeof(d->stat)); return 0;
        default: if (!(d->flags & HCI_UP)) return E_NETDOWN; { uint8_t scan = req[4]; int r = bt_cmd_sync(d, 0x0C1A, &scan, 1, NULL, 0); if (!r && (d = bt_dev_by_index((int)index))) d->scan_enable = scan; return r < 0 ? r : r ? E_IO : 0; }
        }
    }
    }
    return E_NOTTY;
}
static uint32_t hci_ready(void* object) {
    struct hci_socket* s = object; uint32_t mask = 4U;
    if (s->rx.count) mask |= 1U | 64U;
    if (s->bound && (s->channel == CH_RAW || s->channel == CH_USER) && s->dev != 0xFFFF && !socket_dev(s)) mask |= 8U | 16U;
    return mask;
}
static void* hci_source(void* object) { return object; }
static void hci_stat(void* object, struct linux_stat* stat) {
    memset(stat, 0, sizeof(*stat)); stat->dev = 4; stat->ino = 0x60000000ULL + ((struct hci_socket*)object)->id; stat->nlink = 1; stat->mode = 0140777; stat->block_size = 4096;
}
static const struct object_operations hci_operations = {hci_io, hci_ioctl, hci_ready, hci_source, hci_release, hci_stat};
static int64_t hci_create(uint64_t type) {
    if ((type & 0xF) != 3) return E_SOCKTNOSUPPORT;
    if (type & ~(0xFULL | 0x800 | 0x80000)) return E_INVAL;
    unsigned slot = 0; while (slot < SOCKETS && sockets[slot]) slot++;
    if (slot == SOCKETS) return E_MFILE;
    struct hci_socket* s = bt_zalloc(sizeof(*s)); if (!s) return E_NOMEM;
    s->dev = -1; s->id = ++socket_ids; sockets[slot] = s;
    int fd = fd_create_object(&hci_operations, s, (type & 0x800) | (type & 0x80000) | 2, "socket:[bluetooth]");
    if (fd < 0) { sockets[slot] = NULL; bt_free(s, sizeof(*s)); }
    return fd;
}
/* mgmt replies/events to one control socket (bt_mgmt.c) */
void mgmt_socket_send(void* socket, const uint8_t* message, unsigned length);
void mgmt_socket_send(void* socket, const uint8_t* message, unsigned length) {
    struct hci_socket* s = socket; if (s->dead) return;
    socket_push(s, message, length, NULL, 0, true);
    mgmt_monitor_command((int)(int16_t)bt_get16(message + 2), message, length, true, s->cookie);
}
uint32_t mgmt_socket_cookie(void* socket);
uint32_t mgmt_socket_cookie(void* socket) { return ((struct hci_socket*)socket)->cookie; }
void mgmt_foreach_socket(void (*fn)(void* socket, void* arg), void* arg);
void mgmt_foreach_socket(void (*fn)(void* socket, void* arg), void* arg) {
    for (unsigned i = 0; i < SOCKETS; i++) { struct hci_socket* s = sockets[i]; if (s && s->bound && s->channel == CH_CONTROL) fn(s, arg); }
}
static int put_control(struct message64* m, uint64_t address, unsigned* used, int level, int type, const void* data, unsigned n) {
    unsigned space = (16 + n + 7) & ~7U;
    if (!m->control || *used + 16 + n > m->control_size) { m->flags |= 8; return 0; }   /* MSG_CTRUNC */
    uint8_t c[48] = {0}; uint64_t len = 16 + n; memcpy(c, &len, 8); memcpy(c + 8, &level, 4); memcpy(c + 12, &type, 4); memcpy(c + 16, data, n);
    if (bt_to_user(m->control + *used, c, 16 + n)) return E_FAULT;
    *used += space < m->control_size - *used ? space : m->control_size - *used; (void)address; return 0;
}
static int hci_get_option(struct hci_socket* s, uint64_t level, uint64_t name, uint64_t value, uint64_t size_address) {
    if (level == 1) {
        if (name == 29) return bt_get_int_option(value, size_address, s->so_stamp);   /* SO_TIMESTAMP */
        return bt_sol_socket_get(name, value, size_address, 3, 1, 0, 65536, 65536);
    }
    if (level != 0) return E_NOPROTOOPT;
    if (name == 2) {                                                       /* HCI_FILTER */
        uint8_t f[16] = {0}; memcpy(f, &s->type_mask, 4); memcpy(f + 4, s->event_mask, 8); bt_put16(f + 12, s->opcode);
        return bt_get_bytes_option(value, size_address, f, 14);
    }
    if (name == 1) return bt_get_int_option(value, size_address, s->data_dir);
    if (name == 3) return bt_get_int_option(value, size_address, s->hci_stamp);
    return E_NOPROTOOPT;
}
static int hci_set_option(struct hci_socket* s, uint64_t level, uint64_t name, uint64_t value, uint64_t size) {
    uint32_t v = 0;
    if (level == 1) {
        if (name == 29 || name == 35) { if (bt_set_int_option(value, size, &v)) return E_FAULT; s->so_stamp = v != 0; return 0; }   /* SO_TIMESTAMP(NS) */
        if (name == 7 || name == 8 || name == 12 || name == 16 || name == 32 || name == 33) return 0;   /* buffers, priority, SO_PASSCRED */
        return E_NOPROTOOPT;
    }
    if (level != 0) return E_NOPROTOOPT;
    if (s->channel != CH_RAW) return E_BADFD;
    if (name == 2) {
        uint8_t f[16] = {0}; unsigned n = size < 16 ? (unsigned)size : 16;
        if (n < 14) return E_INVAL;
        if (bt_from_user(f, value, n)) return E_FAULT;
        memcpy(&s->type_mask, f, 4); memcpy(s->event_mask, f + 4, 8); s->opcode = (uint16_t)bt_get16(f + 12); return 0;
    }
    if (name == 1 || name == 3) { if (bt_set_int_option(value, size, &v)) return E_FAULT; if (name == 1) s->data_dir = v != 0; else s->hci_stamp = v != 0; return 0; }
    return E_NOPROTOOPT;
}
static int64_t hci_call(struct registers* r, struct hci_socket* s, uint64_t descriptor_flags) {
    switch (r->rax) {
    case 49: {                                                             /* bind */
        struct sockaddr_hci a; if (r->rdx < sizeof(a)) return E_INVAL;
        if (bt_from_user(&a, r->rsi, sizeof(a))) return E_FAULT;
        return hci_bind(s, &a);
    }
    case 51: case 52: {                                                    /* getsockname / getpeername */
        if (r->rax == 52) return E_OPNOTSUPP;
        struct sockaddr_hci a = {31, (uint16_t)(s->bound ? s->dev : 0), (uint16_t)s->channel};
        uint32_t size; if (bt_from_user(&size, r->rdx, 4)) return E_FAULT;
        unsigned n = size < sizeof(a) ? size : sizeof(a);
        if (bt_to_user(r->rsi, &a, n)) return E_FAULT;
        uint32_t full = sizeof(a); return bt_to_user(r->rdx, &full, 4);
    }
    case 44: { struct iovec64 v = {r->rsi, r->rdx}; return hci_io(s, &v, 1, r->rdx, true, descriptor_flags); }
    case 45: { struct iovec64 v = {r->rsi, r->rdx}; return hci_io(s, &v, 1, r->rdx, false, descriptor_flags | (r->r10 & 0x40 ? 0x800 : 0) | (r->r10 & 2 ? BT_PEEK : 0)); }
    case 46: case 47: {                                                    /* sendmsg / recvmsg */
        struct message64 m; struct iovec64 v[64]; unsigned count; uint64_t total;
        int e = bt_read_message(r->rsi, &m, v, &count, &total); if (e) return e;
        if (r->rax == 46) return hci_io(s, v, count, total, true, descriptor_flags);
        uint8_t buffer[BT_FRAME_MAX]; bool truncated = false; uint64_t stamp = 0;
        int64_t n = hci_receive(s, buffer, total > sizeof(buffer) ? sizeof(buffer) : (unsigned)total, descriptor_flags | (r->rdx & 0x40 ? 0x800 : 0) | (r->rdx & 2 ? BT_PEEK : 0), &truncated, &stamp);
        if (n < 0) return n;
        int64_t copied = bt_copy_out_vectors(v, count, buffer, (unsigned)n); if (copied < 0) return copied;
        unsigned used = 0; m.flags = truncated ? 0x20 : 0;               /* MSG_TRUNC */
        uint64_t ns = stamp >> 1; uint64_t tv[2] = {ns / 1000000000ULL, (ns % 1000000000ULL) / 1000};
        if (s->channel == CH_RAW && s->data_dir) { int incoming = (int)(stamp & 1); if ((e = put_control(&m, r->rsi, &used, 0, 1, &incoming, 4))) return e; }
        if (s->channel == CH_RAW && s->hci_stamp) { if ((e = put_control(&m, r->rsi, &used, 0, 3, tv, 16))) return e; }
        if (s->so_stamp) { if ((e = put_control(&m, r->rsi, &used, 1, 29, tv, 16))) return e; }
        m.control_size = used; m.name_size = 0;
        if (bt_to_user(r->rsi, &m, sizeof(m))) return E_FAULT;
        return (r->rdx & 0x20) && truncated ? (int64_t)copied : copied;
    }
    case 54: return hci_set_option(s, r->rsi, r->rdx, r->r10, r->r8);
    case 55: return hci_get_option(s, r->rsi, r->rdx, r->r10, r->r8);
    case 48: return 0;                                                     /* shutdown */
    }
    return E_OPNOTSUPP;                                                    /* connect, listen, accept: not on HCI sockets */
}

/* ---------- AF_BLUETOOTH entry ---------- */
bool bluetooth_descriptor(struct open_file* f) { return fd_object(f, &hci_operations) || l2cap_descriptor(f) || rfcomm_descriptor(f) || alg_descriptor(f); }
int64_t bluetooth_call(struct registers* r) {
    if (r->rax == 41) {
        if (r->rdi == 38) { return alg_call(r, NULL); }                    /* AF_ALG */
        switch (r->rdx) {
        case 0: return l2cap_socket_create(r->rsi, r->rdx, false);       /* BTPROTO_L2CAP */
        case 1: return hci_create(r->rsi);                                 /* BTPROTO_HCI */
        case 3: return rfcomm_socket_create(r->rsi, r->rdx);              /* BTPROTO_RFCOMM */
        default: return E_PROTONOSUPPORT;                                  /* SCO, BNEP, CMTP, HIDP, AVDTP, ISO */
        }
    }
    struct open_file* f = fd_lookup(r->rdi); if (!f) return E_BADF;
    if (alg_descriptor(f)) return alg_call(r, f);
    if (l2cap_descriptor(f)) return l2cap_call(r, f);
    if (rfcomm_descriptor(f)) return rfcomm_call(r, f);
    struct hci_socket* s = fd_object(f, &hci_operations); if (!s) return -88;
    fd_pin(f); int64_t result = hci_call(r, s, fd_status_flags(f)); fd_unpin(f);
    return result;
}

/* ---------- periodic work and boot ---------- */
void bluetooth_poll(uint64_t now);
uint64_t bluetooth_next_event(uint64_t now);
void bluetooth_poll(uint64_t now) {
    for (unsigned i = 0; i < BT_MAX_DEVICES; i++) {
        struct bt_dev* d = bt_dev_by_index((int)i); if (!d) continue;
        cmd_timeouts(d, now);
        for (struct bt_conn *c = d->conns, *next; c; c = next) {
            next = c->next;
            if (!c->idle_deadline || now < c->idle_deadline) continue;
            c->idle_deadline = 0;
            if (c->state == CONN_CONNECTING && c->outgoing && c->link == LINK_LE) { bt_disconnect(c, 0x13); continue; }   /* LE Create Connection Cancel */
            if (c->state == CONN_CONNECTED && !conn_in_use(c)) bt_disconnect(c, 0x13);
        }
        if ((d = bt_dev_by_index((int)i))) mgmt_poll(d, now);
    }
    l2cap_poll(now); smp_poll(now); rfcomm_poll(now);
    worker_run();
}
uint64_t bluetooth_next_event(uint64_t now) {
    uint64_t next = ~0ULL;
    for (unsigned i = 0; i < BT_MAX_DEVICES; i++) {
        struct bt_dev* d = bt_dev_by_index((int)i); if (!d) continue;
        if (d->cmd_sent && d->cmd_deadline < next) next = d->cmd_deadline;
        for (struct bt_conn* c = d->conns; c; c = c->next) if (c->idle_deadline && c->idle_deadline < next) next = c->idle_deadline;
        uint64_t m = mgmt_next_event(d, now); if (m < next) next = m;
    }
    if (worker_stack && (mreq_head || (worker_waiting && worker_ready))) next = now;
    else if (worker_waiting && worker_deadline && worker_deadline < next) next = worker_deadline;
    uint64_t a = l2cap_next_event(now), b = smp_next_event(now), c = rfcomm_next_event(now);
    if (a < next) next = a; if (b < next) next = b; if (c < next) next = c;
    return next;
}
void bluetooth_init(void) {
    if (vhci_node >= 0) return;
    vhci_node = hwnode_add("/dev/vhci", 10, 137, 0, 0600, 0, &vhci_ops, "misc");
    if (!worker_init()) vga_print("[64] BLUETOOTH: no memory for the mgmt worker\n");
    hwnode_set_write_vectors(vhci_node, vhci_write_vectors);
    rfcomm_init();
    unsigned failed = bt_crypto_selftest();
    vga_print(failed ? "[64] FAIL: Bluetooth crypto known answers (AES, CMAC, SMP c1/s1/ah/f4/f5/f6/g2/h6, P-256)\n"
                     : "[64] PASS Bluetooth crypto known answers (FIPS-197, RFC 4493, SMP c1/s1/ah/f4/f5/f6/g2/h6, P-256 debug key, ECDH)\n");
}
/* Bluetooth objects alive (tests: everything is returned after the last socket closed and the controllers left). */
unsigned bluetooth_live_objects(void);
unsigned bluetooth_live_objects(void) {
    unsigned n = 0; for (unsigned i = 0; i < BT_MAX_DEVICES; i++) n += devs[i] != NULL;
    for (unsigned i = 0; i < SOCKETS; i++) n += sockets[i] != NULL;
    for (unsigned i = 0; i < 64; i++) n += conn_table[i] != NULL;
    return n + l2cap_live() + rfcomm_live();
}
