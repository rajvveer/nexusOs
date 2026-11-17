/* nl80211: the generic netlink family "nl80211" (Linux include/uapi/linux/nl80211.h) over the softmac of wifi.c,
 * enough for Debian's wpa_supplicant and hostapd (driver nl80211, user-space SME/MLME, mac80211 semantics) and iw:
 *   - wiphy/interface/station/scan/regulatory queries (split wiphy dump as one message), protocol features
 *   - station mode: TRIGGER_SCAN/ABORT_SCAN/GET_SCAN, AUTHENTICATE (open, SAE with user-space AUTH_DATA),
 *     ASSOCIATE (reassociation via PREV_BSSID), DEAUTHENTICATE/DISASSOCIATE/DISCONNECT, CQM RSSI thresholds
 *   - AP mode: SET_INTERFACE type AP, START_AP/SET_BEACON/STOP_AP, NEW/SET/DEL/GET_STATION, PROBE_CLIENT,
 *     UNEXPECTED_FRAME registration
 *   - both: REGISTER_FRAME + FRAME (+ FRAME_TX_STATUS), NEW/SET/DEL/GET_KEY (CCMP-128, BIP-CMAC-128)
 * Events go to the multicast groups "config", "scan", "regulatory", "mlme" (and "vendor", "nan", "testmode", never
 * used); received management frames go to the registering socket. Changing anything needs CAP_NET_ADMIN (or the
 * desktop owner, netlink_admin()); queries are open, as in Linux. Not supported (EOPNOTSUPP): CONNECT (fullmac),
 * monitor/mesh/IBSS/P2P interfaces, NEW/DEL_INTERFACE, remain-on-channel, scheduled scans, PMKSA caching in the driver,
 * control port over nl80211, TKIP/GCMP/WEP. The family is resolved through nlctrl in wireguard.c (genl_request). */
#include "wifi.h"
#include "nlmsg.h"
#include "nl80211_defs.h"
#include "../../kernel/string.h"

#define NL80211_GENL_ID 0x23
#define GENL_ID_CTRL 0x10
#define CTRL_ATTR_FAMILY_ID 1
#define CTRL_ATTR_FAMILY_NAME 2
#define CTRL_ATTR_VERSION 3
#define CTRL_ATTR_HDRSIZE 4
#define CTRL_ATTR_MAXATTR 5
#define CTRL_ATTR_OPS 6
#define CTRL_ATTR_MCAST_GROUPS 7
enum { GRP_CONFIG = 2, GRP_SCAN, GRP_REGULATORY, GRP_MLME, GRP_VENDOR, GRP_NAN, GRP_TESTMODE };
static const char* const group_names[] = {"config", "scan", "regulatory", "mlme", "vendor", "nan", "testmode"};
static const uint8_t supported_commands[] = {
    NL80211_CMD_GET_WIPHY, NL80211_CMD_SET_WIPHY, NL80211_CMD_GET_INTERFACE, NL80211_CMD_SET_INTERFACE,
    NL80211_CMD_NEW_INTERFACE, NL80211_CMD_DEL_INTERFACE, NL80211_CMD_GET_KEY,
    NL80211_CMD_SET_KEY, NL80211_CMD_NEW_KEY, NL80211_CMD_DEL_KEY, NL80211_CMD_SET_BEACON, NL80211_CMD_START_AP, NL80211_CMD_STOP_AP,
    NL80211_CMD_GET_STATION, NL80211_CMD_SET_STATION, NL80211_CMD_NEW_STATION, NL80211_CMD_DEL_STATION, NL80211_CMD_SET_BSS,
    NL80211_CMD_GET_REG, NL80211_CMD_REQ_SET_REG, NL80211_CMD_TRIGGER_SCAN, NL80211_CMD_GET_SCAN, NL80211_CMD_ABORT_SCAN,
    NL80211_CMD_AUTHENTICATE, NL80211_CMD_ASSOCIATE, NL80211_CMD_DEAUTHENTICATE, NL80211_CMD_DISASSOCIATE, NL80211_CMD_DISCONNECT,
    NL80211_CMD_REGISTER_FRAME, NL80211_CMD_FRAME, NL80211_CMD_FRAME_WAIT_CANCEL, NL80211_CMD_SET_POWER_SAVE, NL80211_CMD_GET_POWER_SAVE,
    NL80211_CMD_SET_CQM, NL80211_CMD_SET_CHANNEL, NL80211_CMD_UNEXPECTED_FRAME, NL80211_CMD_PROBE_CLIENT, NL80211_CMD_GET_PROTOCOL_FEATURES,
    NL80211_CMD_REGISTER_BEACONS,
};
static struct nl_attr_view at[NL80211_ATTR_MAX + 1];
static struct nl_builder *reply_b, *event_b;
static struct netlink_socket* unexpected_nl[WIFI_RADIO_MAX], *beacon_nl[WIFI_RADIO_MAX];
static uint32_t generation = 1;
static char reg_alpha2[3] = "00";

static struct nl_builder* builder(struct nl_builder** slot) {
    if (!*slot) *slot = (void*)arena_alloc(sizeof(struct nl_builder));   /* kept: one for replies, one for events */
    return *slot;                                                         /* (nl_begin resets it) */
}
static void genl_hdr(struct nl_builder* b, uint16_t family, uint8_t cmd, uint8_t version, uint16_t flags, uint32_t seq, uint32_t portid) {
    nl_begin(b, family, flags, seq, portid);
    uint8_t g[4] = {cmd, version, 0, 0}; nl_put(b, g, 4);
}
static void nest(struct nl_builder* b, uint16_t type) {   /* nla_nest_start_noflag() */
    nl_nest_begin(b, type);
    if (!b->overflow && b->depth) { uint16_t plain = type; memcpy(b->buffer + b->nest[b->depth - 1] + 2, &plain, 2); }
}
static uint64_t u64_of(const struct nl_attr_view* a) { uint64_t v = 0; if (a->present && a->length >= 8) memcpy(&v, a->data, 8); return v; }
static bool mac_of(const struct nl_attr_view* a, uint8_t* out) { if (!a->present || a->length != 6) return false; memcpy(out, a->data, 6); return true; }

/* ---- nlctrl: the family directory entry ---- */
int nl80211_family_reply(struct netlink_socket* nl, const struct nlmsghdr* rq) {
    struct nl_builder* b = builder(&reply_b); if (!b) return -12;
    genl_hdr(b, GENL_ID_CTRL, 1 /* CTRL_CMD_NEWFAMILY */, 2, 0, rq->seq, nl_portid(nl));
    nl_attr_string(b, CTRL_ATTR_FAMILY_NAME, "nl80211"); nl_attr16(b, CTRL_ATTR_FAMILY_ID, NL80211_GENL_ID);
    nl_attr32(b, CTRL_ATTR_VERSION, 1); nl_attr32(b, CTRL_ATTR_HDRSIZE, 0); nl_attr32(b, CTRL_ATTR_MAXATTR, NL80211_ATTR_MAX);
    nl_nest_begin(b, CTRL_ATTR_OPS);
    for (unsigned i = 0; i < sizeof(supported_commands); i++) {
        nl_nest_begin(b, (uint16_t)(i + 1)); nl_attr32(b, 1, supported_commands[i]); nl_attr32(b, 2, 0x1E); nl_nest_end(b);
    }
    nl_nest_end(b);
    nl_nest_begin(b, CTRL_ATTR_MCAST_GROUPS);
    for (unsigned i = 0; i < 7; i++) { nl_nest_begin(b, (uint16_t)(i + 1)); nl_attr_string(b, 1, group_names[i]); nl_attr32(b, 2, GRP_CONFIG + i); nl_nest_end(b); }
    nl_nest_end(b);
    nl_send(nl, b);
    return 0;
}
bool nl80211_family_id(uint16_t id) { return id == NL80211_GENL_ID; }

