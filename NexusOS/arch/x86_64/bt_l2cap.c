/* L2CAP (Core Vol 3 Part A) over the core's ACL links, and BTPROTO_L2CAP sockets (Linux ABI, struct sockaddr_l2).
 *  - BR/EDR: signalling on CID 1 (connection, configuration in basic mode - MTU/flush/QoS/FCS/RFC options, an ERTM or
 *    streaming request is answered with basic mode -, disconnection, echo, information: feature mask = fixed channels,
 *    fixed channels = signalling), dynamic channels from CID 0x0040.
 *  - LE: signalling on CID 5 (LE credit-based connections with SDU segmentation and credit flow control, connection
 *    parameter updates answered as central, disconnection), the fixed ATT channel (CID 4: listening sockets get one
 *    child per LE link, unless a client ATT socket owns it), SMP on CID 6 (bt_smp.c).
 *  - Security: a channel needs its socket's BT_SECURITY level before it connects; incoming requests below the
 *    listener's level answer "pending" and continue after authentication/encryption (or "security block").
 *  - Sockets: SOCK_SEQPACKET (message boundaries), SOCK_STREAM, SOCK_RAW (signalling frames, as l2ping uses);
 *    bind/listen/accept/connect (blocking or EINPROGRESS)/send/recv/shutdown/poll, BT_DEFER_SETUP (the first read
 *    accepts), options L2CAP_OPTIONS/LM/CONNINFO, BT_SECURITY/DEFER_SETUP/FLUSHABLE/POWER/CHANNEL_POLICY/SNDMTU/RCVMTU/
 *    MODE/PHY. PSMs below 0x1001 (LE: SPSM below 0x80) need CAP_NET_BIND_SERVICE.
 * Malformed signalling is answered with Command Reject; frames beyond the MTU are dropped; every timeout (connect
 * 40 s, configure 20 s, disconnect 2 s) ends in a closed channel. Behaviour follows Linux where peers depend on it:
 * identifiers 1-128, the BR/EDR feature-mask exchange before the first channel (incoming requests answered "pending"
 * meanwhile), a successful authentication lets waiting channels go on at the level reached, LE K-frames are checked
 * against the IMTU (not our MPS) and malformed SDUs are dropped without a disconnect. */
#include "bt.h"
#include "../../kernel/string.h"

#define L2CAP_DEFAULT_MTU 672
#define L2CAP_LE_MIN_MTU 23
#define RX_LIMIT (256U * 1024U)
struct l2sock;
struct l2cap_chan {
    struct l2cap_chan *next, *all_next; uint32_t id; uint8_t state; bool le_coc, ecred, fixed, raw, initiator;   /* ecred: LE enhanced credit based (Ext-Flowctl), data as le_coc */
    uint16_t psm, scid, dcid, imtu, omtu, mps, peer_mps, tx_credits, rx_credits; uint8_t ident, conf, conf_tries; uint8_t sec_level;
    uint32_t conn_id; struct bt_conn* conn; uint64_t deadline, connect_timeout; int error; bool defer, deferred, pending_rsp, await_info;
    struct l2sock* sock; const struct l2cap_ops* kops; void* kowner;
    uint8_t* sdu; uint32_t sdu_len, sdu_have; struct bt_queue txq;        /* LE CoC: reassembly and frames waiting for credits */
    uint8_t peer[6], peer_type; int dev_index; uint32_t dev_gen;
};
enum { CONF_IN = 1, CONF_OUT = 2, CONF_SENT = 4 };
struct l2sock {
    uint64_t id; int type; bool bound, listening, deferred_accept; uint8_t src[6], src_type; uint16_t psm, cid;
    uint8_t dst[6], dst_type; uint16_t dpsm, dcid; struct l2cap_chan* chan;
    struct l2sock *parent, *accept_head, *accept_next; unsigned backlog, accept_count;
    struct bt_queue rx; uint32_t rx_offset; int error; bool shut_rd, shut_wr, eof, connected_once;
    uint8_t sec_level; bool defer, kernel; uint16_t imtu, omtu; uint8_t mode; bool nonblock_connect;
    uint64_t sndtimeo, rcvtimeo; bool closed;                                          /* SO_SNDTIMEO (also the connect timer, as Linux) / SO_RCVTIMEO, ns; 0 = none */
};
static struct l2cap_chan* chans;
static uint32_t chan_ids; static uint64_t sock_ids; static unsigned live_socks;
static struct l2sock* socks[64];
struct kernel_listener { uint16_t psm; const struct l2cap_ops* ops; void* (*accept)(struct l2cap_chan*); };
static struct kernel_listener klisten[4];

unsigned l2cap_live(void) { unsigned n = live_socks; for (struct l2cap_chan* c = chans; c; c = c->all_next) n++; return n; }
static struct l2cap_chan* chan_get(uint32_t id) { for (struct l2cap_chan* c = chans; c; c = c->all_next) if (c->id == id) return c; return NULL; }
static void sock_wake(struct l2sock* s) { if (s) { bt_wake(s); if (s->parent) bt_wake(s->parent); } }
struct bt_conn* l2cap_chan_conn(struct l2cap_chan* ch) { return ch && ch->conn && bt_conn_get(ch->conn_id) == ch->conn ? ch->conn : NULL; }
uint16_t l2cap_chan_omtu(struct l2cap_chan* ch) { return ch->omtu; }
uint8_t l2cap_chan_sec(struct l2cap_chan* ch) { struct bt_conn* c = l2cap_chan_conn(ch); return c ? c->sec_level : 0; }

/* ---------- channels ---------- */
static uint16_t free_cid(struct bt_conn* c, bool le) {
    for (uint16_t cid = 0x0040; cid < (le ? 0x0080 : 0xFFFF); cid++) {
        bool used = false; for (struct l2cap_chan* ch = c->chans; ch && !used; ch = ch->next) used = ch->scid == cid;
        if (!used) return cid;
    }
    return 0;
}
static struct l2cap_chan* chan_new(void) {
    struct l2cap_chan* ch = bt_zalloc(sizeof(*ch)); if (!ch) return NULL;
    ch->id = ++chan_ids; if (!ch->id) ch->id = ++chan_ids; ch->imtu = L2CAP_DEFAULT_MTU; ch->omtu = L2CAP_DEFAULT_MTU; ch->sec_level = SEC_LOW;
    ch->all_next = chans; chans = ch; return ch;
}
static void chan_attach(struct l2cap_chan* ch, struct bt_conn* c) {
    ch->conn = c; ch->conn_id = c->id; ch->next = c->chans; c->chans = ch; memcpy(ch->peer, c->peer, 6); ch->peer_type = c->peer_type;
    ch->dev_index = c->dev->index; ch->dev_gen = c->dev->generation; bt_conn_busy(c);
}
static void chan_detach(struct l2cap_chan* ch) {
    struct bt_conn* c = l2cap_chan_conn(ch);
    if (c) { for (struct l2cap_chan** p = &c->chans; *p; p = &(*p)->next) if (*p == ch) { *p = ch->next; break; } if (!c->chans) bt_conn_idle(c); }
    ch->conn = NULL; ch->conn_id = 0;
}
static void chan_free(struct l2cap_chan* ch) {
    chan_detach(ch);
    for (struct l2cap_chan** p = &chans; *p; p = &(*p)->all_next) if (*p == ch) { *p = ch->all_next; break; }
    if (ch->sdu) bt_free(ch->sdu, ch->sdu_len ? ch->sdu_len : 1);
    bt_queue_purge(&ch->txq); bt_free(ch, sizeof(*ch));
}
static void sock_free(struct l2sock* s);
/* The channel ended: its owner learns why; a socket keeps its data (EOF after it), a kernel owner gets the state. */
static void chan_closed(struct l2cap_chan* ch, int error) {
    ch->state = CHAN_CLOSED; ch->error = error;
    struct l2sock* s = ch->sock;
    if (s && s->parent && !s->connected_once) {                           /* never accepted: it leaves the listener's queue */
        struct l2sock* parent = s->parent;
        for (struct l2sock** p = &parent->accept_head; *p; p = &(*p)->accept_next) if (*p == s) { *p = s->accept_next; parent->accept_count--; break; }
        ch->sock = NULL; s->chan = NULL; sock_free(s); s = NULL;
    }
    if (s) { s->chan = NULL; s->eof = true; s->closed = true; if (error) s->error = error; else if (!s->connected_once) s->error = E_CONNREFUSED; sock_wake(s); ch->sock = NULL; }
    if (ch->kops) { const struct l2cap_ops* ops = ch->kops; void* owner = ch->kowner; ch->kops = NULL; ops->state(owner, CHAN_CLOSED, error); }
    chan_free(ch);
}
static uint8_t next_ident(struct bt_conn* c) { if (++c->ident > 128 || !c->ident) c->ident = 1; return c->ident; }   /* 1-128 (Linux) */
static void signal(struct bt_conn* c, uint8_t code, uint8_t ident, const uint8_t* data, unsigned length) {
    uint8_t head[4] = {code, ident, (uint8_t)length, (uint8_t)(length >> 8)};
    bt_acl_send2(c, c->link == LINK_LE ? L2CAP_CID_LE_SIGNAL : L2CAP_CID_SIGNAL, head, 4, data, length);
}
/* BR/EDR: the first dynamic channel waits for the peer's feature mask (and fixed channels when it has them), as
 * Linux does: an incoming request is answered "pending, no further information" meanwhile; 4 s timeout. */
static void request_info(struct bt_conn* c, uint16_t type) {
    uint8_t r[2]; bt_put16(r, type); c->info_ident = next_ident(c); c->info_sent = true; c->info_deadline = bt_now() + 4000000000ULL;
    signal(c, 0x0A, c->info_ident, r, 2);
}
static bool info_ready(struct bt_conn* c) {
    if (c->link == LINK_LE || c->info_done) return true;
    if (!c->info_sent) request_info(c, 0x0002);
    return false;
}
static void reject(struct bt_conn* c, uint8_t ident, uint16_t reason, const uint8_t* data, unsigned length) {
    uint8_t r[6]; bt_put16(r, reason); if (length) memcpy(r + 2, data, length); signal(c, 0x01, ident, r, 2 + length);
}
static void send_disconnect(struct l2cap_chan* ch) {
    struct bt_conn* c = l2cap_chan_conn(ch);
    if (!c || ch->fixed || ch->raw) { chan_closed(ch, 0); return; }
    uint8_t r[4]; bt_put16(r, ch->dcid); bt_put16(r + 2, ch->scid); ch->ident = next_ident(c);
    ch->state = CHAN_DISCONN; ch->deadline = bt_now() + 2000000000ULL; signal(c, 0x06, ch->ident, r, 4);
}
static void send_config(struct l2cap_chan* ch) {
    struct bt_conn* c = l2cap_chan_conn(ch); if (!c) return;
    uint8_t r[8]; bt_put16(r, ch->dcid); bt_put16(r + 2, 0); unsigned n = 4;
    if (ch->imtu != L2CAP_DEFAULT_MTU) { r[4] = 0x01; r[5] = 2; bt_put16(r + 6, ch->imtu); n = 8; }
    ch->ident = next_ident(c); ch->conf |= CONF_SENT; ch->deadline = bt_now() + 20000000000ULL; signal(c, 0x04, ch->ident, r, n);
}
static void chan_ready(struct l2cap_chan* ch) {
    ch->state = CHAN_CONNECTED; ch->deadline = 0;
    if (ch->sock) { ch->sock->connected_once = true; ch->sock->error = 0; sock_wake(ch->sock); if (ch->sock->parent) bt_wake(ch->sock->parent); }
    if (ch->kops) ch->kops->state(ch->kowner, CHAN_CONNECTED, 0);
}
static void check_configured(struct l2cap_chan* ch) { if ((ch->conf & (CONF_IN | CONF_OUT)) == (CONF_IN | CONF_OUT) && ch->state == CHAN_CONFIG) chan_ready(ch); }

