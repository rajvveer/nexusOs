/* Bluetooth management API (HCI_CHANNEL_CONTROL, BlueZ doc/mgmt-api.txt, version 1.22): what bluetoothd 5.x, btmgmt
 * and bluetoothctl use to run a controller.
 *  - Controller: READ_VERSION/COMMANDS/INDEX_LIST/EXT_INDEX_LIST/INFO/EXT_INFO, SET_POWERED (full init + settings
 *    written to the controller), CONNECTABLE, DISCOVERABLE (general/limited, timeout), FAST_CONNECTABLE, BONDABLE,
 *    LINK_SECURITY, SSP, LE, BREDR, SECURE_CONN, DEBUG_KEYS, ADVERTISING, SET_DEV_CLASS, SET_LOCAL_NAME,
 *    ADD/REMOVE_UUID (class of device service bits + EIR), SET_DEVICE_ID, SET_APPEARANCE, SET_IO_CAPABILITY,
 *    SET_SCAN_PARAMS, ADD/REMOVE_ADVERTISING + GET_ADV_SIZE_INFO + READ_ADV_FEATURES (one legacy instance).
 *  - Discovery: START_DISCOVERY / START_LIMITED / START_SERVICE_DISCOVERY (RSSI + UUID filter), STOP_DISCOVERY,
 *    CONFIRM_NAME: inquiry (EIR mode) and active LE scanning at once for 10.24 s, DEVICE_FOUND with EIR/AD data (class
 *    of device appended), then Remote Name Request for unnamed devices bluetoothd did not know, DISCOVERING events.
 *  - Pairing: PAIR_DEVICE (BR/EDR: connection, Authentication Requested, Secure Simple Pairing through the controller's
 *    events with Linux's auto-accept rules; LE: connection + SMP in bt_smp.c), CANCEL_PAIR_DEVICE, UNPAIR_DEVICE,
 *    USER_CONFIRM_(NEG_)REPLY, USER_PASSKEY_(NEG_)REPLY, PIN_CODE_(NEG_)REPLY, LOAD_LINK_KEYS, LOAD_LONG_TERM_KEYS,
 *    LOAD_IRKS, LOAD_CONN_PARAM, SET_BLOCKED_KEYS; events NEW_LINK_KEY, NEW_LONG_TERM_KEY, NEW_IRK, USER_CONFIRM_
 *    REQUEST, USER_PASSKEY_REQUEST, PASSKEY_NOTIFY, PIN_CODE_REQUEST, AUTH_FAILED, DEVICE_UNPAIRED.
 *  - Connections: DISCONNECT, GET_CONNECTIONS, GET_CONN_INFO, BLOCK/UNBLOCK_DEVICE, ADD/REMOVE_DEVICE (incoming
 *    allowed for BR/EDR), GET/SET_DEVICE_FLAGS; events DEVICE_CONNECTED/DISCONNECTED, CONNECT_FAILED.
 *  - READ_EXP_FEATURES_INFO, READ_CONTROLLER_CAP, READ_DEF_SYSTEM_CONFIG: answered empty. Everything else: Unknown
 *    Command (READ_COMMANDS lists exactly what is implemented, so bluetoothd does not try the rest).
 * Commands run in the writer's context (they may wait for HCI commands, serialized per controller); PAIR_DEVICE and
 * DISCONNECT complete later from controller events. */
#include "bt.h"
#include "../../kernel/string.h"

void mgmt_socket_send(void* socket, const uint8_t* message, unsigned length);
uint32_t mgmt_socket_cookie(void* socket);
static bool trusted_writer;                                                /* the writer had CAP_NET_ADMIN (captured at write time) */
void mgmt_foreach_socket(void (*fn)(void* socket, void* arg), void* arg);
enum { S_OK = 0, S_UNKNOWN = 0x01, S_NOT_CONNECTED = 0x02, S_FAILED = 0x03, S_CONNECT_FAILED = 0x04, S_AUTH_FAILED = 0x05, S_NOT_PAIRED = 0x06,
       S_NO_RESOURCES = 0x07, S_TIMEOUT = 0x08, S_ALREADY_CONNECTED = 0x09, S_BUSY = 0x0A, S_REJECTED = 0x0B, S_NOT_SUPPORTED = 0x0C,
       S_INVALID = 0x0D, S_DISCONNECTED = 0x0E, S_NOT_POWERED = 0x0F, S_CANCELLED = 0x10, S_INVALID_INDEX = 0x11, S_ALREADY_PAIRED = 0x13,
       S_PERMISSION = 0x14 };
static const uint16_t COMMANDS[] = {0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007, 0x0008, 0x0009, 0x000A, 0x000B, 0x000D, 0x000E,
    0x000F, 0x0010, 0x0011, 0x0012, 0x0013, 0x0014, 0x0015, 0x0016, 0x0017, 0x0018, 0x0019, 0x001A, 0x001B, 0x001C, 0x001D, 0x001E,
    0x001F, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027, 0x0028, 0x0029, 0x002A, 0x002C, 0x002D, 0x002E, 0x0030, 0x0031, 0x0033, 0x0034,
    0x0035, 0x0036, 0x003A, 0x003C, 0x003D, 0x003E, 0x003F, 0x0040, 0x0041, 0x0042, 0x0043, 0x0046, 0x0048, 0x0049, 0x004B, 0x004F, 0x0050, 0x0053};
static const uint16_t EVENTS[] = {0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007, 0x0008, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D,
    0x000E, 0x000F, 0x0010, 0x0011, 0x0012, 0x0013, 0x0014, 0x0015, 0x0016, 0x0017, 0x0018, 0x0019, 0x001A, 0x001B, 0x001D, 0x001E, 0x0020,
    0x0021, 0x0023, 0x0024, 0x0025, 0x002A};

/* ---------- sending ---------- */
static void send_to(void* s, unsigned event, int index, const void* data, unsigned length) {
    uint8_t m[BT_FRAME_MAX]; if (length > sizeof(m) - 6) length = sizeof(m) - 6;
    bt_put16(m, event); bt_put16(m + 2, index < 0 ? 0xFFFF : (unsigned)index); bt_put16(m + 4, length); if (length) memcpy(m + 6, data, length);
    mgmt_socket_send(s, m, 6 + length);
}
struct event_args { unsigned event; int index; const void* data; unsigned length; const void* skip; };
static void event_one(void* s, void* a) { struct event_args* e = a; if (s != e->skip) send_to(s, e->event, e->index, e->data, e->length); }
void mgmt_event(unsigned event, int index, const void* data, unsigned length, const void* skip) {
    struct event_args a = {event, index, data, length, skip}; mgmt_foreach_socket(event_one, &a);
}
static void complete(void* s, unsigned opcode, int index, unsigned status, const void* data, unsigned length) {
    uint8_t m[600]; if (length > sizeof(m) - 3) length = sizeof(m) - 3;
    bt_put16(m, opcode); m[2] = (uint8_t)status; if (length) memcpy(m + 3, data, length);
    if (s) send_to(s, 0x0001, index, m, 3 + length);
}
static void status_only(void* s, unsigned opcode, int index, unsigned status) { uint8_t m[3]; bt_put16(m, opcode); m[2] = (uint8_t)status; if (s) send_to(s, 0x0002, index, m, 3); }
static uint8_t mgmt_status(int error) {
    switch (error) { case 0: return S_OK; case E_BUSY: case E_ALREADY: return S_BUSY; case E_TIMEDOUT: return S_TIMEOUT; case E_NODEV: return S_INVALID_INDEX;
                     case E_NOMEM: return S_NO_RESOURCES; case E_OPNOTSUPP: return S_NOT_SUPPORTED; case E_INVAL: return S_INVALID; case E_HOSTDOWN: return S_CONNECT_FAILED;
                     case E_ACCES: return S_AUTH_FAILED; default: return S_FAILED; }
}

/* ---------- pending operations (PAIR_DEVICE, DISCONNECT) ---------- */
struct pending { bool used; void* socket; int index; uint32_t gen; uint16_t opcode; uint8_t addr[7]; uint32_t conn; uint64_t deadline; };
static struct pending pendings[32];
static struct pending* pending_new(void* s, struct bt_dev* d, unsigned opcode, const uint8_t* addr, uint32_t conn, uint64_t timeout) {
    for (unsigned i = 0; i < 32; i++) if (!pendings[i].used) {
        struct pending* p = &pendings[i]; *p = (struct pending){true, s, d->index, d->generation, (uint16_t)opcode, {0}, conn, timeout ? bt_now() + timeout : 0};
        memcpy(p->addr, addr, 7); return p;
    }
    return NULL;
}
static void pending_done(struct pending* p, unsigned status) {
    if (!p || !p->used) return;
    p->used = false; complete(p->socket, p->opcode, p->index, status, p->addr, 7);
}
static struct pending* pending_find(unsigned opcode, int index, const uint8_t* addr) {
    for (unsigned i = 0; i < 32; i++) if (pendings[i].used && pendings[i].opcode == opcode && pendings[i].index == index && !memcmp(pendings[i].addr, addr, 7)) return &pendings[i];
    return NULL;
}
void mgmt_socket_closed(void* socket) { for (unsigned i = 0; i < 32; i++) if (pendings[i].socket == socket) pendings[i].socket = NULL; }
void mgmt_dev_freed(struct bt_dev* d) {
    for (unsigned i = 0; i < 32; i++) if (pendings[i].used && pendings[i].index == d->index && pendings[i].gen == d->generation) { pending_done(&pendings[i], S_INVALID_INDEX); }
}