/* ---- object lookup ---- */
static struct wifi_vif* vif_from_attrs(void) {
    if (at[NL80211_ATTR_IFINDEX].present) return wifi_vif_by_dev(netdev_by_index(nl_u32(&at[NL80211_ATTR_IFINDEX])));
    if (at[NL80211_ATTR_WDEV].present) {
        uint64_t w = u64_of(&at[NL80211_ATTR_WDEV]);
        for (unsigned i = 0; i < WIFI_RADIO_MAX; i++) if (wifi_radios[i] && wifi_radios[i]->vif && wifi_radios[i]->vif->wdev == w) return wifi_radios[i]->vif;
    }
    return NULL;
}
static struct wifi_radio* radio_from_attrs(void) {
    if (at[NL80211_ATTR_WIPHY].present) { uint32_t i = nl_u32(&at[NL80211_ATTR_WIPHY]); return i < WIFI_RADIO_MAX ? wifi_radios[i] : NULL; }
    struct wifi_vif* v = vif_from_attrs(); return v ? v->radio : NULL;
}
static void ids(struct nl_builder* b, struct wifi_vif* v) {
    nl_attr32(b, NL80211_ATTR_WIPHY, v->radio->index); nl_attr32(b, NL80211_ATTR_IFINDEX, v->dev->index); nl_attr64(b, NL80211_ATTR_WDEV, v->wdev);
}

/* ---- wiphy ---- */
static void band_fill(struct nl_builder* b, unsigned band) {
    nest(b, (uint16_t)band);
    nest(b, NL80211_BAND_ATTR_FREQS);
    unsigned n = 0;
    for (unsigned i = 0; i < WIFI_CHANNELS; i++) {
        unsigned f = wifi_channel_freqs[i]; if ((f >= 5000) != (band == NL80211_BAND_5GHZ)) continue;
        nest(b, (uint16_t)n++); nl_attr32(b, NL80211_FREQUENCY_ATTR_FREQ, f); nl_attr32(b, NL80211_FREQUENCY_ATTR_MAX_TX_POWER, 2000); nl_nest_end(b);
    }
    nl_nest_end(b);
    nest(b, NL80211_BAND_ATTR_RATES);
    static const uint16_t rates24[] = {10, 20, 55, 110, 60, 90, 120, 180, 240, 360, 480, 540}, rates5[] = {60, 90, 120, 180, 240, 360, 480, 540};
    const uint16_t* rates = band == NL80211_BAND_5GHZ ? rates5 : rates24; unsigned count = band == NL80211_BAND_5GHZ ? 8 : 12;
    for (unsigned i = 0; i < count; i++) {
        nest(b, (uint16_t)i); nl_attr32(b, NL80211_BITRATE_ATTR_RATE, rates[i]);
        if (band == NL80211_BAND_2GHZ && i >= 1 && i <= 3) nl_attr(b, NL80211_BITRATE_ATTR_2GHZ_SHORTPREAMBLE, NULL, 0);
        nl_nest_end(b);
    }
    nl_nest_end(b);
    nl_nest_end(b);
}
static void frame_types(struct nl_builder* b, uint16_t attr, bool tx) {
    nest(b, attr);
    nest(b, NL80211_IFTYPE_STATION);
    static const uint16_t sta_tx[] = {0x00d0, 0x0040}, sta_rx[] = {0x00d0, 0x0040};
    static const uint16_t ap_tx[] = {0x0000, 0x0010, 0x0020, 0x0030, 0x0040, 0x0050, 0x0080, 0x00a0, 0x00b0, 0x00c0, 0x00d0}, ap_rx[] = {0x0000, 0x0020, 0x0040, 0x00a0, 0x00b0, 0x00c0, 0x00d0};
    for (unsigned i = 0; i < 2; i++) nl_attr16(b, NL80211_ATTR_FRAME_TYPE, tx ? sta_tx[i] : sta_rx[i]);
    nl_nest_end(b);
    nest(b, NL80211_IFTYPE_AP);
    if (tx) for (unsigned i = 0; i < sizeof(ap_tx) / 2; i++) nl_attr16(b, NL80211_ATTR_FRAME_TYPE, ap_tx[i]);
    else for (unsigned i = 0; i < sizeof(ap_rx) / 2; i++) nl_attr16(b, NL80211_ATTR_FRAME_TYPE, ap_rx[i]);
    nl_nest_end(b);
    nl_nest_end(b);
}
static void wiphy_fill(struct nl_builder* b, struct wifi_radio* r, uint16_t flags, uint32_t seq, uint32_t portid) {
    genl_hdr(b, NL80211_GENL_ID, NL80211_CMD_NEW_WIPHY, 1, flags, seq, portid);
    nl_attr32(b, NL80211_ATTR_WIPHY, r->index); nl_attr_string(b, NL80211_ATTR_WIPHY_NAME, r->name); nl_attr32(b, NL80211_ATTR_GENERATION, generation);
    nl_attr8(b, NL80211_ATTR_WIPHY_RETRY_SHORT, r->retry_short); nl_attr8(b, NL80211_ATTR_WIPHY_RETRY_LONG, r->retry_long);
    nl_attr32(b, NL80211_ATTR_WIPHY_FRAG_THRESHOLD, r->frag_threshold); nl_attr32(b, NL80211_ATTR_WIPHY_RTS_THRESHOLD, r->rts_threshold);
    nl_attr8(b, NL80211_ATTR_WIPHY_COVERAGE_CLASS, 0);
    nl_attr8(b, NL80211_ATTR_MAX_NUM_SCAN_SSIDS, 4); nl_attr16(b, NL80211_ATTR_MAX_SCAN_IE_LEN, 256);
    uint32_t ciphers[7] = {WIFI_CIPHER_CCMP, WIFI_CIPHER_CCMP_256, WIFI_CIPHER_GCMP, WIFI_CIPHER_GCMP_256,
                           WIFI_CIPHER_BIP_CMAC, WIFI_CIPHER_BIP_GMAC_128, WIFI_CIPHER_BIP_GMAC_256};
    nl_attr(b, NL80211_ATTR_CIPHER_SUITES, ciphers, sizeof(ciphers));
    nl_attr32(b, NL80211_ATTR_WIPHY_ANTENNA_AVAIL_TX, 1); nl_attr32(b, NL80211_ATTR_WIPHY_ANTENNA_AVAIL_RX, 1);
    nl_attr32(b, NL80211_ATTR_WIPHY_ANTENNA_TX, 1); nl_attr32(b, NL80211_ATTR_WIPHY_ANTENNA_RX, 1);
    nest(b, NL80211_ATTR_SUPPORTED_IFTYPES); nl_attr(b, NL80211_IFTYPE_STATION, NULL, 0); nl_attr(b, NL80211_IFTYPE_AP, NULL, 0); nl_attr(b, NL80211_IFTYPE_MONITOR, NULL, 0); nl_nest_end(b);
    nest(b, NL80211_ATTR_WIPHY_BANDS); band_fill(b, NL80211_BAND_2GHZ); band_fill(b, NL80211_BAND_5GHZ); nl_nest_end(b);
    nest(b, NL80211_ATTR_SUPPORTED_COMMANDS);
    for (unsigned i = 0; i < sizeof(supported_commands); i++) nl_attr32(b, (uint16_t)(i + 1), supported_commands[i]);
    nl_nest_end(b);
    frame_types(b, NL80211_ATTR_TX_FRAME_TYPES, true); frame_types(b, NL80211_ATTR_RX_FRAME_TYPES, false);
    nest(b, NL80211_ATTR_INTERFACE_COMBINATIONS);
    nest(b, 1);
    nest(b, NL80211_IFACE_COMB_LIMITS); nest(b, 1); nl_attr32(b, NL80211_IFACE_LIMIT_MAX, 1);
    nest(b, NL80211_IFACE_LIMIT_TYPES); nl_attr(b, NL80211_IFTYPE_STATION, NULL, 0); nl_attr(b, NL80211_IFTYPE_AP, NULL, 0); nl_nest_end(b);
    nl_nest_end(b); nl_nest_end(b);
    nl_attr32(b, NL80211_IFACE_COMB_MAXNUM, 1); nl_attr32(b, NL80211_IFACE_COMB_NUM_CHANNELS, 1);
    nl_nest_end(b); nl_nest_end(b);
    nl_attr32(b, NL80211_ATTR_FEATURE_FLAGS, NL80211_FEATURE_SK_TX_STATUS | NL80211_FEATURE_SAE);
    nl_attr32(b, NL80211_ATTR_MAX_AP_ASSOC_STA, WIFI_STA_MAX);
}
static int get_wiphy(struct netlink_socket* nl, const struct nlmsghdr* rq, bool dump) {
    struct nl_builder* b = builder(&reply_b); if (!b) return -12;
    struct wifi_radio* only = (at[NL80211_ATTR_WIPHY].present || at[NL80211_ATTR_IFINDEX].present || at[NL80211_ATTR_WDEV].present) ? radio_from_attrs() : NULL;
    if (!dump) { if (!only) return -19; wiphy_fill(b, only, 0, rq->seq, nl_portid(nl)); nl_send(nl, b); return 0; }
    if (!only && (at[NL80211_ATTR_WIPHY].present || at[NL80211_ATTR_IFINDEX].present || at[NL80211_ATTR_WDEV].present)) return -19;
    nl_dump_begin(nl);
    for (unsigned i = 0; i < WIFI_RADIO_MAX; i++) {
        if (!wifi_radios[i] || (only && wifi_radios[i] != only)) continue;
        builder(&reply_b); wiphy_fill(b, wifi_radios[i], NLM_F_MULTI, rq->seq, nl_portid(nl)); nl_send(nl, b);
    }
    nl_send_done(nl, rq); nl_dump_end(nl);
    return 1;
}