/* ---------- the listening side ---------- */
static struct l2sock* find_listener(struct bt_conn* c, uint16_t psm, uint16_t cid) {
    struct l2sock* any = NULL; bool le = c->link == LINK_LE;
    for (unsigned i = 0; i < 64; i++) {
        struct l2sock* s = socks[i];
        if (!s || !s->listening || (s->src_type != 0) != le || (psm && s->psm != psm) || (cid && s->cid != cid) || (!psm && !cid)) continue;
        if (!memcmp(s->src, c->dev->bdaddr, 6)) return s;
        if (bt_addr_any(s->src)) any = s;
    }
    return any;
}
static struct l2sock* sock_alloc(int type);
static void sock_free(struct l2sock* s);
/* A child socket for an incoming channel; NULL when the backlog is full. */
static struct l2sock* child_socket(struct l2sock* parent, struct l2cap_chan* ch, struct bt_conn* c) {
    if (parent->accept_count >= parent->backlog) return NULL;
    struct l2sock* s = sock_alloc(parent->type); if (!s) return NULL;
    s->parent = parent; s->bound = true; memcpy(s->src, c->dev->bdaddr, 6); s->src_type = parent->src_type; s->psm = parent->psm; s->cid = parent->cid;
    memcpy(s->dst, c->peer, 6); s->dst_type = c->peer_type; s->dpsm = parent->psm; s->sec_level = parent->sec_level; s->defer = parent->defer;
    s->imtu = parent->imtu; s->omtu = parent->omtu; s->chan = ch; ch->sock = s; ch->imtu = parent->imtu ? parent->imtu : L2CAP_DEFAULT_MTU; ch->sec_level = parent->sec_level;
    s->accept_next = NULL; struct l2sock** tail = &parent->accept_head; while (*tail) tail = &(*tail)->accept_next; *tail = s; parent->accept_count++;
    return s;
}
static void incoming_continue(struct l2cap_chan* ch, struct bt_conn* c);
static void connect_request(struct bt_conn* c, uint8_t ident, const uint8_t* q) {
    uint16_t psm = (uint16_t)bt_get16(q), scid = (uint16_t)bt_get16(q + 2); uint8_t r[8]; bt_put16(r, 0); bt_put16(r + 2, scid); bt_put16(r + 6, 0);
    struct l2sock* listener = find_listener(c, psm, 0); struct kernel_listener* kl = NULL;
    for (unsigned i = 0; i < 4 && !listener && !kl; i++) if (klisten[i].psm == psm && klisten[i].ops) kl = &klisten[i];
    unsigned result = 0;
    if (!listener && !kl) result = 0x0002;
    else if (scid < 0x0040) result = 0x0006;
    else { for (struct l2cap_chan* o = c->chans; o; o = o->next) if (o->dcid == scid && !o->fixed) result = 0x0007; }
    if (result) { bt_put16(r + 4, result); signal(c, 0x03, ident, r, 8); return; }
    struct l2cap_chan* ch = chan_new(); uint16_t cid = ch ? free_cid(c, false) : 0;
    if (!ch || !cid) { if (ch) chan_free(ch); bt_put16(r + 4, 0x0004); signal(c, 0x03, ident, r, 8); return; }
    ch->psm = psm; ch->scid = cid; ch->dcid = scid; chan_attach(ch, c);
    if (listener) { if (!child_socket(listener, ch, c)) { chan_free(ch); bt_put16(r + 4, 0x0004); signal(c, 0x03, ident, r, 8); return; } }
    else { ch->kops = kl->ops; ch->kowner = kl->accept(ch); ch->sec_level = SEC_LOW; if (!ch->kowner) { ch->kops = NULL; chan_free(ch); bt_put16(r + 4, 0x0004); signal(c, 0x03, ident, r, 8); return; } }
    bt_put16(r, cid); ch->ident = ident;
    if (!c->info_done) {                                                   /* feature mask first: "pending, no info" */
        ch->state = CHAN_CONNECT2; ch->await_info = true; ch->deadline = bt_now() + 40000000000ULL;
        bt_put16(r + 4, 0x0001); bt_put16(r + 6, 0x0000); signal(c, 0x03, ident, r, 8); info_ready(c); return;
    }
    incoming_continue(ch, c);
}
/* An accepted incoming BR/EDR request goes on: security, BT_DEFER_SETUP, else success and configuration. */
static void incoming_continue(struct l2cap_chan* ch, struct bt_conn* c) {
    uint8_t r[8]; bt_put16(r, ch->scid); bt_put16(r + 2, ch->dcid); bt_put16(r + 6, 0);
    if (!bt_conn_security(c, ch->sec_level, false)) {                     /* authentication pending */
        ch->state = CHAN_SECURITY; ch->pending_rsp = true; ch->deadline = bt_now() + 40000000000ULL;
        bt_put16(r + 4, 0x0001); bt_put16(r + 6, 0x0001); signal(c, 0x03, ch->ident, r, 8); return;
    }
    if (ch->sock && ch->sock->defer) {                                     /* BT_DEFER_SETUP: answered when the socket is read */
        ch->state = CHAN_CONNECT2; ch->deferred = true; ch->deadline = bt_now() + 40000000000ULL;
        bt_put16(r + 4, 0x0001); bt_put16(r + 6, 0x0002); signal(c, 0x03, ch->ident, r, 8); sock_wake(ch->sock); return;
    }
    bt_put16(r + 4, 0); signal(c, 0x03, ch->ident, r, 8); ch->state = CHAN_CONFIG; send_config(ch);
}
/* The feature-mask exchange ended (answer, reject or timeout): the channels that waited for it go on. */
static void start_outgoing(struct l2cap_chan* ch, struct bt_conn* c);
static void info_complete(struct bt_conn* c) {
    uint32_t id = c->id; c->info_done = true; c->info_deadline = 0;
    for (;;) {
        struct l2cap_chan* ch = c->chans; while (ch && !ch->await_info) ch = ch->next;
        if (!ch) return;
        ch->await_info = false;
        if (ch->initiator) start_outgoing(ch, c); else incoming_continue(ch, c);
        if (!(c = bt_conn_get(id))) return;
    }
}
static void accept_now(struct l2cap_chan* ch) {                            /* a pending incoming channel is answered "success" */
    struct bt_conn* c = l2cap_chan_conn(ch); if (!c) return;
    if (ch->ecred) {
        uint8_t r[10]; bt_put16(r, ch->imtu); bt_put16(r + 2, ch->mps); bt_put16(r + 4, ch->rx_credits); bt_put16(r + 6, 0); bt_put16(r + 8, ch->scid);
        signal(c, 0x18, ch->ident, r, 10); chan_ready(ch); return;
    }
    if (ch->le_coc) {
        uint8_t r[10]; bt_put16(r, ch->scid); bt_put16(r + 2, ch->imtu); bt_put16(r + 4, ch->mps); bt_put16(r + 6, ch->rx_credits); bt_put16(r + 8, 0);
        signal(c, 0x15, ch->ident, r, 10); chan_ready(ch); return;
    }
    uint8_t r[8]; bt_put16(r, ch->scid); bt_put16(r + 2, ch->dcid); bt_put16(r + 4, 0); bt_put16(r + 6, 0);
    signal(c, 0x03, ch->ident, r, 8); ch->state = CHAN_CONFIG; ch->pending_rsp = false; send_config(ch);
}
/* A pending incoming channel is refused (Linux l2cap_chan_*connect_reject): LE/Ext-Flowctl answer "insufficient
 * authorization" for a deferred one (the user did not accept it) with our MTU/MPS/credits; BR/EDR security block. */
static void refuse_pending(struct l2cap_chan* ch, uint16_t result) {
    struct bt_conn* c = l2cap_chan_conn(ch);
    if (c) {
        uint16_t le_result = ch->deferred ? 0x0006 : result == 0x0003 ? 0x0005 : result;
        if (ch->ecred) { uint8_t r[8]; bt_put16(r, ch->imtu); bt_put16(r + 2, ch->mps); bt_put16(r + 4, ch->rx_credits); bt_put16(r + 6, le_result); signal(c, 0x18, ch->ident, r, 8); }
        else if (ch->le_coc) { uint8_t r[10]; bt_put16(r, ch->scid); bt_put16(r + 2, ch->imtu); bt_put16(r + 4, ch->mps); bt_put16(r + 6, ch->rx_credits); bt_put16(r + 8, le_result); signal(c, 0x15, ch->ident, r, 10); }
        else { uint8_t r[8]; bt_put16(r, ch->scid); bt_put16(r + 2, ch->dcid); bt_put16(r + 4, result); bt_put16(r + 6, 0); signal(c, 0x03, ch->ident, r, 8); }
    }
    chan_closed(ch, E_ACCES);
}

