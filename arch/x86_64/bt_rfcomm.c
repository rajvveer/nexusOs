/* RFCOMM (Bluetooth RFCOMM 1.2 over TS 07.10) and BTPROTO_RFCOMM stream sockets (struct sockaddr_rc).
 * One session per link on L2CAP PSM 3 (bt_l2cap.c kernel channels), multiplexer control on DLCI 0: SABM/UA/DM/DISC,
 * UIH with FCS (CRC-8 over address + control [+ length]), PN (credit-based flow control negotiated, MTU), MSC, RPN,
 * RLS, TEST, FCON/FCOFF, NSC for unknown commands. DLCI = server channel << 1 | (session initiator ? 0 : 1).
 * Credit-based flow control (7 initial credits, returned as the reader consumes data). A DLC needs its socket's
 * security level on the ACL link before SABM (outgoing) or UA (incoming). Bad FCS, unknown DLCIs, frames beyond the
 * MTU and malformed control messages are dropped or answered with DM/NSC; 20 s timeouts for SABM/DISC/PN. */
#include "bt.h"
#include "../../kernel/string.h"

#define RFCOMM_PSM 3
#define DEFAULT_MTU 667
#define RX_LIMIT (128U * 1024U)
struct rc_session;
struct rc_dlc {
    struct rc_dlc* next; struct rc_session* session; uint8_t dlci, state; bool initiator, cfc, msc_sent;
    uint16_t mtu; unsigned tx_credits, rx_credits; uint64_t deadline; struct rcsock* sock; uint8_t sec_level; bool security_wait;
};
enum { D_PN = 1, D_SABM, D_CONNECTED, D_DISC, D_SECURITY };
struct rc_session {
    struct rc_session* next; struct l2cap_chan* chan; bool initiator, open, closing; uint8_t peer[6]; int dev_index; uint32_t dev_gen;
    struct rc_dlc* dlcs; uint64_t deadline, idle_deadline; uint16_t mtu;
};
struct rcsock {
    uint64_t id; bool bound, listening; uint8_t src[6], channel, dst[6], dchannel; struct rc_dlc* dlc;
    struct rcsock *parent, *accept_head, *accept_next; unsigned backlog, accept_count;
    struct bt_queue rx; uint32_t rx_offset; int error; bool eof, connected_once, shut_rd, shut_wr, defer;
    uint8_t sec_level;
};
static struct rc_session* sessions;
static struct rcsock* socks[64]; static uint64_t sock_ids; static unsigned live;
static uint8_t crc_table[256];
unsigned rfcomm_live(void) { unsigned n = live; for (struct rc_session* s = sessions; s; s = s->next) { n++; for (struct rc_dlc* d = s->dlcs; d; d = d->next) n++; } return n; }

/* ---------- frames ---------- */
static uint8_t fcs(const uint8_t* p, unsigned n) { uint8_t f = 0xFF; while (n--) f = crc_table[f ^ *p++]; return (uint8_t)(0xFF - f); }
static bool fcs_ok(const uint8_t* p, unsigned n, uint8_t received) { uint8_t f = 0xFF; while (n--) f = crc_table[f ^ *p++]; f = crc_table[f ^ received]; return f == 0xCF; }
static uint8_t cr_bit(struct rc_session* s, bool command) { return (uint8_t)(s->initiator == command ? 1 : 0); }
static int send_frame(struct rc_session* s, uint8_t dlci, uint8_t control, bool command, const uint8_t* data, unsigned length, int credits) {
    uint8_t f[1100]; unsigned n = 0;
    if (length + 8 > sizeof(f)) return E_MSGSIZE;
    f[n++] = (uint8_t)(dlci << 2 | cr_bit(s, command) << 1 | 1); f[n++] = control;
    if (length > 127) { f[n++] = (uint8_t)(length << 1); f[n++] = (uint8_t)(length >> 7); } else f[n++] = (uint8_t)(length << 1 | 1);
    unsigned fcs_span = (control & 0xEF) == 0xEF ? 2 : n;                   /* UIH: address + control only */
    if (credits >= 0) f[n++] = (uint8_t)credits;
    if (length) memcpy(f + n, data, length);
    n += length; f[n] = fcs(f, fcs_span); n++;
    return s->chan ? l2cap_kernel_send(s->chan, f, n) : E_NOTCONN;
}
static void send_mcc(struct rc_session* s, uint8_t type, bool command, const uint8_t* value, unsigned length) {
    uint8_t m[16]; m[0] = (uint8_t)(type << 2 | (command ? 2 : 0) | 1); m[1] = (uint8_t)(length << 1 | 1); memcpy(m + 2, value, length);
    send_frame(s, 0, 0xEF, true, m, 2 + length, -1);
}
static void send_pn(struct rc_session* s, struct rc_dlc* d, bool command) {
    uint8_t v[8] = {d->dlci, (uint8_t)(command ? 0xF0 : (d->cfc ? 0xE0 : 0x00)), 0, 0, 0, 0, 0, (uint8_t)(d->cfc || command ? 7 : 0)};
    bt_put16(v + 4, d->mtu); if (d->cfc || command) d->rx_credits = 7;
    send_mcc(s, 0x20, command, v, 8);
}
static void send_msc(struct rc_session* s, struct rc_dlc* d, bool command, uint8_t signals) { uint8_t v[2] = {(uint8_t)(d->dlci << 2 | 2 | 1), signals}; send_mcc(s, 0x38, command, v, 2); if (command) d->msc_sent = true; }

