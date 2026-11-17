/* AF_PACKET sockets (Linux packet(7)) for tcpdump/libpcap, DHCP clients and other raw link-layer users.
 * socket(AF_PACKET, SOCK_RAW | SOCK_DGRAM, htons(protocol)) needs CAP_NET_RAW in the initial user namespace.
 * - bind(sockaddr_ll): protocol (ETH_P_ALL sees outgoing frames too) and interface; protocol 0 receives nothing.
 * - SOCK_RAW carries whole frames (the Ethernet header included), SOCK_DGRAM the network-layer packet with the
 *   link address in sockaddr_ll (sendto builds the header from sll_addr / sll_protocol).
 * - classic BPF filters (SO_ATTACH_FILTER / SO_DETACH_FILTER / SO_LOCK_FILTER) with the Linux checker rules and
 *   the SKF_AD_* ancillary loads, SKF_NET_OFF / SKF_LL_OFF relative loads; the filter result is the snap length.
 * - PACKET_ADD/DROP_MEMBERSHIP (promiscuous, all-multicast, multicast, unicast), undone on close.
 * - PACKET_AUXDATA (tpacket_auxdata cmsg), SO_TIMESTAMP(NS), PACKET_ORIGDEV, PACKET_IGNORE_OUTGOING,
 *   PACKET_STATISTICS (tpacket_stats / tpacket_stats_v3, reset on read), SIOCGSTAMP(NS), FIONREAD.
 * - memory-mapped receive rings: PACKET_VERSION TPACKET_V1/V2/V3, PACKET_RESERVE, PACKET_HDRLEN, PACKET_RX_RING
 *   and mmap() of the ring (pinned pages of a shared-memory file); TPACKET_V3 blocks retire when full or after
 *   tp_retire_blk_tov ms (packet_poll). PACKET_TX_RING, PACKET_FANOUT and PACKET_VNET_HDR are not supported.
 * Every network-layer hook runs under the kernel lock, as the socket calls do. */
#include "netdev.h"
#include "nlmsg.h"
#include "../../kernel/string.h"

#define SOL_PACKET 263
#define ETH_P_ALL 0x0003
#define TP_STATUS_KERNEL 0
#define TP_STATUS_USER 1
#define TP_STATUS_COPY 2
#define TP_STATUS_LOSING 4
#define TP_STATUS_BLK_TMO 0x20
#define RING_MAX_PAGES 16384U    /* 64 MiB */
#define FILTER_MAX 4096U
#define MEMBERSHIPS 16
#define PAGE 4096U

struct sockaddr_ll { uint16_t family, protocol; int32_t ifindex; uint16_t hatype; uint8_t pkttype, halen; uint8_t addr[8]; };
struct tpacket_auxdata { uint32_t tp_status, tp_len, tp_snaplen; uint16_t tp_mac, tp_net, tp_vlan_tci, tp_vlan_tpid; };
struct packet_mreq { int32_t ifindex; uint16_t type, alen; uint8_t address[8]; };
struct sock_filter { uint16_t code; uint8_t jt, jf; uint32_t k; };
struct tpacket_req3 { uint32_t block_size, block_nr, frame_size, frame_nr, retire_blk_tov, sizeof_priv, feature_req_word; };
_Static_assert(sizeof(struct sockaddr_ll) == 20 && sizeof(struct tpacket_auxdata) == 20 && sizeof(struct packet_mreq) == 16, "packet ABI");

struct pframe {
    struct pframe* next;
    unsigned length, original, charge, ifindex, offset;   /* offset: the network header within data (SOCK_RAW) */
    uint16_t protocol, hatype; uint8_t pkttype, halen, addr[8];
    uint64_t stamp_ns;
    uint8_t data[];
};
struct membership { unsigned ifindex, count; uint16_t type, alen; uint8_t address[8]; };
struct packet_socket {
    struct packet_socket* next;
    uint64_t id;
    unsigned type;                 /* 3 SOCK_RAW, 2 SOCK_DGRAM */
    uint16_t protocol;             /* network byte order, 0: not receiving */
    unsigned ifindex;
    struct pframe* head, *tail;
    unsigned queued, count, receive_buffer, send_buffer;
    uint32_t packets, drops, freeze;
    bool auxdata, origdev, ignore_outgoing, timestamp, timestamp_ns, filter_locked, closed;
    int error;                     /* pending (SO_ERROR) */
    uint32_t mark, priority;
    uint64_t receive_timeout, last_stamp_ns;
    struct sock_filter* filter; unsigned filter_length;
    struct membership members[MEMBERSHIPS];
    /* receive ring */
    unsigned version, reserve;
    struct memory_file* ring; uint64_t* pages; unsigned page_count;
    struct tpacket_req3 req;
    unsigned head_frame;           /* V1/V2: the next frame the kernel fills */
    unsigned block, block_offset, block_packets, block_seq, prev_packet;   /* V3 */
    uint64_t block_opened_ns, block_first_ns, block_last_ns;
    bool block_open, frozen;
    unsigned maps;
};
static struct packet_socket* sockets;
static uint64_t next_id;
unsigned packet_socket_count;
static struct packet_socket* sending;   /* a socket never receives its own transmissions (Linux skb_loop_sk) */