/* ---- interfaces ---- */
static void iface_fill(struct nl_builder* b, struct wifi_vif* v, uint8_t cmd, uint16_t flags, uint32_t seq, uint32_t portid) {
    genl_hdr(b, NL80211_GENL_ID, cmd, 1, flags, seq, portid);
    ids(b, v); nl_attr_string(b, NL80211_ATTR_IFNAME, v->dev->name); nl_attr32(b, NL80211_ATTR_IFTYPE, v->iftype);
    nl_attr(b, NL80211_ATTR_MAC, v->dev->addr, 6); nl_attr32(b, NL80211_ATTR_GENERATION, generation); nl_attr8(b, NL80211_ATTR_4ADDR, 0);
    bool operating = (v->iftype == NL80211_IFTYPE_AP && v->ap_started) || (v->iftype == NL80211_IFTYPE_STATION && v->sme == WIFI_ASSOCIATED);
    if (operating) {
        nl_attr32(b, NL80211_ATTR_WIPHY_FREQ, v->freq); nl_attr32(b, NL80211_ATTR_WIPHY_CHANNEL_TYPE, NL80211_CHAN_NO_HT);
        nl_attr32(b, NL80211_ATTR_CHANNEL_WIDTH, NL80211_CHAN_WIDTH_20_NOHT); nl_attr32(b, NL80211_ATTR_CENTER_FREQ1, v->freq);
        if (v->iftype == NL80211_IFTYPE_AP) nl_attr(b, NL80211_ATTR_SSID, v->ap_ssid, v->ap_ssid_len);
        else nl_attr(b, NL80211_ATTR_SSID, v->ssid, v->ssid_len);
    }
    nl_attr32(b, NL80211_ATTR_WIPHY_TX_POWER_LEVEL, 2000);
}
static int get_interface(struct netlink_socket* nl, const struct nlmsghdr* rq, bool dump) {
    struct nl_builder* b = builder(&reply_b); if (!b) return -12;
    if (!dump) { struct wifi_vif* v = vif_from_attrs(); if (!v) return -19; iface_fill(b, v, NL80211_CMD_NEW_INTERFACE, 0, rq->seq, nl_portid(nl)); nl_send(nl, b); return 0; }
    struct wifi_radio* only = at[NL80211_ATTR_WIPHY].present ? radio_from_attrs() : NULL;
    nl_dump_begin(nl);
    for (unsigned i = 0; i < WIFI_RADIO_MAX; i++) {
        struct wifi_radio* r = wifi_radios[i]; if (!r || (only && r != only)) continue;
        for (unsigned vi = 0; vi < 2; vi++) {
            struct wifi_vif* v = vi ? r->vif2 : r->vif; if (!v) continue;
            builder(&reply_b); iface_fill(b, v, NL80211_CMD_NEW_INTERFACE, NLM_F_MULTI, rq->seq, nl_portid(nl)); nl_send(nl, b);
        }
    }
    nl_send_done(nl, rq); nl_dump_end(nl);
    return 1;
}

/* ---- scan results ---- */
static int get_scan(struct netlink_socket* nl, const struct nlmsghdr* rq) {
    struct wifi_vif* v = vif_from_attrs(); if (!v) return -19;
    struct wifi_radio* r = v->radio; struct nl_builder* b = builder(&reply_b); if (!b) return -12;
    uint64_t now = native_clock_ns();
    nl_dump_begin(nl);
    for (unsigned i = 0; i < WIFI_BSS_MAX; i++) {
        struct wifi_bss* e = &r->bss[i]; if (!e->used) continue;
        bool ours = v->iftype == NL80211_IFTYPE_STATION && v->sme != WIFI_IDLE && !memcmp(e->bssid, v->bssid, 6);
        if (!ours && now > e->seen_ns + 30000000000ULL) { e->used = false; continue; }
        builder(&reply_b);
        genl_hdr(b, NL80211_GENL_ID, NL80211_CMD_NEW_SCAN_RESULTS, 1, NLM_F_MULTI, rq->seq, nl_portid(nl));
        nl_attr32(b, NL80211_ATTR_GENERATION, r->generation); nl_attr32(b, NL80211_ATTR_IFINDEX, v->dev->index); nl_attr64(b, NL80211_ATTR_WDEV, v->wdev);
        nest(b, NL80211_ATTR_BSS);
        nl_attr(b, NL80211_BSS_BSSID, e->bssid, 6); nl_attr32(b, NL80211_BSS_FREQUENCY, e->freq); nl_attr64(b, NL80211_BSS_TSF, e->tsf);
        nl_attr16(b, NL80211_BSS_BEACON_INTERVAL, e->beacon_int); nl_attr16(b, NL80211_BSS_CAPABILITY, e->capability);
        nl_attr(b, NL80211_BSS_INFORMATION_ELEMENTS, e->ies, e->ies_len);
        if (e->beaconed) nl_attr(b, NL80211_BSS_BEACON_IES, e->beacon_ies, e->beacon_ies_len);
        if (e->presp) nl_attr(b, NL80211_BSS_PRESP_DATA, NULL, 0);
        nl_attr32(b, NL80211_BSS_SIGNAL_MBM, (uint32_t)e->signal_mbm);
        nl_attr32(b, NL80211_BSS_SEEN_MS_AGO, now > e->seen_ns ? (uint32_t)((now - e->seen_ns) / 1000000ULL) : 0);
        nl_attr32(b, NL80211_BSS_CHAN_WIDTH, 0);
        if (ours && v->sme >= WIFI_AUTHENTICATED) nl_attr32(b, NL80211_BSS_STATUS, v->sme == WIFI_ASSOCIATED ? NL80211_BSS_STATUS_ASSOCIATED : NL80211_BSS_STATUS_AUTHENTICATED);
        nl_nest_end(b);
        nl_send(nl, b);
    }
    nl_send_done(nl, rq); nl_dump_end(nl);
    return 1;
}