/* ---------- outgoing ---------- */
static void start_outgoing(struct l2cap_chan* ch, struct bt_conn* c) {
    if (ch->fixed) { chan_ready(ch); return; }
    if (ch->raw) { chan_ready(ch); return; }
    uint64_t timeout = ch->connect_timeout ? ch->connect_timeout : 40000000000ULL;
    if (!ch->le_coc && !info_ready(c)) { ch->state = CHAN_CONNECT; ch->await_info = true; ch->deadline = bt_now() + timeout; return; }
    if (!bt_conn_security(c, ch->sec_level, true)) { ch->state = CHAN_SECURITY; ch->deadline = bt_now() + timeout; return; }
    ch->ident = next_ident(c); ch->deadline = bt_now() + timeout; ch->state = CHAN_CONNECT;
    if (ch->ecred) {                                                       /* one channel per Credit Based Connection Request */
        uint8_t r[10]; bt_put16(r, ch->psm); bt_put16(r + 2, ch->imtu); bt_put16(r + 4, ch->mps); bt_put16(r + 6, ch->rx_credits); bt_put16(r + 8, ch->scid);
        signal(c, 0x17, ch->ident, r, 10);
    } else if (ch->le_coc) {
        uint8_t r[10]; bt_put16(r, ch->psm); bt_put16(r + 2, ch->scid); bt_put16(r + 4, ch->imtu); bt_put16(r + 6, ch->mps); bt_put16(r + 8, ch->rx_credits);
        signal(c, 0x14, ch->ident, r, 10);
    } else { uint8_t r[4]; bt_put16(r, ch->psm); bt_put16(r + 2, ch->scid); signal(c, 0x02, ch->ident, r, 4); }
}
static struct l2cap_chan* chan_connect(struct bt_dev* d, const uint8_t* addr, uint8_t type, uint16_t psm, uint16_t cid, uint8_t sec, uint16_t imtu, bool raw, bool ecred, int* error) {
    struct bt_conn* c = bt_connect(d, addr, type, error); if (!c) return NULL;
    if (cid == L2CAP_CID_ATT) for (struct l2cap_chan* o = c->chans; o; o = o->next) if (o->fixed && o->scid == L2CAP_CID_ATT && o->initiator) { *error = E_BUSY; return NULL; }
    struct l2cap_chan* ch = chan_new(); if (!ch) { *error = E_NOMEM; return NULL; }
    ch->initiator = true; ch->psm = psm; ch->sec_level = sec; ch->imtu = imtu ? imtu : L2CAP_DEFAULT_MTU; ch->raw = raw;
    if (cid) { ch->fixed = true; ch->scid = ch->dcid = cid; ch->omtu = 23; }
    else if (!raw) {
        ch->scid = free_cid(c, type != 0); if (!ch->scid) { chan_free(ch); *error = E_NOMEM; return NULL; }
        if (type) { ch->le_coc = true; ch->ecred = ecred; unsigned minimum = ecred ? 64 : L2CAP_LE_MIN_MTU; if (ch->imtu < minimum) ch->imtu = (uint16_t)minimum;
                    unsigned acl = d->le_max ? d->le_mtu : d->acl_mtu; ch->mps = (uint16_t)(ch->imtu < acl - 4 ? ch->imtu : acl > 27 ? acl - 4 : 23); if (ch->mps < minimum) ch->mps = (uint16_t)minimum;
                    ch->rx_credits = (uint16_t)(ch->imtu / ch->mps + 1); }
    }
    if (cid == L2CAP_CID_ATT) {                                            /* the client's ATT channel replaces a server child */
        for (struct l2cap_chan* o = c->chans; o; o = o->next) if (o->fixed && o->scid == L2CAP_CID_ATT) { *error = E_BUSY; chan_free(ch); return NULL; }
    }
    chan_attach(ch, c); ch->state = CHAN_CONNECT; ch->deadline = bt_now() + 40000000000ULL;
    if (c->state == CONN_CONNECTED) start_outgoing(ch, c);
    return ch;
}