/* ---- classic BPF (Linux sk_chk_filter / bpf_prog_run semantics) ---- */
#define SKF_AD_OFF (-0x1000)
#define SKF_NET_OFF (-0x100000)
#define SKF_LL_OFF (-0x200000)
struct bpf_context { const uint8_t* data; unsigned length, network; uint16_t protocol, hatype; uint8_t pkttype; unsigned ifindex; uint32_t mark; };
static bool bpf_check(const struct sock_filter* f, unsigned n) {
    if (!n || n > FILTER_MAX) return false;
    for (unsigned i = 0; i < n; i++) {
        uint16_t code = f[i].code; unsigned cls = code & 7;
        switch (cls) {
        case 0: case 1: {   /* LD / LDX */
            unsigned mode = code & 0xE0, size = code & 0x18;
            if (cls == 1 && !(mode == 0x00 || mode == 0x60 || mode == 0x80 || (mode == 0xA0 && size == 0x10)) ) return false;
            if (cls == 1 && mode != 0xA0 && size != 0) return false;
            if (cls == 0 && mode != 0x00 && mode != 0x20 && mode != 0x40 && mode != 0x60 && mode != 0x80) return false;
            if (cls == 0 && (mode == 0x20 || mode == 0x40) && size == 0x18) return false;
            if (cls == 0 && (mode == 0x00 || mode == 0x60 || mode == 0x80) && size != 0) return false;
            if (mode == 0x60 && f[i].k >= 16) return false;
            break;
        }
        case 2: case 3: if (code != cls || f[i].k >= 16) return false; break;   /* ST / STX */
        case 4: {           /* ALU */
            unsigned op = code & 0xF0;
            if (op > 0xA0 || (code & ~0xF8U) != 4) return false;
            if ((op == 0x30 || op == 0x90) && !(code & 8) && f[i].k == 0) return false;   /* division by constant 0 */
            if (op == 0x80 && (code & 8)) return false;
            break;
        }
        case 5: {           /* JMP */
            unsigned op = code & 0xF0;
            if (op > 0x40 || (code & ~0xF8U) != 5) return false;
            if (op == 0x00) { if ((code & 8) || f[i].k >= n - i - 1) return false; }
            else if ((unsigned)f[i].jt >= n - i - 1 || (unsigned)f[i].jf >= n - i - 1) return false;
            break;
        }
        case 6: if (code != 0x06 && code != 0x0E && code != 0x16) return false; break;   /* RET K / X / A */
        case 7: if (code != 0x07 && code != 0x87) return false; break;                     /* TAX / TXA */
        }
    }
    return (f[n - 1].code & 7) == 6;   /* the program ends in a return */
}
static bool bpf_load(const struct bpf_context* c, int32_t k, unsigned size, uint32_t* out) {
    const uint8_t* base = c->data; unsigned length = c->length; int64_t at = k;
    if (k >= SKF_LL_OFF && k < SKF_NET_OFF) { at = (int64_t)k - SKF_LL_OFF; }
    else if (k >= SKF_NET_OFF && k < SKF_AD_OFF) { at = (int64_t)k - SKF_NET_OFF + c->network; }
    else if (k < 0) return false;
    if (at < 0 || (uint64_t)at + size > length) return false;
    const uint8_t* p = base + at;
    *out = size == 4 ? be32(p) : size == 2 ? be16(p) : p[0];
    return true;
}
static uint32_t bpf_ancillary(const struct bpf_context* c, int32_t k, uint32_t a, uint32_t x, bool* ok) {
    *ok = true;
    switch (k - SKF_AD_OFF) {
    case 0: return c->protocol;          /* SKF_AD_PROTOCOL (host order of the network value) */
    case 4: return c->pkttype;
    case 8: return c->ifindex;
    case 20: return c->mark;
    case 24: return 0;                   /* QUEUE */
    case 28: return c->hatype;
    case 32: return 0;                   /* RXHASH */
    case 36: return 0;                   /* CPU */
    case 40: return a ^ x;               /* ALU_XOR_X */
    case 44: case 48: case 60: return 0; /* VLAN_TAG, VLAN_TAG_PRESENT, VLAN_TPID: never tagged */
    case 52: return c->network;          /* PAY_OFFSET (approximation: the network header) */
    case 56: return net_random32();
    }
    *ok = false; return 0;
}
unsigned bpf_run(const struct sock_filter* f, unsigned n, const struct bpf_context* c) {
    uint32_t a = 0, x = 0, mem[16] = {0};
    for (unsigned pc = 0; pc < n; pc++) {
        const struct sock_filter* i = &f[pc]; uint32_t v; bool ok;
        switch (i->code) {
        case 0x00: a = i->k; break;                                                         /* ld #k */
        case 0x20: case 0x28: case 0x30: {                                                  /* ld [k] */
            unsigned size = i->code == 0x20 ? 4 : i->code == 0x28 ? 2 : 1;
            if ((int32_t)i->k >= SKF_AD_OFF && (int32_t)i->k < 0) { a = bpf_ancillary(c, (int32_t)i->k, a, x, &ok); if (!ok) return 0; break; }
            if (!bpf_load(c, (int32_t)i->k, size, &v)) return 0;
            a = v; break;
        }
        case 0x40: case 0x48: case 0x50: {                                                  /* ld [x + k] */
            unsigned size = i->code == 0x40 ? 4 : i->code == 0x48 ? 2 : 1;
            if (!bpf_load(c, (int32_t)(x + i->k), size, &v)) return 0;
            a = v; break;
        }
        case 0x60: a = mem[i->k]; break;
        case 0x80: a = c->length; break;
        case 0x01: x = i->k; break;
        case 0x61: x = mem[i->k]; break;
        case 0x81: x = c->length; break;
        case 0xB1: if (!bpf_load(c, (int32_t)i->k, 1, &v)) return 0; x = (v & 0xF) << 2; break;   /* ldxb 4*([k]&0xf) */
        case 0x02: mem[i->k] = a; break;
        case 0x03: mem[i->k] = x; break;
        case 0x04: a += i->k; break;  case 0x0C: a += x; break;
        case 0x14: a -= i->k; break;  case 0x1C: a -= x; break;
        case 0x24: a *= i->k; break;  case 0x2C: a *= x; break;
        case 0x34: a /= i->k; break;  case 0x3C: if (!x) return 0; a /= x; break;
        case 0x94: a %= i->k; break;  case 0x9C: if (!x) return 0; a %= x; break;
        case 0x44: a |= i->k; break;  case 0x4C: a |= x; break;
        case 0x54: a &= i->k; break;  case 0x5C: a &= x; break;
        case 0x64: a = i->k < 32 ? a << i->k : 0; break;  case 0x6C: a = x < 32 ? a << x : 0; break;
        case 0x74: a = i->k < 32 ? a >> i->k : 0; break;  case 0x7C: a = x < 32 ? a >> x : 0; break;
        case 0xA4: a ^= i->k; break;  case 0xAC: a ^= x; break;
        case 0x84: a = 0U - a; break;
        case 0x05: pc += i->k; break;
        case 0x15: pc += a == i->k ? i->jt : i->jf; break;  case 0x1D: pc += a == x ? i->jt : i->jf; break;
        case 0x25: pc += a > i->k ? i->jt : i->jf; break;   case 0x2D: pc += a > x ? i->jt : i->jf; break;
        case 0x35: pc += a >= i->k ? i->jt : i->jf; break;  case 0x3D: pc += a >= x ? i->jt : i->jf; break;
        case 0x45: pc += a & i->k ? i->jt : i->jf; break;   case 0x4D: pc += a & x ? i->jt : i->jf; break;
        case 0x06: return i->k;
        case 0x0E: return x;
        case 0x16: return a;
        case 0x07: x = a; break;
        case 0x87: a = x; break;
        default: return 0;
        }
    }
    return 0;
}