/* ---------- controller state helpers ---------- */
static bool busy[BT_MAX_DEVICES];
static struct bt_dev* lock_dev(int index, uint32_t gen) {
    for (;;) {
        struct bt_dev* d = bt_dev_get(index, gen); if (!d) return NULL;
        if (!busy[index]) { busy[index] = true; return d; }
        if (bt_wait(&busy[index], bt_now() + 100000000ULL) == E_INTR) return NULL;
    }
}
static void unlock_dev(int index) { busy[index] = false; task_wake_channel(&busy[index]); bt_worker_wake(); }
static bool powered(struct bt_dev* d) { return (d->flags & HCI_UP) && (d->settings & ST_POWERED); }
static uint32_t current(struct bt_dev* d) { return (d->settings & ~ST_POWERED) | (powered(d) ? ST_POWERED : 0); }
static void settings_reply(void* s, unsigned opcode, struct bt_dev* d) {
    uint8_t v[4]; bt_put32(v, current(d)); complete(s, opcode, d->index, S_OK, v, 4);
}
static void new_settings(struct bt_dev* d, const void* skip) { uint8_t v[4]; bt_put32(v, current(d)); mgmt_event(0x0006, d->index, v, 4, skip); }
static int sync(struct bt_dev** d, unsigned opcode, const void* p, unsigned n) {
    int index = (*d)->index; uint32_t gen = (*d)->generation;
    int r = bt_cmd_sync(*d, opcode, p, n, NULL, 0);
    *d = bt_dev_get(index, gen); if (!*d) return E_NODEV;
    return r < 0 ? r : r ? E_IO : 0;
}
static bool uuid16(const struct bt_uuid* u, uint16_t* v) {
    static const uint8_t base[12] = {0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00};
    if (memcmp(u->uuid, base, 12) || u->uuid[14] || u->uuid[15]) return false;
    *v = (uint16_t)bt_get16(u->uuid + 12); return true;
}
static void class_of_device(struct bt_dev* d, uint8_t* cod) {
    uint8_t services = 0; for (unsigned i = 0; i < 24; i++) if (d->uuids[i].used) services |= d->uuids[i].svc_hint;
    cod[0] = d->minor; cod[1] = (uint8_t)(d->major & 0x1F); cod[2] = services;
    if ((d->settings & ST_DISCOVERABLE) && d->discov_limited) cod[1] |= 0x20;
}
static unsigned build_eir(struct bt_dev* d, uint8_t* eir) {
    unsigned n = 0, length = (unsigned)strlen(d->name);
    if (length) { bool shortened = length > 48; if (shortened) length = 48; eir[n++] = (uint8_t)(length + 1); eir[n++] = shortened ? 0x08 : 0x09; memcpy(eir + n, d->name, length); n += length; }
    if (d->did_source) { eir[n++] = 9; eir[n++] = 0x10; bt_put16(eir + n, d->did_source); bt_put16(eir + n + 2, d->did_vendor); bt_put16(eir + n + 4, d->did_product); bt_put16(eir + n + 6, d->did_version); n += 8; }
    unsigned start = n, count = 0;
    for (unsigned i = 0; i < 24; i++) {
        uint16_t v; if (!d->uuids[i].used || !uuid16(&d->uuids[i], &v) || v < 0x1100) continue;
        if (!count) { n += 2; }
        if (n + 2 > 238) break;
        bt_put16(eir + n, v); n += 2; count++;
    }
    if (count) { eir[start] = (uint8_t)(1 + 2 * count); eir[start + 1] = 0x03; }
    start = n; count = 0;
    for (unsigned i = 0; i < 24; i++) {
        uint16_t v; if (!d->uuids[i].used || uuid16(&d->uuids[i], &v)) continue;
        if (n + 2 + 16 * (count + 1) > 240) break;
        if (!count) n += 2;
        memcpy(eir + n, d->uuids[i].uuid, 16); n += 16; count++;
    }
    if (count) { eir[start] = (uint8_t)(1 + 16 * count); eir[start + 1] = 0x07; }
    return n;
}
static unsigned build_adv(struct bt_dev* d, uint8_t* ad, bool scan_response) {
    unsigned n = 0;
    if (d->adv_instance) {
        if (scan_response) { memcpy(ad, d->inst_rsp, d->inst_rsp_len); n = d->inst_rsp_len; }
        else {
            if (d->inst_flags & 0x0E) {                                    /* MGMT_ADV_FLAG_DISCOV/LIMITED/MANAGED_FLAGS */
                uint8_t f = (d->inst_flags & 0x02) || ((d->inst_flags & 0x08) && (d->settings & ST_DISCOVERABLE)) ? 0x02 : 0;
                if (d->inst_flags & 0x04) f = 0x01;
                if (!(d->settings & ST_BREDR)) f |= 0x04;
                ad[n++] = 2; ad[n++] = 0x01; ad[n++] = f;
            }
            if (n + d->inst_adv_len <= 31) { memcpy(ad + n, d->inst_adv, d->inst_adv_len); n += d->inst_adv_len; }
        }
        if (scan_response && (d->inst_flags & 0x40)) {                    /* MGMT_ADV_FLAG_LOCAL_NAME */
            unsigned length = (unsigned)strlen(d->name); if (n + 2 + length > 31) length = n + 2 < 31 ? 31 - n - 2 : 0;
            if (length) { ad[n++] = (uint8_t)(length + 1); ad[n++] = length < strlen(d->name) ? 0x08 : 0x09; memcpy(ad + n, d->name, length); n += length; }
        }
        return n;
    }
    if (scan_response) {
        unsigned length = (unsigned)strlen(d->name); if (length > 29) length = 29;
        if (length) { ad[n++] = (uint8_t)(length + 1); ad[n++] = length < strlen(d->name) ? 0x08 : 0x09; memcpy(ad + n, d->name, length); n += length; }
        return n;
    }
    uint8_t f = (d->settings & ST_DISCOVERABLE) ? (d->discov_limited ? 0x01 : 0x02) : 0; if (!(d->settings & ST_BREDR)) f |= 0x04;
    ad[n++] = 2; ad[n++] = 0x01; ad[n++] = f;
    if (d->appearance) { ad[n++] = 3; ad[n++] = 0x19; bt_put16(ad + n, d->appearance); n += 2; }
    return n;
}
static bool adv_wanted(struct bt_dev* d) {
    if (!powered(d) || !(d->settings & ST_LE)) return false;
    for (struct bt_conn* c = d->conns; c; c = c->next) if (c->link == LINK_LE && c->state == CONN_CONNECTED && c->role == 1) return false;   /* one peripheral link */
    return (d->settings & ST_ADVERTISING) || d->adv_instance;
}
static void update_adv(struct bt_dev* d) {                                 /* async: may run from events */
    bool want = adv_wanted(d);
    if (d->le_adv_on) { uint8_t off = 0; bt_cmd_async(d, 0x200A, &off, 1, NULL, NULL, 0); d->le_adv_on = false; }
    if (!want) return;
    bool connectable = (d->settings & ST_CONNECTABLE) || d->adv_connectable_forced || (d->adv_instance && (d->inst_flags & 0x01));
    uint8_t p[15] = {0}; unsigned interval = (d->settings & ST_DISCOVERABLE) ? 0x00A0 : 0x0800;
    bt_put16(p, interval); bt_put16(p + 2, interval + 0x50); p[4] = connectable ? 0x00 : (d->inst_rsp_len || !d->adv_instance ? 0x02 : 0x03); p[13] = 0x07;
    bt_cmd_async(d, 0x2006, p, 15, NULL, NULL, 0);
    uint8_t data[32] = {0}; data[0] = (uint8_t)build_adv(d, data + 1, false); bt_cmd_async(d, 0x2008, data, 32, NULL, NULL, 0);
    memset(data, 0, sizeof(data)); data[0] = (uint8_t)build_adv(d, data + 1, true); bt_cmd_async(d, 0x2009, data, 32, NULL, NULL, 0);
    uint8_t on = 1; bt_cmd_async(d, 0x200A, &on, 1, NULL, NULL, 0); d->le_adv_on = true;
}
static void update_scan(struct bt_dev* d) {                                /* page/inquiry scan from connectable/discoverable */
    if (!powered(d) || !(d->settings & ST_BREDR)) return;
    uint8_t scan = (d->settings & ST_CONNECTABLE ? 0x02 : 0) | (d->settings & ST_DISCOVERABLE ? 0x01 : 0);
    if (d->accept_count && !(scan & 0x02)) scan |= 0x02;                   /* devices allowed to connect need page scan */
    if (scan == d->scan_enable) return;
    if (scan & 0x01) { uint8_t iac[7]; if (d->discov_limited) { iac[0] = 2; iac[1] = 0x00; iac[2] = 0x8B; iac[3] = 0x9E; iac[4] = 0x33; iac[5] = 0x8B; iac[6] = 0x9E; } else { iac[0] = 1; iac[1] = 0x33; iac[2] = 0x8B; iac[3] = 0x9E; }
                       bt_cmd_async(d, 0x0C3A, iac, d->discov_limited ? 7 : 4, NULL, NULL, 0); }
    bt_cmd_async(d, 0x0C1A, &scan, 1, NULL, NULL, 0); d->scan_enable = scan;
}
static void update_class_eir(struct bt_dev* d) {
    if (!powered(d) || !(d->settings & ST_BREDR)) return;
    uint8_t cod[3]; class_of_device(d, cod); bt_cmd_async(d, 0x0C24, cod, 3, NULL, NULL, 0);
    if (d->settings & ST_SSP) { uint8_t eir[241] = {0}; build_eir(d, eir + 1); bt_cmd_async(d, 0x0C52, eir, 241, NULL, NULL, 0); }
}
static void update_name(struct bt_dev* d) {
    if (!powered(d)) return;
    if (d->settings & ST_BREDR) { uint8_t name[248] = {0}; memcpy(name, d->name, strlen(d->name) < 248 ? strlen(d->name) : 248); bt_cmd_async(d, 0x0C13, name, 248, NULL, NULL, 0); }
    update_class_eir(d); if (d->le_adv_on) update_adv(d);
}
/* Power on/off hook from the core (and HCIDEVUP): write the stored settings to the controller. */
void mgmt_powered(struct bt_dev* d, bool on) {
    if (!on) {
        d->settings &= ~ST_POWERED; d->scan_enable = 0; d->le_adv_on = false; d->le_scan_active = d->inquiry_active = false;
        if (d->discovering) { uint8_t ev[2] = {d->discovery_type, 0}; d->discovering = 0; mgmt_event(0x0013, d->index, ev, 2, NULL); }
        for (unsigned i = 0; i < 32; i++) if (pendings[i].used && pendings[i].index == d->index && pendings[i].gen == d->generation) pending_done(&pendings[i], S_NOT_POWERED);
        return;
    }
    d->settings |= ST_POWERED;
    if (d->settings & ST_LINK_SEC) { uint8_t on1 = 1; bt_cmd_async(d, 0x0C20, &on1, 1, NULL, NULL, 0); }
    update_name(d); update_scan(d); update_adv(d);
}