/* ---------- signalling ---------- */
static struct l2cap_chan* by_scid(struct bt_conn* c, uint16_t cid) { for (struct l2cap_chan* ch = c->chans; ch; ch = ch->next) if (ch->scid == cid && !ch->fixed && !ch->raw) return ch; return NULL; }
static struct l2cap_chan* by_dcid(struct bt_conn* c, uint16_t cid) { for (struct l2cap_chan* ch = c->chans; ch; ch = ch->next) if (ch->dcid == cid && !ch->fixed && !ch->raw) return ch; return NULL; }
static struct l2cap_chan* by_ident(struct bt_conn* c, uint8_t ident) { for (struct l2cap_chan* ch = c->chans; ch; ch = ch->next) if (ch->ident == ident && !ch->fixed && !ch->raw) return ch; return NULL; }
static void config_request(struct bt_conn* c, uint8_t ident, const uint8_t* q, unsigned n) {
    uint16_t cid = (uint16_t)bt_get16(q), flags = (uint16_t)bt_get16(q + 2);
    struct l2cap_chan* ch = by_scid(c, cid);
    if (!ch || (ch->state != CHAN_CONFIG && ch->state != CHAN_CONNECTED)) { uint8_t data[4]; bt_put16(data, cid); bt_put16(data + 2, 0); reject(c, ident, 0x0002, data, 4); return; }
    uint8_t r[64]; bt_put16(r, ch->dcid); bt_put16(r + 2, flags & 1); unsigned result = 0, out = 6, unknown = 6;
    uint8_t unknown_types[16]; unsigned unknown_count = 0;
    for (unsigned at = 4; at < n;) {
        if (at + 2 > n || at + 2 + q[at + 1] > n) { result = 0x0002; break; }   /* truncated option: reject */
        unsigned type = q[at] & 0x7F, hint = q[at] & 0x80, len = q[at + 1]; const uint8_t* v = q + at + 2;
        switch (type) {
        case 0x01: if (len != 2) { result = 0x0002; break; } { uint16_t mtu = (uint16_t)bt_get16(v); if (mtu < 48) { result = 0x0001; if (out + 4 <= sizeof(r)) { r[out] = 0x01; r[out + 1] = 2; bt_put16(r + out + 2, 48); out += 4; } } else ch->omtu = mtu; } break;
        case 0x02: case 0x03: case 0x05: case 0x06: case 0x07: break;
        case 0x04:
            if (len != 9) { result = 0x0002; break; }
            if (v[0] != 0x00) { result = 0x0001; if (out + 11 <= sizeof(r)) { r[out] = 0x04; r[out + 1] = 9; memset(r + out + 2, 0, 9); out += 11; } }
            break;
        default: if (!hint && unknown_count < 16) unknown_types[unknown_count++] = (uint8_t)type;
        }
        if (result == 0x0002) break;
        at += 2 + len;
    }
    if (unknown_count) { result = 0x0003; out = unknown; memcpy(r + out, unknown_types, unknown_count); out += unknown_count; }
    if (result == 0x0002) { bt_put16(r + 4, 0x0002); signal(c, 0x05, ident, r, 6); return; }
    bt_put16(r + 4, result); signal(c, 0x05, ident, r, out);
    if (!result && !(flags & 1)) { ch->conf |= CONF_IN; if (!(ch->conf & CONF_SENT)) send_config(ch); check_configured(ch); }
}
static void config_response(struct bt_conn* c, uint8_t ident, const uint8_t* q, unsigned n) {
    uint16_t cid = (uint16_t)bt_get16(q), result = (uint16_t)bt_get16(q + 4);
    struct l2cap_chan* ch = by_scid(c, cid);
    if (!ch || ch->ident != ident || ch->state != CHAN_CONFIG) return;
    if (result == 0x0000) { ch->conf |= CONF_OUT; check_configured(ch); return; }
    if (result == 0x0004) { ch->deadline = bt_now() + 20000000000ULL; return; }   /* pending */
    if (result == 0x0001 && ++ch->conf_tries < 3) {                         /* unacceptable: adopt the suggested MTU */
        for (unsigned at = 6; at + 2 <= n && at + 2 + q[at + 1] <= n; at += 2 + q[at + 1]) if ((q[at] & 0x7F) == 0x01 && q[at + 1] == 2) { uint16_t mtu = (uint16_t)bt_get16(q + at + 2); if (mtu >= 48) ch->imtu = mtu; }
        send_config(ch); return;
    }
    send_disconnect(ch);
}
static void info_request(struct bt_conn* c, uint8_t ident, const uint8_t* q) {
    uint16_t type = (uint16_t)bt_get16(q); uint8_t r[12] = {0}; bt_put16(r, type);
    if (type == 0x0002) { bt_put16(r + 2, 0); bt_put32(r + 4, 0x00000080); signal(c, 0x0B, ident, r, 8); }
    else if (type == 0x0003) { bt_put16(r + 2, 0); r[4] = 0x02; signal(c, 0x0B, ident, r, 12); }
    else { bt_put16(r + 2, 0x0001); signal(c, 0x0B, ident, r, 4); }
}
static void le_connect_request(struct bt_conn* c, uint8_t ident, const uint8_t* q) {
    uint16_t psm = (uint16_t)bt_get16(q), scid = (uint16_t)bt_get16(q + 2), mtu = (uint16_t)bt_get16(q + 4), mps = (uint16_t)bt_get16(q + 6), credits = (uint16_t)bt_get16(q + 8);
    uint8_t r[10] = {0}; unsigned result = 0;
    struct l2sock* listener = find_listener(c, psm, 0);
    if (mtu < 23 || mps < 23 || mps > 65533) result = 0x000B;
    else if (!listener || listener->mode == 0x81) result = 0x0002;           /* an Ext-Flowctl listener takes 0x17 requests only */
    else if (scid < 0x0040 || scid > 0x007F) result = 0x0009;
    else if (by_dcid(c, scid)) result = 0x000A;
    else if (listener->sec_level > c->sec_level || (listener->sec_level >= SEC_MEDIUM && !c->encrypted)) result = c->encrypted ? 0x0005 : 0x0008;
    if (result) { bt_put16(r + 8, result); signal(c, 0x15, ident, r, 10); return; }
    struct l2cap_chan* ch = chan_new(); uint16_t cid = ch ? free_cid(c, true) : 0;
    if (!ch || !cid) { if (ch) chan_free(ch); bt_put16(r + 8, 0x0004); signal(c, 0x15, ident, r, 10); return; }
    ch->le_coc = true; ch->psm = psm; ch->scid = cid; ch->dcid = scid; ch->omtu = mtu; ch->peer_mps = mps; ch->tx_credits = credits; ch->ident = ident;
    chan_attach(ch, c);
    if (!child_socket(listener, ch, c)) { chan_free(ch); bt_put16(r + 8, 0x0004); signal(c, 0x15, ident, r, 10); return; }
    if (ch->imtu < L2CAP_LE_MIN_MTU) ch->imtu = L2CAP_LE_MIN_MTU;
    unsigned acl = c->dev->le_max ? c->dev->le_mtu : c->dev->acl_mtu; ch->mps = (uint16_t)(ch->imtu < acl - 4 ? ch->imtu : acl > 27 ? acl - 4 : 23); if (ch->mps < 23) ch->mps = 23;
    ch->rx_credits = (uint16_t)(ch->imtu / ch->mps + 1);
    if (ch->sock->defer) { ch->state = CHAN_CONNECT2; ch->deferred = true; ch->deadline = bt_now() + 40000000000ULL; sock_wake(ch->sock); return; }
    accept_now(ch);
}
/* LE Credit Based Connection Request (Ext-Flowctl): up to 5 channels to one SPSM, answered together. */
static void ecred_request(struct bt_conn* c, uint8_t ident, const uint8_t* q, unsigned n) {
    uint16_t psm = (uint16_t)bt_get16(q), mtu = (uint16_t)bt_get16(q + 2), mps = (uint16_t)bt_get16(q + 4), credits = (uint16_t)bt_get16(q + 6);
    unsigned count = (n - 8) / 2; uint8_t r[8 + 10] = {0}; unsigned result = 0;
    struct l2sock* listener = find_listener(c, psm, 0);
    if (n < 10 || (n - 8) % 2 || count > 5 || mtu < 64 || mps < 64 || mps > 65533) result = (n - 8) % 2 || count > 5 || n < 10 ? 0x000C : 0x000B;
    else if (!psm || psm > 0x00FF || !listener) result = 0x0002;          /* any LE listener of the SPSM takes it (Linux) */
    else if (listener->sec_level > c->sec_level || (listener->sec_level >= SEC_MEDIUM && !c->encrypted)) result = c->encrypted ? 0x0005 : 0x0008;
    unsigned created = 0; struct l2cap_chan* first = NULL; bool defer = listener && listener->defer;
    for (unsigned i = 0; !result && i < count; i++) {
        uint16_t scid = (uint16_t)bt_get16(q + 8 + 2 * i);
        if (scid < 0x0040 || scid > 0x007F) { result = 0x0009; continue; }
        if (by_dcid(c, scid)) { result = 0x000A; continue; }
        struct l2cap_chan* ch = chan_new(); uint16_t cid = ch ? free_cid(c, true) : 0;
        if (!ch || !cid) { if (ch) chan_free(ch); result = 0x0004; continue; }
        ch->le_coc = ch->ecred = true; ch->psm = psm; ch->scid = cid; ch->dcid = scid; ch->omtu = mtu; ch->peer_mps = mps; ch->tx_credits = credits; ch->ident = ident;
        chan_attach(ch, c);
        if (!child_socket(listener, ch, c)) { chan_free(ch); result = 0x0004; continue; }
        if (ch->imtu < 64) ch->imtu = 64;
        unsigned acl = c->dev->le_max ? c->dev->le_mtu : c->dev->acl_mtu; ch->mps = (uint16_t)(ch->imtu < acl - 4 ? ch->imtu : acl > 68 ? acl - 4 : 64); if (ch->mps < 64) ch->mps = 64;
        ch->rx_credits = (uint16_t)(ch->imtu / ch->mps + 1);
        bt_put16(r + 8 + 2 * i, cid); created++; if (!first) first = ch;
        if (defer) { ch->state = CHAN_CONNECT2; ch->deferred = true; ch->deadline = bt_now() + 40000000000ULL; sock_wake(ch->sock); continue; }   /* answered on accept */
        ch->state = CHAN_CONNECTED; ch->sock->connected_once = true;
    }
    if (defer && created) return;
    if (first) { bt_put16(r, first->imtu); bt_put16(r + 2, first->mps); bt_put16(r + 4, first->rx_credits); }
    bt_put16(r + 6, result); signal(c, 0x18, ident, r, 8 + 2 * count);
    for (struct l2cap_chan* ch = c->chans; ch; ch = ch->next) if (ch->ecred && ch->ident == ident && ch->state == CHAN_CONNECTED && ch->sock) sock_wake(ch->sock);
    (void)created;
}
static void coc_kick(struct l2cap_chan* ch) {
    struct bt_conn* c = l2cap_chan_conn(ch); if (!c) return;
    while (ch->tx_credits && ch->txq.head) { struct bt_pkt* p = bt_queue_take(&ch->txq); if (bt_acl_send(c, ch->dcid, p->data, p->len) < 0) { bt_queue_put(&ch->txq, p); break; } ch->tx_credits--; bt_pkt_free(p); }
    if (ch->sock) bt_wake(ch->sock);
}
static void signalling(struct bt_conn* c, const uint8_t* data, unsigned length) {
    bool le = c->link == LINK_LE; uint32_t id = c->id;
    for (unsigned at = 0; at + 4 <= length;) {
        if (!(c = bt_conn_get(id))) return;
        uint8_t code = data[at], ident = data[at + 1]; unsigned n = bt_get16(data + at + 2);
        const uint8_t* q = data + at + 4;
        if (at + 4 + n > length || !ident) { reject(c, ident, 0x0000, NULL, 0); return; }   /* malformed: Command Not Understood */
        at += 4 + n;
        static const uint8_t bredr_size[] = {[0x02] = 4, [0x03] = 8, [0x04] = 4, [0x05] = 6, [0x06] = 4, [0x07] = 4, [0x0A] = 2, [0x0B] = 4, [0x01] = 2};
        static const uint8_t le_size[] = {[0x01] = 2, [0x06] = 4, [0x07] = 4, [0x12] = 8, [0x13] = 2, [0x14] = 10, [0x15] = 10, [0x16] = 4, [0x17] = 8, [0x18] = 8, [0x19] = 6, [0x1A] = 2};
        unsigned minimum = le ? (code < sizeof(le_size) ? le_size[code] : 0) : (code < sizeof(bredr_size) ? bredr_size[code] : 0);
        bool known = le ? (code < sizeof(le_size) && le_size[code]) : ((code < sizeof(bredr_size) && bredr_size[code]) || code == 0x08 || code == 0x09);
        if (!known) { reject(c, ident, 0x0000, NULL, 0); if (le) return; continue; }
        if (n < minimum || (le && (code == 0x14 || code == 0x15 || code == 0x12 || code == 0x16) && n != minimum)) { reject(c, ident, 0x0000, NULL, 0); continue; }
        switch (code) {
        case 0x01: {
            if (!le && c->info_sent && !c->info_done && ident == c->info_ident) { info_complete(c); break; }   /* our Information Request refused */
            struct l2cap_chan* ch = by_ident(c, ident); if (ch && (ch->state == CHAN_CONNECT || ch->state == CHAN_CONFIG || ch->state == CHAN_DISCONN)) chan_closed(ch, ch->state == CHAN_DISCONN ? 0 : E_CONNREFUSED); break; }
        case 0x02: if (!le) connect_request(c, ident, q); break;
        case 0x03: {
            uint16_t dcid = (uint16_t)bt_get16(q), scid = (uint16_t)bt_get16(q + 2), result = (uint16_t)bt_get16(q + 4);
            struct l2cap_chan* ch = scid ? by_scid(c, scid) : by_ident(c, ident);
            if (!ch || ch->state != CHAN_CONNECT || ch->ident != ident) break;
            if (result == 0x0001) { ch->deadline = bt_now() + 40000000000ULL; break; }
            if (result) { chan_closed(ch, result == 0x0003 ? E_ACCES : result == 0x0004 ? E_NOMEM : E_CONNREFUSED); break; }
            if (dcid < 0x0040) { chan_closed(ch, E_PROTO); break; }
            ch->dcid = dcid; ch->state = CHAN_CONFIG; ch->conf = 0; send_config(ch); break;
        }
        case 0x04: if (!le) config_request(c, ident, q, n); break;
        case 0x05: if (!le) config_response(c, ident, q, n); break;
        case 0x06: {
            uint16_t dcid = (uint16_t)bt_get16(q), scid = (uint16_t)bt_get16(q + 2);
            struct l2cap_chan* ch = by_scid(c, dcid);
            if (!ch || ch->dcid != scid) { uint8_t r[4]; bt_put16(r, dcid); bt_put16(r + 2, scid); reject(c, ident, 0x0002, r, 4); break; }   /* {local CID, remote CID} of the request */
            uint8_t r[4]; bt_put16(r, dcid); bt_put16(r + 2, scid); signal(c, 0x07, ident, r, 4);
            chan_closed(ch, E_CONNRESET); break;
        }
        case 0x07: { struct l2cap_chan* ch = by_scid(c, (uint16_t)bt_get16(q + 2)); if (ch && ch->state == CHAN_DISCONN) chan_closed(ch, 0); break; }
        case 0x08: signal(c, 0x09, ident, q, n); break;                    /* Echo */
        case 0x09: break;
        case 0x0A: info_request(c, ident, q); break;
        case 0x0B: {                                                       /* Information Response: feature mask, then fixed channels */
            if (!c->info_sent || c->info_done || ident != c->info_ident) break;
            uint16_t type = (uint16_t)bt_get16(q), result = (uint16_t)bt_get16(q + 2);
            if (type == 2 && !result && n >= 8) { c->remote_feat_mask = bt_get32(q + 4); if (c->remote_feat_mask & 0x80) { request_info(c, 0x0003); break; } }
            info_complete(c); break;
        }
        case 0x12: {                                                       /* LE Connection Parameter Update Request */
            uint16_t min = (uint16_t)bt_get16(q), max = (uint16_t)bt_get16(q + 2), latency = (uint16_t)bt_get16(q + 4), timeout = (uint16_t)bt_get16(q + 6);
            if (c->role != 0) { reject(c, ident, 0x0000, NULL, 0); break; }
            bool ok = min >= 6 && max <= 3200 && min <= max && timeout >= 10 && timeout <= 3200 && latency <= 499 && (uint32_t)timeout * 4 > (uint32_t)max * (1U + latency);
            uint8_t r[2]; bt_put16(r, ok ? 0 : 1); signal(c, 0x13, ident, r, 2);
            if (ok) { uint8_t p[14]; bt_put16(p, c->handle); bt_put16(p + 2, min); bt_put16(p + 4, max); bt_put16(p + 6, latency); bt_put16(p + 8, timeout); bt_put16(p + 10, 0); bt_put16(p + 12, 0); bt_cmd_async(c->dev, 0x2013, p, 14, NULL, NULL, 0); }
            break;
        }
        case 0x13: break;
        case 0x14: le_connect_request(c, ident, q); break;
        case 0x17: ecred_request(c, ident, q, n); break;
        case 0x18: {                                                       /* Credit Based Connection Response */
            uint16_t mtu = (uint16_t)bt_get16(q), mps = (uint16_t)bt_get16(q + 2), credits = (uint16_t)bt_get16(q + 4), result = (uint16_t)bt_get16(q + 6);
            struct l2cap_chan* ch = by_ident(c, ident);
            if (!ch || ch->state != CHAN_CONNECT || !ch->ecred) break;
            uint16_t dcid = n >= 10 ? (uint16_t)bt_get16(q + 8) : 0;
            if (!dcid || (result && result != 0x0004 && result != 0x0009 && result != 0x000A)) { chan_closed(ch, result == 0x0005 || result == 0x0008 || result == 0x0007 ? E_ACCES : E_CONNREFUSED); break; }
            if (dcid < 0x0040 || dcid > 0x007F || mtu < 64 || mps < 64 || mps > 65533 || by_dcid(c, dcid)) { send_disconnect(ch); break; }
            ch->dcid = dcid; ch->omtu = mtu; ch->peer_mps = mps; ch->tx_credits = credits; chan_ready(ch); break;
        }
        case 0x19: {                                                       /* Credit Based Reconfigure Request: MTU may only grow */
            uint16_t mtu = (uint16_t)bt_get16(q), mps = (uint16_t)bt_get16(q + 2); unsigned result = 0;
            if ((n - 4) % 2 || n < 6 || mtu < 64 || mps < 64) result = 0x0004;
            for (unsigned i = 0; !result && 4 + 2 * i < n; i++) { struct l2cap_chan* ch = by_dcid(c, (uint16_t)bt_get16(q + 4 + 2 * i)); if (!ch || !ch->ecred) result = 0x0003; else if (mtu < ch->omtu) result = 0x0001; }
            for (unsigned i = 0; !result && 4 + 2 * i < n; i++) { struct l2cap_chan* ch = by_dcid(c, (uint16_t)bt_get16(q + 4 + 2 * i)); ch->omtu = mtu; ch->peer_mps = mps; }
            uint8_t r[2]; bt_put16(r, result); signal(c, 0x1A, ident, r, 2); break;
        }
        case 0x1A: break;
        case 0x15: {
            uint16_t dcid = (uint16_t)bt_get16(q), mtu = (uint16_t)bt_get16(q + 2), mps = (uint16_t)bt_get16(q + 4), credits = (uint16_t)bt_get16(q + 6), result = (uint16_t)bt_get16(q + 8);
            struct l2cap_chan* ch = by_ident(c, ident);
            if (!ch || ch->state != CHAN_CONNECT || !ch->le_coc) break;
            if (result) { chan_closed(ch, result == 0x0005 || result == 0x0008 || result == 0x0007 ? E_ACCES : E_CONNREFUSED); break; }
            if (dcid < 0x0040 || dcid > 0x007F || mtu < 23 || mps < 23 || mps > 65533 || by_dcid(c, dcid)) { send_disconnect(ch); break; }
            ch->dcid = dcid; ch->omtu = mtu; ch->peer_mps = mps; ch->tx_credits = credits; chan_ready(ch); break;
        }
        case 0x16: {                                                       /* LE Flow Control Credit {cid of the sender, credits} */
            struct l2cap_chan* ch = by_dcid(c, (uint16_t)bt_get16(q)); uint32_t credits = bt_get16(q + 2);
            if (!ch || !ch->le_coc || ch->state != CHAN_CONNECTED) break;
            if (ch->tx_credits + credits > 65535) { send_disconnect(ch); break; }   /* credit overflow: the spec requires a disconnect */
            ch->tx_credits = (uint16_t)(ch->tx_credits + credits); coc_kick(ch); break;
        }
        }
        if (le) return;                                                    /* one command per LE signalling frame */
    }
}