/* ---- receive ring ---- */
static unsigned hdrlen_of(unsigned version) { return version == 2 ? 48 + 20 : 32 + 20; }   /* TPACKET{,2,3}_HDRLEN */
static unsigned align16(unsigned v) { return (v + 15) & ~15U; }
static void ring_write(struct packet_socket* s, uint64_t offset, const void* data, unsigned length) {
    const uint8_t* p = data;
    while (length) {
        unsigned page = (unsigned)(offset / PAGE), within = (unsigned)(offset % PAGE), n = PAGE - within; if (n > length) n = length;
        if (page >= s->page_count) return;
        memcpy((uint8_t*)s->pages[page] + within, p, n); p += n; offset += n; length -= n;
    }
}
static uint32_t ring_read32(struct packet_socket* s, uint64_t offset) {
    uint32_t v = 0; uint8_t* out = (uint8_t*)&v;
    for (unsigned i = 0; i < 4; i++) { uint64_t o = offset + i; if (o / PAGE < s->page_count) out[i] = *((volatile uint8_t*)s->pages[o / PAGE] + o % PAGE); }
    return v;
}
static void ring_write32(struct packet_socket* s, uint64_t offset, uint32_t v) { __atomic_thread_fence(__ATOMIC_RELEASE); ring_write(s, offset, &v, 4); }
static void ring_free(struct packet_socket* s) {
    for (unsigned i = 0; i < s->page_count; i++) page_free(s->pages[i]);
    if (s->pages) arena_free((uint64_t)s->pages, ((uint64_t)s->page_count * 8 + PAGE - 1) & ~(uint64_t)(PAGE - 1));
    if (s->ring) memory_file_release(s->ring);
    s->ring = NULL; s->pages = NULL; s->page_count = 0; s->head_frame = 0; s->block = 0; s->block_open = s->frozen = false;
    memset(&s->req, 0, sizeof(s->req));
}
static int ring_setup(struct packet_socket* s, const struct tpacket_req3* req) {
    if (s->maps) return -16;                                    /* EBUSY while mapped */
    if (!req->block_nr) { ring_free(s); return 0; }             /* tp_block_nr 0 tears the ring down */
    if (s->ring) return -16;
    if (!req->block_size || req->block_size % PAGE || req->block_size > (1U << 22)) return -22;
    if (req->frame_size < hdrlen_of(s->version) + s->reserve || req->frame_size % 16 || req->frame_size > req->block_size) return -22;
    uint64_t pages = (uint64_t)req->block_size / PAGE * req->block_nr;
    if (pages > RING_MAX_PAGES) return -12;
    uint64_t frames_per_block = req->block_size / req->frame_size;
    if ((uint64_t)req->frame_nr != frames_per_block * req->block_nr) return -22;
    if (s->version == 2 && (req->sizeof_priv > req->block_size / 2 || req->feature_req_word)) return -22;
    struct memory_file* file = memory_file_create(2); if (!file) return -12;
    file->size = pages * PAGE; file->seals |= 16;               /* (hole punching refused: the pages stay pinned) */
    uint64_t* list = (void*)arena_alloc((pages * 8 + PAGE - 1) & ~(uint64_t)(PAGE - 1));
    if (!list) { memory_file_release(file); return -12; }
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t frame = memory_file_page(file, i, true);
        if (!frame) { while (i--) page_free(list[i]); arena_free((uint64_t)list, (pages * 8 + PAGE - 1) & ~(uint64_t)(PAGE - 1)); memory_file_release(file); return -12; }
        page_retain(frame); list[i] = (uint64_t)phys_to_virt(frame); memset((void*)list[i], 0, PAGE);
    }
    s->ring = file; s->pages = list; s->page_count = (unsigned)pages; s->req = *req;
    if (s->version == 2 && !s->req.retire_blk_tov) s->req.retire_blk_tov = 8;   /* Linux derives ~8 ms from the link speed */
    s->head_frame = 0; s->block = 0; s->block_open = false; s->frozen = false; s->block_seq = 1;
    return 0;
}
static uint64_t frame_offset(struct packet_socket* s, unsigned index) {
    unsigned per_block = s->req.block_size / s->req.frame_size;
    return (uint64_t)(index / per_block) * s->req.block_size + (uint64_t)(index % per_block) * s->req.frame_size;
}
static uint64_t block_offset_of(struct packet_socket* s, unsigned block) { return (uint64_t)block * s->req.block_size; }
static unsigned first_offset(struct packet_socket* s) { return 48 + ((s->req.sizeof_priv + 7) & ~7U); }   /* BLK_PLUS_PRIV */
static void wake(struct packet_socket* s) { task_wake_channel(s); event_source_changed(s, 1); }
static void block_open(struct packet_socket* s, uint64_t now) {
    uint64_t b = block_offset_of(s, s->block);
    uint32_t header[12] = {0};             /* (block_status stays TP_STATUS_KERNEL: the kernel owns the block) */
    header[0] = 2;                          /* version: TPACKET_V3 */
    header[1] = 48;                         /* offset_to_priv (BLK_HDR_LEN) */
    header[4] = first_offset(s);            /* offset_to_first_pkt */
    header[5] = first_offset(s);            /* blk_len */
    header[6] = s->block_seq;               /* seq_num (64-bit) */
    ring_write(s, b, header, 48);
    s->block_open = true; s->block_offset = first_offset(s); s->block_packets = 0; s->prev_packet = 0;
    s->block_opened_ns = now; s->block_first_ns = s->block_last_ns = 0;
}
static void block_close(struct packet_socket* s, bool timeout) {
    uint64_t b = block_offset_of(s, s->block);
    if (s->block_packets) ring_write32(s, b + s->prev_packet, 0);   /* the last packet's tp_next_offset */
    uint32_t values[6] = {s->block_packets, first_offset(s), s->block_offset, s->block_seq, 0, 0};
    ring_write(s, b + 12, values, 16);      /* num_pkts, offset_to_first_pkt, blk_len, seq_num low */
    uint64_t first = s->block_first_ns ? s->block_first_ns : native_realtime_ns(), last = s->block_last_ns ? s->block_last_ns : first;
    uint32_t stamps[4] = {(uint32_t)(first / 1000000000ULL), (uint32_t)(first % 1000000000ULL), (uint32_t)(last / 1000000000ULL), (uint32_t)(last % 1000000000ULL)};
    ring_write(s, b + 32, stamps, 16);      /* ts_first_pkt, ts_last_pkt */
    ring_write32(s, b + 8, TP_STATUS_USER | (timeout ? TP_STATUS_BLK_TMO : 0));   /* block_status, published last */
    s->block_seq++; s->block_open = false;
    s->block = (s->block + 1) % s->req.block_nr;
    wake(s);
}
/* V3: the active block, opening it when the kernel owns it; false while the ring is frozen (all blocks with user) */
static bool block_ready(struct packet_socket* s, uint64_t now) {
    if (s->block_open) return true;
    if (ring_read32(s, block_offset_of(s, s->block) + 8) != TP_STATUS_KERNEL) { if (!s->frozen) s->freeze++; s->frozen = true; return false; }
    s->frozen = false; block_open(s, now); return true;
}
static void ring_deliver(struct packet_socket* s, const uint8_t* data, unsigned length, unsigned original, unsigned maclen,
                         const struct sockaddr_ll* sll, uint64_t stamp) {
    unsigned hdrlen = hdrlen_of(s->version), macoff, netoff;
    if (s->type == 2) { netoff = align16(hdrlen) + 16 + s->reserve; macoff = netoff; }
    else { netoff = align16(hdrlen + (maclen < 16 ? 16 : maclen)) + s->reserve; macoff = netoff - maclen; }
    unsigned frame = s->req.frame_size, snap = length;
    if (macoff + snap > frame) snap = macoff < frame ? frame - macoff : 0;
    uint32_t status = TP_STATUS_USER | (snap < original ? TP_STATUS_COPY : 0) | (s->drops ? TP_STATUS_LOSING : 0);
    uint32_t sec = (uint32_t)(stamp / 1000000000ULL), nsec = (uint32_t)(stamp % 1000000000ULL);
    uint64_t at;
    if (s->version == 2) {
        uint64_t now = native_clock_ns();
        if (!block_ready(s, now)) { s->drops++; return; }
        unsigned need = (macoff + snap + 7) & ~7U;
        if (s->block_offset + need > s->req.block_size) {
            if (!s->block_packets) { snap = s->req.block_size - s->block_offset > macoff ? s->req.block_size - s->block_offset - macoff : 0; need = (macoff + snap + 7) & ~7U; }
            else { block_close(s, false); if (!block_ready(s, now)) { s->drops++; return; } }
        }
        if (s->block_offset + macoff > s->req.block_size) { s->drops++; return; }
        at = block_offset_of(s, s->block) + s->block_offset;
        uint32_t h[12] = {0};
        h[0] = need;                         /* tp_next_offset (0 for the last one, set at close) */
        h[1] = sec; h[2] = nsec; h[3] = snap; h[4] = original; h[5] = status;
        h[6] = macoff | (uint32_t)netoff << 16;   /* tp_mac, tp_net */
        ring_write(s, at, h, 48);
        ring_write(s, at + 48, sll, sizeof(*sll));
        ring_write(s, at + macoff, data, snap);
        s->prev_packet = s->block_offset; s->block_offset += need; s->block_packets++;
        if (!s->block_first_ns) s->block_first_ns = stamp;
        s->block_last_ns = stamp;
        s->packets++;
        if (s->block_offset + 64 > s->req.block_size) block_close(s, false);
        return;
    }
    at = frame_offset(s, s->head_frame);
    if (ring_read32(s, at) != TP_STATUS_KERNEL) { s->drops++; return; }   /* the ring is full */
    if (s->version == 1) {   /* tpacket2_hdr */
        uint32_t h[8] = {0, original, snap, macoff | (uint32_t)netoff << 16, sec, nsec, 0, 0};
        ring_write(s, at + 4, h + 1, 28);
        ring_write(s, at + 32, sll, sizeof(*sll));
    } else {                 /* tpacket_hdr (V1): unsigned long tp_status, tp_len, tp_snaplen, tp_mac/net, tp_sec, tp_usec */
        uint32_t h[7] = {original, snap, macoff | (uint32_t)netoff << 16, sec, nsec / 1000, 0, 0};
        ring_write(s, at + 8, h, 20);
        ring_write(s, at + 32, sll, sizeof(*sll));
    }
    ring_write(s, at + macoff, data, snap);
    ring_write32(s, at, status);
    s->head_frame = (s->head_frame + 1) % s->req.frame_nr; s->packets++;
    wake(s);
}
static bool ring_readable(struct packet_socket* s) {
    if (!s->ring) return false;
    if (s->version == 2) {
        unsigned previous = (s->block + s->req.block_nr - 1) % s->req.block_nr;
        return ring_read32(s, block_offset_of(s, previous) + 8) != TP_STATUS_KERNEL;
    }
    unsigned previous = (s->head_frame + s->req.frame_nr - 1) % s->req.frame_nr;
    return ring_read32(s, frame_offset(s, previous)) != TP_STATUS_KERNEL;
}