/* ---------- discovery ---------- */
static void discovery_stop(struct bt_dev* d, bool notify) {
    if (d->inquiry_active) { bt_cmd_async(d, 0x0402, NULL, 0, NULL, NULL, 0); d->inquiry_active = false; }
    if (d->le_scan_active) { uint8_t off[2] = {0, 0}; bt_cmd_async(d, 0x200C, off, 2, NULL, NULL, 0); d->le_scan_active = false; }
    for (unsigned i = 0; i < d->found_count; i++) if (d->found[i].name_pending) { bt_cmd_async(d, 0x041A, d->found[i].addr, 6, NULL, NULL, 0); d->found[i].name_pending = false; }
    if (d->discovering && notify) { uint8_t ev[2] = {d->discovery_type, 0}; mgmt_event(0x0013, d->index, ev, 2, NULL); }
    d->discovering = 0; d->name_phase = false; d->discovery_deadline = 0; d->filter_rssi = 127; d->filter_uuids = 0;
}
static bool next_name(struct bt_dev* d) {
    for (unsigned i = 0; i < d->found_count; i++) {
        struct bt_found* f = &d->found[i];
        if (f->type || !f->name_needed || f->name_pending) continue;
        uint8_t p[10]; memcpy(p, f->addr, 6); p[6] = f->page_scan_mode; p[7] = 0; bt_put16(p + 8, f->clock_offset | 0x8000);
        f->name_needed = false;
        if (bt_cmd_async(d, 0x0419, p, 10, NULL, NULL, 0)) { f->name_pending = true; d->discovery_deadline = bt_now() + 6000000000ULL; return true; }
    }
    return false;
}
static void discovery_phase_done(struct bt_dev* d) {
    if (!d->discovering || d->inquiry_active || d->le_scan_active) return;
    if (!d->name_phase) { d->name_phase = true; if (next_name(d)) return; }
    discovery_stop(d, true);
}
static struct bt_found* found_entry(struct bt_dev* d, const uint8_t* addr, uint8_t type) {
    for (unsigned i = 0; i < d->found_count; i++) if (d->found[i].type == type && !memcmp(d->found[i].addr, addr, 6)) return &d->found[i];
    if (d->found_count == 32) return NULL;
    struct bt_found* f = &d->found[d->found_count++]; memset(f, 0, sizeof(*f)); memcpy(f->addr, addr, 6); f->type = type; return f;
}
static bool eir_has(const uint8_t* eir, unsigned length, uint8_t type) {
    for (unsigned at = 0; at + 1 < length && eir[at];) { unsigned l = eir[at]; if (at + 1 + l > length) break; if (eir[at + 1] == type) return true; at += 1 + l; }
    return false;
}
static bool eir_uuid_match(struct bt_dev* d, const uint8_t* eir, unsigned length) {
    if (!d->filter_uuids) return true;
    for (unsigned at = 0; at + 1 < length && eir[at];) {
        unsigned l = eir[at], t = eir[at + 1]; if (at + 1 + l > length) break;
        const uint8_t* v = eir + at + 2; unsigned vl = l - 1;
        for (unsigned u = 0; u < d->filter_uuids; u++) {
            const uint8_t* want = d->filter_uuid[u];
            if ((t == 0x02 || t == 0x03)) for (unsigned k = 0; k + 2 <= vl; k += 2) if (!memcmp(want + 12, v + k, 2) && !want[14] && !want[15]) return true;
            if ((t == 0x06 || t == 0x07)) for (unsigned k = 0; k + 16 <= vl; k += 16) if (!memcmp(want, v + k, 16)) return true;
        }
        at += 1 + l;
    }
    return false;
}
static void device_found(struct bt_dev* d, const uint8_t* addr, uint8_t type, int8_t rssi, uint32_t flags, const uint8_t* eir, unsigned eir_len, const uint8_t* cod) {
    if (!d->discovering) return;
    if (d->filter_rssi != 127 && rssi != 127 && rssi < d->filter_rssi) return;
    if (!eir_uuid_match(d, eir, eir_len)) return;
    uint8_t m[14 + 260]; memcpy(m, addr, 6); m[6] = type; m[7] = (uint8_t)rssi; bt_put32(m + 8, flags);
    if (eir_len > 240) eir_len = 240;
    memcpy(m + 14, eir, eir_len);
    if (cod && !eir_has(eir, eir_len, 0x0D)) { m[14 + eir_len] = 4; m[15 + eir_len] = 0x0D; memcpy(m + 16 + eir_len, cod, 3); eir_len += 5; }
    bt_put16(m + 12, eir_len); mgmt_event(0x0012, d->index, m, 14 + eir_len, NULL);
}
static void inquiry_result(struct bt_dev* d, const uint8_t* addr, uint8_t psrm, const uint8_t* cod, uint16_t clock, int8_t rssi, const uint8_t* eir, unsigned eir_len) {
    struct bt_found* f = found_entry(d, addr, 0);
    bool named = eir && (eir_has(eir, eir_len, 0x09));
    if (f) { f->page_scan_mode = psrm; f->clock_offset = clock; f->rssi = rssi; if (!named) f->name_needed = true; }
    unsigned length = 0; if (eir) for (; length + 1 < eir_len && eir[length]; length += 1U + eir[length]) if (length + 1U + eir[length] > eir_len) break;
    if (length > eir_len) length = eir_len;
    device_found(d, addr, 0, rssi, (named ? 0 : 0x01) | (eir ? 0 : 0x02), eir, length, cod);
}
static void le_report(struct bt_dev* d, const uint8_t* r, unsigned avail) {
    unsigned n = r[0]; const uint8_t* q = r + 1;
    for (unsigned i = 0; i < n; i++) {
        if ((unsigned)(q - r) + 9 > avail) return;
        unsigned type = q[0], addr_type = q[1] & 1, len = q[8];
        if ((unsigned)(q - r) + 10 + len > avail || len > 31) return;
        const uint8_t* data = q + 9; int8_t rssi = (int8_t)q[9 + len];
        struct bt_found* f = found_entry(d, q + 2, (uint8_t)(addr_type + 1));
        uint8_t eir[62]; unsigned el = 0;
        if (type == 4 && f && f->adv_len) { memcpy(eir, f->adv, f->adv_len); el = f->adv_len; }   /* scan response: with the advertisement */
        memcpy(eir + el, data, len); el += len;
        if (type != 4 && f) { memcpy(f->adv, data, len); f->adv_len = (uint8_t)len; }
        device_found(d, q + 2, (uint8_t)(addr_type + 1), rssi, type == 3 || type == 2 ? 0x04 : 0, eir, el, NULL);
        q += 10 + len;
    }
}
static int start_discovery(struct bt_dev* d, uint8_t type, bool limited, void* owner) {
    if (!powered(d)) return S_NOT_POWERED;
    if (d->discovering) return S_BUSY;
    bool want_bredr = type & 1, want_le = type & 6;
    if (!type || (type & ~7)) return S_INVALID;
    if ((want_bredr && !(d->settings & ST_BREDR)) || (want_le && !(d->settings & ST_LE))) return want_bredr && !(d->settings & ST_BREDR) && !(d->settings & ST_LE) ? S_REJECTED : S_REJECTED;
    d->found_count = 0; d->discovery_type = type; d->discovery_owner = owner; d->name_phase = false;
    d->discovering = 1; d->discovery_deadline = 0;   /* reports that come with the scan-enable completion count already */
    d->le_scan_active = want_le;                      /* a quick Inquiry Complete must not end a discovery whose scan is starting */
    if (want_bredr) {
        uint8_t inquiry[5] = {limited ? 0x00 : 0x33, 0x8B, 0x9E, 0x08, 0x00};
        d->inquiry_active = true;
        int r = sync(&d, 0x0401, inquiry, 5); if (!d) return S_INVALID_INDEX;
        if (r) { d->inquiry_active = false; d->discovering = 0; return S_FAILED; }
    }
    if (want_le) {
        if (d->le_adv_on && !(d->le_states[0] & 0x10)) {}                 /* advertising and scanning at once: the controller decides */
        uint8_t params[7] = {0x01, 0, 0, 0, 0, 0x00, 0x00}; bt_put16(params + 1, d->scan_interval ? d->scan_interval : 0x0012); bt_put16(params + 3, d->scan_window ? d->scan_window : 0x0012);
        int r = sync(&d, 0x200B, params, 7); if (!d) return S_INVALID_INDEX;
        uint8_t on[2] = {1, 1};
        if (!r) { d->le_scan_active = true; r = sync(&d, 0x200C, on, 2); }
        if (!d) return S_INVALID_INDEX;
        if (r) { d->le_scan_active = false; d->discovering = 0; if (d->inquiry_active) { bt_cmd_async(d, 0x0402, NULL, 0, NULL, NULL, 0); d->inquiry_active = false; } return S_FAILED; }
    }
    d->discovering = 1; d->discovery_deadline = bt_now() + 10240000000ULL + (want_bredr ? 2000000000ULL : 0);
    uint8_t ev[2] = {type, 1}; mgmt_event(0x0013, d->index, ev, 2, NULL);
    return S_OK;
}