/* ---------- data ---------- */
static void deliver(struct l2cap_chan* ch, const uint8_t* data, unsigned length) {
    if (ch->kops) { ch->kops->recv(ch->kowner, data, length); return; }
    struct l2sock* s = ch->sock; if (!s || s->shut_rd) return;
    if (s->rx.bytes + length > RX_LIMIT) return;                           /* receiver not reading: dropped (SEQPACKET semantics) */
    struct bt_pkt* p = bt_pkt_new(length); if (!p) return;
    memcpy(p->data, data, length); bt_queue_put(&s->rx, p); bt_wake(s);
}
/* LE K-frames (Linux l2cap_ecred_data_rcv): no credit left = disconnect; a frame beyond the IMTU, an SDU beyond the
 * IMTU or more data than the SDU announced is dropped (the channel stays); a peer using a smaller MPS than ours is
 * followed. */
static void coc_receive(struct l2cap_chan* ch, struct bt_conn* c, const uint8_t* data, unsigned length) {
    if (!ch->rx_credits) { send_disconnect(ch); return; }
    if (length > ch->imtu) return;
    ch->rx_credits--;
    if (!ch->sdu) {
        if (length < 2) return;
        uint32_t sdu_len = bt_get16(data);
        if (sdu_len > ch->imtu || length - 2 > sdu_len) return;
        if (length - 2 == sdu_len) deliver(ch, data + 2, length - 2);
        else {
            ch->sdu = bt_zalloc(sdu_len ? sdu_len : 1); if (!ch->sdu) return; ch->sdu_len = sdu_len; ch->sdu_have = length - 2; memcpy(ch->sdu, data + 2, length - 2);
            if (length < ch->mps) ch->mps = (uint16_t)(length < 23 ? 23 : length);   /* the peer's MPS is smaller: credits follow it */
        }
    } else {
        if (ch->sdu_have + length > ch->sdu_len) { bt_free(ch->sdu, ch->sdu_len); ch->sdu = NULL; return; }
        memcpy(ch->sdu + ch->sdu_have, data, length); ch->sdu_have += length;
        if (ch->sdu_have == ch->sdu_len) { uint8_t* sdu = ch->sdu; uint32_t n = ch->sdu_len; ch->sdu = NULL; uint32_t id = ch->id; deliver(ch, sdu, n); bt_free(sdu, n ? n : 1); if (!chan_get(id)) return; }
    }
    uint16_t full = (uint16_t)(ch->imtu / ch->mps + 1);                   /* give credits back as data is taken */
    if (ch->rx_credits <= full / 2 && (c = l2cap_chan_conn(ch))) {
        uint16_t more = (uint16_t)(full - ch->rx_credits); ch->rx_credits = full; uint8_t r[4]; bt_put16(r, ch->scid); bt_put16(r + 2, more); signal(c, 0x16, next_ident(c), r, 4);
    }
}
void l2cap_receive(struct bt_conn* c, const uint8_t* frame, unsigned length) {
    if (length < 4 || bt_get16(frame) != length - 4) return;
    uint16_t cid = (uint16_t)bt_get16(frame + 2); const uint8_t* data = frame + 4; unsigned n = length - 4;
    if (c->link == LINK_ACL && cid == L2CAP_CID_SIGNAL) {
        for (struct l2cap_chan* ch = c->chans; ch; ch = ch->next) if (ch->raw) deliver(ch, data, n);   /* SOCK_RAW (l2ping) */
        signalling(c, data, n); return;
    }
    if (c->link == LINK_LE && cid == L2CAP_CID_LE_SIGNAL) { signalling(c, data, n); return; }
    if (c->link == LINK_LE && cid == L2CAP_CID_SMP) { smp_receive(c, data, n); return; }
    if (c->link == LINK_ACL && cid == L2CAP_CID_SMP_BREDR) { uint8_t r[2] = {0x05, 0x05}; if (n) bt_acl_send(c, L2CAP_CID_SMP_BREDR, r, 2); return; }   /* Pairing Not Supported */
    if (c->link == LINK_LE && cid == L2CAP_CID_ATT) {
        for (struct l2cap_chan* ch = c->chans; ch; ch = ch->next) if (ch->fixed && ch->scid == L2CAP_CID_ATT && ch->state == CHAN_CONNECTED) { if (n <= ch->imtu || n <= 517) deliver(ch, data, n); return; }
        return;
    }
    if (cid < 0x0040) return;
    struct l2cap_chan* ch = by_scid(c, cid);
    if (!ch || ch->state != CHAN_CONNECTED) return;
    if (ch->le_coc) { coc_receive(ch, c, data, n); return; }
    if (n > ch->imtu) return;                                              /* beyond our MTU: dropped */
    deliver(ch, data, n);
}
void l2cap_conn_ready(struct bt_conn* c) {
    uint32_t id = c->id;
    for (struct l2cap_chan *ch = c->chans, *next; ch; ch = next) { next = ch->next; if (ch->initiator && ch->state == CHAN_CONNECT && !ch->ident) start_outgoing(ch, c); if (!bt_conn_get(id)) return; }
    if (c->link != LINK_LE) return;
    for (struct l2cap_chan* ch = c->chans; ch; ch = ch->next) if (ch->fixed && ch->scid == L2CAP_CID_ATT) return;   /* a client ATT socket owns it */
    struct l2sock* listener = find_listener(c, 0, L2CAP_CID_ATT); if (!listener) return;
    struct l2cap_chan* ch = chan_new(); if (!ch) return;
    ch->fixed = true; ch->scid = ch->dcid = L2CAP_CID_ATT; ch->omtu = 23; chan_attach(ch, c);
    if (!child_socket(listener, ch, c)) { chan_free(ch); return; }
    ch->imtu = 517; chan_ready(ch);
}
void l2cap_conn_down(struct bt_conn* c, int error) { while (c->chans) chan_closed(c->chans, error ? error : E_CONNRESET); }
void l2cap_security_done(struct bt_conn* c, int status) {
    uint32_t id = c->id;
    for (struct l2cap_chan *ch = c->chans, *next; ch; ch = next) {
        next = ch->next;
        if (ch->state != CHAN_SECURITY) continue;
        /* Linux l2cap_security_cfm: a successful authentication/encryption lets the channel go on at the level the
         * link reached (the pairing itself refuses MITM it cannot provide); a failure refuses or closes it. */
        if (!status && (c->auth_pending || c->enc_pending || c->smp)) continue;   /* still in progress */
        if (!status && c->encrypted) ch->sec_level = c->sec_level;
        if (ch->pending_rsp) { if (status) refuse_pending(ch, 0x0003); else if (ch->sock && ch->sock->defer) { ch->state = CHAN_CONNECT2; ch->deferred = true; sock_wake(ch->sock); } else accept_now(ch); }
        else if (status) chan_closed(ch, status);
        else start_outgoing(ch, c);
        if (!bt_conn_get(id)) return;
    }
}
bool l2cap_conn_in_use(struct bt_conn* c) { return c->chans != NULL; }
void l2cap_poll(uint64_t now) {
    for (struct l2cap_chan *ch = chans, *next; ch; ch = next) {
        next = ch->all_next;
        struct bt_conn* c = ch->await_info ? l2cap_chan_conn(ch) : NULL;
        if (c && c->info_deadline && now >= c->info_deadline) { info_complete(c); next = chans; continue; }   /* no answer: go on */
        if (!ch->deadline || now < ch->deadline) continue;
        ch->deadline = 0;
        if (ch->state == CHAN_DISCONN || ch->state == CHAN_CONNECT) { chan_closed(ch, ch->state == CHAN_DISCONN ? 0 : E_TIMEDOUT); next = chans; continue; }
        if (ch->pending_rsp || ch->deferred) { refuse_pending(ch, 0x0003); next = chans; continue; }
        if (ch->await_info && !ch->initiator) { refuse_pending(ch, 0x0002); next = chans; continue; }
        if (ch->state == CHAN_CONFIG || ch->state == CHAN_SECURITY) { send_disconnect(ch); if (chan_get(ch->id)) ch->error = E_TIMEDOUT; next = chans; }
    }
}
uint64_t l2cap_next_event(uint64_t now) {
    (void)now; uint64_t next = ~0ULL;
    for (struct l2cap_chan* ch = chans; ch; ch = ch->all_next) {
        if (ch->deadline && ch->deadline < next) next = ch->deadline;
        struct bt_conn* c = ch->await_info ? l2cap_chan_conn(ch) : NULL;
        if (c && c->info_deadline && c->info_deadline < next) next = c->info_deadline;
    }
    return next;
}
static int chan_send(struct l2cap_chan* ch, const uint8_t* data, unsigned length) {
    struct bt_conn* c = l2cap_chan_conn(ch);
    if (!c || ch->state != CHAN_CONNECTED) return E_NOTCONN;
    if (length > ch->omtu && !ch->raw) return E_MSGSIZE;
    if (ch->raw) return bt_acl_send(c, L2CAP_CID_SIGNAL, data, length);
    if (!ch->le_coc) return bt_acl_send(c, ch->dcid, data, length);
    unsigned mps = ch->peer_mps < 23 ? 23 : ch->peer_mps; if (ch->txq.count > 64) return E_AGAIN;
    for (unsigned at = 0, first = 1; at < length || first; first = 0) {   /* K-frames: the first carries the SDU length */
        unsigned room = first ? mps - 2 : mps, take = length - at < room ? length - at : room;
        struct bt_pkt* p = bt_pkt_new(take + (first ? 2 : 0)); if (!p) return E_NOMEM;
        if (first) { bt_put16(p->data, length); memcpy(p->data + 2, data + at, take); } else memcpy(p->data, data + at, take);
        bt_queue_put(&ch->txq, p); at += take;
    }
    coc_kick(ch); return 0;
}

