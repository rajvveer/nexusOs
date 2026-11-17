/* NexusOS Bluetooth host stack - internal interfaces (bluetooth.c core, bt_mgmt.c, bt_l2cap.c, bt_smp.c, bt_rfcomm.c,
 * bt_alg.c, bt_crypto.c, btusb.c). Linux ABI: AF_BLUETOOTH sockets (HCI raw/user/monitor/control, L2CAP, RFCOMM),
 * the mgmt API (doc/mgmt-api.txt of BlueZ), /dev/vhci; AF_ALG ecb(aes)/cmac(aes) for BlueZ's crypto.
 * Locking: everything runs under the big kernel lock (syscalls, the BSP device poll, USB completions). Code that
 * sleeps (task_wait_*) re-validates every object afterwards through generation numbers (bt_dev_get, bt_conn_get). */
#ifndef NEXUS_BT_H
#define NEXUS_BT_H
#include "native.h"

enum { E_PERM = -1, E_NOENT = -2, E_INTR = -4, E_IO = -5, E_BADF = -9, E_AGAIN = -11, E_NOMEM = -12, E_ACCES = -13, E_FAULT = -14,
       E_BUSY = -16, E_EXIST = -17, E_NODEV = -19, E_INVAL = -22, E_NFILE = -23, E_MFILE = -24, E_NOTTY = -25, E_PIPE = -32,
       E_NOSYS = -38, E_PROTO = -71, E_BADFD = -77, E_MSGSIZE = -90, E_PROTOTYPE = -91, E_NOPROTOOPT = -92, E_PROTONOSUPPORT = -93,
       E_SOCKTNOSUPPORT = -94, E_OPNOTSUPP = -95, E_AFNOSUPPORT = -97, E_ADDRINUSE = -98, E_ADDRNOTAVAIL = -99, E_NETDOWN = -100,
       E_CONNABORTED = -103, E_CONNRESET = -104, E_ISCONN = -106, E_NOTCONN = -107, E_SHUTDOWN = -108, E_TIMEDOUT = -110,
       E_CONNREFUSED = -111, E_HOSTDOWN = -112, E_HOSTUNREACH = -113, E_ALREADY = -114, E_INPROGRESS = -115, E_BADE = -52,
       E_NOKEY = -126, E_KEYREJECTED = -129 };
#define BT_MAX_DEVICES 8U
#define BT_ACL_MAX 65539U            /* largest L2CAP frame reassembled (65535 + basic header) */
#define BT_FRAME_MAX 1100U           /* HCI socket / vhci packet bound (H:4 type + header + 1024 data) */
#define BT_PEEK (1ULL << 40)         /* MSG_PEEK in the flags passed down (descriptor flags use the low bits) */