/* ---------- keys ---------- */
static struct bt_key* key_find(struct bt_dev* d, const uint8_t* addr, uint8_t type, int kind) {   /* kind: 0 link key, 1 LTK central, 2 LTK peripheral, -1 any LTK */
    for (unsigned i = 0; i < 32; i++) {
        struct bt_key* k = &d->keys[i];
        if (!k->used || memcmp(k->addr, addr, 6)) continue;
        if (kind == 0 && !k->type) return k;
        if (kind && k->type && k->type == type && (kind < 0 || (kind == 1) == (k->central != 0) || k->key_type >= 2)) return k;
    }
    return NULL;
}
static struct bt_key* key_slot(struct bt_dev* d) { for (unsigned i = 0; i < 32; i++) if (!d->keys[i].used) return &d->keys[i]; return &d->keys[31]; }
struct bt_key* mgmt_find_ltk(struct bt_dev* d, const uint8_t* addr, uint8_t type, bool central) { return key_find(d, addr, type, central ? 1 : 2); }
struct bt_key* mgmt_find_ltk_rand(struct bt_dev* d, uint16_t ediv, const uint8_t* rand) {
    for (unsigned i = 0; i < 32; i++) { struct bt_key* k = &d->keys[i]; if (k->used && k->type && !k->central && k->ediv == ediv && !memcmp(k->rand, rand, 8)) return k; }
    return NULL;
}
static unsigned remove_keys(struct bt_dev* d, const uint8_t* addr, uint8_t type) {
    unsigned n = 0;
    for (unsigned i = 0; i < 32; i++) if (d->keys[i].used && !memcmp(d->keys[i].addr, addr, 6) && (d->keys[i].type ? type != 0 : type == 0)) { memset(&d->keys[i], 0, sizeof(d->keys[i])); n++; }
    for (unsigned i = 0; i < 16; i++) if (d->irks[i].used && !memcmp(d->irks[i].addr, addr, 6) && type) { memset(&d->irks[i], 0, sizeof(d->irks[i])); n++; }
    return n;
}
bool mgmt_bondable(struct bt_dev* d) { return (d->settings & ST_BONDABLE) != 0; }
void mgmt_new_ltk(struct bt_conn* c, const uint8_t* ltk, uint8_t key_size, bool authenticated, bool central_role, uint16_t ediv, const uint8_t* rand, bool sc) {
    struct bt_dev* d = c->dev;
    struct bt_key* k = key_find(d, c->peer, c->peer_type, sc ? -1 : central_role ? 1 : 2); if (!k) k = key_slot(d);
    memset(k, 0, sizeof(*k)); k->used = true; memcpy(k->addr, c->peer, 6); k->type = c->peer_type; k->key_type = (uint8_t)((sc ? 2 : 0) + (authenticated ? 1 : 0));
    k->central = central_role && !sc; k->enc_size = key_size; k->ediv = ediv; if (rand) memcpy(k->rand, rand, 8); memcpy(k->val, ltk, 16); k->authenticated = authenticated;
    uint8_t m[37]; m[0] = c->bonding ? 1 : 0; memcpy(m + 1, c->peer, 6); m[7] = c->peer_type; m[8] = k->key_type; m[9] = k->central; m[10] = key_size;
    bt_put16(m + 11, ediv); memcpy(m + 13, k->rand, 8); memcpy(m + 21, ltk, 16); mgmt_event(0x000A, d->index, m, 37, NULL);
}
void mgmt_new_irk(struct bt_conn* c, const uint8_t* irk, const uint8_t* identity, uint8_t identity_type) {
    struct bt_dev* d = c->dev; struct bt_irk* r = NULL;
    for (unsigned i = 0; i < 16 && !r; i++) if (d->irks[i].used && !memcmp(d->irks[i].addr, identity, 6)) r = &d->irks[i];
    for (unsigned i = 0; i < 16 && !r; i++) if (!d->irks[i].used) r = &d->irks[i];
    if (!r) r = &d->irks[15];
    r->used = true; memcpy(r->addr, identity, 6); r->type = identity_type; memcpy(r->val, irk, 16);
    uint8_t m[30]; m[0] = c->bonding ? 1 : 0; memset(m + 1, 0, 6); if (c->peer_type == 2 && (c->peer[5] & 0xC0) == 0x40) memcpy(m + 1, c->peer, 6);
    memcpy(m + 7, identity, 6); m[13] = identity_type; memcpy(m + 14, irk, 16); mgmt_event(0x0018, d->index, m, 30, NULL);
}
void mgmt_new_link_key_le(struct bt_conn* c, const uint8_t* addr, const uint8_t* key, uint8_t type) {   /* SC-derived BR/EDR key */
    struct bt_dev* d = c->dev; struct bt_key* k = key_find(d, addr, 0, 0); if (!k) k = key_slot(d);
    memset(k, 0, sizeof(*k)); k->used = true; memcpy(k->addr, addr, 6); k->type = 0; k->key_type = type; memcpy(k->val, key, 16);
    uint8_t m[26]; m[0] = c->bonding ? 1 : 0; memcpy(m + 1, addr, 6); m[7] = 0; m[8] = type; memcpy(m + 9, key, 16); m[25] = 0;
    mgmt_event(0x0009, d->index, m, 26, NULL);
}
void mgmt_new_csrk(struct bt_conn* c, const uint8_t* csrk, uint8_t type) {   /* NEW_CSRK {store_hint, addr, type, val} */
    uint8_t m[25]; bool unresolvable = c->peer_type == 2 && (c->peer[5] & 0xC0) != 0xC0;   /* random, not static: not stored (Linux) */
    m[0] = c->bonding && !unresolvable ? 1 : 0; memcpy(m + 1, c->peer, 6); m[7] = c->peer_type; m[8] = type; memcpy(m + 9, csrk, 16);
    mgmt_event(0x0019, c->dev->index, m, 25, NULL);
}

/* ---------- pairing ---------- */
static void addr_info(uint8_t* out, const struct bt_conn* c) { memcpy(out, c->peer, 6); out[6] = c->peer_type; }
void mgmt_pair_done(struct bt_conn* c, int mgmt_status_code) {
    struct pending* p = c->pair_op; c->pair_op = NULL;
    if (p) pending_done(p, (unsigned)mgmt_status_code);
    c->pairing = false; if (mgmt_status_code) bt_conn_idle(c);
}
void mgmt_auth_failed(struct bt_conn* c, uint8_t status) {
    if (c->pair_op) { mgmt_pair_done(c, status); return; }
    uint8_t m[8]; addr_info(m, c); m[7] = status; mgmt_event(0x0011, c->dev->index, m, 8, NULL);
}
void mgmt_confirm_request(struct bt_conn* c, uint32_t value, bool hint) { uint8_t m[12]; addr_info(m, c); m[7] = hint; bt_put32(m + 8, value); mgmt_event(0x000F, c->dev->index, m, 12, NULL); }
void mgmt_passkey_request(struct bt_conn* c) { uint8_t m[7]; addr_info(m, c); mgmt_event(0x0010, c->dev->index, m, 7, NULL); }
void mgmt_passkey_notify(struct bt_conn* c, uint32_t passkey) { uint8_t m[12]; addr_info(m, c); bt_put32(m + 7, passkey); m[11] = 0; mgmt_event(0x0017, c->dev->index, m, 12, NULL); }
static uint8_t disconnect_reason(uint8_t hci) { return hci == 0x08 ? 1 : hci == 0x16 ? 2 : hci == 0x13 || hci == 0x14 || hci == 0x15 ? 3 : hci == 0x05 ? 4 : 0; }
void mgmt_conn_complete(struct bt_conn* c, int status) {
    struct bt_dev* d = c->dev;
    if (status) {
        if (c->pair_op) { mgmt_pair_done(c, S_CONNECT_FAILED); return; }
        if (c->outgoing) { uint8_t m[8]; addr_info(m, c); m[7] = S_CONNECT_FAILED; mgmt_event(0x000D, d->index, m, 8, NULL); }
        return;
    }
    if (!c->pair_op) c->io_cap = d->io_cap;
    uint8_t m[13 + 260]; addr_info(m, c); bt_put32(m + 7, c->outgoing ? 0x08 : 0); unsigned el = 0;
    if (c->link == LINK_ACL) {
        for (unsigned i = 0; i < d->found_count; i++) if (!d->found[i].type && !memcmp(d->found[i].addr, c->peer, 6)) { c->page_scan_mode = d->found[i].page_scan_mode; }
        if (c->dev_class[0] | c->dev_class[1] | c->dev_class[2]) { m[13 + el] = 4; m[14 + el] = 0x0D; memcpy(m + 15 + el, c->dev_class, 3); el += 5; }
    }
    bt_put16(m + 11, el); mgmt_event(0x000B, d->index, m, 13 + el, NULL); c->mgmt_connected = true;
    if (c->pair_op && c->link == LINK_ACL) {                               /* PAIR_DEVICE: authenticate the new link */
        if (!bt_conn_security(c, SEC_MEDIUM, true)) return;
        mgmt_pair_done(c, S_OK);
    } else if (c->pair_op && c->link == LINK_LE) {
        if (smp_start(c, SEC_MEDIUM) < 0) mgmt_pair_done(c, S_AUTH_FAILED);
    }
}
void mgmt_conn_closed(struct bt_conn* c, uint8_t hci_reason) {
    struct bt_dev* d = c->dev;
    if (c->pair_op) mgmt_pair_done(c, c->mgmt_connected ? S_DISCONNECTED : S_CONNECT_FAILED);
    uint8_t a[7]; addr_info(a, c);
    struct pending* p = pending_find(0x0014, d->index, a); if (p) pending_done(p, S_OK);
    if (c->mgmt_connected) { uint8_t m[8]; memcpy(m, a, 7); m[7] = disconnect_reason(hci_reason); mgmt_event(0x000C, d->index, m, 8, NULL); }
    else if (c->outgoing && c->state != CONN_CONNECTED && hci_reason && hci_reason != 0x16) { uint8_t m[8]; memcpy(m, a, 7); m[7] = S_CONNECT_FAILED; mgmt_event(0x000D, d->index, m, 8, NULL); }
    if (c->link == LINK_LE && c->role == 1 && d->alive && powered(d)) { c->role = 0; update_adv(d); }
}
/* SSP and legacy pairing events, inquiry/advertising reports, remote names (from the core) */
void mgmt_hci_event(struct bt_dev* d, const uint8_t* e, unsigned length) {
    (void)length; const uint8_t* p = e + 2; unsigned n = e[1]; struct bt_conn* c;
    switch (e[0]) {
    case 0x01: d->inquiry_active = false; discovery_phase_done(d); return;   /* Inquiry Complete */
    case 0x02: for (unsigned i = 0; i < p[0] && n >= 1 + 14U * (i + 1); i++) { const uint8_t* r = p + 1 + 14 * i; inquiry_result(d, r, r[6], r + 9, (uint16_t)bt_get16(r + 12), 127, NULL, 0); } return;
    case 0x22: for (unsigned i = 0; i < p[0] && n >= 1 + 14U * (i + 1); i++) { const uint8_t* r = p + 1 + 14 * i; inquiry_result(d, r, r[6], r + 8, (uint16_t)bt_get16(r + 11), (int8_t)r[13], NULL, 0); } return;
    case 0x2F: if (n >= 255) inquiry_result(d, p + 1, p[7], p + 9, (uint16_t)bt_get16(p + 12), (int8_t)p[14], p + 15, 240); return;
    case 0x07: {                                                           /* Remote Name Request Complete */
        if (n < 7) return;
        char name[249] = {0}; if (!p[0] && n >= 255) { memcpy(name, p + 7, 248); }
        if ((c = bt_conn_lookup(d, p + 1, LINK_ACL)) && !p[0]) { memcpy(c->name, name, 249); c->name_known = true; }
        for (unsigned i = 0; i < d->found_count; i++) if (!d->found[i].type && !memcmp(d->found[i].addr, p + 1, 6) && d->found[i].name_pending) {
            d->found[i].name_pending = false;
            uint8_t eir[250]; unsigned el = 0, nl = (unsigned)strlen(name);
            if (!p[0] && nl) { eir[0] = (uint8_t)(nl + 1); eir[1] = 0x09; memcpy(eir + 2, name, nl); el = nl + 2; }
            device_found(d, p + 1, 0, d->found[i].rssi, p[0] ? 0x10 : 0, eir, el, NULL);   /* NAME_REQUEST_FAILED */
            if (d->name_phase && !next_name(d)) discovery_stop(d, true);
        }
        return;
    }
    case 0x16: {                                                           /* PIN Code Request */
        if (n < 6 || !(c = bt_conn_lookup(d, p, LINK_ACL))) { bt_cmd_async(d, 0x040E, p, 6, NULL, NULL, 0); return; }
        if (!mgmt_bondable(d) && !c->pair_op) { bt_cmd_async(d, 0x040E, p, 6, NULL, NULL, 0); return; }
        uint8_t m[8]; addr_info(m, c); m[7] = c->pending_sec >= SEC_HIGH; mgmt_event(0x000E, d->index, m, 8, NULL); return;
    }
    case 0x17: {                                                           /* Link Key Request */
        if (n < 6) return;
        struct bt_key* k = key_find(d, p, 0, 0); c = bt_conn_lookup(d, p, LINK_ACL);
        bool usable = k && !(c && c->pending_sec >= SEC_HIGH && k->key_type != 0x05 && k->key_type != 0x08);
        if (usable) { uint8_t q[22]; memcpy(q, p, 6); memcpy(q + 6, k->val, 16); if (c) { c->key_type = k->key_type; c->link_key_known = true; } bt_cmd_async(d, 0x040B, q, 22, NULL, NULL, 0); }
        else bt_cmd_async(d, 0x040C, p, 6, NULL, NULL, 0);
        return;
    }
    case 0x18: {                                                           /* Link Key Notification */
        if (n < 23) return;
        c = bt_conn_lookup(d, p, LINK_ACL);
        bool bonding = !c || ((c->auth_type & ~1) != 0x00 && ((c->remote_auth == 0xFF) || (c->remote_auth & ~1) != 0x00));
        if (c && c->pair_op) bonding = true;
        struct bt_key* k = key_find(d, p, 0, 0); if (!k) k = key_slot(d);
        if (bonding || (c && c->link_key_known)) { memset(k, 0, sizeof(*k)); k->used = true; memcpy(k->addr, p, 6); k->type = 0; k->key_type = p[22]; memcpy(k->val, p + 6, 16); }
        if (c) { c->key_type = p[22]; c->link_key_known = true; }
        uint8_t m[26]; m[0] = bonding ? 1 : 0; memcpy(m + 1, p, 6); m[7] = 0; m[8] = p[22]; memcpy(m + 9, p + 6, 16); m[25] = 0;
        mgmt_event(0x0009, d->index, m, 26, NULL); return;
    }
    case 0x31: {                                                           /* IO Capability Request */
        if (n < 6) return;
        c = bt_conn_lookup(d, p, LINK_ACL);
        if (!c || (!mgmt_bondable(d) && !c->pair_op && c->remote_auth != 0xFF && (c->remote_auth & ~1) != 0x00 && !c->auth_pending)) {
            uint8_t q[7]; memcpy(q, p, 6); q[6] = 0x18; bt_cmd_async(d, 0x0434, q, 7, NULL, NULL, 0); return;
        }
        c->ssp_initiator = c->auth_pending; uint8_t cap = c->io_cap == 0x04 ? 0x01 : c->io_cap;
        if (c->auth_type == 0xFF) c->auth_type = 0x04;
        if (c->remote_auth == 0xFF || !c->remote_auth_known) { if (c->io_cap != 0x03 && c->auth_type != 0x00) c->auth_type |= 0x01; }
        else if ((c->remote_auth & ~1) == 0x00) c->auth_type = (uint8_t)(c->remote_auth | (c->auth_type & 1));
        else if (c->remote_io_cap != 0x03 && c->io_cap != 0x03) c->auth_type = (uint8_t)(c->remote_auth | 1);
        else c->auth_type = (uint8_t)((c->remote_auth & ~1) | (c->auth_type & 1));
        if (!mgmt_bondable(d)) c->auth_type &= 0x01;
        uint8_t q[9]; memcpy(q, p, 6); q[6] = cap; q[7] = 0; q[8] = c->auth_type; bt_cmd_async(d, 0x042B, q, 9, NULL, NULL, 0); c->pairing = true; return;
    }
    case 0x32:                                                             /* IO Capability Response */
        if (n < 9 || !(c = bt_conn_lookup(d, p, LINK_ACL))) return;
        c->remote_io_cap = p[6]; c->remote_auth = p[8]; c->remote_auth_known = true; return;
    case 0x33: {                                                           /* User Confirmation Request */
        if (n < 10) return;
        c = bt_conn_lookup(d, p, LINK_ACL); if (!c) { bt_cmd_async(d, 0x042D, p, 6, NULL, NULL, 0); return; }
        bool loc_mitm = c->auth_type & 1, rem_mitm = c->remote_auth_known && (c->remote_auth & 1);
        if (c->pending_sec > SEC_MEDIUM && c->remote_io_cap == 0x03) { bt_cmd_async(d, 0x042D, p, 6, NULL, NULL, 0); return; }
        if ((!loc_mitm || c->remote_io_cap == 0x03) && (!rem_mitm || c->io_cap == 0x03)) {
            bool hint = (!c->ssp_initiator && c->io_cap != 0x03) || key_find(d, p, 0, 0);
            if (!hint) { bt_cmd_async(d, 0x042C, p, 6, NULL, NULL, 0); return; }   /* auto-accept (Just Works, no MITM anywhere) */
            mgmt_confirm_request(c, bt_get32(p + 6), true); return;
        }
        mgmt_confirm_request(c, bt_get32(p + 6), false); return;
    }
    case 0x34: if (n >= 6) { if ((c = bt_conn_lookup(d, p, LINK_ACL))) mgmt_passkey_request(c); else bt_cmd_async(d, 0x042F, p, 6, NULL, NULL, 0); } return;
    case 0x3B: if (n >= 10 && (c = bt_conn_lookup(d, p, LINK_ACL))) mgmt_passkey_notify(c, bt_get32(p + 6)); return;
    case 0x35: if (n >= 6) bt_cmd_async(d, 0x0433, p, 6, NULL, NULL, 0); return;   /* Remote OOB Data Request: no OOB data */
    case 0x36:                                                             /* Simple Pairing Complete */
        if (n < 7 || !(c = bt_conn_lookup(d, p + 1, LINK_ACL))) return;
        c->pairing = false;
        if (p[0] && !c->ssp_initiator) { uint8_t m[8]; addr_info(m, c); m[7] = S_AUTH_FAILED; mgmt_event(0x0011, d->index, m, 8, NULL); }
        return;
    case 0x06:                                                             /* Authentication Complete (the core continues with encryption) */
        if (n < 3 || !(c = bt_conn_by_handle(d, bt_get16(p + 1) & 0x0FFF))) return;
        if (p[0] && !c->pair_op && !c->ssp_initiator) { uint8_t m[8]; addr_info(m, c); m[7] = S_AUTH_FAILED; mgmt_event(0x0011, d->index, m, 8, NULL); }
        if (p[0] && c->pair_op) mgmt_pair_done(c, p[0] == 0x06 || p[0] == 0x05 ? S_AUTH_FAILED : S_FAILED);
        return;
    case 0x3E:
        if (n >= 2 && p[0] == 0x02) le_report(d, p + 1, n - 1);
        return;
    }
}

