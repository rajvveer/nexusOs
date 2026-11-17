/* Wi-Fi: a mac80211-style "softmac" (wifi.c) between nl80211 (nl80211.c: wpa_supplicant, hostapd, iw) and radio
 * drivers. The only driver today is the virtual radio medium of wifi.c (mac80211_hwsim-like: radios created with
 * /proc/sys/net/nexus/wifi_sim hear each other on the same channel); a hardware driver (MediaTek MT7922 on the
 * user's PC) plugs in through struct wifi_driver_ops below and nothing above it changes.
 *
 * ---- Driver interface (what a hardware driver implements) -------------------------------------------------------
 * Frames crossing the interface are 802.11 MPDUs: MAC header + body, no FCS, no radiotap/descriptor headers.
 * The softmac does all MLME (scan, authentication, association, beacon-loss detection, AP beaconing), the 802.3 <->
 * 802.11 data conversion, port control (802.1X "authorized") and the crypto (CCMP-128 for data and robust management
 * frames, BIP-CMAC-128 for group management frames) in software, so a minimal driver only moves frames:
 *   tx(radio, frame, length, cookie)   transmit on the current channel; the driver later calls
 *                                       wifi_tx_status(radio, cookie, acked) (unicast: acked = the peer ACKed after
 *                                       the hardware's retries; group-addressed: acked = sent). Return 0 or -errno
 *                                       (-11 queue full: the frame is dropped).
 *   set_channel(radio, freq_mhz)        tune (20 MHz); the softmac changes channel only when scanning or when an
 *                                       interface starts (AP) or joins (STA) a BSS.
 *   set_mode(radio)                     radio->vif's type / BSSID / association state changed (AP start/stop, STA
 *                                       associated or not): program RX filters (our address, BSSID, beacons of the
 *                                       BSS, probe requests in AP mode, everything while scanning).
 *   destroy(radio)                      the radio is going away (wifi_radio_unregister).
 * and reports received frames with wifi_rx(radio, frame, length, &status) (status: channel frequency, signal in dBm;
 * frames with a bad FCS are not passed up). Optional offloads a firmware like the MT7922's would want later (hardware
 * scan, hardware keys with the Protected bit already removed: WIFI_RX_DECRYPTED, beacon templates) are flags in
 * struct wifi_rx_status / the radio and can be added without changing the userspace view.
 * Registration: wifi_radio_register(ops, priv, mac, &radio) creates phyN and its interface wlanN (station mode,
 * down, no carrier); wifi_radio_unregister() removes both.
 * Context: everything runs with the BKL held (syscalls, network poll); a driver's IRQ handler must only queue frames
 * and let its poll function (called from netdev_poll via wifi_poll) call wifi_rx/wifi_tx_status.
 * ------------------------------------------------------------------------------------------------------------------ */
#ifndef NEXUS_WIFI_H
#define NEXUS_WIFI_H
#include "netdev.h"

#define WIFI_RADIO_MAX 8
#define WIFI_BSS_MAX 32            /* scan table entries per radio */
#define WIFI_STA_MAX 16            /* stations per AP interface */
#define WIFI_REG_MAX 48            /* management frame registrations (NL80211_CMD_REGISTER_FRAME) per interface */
#define WIFI_IE_BYTES 768
#define WIFI_FRAME_MAX 2400        /* largest MPDU (1500-byte MSDU + headers + CCMP) */
#define WIFI_CHANNELS 17           /* 2.4 GHz channels 1-13, 5 GHz 36/40/44/48 */

#define WIFI_CIPHER_CCMP 0x000FAC04U
#define WIFI_CIPHER_BIP_CMAC 0x000FAC06U      /* BIP-CMAC-128 */
#define WIFI_CIPHER_GCMP 0x000FAC08U
#define WIFI_CIPHER_GCMP_256 0x000FAC09U
#define WIFI_CIPHER_CCMP_256 0x000FAC0AU
#define WIFI_CIPHER_BIP_GMAC_128 0x000FAC0BU
#define WIFI_CIPHER_BIP_GMAC_256 0x000FAC0CU

struct wifi_radio;
struct wifi_vif;
struct netlink_socket;

struct wifi_rx_status { unsigned freq; int signal_dbm; unsigned flags; };
#define WIFI_RX_DECRYPTED 1        /* (hardware crypto, not used by the software path) */

struct wifi_driver_ops {
    int (*tx)(struct wifi_radio* radio, const uint8_t* frame, unsigned length, uint64_t cookie);
    int (*set_channel)(struct wifi_radio* radio, unsigned freq);
    void (*set_mode)(struct wifi_radio* radio);
    void (*destroy)(struct wifi_radio* radio);
};