/* ---------- kernel users (RFCOMM) ---------- */
struct l2cap_chan* l2cap_kernel_connect(struct bt_dev* d, const uint8_t* addr, uint16_t psm, uint8_t sec_level, const struct l2cap_ops* ops, void* owner, int* error) {
    struct l2cap_chan* ch = chan_connect(d, addr, 0, psm, 0, sec_level, 1013, false, false, error); if (!ch) return NULL;
    ch->kops = ops; ch->kowner = owner; return ch;
}
int l2cap_kernel_listen(uint16_t psm, const struct l2cap_ops* ops, void* (*accept)(struct l2cap_chan* ch)) {
    for (unsigned i = 0; i < 4; i++) if (!klisten[i].ops || klisten[i].psm == psm) { klisten[i] = (struct kernel_listener){psm, ops, accept}; return 0; }
    return E_NOMEM;
}
void l2cap_kernel_attach(struct l2cap_chan* ch, void* owner) { ch->kowner = owner; ch->imtu = 1013; }
int l2cap_kernel_send(struct l2cap_chan* ch, const uint8_t* data, unsigned length) { return chan_send(ch, data, length); }
void l2cap_kernel_close(struct l2cap_chan* ch) {
    if (!ch) return; ch->kops = NULL; ch->kowner = NULL;
    if (ch->state == CHAN_CONNECTED || ch->state == CHAN_CONFIG) send_disconnect(ch); else if (ch->state != CHAN_DISCONN) chan_closed(ch, 0);
}