/* ---------- commands ---------- */
static bool addr_valid(const uint8_t* a) { return a[6] <= 2; }
static struct bt_conn* conn_of(struct bt_dev* d, const uint8_t* a) { return bt_conn_lookup(d, a, a[6] ? LINK_LE : LINK_ACL); }
static void settings_command(void* s, unsigned op, struct bt_dev* d, uint32_t bit, bool on) {
    if (on) d->settings |= bit; else d->settings &= ~bit;
    settings_reply(s, op, d); new_settings(d, s);
}
static unsigned info_reply(struct bt_dev* d, uint8_t* out, bool ext) {
    memcpy(out, d->bdaddr, 6); out[6] = d->hci_ver; bt_put16(out + 7, d->manufacturer); bt_put32(out + 9, d->supported); bt_put32(out + 13, current(d));
    if (!ext) { class_of_device(d, out + 17); memcpy(out + 20, d->name, strlen(d->name)); memcpy(out + 269, d->short_name, strlen(d->short_name) < 11 ? strlen(d->short_name) : 10); return 280; }
    unsigned n = 19; uint8_t* eir = out + 19;
    if (d->settings & ST_BREDR) { eir[0] = 4; eir[1] = 0x0D; class_of_device(d, eir + 2); eir += 5; }
    if (d->settings & ST_LE) { eir[0] = 3; eir[1] = 0x19; bt_put16(eir + 2, d->appearance); eir += 4; }
    unsigned nl = (unsigned)strlen(d->name); eir[0] = (uint8_t)(nl + 1); eir[1] = 0x09; memcpy(eir + 2, d->name, nl); eir += nl + 2;
    unsigned sl = (unsigned)strlen(d->short_name); eir[0] = (uint8_t)(sl + 1); eir[1] = 0x08; memcpy(eir + 2, d->short_name, sl); eir += sl + 2;
    unsigned el = (unsigned)(eir - (out + 19)); bt_put16(out + 17, el); return n + el;
}
static int set_powered(void* s, unsigned op, struct bt_dev* d, bool on) {
    int index = d->index; uint32_t gen = d->generation;
    if (on == powered(d)) { settings_reply(s, op, d); return 0; }
    int r;
    if (on) { r = bt_dev_open(d); if (!(d = bt_dev_get(index, gen))) return E_NODEV; if (!r || r == E_ALREADY) { mgmt_powered(d, true); r = 0; } }
    else { r = bt_dev_close(d); if (!(d = bt_dev_get(index, gen))) return E_NODEV; d->settings &= ~ST_POWERED; r = 0; }
    if (r) { status_only(s, op, index, r == E_IO ? S_FAILED : mgmt_status(r)); return 0; }
    settings_reply(s, op, d); new_settings(d, s);
    if (!on) { uint8_t cod[3] = {0, 0, 0}; (void)cod; }
    return 0;
}
static void pair_device(void* s, struct bt_dev* d, const uint8_t* q) {
    const unsigned op = 0x0019;
    if (!powered(d)) { complete(s, op, d->index, S_NOT_POWERED, q, 7); return; }
    if (q[7] > 4) { complete(s, op, d->index, S_INVALID, q, 7); return; }
    struct bt_conn* c = conn_of(d, q);
    if (c && c->pair_op) { complete(s, op, d->index, S_BUSY, q, 7); return; }
    if (q[6] == 0 && c && c->state == CONN_CONNECTED && c->authenticated && c->encrypted && key_find(d, q, 0, 0)) { complete(s, op, d->index, S_ALREADY_PAIRED, q, 7); return; }
    int error = 0;
    if (!c) c = bt_connect(d, q, q[6], &error);
    if (!c) { complete(s, op, d->index, error == E_BUSY ? S_BUSY : S_CONNECT_FAILED, q, 7); return; }
    struct pending* p = pending_new(s, d, op, q, c->id, 60000000000ULL); if (!p) { complete(s, op, d->index, S_NO_RESOURCES, q, 7); return; }
    c->pair_op = p; c->io_cap = q[7]; c->bonding = true; c->pairing = true;
    if (q[6] == 0) c->auth_type = q[7] == 0x03 ? 0x02 : 0x03;             /* dedicated bonding (MITM unless NoInputNoOutput) */
    if (q[6] == 0 && c->pending_sec < SEC_MEDIUM) c->pending_sec = SEC_MEDIUM;
    if (c->state != CONN_CONNECTED) return;                                 /* mgmt_conn_complete continues */
    if (q[6] == 0) {
        if (c->authenticated && c->encrypted) { mgmt_pair_done(c, S_OK); return; }
        if (bt_conn_security(c, SEC_MEDIUM, true)) mgmt_pair_done(c, S_OK);
    } else if (smp_start(c, SEC_MEDIUM) < 0) mgmt_pair_done(c, S_AUTH_FAILED);
}
static void user_reply(void* s, unsigned op, struct bt_dev* d, const uint8_t* q, unsigned size) {
    struct bt_conn* c = conn_of(d, q);
    if (!c || c->state != CONN_CONNECTED) { complete(s, op, d->index, S_NOT_CONNECTED, q, 7); return; }
    if (q[6]) { int r = smp_user_reply(c, op, size >= 11 ? bt_get32(q + 7) : 0); complete(s, op, d->index, r ? S_FAILED : S_OK, q, 7); return; }
    uint8_t hci[23]; memcpy(hci, q, 6); unsigned opcode = 0, n = 6;
    switch (op) {
    case 0x001C: opcode = 0x042C; break;
    case 0x001D: opcode = 0x042D; break;
    case 0x001E: opcode = 0x042E; bt_put32(hci + 6, bt_get32(q + 7)); n = 10; break;
    case 0x001F: opcode = 0x042F; break;
    case 0x0016: opcode = 0x040D; hci[6] = q[7]; memcpy(hci + 7, q + 8, 16); n = 23; if (q[7] > 16 || !q[7]) { complete(s, op, d->index, S_INVALID, q, 7); return; } break;
    case 0x0017: opcode = 0x040E; break;
    }
    int index = d->index; int r = sync(&d, opcode, hci, n);
    complete(s, op, index, r == E_NODEV ? S_INVALID_INDEX : r ? S_FAILED : S_OK, q, 7);
}
static void add_uuid_reply(void* s, unsigned op, struct bt_dev* d) { uint8_t cod[3]; class_of_device(d, cod); complete(s, op, d->index, S_OK, cod, 3); mgmt_event(0x0007, d->index, cod, 3, s); update_class_eir(d); }
static const unsigned SIZES[] = {[0x05] = 1, [0x07] = 1, [0x08] = 1, [0x09] = 1, [0x0A] = 1, [0x0B] = 1, [0x0D] = 1, [0x0E] = 2, [0x0F] = 260,
    [0x10] = 17, [0x11] = 16, [0x14] = 7, [0x16] = 24, [0x17] = 7, [0x18] = 1, [0x19] = 8, [0x1A] = 7, [0x1B] = 8, [0x1C] = 7, [0x1D] = 7, [0x1E] = 11,
    [0x1F] = 7, [0x23] = 1, [0x24] = 1, [0x25] = 8, [0x26] = 7, [0x27] = 7, [0x28] = 8, [0x29] = 1, [0x2A] = 1, [0x2C] = 4, [0x2D] = 1, [0x2E] = 1,
    [0x31] = 7, [0x33] = 8, [0x34] = 7, [0x41] = 1, [0x43] = 2, [0x3F] = 1, [0x40] = 5, [0x4F] = 7, [0x50] = 11, [0x06] = 3};