/* ---- the network hook ---- */
static unsigned charge_of(unsigned length) { return length + 256; }
static unsigned header_length(const struct netdev* dev) { return dev->type == ARPHRD_ETHER || dev->type == ARPHRD_LOOPBACK ? ETH_HEADER_LEN : 0; }
void packet_tap(struct netdev* dev, const uint8_t* frame, unsigned length, uint8_t pkttype, uint16_t protocol, bool outgoing) {
    unsigned maclen = header_length(dev);
    if (length < maclen) return;
    uint64_t stamp = 0;

    for (struct packet_socket* s = sockets; s; s = s->next) {
        if (!s->protocol || s->closed) continue;
        uint16_t wanted = be16((const uint8_t*)&s->protocol);
        if (wanted != ETH_P_ALL && (outgoing || wanted != protocol)) continue;   /* only ETH_P_ALL taps see transmissions */
        if (s->ifindex && s->ifindex != dev->index) continue;
        if (outgoing && (s->ignore_outgoing || s == sending)) continue;
        const uint8_t* data = frame; unsigned size = length, network = maclen;
        if (s->type == 2) { data = frame + maclen; size = length - maclen; network = 0; }
        unsigned snap = size;
        if (s->filter) {
            struct bpf_context c = {data, size, network, protocol, dev->type, pkttype, dev->index, 0};
            unsigned verdict = bpf_run(s->filter, s->filter_length, &c);
            if (!verdict) continue;
            if (verdict < snap) snap = verdict;
        }
        if (!stamp) stamp = native_realtime_ns();
        struct sockaddr_ll sll = {AF_PACKET, 0, (int32_t)dev->index, dev->type, pkttype, 0, {0}};
        put16((uint8_t*)&sll.protocol, protocol);
        if (maclen == ETH_HEADER_LEN && dev->type == ARPHRD_ETHER) { sll.halen = 6; memcpy(sll.addr, frame + 6, 6); }   /* the header's source */
        if (s->ring) { ring_deliver(s, data, snap, size, s->type == 2 ? 0 : maclen, &sll, stamp); continue; }
        unsigned charge = charge_of(snap);
        if (s->queued + charge > s->receive_buffer) { s->drops++; continue; }
        struct pframe* p = net_alloc(sizeof(*p) + snap); if (!p) { s->drops++; continue; }
        memset(p, 0, sizeof(*p));
        p->length = snap; p->original = size; p->charge = charge; p->ifindex = dev->index; p->offset = network;
        p->protocol = protocol; p->hatype = dev->type; p->pkttype = pkttype; p->halen = sll.halen; memcpy(p->addr, sll.addr, 8); p->stamp_ns = stamp;
        memcpy(p->data, data, snap);
        if (s->tail) s->tail->next = p; else s->head = p;
        s->tail = p; s->queued += charge; s->count++; s->packets++;
        wake(s);
    }
}
/* V3 block retirement timer (netdev_poll) */
void packet_poll(uint64_t now) {
    for (struct packet_socket* s = sockets; s; s = s->next) {
        if (!s->ring || s->version != 2) continue;
        if (s->block_open && s->block_packets && now - s->block_opened_ns >= (uint64_t)s->req.retire_blk_tov * 1000000ULL) block_close(s, true);
        else if (!s->block_open && s->frozen) block_ready(s, now);
    }
}
static void membership_apply(struct netdev* dev, const struct membership* m, int delta);
void packet_dev_down(struct netdev* dev) {
    /* an unregistered interface: bound sockets report ENETDOWN, memberships on it go */
    for (struct packet_socket* s = sockets; s; s = s->next) {
        if (s->ifindex == dev->index && !dev->present) { s->error = 100; wake(s); }
        if (dev->present) continue;
        for (unsigned i = 0; i < MEMBERSHIPS; i++) if (s->members[i].count && s->members[i].ifindex == dev->index) {
            while (s->members[i].count) { membership_apply(dev, &s->members[i], -1); s->members[i].count--; }
        }
    }
}

