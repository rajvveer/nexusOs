/* LE Security Manager (Core Vol 3 Part H) on fixed channel 6.
 *  - LE legacy pairing: Just Works and Passkey Entry (TK), c1 confirm values, s1 STK, key distribution
 *    (Encryption Information/Central Identification = LTK/EDIV/Rand, Identity Information/Address = IRK).
 *  - LE Secure Connections: P-256 ECDH (peer keys validated; a reflected key is refused), Just Works, Numeric
 *    Comparison (g2, confirmed through mgmt), Passkey Entry (20 rounds of f4), f5 MacKey/LTK, f6 DHKey checks.
 *  - Method selection and when user space is asked follow Linux (net/bluetooth/smp.c): a locally initiated Just Works
 *    is never confirmed, a responder with NoInputNoOutput is not asked, an existing LTK makes Just Works ask.
 *  - Encryption: the central starts it with the STK/LTK (LE Start Encryption); the peripheral answers LTK requests
 *    (pairing key, or a stored key by EDIV/Rand, SC keys by address).
 *  - 30 s SMP timeout, any PDU out of sequence or of the wrong size = Pairing Failed and the pairing ends (the core
 *    disconnects an idle link). Keys reach bluetoothd as NEW_LONG_TERM_KEY / NEW_IRK (bt_mgmt.c). */
#include "bt.h"
#include "../../kernel/string.h"

enum { M_JUST_WORKS = 1, M_JUST_CFM, M_REQ_PASSKEY, M_DSP_PASSKEY, M_OVERLAP };
enum { W_RSP = 1, W_PUBKEY, W_CONFIRM, W_RANDOM, W_DHKEY, W_ENCRYPT, W_KEYS, W_USER };
enum { DIST_ENC = 1, DIST_ID = 2, DIST_SIGN = 4, DIST_LINK = 8 };
struct smp_ctx {
    struct smp_ctx* next; uint32_t conn_id; bool initiator, sc, tk_valid, user_ok, confirm_pending, remote_confirm_seen; uint8_t state, method;
    uint8_t preq[7], pres[7], tk[16], prnd[16], rrnd[16], pcnf[16], rcnf[16], stk[16], ltk[16], mackey[16];
    uint8_t sk[32], pk[64], rpk[64], dhkey[32]; bool have_rpk; uint32_t passkey; unsigned round;
    uint8_t ia[7], ra[7];                     /* LE array order: address (6) then type */
    uint8_t local_dist, remote_dist, got; uint8_t peer_irk[16], id_addr[6], id_type; uint8_t peer_ltk[16], peer_rand[8]; uint16_t peer_ediv; bool got_enc;
    uint8_t key_size; bool authenticated, passkey_entered; uint64_t deadline; uint8_t dhkey_e[16]; bool dhkey_e_seen;
    uint8_t peer_csrk[16], local_csrk[16]; bool sent_csrk;
};
static struct smp_ctx* contexts;
static const uint8_t gen_method[5][5] = {   /* [remote io][local io] */
    {M_JUST_WORKS, M_JUST_CFM, M_REQ_PASSKEY, M_JUST_WORKS, M_REQ_PASSKEY}, {M_JUST_WORKS, M_JUST_CFM, M_REQ_PASSKEY, M_JUST_WORKS, M_REQ_PASSKEY},
    {M_DSP_PASSKEY, M_DSP_PASSKEY, M_REQ_PASSKEY, M_JUST_WORKS, M_DSP_PASSKEY}, {M_JUST_WORKS, M_JUST_CFM, M_JUST_WORKS, M_JUST_WORKS, M_JUST_CFM},
    {M_DSP_PASSKEY, M_DSP_PASSKEY, M_REQ_PASSKEY, M_JUST_WORKS, M_OVERLAP}};
static const uint8_t sc_method[5][5] = {
    {M_JUST_WORKS, M_JUST_CFM, M_REQ_PASSKEY, M_JUST_WORKS, M_REQ_PASSKEY}, {M_JUST_WORKS, M_JUST_CFM, M_REQ_PASSKEY, M_JUST_WORKS, M_REQ_PASSKEY},
    {M_DSP_PASSKEY, M_DSP_PASSKEY, M_REQ_PASSKEY, M_JUST_WORKS, M_DSP_PASSKEY}, {M_JUST_WORKS, M_JUST_CFM, M_JUST_WORKS, M_JUST_WORKS, M_JUST_CFM},
    {M_DSP_PASSKEY, M_DSP_PASSKEY, M_REQ_PASSKEY, M_JUST_WORKS, M_JUST_CFM}};