static void command(void* s, unsigned op, unsigned index, const uint8_t* q, unsigned size) {
    uint8_t out[600]; memset(out, 0, sizeof(out));
    if (op < sizeof(SIZES) / sizeof(SIZES[0]) && SIZES[op] && size != SIZES[op]) { status_only(s, op, (int)index, S_INVALID); return; }
    if (op <= 0x0003 || op == 0x003C || op == 0x0036 || (op == 0x0049 && index == 0xFFFF)) {           /* global commands */
        if (index != 0xFFFF) { status_only(s, op, (int)index, S_INVALID_INDEX); return; }
        switch (op) {
        case 0x0001: out[0] = 1; bt_put16(out + 1, 22); complete(s, op, -1, S_OK, out, 3); return;
        case 0x0002: {
            unsigned nc = sizeof(COMMANDS) / 2, ne = sizeof(EVENTS) / 2; bt_put16(out, nc); bt_put16(out + 2, ne);
            for (unsigned i = 0; i < nc; i++) bt_put16(out + 4 + 2 * i, COMMANDS[i]);
            for (unsigned i = 0; i < ne; i++) bt_put16(out + 4 + 2 * nc + 2 * i, EVENTS[i]);
            complete(s, op, -1, S_OK, out, 4 + 2 * (nc + ne)); return;
        }
        case 0x0003: case 0x003C: {
            unsigned n = 0;
            for (unsigned i = 0; i < BT_MAX_DEVICES; i++) {
                struct bt_dev* d = bt_dev_by_index((int)i); if (!d || d->user || !d->setup_done) continue;
                if (op == 0x0003) bt_put16(out + 2 + 2 * n, i); else { bt_put16(out + 2 + 4 * n, i); out[4 + 4 * n] = 0; out[5 + 4 * n] = (uint8_t)(d->driver ? d->driver->bus : 0); }
                n++;
            }
            bt_put16(out, n); complete(s, op, -1, S_OK, out, 2 + (op == 0x0003 ? 2 : 4) * n); return;
        }
        case 0x0036: bt_put16(out, 0); complete(s, op, -1, S_OK, out, 2); return;
        case 0x0049: bt_put16(out, 0); complete(s, op, -1, S_OK, out, 2); return;
        }
    }
    bool known = false; for (unsigned i = 0; i < sizeof(COMMANDS) / 2; i++) known |= COMMANDS[i] == op;
    if (!known) { status_only(s, op, (int)index, S_UNKNOWN); return; }   /* before the index check, as Linux */
    struct bt_dev* d = index == 0xFFFF ? NULL : bt_dev_by_index((int)index);
    if (!d || d->user || !d->setup_done) { status_only(s, op, (int)index, S_INVALID_INDEX); return; }
    if (op >= 0x0005 && op != 0x0042 && op != 0x0004 && op != 0x0049 && op != 0x0048 && op != 0x004B && op != 0x0040 && op != 0x003D && op != 0x004F && op != 0x0015 &&
        !trusted_writer) { status_only(s, op, (int)index, S_PERMISSION); return; }
    int i0 = d->index; uint32_t gen = d->generation;
    if (!(d = lock_dev(i0, gen))) { status_only(s, op, (int)index, S_INVALID_INDEX); return; }
    switch (op) {
    case 0x0004: complete(s, op, d->index, S_OK, out, info_reply(d, out, false)); break;
    case 0x0042: complete(s, op, d->index, S_OK, out, info_reply(d, out, true)); break;
    case 0x0005: if (q[0] > 1) { status_only(s, op, d->index, S_INVALID); break; } set_powered(s, op, d, q[0]); break;
    case 0x0006: {                                                         /* SET_DISCOVERABLE {val, timeout} */
        unsigned v = q[0], t = bt_get16(q + 1);
        if (v > 2 || (v == 0 && t) || (v == 2 && !t)) { status_only(s, op, d->index, S_INVALID); break; }
        if (!(d->settings & (ST_BREDR | ST_LE))) { status_only(s, op, d->index, S_REJECTED); break; }
        if (v && !(d->settings & ST_CONNECTABLE)) { status_only(s, op, d->index, S_REJECTED); break; }
        if (t && !powered(d)) { status_only(s, op, d->index, S_NOT_POWERED); break; }
        d->discov_limited = v == 2; d->discov_deadline = t ? bt_now() + (uint64_t)t * 1000000000ULL : 0; d->discov_timeout = (uint16_t)t;
        settings_command(s, op, d, ST_DISCOVERABLE, v != 0); update_scan(d); update_class_eir(d); if (d->le_adv_on) update_adv(d); break;
    }
    case 0x0007:                                                           /* SET_CONNECTABLE */
        if (q[0] > 1) { status_only(s, op, d->index, S_INVALID); break; }
        if (!q[0]) d->settings &= ~ST_DISCOVERABLE;
        settings_command(s, op, d, ST_CONNECTABLE, q[0]); update_scan(d); if (d->le_adv_on) update_adv(d); break;
    case 0x0008: if (q[0] > 1 || !(d->settings & ST_BREDR)) { status_only(s, op, d->index, q[0] > 1 ? S_INVALID : S_REJECTED); break; } settings_command(s, op, d, ST_FAST_CONN, q[0]); break;
    case 0x0009: if (q[0] > 1) { status_only(s, op, d->index, S_INVALID); break; } settings_command(s, op, d, ST_BONDABLE, q[0]); break;
    case 0x000A:
        if (q[0] > 1) { status_only(s, op, d->index, S_INVALID); break; }
        if (powered(d) && (d->settings & ST_BREDR)) { uint8_t v = q[0]; if (sync(&d, 0x0C20, &v, 1) == E_NODEV) goto gone; }
        settings_command(s, op, d, ST_LINK_SEC, q[0]); break;
    case 0x000B:
        if (q[0] > 1) { status_only(s, op, d->index, S_INVALID); break; }
        if (!(d->supported & ST_SSP)) { status_only(s, op, d->index, S_NOT_SUPPORTED); break; }
        if (powered(d) && q[0] && !(d->settings & ST_SSP)) { uint8_t v = 1; if (sync(&d, 0x0C56, &v, 1) == E_NODEV) goto gone; }
        if (!q[0]) d->settings &= ~ST_SC;
        settings_command(s, op, d, ST_SSP, q[0]); break;
    case 0x000D:
        if (q[0] > 1) { status_only(s, op, d->index, S_INVALID); break; }
        if (!(d->supported & ST_LE)) { status_only(s, op, d->index, S_NOT_SUPPORTED); break; }
        if (!q[0] && !(d->settings & ST_BREDR)) { status_only(s, op, d->index, S_REJECTED); break; }
        if (powered(d) && (d->settings & ST_BREDR)) { uint8_t v[2] = {q[0], 0}; if (sync(&d, 0x0C6D, v, 2) == E_NODEV) goto gone; }
        if (!q[0]) d->settings &= ~ST_ADVERTISING;
        settings_command(s, op, d, ST_LE, q[0]); update_adv(d); break;
    case 0x002A:
        if (q[0] > 1) { status_only(s, op, d->index, S_INVALID); break; }
        if (!(d->supported & ST_BREDR) || !(d->settings & ST_LE)) { status_only(s, op, d->index, S_NOT_SUPPORTED); break; }
        if (powered(d) && !q[0]) { status_only(s, op, d->index, S_REJECTED); break; }
        if (!q[0]) d->settings &= ~(ST_SSP | ST_LINK_SEC | ST_FAST_CONN | ST_DISCOVERABLE | ST_CONNECTABLE);
        settings_command(s, op, d, ST_BREDR, q[0]); break;
    case 0x002D:
        if (q[0] > 2) { status_only(s, op, d->index, S_INVALID); break; }
        if (!(d->supported & ST_SC)) { status_only(s, op, d->index, S_NOT_SUPPORTED); break; }
        if (powered(d) && (d->settings & ST_BREDR) && (d->settings & ST_SSP) && (d->features[2][1] & 1)) { uint8_t v = q[0] ? 1 : 0; if (sync(&d, 0x0C7A, &v, 1) == E_NODEV) goto gone; }
        d->sc_only = q[0] == 2; settings_command(s, op, d, ST_SC, q[0] != 0); break;
    case 0x002E: if (q[0] > 2) { status_only(s, op, d->index, S_INVALID); break; } settings_command(s, op, d, ST_DEBUG_KEYS, q[0] != 0); break;
    case 0x0029:                                                           /* SET_ADVERTISING {0, 1, 2 = connectable} */
        if (q[0] > 2) { status_only(s, op, d->index, S_INVALID); break; }
        if (!(d->settings & ST_LE)) { status_only(s, op, d->index, S_REJECTED); break; }
        d->adv_connectable_forced = q[0] == 2; settings_command(s, op, d, ST_ADVERTISING, q[0] != 0); update_adv(d); break;
    case 0x000E:                                                           /* SET_DEV_CLASS {major, minor} */
        if ((q[1] & 0x03) || (q[0] & 0xE0)) { status_only(s, op, d->index, S_INVALID); break; }
        d->major = q[0]; d->minor = q[1]; add_uuid_reply(s, op, d); break;
    case 0x000F:                                                           /* SET_LOCAL_NAME {name[249], short[11]} */
        memcpy(d->name, q, 248); d->name[248] = 0; memcpy(d->short_name, q + 249, 10); d->short_name[10] = 0;
        complete(s, op, d->index, S_OK, q, 260); mgmt_event(0x0008, d->index, q, 260, s); update_name(d); break;
    case 0x0010: {                                                         /* ADD_UUID */
        struct bt_uuid* u = NULL;
        for (unsigned i = 0; i < 24 && !u; i++) if (!d->uuids[i].used) u = &d->uuids[i];
        if (!u) { status_only(s, op, d->index, S_NO_RESOURCES); break; }
        memcpy(u->uuid, q, 16); u->svc_hint = q[16]; u->used = true; add_uuid_reply(s, op, d); break;
    }
    case 0x0011: {                                                         /* REMOVE_UUID (all zero: every UUID) */
        static const uint8_t zero[16] = {0}; bool all = !memcmp(q, zero, 16), found = false;
        for (unsigned i = 0; i < 24; i++) if (d->uuids[i].used && (all || !memcmp(d->uuids[i].uuid, q, 16))) { d->uuids[i].used = false; found = true; }
        if (!found && !all) { status_only(s, op, d->index, S_INVALID); break; }
        add_uuid_reply(s, op, d); break;
    }
    case 0x0012: {                                                         /* LOAD_LINK_KEYS {debug, count, keys[25]} */
        if (size < 3 || q[0] > 1 || size != 3U + 25U * bt_get16(q + 1) || bt_get16(q + 1) > 32) { status_only(s, op, d->index, S_INVALID); break; }
        for (unsigned i = 0; i < 32; i++) if (d->keys[i].used && !d->keys[i].type) memset(&d->keys[i], 0, sizeof(d->keys[i]));
        bool bad = false;
        for (unsigned i = 0; i < bt_get16(q + 1); i++) { const uint8_t* k = q + 3 + 25 * i; if (k[6] != 0 || k[7] > 0x08) bad = true; }
        if (bad) { status_only(s, op, d->index, S_INVALID); break; }
        for (unsigned i = 0; i < bt_get16(q + 1); i++) {
            const uint8_t* k = q + 3 + 25 * i; struct bt_key* key = key_slot(d);
            memset(key, 0, sizeof(*key)); key->used = true; memcpy(key->addr, k, 6); key->key_type = k[7]; memcpy(key->val, k + 8, 16); key->pin_len = k[24];
        }
        if (q[0]) d->settings |= ST_DEBUG_KEYS; else d->settings &= ~ST_DEBUG_KEYS;
        complete(s, op, d->index, S_OK, NULL, 0); break;
    }
    case 0x0013: {                                                         /* LOAD_LONG_TERM_KEYS {count, keys[36]} */
        if (size < 2 || size != 2U + 36U * bt_get16(q) || bt_get16(q) > 32) { status_only(s, op, d->index, S_INVALID); break; }
        for (unsigned i = 0; i < bt_get16(q); i++) { const uint8_t* k = q + 2 + 36 * i; if (k[6] < 1 || k[6] > 2 || k[7] > 4 || k[8] > 1) { status_only(s, op, d->index, S_INVALID); goto unlock; } }
        for (unsigned i = 0; i < 32; i++) if (d->keys[i].used && d->keys[i].type) memset(&d->keys[i], 0, sizeof(d->keys[i]));
        for (unsigned i = 0; i < bt_get16(q); i++) {
            const uint8_t* k = q + 2 + 36 * i; struct bt_key* key = key_slot(d);
            memset(key, 0, sizeof(*key)); key->used = true; memcpy(key->addr, k, 6); key->type = k[6]; key->key_type = k[7]; key->central = k[8]; key->enc_size = k[9];
            key->ediv = (uint16_t)bt_get16(k + 10); memcpy(key->rand, k + 12, 8); memcpy(key->val, k + 20, 16); key->authenticated = k[7] & 1;
        }
        complete(s, op, d->index, S_OK, NULL, 0); break;
    }
    case 0x0030: {                                                         /* LOAD_IRKS {count, irks[23]} */
        if (size < 2 || size != 2U + 23U * bt_get16(q) || bt_get16(q) > 16) { status_only(s, op, d->index, S_INVALID); break; }
        memset(d->irks, 0, sizeof(d->irks)); d->rpa_resolving = true;
        for (unsigned i = 0; i < bt_get16(q); i++) { const uint8_t* k = q + 2 + 23 * i; d->irks[i].used = true; memcpy(d->irks[i].addr, k, 6); d->irks[i].type = k[6]; memcpy(d->irks[i].val, k + 7, 16); }
        complete(s, op, d->index, S_OK, NULL, 0); break;
    }
    case 0x0035: if (size < 2 || size != 2U + 15U * bt_get16(q)) { status_only(s, op, d->index, S_INVALID); break; } complete(s, op, d->index, S_OK, NULL, 0); break;
    case 0x0046: if (size < 2 || size != 2U + 17U * bt_get16(q)) { status_only(s, op, d->index, S_INVALID); break; } complete(s, op, d->index, S_OK, NULL, 0); break;
    case 0x0014: {                                                         /* DISCONNECT */
        if (!addr_valid(q)) { complete(s, op, d->index, S_INVALID, q, 7); break; }
        if (!powered(d)) { complete(s, op, d->index, S_NOT_POWERED, q, 7); break; }
        struct bt_conn* c = conn_of(d, q);
        if (!c || c->state != CONN_CONNECTED) { complete(s, op, d->index, S_NOT_CONNECTED, q, 7); break; }
        if (pending_find(op, d->index, q)) { complete(s, op, d->index, S_BUSY, q, 7); break; }
        if (!pending_new(s, d, op, q, c->id, 0)) { complete(s, op, d->index, S_NO_RESOURCES, q, 7); break; }
        bt_disconnect(c, 0x13); break;
    }
    case 0x0015: {                                                         /* GET_CONNECTIONS */
        unsigned n = 0;
        for (struct bt_conn* c = d->conns; c && n < 64; c = c->next) if (c->state == CONN_CONNECTED && c->mgmt_connected) { addr_info(out + 2 + 7 * n, c); n++; }
        bt_put16(out, n); complete(s, op, d->index, S_OK, out, 2 + 7 * n); break;
    }
    case 0x0016: case 0x0017: case 0x001C: case 0x001D: case 0x001E: case 0x001F: if (!addr_valid(q)) { complete(s, op, d->index, S_INVALID, q, 7); break; } user_reply(s, op, d, q, size); break;
    case 0x0018: if (q[0] > 4) { status_only(s, op, d->index, S_INVALID); break; } d->io_cap = q[0]; complete(s, op, d->index, S_OK, NULL, 0); break;
    case 0x0019: if (!addr_valid(q)) { complete(s, op, d->index, S_INVALID, q, 7); break; } pair_device(s, d, q); break;
    case 0x001A: {                                                         /* CANCEL_PAIR_DEVICE */
        struct bt_conn* c = conn_of(d, q);
        if (!c || !c->pair_op) { complete(s, op, d->index, S_INVALID, q, 7); break; }
        mgmt_pair_done(c, S_CANCELLED); complete(s, op, d->index, S_OK, q, 7);
        if (!l2cap_conn_in_use(c)) bt_disconnect(c, 0x13);
        break;
    }
    case 0x001B: {                                                         /* UNPAIR_DEVICE {addr, disconnect} */
        if (!addr_valid(q) || q[7] > 1) { complete(s, op, d->index, S_INVALID, q, 7); break; }
        if (!powered(d)) { complete(s, op, d->index, S_NOT_POWERED, q, 7); break; }
        if (!remove_keys(d, q, q[6])) { complete(s, op, d->index, S_NOT_PAIRED, q, 7); break; }
        struct bt_conn* c = conn_of(d, q);
        if (c && q[7]) bt_disconnect(c, 0x13);
        complete(s, op, d->index, S_OK, q, 7); mgmt_event(0x0016, d->index, q, 7, s); break;
    }
    case 0x0023: case 0x0041: case 0x003A: {                               /* START_(LIMITED_|SERVICE_)DISCOVERY */
        if (op == 0x003A) {
            if (size < 4 || size != 4U + 16U * bt_get16(q + 2) || bt_get16(q + 2) > 16) { status_only(s, op, d->index, S_INVALID); break; }
        }
        unsigned r = start_discovery(d, q[0], op == 0x0041, s);
        if (!(d = bt_dev_get(i0, gen))) goto gone;
        if (!r && op == 0x003A) { d->filter_rssi = (int8_t)q[1]; d->filter_uuids = (uint8_t)bt_get16(q + 2); memcpy(d->filter_uuid, q + 4, 16U * d->filter_uuids); }
        complete(s, op, d->index, r, q, 1); break;
    }
    case 0x0024:                                                           /* STOP_DISCOVERY */
        if (!d->discovering) { complete(s, op, d->index, S_REJECTED, q, 1); break; }
        if (q[0] != d->discovery_type) { complete(s, op, d->index, S_INVALID, q, 1); break; }
        discovery_stop(d, false); complete(s, op, d->index, S_OK, q, 1);
        { uint8_t ev[2] = {q[0], 0}; mgmt_event(0x0013, d->index, ev, 2, NULL); }
        break;
    case 0x0025: {                                                         /* CONFIRM_NAME {addr, name_known} */
        if (!d->discovering) { complete(s, op, d->index, S_FAILED, q, 7); break; }
        struct bt_found* f = NULL; for (unsigned i = 0; i < d->found_count; i++) if (!memcmp(d->found[i].addr, q, 6) && d->found[i].type == q[6]) f = &d->found[i];
        if (!f) { complete(s, op, d->index, S_INVALID, q, 7); break; }
        f->name_needed = !q[7]; complete(s, op, d->index, S_OK, q, 7); break;
    }
    case 0x0026: case 0x0027: {                                            /* BLOCK / UNBLOCK_DEVICE */
        if (!addr_valid(q)) { complete(s, op, d->index, S_INVALID, q, 7); break; }
        int at = -1; for (unsigned i = 0; i < d->blocked_count; i++) if (!memcmp(d->blocked[i], q, 7)) at = (int)i;
        if (op == 0x0026) { if (at < 0) { if (d->blocked_count == 16) { complete(s, op, d->index, S_NO_RESOURCES, q, 7); break; } memcpy(d->blocked[d->blocked_count++], q, 7); } }
        else { if (at < 0) { complete(s, op, d->index, S_INVALID, q, 7); break; } bt_move(d->blocked[at], d->blocked[at + 1], 7U * (d->blocked_count - (unsigned)at - 1)); d->blocked_count--; }
        complete(s, op, d->index, S_OK, q, 7); mgmt_event(op == 0x0026 ? 0x0014 : 0x0015, d->index, q, 7, s); break;
    }
    case 0x0028: d->did_source = (uint16_t)bt_get16(q); d->did_vendor = (uint16_t)bt_get16(q + 2); d->did_product = (uint16_t)bt_get16(q + 4); d->did_version = (uint16_t)bt_get16(q + 6);
        if (d->did_source > 2) { d->did_source = 0; status_only(s, op, d->index, S_INVALID); break; }
        complete(s, op, d->index, S_OK, NULL, 0); update_class_eir(d); break;
    case 0x002C: { unsigned i = bt_get16(q), w = bt_get16(q + 2); if (i < 4 || i > 0x4000 || w < 4 || w > 0x4000 || w > i) { status_only(s, op, d->index, S_INVALID); break; }
                   d->scan_interval = (uint16_t)i; d->scan_window = (uint16_t)w; complete(s, op, d->index, S_OK, NULL, 0); break; }
    case 0x0031: {                                                         /* GET_CONN_INFO */
        struct bt_conn* c = conn_of(d, q); memcpy(out, q, 7);
        if (!c || c->state != CONN_CONNECTED) { complete(s, op, d->index, S_NOT_CONNECTED, out, 10); break; }
        out[7] = 127; out[8] = 127; out[9] = 127; complete(s, op, d->index, S_OK, out, 10); break;
    }
    case 0x0033: {                                                         /* ADD_DEVICE {addr, action} */
        if (!addr_valid(q) || q[7] > 2 || (q[6] == 0 && q[7] != 1)) { complete(s, op, d->index, S_INVALID, q, 7); break; }
        bool have = false; for (unsigned i = 0; i < d->accept_count; i++) have |= !memcmp(d->accept_list[i], q, 7);
        if (!have && d->accept_count < 16) memcpy(d->accept_list[d->accept_count++], q, 7);
        complete(s, op, d->index, S_OK, q, 7); mgmt_event(0x001A, d->index, q, 8, s); update_scan(d); break;
    }
    case 0x0034: {                                                         /* REMOVE_DEVICE (BDADDR_ANY: all) */
        static const uint8_t any[6] = {0}; bool all = !memcmp(q, any, 6), found = false;
        if (all && q[6]) { complete(s, op, d->index, S_INVALID, q, 7); break; }
        for (unsigned i = 0; i < d->accept_count;) {
            if (all || !memcmp(d->accept_list[i], q, 7)) { mgmt_event(0x001B, d->index, d->accept_list[i], 7, s); bt_move(d->accept_list[i], d->accept_list[i + 1], 7U * (d->accept_count - i - 1)); d->accept_count--; found = true; }
            else i++;
        }
        if (!found && !all) { complete(s, op, d->index, S_INVALID, q, 7); break; }
        complete(s, op, d->index, S_OK, q, 7); update_scan(d); break;
    }
    case 0x003D:                                                           /* READ_ADV_FEATURES */
        bt_put32(out, 0x0000007F); out[4] = 31; out[5] = 31; out[6] = 1; out[7] = d->adv_instance ? 1 : 0; out[8] = 1;
        complete(s, op, d->index, S_OK, out, 8 + (d->adv_instance ? 1 : 0)); break;
    case 0x003E: {                                                         /* ADD_ADVERTISING {instance, flags, duration, timeout, adv_len, rsp_len, data} */
        if (size < 11 || size != 11U + q[9] + q[10] || q[0] != 1 || q[9] > 31 || q[10] > 31) { status_only(s, op, d->index, q[0] != 1 && size >= 11 ? S_INVALID : S_INVALID); break; }
        if (!(d->settings & ST_LE)) { status_only(s, op, d->index, S_REJECTED); break; }
        bool added = !d->adv_instance;
        d->adv_instance = 1; d->inst_flags = bt_get32(q + 1); d->inst_adv_len = q[9]; d->inst_rsp_len = q[10];
        memcpy(d->inst_adv, q + 11, q[9]); memcpy(d->inst_rsp, q + 11 + q[9], q[10]);
        out[0] = 1; complete(s, op, d->index, S_OK, out, 1); if (added) mgmt_event(0x0023, d->index, out, 1, s); update_adv(d); break;
    }
    case 0x003F:                                                           /* REMOVE_ADVERTISING {instance 0 = all} */
        if ((q[0] && q[0] != 1) || (q[0] == 1 && !d->adv_instance)) { status_only(s, op, d->index, S_INVALID); break; }
        if (d->adv_instance) { out[0] = 1; mgmt_event(0x0024, d->index, out, 1, s); }
        d->adv_instance = 0; out[0] = q[0]; complete(s, op, d->index, S_OK, out, 1); update_adv(d); break;
    case 0x0040: out[0] = q[0]; bt_put32(out + 1, bt_get32(q + 1)); out[5] = (bt_get32(q + 1) & 0x0E) ? 28 : 31; out[6] = (bt_get32(q + 1) & 0x40) ? 31 - 2 - (uint8_t)(strlen(d->name) < 29 ? strlen(d->name) : 29) : 31;
        if (q[0] != 1) { status_only(s, op, d->index, S_INVALID); break; }
        complete(s, op, d->index, S_OK, out, 7); break;
    case 0x0043: d->appearance = (uint16_t)bt_get16(q); complete(s, op, d->index, S_OK, NULL, 0); if (d->le_adv_on) update_adv(d); break;
    case 0x0048: bt_put16(out, 0); complete(s, op, d->index, S_OK, out, 2); break;
    case 0x0049: bt_put16(out, 0); complete(s, op, d->index, S_OK, out, 2); break;
    case 0x004B: complete(s, op, d->index, S_OK, NULL, 0); break;
    case 0x004F: memcpy(out, q, 7); bt_put32(out + 7, 0); bt_put32(out + 11, 0); complete(s, op, d->index, S_OK, out, 15); break;
    case 0x0050: if (bt_get32(q + 7)) { complete(s, op, d->index, S_INVALID, q, 7); break; } complete(s, op, d->index, S_OK, q, 7); break;
    case 0x0053:                                                           /* REMOVE_ADV_MONITOR: no monitors exist (handle 0 = all) */
        if (size != 2 || bt_get16(q)) { status_only(s, op, d->index, S_INVALID); break; }
        complete(s, op, d->index, S_OK, q, 2); break;
    default: status_only(s, op, d->index, S_UNKNOWN);
    }
unlock:
    unlock_dev(i0); return;
gone:
    status_only(s, op, (int)index, S_INVALID_INDEX); unlock_dev(i0);
}
void mgmt_socket_command(void* socket, const uint8_t* message, unsigned length, bool trusted) {
    trusted_writer = trusted;
    unsigned opcode = bt_get16(message), index = bt_get16(message + 2), size = bt_get16(message + 4);
    mgmt_monitor_command((int)(int16_t)index, message, length, false, mgmt_socket_cookie(socket));
    if (size != length - 6) { status_only(socket, opcode, (int)index, S_INVALID); return; }
    command(socket, opcode, index, message + 6, size);
}