struct wifi_key {
    bool set; uint32_t cipher; uint8_t index, rounds, mic_len;
    uint8_t key[32]; uint32_t rk[60];      /* AES-128/256 key and its expanded encryption schedule (14 rounds max) */
    uint64_t tx_pn, rx_pn;                 /* 48-bit packet numbers (rx_pn: highest accepted, replay check) */
};
struct wifi_bss {
    bool used; uint8_t bssid[6]; unsigned freq; uint16_t capability, beacon_int; uint64_t tsf;
    uint8_t ies[WIFI_IE_BYTES]; uint16_t ies_len; bool presp;        /* last probe response's IEs, else the beacon's */
    uint8_t beacon_ies[WIFI_IE_BYTES]; uint16_t beacon_ies_len; bool beaconed;
    int32_t signal_mbm; uint64_t seen_ns;
};
struct wifi_station {                      /* an AP interface's peer (NL80211_CMD_NEW_STATION), or a STA's AP */
    bool used; uint8_t addr[6]; uint16_t aid, capability, listen_interval;
    uint32_t flags;                        /* 1 << NL80211_STA_FLAG_* */
    struct wifi_key ptk;
    uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes, tx_failed, rx_dropped, connected_ns, last_rx_ns;
    int signal;
};
struct wifi_reg {
    bool used; struct netlink_socket* nl; uint16_t type; uint8_t match_len; uint8_t match[32];
};
enum wifi_sme { WIFI_IDLE, WIFI_AUTHENTICATING, WIFI_AUTHENTICATED, WIFI_ASSOCIATING, WIFI_ASSOCIATED };

struct wifi_vif {
    struct wifi_radio* radio; struct netdev* dev; uint32_t iftype; uint64_t wdev;
    uint16_t seq;
    struct netlink_socket* owner;          /* NL80211_ATTR_SOCKET_OWNER: torn down when it closes */
    struct wifi_reg regs[WIFI_REG_MAX];
    /* station (client) */
    enum wifi_sme sme; uint8_t bssid[6]; unsigned freq; uint8_t ssid[32]; uint8_t ssid_len; uint16_t auth_alg;
    uint8_t pending[WIFI_FRAME_MAX]; unsigned pending_len, tries; uint64_t deadline_ns;   /* auth/assoc retry */
    bool control_port, control_port_no_encrypt, use_mfp; uint16_t control_port_ethertype;
    struct wifi_station ap;
    struct wifi_key gtk[4], igtk[2];       /* receive: group keys 1-3, BIP keys 4-5 */
    uint64_t last_beacon_ns, probe_sent_ns; int cqm_threshold; unsigned cqm_hysteresis; int cqm_state;
    /* access point */
    bool ap_started, hidden, privacy;
    uint8_t beacon[WIFI_IE_BYTES * 2]; unsigned beacon_head, beacon_tail;
    uint16_t beacon_int, dtim; uint64_t next_beacon_ns;
    uint8_t ap_ssid[32]; uint8_t ap_ssid_len;
    struct wifi_station stations[WIFI_STA_MAX];
    struct wifi_key gtk_tx[4], igtk_tx[2]; uint8_t default_key, default_mgmt_key;
    uint64_t probe_cookie;
};

struct wifi_radio {
    bool used; unsigned index; char name[16]; uint8_t mac[6];
    const struct wifi_driver_ops* ops; void* priv;
    unsigned freq;                         /* the channel the radio is tuned to */
    struct wifi_vif* vif;                  /* the primary interface */
    struct wifi_vif* vif2;                 /* an optional second interface on this phy (STA+AP concurrency) */
    struct wifi_bss bss[WIFI_BSS_MAX]; uint32_t generation;
    /* scan (software: one channel at a time, probe requests when SSIDs are given) */
    bool scanning, scan_aborted; struct wifi_vif* scan_vif; unsigned scan_freqs[WIFI_CHANNELS], scan_count, scan_pos;
    uint64_t scan_dwell_end; uint8_t scan_ssids[4][33]; unsigned scan_nssids; uint8_t scan_ie[256]; unsigned scan_ie_len;
    uint32_t frag_threshold, rts_threshold; uint8_t retry_short, retry_long;
    /* virtual medium (hwsim) */
    int sim_signal; bool sim_off;          /* signal of this radio's transmissions as the others hear them; out of range */
};

extern struct wifi_radio* wifi_radios[WIFI_RADIO_MAX];
extern const unsigned wifi_channel_freqs[WIFI_CHANNELS];