static void wipe(void* p, unsigned n) { volatile uint8_t* q = p; while (n--) *q++ = 0; }
static struct smp_ctx* ctx_of(struct bt_conn* c) { return c->smp; }
static void ctx_free(struct bt_conn* c) {
    struct smp_ctx* x = c->smp; if (!x) return;
    for (struct smp_ctx** p = &contexts; *p; p = &(*p)->next) if (*p == x) { *p = x->next; break; }
    c->smp = NULL; wipe(x, sizeof(*x)); bt_free(x, sizeof(*x));
}
static void send_pdu(struct bt_conn* c, uint8_t code, const void* data, unsigned length) {
    uint8_t pdu[65]; pdu[0] = code; if (length) memcpy(pdu + 1, data, length); bt_acl_send(c, L2CAP_CID_SMP, pdu, 1 + length);
}
static void finish(struct bt_conn* c, int error) {                         /* pairing ended: users learn the outcome */
    struct smp_ctx* x = ctx_of(c); uint32_t id = c->id;
    if (x && !error) { c->sec_level = x->authenticated ? (x->sc && x->key_size == 16 ? SEC_FIPS : SEC_HIGH) : SEC_MEDIUM; }
    ctx_free(c);
    if (!(c = bt_conn_get(id))) return;
    if (c->pair_op) mgmt_pair_done(c, error ? 0x05 : 0);
    if ((c = bt_conn_get(id))) bt_conn_security_changed(c, error);
}
static void fail(struct bt_conn* c, uint8_t reason) {
    if (!ctx_of(c)) return;
    send_pdu(c, 0x05, &reason, 1);
    if (!c->pair_op) mgmt_auth_failed(c, 0x05);
    finish(c, E_ACCES);
}
static void addresses(struct smp_ctx* x, struct bt_conn* c) {
    uint8_t* local = x->initiator ? x->ia : x->ra; uint8_t* remote = x->initiator ? x->ra : x->ia;
    memcpy(local, c->dev->bdaddr, 6); local[6] = c->own_type; memcpy(remote, c->peer, 6); remote[6] = (uint8_t)(c->peer_type ? c->peer_type - 1 : 0);
}
static struct smp_ctx* ctx_new(struct bt_conn* c, bool initiator) {
    struct smp_ctx* x = bt_zalloc(sizeof(*x)); if (!x) return NULL;
    x->conn_id = c->id; x->initiator = initiator; x->deadline = bt_now() + 30000000000ULL; x->next = contexts; contexts = x; c->smp = x;
    addresses(x, c); return x;
}
static uint8_t local_io(struct bt_conn* c) { return c->io_cap <= 4 ? c->io_cap : 0x03; }
static bool sc_enabled(struct bt_conn* c) { return (c->dev->settings & ST_SC) != 0; }
static uint8_t auth_req(struct bt_conn* c, uint8_t level) {                /* ours as initiator (Linux smp_conn_security) */
    uint8_t a = mgmt_bondable(c->dev) ? 0x01 : 0;
    if (local_io(c) != 0x03 || level >= SEC_HIGH) a |= 0x04;
    if (sc_enabled(c)) a |= (c->dev->settings & ST_SSP) ? 0x28 : 0x08;      /* SC (+ CT2 with SSP) */
    return a;
}
/* The keys either side may send (Linux build_pairing_cmd): LTK and CSRK when bondable, the peer's identity once
 * user space loaded IRKs, the BR/EDR link key derived from an SC LTK (with SSP on). */