/* ---- stations ---- */
static void station_fill(struct nl_builder* b, struct wifi_vif* v, struct wifi_station* s, uint16_t flags, uint32_t seq, uint32_t portid) {
    uint64_t now = native_clock_ns();
    genl_hdr(b, NL80211_GENL_ID, NL80211_CMD_NEW_STATION, 1, flags, seq, portid);
    nl_attr32(b, NL80211_ATTR_IFINDEX, v->dev->index); nl_attr(b, NL80211_ATTR_MAC, s->addr, 6); nl_attr32(b, NL80211_ATTR_GENERATION, generation);
    nest(b, NL80211_ATTR_STA_INFO);
    nl_attr32(b, NL80211_STA_INFO_INACTIVE_TIME, now > s->last_rx_ns ? (uint32_t)((now - s->last_rx_ns) / 1000000ULL) : 0);
    nl_attr32(b, NL80211_STA_INFO_RX_BYTES, (uint32_t)s->rx_bytes); nl_attr32(b, NL80211_STA_INFO_TX_BYTES, (uint32_t)s->tx_bytes);
    nl_attr64(b, NL80211_STA_INFO_RX_BYTES64, s->rx_bytes); nl_attr64(b, NL80211_STA_INFO_TX_BYTES64, s->tx_bytes);
    nl_attr32(b, NL80211_STA_INFO_RX_PACKETS, (uint32_t)s->rx_packets); nl_attr32(b, NL80211_STA_INFO_TX_PACKETS, (uint32_t)s->tx_packets);
    nl_attr32(b, NL80211_STA_INFO_TX_FAILED, (uint32_t)s->tx_failed);
    int signal = s->signal ? s->signal : -50;
    nl_attr8(b, NL80211_STA_INFO_SIGNAL, (uint8_t)(int8_t)signal); nl_attr8(b, NL80211_STA_INFO_SIGNAL_AVG, (uint8_t)(int8_t)signal);
    if (v->iftype == NL80211_IFTYPE_STATION) nl_attr8(b, NL80211_STA_INFO_BEACON_SIGNAL_AVG, (uint8_t)(int8_t)signal);
    nest(b, NL80211_STA_INFO_TX_BITRATE); nl_attr16(b, NL80211_RATE_INFO_BITRATE, 540); nl_attr32(b, NL80211_RATE_INFO_BITRATE32, 540); nl_nest_end(b);
    nest(b, NL80211_STA_INFO_RX_BITRATE); nl_attr16(b, NL80211_RATE_INFO_BITRATE, 540); nl_attr32(b, NL80211_RATE_INFO_BITRATE32, 540); nl_nest_end(b);
    nl_attr32(b, NL80211_STA_INFO_CONNECTED_TIME, now > s->connected_ns ? (uint32_t)((now - s->connected_ns) / 1000000000ULL) : 0);
    uint32_t update[2] = {0xFE, s->flags & 0xFE}; nl_attr(b, NL80211_STA_INFO_STA_FLAGS, update, 8);
    nest(b, NL80211_STA_INFO_BSS_PARAM);
    nl_attr(b, NL80211_STA_BSS_PARAM_SHORT_SLOT_TIME, NULL, 0);
    nl_attr8(b, NL80211_STA_BSS_PARAM_DTIM_PERIOD, (uint8_t)(v->dtim ? v->dtim : 2)); nl_attr16(b, NL80211_STA_BSS_PARAM_BEACON_INTERVAL, v->beacon_int ? v->beacon_int : 100);
    nl_nest_end(b);
    nl_nest_end(b);
}
static int get_station(struct netlink_socket* nl, const struct nlmsghdr* rq, bool dump) {
    struct wifi_vif* v = vif_from_attrs(); if (!v) return -19;
    struct nl_builder* b = builder(&reply_b); if (!b) return -12;
    if (!dump) {
        uint8_t mac[6]; if (!mac_of(&at[NL80211_ATTR_MAC], mac)) return -22;
        struct wifi_station* s = wifi_station_find(v, mac); if (!s) return -2;
        station_fill(b, v, s, 0, rq->seq, nl_portid(nl)); nl_send(nl, b); return 0;
    }
    nl_dump_begin(nl);
    if (v->iftype == NL80211_IFTYPE_STATION) { if (v->ap.used) { station_fill(b, v, &v->ap, NLM_F_MULTI, rq->seq, nl_portid(nl)); nl_send(nl, b); } }
    else for (unsigned i = 0; i < WIFI_STA_MAX; i++) if (v->stations[i].used) { builder(&reply_b); station_fill(b, v, &v->stations[i], NLM_F_MULTI, rq->seq, nl_portid(nl)); nl_send(nl, b); }
    nl_send_done(nl, rq); nl_dump_end(nl);
    return 1;
}
static bool flags_update(uint32_t* mask, uint32_t* set) {
    *mask = *set = 0;
    if (at[NL80211_ATTR_STA_FLAGS2].present) {
        if (at[NL80211_ATTR_STA_FLAGS2].length < 8) return false;
        memcpy(mask, at[NL80211_ATTR_STA_FLAGS2].data, 4); memcpy(set, at[NL80211_ATTR_STA_FLAGS2].data + 4, 4);
        *mask &= 0xFE; *set &= *mask; return true;
    }
    if (at[NL80211_ATTR_STA_FLAGS].present) {                                     /* old style: listed flags are set */
        struct nl_attr_view f[NL80211_STA_FLAG_MAX + 1];
        if (!nl_parse(at[NL80211_ATTR_STA_FLAGS].data, at[NL80211_ATTR_STA_FLAGS].length, f, NL80211_STA_FLAG_MAX)) return false;
        *mask = 0xFE; for (unsigned i = 1; i <= NL80211_STA_FLAG_MAX; i++) if (f[i].present) *set |= 1U << i;
    }
    return true;
}
static int set_station(struct wifi_vif* v, bool create) {
    uint8_t mac[6]; if (!mac_of(&at[NL80211_ATTR_MAC], mac)) return -22;
    uint32_t mask, set; if (!flags_update(&mask, &set)) return -22;
    if (v->iftype == NL80211_IFTYPE_STATION) {
        if (create) return -95;
        if (!v->ap.used || memcmp(v->ap.addr, mac, 6)) return -2;
        if (mask & ~(1U << NL80211_STA_FLAG_AUTHORIZED)) mask &= 1U << NL80211_STA_FLAG_AUTHORIZED;   /* the rest is the MLME's */
        v->ap.flags = (v->ap.flags & ~mask) | (set & mask);
        return 0;
    }
    if (v->iftype != NL80211_IFTYPE_AP) return -95;
    struct wifi_station* s = wifi_station_find(v, mac);
    if (create) {   /* without NL80211_FEATURE_FULL_AP_CLIENT_STATE a new station starts associated (hostapd relies on it) */
        if (s) return -17; s = wifi_station_add(v, mac); if (!s) return -28;
        s->flags |= 1U << NL80211_STA_FLAG_AUTHENTICATED | 1U << NL80211_STA_FLAG_ASSOCIATED;
    }
    else if (!s) return -2;
    if (at[NL80211_ATTR_STA_AID].present) s->aid = nl_u16(&at[NL80211_ATTR_STA_AID]);
    if (at[NL80211_ATTR_STA_LISTEN_INTERVAL].present) s->listen_interval = nl_u16(&at[NL80211_ATTR_STA_LISTEN_INTERVAL]);
    if (at[NL80211_ATTR_STA_CAPABILITY].present) s->capability = nl_u16(&at[NL80211_ATTR_STA_CAPABILITY]);
    s->flags = (s->flags & ~mask) | (set & mask);
    return 0;
}

