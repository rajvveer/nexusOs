/* TUN/TAP virtual interfaces (Linux tuntap) and the dummy device. A userspace VPN (OpenVPN, wireguard-go, ...) opens
 * /dev/net/tun, TUNSETIFF names an IFF_TUN (L3) or IFF_TAP (L2) interface, and then reads frames the kernel routed to
 * that interface and writes decapsulated frames back into the stack. Interfaces are also created over rtnetlink
 * (`ip link add type tun|tap|dummy`, with TUNSETPERSIST / IFLA_TUN_OWNER). Everything runs under the kernel lock.
 *
 * ioctls: TUNSETIFF, TUNGETIFF, TUNSETPERSIST, TUNSETOWNER, TUNSETGROUP, TUNGETFEATURES, TUNSETOFFLOAD (accepted),
 * TUNGETSNDBUF/TUNSETSNDBUF, TUNSETLINK, plus the shared SIOCGIF / SIOCSIF interface ioctls through netif_ioctl.
 * IFF_NO_PI drops the 4-byte packet-information header; otherwise each frame carries it (flags + protocol). */
#include "netdev.h"
#include "nlmsg.h"
#include "../../kernel/string.h"

#define TUNSETNOCSUM  0x400454c8
#define TUNSETDEBUG   0x400454c9
#define TUNSETIFF     0x400454ca
#define TUNSETPERSIST 0x400454cb
#define TUNSETOWNER   0x400454cc
#define TUNSETLINK    0x400454cd
#define TUNSETGROUP   0x400454ce
#define TUNGETFEATURES 0x800454cf
#define TUNSETOFFLOAD 0x400454d0
#define TUNGETIFF     0x800454d2
#define TUNGETSNDBUF  0x800454d3
#define TUNSETSNDBUF  0x400454d4
#define TUNSETVNETHDRSZ 0x400454d8
#define TUNSETQUEUE   0x400454d9

#define IFF_TUN 0x0001
#define IFF_TAP 0x0002
#define IFF_NO_PI 0x1000
#define IFF_ONE_QUEUE 0x2000
#define IFF_VNET_HDR 0x4000
#define IFF_MULTI_QUEUE 0x0100
#define IFF_ATTACH_QUEUE 0x0200
#define IFF_DETACH_QUEUE 0x0400
#define IFF_PERSIST 0x0800
#define TUN_QUEUE_MAX 500

struct ifreq_ifru { char name[IFNAMSIZ]; union { int16_t flags; uint8_t pad[24]; } u; };

struct tun_frame { struct tun_frame* next; unsigned length; uint16_t protocol; uint8_t data[]; };
struct tun_device {
    struct netdev* dev;
    bool tap, no_pi, persist, vnet_hdr;
    uint32_t owner, group;              /* ~0: unset */
    unsigned attached;                  /* open file descriptors bound to it */
    struct tun_frame* head, *tail; unsigned queued; uint64_t dropped;
    void* reader;                       /* wait channel (the bound file) */
    unsigned sndbuf;
};
struct tun_file { struct tun_device* tun; };
static struct tun_device* devices[16];

static struct tun_device* tun_of(struct netdev* dev) { return dev ? dev->priv : NULL; }
static void tun_free_queue(struct tun_device* t) {
    while (t->head) { struct tun_frame* f = t->head; t->head = f->next; net_free(f, sizeof(*f) + f->length); }
    t->tail = NULL; t->queued = 0;
}
static void forget(struct tun_device* t) {
    for (unsigned i = 0; i < 16; i++) if (devices[i] == t) devices[i] = NULL;
    tun_free_queue(t); net_free(t, sizeof(*t));
}
/* netdev xmit: the stack sends to the interface -> queue for the reader (userspace). */
static int tun_xmit(struct netdev* dev, const void* data, unsigned length) {
    struct tun_device* t = tun_of(dev); if (!t) return -100;
    if (!t->attached) { t->dropped++; return -11; }        /* no reader: dropped, like a full qdisc */
    if (t->queued >= TUN_QUEUE_MAX) { t->dropped++; return -11; }
    struct tun_frame* f = net_alloc(sizeof(*f) + length); if (!f) { t->dropped++; return -11; }
    f->next = NULL; f->length = length;
    f->protocol = t->tap ? ETH_TYPE_IPV6 : (length && (((const uint8_t*)data)[0] >> 4) == 6 ? ETH_TYPE_IPV6 : ETH_TYPE_IPV4);
    if (t->tap && length >= ETH_HEADER_LEN) f->protocol = be16((const uint8_t*)data + 12);
    memcpy(f->data, data, length);
    if (t->tail) t->tail->next = f; else t->head = f;
    t->tail = f; t->queued++;
    if (t->reader) { task_wake_channel(t->reader); event_source_changed(t->reader, 1); }
    return 0;
}
static void tun_destroy(struct netdev* dev) {
    struct tun_device* t = tun_of(dev); dev->priv = NULL;
    if (t) { t->dev = NULL; if (t->reader) { task_wake_channel(t->reader); event_source_changed(t->reader, 1 | 8); } if (!t->attached) forget(t); }   /* readers see EBADFD */
}
static const struct netdev_ops tun_ops = {tun_xmit, NULL, tun_destroy, NULL};
static int dummy_xmit(struct netdev* dev, const void* data, unsigned length) { (void)data; dev->stats.tx_packets++; dev->stats.tx_bytes += length; return 0; }
static const struct netdev_ops dummy_ops = {dummy_xmit, NULL, NULL, NULL};