/* ---- memberships ---- */
static void membership_apply(struct netdev* dev, const struct membership* m, int delta) {
    if (m->type == 1) netdev_promiscuity(dev, delta);          /* PACKET_MR_PROMISC */
    else if (m->type == 2) netdev_allmulti(dev, delta);        /* PACKET_MR_ALLMULTI */
    else if (m->type == 0 || m->type == 3) netdev_allmulti(dev, delta);   /* a link multicast/unicast address: all-multicast (imperfect filter) */
}
static int membership(struct packet_socket* s, const struct packet_mreq* q, bool add) {
    if (q->type > 3 || q->alen > 8) return -22;
    struct netdev* dev = netdev_by_index((unsigned)q->ifindex); if (!dev || !dev->present) return -19;
    for (unsigned i = 0; i < MEMBERSHIPS; i++) {
        struct membership* m = &s->members[i];
        if (!m->count || m->ifindex != dev->index || m->type != q->type || m->alen != q->alen || memcmp(m->address, q->address, q->alen)) continue;
        if (add) { m->count++; membership_apply(dev, m, 1); return 0; }
        m->count--; membership_apply(dev, m, -1); return 0;
    }
    if (!add) return -99;   /* EADDRNOTAVAIL */
    for (unsigned i = 0; i < MEMBERSHIPS; i++) {
        struct membership* m = &s->members[i];
        if (m->count) continue;
        m->ifindex = dev->index; m->type = q->type; m->alen = q->alen; memcpy(m->address, q->address, q->alen); m->count = 1;
        membership_apply(dev, m, 1); return 0;
    }
    return -105;
}

/* ---- object operations ---- */
static void frame_free(struct packet_socket* s, struct pframe* p) { s->queued -= p->charge; s->count--; net_free(p, sizeof(*p) + p->length); }
static uint32_t packet_ready(void* object) {
    struct packet_socket* s = object;
    return (s->head || ring_readable(s) ? 1U : 0) | 4U | (s->error ? 8U : 0);
}
static void* packet_source(void* object) { return object; }
static void packet_release(void* object) {
    struct packet_socket* s = object;
    for (struct packet_socket** link = &sockets; *link; link = &(*link)->next) if (*link == s) { *link = s->next; break; }
    packet_socket_count--;
    for (unsigned i = 0; i < MEMBERSHIPS; i++) if (s->members[i].count) {
        struct netdev* dev = netdev_by_index(s->members[i].ifindex);
        while (dev && s->members[i].count) { membership_apply(dev, &s->members[i], -1); s->members[i].count--; }
    }
    while (s->head) { struct pframe* p = s->head; s->head = p->next; frame_free(s, p); }
    if (s->filter) net_free(s->filter, s->filter_length * sizeof(struct sock_filter));
    ring_free(s);
    net_free(s, sizeof(*s));
}
static void packet_stat(void* object, struct linux_stat* stat) {
    memset(stat, 0, sizeof(*stat)); stat->dev = 4; stat->ino = 0x50000000ULL + ((struct packet_socket*)object)->id; stat->nlink = 1; stat->mode = 0140777;
    stat->uid = credentials_visible_id(credentials_kernel_id(credentials_current(), false, 1), false);
    stat->gid = credentials_visible_id(credentials_kernel_id(credentials_current(), true, 1), true); stat->block_size = 4096;
}
static int64_t packet_send(struct packet_socket* s, const struct iovec64* vectors, unsigned count, uint64_t total, const struct sockaddr_ll* to);
static int64_t packet_receive(struct packet_socket* s, const struct iovec64* vectors, unsigned count, uint64_t total, uint64_t flags,
                              uint64_t descriptor_flags, struct message64* message, uint64_t name, uint32_t* name_size);
static int64_t packet_io(void* object, const struct iovec64* vectors, unsigned count, uint64_t total, bool write, uint64_t flags) {
    return write ? packet_send(object, vectors, count, total, NULL) : packet_receive(object, vectors, count, total, 0, flags, NULL, 0, NULL);
}
static int packet_ioctl(void* object, uint64_t command, uint64_t argument, uint64_t flags) {
    (void)flags; struct packet_socket* s = object;
    if ((uint32_t)command == 0x541B) {                       /* FIONREAD */
        if (!accessible(argument, 4, true)) return -14;
        return uput32(argument, (uint32_t)(s->head ? (int)s->head->length : 0));
    }
    if ((uint32_t)command == 0x8906 || (uint32_t)command == 0x8907) {   /* SIOCGSTAMP(NS) */
        if (!s->last_stamp_ns) return -2;
        if (!accessible(argument, 16, true)) return -14;
        int64_t t[2] = {(int64_t)(s->last_stamp_ns / 1000000000ULL), (int64_t)(s->last_stamp_ns % 1000000000ULL)};
        if ((uint32_t)command == 0x8906) t[1] /= 1000;
        return copy_to_user((void*)argument, t, 16) ? -14 : 0;
    }
    if (((uint32_t)command & 0xFF00) == 0x8900) return netif_ioctl(command, argument);
    return -25;
}
static const struct object_operations packet_operations = {packet_io, packet_ioctl, packet_ready, packet_source, packet_release, packet_stat};
bool packet_descriptor(struct open_file* description) { return fd_object(description, &packet_operations) != NULL; }
/* mmap() of the receive ring */
struct memory_file* packet_mapping(struct open_file* description, uint64_t* offset, uint64_t length) {
    struct packet_socket* s = fd_object(description, &packet_operations);
    if (!s || !s->ring || *offset || length > (uint64_t)s->page_count * PAGE) return NULL;
    s->maps++;   /* (Linux keeps the ring busy for as long as it was ever mapped: mapped_count, here until close) */
    return s->ring;
}