static inline unsigned bt_get16(const uint8_t* p) { return p[0] | (unsigned)p[1] << 8; }
static inline uint32_t bt_get32(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline void bt_put16(uint8_t* p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void bt_put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static inline bool bt_addr_any(const uint8_t* a) { return !(a[0] | a[1] | a[2] | a[3] | a[4] | a[5]); }
static inline void bt_move(void* to, const void* from, unsigned n) {   /* overlapping copy (the kernel has no memmove) */
    uint8_t* d = to; const uint8_t* s = from;
    if (d < s) for (unsigned i = 0; i < n; i++) d[i] = s[i]; else for (unsigned i = n; i--;) d[i] = s[i];
}

/* ---------- memory: page-backed objects, counted outside the per-program audit only where noted ---------- */
void* bt_zalloc(uint64_t bytes);
void bt_free(void* p, uint64_t bytes);
int bt_from_user(void* kernel, uint64_t user, uint64_t n);   /* 0 or E_FAULT (accessible + copy_from_user) */
int bt_to_user(uint64_t user, const void* kernel, uint64_t n);

/* ---------- packets (queues of whole frames) ---------- */
struct bt_pkt { struct bt_pkt* next; uint32_t len, alloc; uint64_t stamp; uint8_t data[]; };
struct bt_pkt* bt_pkt_new(unsigned len);
void bt_pkt_free(struct bt_pkt* p);
struct bt_queue { struct bt_pkt *head, *tail; unsigned count, bytes; };
void bt_queue_put(struct bt_queue* q, struct bt_pkt* p);
struct bt_pkt* bt_queue_take(struct bt_queue* q);
void bt_queue_purge(struct bt_queue* q);

/* ---------- controllers ---------- */
enum { HCI_UP = 1U << 0, HCI_INIT = 1U << 1, HCI_RUNNING = 1U << 2, HCI_RAW = 1U << 8, HCI_USER = 1U << 13 };
enum { PKT_COMMAND = 1, PKT_ACL = 2, PKT_SCO = 3, PKT_EVENT = 4, PKT_ISO = 5, PKT_VENDOR = 0xFF };
/* mgmt settings bits */
enum { ST_POWERED = 1U << 0, ST_CONNECTABLE = 1U << 1, ST_FAST_CONN = 1U << 2, ST_DISCOVERABLE = 1U << 3, ST_BONDABLE = 1U << 4,
       ST_LINK_SEC = 1U << 5, ST_SSP = 1U << 6, ST_BREDR = 1U << 7, ST_HS = 1U << 8, ST_LE = 1U << 9, ST_ADVERTISING = 1U << 10,
       ST_SC = 1U << 11, ST_DEBUG_KEYS = 1U << 12, ST_PRIVACY = 1U << 13, ST_CONFIG = 1U << 14, ST_STATIC_ADDR = 1U << 15,
       ST_PHY = 1U << 16, ST_WBS = 1U << 17 };
struct bt_dev;
struct bt_driver {                   /* host -> controller transport (vhci in bluetooth.c, USB in btusb.c) */
    const char* name; unsigned bus;   /* HCI_VIRTUAL 0, HCI_USB 1 */
    int (*send)(struct bt_dev* d, const uint8_t* packet, unsigned length);   /* H:4 type byte first; 0 or -errno */
    bool (*can_send)(struct bt_dev* d, unsigned type);   /* queue room for one more packet of this type */
    int (*setup)(struct bt_dev* d);  /* optional vendor init after Reset (btmtk etc.), task context */
};
struct bt_cmd;
typedef void (*bt_cmd_done)(struct bt_dev* d, struct bt_cmd* c, int status, const uint8_t* rp, unsigned rlen);
struct bt_cmd {
    struct bt_cmd* next; uint16_t opcode; uint8_t plen; uint8_t params[255];
    bt_cmd_done done; void* ctx; uint64_t arg;
    bool sync, completed, abandoned, worker; int status; uint8_t rp[64]; uint16_t rlen;
};
struct bt_key { uint8_t addr[6], type, key_type, val[16], pin_len, authenticated, enc_size, central; uint16_t ediv; uint8_t rand[8]; bool used; };
struct bt_irk { uint8_t addr[6], type, val[16]; bool used; };
struct bt_found { uint8_t addr[6], type; int8_t rssi; bool name_needed, name_pending; uint8_t page_scan_mode; uint16_t clock_offset; uint8_t adv_len, adv[31]; };
struct bt_uuid { uint8_t uuid[16], svc_hint; bool used; };
struct bt_conn;
struct bt_dev {
    int index; uint32_t generation; bool alive, setup_done;
    const struct bt_driver* driver; void* driver_data;
    uint32_t flags; void* user;      /* user: the HCI_CHANNEL_USER socket owning the device */
    uint8_t bdaddr[6], features[3][8], max_page, le_features[8], le_states[8], commands[64];
    uint8_t hci_ver, lmp_ver; uint16_t hci_rev, lmp_subver, manufacturer;
    uint16_t acl_mtu, acl_max, sco_mtu, sco_max, le_mtu, le_max, acl_cnt, le_cnt; uint8_t cmd_cnt;
    struct bt_cmd *cmd_head, *cmd_tail, *cmd_sent; unsigned cmd_queued; uint64_t cmd_deadline;
    uint32_t stat[10];               /* err_rx err_tx cmd_tx evt_rx acl_tx acl_rx sco_tx sco_rx byte_rx byte_tx */
    struct bt_conn* conns;
    /* host state (mgmt) */
    uint32_t settings, supported; bool powering, reported; uint8_t io_cap, dev_class[3], major, minor;
    char name[249], short_name[11]; uint16_t appearance;
    uint8_t scan_enable;             /* written Write Scan Enable value */
    bool le_adv_on, discov_limited, adv_connectable_forced, sc_only; uint16_t scan_interval, scan_window;
    uint8_t adv_instance; uint32_t inst_flags; uint8_t inst_adv[31], inst_adv_len, inst_rsp[31], inst_rsp_len;   /* ADD_ADVERTISING */
    int8_t filter_rssi; uint8_t filter_uuids, filter_uuid[16][16];          /* START_SERVICE_DISCOVERY */
    struct bt_uuid uuids[24];
    struct bt_key keys[32];          /* BR/EDR link keys (type 0) and LE LTKs */
    struct bt_irk irks[16];
    uint8_t blocked[16][7]; unsigned blocked_count;
    uint8_t accept_list[16][7]; unsigned accept_count;   /* mgmt Add Device (BR/EDR: incoming allowed) */
    uint16_t did_source, did_vendor, did_product, did_version;
    uint16_t discov_timeout; uint64_t discov_deadline;
    /* discovery */
    uint8_t discovering, discovery_type; void* discovery_owner; uint64_t discovery_deadline; bool inquiry_active, le_scan_active, name_phase;
    struct bt_found found[32]; unsigned found_count;
    uint64_t idle_check;
    bool rpa_resolving;               /* LOAD_IRKS seen: SMP asks for the peer's identity (Linux HCI_RPA_RESOLVING) */
};
struct bt_dev* bt_dev_get(int index, uint32_t generation);   /* the live device of index with that generation */
struct bt_dev* bt_dev_by_index(int index);
struct bt_dev* bt_dev_route(const uint8_t* src, bool le);   /* a powered controller for a socket bound to src (ANY: first) */
struct bt_dev* bt_dev_register(const struct bt_driver* driver, void* data, const char* why);
void bt_dev_unregister(struct bt_dev* d);
void bt_dev_receive(struct bt_dev* d, const uint8_t* packet, unsigned length);   /* controller -> host (H:4 type first) */
int bt_dev_open(struct bt_dev* d);    /* HCIDEVUP / mgmt power on: task context, sleeps for the init sequence */
int bt_dev_close(struct bt_dev* d);
/* commands: queued FIFO, one in flight per Num_HCI_Command_Packets credit, 2 s timeout */
bool bt_cmd_async(struct bt_dev* d, unsigned opcode, const void* params, unsigned plen, bt_cmd_done done, void* ctx, uint64_t arg);
int bt_cmd_sync(struct bt_dev* d, unsigned opcode, const void* params, unsigned plen, uint8_t* rp, unsigned rmax);   /* <0 errno, else HCI status */
void bt_send_raw(struct bt_dev* d, const uint8_t* packet, unsigned length, const void* from_socket);   /* raw socket write */
int bt_hci_errno(int status);        /* HCI status -> -errno */

/* ---------- connections ---------- */
enum { LINK_ACL = 1, LINK_LE = 2 };
enum { CONN_CONNECTING = 1, CONN_CONNECTED, CONN_CLOSING };
enum { SEC_SDP = 0, SEC_LOW = 1, SEC_MEDIUM = 2, SEC_HIGH = 3, SEC_FIPS = 4 };
struct l2cap_chan;
struct smp_ctx;
struct bt_conn {
    struct bt_conn* next; struct bt_dev* dev; uint32_t id;
    uint16_t handle; uint8_t link, state, role; bool outgoing, handle_valid;
    uint8_t peer[6], peer_type;       /* mgmt address type: 0 BR/EDR, 1 LE public, 2 LE random */
    uint8_t own_type;                 /* LE own address type used (0 public) */
    uint8_t dev_class[3], remote_features[8]; char name[249]; bool name_known;
    /* security */
    uint8_t sec_level, pending_sec, auth_type, io_cap, remote_io_cap, remote_auth, key_type, enc_key_size;
    bool encrypted, authenticated, auth_pending, enc_pending, ssp_initiator, pairing, mgmt_connected, link_key_known, bonding, remote_auth_known;
    uint8_t page_scan_mode;
    uint8_t ltk[16];
    /* ACL */
    uint8_t* rx; uint32_t rx_len, rx_want, rx_alloc; struct bt_queue txq; unsigned sent;
    /* L2CAP */
    uint8_t ident; struct l2cap_chan* chans; uint32_t remote_feat_mask; bool info_sent, info_done;
    uint8_t info_ident; uint64_t info_deadline;   /* BR/EDR feature-mask exchange before the first channel (Linux) */
    struct smp_ctx* smp;
    void* pair_op;                    /* mgmt PAIR_DEVICE in progress */
    uint64_t idle_deadline;           /* no channel left: disconnect then (0: none) */
    uint8_t disc_reason;
    unsigned waiters;
};
struct bt_conn* bt_conn_get(uint32_t id);
struct bt_conn* bt_conn_lookup(struct bt_dev* d, const uint8_t* addr, uint8_t link);
struct bt_conn* bt_conn_by_handle(struct bt_dev* d, unsigned handle);
struct bt_conn* bt_conn_new(struct bt_dev* d, const uint8_t* addr, uint8_t peer_type, uint8_t link, bool outgoing);
/* An ACL (BR/EDR) or LE link to addr: an existing one, else Create Connection / LE Create Connection is sent. */
struct bt_conn* bt_connect(struct bt_dev* d, const uint8_t* addr, uint8_t peer_type, int* error);
void bt_disconnect(struct bt_conn* c, uint8_t reason);
void bt_conn_idle(struct bt_conn* c);  /* no user left: disconnect after a short grace period */
void bt_conn_busy(struct bt_conn* c);
int bt_acl_send(struct bt_conn* c, uint16_t cid, const uint8_t* data, unsigned length);  /* one L2CAP B-frame */
int bt_acl_send2(struct bt_conn* c, uint16_t cid, const uint8_t* head, unsigned hlen, const uint8_t* data, unsigned length);
/* security: start authentication/encryption so the link reaches level; true if already there */
bool bt_conn_security(struct bt_conn* c, uint8_t level, bool initiator);
void bt_conn_security_changed(struct bt_conn* c, int status);   /* -> L2CAP/RFCOMM waiters, mgmt pair */
const char* bt_conn_text(const struct bt_conn* c);

/* ---------- mgmt (bt_mgmt.c) ---------- */
void mgmt_index_added(struct bt_dev* d);
void mgmt_index_removed(struct bt_dev* d);
void mgmt_powered(struct bt_dev* d, bool on);
void mgmt_event(unsigned event, int index, const void* data, unsigned length, const void* skip_socket);
void mgmt_socket_command(void* socket, const uint8_t* message, unsigned length, bool trusted);   /* HCI_CHANNEL_CONTROL write (worker) */
void mgmt_socket_closed(void* socket);
void mgmt_hci_event(struct bt_dev* d, const uint8_t* event, unsigned length);   /* events the core does not consume */
void mgmt_conn_complete(struct bt_conn* c, int status);       /* status: HCI status, 0 = up */
void mgmt_conn_closed(struct bt_conn* c, uint8_t hci_reason);
void mgmt_pair_done(struct bt_conn* c, int mgmt_status);
void mgmt_new_ltk(struct bt_conn* c, const uint8_t* ltk, uint8_t key_size, bool authenticated, bool central_role, uint16_t ediv, const uint8_t* rand, bool sc);
void mgmt_new_irk(struct bt_conn* c, const uint8_t* irk, const uint8_t* identity, uint8_t identity_type);
void mgmt_new_csrk(struct bt_conn* c, const uint8_t* csrk, uint8_t type);   /* type: 0/2 local, 1/3 remote (2/3 authenticated) */
void mgmt_new_link_key_le(struct bt_conn* c, const uint8_t* addr, const uint8_t* key, uint8_t type);   /* SC cross-transport */
void mgmt_confirm_request(struct bt_conn* c, uint32_t value, bool hint);
void mgmt_passkey_request(struct bt_conn* c);
void mgmt_passkey_notify(struct bt_conn* c, uint32_t passkey);
void mgmt_auth_failed(struct bt_conn* c, uint8_t mgmt_status);
struct bt_key* mgmt_find_ltk(struct bt_dev* d, const uint8_t* addr, uint8_t type, bool central);
struct bt_key* mgmt_find_ltk_rand(struct bt_dev* d, uint16_t ediv, const uint8_t* rand);
bool mgmt_bondable(struct bt_dev* d);
void mgmt_poll(struct bt_dev* d, uint64_t now);
uint64_t mgmt_next_event(struct bt_dev* d, uint64_t now);
void mgmt_dev_freed(struct bt_dev* d);
void mgmt_monitor_command(int index, const uint8_t* message, unsigned length, bool event, uint32_t cookie);

/* ---------- L2CAP (bt_l2cap.c) ---------- */
#define L2CAP_CID_SIGNAL 0x0001
#define L2CAP_CID_ATT 0x0004
#define L2CAP_CID_LE_SIGNAL 0x0005
#define L2CAP_CID_SMP 0x0006
#define L2CAP_CID_SMP_BREDR 0x0007
void l2cap_receive(struct bt_conn* c, const uint8_t* frame, unsigned length);   /* a whole B-frame */
void l2cap_conn_ready(struct bt_conn* c);          /* link up: fixed channels (ATT listeners) */
void l2cap_conn_down(struct bt_conn* c, int error); /* link gone: every channel closed with error */
void l2cap_security_done(struct bt_conn* c, int status);
bool l2cap_conn_in_use(struct bt_conn* c);
void l2cap_poll(uint64_t now);
uint64_t l2cap_next_event(uint64_t now);
/* kernel users of a dynamic channel (RFCOMM): */
struct l2cap_ops { void (*state)(void* owner, int state, int error); void (*recv)(void* owner, const uint8_t* data, unsigned length); };
enum { CHAN_CLOSED = 0, CHAN_BOUND, CHAN_LISTEN, CHAN_CONNECT, CHAN_CONNECT2, CHAN_CONFIG, CHAN_CONNECTED, CHAN_DISCONN, CHAN_SECURITY };
struct l2cap_chan* l2cap_kernel_connect(struct bt_dev* d, const uint8_t* addr, uint16_t psm, uint8_t sec_level, const struct l2cap_ops* ops, void* owner, int* error);
int l2cap_kernel_listen(uint16_t psm, const struct l2cap_ops* ops, void* (*accept)(struct l2cap_chan* ch));
void l2cap_kernel_attach(struct l2cap_chan* ch, void* owner);
int l2cap_kernel_send(struct l2cap_chan* ch, const uint8_t* data, unsigned length);
void l2cap_kernel_close(struct l2cap_chan* ch);
struct bt_conn* l2cap_chan_conn(struct l2cap_chan* ch);
uint16_t l2cap_chan_omtu(struct l2cap_chan* ch);
uint8_t l2cap_chan_sec(struct l2cap_chan* ch);
bool l2cap_descriptor(struct open_file* f);
int64_t l2cap_socket_create(uint64_t type, uint64_t protocol, bool kernel);
int64_t l2cap_call(struct registers* r, struct open_file* f);
unsigned l2cap_live(void);

/* ---------- SMP (bt_smp.c) ---------- */
void smp_receive(struct bt_conn* c, const uint8_t* pdu, unsigned length);
int smp_start(struct bt_conn* c, uint8_t sec_level);       /* central: Pairing Request; peripheral: Security Request */
void smp_conn_closed(struct bt_conn* c);
void smp_encrypt_changed(struct bt_conn* c, int status);
void smp_ltk_request(struct bt_conn* c, uint16_t ediv, const uint8_t* rand);
int smp_user_reply(struct bt_conn* c, unsigned opcode, uint32_t passkey);   /* mgmt confirm/passkey (negative) replies */
void smp_poll(uint64_t now);
uint64_t smp_next_event(uint64_t now);
bool bt_le_encrypt(struct bt_conn* c, const uint8_t* ltk, uint16_t ediv, const uint8_t* rand, uint8_t key_size);

/* ---------- RFCOMM (bt_rfcomm.c) ---------- */
void rfcomm_init(void);
bool rfcomm_descriptor(struct open_file* f);
int64_t rfcomm_socket_create(uint64_t type, uint64_t protocol);
int64_t rfcomm_call(struct registers* r, struct open_file* f);
void rfcomm_poll(uint64_t now);
uint64_t rfcomm_next_event(uint64_t now);
unsigned rfcomm_live(void);

/* ---------- AF_ALG (bt_alg.c) ---------- */
bool alg_descriptor(struct open_file* f);
int64_t alg_call(struct registers* r, struct open_file* f);

/* ---------- socket helpers shared by the protocol files (bluetooth.c) ---------- */
struct bt_sockopt { uint64_t level, name, value, size; };
int bt_sol_socket_get(uint64_t name, uint64_t value, uint64_t size_address, int type, int protocol, int error, unsigned rcvbuf, unsigned sndbuf);
int bt_get_int_option(uint64_t value, uint64_t size_address, uint32_t v);
int bt_get_bytes_option(uint64_t value, uint64_t size_address, const void* data, unsigned n);
int bt_set_int_option(uint64_t value, uint64_t size, uint32_t* out);
int64_t bt_copy_out_vectors(const struct iovec64* v, unsigned count, const uint8_t* data, unsigned n);
int64_t bt_copy_in_vectors(const struct iovec64* v, unsigned count, uint64_t total, uint8_t* data, unsigned max);
int bt_read_message(uint64_t address, struct message64* m, struct iovec64* v, unsigned* count, uint64_t* total);
uint64_t bt_now(void);
void bt_wake(void* channel);         /* task_wake_channel + event_source_changed */
/* The Bluetooth worker (bluetooth.c): mgmt commands run there, off the writer's context (Linux semantics: the write
 * returns at once, the reply follows), so a controller emulated in the writer's own thread (BlueZ's hciemu testers)
 * can answer. bt_wait sleeps a task, or yields the worker back to whoever resumed it, until woken or the deadline. */
bool bt_in_worker(void);
int bt_wait(void* channel, uint64_t deadline);   /* 0, E_TIMEDOUT, E_INTR (tasks only) */
void bt_worker_wake(void);                        /* a wait of the worker may be over: resume it at the next safe point */
bool bt_privileged(unsigned capability);

/* ---------- crypto (bt_crypto.c) ---------- */
struct bt_aes { uint8_t round[240]; unsigned rounds; };
struct bt_cmac { const struct bt_aes* key; uint8_t x[16], block[16]; unsigned used; };
int bt_aes_key(struct bt_aes* k, const uint8_t* key, unsigned bytes);
void bt_aes_encrypt(const struct bt_aes* k, uint8_t* block);
void bt_aes_decrypt(const struct bt_aes* k, uint8_t* block);
void bt_aes_wipe(struct bt_aes* k);
void bt_cmac_init(struct bt_cmac* m, const struct bt_aes* key);
void bt_cmac_update(struct bt_cmac* m, const uint8_t* data, unsigned length);
void bt_cmac_final(struct bt_cmac* m, uint8_t* tag);
void bt_cmac(const uint8_t* key, unsigned key_bytes, const uint8_t* data, unsigned length, uint8_t* tag);
void bt_smp_e(const uint8_t* key, const uint8_t* in, uint8_t* out);
void bt_smp_c1(const uint8_t* k, const uint8_t* r, const uint8_t* preq, const uint8_t* pres, uint8_t iat, const uint8_t* ia, uint8_t rat, const uint8_t* ra, uint8_t* out);
void bt_smp_s1(const uint8_t* k, const uint8_t* r1, const uint8_t* r2, uint8_t* out);
void bt_smp_ah(const uint8_t* irk, const uint8_t* r, uint8_t* out);
void bt_smp_f4(const uint8_t* u, const uint8_t* v, const uint8_t* x, uint8_t z, uint8_t* out);
void bt_smp_f5(const uint8_t* w, const uint8_t* n1, const uint8_t* n2, const uint8_t* a1, const uint8_t* a2, uint8_t* mackey, uint8_t* ltk);
void bt_smp_f6(const uint8_t* w, const uint8_t* n1, const uint8_t* n2, const uint8_t* r, const uint8_t* iocap, const uint8_t* a1, const uint8_t* a2, uint8_t* out);
uint32_t bt_smp_g2(const uint8_t* u, const uint8_t* v, const uint8_t* x, const uint8_t* y);
void bt_smp_h6(const uint8_t* w, const uint8_t* key_id, uint8_t* out);
void bt_smp_h7(const uint8_t* salt, const uint8_t* w, uint8_t* out);
bool bt_p256_valid(const uint8_t* key);
bool bt_p256_keypair(uint8_t* private_le, uint8_t* public_le);
bool bt_p256_public(const uint8_t* private_le, uint8_t* public_le);
bool bt_p256_dhkey(const uint8_t* private_le, const uint8_t* peer_public_le, uint8_t* dhkey_le);
unsigned bt_crypto_selftest(void);
#endif