/* ---- keys ---- */
struct key_args { const uint8_t* data; unsigned length; unsigned index; uint32_t cipher; const uint8_t* seq; unsigned seq_length; int type; bool def, def_mgmt; bool has_index; };
static bool key_parse(struct key_args* k) {
    memset(k, 0, sizeof(*k)); k->type = -1;
    if (at[NL80211_ATTR_KEY].present) {
        struct nl_attr_view ka[NL80211_KEY_MAX + 1];
        if (!nl_parse(at[NL80211_ATTR_KEY].data, at[NL80211_ATTR_KEY].length, ka, NL80211_KEY_MAX)) return false;
        if (ka[NL80211_KEY_DATA].present) { k->data = ka[NL80211_KEY_DATA].data; k->length = ka[NL80211_KEY_DATA].length; }
        if (ka[NL80211_KEY_IDX].present) { k->index = nl_u8(&ka[NL80211_KEY_IDX]); k->has_index = true; }
        k->cipher = nl_u32(&ka[NL80211_KEY_CIPHER]);
        if (ka[NL80211_KEY_SEQ].present) { k->seq = ka[NL80211_KEY_SEQ].data; k->seq_length = ka[NL80211_KEY_SEQ].length; }
        if (ka[NL80211_KEY_TYPE].present) k->type = (int)nl_u32(&ka[NL80211_KEY_TYPE]);
        k->def = ka[NL80211_KEY_DEFAULT].present; k->def_mgmt = ka[NL80211_KEY_DEFAULT_MGMT].present;
        return true;
    }
    if (at[NL80211_ATTR_KEY_DATA].present) { k->data = at[NL80211_ATTR_KEY_DATA].data; k->length = at[NL80211_ATTR_KEY_DATA].length; }
    if (at[NL80211_ATTR_KEY_IDX].present) { k->index = nl_u8(&at[NL80211_ATTR_KEY_IDX]); k->has_index = true; }
    k->cipher = nl_u32(&at[NL80211_ATTR_KEY_CIPHER]);
    if (at[NL80211_ATTR_KEY_SEQ].present) { k->seq = at[NL80211_ATTR_KEY_SEQ].data; k->seq_length = at[NL80211_ATTR_KEY_SEQ].length; }
    if (at[NL80211_ATTR_KEY_TYPE].present) k->type = (int)nl_u32(&at[NL80211_ATTR_KEY_TYPE]);
    k->def = at[NL80211_ATTR_KEY_DEFAULT].present; k->def_mgmt = at[NL80211_ATTR_KEY_DEFAULT_MGMT].present;
    return true;
}
static int key_command(struct netlink_socket* nl, const struct nlmsghdr* rq, struct wifi_vif* v, uint8_t cmd) {
    struct key_args k; if (!key_parse(&k)) return -22;
    uint8_t mac[6]; bool has_mac = mac_of(&at[NL80211_ATTR_MAC], mac);
    if (has_mac && !memcmp(mac, "\xff\xff\xff\xff\xff\xff", 6)) has_mac = false;
    bool pairwise = k.type == NL80211_KEYTYPE_PAIRWISE || (k.type == -1 && has_mac);
    if (k.index > 5) return -22;
    if (cmd == NL80211_CMD_NEW_KEY) {
        if (!k.data) return -22;
        return wifi_set_key(v, pairwise ? mac : NULL, k.index, k.cipher, k.data, k.length, k.seq, k.seq_length, pairwise);
    }
    if (cmd == NL80211_CMD_DEL_KEY) { int e = wifi_del_key(v, pairwise ? mac : NULL, k.index, pairwise); return e == -2 ? 0 : e; }
    if (cmd == NL80211_CMD_SET_KEY) {
        if (k.def_mgmt) { if (k.index < 4 || k.index > 5) return -22; v->default_mgmt_key = (uint8_t)k.index; return 0; }
        if (k.def || !k.def_mgmt) { if (k.index > 3) return -22; v->default_key = (uint8_t)k.index; return 0; }
        return 0;
    }
    /* GET_KEY: the transmit sequence counter of our key (hostapd puts the GTK's into message 3) */
    struct wifi_key* key = wifi_get_key(v, pairwise ? mac : NULL, k.index, pairwise); if (!key) return -2;
    struct nl_builder* b = builder(&reply_b); if (!b) return -12;
    genl_hdr(b, NL80211_GENL_ID, NL80211_CMD_NEW_KEY, 1, 0, rq->seq, nl_portid(nl));
    nl_attr32(b, NL80211_ATTR_IFINDEX, v->dev->index); nl_attr8(b, NL80211_ATTR_KEY_IDX, (uint8_t)k.index);
    if (pairwise) nl_attr(b, NL80211_ATTR_MAC, mac, 6);
    uint8_t seq[6]; for (unsigned i = 0; i < 6; i++) seq[i] = (uint8_t)(key->tx_pn >> (8 * i));
    nl_attr(b, NL80211_ATTR_KEY_SEQ, seq, 6); nl_attr32(b, NL80211_ATTR_KEY_CIPHER, key->cipher);
    nest(b, NL80211_ATTR_KEY); nl_attr8(b, NL80211_KEY_IDX, (uint8_t)k.index); nl_attr(b, NL80211_KEY_SEQ, seq, 6); nl_attr32(b, NL80211_KEY_CIPHER, key->cipher); nl_nest_end(b);
    nl_send(nl, b);
    return 0;
}

/* ---- regulatory ---- */
static int get_reg(struct netlink_socket* nl, const struct nlmsghdr* rq) {
    struct nl_builder* b = builder(&reply_b); if (!b) return -12;
    genl_hdr(b, NL80211_GENL_ID, NL80211_CMD_GET_REG, 1, 0, rq->seq, nl_portid(nl));
    nl_attr_string(b, NL80211_ATTR_REG_ALPHA2, reg_alpha2); nl_attr8(b, NL80211_ATTR_DFS_REGION, 0);
    nest(b, NL80211_ATTR_REG_RULES);
    static const uint32_t rules[2][3] = {{2402000, 2482000, 40000}, {5170000, 5250000, 80000}};
    for (unsigned i = 0; i < 2; i++) {
        nest(b, (uint16_t)i);
        nl_attr32(b, NL80211_ATTR_REG_RULE_FLAGS, 0); nl_attr32(b, NL80211_ATTR_FREQ_RANGE_START, rules[i][0]);
        nl_attr32(b, NL80211_ATTR_FREQ_RANGE_END, rules[i][1]); nl_attr32(b, NL80211_ATTR_FREQ_RANGE_MAX_BW, rules[i][2]);
        nl_attr32(b, NL80211_ATTR_POWER_RULE_MAX_ANT_GAIN, 0); nl_attr32(b, NL80211_ATTR_POWER_RULE_MAX_EIRP, 2000);
        nl_nest_end(b);
    }
    nl_nest_end(b);
    nl_send(nl, b);
    return 0;
}

/* ---- scan ---- */
static int trigger_scan(struct wifi_vif* v) {
    unsigned freqs[WIFI_CHANNELS], nfreqs = 0, nssids = 0; uint8_t ssids[4][33];
    if (at[NL80211_ATTR_SCAN_FREQUENCIES].present) {
        const struct nl_attr_view* list = &at[NL80211_ATTR_SCAN_FREQUENCIES];
        for (unsigned off = 0; off + 4 <= list->length;) {
            uint16_t size; memcpy(&size, list->data + off, 2); if (size < 4 || size > list->length - off) return -22;
            if (size >= 8) { if (nfreqs >= WIFI_CHANNELS) return -22; uint32_t f; memcpy(&f, list->data + off + 4, 4); freqs[nfreqs++] = f; }
            off += (size + 3u) & ~3u;
        }
        if (!nfreqs) return -22;
    }
    if (at[NL80211_ATTR_SCAN_SSIDS].present) {
        const struct nl_attr_view* list = &at[NL80211_ATTR_SCAN_SSIDS];
        for (unsigned off = 0; off + 4 <= list->length;) {
            uint16_t size; memcpy(&size, list->data + off, 2); if (size < 4 || size > list->length - off) return -22;
            unsigned len = size - 4u; if (len > 32 || nssids >= 4) return -22;
            memset(ssids[nssids], 0, 33); memcpy(ssids[nssids], list->data + off + 4, len); ssids[nssids][32] = (uint8_t)len; nssids++;
            off += (size + 3u) & ~3u;
        }
    }
    const struct nl_attr_view* ie = &at[NL80211_ATTR_IE];
    return wifi_scan_start(v, freqs, nfreqs, (const uint8_t (*)[33])ssids, nssids, ie->present ? ie->data : NULL, ie->present ? ie->length : 0);
}