/* ---------- index lifecycle and timers ---------- */
void mgmt_index_added(struct bt_dev* d) {
    mgmt_event(0x0004, d->index, NULL, 0, NULL);
    uint8_t ext[2] = {0, (uint8_t)(d->driver ? d->driver->bus : 0)}; mgmt_event(0x0020, d->index, ext, 2, NULL);
}
void mgmt_index_removed(struct bt_dev* d) {
    for (unsigned i = 0; i < 32; i++) if (pendings[i].used && pendings[i].index == d->index && pendings[i].gen == d->generation) pending_done(&pendings[i], S_INVALID_INDEX);
    mgmt_event(0x0005, d->index, NULL, 0, NULL);
    uint8_t ext[2] = {0, (uint8_t)(d->driver ? d->driver->bus : 0)}; mgmt_event(0x0021, d->index, ext, 2, NULL);
}
void mgmt_poll(struct bt_dev* d, uint64_t now) {
    if (d->discov_deadline && now >= d->discov_deadline) {
        d->discov_deadline = 0; d->discov_timeout = 0; d->settings &= ~ST_DISCOVERABLE; update_scan(d); update_class_eir(d); if (d->le_adv_on) update_adv(d); new_settings(d, NULL);
    }
    if (d->discovering && d->discovery_deadline && now >= d->discovery_deadline) {
        d->discovery_deadline = 0;
        if (d->le_scan_active) { uint8_t off[2] = {0, 0}; bt_cmd_async(d, 0x200C, off, 2, NULL, NULL, 0); d->le_scan_active = false; }
        if (d->inquiry_active) { bt_cmd_async(d, 0x0402, NULL, 0, NULL, NULL, 0); d->inquiry_active = false; }
        if (d->name_phase) discovery_stop(d, true); else discovery_phase_done(d);
    }
    for (unsigned i = 0; i < 32; i++) {
        struct pending* p = &pendings[i];
        if (!p->used || !p->deadline || now < p->deadline || p->index != d->index) continue;
        struct bt_conn* c = bt_conn_get(p->conn);
        if (c && c->pair_op == p) { mgmt_pair_done(c, S_TIMEOUT); bt_disconnect(c, 0x13); } else pending_done(p, S_TIMEOUT);
    }
}
uint64_t mgmt_next_event(struct bt_dev* d, uint64_t now) {
    (void)now; uint64_t next = ~0ULL;
    if (d->discov_deadline) next = d->discov_deadline;
    if (d->discovering && d->discovery_deadline && d->discovery_deadline < next) next = d->discovery_deadline;
    for (unsigned i = 0; i < 32; i++) if (pendings[i].used && pendings[i].deadline && pendings[i].index == d->index && pendings[i].deadline < next) next = pendings[i].deadline;
    return next;
}