/* ---- creation ---- */
static struct tun_device* create_device(const char* name, bool tap, uint32_t owner) {
    unsigned slot = 16; for (unsigned i = 0; i < 16; i++) if (!devices[i]) { slot = i; break; }
    if (slot == 16) return NULL;
    struct tun_device* t = net_alloc(sizeof(*t)); if (!t) return NULL;
    memset(t, 0, sizeof(*t)); t->tap = tap; t->owner = owner; t->group = ~0u; t->sndbuf = 0x10000;
    char final[IFNAMSIZ]; if (netdev_name_template(name && name[0] ? name : "", tap ? "tap%d" : "tun%d", final)) { net_free(t, sizeof(*t)); return NULL; }
    struct netdev* dev = netdev_create(final, tap ? ARPHRD_ETHER : ARPHRD_NONE, &tun_ops, t);
    if (!dev) { net_free(t, sizeof(*t)); return NULL; }
    strcpy(dev->kind, tap ? "tap" : "tun");
    dev->mtu = dev->max_mtu = 1500; dev->min_mtu = tap ? 68 : 68; dev->mtu6 = 1500; dev->conf6.mtu = 1500;
    dev->flags |= IFF_MULTICAST; dev->flags &= ~(uint32_t)IFF_NOARP;
    if (tap) { if (!native_random(dev->addr, 6)) memset(dev->addr, 0x02, 6); dev->addr[0] = (uint8_t)((dev->addr[0] & 0xFE) | 0x02); memcpy(dev->perm_addr, dev->addr, 6); }
    else dev->flags |= IFF_NOARP | IFF_POINTOPOINT;
    dev->carrier = true;
    t->dev = dev; devices[slot] = t;
    return t;
}
/* rtnetlink: `ip link add [name] type tun|tap` (IFLA_INFO_DATA carries IFLA_TUN_TYPE 1=tun/2=tap and IFLA_TUN_OWNER). */
int tun_create_netlink(const char* name, const struct nl_attr_view* info, unsigned info_count, struct netdev** out) {
    (void)info_count; bool tap = false; uint32_t owner = ~0u;
    if (info[2].present) {   /* IFLA_INFO_DATA */
        struct nl_attr_view data[12]; if (nl_parse(info[2].data, info[2].length, data, 11)) {
            if (data[1].present) tap = nl_u8(&data[1]) == 2;     /* IFLA_TUN_TYPE */
            if (data[2].present) owner = nl_u32(&data[2]);        /* IFLA_TUN_OWNER */
        }
    }
    struct tun_device* t = create_device(name, tap, owner); if (!t) return -12;
    t->persist = true;   /* an interface made administratively exists until explicitly deleted */
    *out = t->dev; return 0;
}
int dummy_create(const char* name, struct netdev** out) {
    struct netdev* dev = netdev_create(name, ARPHRD_ETHER, &dummy_ops, NULL); if (!dev) return -12;
    strcpy(dev->kind, "dummy"); dev->mtu = dev->max_mtu = 1500; dev->mtu6 = 1500; dev->conf6.mtu = 1500;
    dev->flags |= IFF_MULTICAST; dev->carrier = true;
    if (!native_random(dev->addr, 6)) memset(dev->addr, 0x02, 6);
    dev->addr[0] = (uint8_t)((dev->addr[0] & 0xFE) | 0x02); memcpy(dev->perm_addr, dev->addr, 6);
    *out = dev; return 0;
}

/* ---- /dev/net/tun character device (FD_OBJECT) ---- */
static int64_t tun_io(void* object, const struct iovec64* vectors, unsigned count, uint64_t total, bool write, uint64_t flags);
static int tun_ioctl(void* object, uint64_t command, uint64_t argument, uint64_t flags);
static uint32_t tun_ready(void* object);
static void* tun_source(void* object);
static void tun_release(void* object);
static void tun_stat(void* object, struct linux_stat* stat);
static const struct object_operations tun_file_ops = {tun_io, tun_ioctl, tun_ready, tun_source, tun_release, tun_stat};