/* ---- send / receive ---- */
static int64_t packet_send(struct packet_socket* s, const struct iovec64* vectors, unsigned count, uint64_t total, const struct sockaddr_ll* to) {
    unsigned ifindex = to && to->ifindex ? (unsigned)to->ifindex : s->ifindex;
    if (!ifindex) return -6;                                    /* ENXIO: no interface */
    struct netdev* dev = netdev_by_index(ifindex); if (!dev || !dev->present) return -6;
    if (!(dev->flags & IFF_UP)) return -100;
    uint16_t protocol = to && to->protocol ? be16((const uint8_t*)&to->protocol) : be16((const uint8_t*)&s->protocol);
    unsigned maclen = header_length(dev), limit = dev->mtu + (s->type == 3 ? maclen : 0);
    if (total > limit) return -90;
    if (s->type == 3 && total < maclen) return -22;
    if (s->type == 2 && dev->type == ARPHRD_ETHER && (!to || to->halen < 6)) return -22;   /* the destination address */
    uint8_t small[ETH_HEADER_LEN + 1536]; bool big = ETH_HEADER_LEN + total > sizeof(small);
    uint8_t* frame = big ? (void*)arena_alloc(ETH_HEADER_LEN + 65536) : small; if (!frame) return -12;
    unsigned at = 0;
    if (s->type == 2 && maclen) { memcpy(frame, to->addr, 6); memcpy(frame + 6, dev->addr, 6); put16(frame + 12, protocol); at = ETH_HEADER_LEN; }
    if (total && !iov_access(vectors, count, 0, total, false)) { if (big) arena_free((uint64_t)frame, ETH_HEADER_LEN + 65536); return -14; }
    if (total) iov_copy(vectors, count, 0, frame + at, (unsigned)total, true);    /* (true: from user memory) */
    unsigned length = at + (unsigned)total;
    if (maclen && s->type == 3) protocol = be16(frame + 12);
    if (!maclen && !protocol && length) protocol = (frame[0] >> 4) == 6 ? ETH_TYPE_IPV6 : ETH_TYPE_IPV4;
    struct packet_socket* saved = sending; sending = s;
    int result = netdev_xmit(dev, frame, length, protocol);
    sending = saved;
    if (big) arena_free((uint64_t)frame, ETH_HEADER_LEN + 65536);
    if (result == -11) return -105;
    return result < 0 ? result : (int64_t)total;
}
static void control_put(uint8_t* buffer, unsigned capacity, unsigned* used, int32_t level, int32_t type, const void* data, unsigned length, bool* truncated) {
    unsigned space = 16 + ((length + 7) & ~7U), needed = 16 + length;
    if (*used + needed > capacity) { *truncated = true; return; }
    uint64_t header = 16 + length; memcpy(buffer + *used, &header, 8); memcpy(buffer + *used + 8, &level, 4); memcpy(buffer + *used + 12, &type, 4);
    memcpy(buffer + *used + 16, data, length);
    if (*used + space <= capacity) { memset(buffer + *used + needed, 0, space - needed); *used += space; } else *used = capacity;
}
static int64_t packet_receive(struct packet_socket* s, const struct iovec64* vectors, unsigned count, uint64_t total, uint64_t flags,
                              uint64_t descriptor_flags, struct message64* message, uint64_t name, uint32_t* name_size) {
    if (s->error) { int e = s->error; s->error = 0; return -e; }
    uint64_t deadline = s->receive_timeout ? native_clock_ns() + s->receive_timeout : 0;
    while (!s->head) {
        if ((flags & 0x40) || (descriptor_flags & 0x800)) return -11;
        int error = deadline ? task_wait_until(s, deadline) : task_wait_channel(s);
        if (error) return error == -110 ? -11 : error;
        if (s->error) { int e = s->error; s->error = 0; return -e; }
    }
    struct pframe* p = s->head;
    unsigned amount = total < p->length ? (unsigned)total : p->length;
    if (amount && !iov_access(vectors, count, 0, amount, true)) return -14;
    if (amount) iov_copy(vectors, count, 0, p->data, amount, false);
    if (name && name_size) {
        struct sockaddr_ll sll = {AF_PACKET, 0, (int32_t)p->ifindex, p->hatype, p->pkttype, p->halen, {0}};
        put16((uint8_t*)&sll.protocol, p->protocol); memcpy(sll.addr, p->addr, 8);
        unsigned n = *name_size < sizeof(sll) ? *name_size : (unsigned)sizeof(sll);
        if (n && !accessible(name, n, true)) return -14;
        if (n) copy_to_user((void*)name, &sll, n);
        *name_size = sizeof(sll);   /* Linux pads the address to the full sockaddr_ll */
    }
    if (message) {
        uint8_t control[96]; unsigned used = 0; bool truncated = false;
        unsigned capacity = message->control_size < sizeof(control) ? (unsigned)message->control_size : (unsigned)sizeof(control);
        if (s->timestamp || s->timestamp_ns) {
            int64_t t[2] = {(int64_t)(p->stamp_ns / 1000000000ULL), (int64_t)(p->stamp_ns % 1000000000ULL)};
            if (!s->timestamp_ns) t[1] /= 1000;
            control_put(control, capacity, &used, 1, s->timestamp_ns ? 35 : 29, t, 16, &truncated);
        }
        if (s->auxdata) {
            struct tpacket_auxdata aux = {TP_STATUS_USER, p->original, p->length, 0, (uint16_t)p->offset, 0, 0};
            control_put(control, capacity, &used, SOL_PACKET, 8, &aux, sizeof(aux), &truncated);
        }
        if (used && !accessible(message->control, used, true)) return -14;
        if (used) copy_to_user((void*)message->control, control, used);
        message->control_size = used;
        message->flags = (p->length > total ? 0x20U : 0) | (truncated ? 8U : 0);   /* MSG_TRUNC, MSG_CTRUNC */
    }
    unsigned length = p->length;
    s->last_stamp_ns = p->stamp_ns;
    if (!(flags & 2)) { s->head = p->next; if (!s->head) s->tail = NULL; frame_free(s, p); }   /* MSG_PEEK keeps it */
    return (flags & 0x20) ? length : amount;
}