static void dist_masks(struct bt_conn* c, bool sc, uint8_t* local, uint8_t* remote) {
    uint8_t l = 0, r = 0;
    if (mgmt_bondable(c->dev)) { l = DIST_ENC | DIST_SIGN; r = DIST_ENC | DIST_SIGN; }
    if (c->dev->rpa_resolving) r |= DIST_ID;
    if (sc && (c->dev->settings & ST_SSP)) { l |= DIST_LINK; r |= DIST_LINK; }
    *local = l; *remote = r;
}
static void build_request(struct bt_conn* c, uint8_t code, uint8_t* pdu, uint8_t auth, uint8_t init_dist, uint8_t resp_dist) {
    pdu[0] = code; pdu[1] = local_io(c); pdu[2] = 0; pdu[3] = auth; pdu[4] = 16; pdu[5] = init_dist; pdu[6] = resp_dist;
}
static uint8_t select_method(struct smp_ctx* x) {
    const uint8_t* local = x->initiator ? x->preq : x->pres; const uint8_t* remote = x->initiator ? x->pres : x->preq;
    uint8_t lio = local[1], rio = remote[1]; bool mitm = (local[3] | remote[3]) & 0x04;
    uint8_t m;
    if (x->sc) m = mitm ? (lio > 4 || rio > 4 ? M_JUST_CFM : sc_method[rio][lio]) : M_JUST_WORKS;
    else m = !mitm ? M_JUST_CFM : (lio > 4 || rio > 4 ? M_JUST_CFM : gen_method[rio][lio]);
    if (m == M_JUST_CFM && x->initiator) m = M_JUST_WORKS;                  /* locally initiated: not confirmed */
    if (m == M_JUST_CFM && lio == 0x03) m = M_JUST_WORKS;                   /* nobody to ask */
    if (m == M_OVERLAP) m = x->initiator ? M_DSP_PASSKEY : M_REQ_PASSKEY;
    return m;
}
static void random16(uint8_t* r) { if (!native_random(r, 16)) { uint64_t t = bt_now(); for (unsigned i = 0; i < 16; i++) r[i] ^= (uint8_t)(t >> (8 * (i & 7))) + (uint8_t)i; } }
/* ---- legacy ---- */
static void legacy_confirm(struct bt_conn* c, struct smp_ctx* x) {
    random16(x->prnd);
    bt_smp_c1(x->tk, x->prnd, x->preq + 0, x->pres + 0, x->ia[6], x->ia, x->ra[6], x->ra, x->pcnf);
    send_pdu(c, 0x03, x->pcnf, 16); x->confirm_pending = false;
}
static void legacy_start_user(struct bt_conn* c, struct smp_ctx* x) {    /* TK: zero (Just Works) or the passkey */
    memset(x->tk, 0, 16);
    switch (x->method) {
    case M_JUST_WORKS: x->tk_valid = true; break;
    case M_JUST_CFM: x->state = W_USER; mgmt_confirm_request(c, 0, true); return;
    case M_REQ_PASSKEY: x->state = W_USER; mgmt_passkey_request(c); return;
    case M_DSP_PASSKEY: { uint32_t v = 0; native_random(&v, 4); x->passkey = v % 1000000U; bt_put32(x->tk, x->passkey); x->tk_valid = true; mgmt_passkey_notify(c, x->passkey); break; }
    }
}
/* ---- secure connections ---- */
static void sc_send_confirm(struct bt_conn* c, struct smp_ctx* x) {       /* Cb (or Ca/Cb in a passkey round) */
    random16(x->prnd);
    uint8_t z = 0;
    if (x->method == M_REQ_PASSKEY || x->method == M_DSP_PASSKEY) z = (uint8_t)(0x80 | ((x->passkey >> x->round) & 1));
    bt_smp_f4(x->pk, x->rpk, x->prnd, z, x->pcnf);
    send_pdu(c, 0x03, x->pcnf, 16);
}
static bool sc_check_confirm(struct smp_ctx* x) {
    uint8_t z = 0, expect[16];
    if (x->method == M_REQ_PASSKEY || x->method == M_DSP_PASSKEY) z = (uint8_t)(0x80 | ((x->passkey >> x->round) & 1));
    bt_smp_f4(x->rpk, x->pk, x->rrnd, z, expect);
    return !memcmp(expect, x->rcnf, 16);
}
static void sc_keys(struct smp_ctx* x) {
    const uint8_t* na = x->initiator ? x->prnd : x->rrnd; const uint8_t* nb = x->initiator ? x->rrnd : x->prnd;
    bt_smp_f5(x->dhkey, na, nb, x->ia, x->ra, x->mackey, x->ltk);
}
static void sc_dhkey_value(struct smp_ctx* x, bool local, uint8_t* out) {
    uint8_t r[16] = {0}; if (x->method == M_REQ_PASSKEY || x->method == M_DSP_PASSKEY) bt_put32(r, x->passkey);
    bool init_side = local ? x->initiator : !x->initiator;                  /* whose check value */
    const uint8_t* io = init_side ? x->preq + 1 : x->pres + 1;
    const uint8_t* n1 = local ? x->prnd : x->rrnd; const uint8_t* n2 = local ? x->rrnd : x->prnd;
    bt_smp_f6(x->mackey, n1, n2, r, io, init_side ? x->ia : x->ra, init_side ? x->ra : x->ia, out);
}
static void start_encryption_or_wait(struct bt_conn* c, struct smp_ctx* x, const uint8_t* key) {
    if (x->initiator) {
        uint8_t k[16]; memcpy(k, key, 16); if (x->key_size < 16) memset(k + x->key_size, 0, 16 - x->key_size);
        uint8_t zero[8] = {0}; x->state = W_ENCRYPT; if (!bt_le_encrypt(c, k, 0, zero, x->key_size)) fail(c, 0x08); wipe(k, 16);
    } else x->state = W_ENCRYPT;                                            /* the central's LTK request follows */
}
static void sc_send_dhkey_check(struct bt_conn* c, struct smp_ctx* x) { uint8_t e[16]; sc_dhkey_value(x, true, e); send_pdu(c, 0x0D, e, 16); }
static void sc_after_random(struct bt_conn* c, struct smp_ctx* x) {      /* nonces known: derive keys, user step, check */
    if ((x->method == M_REQ_PASSKEY || x->method == M_DSP_PASSKEY) && ++x->round < 20) {
        x->state = W_CONFIRM; x->remote_confirm_seen = false;
        if (x->initiator) sc_send_confirm(c, x);
        return;
    }
    sc_keys(x);
    if (x->method == M_JUST_CFM && !x->user_ok) {
        const uint8_t* na = x->initiator ? x->prnd : x->rrnd; const uint8_t* nb = x->initiator ? x->rrnd : x->prnd;
        const uint8_t* pka = x->initiator ? x->pk : x->rpk; const uint8_t* pkb = x->initiator ? x->rpk : x->pk;
        x->state = W_USER; mgmt_confirm_request(c, bt_smp_g2(pka, pkb, na, nb), false); return;
    }
    if (x->method == M_JUST_WORKS && !x->user_ok && !x->initiator && mgmt_find_ltk(c->dev, c->peer, c->peer_type, false) && local_io(c) != 0x03) {
        x->state = W_USER; mgmt_confirm_request(c, 0, true); return;        /* re-pairing over an existing key: ask */
    }
    x->state = W_DHKEY;
    if (x->initiator) sc_send_dhkey_check(c, x);
    else if (x->dhkey_e_seen) {                                             /* the initiator's check arrived during the user step */
        uint8_t expect[16]; sc_dhkey_value(x, false, expect);
        if (memcmp(expect, x->dhkey_e, 16)) { fail(c, 0x0B); return; }
        sc_send_dhkey_check(c, x); start_encryption_or_wait(c, x, x->ltk);
    }
}
static void sc_public_key_done(struct bt_conn* c, struct smp_ctx* x) {
    if (!bt_p256_dhkey(x->sk, x->rpk, x->dhkey)) { fail(c, 0x0B); return; }
    x->round = 0;
    if (x->method == M_REQ_PASSKEY) { x->state = W_USER; mgmt_passkey_request(c); return; }
    if (x->method == M_DSP_PASSKEY) { uint32_t v = 0; native_random(&v, 4); x->passkey = v % 1000000U; mgmt_passkey_notify(c, x->passkey); }
    x->state = W_CONFIRM;
    if (x->method == M_JUST_WORKS || x->method == M_JUST_CFM) { if (!x->initiator) sc_send_confirm(c, x); return; }
    if (x->initiator) sc_send_confirm(c, x);                                /* passkey rounds: the initiator starts */
}
static bool sc_keypair(struct smp_ctx* x, struct bt_conn* c) {
    if (c->dev->settings & ST_DEBUG_KEYS) {                                 /* the specification's debug key pair */
        static const uint8_t debug_sk[32] = {0xbd, 0x1a, 0x3c, 0xcd, 0xa6, 0xb8, 0x99, 0x58, 0x99, 0xb7, 0x40, 0xeb, 0x7b, 0x60, 0xff, 0x4a,
                                             0x50, 0x3f, 0x10, 0xd2, 0xe3, 0xb3, 0xc9, 0x74, 0x38, 0x5f, 0xc5, 0xa3, 0xd4, 0xf6, 0x49, 0x3f};
        memcpy(x->sk, debug_sk, 32); return bt_p256_public(x->sk, x->pk);
    }
    return bt_p256_keypair(x->sk, x->pk);
}
/* ---- key distribution ---- */
static void distribute_local(struct bt_conn* c, struct smp_ctx* x) {
    uint8_t dist = x->initiator ? x->preq[5] & x->pres[5] : x->pres[6];   /* what the response agreed we send */
    if (x->sc) dist &= (uint8_t)~(DIST_ENC | DIST_LINK);                   /* SC: derived, never sent */
    if (dist & DIST_ENC) {
        uint8_t ltk[16], rand[8]; uint16_t ediv; random16(ltk); if (x->key_size < 16) memset(ltk + x->key_size, 0, 16 - x->key_size);
        native_random(rand, 8); native_random(&ediv, 2);
        send_pdu(c, 0x06, ltk, 16); uint8_t ident[10]; bt_put16(ident, ediv); memcpy(ident + 2, rand, 8); send_pdu(c, 0x07, ident, 10);
        mgmt_new_ltk(c, ltk, x->key_size, x->authenticated, false, ediv, rand, false);
        wipe(ltk, 16);
    }
    /* no local IRK (privacy is not offered): identity is the public address, DIST_ID is never agreed for us */
    if (dist & DIST_SIGN) { random16(x->local_csrk); send_pdu(c, 0x0A, x->local_csrk, 16); x->sent_csrk = true; }
}
static uint8_t expected_remote(struct smp_ctx* x) {
    uint8_t d = x->initiator ? x->pres[6] : x->pres[5]; if (x->sc) d &= (uint8_t)~(DIST_ENC | DIST_LINK);
    return d & (DIST_ENC | DIST_ID | DIST_SIGN);
}
/* SC with the link-key bit: the BR/EDR key of the same peer (Linux sc_generate_link_key: ILK = h7(salt "tmp1", LTK)
 * with CT2, else h6(LTK, "tmp1"); key = h6(ILK, "lebr")), reported as NEW_LINK_KEY for its identity address. */