/* ---- MLME requests ---- */
static int authenticate(struct wifi_vif* v) {
    uint8_t bssid[6]; if (!mac_of(&at[NL80211_ATTR_MAC], bssid) || !at[NL80211_ATTR_SSID].present || !at[NL80211_ATTR_AUTH_TYPE].present) return -22;
    uint32_t type = nl_u32(&at[NL80211_ATTR_AUTH_TYPE]);
    uint16_t alg = type == NL80211_AUTHTYPE_OPEN_SYSTEM ? 0 : type == NL80211_AUTHTYPE_SAE ? 3 : 0xFFFF;
    if (alg == 0xFFFF) return -95;
    const struct nl_attr_view *data = &at[NL80211_ATTR_AUTH_DATA], *ie = &at[NL80211_ATTR_IE];
    if (alg == 3 && !data->present) return -22;
    return wifi_authenticate(v, bssid, nl_u32(&at[NL80211_ATTR_WIPHY_FREQ]), at[NL80211_ATTR_SSID].data, at[NL80211_ATTR_SSID].length, alg,
                             data->present ? data->data : NULL, data->present ? data->length : 0, ie->present ? ie->data : NULL, ie->present ? ie->length : 0);
}
static int associate(struct wifi_vif* v) {
    uint8_t bssid[6], prev[6]; if (!mac_of(&at[NL80211_ATTR_MAC], bssid) || !at[NL80211_ATTR_SSID].present) return -22;
    bool has_prev = mac_of(&at[NL80211_ATTR_PREV_BSSID], prev);
    const struct nl_attr_view* ie = &at[NL80211_ATTR_IE];
    uint32_t mfp = at[NL80211_ATTR_USE_MFP].present ? nl_u32(&at[NL80211_ATTR_USE_MFP]) : NL80211_MFP_NO;
    return wifi_associate(v, bssid, nl_u32(&at[NL80211_ATTR_WIPHY_FREQ]), at[NL80211_ATTR_SSID].data, at[NL80211_ATTR_SSID].length,
                          ie->present ? ie->data : NULL, ie->present ? ie->length : 0, has_prev ? prev : NULL,
                          at[NL80211_ATTR_CONTROL_PORT].present, nl_u16(&at[NL80211_ATTR_CONTROL_PORT_ETHERTYPE]),
                          at[NL80211_ATTR_CONTROL_PORT_NO_ENCRYPT].present, mfp == NL80211_MFP_REQUIRED);
}
static int register_frame(struct netlink_socket* nl, struct wifi_vif* v) {
    uint16_t type = at[NL80211_ATTR_FRAME_TYPE].present ? nl_u16(&at[NL80211_ATTR_FRAME_TYPE]) : 0x00d0;
    const struct nl_attr_view* m = &at[NL80211_ATTR_FRAME_MATCH];
    if ((type & 0x0C) != 0 || (m->present && m->length > 32)) return -22;       /* management frames */
    for (unsigned i = 0; i < WIFI_REG_MAX; i++) if (!v->regs[i].used) {
        struct wifi_reg* r = &v->regs[i]; r->used = true; r->nl = nl; r->type = (uint16_t)(type & 0xFC);
        r->match_len = (uint8_t)(m->present ? m->length : 0); if (r->match_len) memcpy(r->match, m->data, r->match_len);
        return 0;
    }
    return -28;
}
static int frame_tx(struct netlink_socket* nl, const struct nlmsghdr* rq, struct wifi_vif* v) {
    if (!at[NL80211_ATTR_FRAME].present) return -22;
    bool no_ack = at[NL80211_ATTR_DONT_WAIT_FOR_ACK].present; uint64_t cookie = 0;
    int e = wifi_tx_mgmt(v, at[NL80211_ATTR_FRAME].data, at[NL80211_ATTR_FRAME].length, nl_u32(&at[NL80211_ATTR_WIPHY_FREQ]), no_ack, &cookie);
    if (e) return e;
    if (no_ack) return 0;
    struct nl_builder* b = builder(&reply_b); if (!b) return -12;
    genl_hdr(b, NL80211_GENL_ID, NL80211_CMD_FRAME, 1, 0, rq->seq, nl_portid(nl));
    nl_attr64(b, NL80211_ATTR_COOKIE, cookie); nl_send(nl, b);
    return 0;
}
static int set_channel(struct wifi_vif* v, struct wifi_radio* r) {
    unsigned freq = nl_u32(&at[NL80211_ATTR_WIPHY_FREQ]); bool known = false;
    for (unsigned i = 0; i < WIFI_CHANNELS; i++) if (wifi_channel_freqs[i] == freq) known = true;
    if (!known) return -22;
    if (v && ((v->iftype == NL80211_IFTYPE_AP && v->ap_started) || (v->iftype == NL80211_IFTYPE_STATION && v->sme != WIFI_IDLE)))
        return freq == v->freq ? 0 : -16;
    struct wifi_vif* other = v == r->vif ? r->vif2 : r->vif;                       /* one radio: a co-hosted active vif holds the channel */
    if (other && ((other->iftype == NL80211_IFTYPE_AP && other->ap_started) || (other->iftype == NL80211_IFTYPE_STATION && other->sme != WIFI_IDLE)))
        return freq == r->freq ? 0 : -16;
    if (r->scanning) return -16;
    if (v) v->freq = freq;
    r->freq = freq; if (r->ops->set_channel) r->ops->set_channel(r, freq);
    return 0;
}
static int probe_client(struct netlink_socket* nl, const struct nlmsghdr* rq, struct wifi_vif* v) {
    uint8_t mac[6]; if (!mac_of(&at[NL80211_ATTR_MAC], mac)) return -22;
    if (v->iftype != NL80211_IFTYPE_AP) return -95;
    struct wifi_station* s = wifi_station_find(v, mac); if (!s) return -2;
    uint64_t cookie = ++v->probe_cookie | 1ULL << 40;
    struct nl_builder* b = builder(&reply_b); if (!b) return -12;
    genl_hdr(b, NL80211_GENL_ID, NL80211_CMD_PROBE_CLIENT, 1, 0, rq->seq, nl_portid(nl)); nl_attr64(b, NL80211_ATTR_COOKIE, cookie); nl_send(nl, b);
    struct nl_builder* e = builder(&event_b); if (!e) return 0;
    genl_hdr(e, NL80211_GENL_ID, NL80211_CMD_PROBE_CLIENT, 1, 0, 0, 0);
    ids(e, v); nl_attr(e, NL80211_ATTR_MAC, mac, 6); nl_attr64(e, NL80211_ATTR_COOKIE, cookie); nl_attr(e, NL80211_ATTR_ACK, NULL, 0);
    nl_multicast(16, GRP_MLME, e, NULL);
    return 0;
}
static int set_cqm(struct wifi_vif* v) {
    if (!at[NL80211_ATTR_CQM].present) return -22;
    struct nl_attr_view c[NL80211_ATTR_CQM_MAX + 1];
    if (!nl_parse(at[NL80211_ATTR_CQM].data, at[NL80211_ATTR_CQM].length, c, NL80211_ATTR_CQM_MAX)) return -22;
    if (c[NL80211_ATTR_CQM_RSSI_THOLD].present) {
        int32_t t = (int32_t)nl_u32(&c[NL80211_ATTR_CQM_RSSI_THOLD]);
        if (t > 0 || t < -150) return -22;
        v->cqm_threshold = t; v->cqm_hysteresis = nl_u32(&c[NL80211_ATTR_CQM_RSSI_HYST]); v->cqm_state = 0;
        if (v->cqm_hysteresis > 50) v->cqm_hysteresis = 50;
    }
    return 0;
}