/* ---- socket calls ---- */
static int name_load(uint64_t address, uint64_t size, struct sockaddr_ll* name) {
    if (size < 12) return -22;
    memset(name, 0, sizeof(*name));
    unsigned n = size < sizeof(*name) ? (unsigned)size : (unsigned)sizeof(*name);
    if (!accessible(address, n, false)) return -14;
    copy_from_user(name, (void*)address, n);
    return name->family == AF_PACKET ? 0 : -22;
}
static int create(uint64_t type, uint64_t protocol) {
    unsigned kind = type & 0xF;
    if ((type & ~(0xFULL | 0x800ULL | 0x80000ULL)) || (kind != 2 && kind != 3 && kind != 10)) return -94;
    if (kind == 10) return -94;                                  /* SOCK_PACKET (obsolete) */
    if (!net_raw_capable()) return -1;
    if (protocol > 0xFFFF) return -22;
    struct packet_socket* s = net_alloc(sizeof(*s)); if (!s) return -12;
    memset(s, 0, sizeof(*s)); s->id = ++next_id; s->type = kind; s->protocol = (uint16_t)protocol;
    s->receive_buffer = s->send_buffer = 212992;
    int fd = fd_create_object(&packet_operations, s, (type & 0x800) | (type & 0x80000) | 2, "socket:[packet]");
    if (fd < 0) { net_free(s, sizeof(*s)); return fd; }
    s->next = sockets; sockets = s; packet_socket_count++;
    return fd;
}
static int put_int(uint64_t value_address, uint64_t size_address, uint32_t value) {
    if (!accessible(size_address, 4, true)) return -14;
    int32_t capacity = (int32_t)uget32(size_address); if (capacity < 0) return -22;
    unsigned n = capacity < 4 ? (unsigned)capacity : 4;
    if (n && !accessible(value_address, n, true)) return -14;
    if (n) copy_to_user((void*)value_address, &value, n);
    return uput32(size_address, n);
}
static int set_option(struct packet_socket* s, uint64_t level, uint64_t option, uint64_t address, uint64_t size) {
    uint32_t value = 0;
    if (size >= 4 && accessible(address, 4, false)) copy_from_user(&value, (void*)address, 4);
    if (level == 1) {
        switch (option) {
        case 26: {                                                   /* SO_ATTACH_FILTER */
            if (size < 16 || !accessible(address, 16, false)) return size < 16 ? -22 : -14;
            if (s->filter_locked) return -1;
            uint16_t n = uget16(address); uint64_t program = uget64(address + 8);
            if (!n || n > FILTER_MAX) return -22;
            if (!accessible(program, (uint64_t)n * sizeof(struct sock_filter), false)) return -14;
            struct sock_filter* copy = net_alloc(n * sizeof(struct sock_filter)); if (!copy) return -12;
            copy_from_user(copy, (void*)program, n * sizeof(struct sock_filter));
            if (!bpf_check(copy, n)) { net_free(copy, n * sizeof(struct sock_filter)); return -22; }
            if (s->filter) net_free(s->filter, s->filter_length * sizeof(struct sock_filter));
            s->filter = copy; s->filter_length = n;
            return 0;
        }
        case 27:                                                     /* SO_DETACH_FILTER */
            if (s->filter_locked) return -1;
            if (!s->filter) return -2;
            net_free(s->filter, s->filter_length * sizeof(struct sock_filter)); s->filter = NULL; s->filter_length = 0; return 0;
        case 44: if (size < 4) return -22; if (!value && s->filter_locked) return -1; s->filter_locked = value != 0; return 0;   /* SO_LOCK_FILTER */
        case 8: case 33: if (size < 4) return -22; s->receive_buffer = value > 0x3FFFFFFF ? 0x7FFFFFFE : (value * 2 < 2304 ? 2304 : value * 2); return 0;
        case 7: case 32: if (size < 4) return -22; s->send_buffer = value > 0x3FFFFFFF ? 0x7FFFFFFE : (value * 2 < 4608 ? 4608 : value * 2); return 0;
        case 29: if (size < 4) return -22; s->timestamp = value != 0; if (value) s->timestamp_ns = false; return 0;   /* SO_TIMESTAMP */
        case 35: if (size < 4) return -22; s->timestamp_ns = value != 0; if (value) s->timestamp = false; return 0;  /* SO_TIMESTAMPNS */
        case 36: if (size < 4) return -22; if (!net_raw_capable() && !netlink_admin()) return -1; s->mark = value; return 0;   /* SO_MARK */
        case 12: if (size < 4) return -22; s->priority = value; return 0;
        case 20: {                                                   /* SO_RCVTIMEO */
            if (size < 16 || !accessible(address, 16, false)) return -22;
            int64_t t[2]; copy_from_user(t, (void*)address, 16); if (t[0] < 0 || t[1] < 0 || t[1] >= 1000000) return -33;
            s->receive_timeout = (uint64_t)t[0] * 1000000000ULL + (uint64_t)t[1] * 1000ULL; return 0;
        }
        case 25: {                                                   /* SO_BINDTODEVICE */
            char name[IFNAMSIZ] = {0}; unsigned n = size < IFNAMSIZ - 1 ? (unsigned)size : IFNAMSIZ - 1;
            if (n && !accessible(address, n, false)) return -14;
            if (n) copy_from_user(name, (void*)address, n);
            if (!name[0]) { s->ifindex = 0; return 0; }
            struct netdev* d = netdev_by_name(name); if (!d) return -19;
            s->ifindex = d->index; return 0;
        }
        case 2: case 9: case 15: case 46: case 47: case 49: case 50: case 69: return 0;   /* REUSEADDR, KEEPALIVE, REUSEPORT, busy poll, ... */
        }
        return -92;
    }
    if (level != SOL_PACKET) return -92;
    switch (option) {
    case 1: case 2: {                                                /* PACKET_ADD/DROP_MEMBERSHIP (packet_mreq or packet_mreq_max) */
        if (size < sizeof(struct packet_mreq) || !accessible(address, sizeof(struct packet_mreq), false)) return size < sizeof(struct packet_mreq) ? -22 : -14;
        struct packet_mreq q; copy_from_user(&q, (void*)address, sizeof(q));
        return membership(s, &q, option == 1);
    }
    case 5: {                                                        /* PACKET_RX_RING: tpacket_req or tpacket_req3 */
        unsigned need = s->version == 2 ? 28 : 16;
        if (size < need || !accessible(address, need, false)) return size < need ? -22 : -14;
        struct tpacket_req3 req; memset(&req, 0, sizeof(req)); copy_from_user(&req, (void*)address, need);
        return ring_setup(s, &req);
    }
    case 13: return -22;                                             /* PACKET_TX_RING: not supported */
    case 8: if (size < 4) return -22; s->auxdata = value != 0; return 0;
    case 9: if (size < 4) return -22; s->origdev = value != 0; return 0;
    case 10: if (size < 4) return -22; if (s->ring) return -16; if (value > 2) return -22; s->version = value; return 0;   /* PACKET_VERSION */
    case 12: if (size < 4) return -22; if (s->ring) return -16; if (value > 65535) return -22; s->reserve = value; return 0;   /* PACKET_RESERVE */
    case 14: case 16: case 17: case 21: if (size < 4) return -22; return 0;   /* LOSS, TX_TIMESTAMP, TIMESTAMP, QDISC_BYPASS */
    case 23: if (size < 4) return -22; s->ignore_outgoing = value != 0; return 0;   /* PACKET_IGNORE_OUTGOING */
    case 15: case 18: case 22: return -95;                           /* VNET_HDR, FANOUT, FANOUT_DATA */
    }
    return -92;
}
static int get_option(struct packet_socket* s, uint64_t level, uint64_t option, uint64_t address, uint64_t size_address) {
    if (level == 1) {
        uint32_t v;
        switch (option) {
        case 3: v = s->type; break;
        case 4: v = (uint32_t)s->error; s->error = 0; break;
        case 8: v = s->receive_buffer; break;
        case 7: v = s->send_buffer; break;
        case 38: v = be16((const uint8_t*)&s->protocol); break;
        case 39: v = AF_PACKET; break;
        case 29: v = s->timestamp; break;
        case 35: v = s->timestamp_ns; break;
        case 36: v = s->mark; break;
        case 12: v = s->priority; break;
        case 44: v = s->filter_locked; break;
        case 48: v = 64; break;                                      /* SO_BPF_EXTENSIONS: SKF_AD_MAX */
        default: return -92;
        }
        return put_int(address, size_address, v);
    }
    if (level != SOL_PACKET) return -92;
    switch (option) {
    case 6: {                                                        /* PACKET_STATISTICS (reset on read; drops count as received) */
        uint32_t stats[3] = {s->packets + s->drops, s->drops, s->freeze};
        unsigned n = s->version == 2 ? 12 : 8;
        if (!accessible(size_address, 4, true)) return -14;
        int32_t capacity = (int32_t)uget32(size_address); if (capacity < 0) return -22;
        if ((unsigned)capacity < n) n = (unsigned)capacity;
        if (n && !accessible(address, n, true)) return -14;
        if (n) copy_to_user((void*)address, stats, n);
        uput32(size_address, n); s->packets = s->drops = s->freeze = 0; return 0;
    }
    case 11: {                                                       /* PACKET_HDRLEN: in = version */
        if (!accessible(size_address, 4, true)) return -14;
        if ((int32_t)uget32(size_address) < 4 || !accessible(address, 4, true)) return -22;
        uint32_t version = uget32(address); if (version > 2) return -22;
        return put_int(address, size_address, hdrlen_of(version));
    }
    case 8: return put_int(address, size_address, s->auxdata);
    case 9: return put_int(address, size_address, s->origdev);
    case 10: return put_int(address, size_address, s->version);
    case 12: return put_int(address, size_address, s->reserve);
    case 23: return put_int(address, size_address, s->ignore_outgoing);
    case 14: case 17: case 21: return put_int(address, size_address, 0);
    }
    return -92;
}
static int bind_to(struct packet_socket* s, const struct sockaddr_ll* name) {
    if (name->ifindex < 0) return -19;
    if (name->ifindex) { struct netdev* d = netdev_by_index((unsigned)name->ifindex); if (!d || !d->present) return -19; }
    s->ifindex = (unsigned)name->ifindex;
    if (name->protocol) s->protocol = name->protocol;
    struct netdev* d = s->ifindex ? netdev_by_index(s->ifindex) : NULL;
    if (d && !(d->flags & IFF_UP)) s->error = 100;   /* ENETDOWN on the next call (as libpcap checks with SO_ERROR) */
    return 0;
}
static int name_store(struct packet_socket* s, uint64_t address, uint64_t size_address) {
    if (!accessible(size_address, 4, true)) return -14;
    struct netdev* d = s->ifindex ? netdev_by_index(s->ifindex) : NULL;
    struct sockaddr_ll sll = {AF_PACKET, s->protocol, (int32_t)s->ifindex, d ? d->type : 0, 0, 0, {0}};
    if (d && d->type == ARPHRD_ETHER) { sll.halen = 6; memcpy(sll.addr, d->addr, 6); }
    int32_t capacity = (int32_t)uget32(size_address); if (capacity < 0) return -22;
    unsigned full = 12 + sll.halen;   /* offsetof(sll_addr) + halen */
    unsigned n = (unsigned)capacity < sizeof(sll) ? (unsigned)capacity : (unsigned)sizeof(sll);
    if (n && !accessible(address, n, true)) return -14;
    if (n) copy_to_user((void*)address, &sll, n);
    return uput32(size_address, full);
}
int64_t packet_call(struct registers* r) {
    if (r->rax == 41) return create(r->rsi, r->rdx);
    struct open_file* description = fd_lookup(r->rdi); if (!description) return -9;
    struct packet_socket* s = fd_object(description, &packet_operations); if (!s) return -88;
    uint64_t descriptor_flags = fd_status_flags(description);
    int64_t result;
    fd_pin(description);
    switch (r->rax) {
    case 49: { struct sockaddr_ll name; result = name_load(r->rsi, r->rdx, &name); if (!result) result = bind_to(s, &name); break; }
    case 51: result = name_store(s, r->rsi, r->rdx); break;
    case 42: case 52: case 43: case 288: case 50: result = -95; break;     /* connect, getpeername, accept, listen */
    case 48: result = 0; break;                                            /* shutdown */
    case 44: {                                                             /* sendto */
        struct sockaddr_ll to, *target = NULL;
        if (r->r8) { result = name_load(r->r8, r->r9, &to); if (result) break; target = &to; }
        struct iovec64 vector = {r->rsi, r->rdx}; result = packet_send(s, &vector, 1, r->rdx, target); break;
    }
    case 45: {                                                             /* recvfrom */
        struct iovec64 vector = {r->rsi, r->rdx}; uint32_t size = 0;
        if (r->r8) { if (!accessible(r->r9, 4, true)) { result = -14; break; } size = uget32(r->r9); if ((int32_t)size < 0) { result = -22; break; } }
        result = packet_receive(s, &vector, 1, r->rdx, r->r10, descriptor_flags, NULL, r->r8, r->r8 ? &size : NULL);
        if (result >= 0 && r->r8) { if (!accessible(r->r9, 4, true)) { result = -14; break; } uput32(r->r9, size); }
        break;
    }
    case 46: case 47: {                                                    /* sendmsg / recvmsg */
        if (!accessible(r->rsi, sizeof(struct message64), r->rax == 47)) { result = -14; break; }
        struct message64 message; copy_from_user(&message, (void*)r->rsi, sizeof(message));
        if (message.count > 1024) { result = -22; break; }
        struct iovec64 small[64], *vectors = small; uint64_t total = 0;
        if (message.count > 64) { result = -22; break; }
        if (message.count && !accessible(message.vectors, message.count * sizeof(struct iovec64), false)) { result = -14; break; }
        if (message.count && copy_from_user(vectors, (void*)message.vectors, message.count * sizeof(struct iovec64))) { result = -14; break; }
        bool overflow = false;
        for (uint64_t i = 0; i < message.count; i++) { if (vectors[i].size > 0x7FFFFFFF - total) overflow = true; else total += vectors[i].size; }
        if (overflow) { result = -22; break; }
        if (r->rax == 46) {
            struct sockaddr_ll to, *target = NULL;
            if (message.name) { result = name_load(message.name, message.name_size, &to); if (result) break; target = &to; }
            result = packet_send(s, vectors, (unsigned)message.count, total, target); break;
        }
        uint32_t name_size = message.name ? message.name_size : 0;
        result = packet_receive(s, vectors, (unsigned)message.count, total, r->rdx, descriptor_flags, &message, message.name, message.name ? &name_size : NULL);
        if (result >= 0) {
            message.name_size = name_size;
            if (!accessible(r->rsi, sizeof(message), true)) { result = -14; break; }
            copy_to_user((void*)r->rsi, &message, sizeof(message));
        }
        break;
    }
    case 54: result = set_option(s, r->rsi, r->rdx, r->r10, r->r8); break;
    case 55: result = get_option(s, r->rsi, r->rdx, r->r10, r->r8); break;
    default: result = -95;
    }
    fd_unpin(description);
    return result;
}