/* ---------- sockets (declarations) ---------- */
static void sock_wake(struct rcsock* k) { if (k) { bt_wake(k); if (k->parent) bt_wake(k->parent); } }
static struct rcsock* sock_alloc(void);
static void sock_free(struct rcsock* k);

/* ---------- DLCs and sessions ---------- */
static struct rc_dlc* dlc_find(struct rc_session* s, uint8_t dlci) { for (struct rc_dlc* d = s->dlcs; d; d = d->next) if (d->dlci == dlci) return d; return NULL; }
static struct rc_dlc* dlc_new(struct rc_session* s, uint8_t dlci, bool initiator) {
    struct rc_dlc* d = bt_zalloc(sizeof(*d)); if (!d) return NULL;
    d->session = s; d->dlci = dlci; d->initiator = initiator; d->mtu = s->mtu ? s->mtu : DEFAULT_MTU; d->cfc = true; d->next = s->dlcs; s->dlcs = d; s->idle_deadline = 0;
    return d;
}
static void session_close(struct rc_session* s, int error);
static void dlc_free(struct rc_dlc* d, int error) {
    struct rc_session* s = d->session;
    for (struct rc_dlc** p = &s->dlcs; *p; p = &(*p)->next) if (*p == d) { *p = d->next; break; }
    struct rcsock* k = d->sock;
    if (k && k->parent && !k->connected_once) {                           /* never accepted: it leaves the listener's queue */
        for (struct rcsock** p = &k->parent->accept_head; *p; p = &(*p)->accept_next) if (*p == k) { *p = k->accept_next; k->parent->accept_count--; break; }
        k->dlc = NULL; sock_free(k); k = NULL;
    }
    if (k) { k->dlc = NULL; k->eof = true; if (error && (!k->connected_once || error != E_CONNRESET)) k->error = error; if (!k->connected_once && !error) k->error = E_CONNREFUSED; sock_wake(k); }
    bt_free(d, sizeof(*d));
    if (!s->dlcs && !s->closing) s->idle_deadline = bt_now() + 2000000000ULL;   /* the last DLC: the session goes after 2 s */
}
static void dlc_connected(struct rc_dlc* d) {
    d->state = D_CONNECTED; d->deadline = 0;
    if (d->sock) { d->sock->connected_once = true; d->sock->error = 0; sock_wake(d->sock); }
    send_msc(d->session, d, true, 0x8D);
}
static void session_free(struct rc_session* s) {
    for (struct rc_session** p = &sessions; *p; p = &(*p)->next) if (*p == s) { *p = s->next; break; }
    bt_free(s, sizeof(*s));
}
static void session_close(struct rc_session* s, int error) {
    if (s->closing) return;
    s->closing = true;
    while (s->dlcs) dlc_free(s->dlcs, error);
    if (s->chan) { struct l2cap_chan* ch = s->chan; s->chan = NULL; l2cap_kernel_close(ch); }
    session_free(s);
}
static struct rc_session* session_for(const uint8_t* peer, int dev_index) {
    for (struct rc_session* s = sessions; s; s = s->next) if (!s->closing && s->dev_index == dev_index && !memcmp(s->peer, peer, 6)) return s;
    return NULL;
}
static void l2_state(void* owner, int state, int error);
static void l2_recv(void* owner, const uint8_t* data, unsigned length);
static const struct l2cap_ops session_ops = {l2_state, l2_recv};
static struct rcsock* listener_for(uint8_t channel, const uint8_t* local) {
    struct rcsock* any = NULL;
    for (unsigned i = 0; i < 64; i++) { struct rcsock* k = socks[i]; if (!k || !k->listening || k->channel != channel) continue; if (!memcmp(k->src, local, 6)) return k; if (bt_addr_any(k->src)) any = k; }
    return any;
}
static void dlc_start(struct rc_dlc* d) {                                  /* initiator: PN, then SABM once secure */
    struct rc_session* s = d->session; d->state = D_PN; d->deadline = bt_now() + 20000000000ULL; send_pn(s, d, true);
}
static void dlc_sabm(struct rc_dlc* d) {
    struct rc_session* s = d->session; struct bt_conn* c = l2cap_chan_conn(s->chan);
    if (!c) return;
    if (!bt_conn_security(c, d->sec_level, true)) { d->state = D_SECURITY; d->security_wait = true; d->deadline = bt_now() + 40000000000ULL; return; }
    d->state = D_SABM; d->deadline = bt_now() + 20000000000ULL; send_frame(s, d->dlci, 0x3F, true, NULL, 0, -1);   /* SABM + P */
}
static void incoming_sabm(struct rc_session* s, uint8_t dlci) {
    if (dlci == 0) { s->open = true; send_frame(s, 0, 0x73, false, NULL, 0, -1); return; }   /* UA + F */
    struct rc_dlc* d = dlc_find(s, dlci);
    struct bt_dev* dev = bt_dev_get(s->dev_index, s->dev_gen);
    struct rcsock* listener = dev ? listener_for((uint8_t)(dlci >> 1), dev->bdaddr) : NULL;
    if ((d && d->state == D_CONNECTED) || !listener || listener->accept_count >= listener->backlog || (dlci & 1) != (s->initiator ? 1 : 0) || !s->open) {
        if (d && d->state != D_CONNECTED) dlc_free(d, E_CONNREFUSED);
        send_frame(s, dlci, 0x1F, false, NULL, 0, -1); return;               /* DM + F */
    }
    if (!d && !(d = dlc_new(s, dlci, false))) { send_frame(s, dlci, 0x1F, false, NULL, 0, -1); return; }
    struct rcsock* k = sock_alloc(); if (!k) { dlc_free(d, E_NOMEM); send_frame(s, dlci, 0x1F, false, NULL, 0, -1); return; }
    k->parent = listener; k->bound = true; memcpy(k->src, dev->bdaddr, 6); k->channel = listener->channel; memcpy(k->dst, s->peer, 6); k->dchannel = (uint8_t)(dlci >> 1);
    k->sec_level = listener->sec_level; k->dlc = d; d->sock = k; d->sec_level = listener->sec_level;
    struct rcsock** tail = &listener->accept_head; while (*tail) tail = &(*tail)->accept_next; *tail = k; listener->accept_count++;
    struct bt_conn* c = l2cap_chan_conn(s->chan);
    if (c && !bt_conn_security(c, d->sec_level, false)) { d->state = D_SECURITY; d->security_wait = true; d->deadline = bt_now() + 40000000000ULL; return; }
    send_frame(s, dlci, 0x73, false, NULL, 0, -1); dlc_connected(d);
}
static void mcc(struct rc_session* s, const uint8_t* data, unsigned length) {
    if (length < 2 || !(data[0] & 1) || !(data[1] & 1)) return;
    uint8_t type = data[0] >> 2; bool command = (data[0] & 2) != 0; unsigned n = data[1] >> 1; const uint8_t* v = data + 2;
    if (n > length - 2) return;
    switch (type) {
    case 0x20: {                                                           /* PN */
        if (n != 8) return;
        uint8_t dlci = v[0] & 0x3F; uint16_t mtu = (uint16_t)bt_get16(v + 4);
        if (!dlci) return;
        struct rc_dlc* d = dlc_find(s, dlci);
        /* Linux rfcomm_apply_pn: flow control 0xF0 or 0xE0 = credit based, the credits byte as sent (BlueZ's emulator
         * answers 0xF0 with 255 credits) */
        bool cfc = v[1] == 0xF0 || v[1] == 0xE0;
        if (command) {
            if (!d) { if (!(d = dlc_new(s, dlci, false))) return; d->state = 0; }
            if (d->state != D_CONNECTED) { d->cfc = cfc; d->tx_credits = cfc ? v[7] : 0; }
            if (mtu < d->mtu && mtu >= 23) d->mtu = mtu; else if (mtu < 23) d->mtu = 23;
            send_pn(s, d, false); d->deadline = bt_now() + 20000000000ULL; return;
        }
        if (!d || d->state != D_PN) return;
        d->cfc = cfc; d->tx_credits = cfc ? v[7] : 0;
        if (mtu >= 23 && mtu < d->mtu) d->mtu = mtu;
        dlc_sabm(d); return;
    }
    case 0x38: {                                                           /* MSC */
        if (n < 2) return;
        if (command) { uint8_t r[2] = {v[0], v[1]}; send_mcc(s, 0x38, false, r, 2); }
        return;
    }
    case 0x24: {                                                           /* RPN: echo the request (or defaults) */
        if (!command) return;
        uint8_t r[8] = {v[0], 0x07, 0x03, 0x00, 0x00, 0x11, 0x13, 0x3F};   /* 9600 8N1, no flow control, all parameters */
        if (n == 8) memcpy(r, v, 8); else if (n != 1) return;
        r[0] = v[0]; send_mcc(s, 0x24, false, r, 8); return;
    }
    case 0x14: if (command && n == 2) send_mcc(s, 0x14, false, v, 2); return;   /* RLS */
    case 0x08: if (command && n <= 12) send_mcc(s, 0x08, false, v, n); return;   /* TEST */
    case 0x28: case 0x18: if (command && !n) send_mcc(s, type, false, NULL, 0); return;   /* FCON / FCOFF */
    case 0x04: return;                                                     /* NSC */
    default: if (command) { uint8_t r = data[0]; send_mcc(s, 0x04, false, &r, 1); }
    }
}
static void deliver(struct rc_dlc* d, const uint8_t* data, unsigned length) {
    struct rcsock* k = d->sock; if (!k || k->shut_rd || !length) return;
    if (k->rx.bytes + length > RX_LIMIT) return;
    struct bt_pkt* p = bt_pkt_new(length); if (!p) return;
    memcpy(p->data, data, length); bt_queue_put(&k->rx, p); bt_wake(k);
}
static void give_credits(struct rc_dlc* d) {
    if (!d->cfc || d->state != D_CONNECTED || !d->sock) return;
    unsigned queued = d->sock->rx.count;
    if (d->rx_credits < 4 && queued < 16) { unsigned more = 7 - d->rx_credits; d->rx_credits += more; send_frame(d->session, d->dlci, 0xFF, true, NULL, 0, (int)more); }
}
static void l2_recv(void* owner, const uint8_t* f, unsigned length) {
    struct rc_session* s = owner;
    if (length < 4 || !(f[0] & 1)) return;
    uint8_t dlci = f[0] >> 2, control = f[1], type = control & 0xEF; bool pf = control & 0x10;
    unsigned at = 2, n;
    if (f[2] & 1) { n = f[2] >> 1; at = 3; } else { if (length < 5) return; n = (f[2] >> 1) | (unsigned)f[3] << 7; at = 4; }
    unsigned credits = 0; struct rc_dlc* d = dlci ? dlc_find(s, dlci) : NULL;
    if (type == 0xEF && pf && d && d->cfc) { if (at >= length - 1) return; credits = f[at++]; }   /* credit byte only with CFC (Linux) */
    if (at + 1 > length) return;
    n = length - at - 1;                                                   /* the payload is what lies before the FCS: the length field is not trusted (Linux) */
    if (!fcs_ok(f, type == 0xEF ? 2 : at, f[length - 1])) return;      /* UIH: address + control; others with the length */
    const uint8_t* data = f + at;
    switch (type) {
    case 0x2F: incoming_sabm(s, dlci); return;                             /* SABM */
    case 0x63:                                                             /* UA */
        if (!dlci) { if (!s->open) { s->open = true; s->deadline = 0; for (struct rc_dlc* x = s->dlcs; x; x = x->next) if (!x->state) dlc_start(x); } else if (s->closing) {} return; }
        if (!d) return;
        if (d->state == D_SABM) { dlc_connected(d); return; }
        if (d->state == D_DISC) { dlc_free(d, 0); return; }
        return;
    case 0x0F:                                                             /* DM */
        if (!dlci) { session_close(s, E_CONNREFUSED); return; }
        if (d) dlc_free(d, E_CONNREFUSED);
        return;
    case 0x43:                                                             /* DISC */
        send_frame(s, dlci, 0x73, false, NULL, 0, -1);
        if (!dlci) { session_close(s, E_CONNRESET); return; }
        if (d) dlc_free(d, E_CONNRESET);
        return;
    case 0xEF:                                                             /* UIH */
        if (!dlci) { mcc(s, data, n); return; }
        if (!d || d->state != D_CONNECTED) { if (!d) send_frame(s, dlci, 0x1F, false, NULL, 0, -1); return; }
        if (credits) d->tx_credits = d->tx_credits + credits > 255 ? 255 : d->tx_credits + credits;
        if (d->sock) bt_wake(d->sock);
        if (!n) return;
        if (n > d->mtu) return;
        if (d->cfc && d->rx_credits) d->rx_credits--;                      /* beyond the credits: kept as Linux does (bounded by RX_LIMIT) */
        deliver(d, data, n); give_credits(d); return;
    }
}
static void l2_state(void* owner, int state, int error) {
    struct rc_session* s = owner;
    if (state == CHAN_CONNECTED) {
        struct bt_conn* c = l2cap_chan_conn(s->chan); s->mtu = s->chan ? (uint16_t)(l2cap_chan_omtu(s->chan) > 1013 ? 1008 : l2cap_chan_omtu(s->chan) - 5) : DEFAULT_MTU;
        (void)c;
        if (s->initiator) { s->deadline = bt_now() + 20000000000ULL; send_frame(s, 0, 0x3F, true, NULL, 0, -1); }   /* SABM on DLCI 0 */
        return;
    }
    if (state == CHAN_CLOSED) { s->chan = NULL; if (!s->closing) session_close(s, error ? error : E_CONNRESET); }
}
static void* session_accept(struct l2cap_chan* ch) {
    struct bt_conn* c = l2cap_chan_conn(ch); if (!c) return NULL;
    struct rc_session* s = bt_zalloc(sizeof(*s)); if (!s) return NULL;
    s->chan = ch; s->initiator = false; memcpy(s->peer, c->peer, 6); s->dev_index = c->dev->index; s->dev_gen = c->dev->generation; s->mtu = DEFAULT_MTU;
    s->next = sessions; sessions = s; l2cap_kernel_attach(ch, s); return s;
}
void rfcomm_init(void) {
    for (unsigned i = 0; i < 256; i++) { uint8_t r = (uint8_t)i; for (unsigned b = 0; b < 8; b++) r = (r & 1) ? (uint8_t)((r >> 1) ^ 0xE0) : (uint8_t)(r >> 1); crc_table[i] = r; }
    l2cap_kernel_listen(RFCOMM_PSM, &session_ops, session_accept);
}
/* security changes reach DLCs waiting for them (called from the poll and after sends) */
static void security_check(void) {
    for (struct rc_session* s = sessions; s; s = s->next) for (struct rc_dlc *d = s->dlcs, *next; d; d = next) {
        next = d->next;
        if (d->state != D_SECURITY) continue;
        struct bt_conn* c = l2cap_chan_conn(s->chan); if (!c) continue;
        if (c->encrypted && c->sec_level >= d->sec_level) { d->security_wait = false; if (d->initiator) dlc_sabm(d); else { send_frame(s, d->dlci, 0x73, false, NULL, 0, -1); dlc_connected(d); } }
        else if (!c->auth_pending && !c->enc_pending && !c->smp && c->sec_level < d->sec_level && d->deadline && d->deadline - bt_now() < 38000000000ULL) {
            if (!d->initiator) send_frame(s, d->dlci, 0x1F, false, NULL, 0, -1);
            dlc_free(d, E_ACCES); next = s->dlcs;
        }
    }
}
void rfcomm_poll(uint64_t now) {
    security_check();
    for (struct rc_session *s = sessions, *ns; s; s = ns) {
        ns = s->next;
        if (s->deadline && now >= s->deadline) { s->deadline = 0; session_close(s, E_TIMEDOUT); ns = sessions; continue; }
        if (s->idle_deadline && now >= s->idle_deadline && !s->dlcs) { s->idle_deadline = 0; if (s->open && s->initiator) send_frame(s, 0, 0x53, true, NULL, 0, -1); session_close(s, 0); ns = sessions; continue; }
        for (struct rc_dlc *d = s->dlcs, *nd; d; d = nd) {
            nd = d->next;
            if (!d->deadline || now < d->deadline) continue;
            d->deadline = 0;
            if (d->state != D_CONNECTED) { if (d->state == D_SECURITY && !d->initiator) send_frame(s, d->dlci, 0x1F, false, NULL, 0, -1); dlc_free(d, d->state == D_DISC ? 0 : E_TIMEDOUT); nd = s->dlcs; }
        }
    }
}
uint64_t rfcomm_next_event(uint64_t now) {
    uint64_t next = ~0ULL; bool waiting = false;
    for (struct rc_session* s = sessions; s; s = s->next) {
        if (s->deadline && s->deadline < next) next = s->deadline;
        if (s->idle_deadline && s->idle_deadline < next) next = s->idle_deadline;
        for (struct rc_dlc* d = s->dlcs; d; d = d->next) { if (d->deadline && d->deadline < next) next = d->deadline; waiting |= d->state == D_SECURITY; }
    }
    if (waiting && now + 50000000ULL < next) next = now + 50000000ULL;      /* security outcome polled */
    return next;
}