int nl80211_request(struct netlink_socket* nl, const struct nlmsghdr* rq, const uint8_t* payload, unsigned length) {
    if (length < 4) return -22;
    if (task_net_namespace()) return -19;                                        /* an isolated namespace has no wiphys */
    uint8_t cmd = payload[0];
    if (!nl_parse(payload + 4, length - 4, at, NL80211_ATTR_MAX)) return -22;
    bool dump = (rq->flags & NLM_F_DUMP) == NLM_F_DUMP;
    switch (cmd) {                                                                  /* queries: anyone */
    case NL80211_CMD_GET_PROTOCOL_FEATURES: {
        struct nl_builder* b = builder(&reply_b); if (!b) return -12;
        genl_hdr(b, NL80211_GENL_ID, NL80211_CMD_GET_PROTOCOL_FEATURES, 1, 0, rq->seq, nl_portid(nl));
        nl_attr32(b, NL80211_ATTR_PROTOCOL_FEATURES, NL80211_PROTOCOL_FEATURE_SPLIT_WIPHY_DUMP); nl_send(nl, b); return 0;
    }
    case NL80211_CMD_GET_WIPHY: return get_wiphy(nl, rq, dump);
    case NL80211_CMD_GET_INTERFACE: return get_interface(nl, rq, dump);
    case NL80211_CMD_GET_SCAN: return get_scan(nl, rq);
    case NL80211_CMD_GET_STATION: return get_station(nl, rq, dump);
    case NL80211_CMD_GET_REG: return get_reg(nl, rq);
    case NL80211_CMD_GET_SURVEY: if (!dump) return -95; nl_dump_begin(nl); nl_send_done(nl, rq); nl_dump_end(nl); return 1;
    case NL80211_CMD_GET_POWER_SAVE: {
        struct wifi_vif* v = vif_from_attrs(); if (!v) return -19;
        struct nl_builder* b = builder(&reply_b); if (!b) return -12;
        genl_hdr(b, NL80211_GENL_ID, NL80211_CMD_GET_POWER_SAVE, 1, 0, rq->seq, nl_portid(nl)); nl_attr32(b, NL80211_ATTR_PS_STATE, 0); nl_send(nl, b); return 0;
    }
    default: break;
    }
    if (!netlink_admin()) return -1;                                                /* GENL_UNS_ADMIN_PERM */
    if (cmd == NL80211_CMD_REQ_SET_REG) {
        char alpha2[4]; if (!nl_string(&at[NL80211_ATTR_REG_ALPHA2], alpha2, sizeof(alpha2)) || strlen(alpha2) != 2) return -22;
        memcpy(reg_alpha2, alpha2, 3); return 0;
    }
    if (cmd == NL80211_CMD_REGISTER_BEACONS) {                                     /* hostapd (OBSS scan); no beacon events */
        struct wifi_radio* r = radio_from_attrs(); if (!r) return -19;
        if (beacon_nl[r->index] && beacon_nl[r->index] != nl) return -16;
        beacon_nl[r->index] = nl; return 0;
    }
    if (cmd == NL80211_CMD_SET_WIPHY && !at[NL80211_ATTR_IFINDEX].present) {
        struct wifi_radio* r = radio_from_attrs(); if (!r) return -19;
        if (at[NL80211_ATTR_WIPHY_FREQ].present) { int e = set_channel(r->vif, r); if (e) return e; }
        goto wiphy_params;
    }
    if (cmd == NL80211_CMD_NEW_INTERFACE) {                                        /* a 2nd interface on the phy (STA+AP concurrency) */
        struct wifi_radio* r = radio_from_attrs(); if (!r) return -19;
        if (!at[NL80211_ATTR_IFTYPE].present) return -22;
        char want[IFNAMSIZ + 1] = ""; nl_string(&at[NL80211_ATTR_IFNAME], want, sizeof(want));
        struct wifi_vif* nv; int e = wifi_add_vif(r, nl_u32(&at[NL80211_ATTR_IFTYPE]), want, &nv); if (e) return e;
        if (at[NL80211_ATTR_SOCKET_OWNER].present) nv->owner = nl;
        generation++;
        struct nl_builder* b = builder(&reply_b); if (b) { iface_fill(b, nv, NL80211_CMD_NEW_INTERFACE, 0, rq->seq, nl_portid(nl)); nl_send(nl, b); }
        return b ? 1 : 0;
    }
    struct wifi_vif* v = vif_from_attrs(); if (!v) return -19;
    struct wifi_radio* r = v->radio;
    switch (cmd) {
    case NL80211_CMD_SET_WIPHY:
        if (at[NL80211_ATTR_WIPHY_FREQ].present) { int e = set_channel(v, r); if (e) return e; }
    wiphy_params:
        r = radio_from_attrs(); if (!r) return -19;
        if (at[NL80211_ATTR_WIPHY_RETRY_SHORT].present) { uint8_t x = nl_u8(&at[NL80211_ATTR_WIPHY_RETRY_SHORT]); if (!x) return -22; r->retry_short = x; }
        if (at[NL80211_ATTR_WIPHY_RETRY_LONG].present) { uint8_t x = nl_u8(&at[NL80211_ATTR_WIPHY_RETRY_LONG]); if (!x) return -22; r->retry_long = x; }
        if (at[NL80211_ATTR_WIPHY_FRAG_THRESHOLD].present) r->frag_threshold = nl_u32(&at[NL80211_ATTR_WIPHY_FRAG_THRESHOLD]);
        if (at[NL80211_ATTR_WIPHY_RTS_THRESHOLD].present) r->rts_threshold = nl_u32(&at[NL80211_ATTR_WIPHY_RTS_THRESHOLD]);
        if (at[NL80211_ATTR_WIPHY_NAME].present) return -95;
        return 0;
    case NL80211_CMD_SET_CHANNEL: return set_channel(v, r);
    case NL80211_CMD_SET_INTERFACE:
        if (at[NL80211_ATTR_4ADDR].present && nl_u8(&at[NL80211_ATTR_4ADDR])) return -95;
        if (at[NL80211_ATTR_IFTYPE].present) { int e = wifi_set_iftype(v, nl_u32(&at[NL80211_ATTR_IFTYPE])); if (e) return e; generation++; }
        return 0;
    case NL80211_CMD_NEW_KEY: case NL80211_CMD_SET_KEY: case NL80211_CMD_DEL_KEY: case NL80211_CMD_GET_KEY:
        return key_command(nl, rq, v, cmd);
    case NL80211_CMD_DEL_INTERFACE:
        unexpected_nl[r->index] = NULL; beacon_nl[r->index] = NULL;
        if (v == r->vif2) { generation++; return wifi_del_vif(v); }
        return -95;                                                                /* the primary interface stays */
    case NL80211_CMD_START_AP: {
        const struct nl_attr_view *head = &at[NL80211_ATTR_BEACON_HEAD], *tail = &at[NL80211_ATTR_BEACON_TAIL], *ssid = &at[NL80211_ATTR_SSID];
        if (!head->present) return -22;
        int e = wifi_start_ap(v, head->data, head->length, tail->present ? tail->data : NULL, tail->present ? tail->length : 0,
                              nl_u32(&at[NL80211_ATTR_BEACON_INTERVAL]), nl_u32(&at[NL80211_ATTR_DTIM_PERIOD]),
                              ssid->present ? ssid->data : NULL, ssid->present ? ssid->length : 0,
                              at[NL80211_ATTR_HIDDEN_SSID].present && nl_u32(&at[NL80211_ATTR_HIDDEN_SSID]),
                              at[NL80211_ATTR_PRIVACY].present, nl_u32(&at[NL80211_ATTR_WIPHY_FREQ]));
        if (!e && at[NL80211_ATTR_SOCKET_OWNER].present) v->owner = nl;
        return e;
    }
    case NL80211_CMD_SET_BEACON: {
        const struct nl_attr_view *head = &at[NL80211_ATTR_BEACON_HEAD], *tail = &at[NL80211_ATTR_BEACON_TAIL];
        return wifi_set_beacon(v, head->present ? head->data : NULL, head->length, tail->present ? tail->data : NULL, tail->present ? tail->length : 0);
    }
    case NL80211_CMD_STOP_AP: return wifi_stop_ap(v);
    case NL80211_CMD_NEW_STATION: return set_station(v, true);
    case NL80211_CMD_SET_STATION: return set_station(v, false);
    case NL80211_CMD_DEL_STATION: {
        if (v->iftype != NL80211_IFTYPE_AP) return -95;
        uint8_t mac[6]; bool one = mac_of(&at[NL80211_ATTR_MAC], mac) && memcmp(mac, "\xff\xff\xff\xff\xff\xff", 6);
        if (one && !wifi_station_find(v, mac)) return -2;
        wifi_station_del(v, one ? mac : NULL); return 0;
    }
    case NL80211_CMD_SET_BSS: case NL80211_CMD_SET_POWER_SAVE: case NL80211_CMD_FRAME_WAIT_CANCEL: case NL80211_CMD_SET_TX_BITRATE_MASK:
    case NL80211_CMD_SET_NOACK_MAP:
        return 0;
    case NL80211_CMD_SET_CQM: return set_cqm(v);
    case NL80211_CMD_TRIGGER_SCAN: return trigger_scan(v);
    case NL80211_CMD_ABORT_SCAN: return wifi_scan_abort(v);
    case NL80211_CMD_AUTHENTICATE: return authenticate(v);
    case NL80211_CMD_ASSOCIATE: { int e = associate(v); if (!e && at[NL80211_ATTR_SOCKET_OWNER].present) v->owner = nl; return e; }
    case NL80211_CMD_DEAUTHENTICATE: case NL80211_CMD_DISASSOCIATE: {
        uint8_t bssid[6]; if (!mac_of(&at[NL80211_ATTR_MAC], bssid)) return -22;
        return wifi_deauthenticate(v, bssid, nl_u16(&at[NL80211_ATTR_REASON_CODE]), cmd == NL80211_CMD_DISASSOCIATE, at[NL80211_ATTR_LOCAL_STATE_CHANGE].present);
    }
    case NL80211_CMD_DISCONNECT:
        if (v->iftype != NL80211_IFTYPE_STATION || v->sme == WIFI_IDLE) return -107;
        return wifi_deauthenticate(v, v->bssid, at[NL80211_ATTR_REASON_CODE].present ? nl_u16(&at[NL80211_ATTR_REASON_CODE]) : 3, false, false);
    case NL80211_CMD_REGISTER_FRAME: return register_frame(nl, v);
    case NL80211_CMD_FRAME: return frame_tx(nl, rq, v);
    case NL80211_CMD_UNEXPECTED_FRAME:
        if (v->iftype != NL80211_IFTYPE_AP) return -22;
        if (unexpected_nl[r->index] && unexpected_nl[r->index] != nl) return -16;
        unexpected_nl[r->index] = nl; return 0;
    case NL80211_CMD_PROBE_CLIENT: return probe_client(nl, rq, v);
    default: return -95;
    }
}
void nl80211_socket_closed(struct netlink_socket* nl) {
    for (unsigned i = 0; i < WIFI_RADIO_MAX; i++) { if (unexpected_nl[i] == nl) unexpected_nl[i] = NULL; if (beacon_nl[i] == nl) beacon_nl[i] = NULL; }
    wifi_owner_closed(nl);
}