/* /proc/net/packet */
unsigned packet_proc_lines(char* out, unsigned capacity) {
    unsigned used = 0; out[0] = 0;
    for (struct packet_socket* s = sockets; s; s = s->next) {
        char line[128], number[24]; unsigned n = 0;
        static const char hex[] = "0123456789abcdef";
        uint64_t id = 0xffff800000000000ULL | s->id << 8;
        for (int i = 15; i >= 0; i--) line[n++] = hex[(id >> (i * 4)) & 15];
        line[n++] = ' ';
        const char* fields[] = {"3     ", s->type == 3 ? "3    " : "2    "};
        for (unsigned f = 0; f < 2; f++) { strcpy(line + n, fields[f]); n += strlen(fields[f]); }
        uint16_t p = be16((const uint8_t*)&s->protocol);
        for (int i = 3; i >= 0; i--) line[n++] = hex[(p >> (i * 4)) & 15];
        line[n++] = ' '; line[n++] = ' '; line[n++] = ' ';
        uint_to_str(s->ifindex, number); strcpy(line + n, number); n += strlen(number);
        while (n < 41) line[n++] = ' ';
        line[n++] = s->protocol ? '1' : '0'; line[n++] = ' ';
        uint_to_str(s->queued, number); strcpy(line + n, number); n += strlen(number); line[n++] = ' ';
        uint_to_str(0, number); strcpy(line + n, number); n += strlen(number); line[n++] = ' ';
        uint_to_str((unsigned)(0x50000000U + s->id), number); strcpy(line + n, number); n += strlen(number);
        line[n++] = '\n'; line[n] = 0;
        if (used + n + 1 > capacity) break;
        memcpy(out + used, line, n + 1); used += n;
    }
    return used;
}