/* driver -> softmac */
int wifi_radio_register(const struct wifi_driver_ops* ops, void* priv, const uint8_t mac[6], struct wifi_radio** out);
void wifi_radio_unregister(struct wifi_radio* radio);
void wifi_rx(struct wifi_radio* radio, const uint8_t* frame, unsigned length, const struct wifi_rx_status* status);
void wifi_tx_status(struct wifi_radio* radio, uint64_t cookie, bool acked);
/* network core */
void wifi_poll(uint64_t now);              /* netdev_poll: timers, beacons, scan, the virtual medium */
bool wifi_rx_pending(void);                /* network.c: frames queued on the virtual medium */
void wifi_rx_flush(void);
unsigned wifi_sim_text(char* out, unsigned capacity);        /* /proc/sys/net/nexus/wifi_sim */
int wifi_sim_command(const char* text);
/* softmac services for nl80211.c */
struct wifi_vif* wifi_vif_by_dev(const struct netdev* dev);
int wifi_set_iftype(struct wifi_vif* vif, uint32_t iftype);
int wifi_add_vif(struct wifi_radio* radio, uint32_t iftype, const char* want_name, struct wifi_vif** out);   /* a 2nd vif on the phy */
int wifi_del_vif(struct wifi_vif* vif);                                                /* remove a secondary vif */
int wifi_scan_start(struct wifi_vif* vif, const unsigned* freqs, unsigned nfreqs, const uint8_t (*ssids)[33], unsigned nssids,
                    const uint8_t* ie, unsigned ie_len);
int wifi_scan_abort(struct wifi_vif* vif);
int wifi_authenticate(struct wifi_vif* vif, const uint8_t* bssid, unsigned freq, const uint8_t* ssid, unsigned ssid_len,
                      uint16_t alg, const uint8_t* data, unsigned data_len, const uint8_t* ie, unsigned ie_len);
int wifi_associate(struct wifi_vif* vif, const uint8_t* bssid, unsigned freq, const uint8_t* ssid, unsigned ssid_len,
                   const uint8_t* ie, unsigned ie_len, const uint8_t* prev_bssid, bool control_port, uint16_t ethertype,
                   bool no_encrypt, bool mfp);
int wifi_deauthenticate(struct wifi_vif* vif, const uint8_t* bssid, uint16_t reason, bool disassoc, bool local_only);
int wifi_start_ap(struct wifi_vif* vif, const uint8_t* head, unsigned head_len, const uint8_t* tail, unsigned tail_len,
                  unsigned interval, unsigned dtim, const uint8_t* ssid, unsigned ssid_len, bool hidden, bool privacy, unsigned freq);
int wifi_set_beacon(struct wifi_vif* vif, const uint8_t* head, unsigned head_len, const uint8_t* tail, unsigned tail_len);
int wifi_stop_ap(struct wifi_vif* vif);
int wifi_tx_mgmt(struct wifi_vif* vif, const uint8_t* frame, unsigned length, unsigned freq, bool no_ack, uint64_t* cookie);
struct wifi_station* wifi_station_find(struct wifi_vif* vif, const uint8_t* addr);
struct wifi_station* wifi_station_add(struct wifi_vif* vif, const uint8_t* addr);
void wifi_station_del(struct wifi_vif* vif, const uint8_t* addr);
int wifi_set_key(struct wifi_vif* vif, const uint8_t* addr, unsigned index, uint32_t cipher, const uint8_t* key, unsigned key_len,
                 const uint8_t* seq, unsigned seq_len, bool pairwise);
int wifi_del_key(struct wifi_vif* vif, const uint8_t* addr, unsigned index, bool pairwise);
struct wifi_key* wifi_get_key(struct wifi_vif* vif, const uint8_t* addr, unsigned index, bool pairwise);
void wifi_set_authorized(struct wifi_vif* vif, const uint8_t* addr, bool authorized);
void wifi_owner_closed(struct netlink_socket* nl);
bool wifi_selftest(void);                 /* AES-128 (FIPS-197), AES-CMAC (RFC 4493), CCMP (IEEE 802.11 M.6.4) */

/* nl80211.c */
void nl80211_event_frame(struct wifi_vif* vif, uint8_t cmd, const uint8_t* frame, unsigned length, unsigned freq, int signal);
void nl80211_event_timeout(struct wifi_vif* vif, uint8_t cmd, const uint8_t* addr);
bool nl80211_deliver_mgmt(struct wifi_vif* vif, const uint8_t* frame, unsigned length, unsigned freq, int signal);
void nl80211_event_tx_status(struct wifi_vif* vif, uint64_t cookie, const uint8_t* frame, unsigned length, bool acked);
void nl80211_event_scan(struct wifi_vif* vif, uint8_t cmd);
void nl80211_event_station(struct wifi_vif* vif, uint8_t cmd, const uint8_t* addr);
void nl80211_event_unexpected(struct wifi_vif* vif, const uint8_t* addr);
void nl80211_event_cqm(struct wifi_vif* vif, bool low, int signal);
void nl80211_event_iface(struct wifi_vif* vif, uint8_t cmd);
#endif