static void sc_link_key(struct bt_conn* c, struct smp_ctx* x) {
    static const uint8_t tmp1[16] = {0x31, 0x70, 0x6d, 0x74}, lebr[4] = {0x72, 0x62, 0x65, 0x6c};
    uint8_t ilk[16], key[16]; bool ct2 = (x->preq[3] & x->pres[3] & 0x20) != 0;
    if (ct2) bt_smp_h7(tmp1, x->ltk, ilk); else bt_smp_h6(x->ltk, tmp1, ilk);
    bt_smp_h6(ilk, lebr, key);
    uint8_t type = (c->dev->settings & ST_DEBUG_KEYS) ? 0x03 : x->authenticated && x->key_size == 16 ? 0x08 : 0x07;
    mgmt_new_link_key_le(c, (x->got & DIST_ID) ? x->id_addr : c->peer, key, type);
    wipe(ilk, 16); wipe(key, 16);
}
static void keys_maybe_done(struct bt_conn* c, struct smp_ctx* x) {
    uint8_t want = expected_remote(x);
    if ((x->got & want) != want) return;
    if (x->initiator) distribute_local(c, x);
    if (x->sc) mgmt_new_ltk(c, x->ltk, x->key_size, x->authenticated, false, 0, NULL, true);
    else if (x->got_enc) mgmt_new_ltk(c, x->peer_ltk, x->key_size, x->authenticated, true, x->peer_ediv, x->peer_rand, false);   /* the peer's key: used when we are central */
    if (x->got & DIST_ID) mgmt_new_irk(c, x->peer_irk, x->id_addr, x->id_type);
    if (x->got & DIST_SIGN) mgmt_new_csrk(c, x->peer_csrk, x->authenticated ? 3 : 1);
    if (x->sent_csrk) mgmt_new_csrk(c, x->local_csrk, x->authenticated ? 2 : 0);
    if (x->sc && ((x->initiator ? x->pres[5] : x->pres[6]) & DIST_LINK)) sc_link_key(c, x);
    finish(c, 0);
}
void smp_encrypt_changed(struct bt_conn* c, int status) {
    struct smp_ctx* x = ctx_of(c);
    if (!x) {                                                               /* re-encryption with a stored key */
        if (!status && c->encrypted) {
            struct bt_key* k = mgmt_find_ltk(c->dev, c->peer, c->peer_type, c->role == 0);
            if (!k) for (unsigned i = 0; i < 32 && !k; i++) if (c->dev->keys[i].used && c->dev->keys[i].type && !memcmp(c->dev->keys[i].addr, c->peer, 6)) k = &c->dev->keys[i];
            c->sec_level = k && k->authenticated ? SEC_HIGH : SEC_MEDIUM;
        }
        bt_conn_security_changed(c, status ? bt_hci_errno(status) : 0); return;
    }
    if (x->state != W_ENCRYPT) return;
    if (status || !c->encrypted) { fail(c, 0x08); return; }
    x->state = W_KEYS; c->enc_key_size = x->key_size;
    c->bonding = ((x->preq[3] & x->pres[3] & 0x01) != 0) || c->pair_op;    /* store hint of the key events */
    if (!x->initiator) distribute_local(c, x);
    keys_maybe_done(c, x);
}
bool bt_le_encrypt(struct bt_conn* c, const uint8_t* ltk, uint16_t ediv, const uint8_t* rand, uint8_t key_size) {
    uint8_t p[28]; bt_put16(p, c->handle); memcpy(p + 2, rand, 8); bt_put16(p + 10, ediv); memcpy(p + 12, ltk, 16);
    (void)key_size; c->enc_pending = bt_cmd_async(c->dev, 0x2019, p, 28, NULL, NULL, 0); wipe(p, sizeof(p)); return c->enc_pending;
}
void smp_ltk_request(struct bt_conn* c, uint16_t ediv, const uint8_t* rand) {
    struct smp_ctx* x = ctx_of(c); uint8_t p[18]; bt_put16(p, c->handle); static const uint8_t zero[8] = {0};
    if (x && x->state == W_ENCRYPT && !ediv && !memcmp(rand, zero, 8)) {
        const uint8_t* key = x->sc ? x->ltk : x->stk; memcpy(p + 2, key, 16); if (x->key_size < 16) memset(p + 2 + x->key_size, 0, 16 - x->key_size);
        bt_cmd_async(c->dev, 0x201A, p, 18, NULL, NULL, 0); wipe(p, sizeof(p)); return;
    }
    struct bt_key* k = (!ediv && !memcmp(rand, zero, 8)) ? NULL : mgmt_find_ltk_rand(c->dev, ediv, rand);
    if (!k && !ediv && !memcmp(rand, zero, 8))                              /* SC keys: by address */
        for (unsigned i = 0; i < 32 && !k; i++) { struct bt_key* q = &c->dev->keys[i]; if (q->used && q->type == c->peer_type && q->key_type >= 2 && !memcmp(q->addr, c->peer, 6)) k = q; }
    if (k && !memcmp(k->addr, c->peer, 6)) { memcpy(p + 2, k->val, 16); c->enc_key_size = k->enc_size; bt_cmd_async(c->dev, 0x201A, p, 18, NULL, NULL, 0); wipe(p, sizeof(p)); return; }
    bt_cmd_async(c->dev, 0x201B, p, 2, NULL, NULL, 0);
}
/* ---- entry points ---- */
int smp_start(struct bt_conn* c, uint8_t level) {
    if (c->link != LINK_LE || c->state != CONN_CONNECTED) return E_NOTCONN;
    if (ctx_of(c)) return 0;                                                /* already pairing */
    if (c->role == 1) { uint8_t a = auth_req(c, level); send_pdu(c, 0x0B, &a, 1); return 0; }   /* peripheral: Security Request */
    struct smp_ctx* x = ctx_new(c, true); if (!x) return E_NOMEM;
    uint8_t auth = auth_req(c, level), local, remote; dist_masks(c, (auth & 0x08) != 0, &local, &remote);
    build_request(c, 0x01, x->preq, auth, local, remote);
    x->state = W_RSP; send_pdu(c, 0x01, x->preq + 1, 6); return 0;
}
static bool pairing_params_valid(const uint8_t* p) { return p[1] <= 4 && p[2] <= 1; }   /* key size: checked after (0x06, as Linux) */
void smp_receive(struct bt_conn* c, const uint8_t* pdu, unsigned length) {
    if (!length) return;
    static const uint8_t sizes[] = {0, 7, 7, 17, 17, 2, 17, 11, 17, 8, 17, 2, 65, 17, 2};
    uint8_t code = pdu[0]; struct smp_ctx* x = ctx_of(c); uint32_t id = c->id;
    if (code == 0 || code >= sizeof(sizes)) { uint8_t r = 0x07; send_pdu(c, 0x05, &r, 1); if (x) finish(c, E_ACCES); return; }   /* Command Not Supported */
    if (length != sizes[code]) { uint8_t r = 0x0A; send_pdu(c, 0x05, &r, 1); if (x) { if (!c->pair_op) mgmt_auth_failed(c, 0x05); finish(c, E_ACCES); } return; }
    const uint8_t* q = pdu + 1;
    switch (code) {
    case 0x01: {                                                           /* Pairing Request (we are the responder) */
        if (c->role != 1) { uint8_t r = 0x07; send_pdu(c, 0x05, &r, 1); return; }
        if (x && x->state != W_RSP) { fail(c, 0x08); return; }
        if (!pairing_params_valid(pdu)) { uint8_t r = 0x0A; send_pdu(c, 0x05, &r, 1); return; }
        if (!mgmt_bondable(c->dev) && (q[2] & 0x03)) { uint8_t r = 0x05; send_pdu(c, 0x05, &r, 1); mgmt_auth_failed(c, 0x05); return; }
        if (c->dev->sc_only && !(q[2] & 0x08)) { uint8_t r = 0x03; send_pdu(c, 0x05, &r, 1); return; }
        if (!x && !(x = ctx_new(c, false))) return;
        memcpy(x->preq, pdu, 7);
        /* Linux: the responder matches the initiator's AuthReq (masked to what is enabled), bonding as bondable */
        uint8_t auth = (uint8_t)(q[2] & (sc_enabled(c) ? 0x3F : 0x07));
        if (mgmt_bondable(c->dev)) auth |= 0x01; else auth &= (uint8_t)~0x01;
        if (!sc_enabled(c) || !(auth & 0x08)) auth &= (uint8_t)~0x08;
        uint8_t local, remote; dist_masks(c, (auth & 0x08) != 0, &local, &remote);
        build_request(c, 0x02, x->pres, auth, q[4] & remote, q[5] & local);
        x->sc = (auth & 0x08) != 0; x->key_size = q[3] < 16 ? q[3] : 16;
        if (q[3] < 7 || q[3] > 16) { fail(c, 0x06); return; }
        x->method = select_method(x); x->authenticated = x->method != M_JUST_WORKS && x->method != M_JUST_CFM;
        if (c->pending_sec >= SEC_HIGH && !x->authenticated) { fail(c, 0x03); return; }
        send_pdu(c, 0x02, x->pres + 1, 6);
        if (x->sc) { if (!sc_keypair(x, c)) { fail(c, 0x08); return; } x->state = W_PUBKEY; return; }
        x->state = W_CONFIRM; legacy_start_user(c, x); return;
    }
    case 0x02: {                                                           /* Pairing Response */
        if (!x || !x->initiator || x->state != W_RSP) { if (x) fail(c, 0x08); return; }
        if (!pairing_params_valid(pdu)) { fail(c, 0x0A); return; }
        memcpy(x->pres, pdu, 7);
        x->sc = (x->preq[3] & 0x08) && (q[2] & 0x08); x->key_size = q[3] < x->preq[4] ? q[3] : x->preq[4];
        if (x->key_size < 7) { fail(c, 0x06); return; }
        if ((q[4] & ~x->preq[5]) || (q[5] & ~x->preq[6])) { fail(c, 0x0A); return; }   /* more keys than offered */
        x->method = select_method(x); x->authenticated = x->method != M_JUST_WORKS && x->method != M_JUST_CFM;
        if (c->pending_sec >= SEC_HIGH && !x->authenticated) { fail(c, 0x03); return; }
        if (x->sc) { if (!sc_keypair(x, c)) { fail(c, 0x08); return; } x->state = W_PUBKEY; send_pdu(c, 0x0C, x->pk, 64); return; }
        x->state = W_CONFIRM; legacy_start_user(c, x);
        if ((x = ctx_of(c)) && x->tk_valid) legacy_confirm(c, x);
        return;
    }
    case 0x0C: {                                                           /* Pairing Public Key */
        if (!x || !x->sc || x->state != W_PUBKEY) { if (x) fail(c, 0x08); return; }
        memcpy(x->rpk, q, 64);
        if (!memcmp(x->rpk, x->pk, 64) || !bt_p256_valid(x->rpk)) { fail(c, 0x0B); return; }   /* reflected or invalid key */
        if (!x->initiator) send_pdu(c, 0x0C, x->pk, 64);
        sc_public_key_done(c, x); return;
    }
    case 0x03: {                                                           /* Pairing Confirm */
        if (!x || (x->state != W_CONFIRM && !(x->state == W_USER && !x->initiator))) { if (x) fail(c, 0x08); return; }
        memcpy(x->rcnf, q, 16); x->remote_confirm_seen = true;
        if (x->state == W_USER) { x->confirm_pending = true; return; }     /* answered after the user step */
        if (x->sc) {
            if (x->initiator) { x->state = W_RANDOM; send_pdu(c, 0x04, x->prnd, 16); return; }
            if (x->method == M_JUST_WORKS || x->method == M_JUST_CFM) { fail(c, 0x08); return; }   /* the responder sends Cb in these */
            sc_send_confirm(c, x); x->state = W_RANDOM; return;
        }
        if (x->initiator) { x->state = W_RANDOM; send_pdu(c, 0x04, x->prnd, 16); return; }
        if (x->tk_valid) { legacy_confirm(c, x); x->state = W_RANDOM; } else x->confirm_pending = true;
        return;
    }
    case 0x04: {                                                           /* Pairing Random */
        if (!x || (x->state != W_RANDOM && !(x->sc && !x->initiator && x->state == W_CONFIRM && (x->method == M_JUST_WORKS || x->method == M_JUST_CFM)))) { if (x) fail(c, 0x08); return; }
        memcpy(x->rrnd, q, 16);
        if (x->sc) {
            if (x->initiator) { if (!sc_check_confirm(x)) { fail(c, 0x04); return; } sc_after_random(c, x); return; }
            if (x->method != M_JUST_WORKS && x->method != M_JUST_CFM && !sc_check_confirm(x)) { fail(c, 0x04); return; }
            send_pdu(c, 0x04, x->prnd, 16);
            if (!(x = ctx_of(c))) return;
            sc_after_random(c, x); return;
        }
        uint8_t expect[16];
        bt_smp_c1(x->tk, x->rrnd, x->preq, x->pres, x->ia[6], x->ia, x->ra[6], x->ra, expect);
        if (memcmp(expect, x->rcnf, 16)) { fail(c, 0x04); return; }
        const uint8_t* mrand = x->initiator ? x->prnd : x->rrnd; const uint8_t* srand = x->initiator ? x->rrnd : x->prnd;
        bt_smp_s1(x->tk, srand, mrand, x->stk);
        if (!x->initiator) send_pdu(c, 0x04, x->prnd, 16);
        if ((x = ctx_of(c))) start_encryption_or_wait(c, x, x->stk);
        return;
    }
    case 0x0D: {                                                           /* Pairing DHKey Check */
        if (!x || !x->sc) { if (x) fail(c, 0x08); return; }
        if (!x->initiator && x->state == W_USER) { memcpy(x->dhkey_e, q, 16); x->dhkey_e_seen = true; return; }
        if (x->state != W_DHKEY) { fail(c, 0x08); return; }
        uint8_t expect[16]; sc_dhkey_value(x, false, expect);
        if (memcmp(expect, q, 16)) { fail(c, 0x0B); return; }
        if (x->initiator) { start_encryption_or_wait(c, x, x->ltk); return; }
        sc_send_dhkey_check(c, x); start_encryption_or_wait(c, x, x->ltk); return;
    }
    case 0x05:                                                             /* Pairing Failed */
        if (x) { if (!c->pair_op) mgmt_auth_failed(c, 0x05); finish(c, E_ACCES); }
        return;
    case 0x06: if (!x || x->state != W_KEYS || !(expected_remote(x) & DIST_ENC) || (x->got & 0x80)) { if (x) fail(c, 0x08); return; } memcpy(x->peer_ltk, q, 16); x->got |= 0x80; return;
    case 0x07:
        if (!x || x->state != W_KEYS || !(x->got & 0x80) || (x->got & DIST_ENC)) { if (x) fail(c, 0x08); return; }
        x->peer_ediv = (uint16_t)bt_get16(q); memcpy(x->peer_rand, q + 2, 8); x->got |= DIST_ENC; x->got_enc = true; keys_maybe_done(c, x); return;
    case 0x08: if (!x || x->state != W_KEYS || !(expected_remote(x) & DIST_ID) || (x->got & 0x40)) { if (x) fail(c, 0x08); return; } memcpy(x->peer_irk, q, 16); x->got |= 0x40; return;
    case 0x09:
        if (!x || x->state != W_KEYS || !(x->got & 0x40) || (x->got & DIST_ID) || q[0] > 1) { if (x) fail(c, 0x08); return; }
        x->id_type = (uint8_t)(q[0] + 1); memcpy(x->id_addr, q + 1, 6); x->got |= DIST_ID; keys_maybe_done(c, x); return;
    case 0x0A: {                                                           /* Signing Information (CSRK): after LTK and identity */
        uint8_t want = x ? expected_remote(x) : 0, before = want & (DIST_ENC | DIST_ID);
        if (!x || x->state != W_KEYS || !(want & DIST_SIGN) || (x->got & DIST_SIGN) || (x->got & before) != before) { if (x) fail(c, 0x08); return; }
        memcpy(x->peer_csrk, q, 16); x->got |= DIST_SIGN; keys_maybe_done(c, x); return;
    }
    case 0x0B:                                                             /* Security Request (we are central) */
        if (c->role != 0) { uint8_t r = 0x07; send_pdu(c, 0x05, &r, 1); return; }   /* only a peripheral may ask (Linux) */
        if (x) return;
        { struct bt_key* k = mgmt_find_ltk(c->dev, c->peer, c->peer_type, true);
          if (!k) for (unsigned i = 0; i < 32 && !k; i++) { struct bt_key* q2 = &c->dev->keys[i]; if (q2->used && q2->type == c->peer_type && q2->key_type >= 2 && !memcmp(q2->addr, c->peer, 6)) k = q2; }
          if (k && (!(q[0] & 0x04) || k->authenticated)) { bt_le_encrypt(c, k->val, k->ediv, k->rand, k->enc_size); return; } }
        if (c->pending_sec < SEC_MEDIUM) c->pending_sec = SEC_MEDIUM;
        smp_start(c, (q[0] & 0x04) ? SEC_HIGH : SEC_MEDIUM); return;
    case 0x0E: return;                                                     /* Keypress Notification */
    }
    (void)id;
}
int smp_user_reply(struct bt_conn* c, unsigned opcode, uint32_t passkey) {
    struct smp_ctx* x = ctx_of(c);
    if (!x || x->state != W_USER) return E_INVAL;
    if (opcode == 0x001D || opcode == 0x001F) { fail(c, opcode == 0x001D ? 0x0C : 0x01); return 0; }
    if (opcode == 0x001E) { if (passkey > 999999) { fail(c, 0x01); return 0; } x->passkey = passkey; }
    x->user_ok = true;
    if (!x->sc) {
        if (opcode == 0x001E) bt_put32(x->tk, passkey);
        x->tk_valid = true; x->state = W_CONFIRM;
        if (x->initiator) legacy_confirm(c, x);
        else if (x->confirm_pending) { legacy_confirm(c, x); x->state = W_RANDOM; }
        return 0;
    }
    if (x->method == M_REQ_PASSKEY && !x->passkey_entered) {               /* passkey known: the 20 rounds begin */
        x->passkey_entered = true; x->state = W_CONFIRM;
        if (x->initiator) sc_send_confirm(c, x);
        else if (x->remote_confirm_seen) { sc_send_confirm(c, x); x->state = W_RANDOM; }
        return 0;
    }
    sc_after_random(c, x); return 0;
}
void smp_conn_closed(struct bt_conn* c) { if (ctx_of(c)) { if (c->pair_op) mgmt_pair_done(c, 0x0E); ctx_free(c); } }
void smp_poll(uint64_t now) {
    for (struct smp_ctx *x = contexts, *next; x; x = next) {
        next = x->next;
        if (now < x->deadline) continue;
        struct bt_conn* c = bt_conn_get(x->conn_id);
        if (!c) { for (struct smp_ctx** p = &contexts; *p; p = &(*p)->next) if (*p == x) { *p = x->next; break; } wipe(x, sizeof(*x)); bt_free(x, sizeof(*x)); next = contexts; continue; }
        if (!c->pair_op) mgmt_auth_failed(c, 0x08);
        finish(c, E_TIMEDOUT); bt_disconnect(c, 0x13); next = contexts;   /* the specification: no more SMP on this link */
    }
}
uint64_t smp_next_event(uint64_t now) { (void)now; uint64_t next = ~0ULL; for (struct smp_ctx* x = contexts; x; x = x->next) if (x->deadline < next) next = x->deadline; return next; }