/* ---- events ---- */
static struct nl_builder* event(uint8_t cmd) {
    struct nl_builder* b = builder(&event_b); if (!b) return NULL;
    genl_hdr(b, NL80211_GENL_ID, cmd, 1, 0, 0, 0); return b;
}
void nl80211_event_frame(struct wifi_vif* v, uint8_t cmd, const uint8_t* frame, unsigned length, unsigned freq, int signal) {
    struct nl_builder* b = event(cmd); if (!b) return;
    ids(b, v); nl_attr(b, NL80211_ATTR_FRAME, frame, length);
    if (freq) nl_attr32(b, NL80211_ATTR_WIPHY_FREQ, freq);
    if (signal) nl_attr32(b, NL80211_ATTR_RX_SIGNAL_DBM, (uint32_t)signal);
    nl_multicast(16, GRP_MLME, b, NULL);
}
void nl80211_event_timeout(struct wifi_vif* v, uint8_t cmd, const uint8_t* addr) {
    struct nl_builder* b = event(cmd); if (!b) return;
    ids(b, v); nl_attr(b, NL80211_ATTR_TIMED_OUT, NULL, 0); nl_attr(b, NL80211_ATTR_MAC, addr, 6);
    nl_multicast(16, GRP_MLME, b, NULL);
}
bool nl80211_deliver_mgmt(struct wifi_vif* v, const uint8_t* frame, unsigned length, unsigned freq, int signal) {
    uint16_t type = (uint16_t)(frame[0] & 0xFC);
    for (unsigned i = 0; i < WIFI_REG_MAX; i++) {
        struct wifi_reg* r = &v->regs[i];
        if (!r->used || r->type != type || length < 24u + r->match_len || memcmp(frame + 24, r->match, r->match_len)) continue;
        struct nl_builder* b = event(NL80211_CMD_FRAME); if (!b) return false;
        ids(b, v); nl_attr32(b, NL80211_ATTR_WIPHY_FREQ, freq); nl_attr32(b, NL80211_ATTR_RX_SIGNAL_DBM, (uint32_t)signal);
        nl_attr(b, NL80211_ATTR_FRAME, frame, length);
        nl_unicast(r->nl, b);
        return true;
    }
    return false;
}
void nl80211_event_tx_status(struct wifi_vif* v, uint64_t cookie, const uint8_t* frame, unsigned length, bool acked) {
    struct nl_builder* b = event(NL80211_CMD_FRAME_TX_STATUS); if (!b) return;
    ids(b, v); nl_attr(b, NL80211_ATTR_FRAME, frame, length); nl_attr64(b, NL80211_ATTR_COOKIE, cookie);
    if (acked) nl_attr(b, NL80211_ATTR_ACK, NULL, 0);
    nl_multicast(16, GRP_MLME, b, NULL);
}
void nl80211_event_scan(struct wifi_vif* v, uint8_t cmd) {
    struct nl_builder* b = event(cmd); if (!b) return;
    struct wifi_radio* r = v->radio;
    ids(b, v); nl_attr32(b, NL80211_ATTR_GENERATION, r->generation);
    nest(b, NL80211_ATTR_SCAN_SSIDS);
    for (unsigned i = 0; i < r->scan_nssids; i++) nl_attr(b, (uint16_t)i, r->scan_ssids[i], r->scan_ssids[i][32]);
    nl_nest_end(b);
    nest(b, NL80211_ATTR_SCAN_FREQUENCIES);
    for (unsigned i = 0; i < r->scan_count; i++) nl_attr32(b, (uint16_t)i, r->scan_freqs[i]);
    nl_nest_end(b);
    nl_multicast(16, GRP_SCAN, b, NULL);
}
void nl80211_event_station(struct wifi_vif* v, uint8_t cmd, const uint8_t* addr) {
    struct nl_builder* b = event(cmd); if (!b) return;
    nl_attr32(b, NL80211_ATTR_IFINDEX, v->dev->index); nl_attr(b, NL80211_ATTR_MAC, addr, 6); nl_attr32(b, NL80211_ATTR_GENERATION, ++generation);
    nest(b, NL80211_ATTR_STA_INFO); nl_nest_end(b);
    nl_multicast(16, GRP_MLME, b, NULL);
}
void nl80211_event_unexpected(struct wifi_vif* v, const uint8_t* addr) {
    struct netlink_socket* nl = unexpected_nl[v->radio->index]; if (!nl) return;
    struct nl_builder* b = event(NL80211_CMD_UNEXPECTED_FRAME); if (!b) return;
    ids(b, v); nl_attr(b, NL80211_ATTR_MAC, addr, 6);
    nl_unicast(nl, b);
}
void nl80211_event_cqm(struct wifi_vif* v, bool low, int signal) {
    struct nl_builder* b = event(NL80211_CMD_NOTIFY_CQM); if (!b) return;
    ids(b, v); nl_attr(b, NL80211_ATTR_MAC, v->bssid, 6);
    nest(b, NL80211_ATTR_CQM);
    nl_attr32(b, NL80211_ATTR_CQM_RSSI_THRESHOLD_EVENT, low ? 0 : 1); nl_attr32(b, NL80211_ATTR_CQM_RSSI_LEVEL, (uint32_t)signal);
    nl_nest_end(b);
    nl_multicast(16, GRP_MLME, b, NULL);
}
void nl80211_event_iface(struct wifi_vif* v, uint8_t cmd) {
    if (cmd == NL80211_CMD_DEL_INTERFACE) { unexpected_nl[v->radio->index] = NULL; beacon_nl[v->radio->index] = NULL; }
    struct nl_builder* b = builder(&event_b); if (!b) return;
    iface_fill(b, v, cmd, 0, 0, 0);
    nl_multicast(16, GRP_CONFIG, b, NULL);
}