/* ---------- sockets ---------- */
struct sockaddr_rc { uint16_t family; uint8_t bdaddr[6]; uint8_t channel, pad; };
static int64_t sock_io(void* object, const struct iovec64* v, unsigned count, uint64_t total, bool write, uint64_t flags);
static int sock_ioctl(void* object, uint64_t command, uint64_t argument, uint64_t flags) {
    (void)flags; struct rcsock* k = object;
    if ((uint32_t)command == 0x541B) { uint32_t n = k->rx.bytes - k->rx_offset; return bt_to_user(argument, &n, 4); }
    if ((uint32_t)command == 0x5411) { uint32_t n = 0; return bt_to_user(argument, &n, 4); }
    return E_NOTTY;                                                        /* RFCOMM TTYs (RFCOMMCREATEDEV) are not offered */
}
static uint32_t sock_ready(void* object) {
    struct rcsock* k = object; uint32_t mask = 0;
    if (k->listening) { for (struct rcsock* c = k->accept_head; c; c = c->accept_next) if (!c->dlc || c->dlc->state == D_CONNECTED) mask |= 1U | 64U; return mask; }
    if (k->rx.count) mask |= 1U | 64U;
    if (k->dlc && k->dlc->state == D_CONNECTED && (!k->dlc->cfc || k->dlc->tx_credits)) mask |= 4U | 256U;
    if (!k->dlc && (k->connected_once || k->error)) mask |= 1U | 4U | 16U | 8192U | (k->error && k->error != E_CONNRESET ? 8U : 0);   /* POLLOUT too, as Linux */
    return mask;
}
static void* sock_source(void* object) { return object; }
static void sock_release(void* object);
static void sock_stat(void* object, struct linux_stat* stat) { memset(stat, 0, sizeof(*stat)); stat->dev = 4; stat->ino = 0x62000000ULL + ((struct rcsock*)object)->id; stat->nlink = 1; stat->mode = 0140777; stat->block_size = 4096; }
static const struct object_operations sock_operations = {sock_io, sock_ioctl, sock_ready, sock_source, sock_release, sock_stat};
bool rfcomm_descriptor(struct open_file* f) { return fd_object(f, &sock_operations) != NULL; }
static struct rcsock* sock_alloc(void) {
    unsigned slot = 0; while (slot < 64 && socks[slot]) slot++;
    if (slot == 64) return NULL;
    struct rcsock* k = bt_zalloc(sizeof(*k)); if (!k) return NULL;
    k->id = ++sock_ids; k->sec_level = SEC_LOW; socks[slot] = k; live++; return k;
}
static void sock_free(struct rcsock* k) { for (unsigned i = 0; i < 64; i++) if (socks[i] == k) socks[i] = NULL; bt_queue_purge(&k->rx); live--; bt_free(k, sizeof(*k)); }
static void sock_close_dlc(struct rcsock* k) {
    struct rc_dlc* d = k->dlc; if (!d) return;
    k->dlc = NULL; d->sock = NULL;
    if (d->state == D_CONNECTED || d->state == D_SABM) { d->state = D_DISC; d->deadline = bt_now() + 2000000000ULL; send_frame(d->session, d->dlci, 0x53, true, NULL, 0, -1); }
    else { if (d->state == D_SECURITY && !d->initiator) send_frame(d->session, d->dlci, 0x1F, false, NULL, 0, -1); dlc_free(d, 0); }
}
static void sock_release(void* object) {
    struct rcsock* k = object;
    while (k->accept_head) { struct rcsock* c = k->accept_head; k->accept_head = c->accept_next; c->parent = NULL; sock_close_dlc(c); sock_free(c); }
    if (k->parent) for (struct rcsock** p = &k->parent->accept_head; *p; p = &(*p)->accept_next) if (*p == k) { *p = k->accept_next; k->parent->accept_count--; break; }
    sock_close_dlc(k); sock_free(k);
}
int64_t rfcomm_socket_create(uint64_t type, uint64_t protocol) {
    (void)protocol;
    if (type & ~(0xFULL | 0x800 | 0x80000)) return E_INVAL;
    if ((type & 0xF) != 1) return (type & 0xF) == 3 ? E_PROTONOSUPPORT : E_SOCKTNOSUPPORT;   /* SOCK_RAW (TTY control): not offered */
    struct rcsock* k = sock_alloc(); if (!k) return E_NOMEM;
    int fd = fd_create_object(&sock_operations, k, (type & 0x800) | (type & 0x80000) | 2, "socket:[rfcomm]");
    if (fd < 0) sock_free(k);
    return fd;
}
static int sock_connect(struct rcsock* k, const struct sockaddr_rc* a, uint64_t flags) {
    if (k->listening) return E_INVAL;
    if (k->dlc) return k->dlc->state == D_CONNECTED ? E_ISCONN : E_ALREADY;
    if (k->connected_once) return E_ISCONN;
    if (a->family != 31 || !a->channel || a->channel > 30 || bt_addr_any(a->bdaddr)) return E_INVAL;
    struct bt_dev* dev = bt_dev_route(k->bound ? k->src : NULL, false); if (!dev) return E_HOSTUNREACH;
    struct rc_session* s = session_for(a->bdaddr, dev->index);
    if (!s) {
        s = bt_zalloc(sizeof(*s)); if (!s) return E_NOMEM;
        s->initiator = true; memcpy(s->peer, a->bdaddr, 6); s->dev_index = dev->index; s->dev_gen = dev->generation; s->mtu = DEFAULT_MTU;
        s->next = sessions; sessions = s;
        int error = 0; s->chan = l2cap_kernel_connect(dev, a->bdaddr, RFCOMM_PSM, SEC_LOW, &session_ops, s, &error);
        if (!s->chan) { session_free(s); return error ? error : E_HOSTUNREACH; }
    }
    uint8_t dlci = (uint8_t)(a->channel << 1 | (s->initiator ? 0 : 1));
    if (dlc_find(s, dlci)) return E_BUSY;
    struct rc_dlc* d = dlc_new(s, dlci, true); if (!d) return E_NOMEM;
    d->sock = k; k->dlc = d; d->sec_level = k->sec_level; memcpy(k->dst, a->bdaddr, 6); k->dchannel = a->channel;
    if (!k->bound) { memcpy(k->src, dev->bdaddr, 6); k->bound = true; }
    if (s->open) dlc_start(d);
    for (;;) {
        if (k->dlc && k->dlc->state == D_CONNECTED) return 0;
        if (!k->dlc) { int e = k->error ? k->error : E_CONNREFUSED; k->error = 0; return e; }
        if (flags & 0x800) return E_INPROGRESS;
        int e = task_wait_until(k, bt_now() + 50000000ULL); if (e == E_INTR) return e;
        security_check();
    }
}
static int64_t sock_send(struct rcsock* k, const uint8_t* data, unsigned length, uint64_t flags) {
    unsigned at = 0;
    while (at < length) {
        struct rc_dlc* d = k->dlc;
        if (k->shut_wr) return at ? (int64_t)at : E_PIPE;
        if (!d) return at ? (int64_t)at : (k->connected_once ? E_PIPE : E_NOTCONN);
        if (d->state != D_CONNECTED || (d->cfc && !d->tx_credits)) {
            if (flags & 0x800) return at ? (int64_t)at : E_AGAIN;
            int e = task_wait_until(k, bt_now() + 100000000ULL); if (e == E_INTR) return at ? (int64_t)at : e;
            continue;
        }
        unsigned n = length - at < d->mtu ? length - at : d->mtu;
        int give = -1;
        if (d->cfc && d->rx_credits < 4) { give = (int)(7 - d->rx_credits); d->rx_credits = 7; }
        int r = send_frame(d->session, d->dlci, give >= 0 ? 0xFF : 0xEF, true, data + at, n, give);
        if (r == E_AGAIN) { if (give >= 0) d->rx_credits -= (unsigned)give; if (flags & 0x800) return at ? (int64_t)at : E_AGAIN; task_wait_until(k, bt_now() + 20000000ULL); continue; }
        if (r < 0) return at ? (int64_t)at : r;
        if (d->cfc) d->tx_credits--;
        at += n;
    }
    return at;
}
static int64_t sock_recv(struct rcsock* k, const struct iovec64* v, unsigned count, uint64_t total, uint64_t flags) {
    for (;;) {
        if (k->rx.head) {
            uint8_t buffer[4096]; unsigned n = 0;
            for (struct bt_pkt* q = k->rx.head; q && n < total && n < sizeof(buffer);) {
                unsigned off = q == k->rx.head ? k->rx_offset : 0, take = q->len - off;
                if (take > total - n) take = (unsigned)(total - n);
                if (take > sizeof(buffer) - n) take = sizeof(buffer) - n;
                memcpy(buffer + n, q->data + off, take); n += take; q = q->next;
            }
            int64_t copied = bt_copy_out_vectors(v, count, buffer, n); if (copied < 0) return copied;
            if (!(flags & BT_PEEK)) for (unsigned left = n; left;) {
                struct bt_pkt* q = k->rx.head; unsigned avail = q->len - k->rx_offset;
                if (left >= avail) { left -= avail; k->rx_offset = 0; bt_pkt_free(bt_queue_take(&k->rx)); } else { k->rx_offset += left; left = 0; }
            }
            if (k->dlc) give_credits(k->dlc);
            return copied;
        }
        if (k->shut_rd || (!k->dlc && k->eof)) { if (k->error && k->error != E_CONNRESET) { int e = k->error; k->error = 0; return e; } return 0; }
        if (!k->dlc) return k->connected_once ? 0 : E_NOTCONN;
        if (flags & 0x800) return E_AGAIN;
        int e = task_wait_channel(k); if (e) return e;
    }
}
static int64_t sock_io(void* object, const struct iovec64* v, unsigned count, uint64_t total, bool write, uint64_t flags) {
    struct rcsock* k = object;
    if (k->listening) return E_NOTCONN;
    if (write) {
        if (total > 65536) total = 65536;
        uint8_t* buffer = bt_zalloc(total ? total : 1); if (!buffer) return E_NOMEM;
        int64_t n = 0; unsigned at = 0;
        for (unsigned i = 0; i < count && at < total; i++) { unsigned take = v[i].size < total - at ? (unsigned)v[i].size : (unsigned)(total - at); if (take && bt_from_user(buffer + at, v[i].address, take)) { n = E_FAULT; break; } at += take; }
        if (n >= 0) n = sock_send(k, buffer, at, flags);
        bt_free(buffer, total ? total : 1); return n;
    }
    return sock_recv(k, v, count, total, flags);
}
static int put_addr(uint64_t address, uint64_t size_address, const uint8_t* bdaddr, uint8_t channel) {
    if (!address) return 0;
    struct sockaddr_rc a = {31, {0}, channel, 0}; memcpy(a.bdaddr, bdaddr, 6);
    uint32_t size; if (bt_from_user(&size, size_address, 4)) return E_FAULT;
    if ((int32_t)size < 0) return E_INVAL;
    if (bt_to_user(address, &a, size < 10 ? size : 10)) return E_FAULT;
    uint32_t full = 10; return bt_to_user(size_address, &full, 4);
}
static int64_t sock_accept(struct rcsock* k, uint64_t address, uint64_t size_address, uint64_t accept_flags, uint64_t flags) {
    if (!k->listening) return E_INVAL;
    if (accept_flags & ~(0x800ULL | 0x80000ULL)) return E_INVAL;
    for (;;) {
        for (struct rcsock** p = &k->accept_head; *p; p = &(*p)->accept_next) {
            struct rcsock* c = *p;
            if (c->dlc && c->dlc->state != D_CONNECTED) continue;
            *p = c->accept_next; k->accept_count--; c->parent = NULL; c->accept_next = NULL;
            int fd = fd_create_object(&sock_operations, c, (accept_flags & 0x800) | (accept_flags & 0x80000) | 2, "socket:[rfcomm]");
            if (fd < 0) { sock_close_dlc(c); sock_free(c); return fd; }
            put_addr(address, size_address, c->dst, c->dchannel); return fd;
        }
        if (flags & 0x800) return E_AGAIN;
        int e = task_wait_until(k, bt_now() + 100000000ULL); if (e == E_INTR) return e;
        security_check();
    }
}
static uint8_t lm_level(uint32_t lm) { return lm & 0x40 ? SEC_FIPS : lm & 0x20 ? SEC_HIGH : lm & (0x02 | 0x04) ? SEC_MEDIUM : SEC_LOW; }
int64_t rfcomm_call(struct registers* r, struct open_file* f) {
    struct rcsock* k = fd_object(f, &sock_operations); if (!k) return -88;
    uint64_t flags = fd_status_flags(f); int64_t result = 0;
    fd_pin(f);
    switch (r->rax) {
    case 49: {
        struct sockaddr_rc a = {0}; if (r->rdx < 9) { result = E_INVAL; break; }
        if (bt_from_user(&a, r->rsi, r->rdx < 10 ? r->rdx : 10)) { result = E_FAULT; break; }
        if (a.family != 31 || a.channel > 30 || k->bound) { result = E_INVAL; break; }
        for (unsigned i = 0; i < 64 && a.channel; i++) { struct rcsock* o = socks[i]; if (o && o != k && o->bound && !o->parent && o->channel == a.channel && (o->listening || !o->dlc) && (bt_addr_any(o->src) || bt_addr_any(a.bdaddr) || !memcmp(o->src, a.bdaddr, 6))) { result = E_ADDRINUSE; break; } }
        if (result) break;
        memcpy(k->src, a.bdaddr, 6); k->channel = a.channel; k->bound = true; break;
    }
    case 50:
        if (!k->bound || k->dlc) { result = E_INVAL; break; }
        if (!k->channel) {                                                 /* channel 0: the first free one */
            for (uint8_t ch = 1; ch <= 30 && !k->channel; ch++) { bool used = false; for (unsigned i = 0; i < 64; i++) if (socks[i] && socks[i]->listening && socks[i]->channel == ch) used = true; if (!used) k->channel = ch; }
            if (!k->channel) { result = E_ADDRINUSE; break; }
        }
        k->listening = true; k->backlog = (unsigned)(r->rsi > 32 ? 32 : r->rsi ? r->rsi : 1); break;
    case 42: { struct sockaddr_rc a = {0}; if (r->rdx < 9) { result = E_INVAL; break; } if (bt_from_user(&a, r->rsi, r->rdx < 10 ? r->rdx : 10)) { result = E_FAULT; break; } result = sock_connect(k, &a, flags); break; }
    case 43: result = sock_accept(k, r->rsi, r->rdx, 0, flags); break;
    case 288: result = sock_accept(k, r->rsi, r->rdx, r->r10, flags); break;
    case 51: result = put_addr(r->rsi, r->rdx, k->src, k->channel); break;
    case 52: if (!k->dlc && !k->connected_once) { result = E_NOTCONN; break; } result = put_addr(r->rsi, r->rdx, k->dst, k->dchannel); break;
    case 44: { struct iovec64 v = {r->rsi, r->rdx}; result = sock_io(k, &v, 1, r->rdx, true, flags | (r->r10 & 0x40 ? 0x800 : 0)); break; }
    case 45: { struct iovec64 v = {r->rsi, r->rdx}; result = sock_io(k, &v, 1, r->rdx, false, flags | (r->r10 & 0x40 ? 0x800 : 0) | (r->r10 & 2 ? BT_PEEK : 0)); break; }
    case 46: case 47: {
        struct message64 m; struct iovec64 v[64]; unsigned count; uint64_t total;
        int e = bt_read_message(r->rsi, &m, v, &count, &total); if (e) { result = e; break; }
        result = sock_io(k, v, count, total, r->rax == 46, flags | (r->rdx & 0x40 ? 0x800 : 0) | (r->rdx & 2 ? BT_PEEK : 0));
        if (r->rax == 47 && result >= 0) { m.flags = 0; m.control_size = 0; m.name_size = 0; if (bt_to_user(r->rsi, &m, sizeof(m))) result = E_FAULT; }
        break;
    }
    case 48:
        if (r->rsi > 2) { result = E_INVAL; break; }
        if (!k->dlc && !k->listening && !k->connected_once) { result = E_NOTCONN; break; }
        if (r->rsi != 1) k->shut_rd = true;
        if (r->rsi != 0) { k->shut_wr = true; sock_close_dlc(k); }
        bt_wake(k); break;
    case 54: {
        uint32_t v = 0;
        if (r->rsi == 18 && r->rdx == 3) { if (bt_set_int_option(r->r10, r->r8, &v)) { result = E_FAULT; break; } k->sec_level = lm_level(v); if (k->dlc) k->dlc->sec_level = k->sec_level; break; }   /* RFCOMM_LM */
        if (r->rsi == 274 && r->rdx == 4) { uint8_t o[2] = {0}; if (r->r8 < 1 || bt_from_user(o, r->r10, r->r8 < 2 ? r->r8 : 2)) { result = E_INVAL; break; } if (o[0] > SEC_FIPS) { result = E_INVAL; break; } k->sec_level = o[0] < SEC_LOW ? SEC_LOW : o[0]; if (k->dlc) k->dlc->sec_level = k->sec_level; break; }
        if (r->rsi == 274 && r->rdx == 7) { if (bt_set_int_option(r->r10, r->r8, &v)) { result = E_FAULT; break; } k->defer = v != 0; break; }
        if (r->rsi == 1) break;                                            /* SOL_SOCKET options: accepted */
        result = E_NOPROTOOPT; break;
    }
    case 55: {
        if (r->rsi == 1) { int e = k->dlc ? 0 : (k->connected_once ? 0 : k->error); if (r->rdx == 4 && !k->dlc) k->error = 0; result = bt_sol_socket_get(r->rdx, r->r10, r->r8, 1, 3, e, RX_LIMIT, 65536); break; }
        if (r->rsi == 18 && r->rdx == 3) { uint32_t lm = k->sec_level >= SEC_FIPS ? 0x66 : k->sec_level == SEC_HIGH ? 0x26 : k->sec_level == SEC_MEDIUM ? 0x06 : 0; result = bt_get_int_option(r->r10, r->r8, lm); break; }
        if (r->rsi == 18 && r->rdx == 2) {                                 /* RFCOMM_CONNINFO */
            struct bt_conn* c = k->dlc && k->dlc->session->chan ? l2cap_chan_conn(k->dlc->session->chan) : NULL;
            if (!c || k->dlc->state != D_CONNECTED) { result = E_NOTCONN; break; }
            uint8_t o[6] = {0}; bt_put16(o, c->handle); memcpy(o + 2, c->dev_class, 3); result = bt_get_bytes_option(r->r10, r->r8, o, 6); break;
        }
        if (r->rsi == 274 && r->rdx == 4) { uint8_t o[2] = {k->sec_level, 16}; result = bt_get_bytes_option(r->r10, r->r8, o, 2); break; }
        if (r->rsi == 274 && r->rdx == 7) { result = bt_get_int_option(r->r10, r->r8, k->defer); break; }
        result = E_NOPROTOOPT; break;
    }
    default: result = E_OPNOTSUPP;
    }
    fd_unpin(f);
    return result;
}