bool tun_descriptor(struct open_file* description) { return fd_object(description, &tun_file_ops) != NULL; }
int tun_open(uint64_t flags) {
    struct tun_file* tf = net_alloc(sizeof(*tf)); if (!tf) return -12;
    tf->tun = NULL;
    int fd = fd_create_object(&tun_file_ops, tf, (flags & 0x800) | (flags & 0x80000) | 2, "/dev/net/tun");
    if (fd < 0) net_free(tf, sizeof(*tf));
    return fd;
}
static int set_iff(struct tun_file* tf, uint64_t argument) {
    if (tf->tun) return -22;                                   /* already attached */
    if (!accessible(argument, sizeof(struct ifreq_ifru), false)) return -14;
    struct ifreq_ifru req; copy_from_user(&req, (void*)argument, sizeof(req));
    uint16_t want = (uint16_t)req.u.flags; bool tap = (want & IFF_TAP) != 0;
    if (!(want & (IFF_TUN | IFF_TAP)) || (want & IFF_TUN && want & IFF_TAP)) return -22;
    if (want & (IFF_VNET_HDR | IFF_MULTI_QUEUE)) { /* accepted but single-queue, no vnet header offered */ }
    /* CLONE_NEWNET (namespaces.c): TUN/TAP interfaces live in the single initial stack; an isolated netns has only lo */
    if (task_net_namespace()) return -1;
    char name[IFNAMSIZ + 1] = {0}; memcpy(name, req.name, IFNAMSIZ);
    struct tun_device* t = NULL;
    if (name[0]) {
        struct netdev* dev = netdev_by_name(name);
        if (dev) {
            if (strcmp(dev->kind, tap ? "tap" : "tun")) return -22;
            t = tun_of(dev); if (!t) return -22;
            if (t->attached && !(want & IFF_MULTI_QUEUE)) return -16;   /* EBUSY: already owned */
            if (t->owner != ~0u && t->owner != credentials_kernel_id(credentials_current(), false, 3) && !net_raw_capable()) return -1;
        }
    }
    if (!t) {
        if (!net_raw_capable()) return -1;                     /* creating a new interface needs CAP_NET_ADMIN/owner */
        t = create_device(name, tap, credentials_kernel_id(credentials_current(), false, 3));
        if (!t) return -12;
    }
    t->no_pi = (want & IFF_NO_PI) != 0; t->vnet_hdr = (want & IFF_VNET_HDR) != 0;
    t->attached++; tf->tun = t; t->reader = tf;
    /* report the resulting flags and name back */
    memset(&req, 0, sizeof(req)); strncpy(req.name, t->dev->name, IFNAMSIZ);
    req.u.flags = (int16_t)((tap ? IFF_TAP : IFF_TUN) | (t->no_pi ? IFF_NO_PI : 0) | (t->persist ? IFF_PERSIST : 0));
    if (accessible(argument, sizeof(req), true)) copy_to_user((void*)argument, &req, sizeof(req));
    return 0;
}
static int get_iff(struct tun_file* tf, uint64_t argument) {
    if (!tf->tun || !tf->tun->dev) return -77;
    if (!accessible(argument, sizeof(struct ifreq_ifru), true)) return -14;
    struct ifreq_ifru req; memset(&req, 0, sizeof(req)); strncpy(req.name, tf->tun->dev->name, IFNAMSIZ);
    req.u.flags = (int16_t)((tf->tun->tap ? IFF_TAP : IFF_TUN) | (tf->tun->no_pi ? IFF_NO_PI : 0) | (tf->tun->persist ? IFF_PERSIST : 0));
    return copy_to_user((void*)argument, &req, sizeof(req)) ? -14 : 0;
}
static int tun_ioctl(void* object, uint64_t command, uint64_t argument, uint64_t flags) {
    (void)flags; struct tun_file* tf = object; struct tun_device* t = tf->tun;
    switch ((uint32_t)command) {
    case TUNSETIFF: return set_iff(tf, argument);
    case TUNGETIFF: return get_iff(tf, argument);
    case TUNSETPERSIST: if (!t) return -22; t->persist = argument != 0; return 0;
    case TUNSETOWNER: if (!t) return -22; t->owner = (uint32_t)argument; return 0;
    case TUNSETGROUP: if (!t) return -22; t->group = (uint32_t)argument; return 0;
    case TUNSETLINK: case TUNSETNOCSUM: case TUNSETDEBUG: case TUNSETOFFLOAD: case TUNSETVNETHDRSZ: return 0;
    case TUNSETQUEUE: return 0;
    case TUNGETFEATURES:
        if (!accessible(argument, 4, true)) return -14;
        return uput32(argument, IFF_TUN | IFF_TAP | IFF_NO_PI | IFF_ONE_QUEUE | IFF_MULTI_QUEUE | IFF_PERSIST);
    case TUNGETSNDBUF: if (!t || !accessible(argument, 4, true)) return t ? -14 : -22; return uput32(argument, (uint32_t)t->sndbuf);
    case TUNSETSNDBUF: if (!t || !accessible(argument, 4, false)) return t ? -14 : -22; t->sndbuf = uget32(argument); return 0;
    case 0x541B: if (!accessible(argument, 4, true)) return -14; return uput32(argument, (uint32_t)(t && t->head ? (int)(t->head->length + (t->no_pi ? 0 : 4)) : 0));
    }
    if (t && ((uint32_t)command & 0xFF00) == 0x8900) return netif_ioctl(command, argument);
    return -25;
}
/* read: hand the reader one queued frame (prefixed with the 4-byte packet info unless IFF_NO_PI). */
static int64_t tun_read(struct tun_device* t, const struct iovec64* vectors, unsigned count, uint64_t total, uint64_t flags, uint64_t dflags) {
    while (!t->head) {
        if (!t->dev) return -77;                                /* the interface was deleted under us */
        if ((flags & 0x40) || (dflags & 0x800)) return -11;
        int error = task_wait_channel(t->reader); if (error) return error;
    }
    struct tun_frame* f = t->head;
    unsigned header = t->no_pi ? 0 : 4; uint8_t pi[4] = {0, 0, (uint8_t)(f->protocol >> 8), (uint8_t)f->protocol};
    unsigned want = header + f->length;
    if (total < want) return -22;                              /* EINVAL: Linux requires the whole frame to fit */
    if (!iov_access(vectors, count, 0, want, true)) return -14;
    if (header) iov_copy(vectors, count, 0, pi, header, false);          /* (false: kernel -> user memory) */
    iov_copy(vectors, count, header, f->data, f->length, false);
    t->head = f->next; if (!t->head) t->tail = NULL; t->queued--;
    net_free(f, sizeof(*f) + f->length);
    return want;
}
/* write: a frame from userspace, injected into the stack (TUN: an L3 packet; TAP: an Ethernet frame). */
static int64_t tun_write(struct tun_device* t, const struct iovec64* vectors, unsigned count, uint64_t total) {
    unsigned header = t->no_pi ? 0 : 4;
    if (total < header + (t->tap ? ETH_HEADER_LEN : 1u) || total > header + 65535) return -22;
    unsigned length = (unsigned)total - header;
    uint8_t* buffer = (void*)arena_alloc(length); if (!buffer) return -105;
    if (!iov_access(vectors, count, 0, total, false)) { arena_free((uint64_t)buffer, length); return -14; }
    iov_copy(vectors, count, header, buffer, length, true);               /* (true: from user memory) */
    t->dev->stats.rx_packets++; t->dev->stats.rx_bytes += length;
    if (t->tap) netdev_receive(t->dev, buffer, length);
    else {
        uint16_t proto = length && (buffer[0] >> 4) == 6 ? ETH_TYPE_IPV6 : ETH_TYPE_IPV4;
        netdev_input_l3(t->dev, buffer, length, proto, 0 /* PACKET_HOST */);
    }
    arena_free((uint64_t)buffer, length);
    return (int64_t)total;
}
static int64_t tun_io(void* object, const struct iovec64* vectors, unsigned count, uint64_t total, bool write, uint64_t flags) {
    struct tun_file* tf = object; if (!tf->tun || !tf->tun->dev) return -77;    /* EBADFD until TUNSETIFF / after the interface went */
    return write ? tun_write(tf->tun, vectors, count, total) : tun_read(tf->tun, vectors, count, total, flags, 0);
}
static uint32_t tun_ready(void* object) {
    struct tun_file* tf = object; if (!tf->tun) return 4;
    if (!tf->tun->dev) return 1u | 8u;                         /* gone: readable (EBADFD) and error */
    return (tf->tun->head ? 1u : 0) | 4u;                      /* always writable */
}
static void* tun_source(void* object) { struct tun_file* tf = object; return tf->tun ? tf->tun->reader : object; }
static void tun_release(void* object) {
    struct tun_file* tf = object; struct tun_device* t = tf->tun;
    if (t) {
        if (t->attached) t->attached--;
        if (!t->attached) {
            t->reader = NULL; tun_free_queue(t);
            struct netdev* dev = t->dev;
            if (!dev) forget(t);                                   /* the interface is already gone (even a persistent one) */
            else if (!t->persist) netdev_unregister(dev);           /* (tun_destroy frees t) */
        }
    }
    net_free(tf, sizeof(*tf));
}
static void tun_stat(void* object, struct linux_stat* stat) {
    (void)object; memset(stat, 0, sizeof(*stat)); stat->dev = 5; stat->ino = 0x54554e00; stat->nlink = 1;
    stat->mode = 0020666; stat->rdev = (10u << 8) | 200u;      /* c 10:200, like Linux /dev/net/tun */
    stat->block_size = 4096;
}