/* ---------- sockets ---------- */
struct sockaddr_l2 { uint16_t family, psm; uint8_t bdaddr[6]; uint16_t cid; uint8_t type, pad; };
static int64_t sock_io(void* object, const struct iovec64* v, unsigned count, uint64_t total, bool write, uint64_t flags);
static int sock_ioctl(void* object, uint64_t command, uint64_t argument, uint64_t flags);
static uint32_t sock_ready(void* object);
static void* sock_source(void* object) { return object; }
static void sock_release(void* object);
static void sock_stat(void* object, struct linux_stat* stat) { memset(stat, 0, sizeof(*stat)); stat->dev = 4; stat->ino = 0x61000000ULL + ((struct l2sock*)object)->id; stat->nlink = 1; stat->mode = 0140777; stat->block_size = 4096; }
static const struct object_operations sock_operations = {sock_io, sock_ioctl, sock_ready, sock_source, sock_release, sock_stat};
bool l2cap_descriptor(struct open_file* f) { return fd_object(f, &sock_operations) != NULL; }
static struct l2sock* sock_alloc(int type) {
    unsigned slot = 0; while (slot < 64 && socks[slot]) slot++;
    if (slot == 64) return NULL;
    struct l2sock* s = bt_zalloc(sizeof(*s)); if (!s) return NULL;
    s->id = ++sock_ids; s->type = type; s->sec_level = SEC_LOW; s->imtu = L2CAP_DEFAULT_MTU; s->omtu = L2CAP_DEFAULT_MTU;
    socks[slot] = s; live_socks++; return s;
}
static void sock_free(struct l2sock* s) {
    for (unsigned i = 0; i < 64; i++) if (socks[i] == s) socks[i] = NULL;
    bt_queue_purge(&s->rx); live_socks--; bt_free(s, sizeof(*s));
}
static void sock_close_chan(struct l2sock* s) {
    struct l2cap_chan* ch = s->chan; if (!ch) return;
    s->chan = NULL; ch->sock = NULL; s->closed = true;
    if (ch->deferred || ch->pending_rsp) { refuse_pending(ch, 0x0003); return; }
    if (ch->await_info && !ch->initiator) { refuse_pending(ch, 0x0002); return; }   /* answered "pending": now refused (Linux: bad PSM) */
    if (ch->state == CHAN_CONNECTED || ch->state == CHAN_CONFIG || (ch->state == CHAN_CONNECT && ch->ident && !ch->fixed && !ch->raw)) send_disconnect(ch);
    else if (ch->state != CHAN_DISCONN) chan_closed(ch, 0);
}
static void sock_release(void* object) {
    struct l2sock* s = object;
    while (s->accept_head) { struct l2sock* child = s->accept_head; s->accept_head = child->accept_next; child->parent = NULL; sock_close_chan(child); sock_free(child); }
    if (s->parent) {                                                       /* closed before accept: unlink */
        for (struct l2sock** p = &s->parent->accept_head; *p; p = &(*p)->accept_next) if (*p == s) { *p = s->accept_next; s->parent->accept_count--; break; }
    }
    sock_close_chan(s); sock_free(s);
}
int64_t l2cap_socket_create(uint64_t type, uint64_t protocol, bool kernel) {
    (void)protocol; (void)kernel; int t = (int)(type & 0xF);
    if (type & ~(0xFULL | 0x800 | 0x80000)) return E_INVAL;
    if (t != 1 && t != 5 && t != 3) return t == 2 ? E_PROTONOSUPPORT : E_SOCKTNOSUPPORT;
    if (t == 3 && !bt_privileged(13)) return E_PERM;
    struct l2sock* s = sock_alloc(t); if (!s) return E_NOMEM;
    int fd = fd_create_object(&sock_operations, s, (type & 0x800) | (type & 0x80000) | 2, "socket:[l2cap]");
    if (fd < 0) sock_free(s);
    return fd;
}
static bool psm_valid(uint16_t psm, bool le) { return le ? psm >= 0x0001 && psm <= 0x00FF : (psm & 0x0101) == 0x0001; }
static int sock_bind(struct l2sock* s, const struct sockaddr_l2* a, unsigned size) {
    if (s->bound || s->chan) return E_INVAL;
    if (size < 2 || a->family != 31) return E_INVAL;
    if (a->psm && a->cid) return E_INVAL;
    if (a->type > 2) return E_INVAL;
    bool le = a->type != 0;
    if (le && a->cid && a->cid != L2CAP_CID_ATT) return E_INVAL;
    if (a->psm && !psm_valid(a->psm, le)) return E_INVAL;
    if (!le && a->cid) return E_INVAL;
    if (a->psm && (le ? a->psm < 0x0080 : a->psm < 0x1001) && !bt_privileged(10)) return E_ACCES;
    for (unsigned i = 0; i < 64; i++) {
        struct l2sock* o = socks[i];
        if (!o || o == s || !o->bound || o->parent || (o->src_type != 0) != le) continue;
        if (((a->psm && o->psm == a->psm) || (a->cid && o->cid == a->cid)) && (bt_addr_any(o->src) || bt_addr_any(a->bdaddr) || !memcmp(o->src, a->bdaddr, 6))) return E_ADDRINUSE;
    }
    memcpy(s->src, a->bdaddr, 6); s->src_type = a->type; s->psm = a->psm; s->cid = a->cid; s->bound = true;
    return 0;
}
static int wait_state(struct l2sock* s, uint64_t flags) {
    uint64_t end = s->sndtimeo ? bt_now() + s->sndtimeo : 0;
    for (;;) {
        if (s->chan && s->chan->state == CHAN_CONNECTED) return 0;
        if (!s->chan) { int e = s->error ? s->error : E_CONNREFUSED; s->error = 0; return e; }
        if (flags & 0x800) return E_INPROGRESS;
        int e = end ? task_wait_until(s, end) : task_wait_channel(s);
        if (e == E_TIMEDOUT) return E_INPROGRESS;                          /* Linux bt_sock_wait_state: SO_SNDTIMEO over */
        if (e) return e;
    }
}
static int sock_connect(struct l2sock* s, const struct sockaddr_l2* a, unsigned size, uint64_t flags) {
    if (s->listening) return E_INVAL;
    if (s->chan) return s->chan->state == CHAN_CONNECTED ? E_ISCONN : E_ALREADY;
    if (s->connected_once) return E_ISCONN;
    if (size < 2 || a->family != 31 || a->type > 2) return E_INVAL;
    bool le = a->type != 0;
    if (s->type != 3) {
        if (a->psm && a->cid) return E_INVAL;
        if (!a->psm && !a->cid) return E_INVAL;
        if (a->cid && (!le || a->cid != L2CAP_CID_ATT)) return E_INVAL;
        if (a->psm && !psm_valid(a->psm, le)) return E_INVAL;
    }
    if (bt_addr_any(a->bdaddr)) return E_INVAL;
    if (s->bound && (s->src_type != 0) != le) return E_INVAL;
    struct bt_dev* d = bt_dev_route(s->bound ? s->src : NULL, le); if (!d) return E_HOSTUNREACH;
    int error = 0;
    struct l2cap_chan* ch = chan_connect(d, a->bdaddr, a->type, a->psm, a->cid, s->sec_level, s->imtu, s->type == 3, s->mode == 0x81 && a->type, &error);
    if (!ch) return error;
    s->chan = ch; ch->sock = s; memcpy(s->dst, a->bdaddr, 6); s->dst_type = a->type; s->dpsm = a->psm; s->dcid = a->cid;
    if (s->sndtimeo) { ch->connect_timeout = s->sndtimeo; if (ch->state != CHAN_CONNECTED) ch->deadline = bt_now() + s->sndtimeo; }
    if (!s->bound) { memcpy(s->src, d->bdaddr, 6); s->src_type = le ? 1 : 0; s->bound = true; }
    else if (bt_addr_any(s->src)) memcpy(s->src, d->bdaddr, 6);
    if (ch->state == CHAN_CONNECTED) { s->connected_once = true; return 0; }
    return wait_state(s, flags);
}
static void fill_addr(struct sockaddr_l2* a, const uint8_t* addr, uint8_t type, uint16_t psm, uint16_t cid) { memset(a, 0, sizeof(*a)); a->family = 31; a->psm = psm; memcpy(a->bdaddr, addr, 6); a->cid = cid; a->type = type; }
static int put_addr(uint64_t address, uint64_t size_address, const struct sockaddr_l2* a) {
    if (!address) return 0;
    uint32_t size; if (bt_from_user(&size, size_address, 4)) return E_FAULT;
    if ((int32_t)size < 0) return E_INVAL;
    if (bt_to_user(address, a, size < 14 ? size : 14)) return E_FAULT;
    uint32_t full = 14; return bt_to_user(size_address, &full, 4);
}
static int64_t sock_accept(struct l2sock* s, uint64_t address, uint64_t size_address, uint64_t accept_flags, uint64_t flags) {
    if (!s->listening) return E_INVAL;
    if (accept_flags & ~(0x800ULL | 0x80000ULL)) return E_INVAL;
    for (;;) {
        struct l2sock* child = s->accept_head;
        while (child && child->chan && child->chan->state != CHAN_CONNECTED && !child->chan->deferred) child = child->accept_next;
        if (child && (!child->chan || child->chan->state == CHAN_CONNECTED || child->chan->deferred)) {
            for (struct l2sock** p = &s->accept_head; *p; p = &(*p)->accept_next) if (*p == child) { *p = child->accept_next; break; }
            s->accept_count--; child->parent = NULL; child->accept_next = NULL;
            int fd = fd_create_object(&sock_operations, child, (accept_flags & 0x800) | (accept_flags & 0x80000) | 2, "socket:[l2cap]");
            if (fd < 0) { sock_close_chan(child); sock_free(child); return fd; }
            struct sockaddr_l2 a; fill_addr(&a, child->dst, child->dst_type, child->psm, child->chan ? child->chan->dcid : 0);
            if (address && put_addr(address, size_address, &a)) return fd;    /* the descriptor exists: like Linux, EFAULT is not reported */
            return fd;
        }
        if (flags & 0x800) return E_AGAIN;
        int e = task_wait_channel(s); if (e) return e;
    }
}
static int64_t sock_send(struct l2sock* s, const uint8_t* data, unsigned length, uint64_t flags) {
    for (;;) {
        if (s->error && !s->chan) { int e = s->error; if (s->connected_once) s->error = 0; return e == E_CONNRESET || !e ? E_PIPE : e; }
        if (s->shut_wr) return E_PIPE;
        struct l2cap_chan* ch = s->chan;
        if (!ch) return s->connected_once ? E_PIPE : E_NOTCONN;
        if (ch->state != CHAN_CONNECTED) { if (ch->deferred) return E_NOTCONN; if (flags & 0x800) return E_AGAIN; int e = task_wait_channel(s); if (e) return e; continue; }
        if (s->type == 1) {                                                /* stream: in MTU-sized pieces */
            unsigned at = 0;
            while (at < length) {
                unsigned n = length - at < ch->omtu ? length - at : ch->omtu;
                int r = chan_send(ch, data + at, n);
                if (r == E_AGAIN) { if (at || (flags & 0x800)) return at ? (int64_t)at : E_AGAIN; struct bt_conn* c = l2cap_chan_conn(ch); int e = task_wait_channel(c && !ch->le_coc ? (void*)&c->txq : (void*)s); if (e) return at ? (int64_t)at : e; if (!(ch = s->chan)) break; continue; }
                if (r < 0) return at ? (int64_t)at : r;
                at += n;
            }
            return at;
        }
        int r = chan_send(ch, data, length);
        if (r == E_AGAIN && !(flags & 0x800)) { struct bt_conn* c = l2cap_chan_conn(ch); int e = task_wait_channel(c && !ch->le_coc ? (void*)&c->txq : (void*)s); if (e) return e; continue; }
        return r < 0 ? r : (int64_t)length;
    }
}
static int64_t sock_recv(struct l2sock* s, const struct iovec64* v, unsigned count, uint64_t total, uint64_t flags, bool* truncated) {
    *truncated = false;
    if (s->chan && s->chan->deferred) {                                    /* BT_DEFER_SETUP: reading accepts the channel */
        struct l2cap_chan* ch = s->chan; ch->deferred = false; accept_now(ch); return 0;
    }
    for (;;) {
        struct bt_pkt* p = s->rx.head;
        if (p) {
            if (s->type == 1) {                                            /* stream: as many bytes as fit, across packets */
                uint8_t buffer[4096]; unsigned n = 0;
                for (struct bt_pkt* q = s->rx.head; q && n < total && n < sizeof(buffer);) {
                    unsigned off = q == s->rx.head ? s->rx_offset : 0, take = q->len - off;
                    if (take > total - n) take = (unsigned)(total - n);
                    if (take > sizeof(buffer) - n) take = sizeof(buffer) - n;
                    memcpy(buffer + n, q->data + off, take); n += take; q = q->next;
                }
                int64_t copied = bt_copy_out_vectors(v, count, buffer, n); if (copied < 0) return copied;
                if (!(flags & BT_PEEK)) for (unsigned left = n; left;) {
                    struct bt_pkt* q = s->rx.head; unsigned avail = q->len - s->rx_offset;
                    if (left >= avail) { left -= avail; s->rx_offset = 0; bt_pkt_free(bt_queue_take(&s->rx)); } else { s->rx_offset += left; left = 0; }
                }
                return copied;
            }
            unsigned n = p->len < total ? p->len : (unsigned)total; *truncated = p->len > total;
            int64_t copied = bt_copy_out_vectors(v, count, p->data, n); if (copied < 0) return copied;
            if (!(flags & BT_PEEK)) bt_pkt_free(bt_queue_take(&s->rx));
            return (flags & (1ULL << 41)) && *truncated ? (int64_t)p->len : copied;   /* MSG_TRUNC: the full length */
        }
        if (s->shut_rd || (!s->chan && s->eof)) { if (s->error && s->error != E_CONNRESET) { int e = s->error; s->error = 0; return e; } return 0; }
        if (!s->chan) return s->connected_once ? 0 : E_NOTCONN;
        if (flags & 0x800) return E_AGAIN;
        int e = s->rcvtimeo ? task_wait_until(s, bt_now() + s->rcvtimeo) : task_wait_channel(s);
        if (e == E_TIMEDOUT) return E_AGAIN;                               /* SO_RCVTIMEO */
        if (e) return e;
    }
}
static int64_t sock_io(void* object, const struct iovec64* v, unsigned count, uint64_t total, bool write, uint64_t flags) {
    struct l2sock* s = object;
    if (s->listening) return E_NOTCONN;
    if (write) {
        uint8_t* buffer = bt_zalloc(total ? total : 1); if (!buffer) return E_NOMEM;
        if (total > 65535) { bt_free(buffer, total); return E_MSGSIZE; }
        int64_t n = bt_copy_in_vectors(v, count, total, buffer, (unsigned)total);
        if (n >= 0) n = sock_send(s, buffer, (unsigned)n, flags);
        bt_free(buffer, total ? total : 1); return n;
    }
    bool truncated; return sock_recv(s, v, count, total, flags, &truncated);
}
static int sock_ioctl(void* object, uint64_t command, uint64_t argument, uint64_t flags) {
    (void)flags; struct l2sock* s = object;
    if ((uint32_t)command == 0x541B) { uint32_t n = s->rx.head ? (s->type == 1 ? s->rx.bytes - s->rx_offset : s->rx.head->len) : 0; return bt_to_user(argument, &n, 4); }   /* FIONREAD */
    if ((uint32_t)command == 0x5411) { uint32_t n = s->chan ? s->chan->txq.bytes : 0; return bt_to_user(argument, &n, 4); }   /* TIOCOUTQ */
    return E_NOTTY;
}
static uint32_t sock_ready(void* object) {
    struct l2sock* s = object; uint32_t mask = 0;
    if (s->listening) { for (struct l2sock* c = s->accept_head; c; c = c->accept_next) if (!c->chan || c->chan->state == CHAN_CONNECTED || c->chan->deferred) mask |= 1U | 64U; return mask; }
    if (s->rx.count || (s->chan && s->chan->deferred)) mask |= 1U | 64U;
    if (s->chan && s->chan->state == CHAN_CONNECTED && (!s->chan->le_coc || s->chan->txq.count < 64)) mask |= 4U | 256U;
    if (!s->chan && (s->connected_once || s->error)) mask |= 1U | 4U | 16U | (s->error && s->error != E_CONNRESET ? 8U : 0) | 8192U;   /* as Linux: POLLOUT too (connect callbacks wait for it) */
    if (s->shut_rd) mask |= 1U | 8192U;
    if (s->shut_rd && s->shut_wr && !s->chan) mask |= 16U;                 /* SHUTDOWN_MASK and closed: POLLHUP (Linux) */
    if (s->closed && !s->chan) mask |= 4U | 16U;                           /* a channel existed and is gone (aborted connect) */
    return mask;
}
static uint8_t lm_to_level(uint32_t lm) { return lm & 0x40 ? SEC_FIPS : lm & 0x20 ? SEC_HIGH : lm & (0x02 | 0x04) ? SEC_MEDIUM : SEC_LOW; }
static uint32_t level_to_lm(uint8_t level) { return level >= SEC_FIPS ? 0x02 | 0x04 | 0x20 | 0x40 : level == SEC_HIGH ? 0x02 | 0x04 | 0x20 : level == SEC_MEDIUM ? 0x02 | 0x04 : 0; }
static int get_option(struct l2sock* s, uint64_t level, uint64_t name, uint64_t value, uint64_t size_address) {
    struct l2cap_chan* ch = s->chan;
    if (level == 1) {
        if (name == 20 || name == 21 || name == 66 || name == 67) { uint64_t ns = name == 21 || name == 67 ? s->sndtimeo : s->rcvtimeo; uint64_t tv[2] = {ns / 1000000000ULL, ns % 1000000000ULL / 1000}; return bt_get_bytes_option(value, size_address, tv, 16); }
        if (name == 4) { int e = s->chan ? 0 : (s->connected_once ? 0 : s->error); s->error = s->chan ? s->error : 0; return bt_get_int_option(value, size_address, (uint32_t)-e); }   /* SO_ERROR reads and clears */
        return bt_sol_socket_get(name, value, size_address, s->type, 0, 0, RX_LIMIT, 65536);
    }
    if (level == 6) {
        if (name == 1) {                                                   /* L2CAP_OPTIONS */
            uint8_t o[12] = {0}; bt_put16(o, ch ? ch->omtu : s->omtu); bt_put16(o + 2, ch ? ch->imtu : s->imtu); bt_put16(o + 4, 0xFFFF); o[6] = ch && ch->le_coc ? 0x80 : s->mode; o[7] = 1; o[8] = 3; bt_put16(o + 10, 63);
            return bt_get_bytes_option(value, size_address, o, 12);
        }
        if (name == 2) {                                                   /* L2CAP_CONNINFO */
            struct bt_conn* c = ch ? l2cap_chan_conn(ch) : NULL; if (!c || ch->state != CHAN_CONNECTED) return E_NOTCONN;
            uint8_t o[6] = {0}; bt_put16(o, c->handle); memcpy(o + 2, c->dev_class, 3); return bt_get_bytes_option(value, size_address, o, 6);
        }
        if (name == 3) return bt_get_int_option(value, size_address, level_to_lm(s->sec_level));   /* L2CAP_LM */
        return E_NOPROTOOPT;
    }
    if (level != 274) return E_NOPROTOOPT;
    switch (name) {
    case 4: { struct bt_conn* c = ch ? l2cap_chan_conn(ch) : NULL; uint8_t o[2] = {c && ch->state == CHAN_CONNECTED && c->sec_level > s->sec_level ? c->sec_level : s->sec_level, c ? (c->enc_key_size ? c->enc_key_size : 16) : 0}; return bt_get_bytes_option(value, size_address, o, 2); }
    case 7: return bt_get_int_option(value, size_address, s->defer);
    case 8: return bt_get_int_option(value, size_address, 0);
    case 9: { uint8_t o = 1; return bt_get_bytes_option(value, size_address, &o, 1); }
    case 10: return bt_get_int_option(value, size_address, 0);
    case 12: if (!ch || ch->state != CHAN_CONNECTED) return E_NOTCONN; { uint16_t mtu = ch->omtu; return bt_get_bytes_option(value, size_address, &mtu, 2); }
    case 13: { uint16_t mtu = ch ? ch->imtu : s->imtu; return bt_get_bytes_option(value, size_address, &mtu, 2); }
    case 14: if (!ch) return E_NOTCONN; return bt_get_int_option(value, size_address, ch->le_coc || s->dst_type ? 0x0200 : 0x0001);   /* BT_PHY: LE 1M / BR 1M 1-slot */
    case 15: { uint8_t m = ch && ch->ecred ? 0x04 : ch && ch->le_coc ? 0x03 : s->mode == 0x81 ? 0x04 : s->mode == 0x80 ? 0x03 : 0; return bt_get_bytes_option(value, size_address, &m, 1); }   /* BT_MODE: LE flowctl 3 / basic 0 */
    }
    return E_NOPROTOOPT;
}
static int set_option(struct l2sock* s, uint64_t level, uint64_t name, uint64_t value, uint64_t size) {
    uint32_t v = 0;
    if (level == 1) {
        if (name == 20 || name == 21 || name == 66 || name == 67) {         /* SO_RCVTIMEO / SO_SNDTIMEO (old and new: both a 64-bit timeval here) */
            uint64_t tv[2]; if (size < 16) return E_INVAL; if (bt_from_user(tv, value, 16)) return E_FAULT;
            if (tv[1] >= 1000000ULL || (int64_t)tv[0] < 0) return E_INVAL;   /* EDOM in Linux for tv_usec; negative = none */
            uint64_t ns = tv[0] > 1000000000ULL ? 0 : tv[0] * 1000000000ULL + tv[1] * 1000ULL;
            if (name == 21 || name == 67) s->sndtimeo = ns; else s->rcvtimeo = ns; return 0;
        }
        if (name == 7 || name == 8 || name == 12 || name == 13 || name == 29 || name == 9 || name == 16) return 0;   /* buffers, priority, linger, timestamps, keepalive, passcred */
        return E_NOPROTOOPT;
    }
    if (level == 6) {
        if (name == 1) {
            if (s->chan && s->chan->state == CHAN_CONNECTED) return E_INVAL;
            uint8_t o[12] = {0}; if (size < 12 && size < 10) return E_INVAL;
            if (bt_from_user(o, value, size < 12 ? size : 12)) return E_FAULT;
            if (o[6] != 0x00) return E_INVAL;                              /* basic mode only (ERTM/streaming not offered) */
            uint16_t imtu = (uint16_t)bt_get16(o + 2); if (imtu < 48) return E_INVAL;
            s->omtu = (uint16_t)bt_get16(o); s->imtu = imtu; s->mode = 0; return 0;
        }
        if (name == 3) { if (bt_set_int_option(value, size, &v)) return E_FAULT; s->sec_level = lm_to_level(v); if (s->chan) s->chan->sec_level = s->sec_level; return 0; }
        return E_NOPROTOOPT;
    }
    if (level != 274) return E_NOPROTOOPT;
    switch (name) {
    case 4: {                                                              /* BT_SECURITY {level, key_size} */
        uint8_t o[2] = {0}; if (size < 1) return E_INVAL;
        if (bt_from_user(o, value, size < 2 ? size : 2)) return E_FAULT;
        if (o[0] > SEC_FIPS) return E_INVAL;
        s->sec_level = o[0] < SEC_LOW && s->psm != 1 ? SEC_LOW : o[0];
        if (s->chan) { s->chan->sec_level = s->sec_level; struct bt_conn* c = l2cap_chan_conn(s->chan); if (c && s->chan->state == CHAN_CONNECTED) bt_conn_security(c, s->sec_level, true); }
        return 0;
    }
    case 7: if (s->listening || s->chan) return E_INVAL; if (bt_set_int_option(value, size, &v)) return E_FAULT; s->defer = v != 0; return 0;
    case 8: case 9: case 10: return bt_set_int_option(value, size, &v);
    case 13: { uint16_t mtu; if (size < 2 || bt_from_user(&mtu, value, 2)) return E_INVAL; if (s->chan && s->chan->state == CHAN_CONNECTED) return E_ISCONN; if (mtu < 23) return E_INVAL; s->imtu = mtu; return 0; }
    case 15: { uint8_t m; if (size < 1 || bt_from_user(&m, value, 1)) return E_FAULT; if (m != 0 && m != 3 && m != 4) return E_INVAL; s->mode = m == 4 ? 0x81 : m == 3 ? 0x80 : 0; return 0; }
    }
    return E_NOPROTOOPT;
}
int64_t l2cap_call(struct registers* r, struct open_file* f) {
    struct l2sock* s = fd_object(f, &sock_operations); if (!s) return -88;
    uint64_t flags = fd_status_flags(f); int64_t result;
    fd_pin(f);
    switch (r->rax) {
    case 49: { struct sockaddr_l2 a = {0}; unsigned n = r->rdx < 14 ? (unsigned)r->rdx : 14; if (bt_from_user(&a, r->rsi, n)) { result = E_FAULT; break; } result = sock_bind(s, &a, n); break; }
    case 50: if (!s->bound || (!s->psm && !s->cid) || (s->type != 1 && s->type != 5)) { result = E_INVAL; break; }
             s->listening = true; s->backlog = (unsigned)(r->rsi > 32 ? 32 : r->rsi ? r->rsi : 1); result = 0; break;
    case 42: { struct sockaddr_l2 a = {0}; unsigned n = r->rdx < 14 ? (unsigned)r->rdx : 14; if (bt_from_user(&a, r->rsi, n)) { result = E_FAULT; break; } result = sock_connect(s, &a, n, flags); break; }
    case 43: result = sock_accept(s, r->rsi, r->rdx, 0, flags); break;
    case 288: result = sock_accept(s, r->rsi, r->rdx, r->r10, flags); break;
    case 51: case 52: {
        struct sockaddr_l2 a;
        if (r->rax == 52) { if (!s->chan && !s->connected_once) { result = E_NOTCONN; break; } fill_addr(&a, s->dst, s->dst_type, s->dpsm ? s->dpsm : s->psm, s->chan ? s->chan->dcid : s->dcid); }
        else fill_addr(&a, s->src, s->src_type, s->psm, s->chan ? s->chan->scid : s->cid);
        result = put_addr(r->rsi, r->rdx, &a); break;
    }
    case 44: { struct iovec64 v = {r->rsi, r->rdx}; result = sock_io(s, &v, 1, r->rdx, true, flags | (r->r10 & 0x40 ? 0x800 : 0)); break; }
    case 45: { struct iovec64 v = {r->rsi, r->rdx}; result = sock_io(s, &v, 1, r->rdx, false, flags | (r->r10 & 0x40 ? 0x800 : 0) | (r->r10 & 2 ? BT_PEEK : 0)); break; }
    case 46: case 47: {
        struct message64 m; struct iovec64 v[64]; unsigned count; uint64_t total;
        int e = bt_read_message(r->rsi, &m, v, &count, &total); if (e) { result = e; break; }
        uint64_t mf = flags | (r->rdx & 0x40 ? 0x800 : 0);
        if (r->rax == 46) { result = sock_io(s, v, count, total, true, mf); break; }
        if (s->listening) { result = E_NOTCONN; break; }
        bool truncated; result = sock_recv(s, v, count, total, mf | (r->rdx & 2 ? BT_PEEK : 0) | (r->rdx & 0x20 ? (1ULL << 41) : 0), &truncated);
        if (result >= 0) {
            m.flags = truncated ? 0x20 : 0; m.control_size = 0;
            if (m.name && m.name_size) { struct sockaddr_l2 a; fill_addr(&a, s->dst, s->dst_type, s->dpsm, 0); unsigned n = m.name_size < 14 ? m.name_size : 14; if (bt_to_user(m.name, &a, n)) { result = E_FAULT; break; } m.name_size = 14; }
            if (bt_to_user(r->rsi, &m, sizeof(m))) result = E_FAULT;
        }
        break;
    }
    case 48:                                                               /* shutdown */
        if (r->rsi > 2) { result = E_INVAL; break; }
        if (!s->chan && !s->listening && !s->connected_once) { result = E_NOTCONN; break; }
        if (r->rsi != 1) s->shut_rd = true;
        if (r->rsi != 0) {                                                 /* SHUT_WR: disconnect; the socket closes on the response */
            s->shut_wr = true; struct l2cap_chan* ch = s->chan;
            if (ch && ch->state == CHAN_CONNECTED && !ch->fixed && !ch->raw) send_disconnect(ch); else sock_close_chan(s);
        }
        bt_wake(s); result = 0; break;
    case 54: result = set_option(s, r->rsi, r->rdx, r->r10, r->r8); break;
    case 55: result = get_option(s, r->rsi, r->rdx, r->r10, r->r8); break;
    default: result = E_OPNOTSUPP;
    }
    fd_unpin(f);
    return result;
}
