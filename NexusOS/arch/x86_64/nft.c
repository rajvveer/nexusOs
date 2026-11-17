/* nf_tables: the modern Linux firewall, driven by Debian's `nft` over NETLINK_NETFILTER (nfnetlink). Enough of the
 * protocol for `nft -f`, `nft list ruleset`, `nft flush ruleset` and the rule sets distributions and firewall
 * managers write: tables, base chains of type filter, nat and route (hooks input/forward/output/prerouting/
 * postrouting, priorities, policies) and regular chains; expressions payload (load and mangle with checksum update),
 * meta and ct (load and set: mark, priority, ct mark), cmp, range, bitwise, byteorder, immediate, counter, quota,
 * limit (token bucket), lookup (sets, maps, verdict maps), dynset (add/update/delete from the packet path), objref,
 * numgen, fib (route lookups: rpfilter, address types), rt (TCP MSS), exthdr (IPv6 extension headers, TCP options
 * incl. MSS clamping), log, reject, notrack and the NAT statements snat / dnat / masquerade / redirect (conntrack.c);
 * sets with plain, interval (rbtree boundaries) and range (pipapo key/key_end) elements, concatenated keys, maps and
 * verdict maps, element and set timeouts; named counter, quota and limit objects; comments (userdata). Messages arrive in a transaction (NFNL_MSG_BATCH_BEGIN..END): the whole
 * batch is parsed into a staging copy and committed only if every message validates, so a bad rule set leaves the
 * live one untouched (nf_tables' commit/abort). The base chains register the netfilter hooks; nf_hook() walks the
 * matching chains in priority order and returns NF_ACCEPT / NF_DROP (a reject verdict sends its ICMP or TCP reset).
 * Rules are kept as the exact expression bytes `nft` sent, so `nft list ruleset` reproduces what was loaded. Runs
 * under the kernel lock, like every socket call and every hook. Enforcement is covered by tests/test_x64_firewall.py. */
#include "nlmsg.h"
#include "../../kernel/ip.h"
#include "../../kernel/string.h"

/* ---- nfnetlink / nf_tables constants ---- */
#define NFNL_SUBSYS_NFTABLES 10
#define NFNL_MSG_BATCH_BEGIN 16
#define NFNL_MSG_BATCH_END 17
#define NFT_MSG_NEWTABLE 0
#define NFT_MSG_GETTABLE 1
#define NFT_MSG_DELTABLE 2
#define NFT_MSG_NEWCHAIN 3
#define NFT_MSG_GETCHAIN 4
#define NFT_MSG_DELCHAIN 5
#define NFT_MSG_NEWRULE 6
#define NFT_MSG_GETRULE 7
#define NFT_MSG_DELRULE 8
#define NFT_MSG_NEWSET 9
#define NFT_MSG_GETSET 10
#define NFT_MSG_DELSET 11
#define NFT_MSG_NEWSETELEM 12
#define NFT_MSG_GETSETELEM 13
#define NFT_MSG_DELSETELEM 14
#define NFT_MSG_NEWGEN 15
#define NFT_MSG_GETGEN 16
#define NFT_MSG_NEWOBJ 18
#define NFT_MSG_GETOBJ 19
#define NFT_MSG_DELOBJ 20
#define NFT_MSG_GETOBJ_RESET 21
#define NFT_MSG_NEWFLOWTABLE 22
#define NFT_MSG_GETFLOWTABLE 23
#define NFT_MSG_DELFLOWTABLE 24
#define NFT_MSG_GETRULE_RESET 25
#define NFT_MSG_DESTROYTABLE 26
#define NFT_MSG_DESTROYCHAIN 27
#define NFT_MSG_DESTROYRULE 28
#define NFT_MSG_DESTROYSET 29
#define NFT_MSG_DESTROYSETELEM 30
#define NFT_MSG_DESTROYOBJ 31
#define NFT_MSG_DESTROYFLOWTABLE 32
#define NFT_MSG_GETSETELEM_RESET 33

#define NFTA_TABLE_NAME 1
#define NFTA_TABLE_FLAGS 2
#define NFTA_TABLE_USE 3
#define NFTA_TABLE_HANDLE 4
#define NFTA_CHAIN_TABLE 1
#define NFTA_CHAIN_HANDLE 2
#define NFTA_CHAIN_NAME 3
#define NFTA_CHAIN_HOOK 4
#define NFTA_CHAIN_POLICY 5
#define NFTA_CHAIN_USE 6
#define NFTA_CHAIN_TYPE 7
#define NFTA_CHAIN_COUNTERS 8
#define NFTA_CHAIN_FLAGS 10
#define NFTA_HOOK_HOOKNUM 1
#define NFTA_HOOK_PRIORITY 2
#define NFTA_HOOK_DEV 3
#define NFTA_RULE_TABLE 1
#define NFTA_RULE_CHAIN 2
#define NFTA_RULE_HANDLE 3
#define NFTA_RULE_EXPRESSIONS 4
#define NFTA_RULE_POSITION 6
#define NFTA_RULE_ID 8
#define NFTA_EXPR_NAME 1
#define NFTA_EXPR_DATA 2
#define NFTA_SET_TABLE 1
#define NFTA_SET_NAME 2
#define NFTA_SET_FLAGS 3
#define NFTA_SET_KEY_TYPE 4
#define NFTA_SET_KEY_LEN 5
#define NFTA_SET_DATA_TYPE 6
#define NFTA_SET_DATA_LEN 7
#define NFTA_SET_ID 10
#define NFTA_SET_ELEM_LIST_TABLE 1
#define NFTA_SET_ELEM_LIST_SET 2
#define NFTA_SET_ELEM_LIST_ELEMENTS 3
#define NFTA_SET_ELEM_KEY 1
#define NFTA_SET_ELEM_DATA 2
#define NFTA_DATA_VALUE 1
#define NFTA_DATA_VERDICT 2
#define NFTA_VERDICT_CODE 1
#define NFTA_VERDICT_CHAIN 2
#define NFTA_OBJ_TABLE 1
#define NFTA_OBJ_NAME 2
#define NFTA_OBJ_TYPE 3
#define NFTA_OBJ_DATA 4
#define NFTA_OBJ_USE 5
#define NFTA_OBJ_HANDLE 6
#define NFT_OBJECT_COUNTER 1
#define NFTA_COUNTER_BYTES 1
#define NFTA_COUNTER_PACKETS 2
#define NFTA_LIST_ELEM 1
#define NFTA_GEN_ID 1

/* verdicts (stored big-endian in NFTA_VERDICT_CODE) */
#define NFT_CONTINUE (-1)
#define NFT_BREAK (-2)
#define NFT_JUMP (-3)
#define NFT_GOTO (-4)
#define NFT_RETURN (-5)

#define TABLE_MAX 16
#define CHAIN_MAX 96
#define RULE_MAX 640
#define SET_MAX 96
#define OBJ_MAX 64
#define ELEM_MAX 2048
#define EXPR_BYTES 1024
#define KEY_BYTES 64
#define USERDATA_BYTES 128
#define RULE_STATES 4
#define NFT_DATA_VERDICT_TYPE 0xffffff00U
#define NFT_SET_ANONYMOUS 0x1
#define NFT_SET_INTERVAL 0x4
#define NFT_SET_MAP 0x8
#define NFT_SET_TIMEOUT 0x10
#define NFT_SET_EVAL 0x20
#define NFT_SET_OBJECT 0x40
#define NFT_SET_CONCAT 0x80
#define NFT_SET_EXPR 0x100
#define NFT_SET_ELEM_INTERVAL_END 0x1
#define NFT_OBJECT_QUOTA 2
#define NFT_OBJECT_CT_HELPER 3
#define NFT_OBJECT_LIMIT 4

struct userdata { uint8_t bytes[USERDATA_BYTES]; uint16_t length; };
struct expr_state { uint32_t offset; uint64_t tokens, last_ns, consumed; uint32_t sequence; };   /* limit / quota / numgen */
struct nft_rule {
    bool used; unsigned table, chain; uint64_t handle;
    int64_t position;                /* order within the chain (append/insert/position); `order` below sorts by it */
    uint8_t expr[EXPR_BYTES]; unsigned expr_len;
    uint64_t packets, bytes;         /* a `counter` expression accumulates here (by the rule's first counter) */
    struct expr_state state[RULE_STATES]; unsigned states;
    struct userdata user;            /* NFTA_RULE_USERDATA (comment) */
};
struct nft_elem {
    bool used; bool has_end; unsigned set; uint32_t flags;    /* flags: NFT_SET_ELEM_INTERVAL_END (an interval boundary) */
    uint8_t key[KEY_BYTES], key_end[KEY_BYTES], data[KEY_BYTES]; unsigned key_len, data_len;
    char chain[32];                  /* a verdict map element: jump/goto target (verdict code in data) */
    uint64_t timeout_ms, expires_ns; /* 0: no timeout */
    struct userdata user;
    uint8_t expr[96]; uint16_t expr_len;   /* per-element expressions (a meter: dynset ... { key limit/counter/quota }) */
    uint64_t packets, bytes, tokens, last_ns, consumed;
};
struct nft_set {
    bool used; unsigned table; char name[32]; uint64_t handle;
    uint32_t flags, key_type, data_type, policy, objtype; unsigned key_len, data_len; uint64_t id, timeout_ms; uint32_t gc_interval;
    uint8_t fields[16]; unsigned field_count;   /* NFTA_SET_DESC_CONCAT field lengths */
    uint32_t size;
    bool fresh;                      /* created in the open batch: its NFTA_SET_ID is valid (anonymous sets) */
    struct userdata user;
};
struct nft_chain {
    bool used; unsigned table; char name[32]; uint64_t handle;
    bool base; uint32_t hooknum; int32_t priority; uint8_t policy; char type[16]; uint32_t flags;
    struct userdata user;
};
struct nft_table { bool used; uint8_t family; char name[32]; uint32_t flags; uint64_t handle; struct userdata user; };
struct nft_obj {
    bool used; unsigned table; char name[32]; uint64_t handle; uint32_t type;
    uint64_t packets, bytes;                                   /* counter */
    uint64_t quota, consumed; uint32_t quota_flags;            /* quota */
    uint64_t rate, unit; uint32_t burst, limit_type, limit_flags; uint64_t tokens, last_ns;   /* limit */
    char helper[16]; uint16_t helper_l3; uint8_t helper_l4;   /* ct helper */
    struct userdata user;
};

struct ruleset {
    struct nft_table tables[TABLE_MAX];
    struct nft_chain chains[CHAIN_MAX];
    struct nft_rule rules[RULE_MAX];
    struct nft_set sets[SET_MAX];
    struct nft_elem elems[ELEM_MAX];
    struct nft_obj objects[OBJ_MAX];
    uint64_t next_handle;
    uint16_t order[RULE_MAX]; unsigned ordered;   /* used rules sorted by (chain, position), rebuilt at commit */
};
static struct ruleset* live;         /* committed */
static struct ruleset* work;         /* staging during a batch */
static uint32_t generation = 1;
static bool any_base;                /* cached: does `live` have a base chain (nf_active) */
static bool batch_failed;            /* a message of the open batch failed: the batch must not commit */
static const void* batch_owner;      /* the netlink socket whose batch `work` belongs to */

static void userdata_store(struct userdata* u, const struct nl_attr_view* a) {
    if (!a->present) return;
    u->length = (uint16_t)(a->length < USERDATA_BYTES ? a->length : USERDATA_BYTES); memcpy(u->bytes, a->data, u->length);
}
static void userdata_dump(struct nl_builder* b, uint16_t type, const struct userdata* u) { if (u->length) nl_attr(b, type, u->bytes, u->length); }

/* ---- object lookup in a ruleset ---- */
static struct nft_table* table_find(struct ruleset* s, uint8_t family, const char* name) {
    for (unsigned i = 0; i < TABLE_MAX; i++) if (s->tables[i].used && s->tables[i].family == family && !strcmp(s->tables[i].name, name)) return &s->tables[i];
    return NULL;
}
static int table_index(struct ruleset* s, struct nft_table* t) { return t ? (int)(t - s->tables) : -1; }
static struct nft_chain* chain_find(struct ruleset* s, int table, const char* name) {
    for (unsigned i = 0; i < CHAIN_MAX; i++) if (s->chains[i].used && (int)s->chains[i].table == table && !strcmp(s->chains[i].name, name)) return &s->chains[i];
    return NULL;
}
static struct nft_set* set_find(struct ruleset* s, int table, const char* name) {
    for (unsigned i = 0; i < SET_MAX; i++) if (s->sets[i].used && (int)s->sets[i].table == table && !strcmp(s->sets[i].name, name)) return &s->sets[i];
    return NULL;
}

/* A set named by a rule or element list: by name, else (anonymous sets of this batch, named "__set%d" by nft) by id. */
static struct nft_set* set_ref(struct ruleset* s, int table, const struct nl_attr_view* name, const struct nl_attr_view* id) {
    char sname[32]; if (!nl_string(name, sname, sizeof(sname))) return NULL;
    struct nft_set* set = set_find(s, table, sname); if (set || !id || !id->present) return set;
    uint32_t want = nl_be32(id);
    for (unsigned i = 0; i < SET_MAX; i++) if (s->sets[i].used && s->sets[i].fresh && (int)s->sets[i].table == table && s->sets[i].id == want) return &s->sets[i];
    return NULL;
}

/* ---- batch staging ---- */
static struct nfgenmsg { uint8_t family, version; uint16_t res_id; } nfgen;
static bool parse_nfgen(const uint8_t* p, unsigned length, struct nfgenmsg* g, const uint8_t** attrs, unsigned* attr_len) {
    if (length < 4) return false;
    g->family = p[0]; g->version = p[1]; memcpy(&g->res_id, p + 2, 2);
    *attrs = p + 4; *attr_len = length - 4; return true;
}
static struct ruleset* staging(void) {
    if (!work) { work = net_alloc(sizeof(*work)); if (work) { if (live) *work = *live; else memset(work, 0, sizeof(*work)); } }
    return work;
}

/* Give a chain's rules positions 1024, 2048, ... in their current order. */
static void renumber(struct ruleset* s, unsigned chain) {
    uint16_t index[RULE_MAX]; unsigned n = 0;
    for (unsigned i = 0; i < RULE_MAX; i++) if (s->rules[i].used && s->rules[i].chain == chain) {
        unsigned at = n++;
        while (at > 0 && s->rules[index[at - 1]].position > s->rules[i].position) { index[at] = index[at - 1]; at--; }
        index[at] = (uint16_t)i;
    }
    for (unsigned k = 0; k < n; k++) s->rules[index[k]].position = (int64_t)(k + 1) * 1024;
}
/* Expressions the evaluator implements; anything else is refused at load time (EOPNOTSUPP), so `nft` reports it
 * instead of a rule that silently would not do what it says (hash, socket, osf, tproxy, synproxy, xfrm, queue, dup,
 * fwd, connlimit, ct helper/zone/label/event writes, set-based objref, dynset with per-element expressions). */
static struct nft_set* set_find(struct ruleset* s, int table, const char* name);
static struct nft_obj* obj_find(struct ruleset* s, unsigned table, uint32_t type, const char* name);
static int validate_rule(struct ruleset* s, int table, int chain, uint8_t* list, unsigned length) {
    const struct nft_chain* c = chain >= 0 ? &s->chains[chain] : NULL;
    bool nat_chain = c && (!c->base || !strcmp(c->type, "nat"));   /* base nat chains and regular chains (jump targets) */
    uint32_t hook = c && c->base ? c->hooknum : 5;
    for (unsigned at = 0; at + 4 <= length;) {
        uint16_t size; memcpy(&size, list + at, 2); if (size < 4 || size > length - at) return -22;
        struct nl_attr_view expr[3]; char name[24] = "";
        if (!nl_parse(list + at + 4, size - 4, expr, 2) || !nl_string(&expr[1], name, sizeof(name))) return -22;
        struct nl_attr_view d[12]; memset(d, 0, sizeof(d));
        if (expr[2].present && !nl_parse(expr[2].data, expr[2].length, d, 11)) return -22;
        bool ok = !strcmp(name, "immediate") || !strcmp(name, "cmp") || !strcmp(name, "bitwise") || !strcmp(name, "counter") ||
                  !strcmp(name, "reject") || !strcmp(name, "log") || !strcmp(name, "range") || !strcmp(name, "byteorder") ||
                  !strcmp(name, "limit") || !strcmp(name, "quota") || !strcmp(name, "numgen") || !strcmp(name, "fib") ||
                  !strcmp(name, "rt") || !strcmp(name, "notrack") || !strcmp(name, "payload") || !strcmp(name, "exthdr");
        if (!strcmp(name, "bitwise") && d[6].present && nl_be32(&d[6]) > 2) return -95;
        if (!strcmp(name, "meta")) {
            uint32_t key = nl_be32(&d[2]);
            ok = !d[3].present || key == 3 || key == 2 || key == 12 || key == 19;   /* set: mark, priority, nftrace, pkttype */
        }
        if (!strcmp(name, "ct")) {
            uint32_t key = nl_be32(&d[2]);
            ok = !d[4].present || key == 3;                                       /* set: ct mark only */
            if (!d[4].present && (key == 4 || key == 6 || key == 13 || key == 16 || key == 17 || key == 18)) ok = false;   /* secmark, helper, labels, avgpkt, zone, eventmask */
        }
        if (!strcmp(name, "exthdr") && d[7].present) {                            /* writes: TCP MSS (clamping) only */
            if (nl_be32(&d[6]) != 1 || d[2].data[0] != 2 || nl_be32(&d[3]) != 2 || nl_be32(&d[4]) != 2) return -95;
        }
        if (!strcmp(name, "payload") && d[5].present && nl_be32(&d[4]) > 16) return -95;
        if (!strcmp(name, "nat") || !strcmp(name, "masq") || !strcmp(name, "redir")) {
            if (!nat_chain) return -95;                                           /* NAT statements live in nat chains */
            bool dst_hook = hook == 0 || hook == 3, src_hook = hook == 1 || hook == 4, any = hook == 5;
            if (!strcmp(name, "nat")) {
                uint32_t type = nl_be32(&d[1]); if (type > 1 || (nl_be32(&d[7]) & 64)) return -95;   /* no netmap */
                if (!any && (type == 1 ? !dst_hook : !src_hook)) return -95;      /* dnat: prerouting/output; snat: postrouting/input */
            } else if (!strcmp(name, "masq") && !any && hook != 4) return -95;
            else if (!strcmp(name, "redir") && !any && !dst_hook) return -95;
            ok = true;
        }
        if (!strcmp(name, "lookup")) {
            struct nft_set* set = set_ref(s, table, &d[1], &d[4]); if (!set) return -2;   /* unknown set */
            if (d[3].present && !(set->flags & NFT_SET_MAP)) return -22;               /* a map lookup of a plain set */
            unsigned want = (unsigned)strlen(set->name) + 1;
            if (d[1].length != want || memcmp(d[1].data, set->name, want)) {   /* "__set%d" -> its real name, same padded size */
                if (((4 + want + 3u) & ~3u) != ((4 + d[1].length + 3u) & ~3u)) return -95;
                uint8_t* at_name = (uint8_t*)d[1].data; uint16_t new_len = (uint16_t)(4 + want);
                memset(at_name, 0, (d[1].length + 3u) & ~3u); memcpy(at_name, set->name, want); memcpy(at_name - 4, &new_len, 2);
            }
            ok = true;
        }
        if (!strcmp(name, "dynset")) {
            if (d[7].present || d[10].present) {                                  /* per-element expressions: limit, counter, quota */
                const struct nl_attr_view* list = d[10].present ? &d[10] : &d[7]; unsigned total = 0;
                for (unsigned k = 0; k + 4 <= list->length;) {
                    uint16_t ks; memcpy(&ks, list->data + k, 2); if (ks < 4 || ks > list->length - k) return -22;
                    const uint8_t* body = d[10].present ? list->data + k + 4 : list->data; unsigned blen = d[10].present ? ks - 4u : list->length;
                    struct nl_attr_view ex[3]; char ename[24] = "";
                    if (!nl_parse(body, blen, ex, 2) || !nl_string(&ex[1], ename, sizeof(ename))) return -22;
                    if (strcmp(ename, "limit") && strcmp(ename, "counter") && strcmp(ename, "quota")) return -95;
                    total += (blen + 4 + 3u) & ~3u;
                    if (!d[10].present) break;
                    k += (ks + 3u) & ~3u;
                }
                if (total > 96) return -95;
            }
            struct nft_set* set = set_ref(s, table, &d[1], &d[2]); if (!set) return -2;
            if (!(set->flags & NFT_SET_EVAL)) return -95;                         /* only "dynamic" sets change from the packet path */
            if (nl_be32(&d[3]) > 2) return -95;
            ok = true;
        }
        if (!strcmp(name, "objref")) {
            if (d[3].present || d[4].present) return -95;                          /* object maps */
            uint32_t type = nl_be32(&d[1]); char oname[32];
            if (!nl_string(&d[2], oname, sizeof(oname))) return -22;
            if (type != NFT_OBJECT_COUNTER && type != NFT_OBJECT_QUOTA && type != NFT_OBJECT_LIMIT) return -95;
            if (!obj_find(s, (unsigned)table, type, oname)) return -2;
            ok = true;
        }
        if (!strcmp(name, "fib")) { uint32_t result = nl_be32(&d[2]); if (result < 1 || result > 3) return -95; }
        if (!strcmp(name, "rt")) { uint32_t key = nl_be32(&d[2]); if (key > 3) return -95; }
        if (!ok) return -95;
        at += (size + 3u) & ~3u;
    }
    return 0;
}

/* ---- message handlers (operate on the staging ruleset) ---- */
static int do_newtable(struct ruleset* s, uint8_t family, const struct nl_attr_view* a, bool excl) {
    if (family != 1 && family != 2 && family != 10) return -95;   /* inet, ip, ip6 (no arp/bridge/netdev hooks here) */
    char name[32]; if (!nl_string(&a[NFTA_TABLE_NAME], name, sizeof(name))) return -22;
    if (table_find(s, family, name)) return excl ? -17 : 0;   /* `nft create` vs `nft add` */
    for (unsigned i = 0; i < TABLE_MAX; i++) if (!s->tables[i].used) {
        memset(&s->tables[i], 0, sizeof(s->tables[i]));
        s->tables[i].used = true; s->tables[i].family = family; s->tables[i].flags = nl_be32(&a[NFTA_TABLE_FLAGS]); s->tables[i].handle = ++s->next_handle;
        strcpy(s->tables[i].name, name); userdata_store(&s->tables[i].user, &a[6]); return 0;
    }
    return -28;
}
static void drop_table(struct ruleset* s, int table) {
    for (unsigned i = 0; i < RULE_MAX; i++) if (s->rules[i].used && (int)s->rules[i].table == table) s->rules[i].used = false;
    for (unsigned i = 0; i < ELEM_MAX; i++) if (s->elems[i].used && (int)s->sets[s->elems[i].set].table == table) s->elems[i].used = false;
    for (unsigned i = 0; i < CHAIN_MAX; i++) if (s->chains[i].used && (int)s->chains[i].table == table) s->chains[i].used = false;
    for (unsigned i = 0; i < SET_MAX; i++) if (s->sets[i].used && (int)s->sets[i].table == table) s->sets[i].used = false;
    for (unsigned i = 0; i < OBJ_MAX; i++) if (s->objects[i].used && (int)s->objects[i].table == table) s->objects[i].used = false;
    s->tables[table].used = false;
}
static int do_deltable(struct ruleset* s, uint8_t family, const struct nl_attr_view* a) {
    if (!a[NFTA_TABLE_NAME].present) {   /* flush ruleset: every table of this family (0: all) */
        for (unsigned i = 0; i < TABLE_MAX; i++) if (s->tables[i].used && (!family || s->tables[i].family == family)) drop_table(s, (int)i);
        return 0;
    }
    char name[32]; if (!nl_string(&a[NFTA_TABLE_NAME], name, sizeof(name))) return -22;
    struct nft_table* t = table_find(s, family, name); if (!t) return -2;
    drop_table(s, table_index(s, t)); return 0;
}
static int do_newchain(struct ruleset* s, uint8_t family, const struct nl_attr_view* a, bool excl) {
    char tname[32], cname[32];
    if (!nl_string(&a[NFTA_CHAIN_TABLE], tname, sizeof(tname)) || !nl_string(&a[NFTA_CHAIN_NAME], cname, sizeof(cname))) return -22;
    struct nft_table* t = table_find(s, family, tname); if (!t) return -2;
    struct nft_chain* c = chain_find(s, table_index(s, t), cname);
    if (c && excl) return -17;
    if (!c) { for (unsigned i = 0; i < CHAIN_MAX; i++) if (!s->chains[i].used) { c = &s->chains[i]; memset(c, 0, sizeof(*c)); c->used = true; c->table = (unsigned)table_index(s, t); strcpy(c->name, cname); c->handle = ++s->next_handle; c->policy = NF_ACCEPT; break; } }
    if (!c) return -28;
    if (a[NFTA_CHAIN_POLICY].present) c->policy = (uint8_t)nl_be32(&a[NFTA_CHAIN_POLICY]);
    if (a[NFTA_CHAIN_FLAGS].present) c->flags = nl_be32(&a[NFTA_CHAIN_FLAGS]);
    if (a[NFTA_CHAIN_TYPE].present) nl_string(&a[NFTA_CHAIN_TYPE], c->type, sizeof(c->type));
    userdata_store(&c->user, &a[12]);
    if (a[NFTA_CHAIN_HOOK].present) {
        struct nl_attr_view h[5]; if (!nl_parse(a[NFTA_CHAIN_HOOK].data, a[NFTA_CHAIN_HOOK].length, h, 4)) return -22;
        if (h[3].present || h[4].present) { c->used = false; return -95; }   /* netdev/ingress devices */
        c->base = true; c->hooknum = nl_be32(&h[NFTA_HOOK_HOOKNUM]); c->priority = (int32_t)nl_be32(&h[NFTA_HOOK_PRIORITY]);
        if (!c->type[0]) strcpy(c->type, "filter");
        bool filter = !strcmp(c->type, "filter"), nat = !strcmp(c->type, "nat"), route = !strcmp(c->type, "route");
        /* nat: prerouting, input, output, postrouting (not forward); route: output (Linux nft_chain_route) */
        if (c->hooknum > 4 || (!filter && !nat && !route) || (nat && c->hooknum == 2) || (route && c->hooknum != 3) ||
            (nat && family != 1 && family != 2 && family != 10)) { c->used = false; return -95; }
        if (nat) c->policy = NF_ACCEPT;                                     /* nat chains never drop by policy */
    }
    return 0;
}
static int do_delchain(struct ruleset* s, uint8_t family, const struct nl_attr_view* a) {
    char tname[32], cname[32];
    if (!nl_string(&a[NFTA_CHAIN_TABLE], tname, sizeof(tname))) return -22;
    struct nft_table* t = table_find(s, family, tname); if (!t) return -2;
    if (!nl_string(&a[NFTA_CHAIN_NAME], cname, sizeof(cname))) return -22;
    struct nft_chain* c = chain_find(s, table_index(s, t), cname); if (!c) return -2;
    for (unsigned i = 0; i < RULE_MAX; i++) if (s->rules[i].used && s->rules[i].chain == (unsigned)(c - s->chains)) s->rules[i].used = false;
    c->used = false; return 0;
}
static int do_newrule(struct ruleset* s, uint8_t family, const struct nlmsghdr* rq, const struct nl_attr_view* a) {
    char tname[32], cname[32];
    if (!nl_string(&a[NFTA_RULE_TABLE], tname, sizeof(tname)) || !nl_string(&a[NFTA_RULE_CHAIN], cname, sizeof(cname))) return -22;
    struct nft_table* t = table_find(s, family, tname); if (!t) return -2;
    struct nft_chain* c = chain_find(s, table_index(s, t), cname); if (!c) return -2;
    if (!a[NFTA_RULE_EXPRESSIONS].present || a[NFTA_RULE_EXPRESSIONS].length > EXPR_BYTES) return a[NFTA_RULE_EXPRESSIONS].length > EXPR_BYTES ? -7 : 0;
    uint8_t expr[EXPR_BYTES]; unsigned expr_len = a[NFTA_RULE_EXPRESSIONS].length; memcpy(expr, a[NFTA_RULE_EXPRESSIONS].data, expr_len);
    int verdict = validate_rule(s, table_index(s, t), (int)(c - s->chains), expr, expr_len);
    if (verdict) return verdict;
    unsigned chain = (unsigned)(c - s->chains);
    /* where: append (NLM_F_APPEND) after NFTA_RULE_POSITION or at the end; insert before it or at the start */
    int64_t low = 0, high = 0; bool any = false;
    for (unsigned i = 0; i < RULE_MAX; i++) if (s->rules[i].used && s->rules[i].chain == chain) {
        if (!any || s->rules[i].position < low) low = s->rules[i].position;
        if (!any || s->rules[i].position > high) high = s->rules[i].position;
        any = true;
    }
    bool append = (rq->flags & NLM_F_APPEND) != 0; int64_t position;
    if (a[NFTA_RULE_POSITION].present) {
        uint64_t raw = 0; if (a[NFTA_RULE_POSITION].length >= 8) memcpy(&raw, a[NFTA_RULE_POSITION].data, 8);
        uint64_t handle = __builtin_bswap64(raw);
        struct nft_rule* anchor = NULL;
        for (unsigned i = 0; i < RULE_MAX; i++) if (s->rules[i].used && s->rules[i].chain == chain && s->rules[i].handle == handle) anchor = &s->rules[i];
        if (!anchor) return -2;
        renumber(s, chain);   /* 1024 apart: a midpoint always exists */
        position = append ? anchor->position + 512 : anchor->position - 512;
    } else position = !any ? 0 : append ? high + 1024 : low - 1024;
    for (unsigned i = 0; i < RULE_MAX; i++) if (!s->rules[i].used) {
        struct nft_rule* r = &s->rules[i]; memset(r, 0, sizeof(*r));
        r->used = true; r->table = (unsigned)table_index(s, t); r->chain = chain; r->handle = ++s->next_handle; r->position = position;
        r->expr_len = expr_len; memcpy(r->expr, expr, expr_len); userdata_store(&r->user, &a[7]);
        return 0;
    }
    return -28;
}
static int do_delrule(struct ruleset* s, uint8_t family, const struct nl_attr_view* a) {
    char tname[32], cname[32];
    if (!nl_string(&a[NFTA_RULE_TABLE], tname, sizeof(tname))) return -22;
    struct nft_table* t = table_find(s, family, tname); if (!t) return -2;
    int chain = -1;
    if (a[NFTA_RULE_CHAIN].present) { if (!nl_string(&a[NFTA_RULE_CHAIN], cname, sizeof(cname))) return -22; struct nft_chain* c = chain_find(s, table_index(s, t), cname); if (!c) return -2; chain = (int)(c - s->chains); }
    if (a[NFTA_RULE_HANDLE].present) {   /* one rule (nft delete rule ... handle N) */
        uint64_t raw = 0; if (a[NFTA_RULE_HANDLE].length >= 8) memcpy(&raw, a[NFTA_RULE_HANDLE].data, 8); uint64_t handle = __builtin_bswap64(raw);
        for (unsigned i = 0; i < RULE_MAX; i++) if (s->rules[i].used && (int)s->rules[i].table == table_index(s, t) && s->rules[i].handle == handle) { s->rules[i].used = false; return 0; }
        return -2;
    }
    for (unsigned i = 0; i < RULE_MAX; i++)   /* every rule of the chain, or of the table (nft flush chain / flush table) */
        if (s->rules[i].used && (int)s->rules[i].table == table_index(s, t) && (chain < 0 || (int)s->rules[i].chain == chain)) s->rules[i].used = false;
    return 0;
}
static int do_delobj(struct ruleset* s, uint8_t family, const struct nl_attr_view* a) {
    char tname[32], oname[32];
    if (!nl_string(&a[NFTA_OBJ_TABLE], tname, sizeof(tname)) || !nl_string(&a[NFTA_OBJ_NAME], oname, sizeof(oname))) return -22;
    struct nft_table* t = table_find(s, family, tname); if (!t) return -2;
    for (unsigned i = 0; i < OBJ_MAX; i++) if (s->objects[i].used && (int)s->objects[i].table == table_index(s, t) && !strcmp(s->objects[i].name, oname)) { s->objects[i].used = false; return 0; }
    return -2;
}
static uint64_t nl_be64v(const struct nl_attr_view* a) { uint64_t v = 0; if (a->present && a->length >= 8) memcpy(&v, a->data, 8); return __builtin_bswap64(v); }
static int do_newset(struct ruleset* s, uint8_t family, const struct nl_attr_view* a, bool excl) {
    char tname[32], sname[32];
    if (!nl_string(&a[NFTA_SET_TABLE], tname, sizeof(tname)) || !nl_string(&a[NFTA_SET_NAME], sname, sizeof(sname))) return -22;
    struct nft_table* t = table_find(s, family, tname); if (!t) return -2;
    char* pattern = strstr(sname, "%d");
    if (pattern) {   /* "__set%d": the lowest free number, as nf_tables_set_alloc_name */
        char prefix[32], suffix[32]; unsigned plen = (unsigned)(pattern - sname);
        memcpy(prefix, sname, plen); prefix[plen] = 0; strcpy(suffix, pattern + 2);
        bool named = false;
        for (unsigned n = 0; n < 100 && !named; n++) {
            char candidate[48]; strcpy(candidate, prefix); char* q = candidate + plen;
            if (n >= 10) *q++ = (char)('0' + n / 10);
            *q++ = (char)('0' + n % 10); *q = 0;
            if (strlen(candidate) + strlen(suffix) >= sizeof(sname)) return -36;
            strcat(candidate, suffix);
            if (!set_find(s, table_index(s, t), candidate)) { strcpy(sname, candidate); named = true; }
        }
        if (!named) return -28;
    } else if (set_find(s, table_index(s, t), sname)) return excl ? -17 : 0;
    uint32_t flags = nl_be32(&a[NFTA_SET_FLAGS]);
    if (flags & (NFT_SET_EXPR | NFT_SET_OBJECT) || a[17].present) return -95;   /* per-element expressions, object maps */
    for (unsigned i = 0; i < SET_MAX; i++) if (!s->sets[i].used) {
        struct nft_set* set = &s->sets[i]; memset(set, 0, sizeof(*set));
        set->used = true; set->table = (unsigned)table_index(s, t); strcpy(set->name, sname); set->handle = ++s->next_handle;
        set->flags = flags; set->key_type = nl_be32(&a[NFTA_SET_KEY_TYPE]); set->key_len = nl_be32(&a[NFTA_SET_KEY_LEN]);
        set->data_type = nl_be32(&a[NFTA_SET_DATA_TYPE]); set->data_len = nl_be32(&a[NFTA_SET_DATA_LEN]); set->id = nl_be32(&a[NFTA_SET_ID]);
        set->policy = nl_be32(&a[8]); set->timeout_ms = nl_be64v(&a[11]); set->gc_interval = nl_be32(&a[12]); set->objtype = nl_be32(&a[15]);
        userdata_store(&set->user, &a[13]);
        set->fresh = true;
        if (a[9].present) {   /* NFTA_SET_DESC: size, concatenation field lengths */
            struct nl_attr_view d[3]; if (!nl_parse(a[9].data, a[9].length, d, 2)) { set->used = false; return -22; }
            set->size = nl_be32(&d[1]);
            if (d[2].present) {
                for (unsigned at = 0; at + 4 <= d[2].length && set->field_count < 16;) {
                    uint16_t size; memcpy(&size, d[2].data + at, 2); if (size < 4 || size > d[2].length - at) break;
                    struct nl_attr_view f[2]; if (nl_parse(d[2].data + at + 4, size - 4, f, 1)) set->fields[set->field_count++] = (uint8_t)nl_be32(&f[1]);
                    at += (size + 3u) & ~3u;
                }
            }
        }
        if (!set->key_len || set->key_len > KEY_BYTES || set->data_len > KEY_BYTES) { set->used = false; return -22; }
        if ((flags & NFT_SET_MAP) && set->data_type != NFT_DATA_VERDICT_TYPE && !set->data_len) { set->used = false; return -22; }
        return 0;
    }
    return -28;
}
static int do_delset(struct ruleset* s, uint8_t family, const struct nl_attr_view* a) {
    char tname[32], sname[32];
    if (!nl_string(&a[NFTA_SET_TABLE], tname, sizeof(tname)) || !nl_string(&a[NFTA_SET_NAME], sname, sizeof(sname))) return -22;
    struct nft_table* t = table_find(s, family, tname); if (!t) return -2;
    struct nft_set* set = set_find(s, table_index(s, t), sname); if (!set) return -2;
    for (unsigned i = 0; i < ELEM_MAX; i++) if (s->elems[i].used && s->elems[i].set == (unsigned)(set - s->sets)) s->elems[i].used = false;
    set->used = false; return 0;
}
static int data_value(const struct nl_attr_view* data_attr, uint8_t* out, unsigned* out_len);
/* An element's key/data: NFTA_DATA_VALUE bytes, or (data) a verdict: code in data[0..3] (host order), chain name. */
static int elem_data(const struct nl_attr_view* attr, uint8_t* out, unsigned* out_len, char* chain) {
    struct nl_attr_view d[3]; if (!nl_parse(attr->data, attr->length, d, 2)) return -22;
    if (d[NFTA_DATA_VERDICT].present) {
        struct nl_attr_view v[3]; if (!nl_parse(d[NFTA_DATA_VERDICT].data, d[NFTA_DATA_VERDICT].length, v, 2)) return -22;
        int32_t code = (int32_t)nl_be32(&v[NFTA_VERDICT_CODE]); memcpy(out, &code, 4); *out_len = 4;
        if (chain) { chain[0] = 0; if (v[NFTA_VERDICT_CHAIN].present) nl_string(&v[NFTA_VERDICT_CHAIN], chain, 32); }
        return 1;
    }
    if (!d[NFTA_DATA_VALUE].present || d[NFTA_DATA_VALUE].length > KEY_BYTES) return -22;
    *out_len = d[NFTA_DATA_VALUE].length; memcpy(out, d[NFTA_DATA_VALUE].data, *out_len); return 0;
}
static bool elem_same(const struct nft_elem* e, unsigned set, const uint8_t* key, unsigned kl, uint32_t flags) {
    return e->used && e->set == set && e->key_len == kl && !memcmp(e->key, key, kl) &&
           (e->flags & NFT_SET_ELEM_INTERVAL_END) == (flags & NFT_SET_ELEM_INTERVAL_END);
}
static int do_setelem(struct ruleset* s, uint8_t family, const struct nl_attr_view* a, bool add, bool excl) {
    char tname[32], sname[32];
    if (!nl_string(&a[NFTA_SET_ELEM_LIST_TABLE], tname, sizeof(tname)) || !nl_string(&a[NFTA_SET_ELEM_LIST_SET], sname, sizeof(sname))) return -22;
    struct nft_table* t = table_find(s, family, tname); if (!t) return -2;
    struct nft_set* set = set_ref(s, table_index(s, t), &a[NFTA_SET_ELEM_LIST_SET], &a[4] /* NFTA_SET_ELEM_LIST_SET_ID */); if (!set) return -2;
    if (!a[NFTA_SET_ELEM_LIST_ELEMENTS].present) {
        if (!add) for (unsigned i = 0; i < ELEM_MAX; i++) if (s->elems[i].used && s->elems[i].set == (unsigned)(set - s->sets)) s->elems[i].used = false;   /* flush set */
        return 0;
    }
    const uint8_t* list = a[NFTA_SET_ELEM_LIST_ELEMENTS].data; unsigned length = a[NFTA_SET_ELEM_LIST_ELEMENTS].length;
    unsigned slot = (unsigned)(set - s->sets); uint64_t now = native_clock_ns();
    for (unsigned at = 0; at + 4 <= length;) {
        uint16_t size; memcpy(&size, list + at, 2);
        if (size < 4 || size > length - at) return -22;
        /* (libnftnl numbers the element nests 1, 2, ...; the kernel does not check their type) */
        struct nl_attr_view e[12]; if (!nl_parse(list + at + 4, size - 4, e, 11)) return -22;
        if (e[7].present || e[11].present) return -95;                       /* element expressions */
        uint8_t key[KEY_BYTES], key_end[KEY_BYTES], data[KEY_BYTES]; unsigned kl = 0, kel = 0, dl = 0; char chain[32] = "";
        if (!e[1].present || elem_data(&e[1], key, &kl, NULL) != 0) return -22;
        if (kl != set->key_len && !(set->flags & NFT_SET_CONCAT)) return -22;
        bool has_end = e[10].present && elem_data(&e[10], key_end, &kel, NULL) == 0;
        if (e[10].present && (!has_end || kel != kl)) return -22;
        uint32_t flags = e[3].present ? nl_be32(&e[3]) : 0;
        if (add) {
            bool interval_end = flags & NFT_SET_ELEM_INTERVAL_END;
            if ((set->flags & NFT_SET_MAP) && !interval_end) {
                if (!e[2].present) return -22;
                int kind = elem_data(&e[2], data, &dl, chain); if (kind < 0) return -22;
                if ((kind == 1) != (set->data_type == NFT_DATA_VERDICT_TYPE)) return -22;
                if (kind == 1 && chain[0]) {
                    int32_t code; memcpy(&code, data, 4);
                    if ((code == NFT_JUMP || code == NFT_GOTO) && !chain_find(s, (int)set->table, chain)) return -2;
                }
                if (kind == 0 && dl != set->data_len) return -22;
            } else if (e[2].present && !(set->flags & NFT_SET_MAP)) return -22;
            if ((e[4].present || e[5].present) && !(set->flags & NFT_SET_TIMEOUT)) return -22;
            uint64_t timeout = e[4].present ? nl_be64v(&e[4]) : set->timeout_ms;
            int found = -1;
            for (unsigned i = 0; i < ELEM_MAX; i++) if (elem_same(&s->elems[i], slot, key, kl, flags)) { found = (int)i; break; }
            if (found >= 0 && excl) return -17;
            if (found < 0) {
                unsigned count = 0; for (unsigned i = 0; i < ELEM_MAX; i++) if (s->elems[i].used && s->elems[i].set == slot) count++;
                if (set->size && count >= set->size) return -23;           /* ENFILE: the set is full */
                for (unsigned i = 0; i < ELEM_MAX; i++) if (!s->elems[i].used) { found = (int)i; break; }
                if (found < 0) return -28;
            }
            struct nft_elem* el = &s->elems[found]; memset(el, 0, sizeof(*el));
            el->used = true; el->set = slot; el->flags = flags; el->key_len = kl; memcpy(el->key, key, kl);
            if (has_end) { el->has_end = true; memcpy(el->key_end, key_end, kl); }
            el->data_len = dl; memcpy(el->data, data, dl); strcpy(el->chain, chain);
            el->timeout_ms = (set->flags & NFT_SET_TIMEOUT) ? timeout : 0;
            el->expires_ns = el->timeout_ms ? now + el->timeout_ms * 1000000ULL : 0;
            if (e[5].present && el->timeout_ms) { uint64_t left = nl_be64v(&e[5]); if (left < el->timeout_ms) el->expires_ns = now + left * 1000000ULL; }
            userdata_store(&el->user, &e[6]);
        } else {
            bool hit = false;
            for (unsigned i = 0; i < ELEM_MAX; i++) if (elem_same(&s->elems[i], slot, key, kl, flags)) { s->elems[i].used = false; hit = true; }
            if (!hit) return -2;
        }
        at += (size + 3u) & ~3u;
    }
    return 0;
}
static int do_newobj(struct ruleset* s, uint8_t family, const struct nl_attr_view* a, bool excl) {
    char tname[32], oname[32];
    if (!nl_string(&a[NFTA_OBJ_TABLE], tname, sizeof(tname)) || !nl_string(&a[NFTA_OBJ_NAME], oname, sizeof(oname))) return -22;
    struct nft_table* t = table_find(s, family, tname); if (!t) return -2;
    uint32_t type = nl_be32(&a[NFTA_OBJ_TYPE]);
    if (type != NFT_OBJECT_COUNTER && type != NFT_OBJECT_QUOTA && type != NFT_OBJECT_LIMIT) return -95;   /* ct helpers, timeouts, ...: not here */
    struct nl_attr_view d[8]; memset(d, 0, sizeof(d));
    if (a[NFTA_OBJ_DATA].present && !nl_parse(a[NFTA_OBJ_DATA].data, a[NFTA_OBJ_DATA].length, d, 7)) return -22;
    struct nft_obj* o = NULL;
    for (unsigned i = 0; i < OBJ_MAX; i++) if (s->objects[i].used && (int)s->objects[i].table == table_index(s, t) && !strcmp(s->objects[i].name, oname)) o = &s->objects[i];
    if (o && (excl || o->type != type)) return o->type != type ? -22 : -17;
    if (!o) {
        for (unsigned i = 0; i < OBJ_MAX && !o; i++) if (!s->objects[i].used) o = &s->objects[i];
        if (!o) return -28;
        memset(o, 0, sizeof(*o)); o->used = true; o->table = (unsigned)table_index(s, t); strcpy(o->name, oname); o->handle = ++s->next_handle; o->type = type;
    }
    if (type == NFT_OBJECT_COUNTER) { o->bytes = nl_be64v(&d[1]); o->packets = nl_be64v(&d[2]); }
    else if (type == NFT_OBJECT_QUOTA) { o->quota = nl_be64v(&d[1]); o->quota_flags = nl_be32(&d[2]); if (d[4].present) o->consumed = nl_be64v(&d[4]); }
    else if (type == NFT_OBJECT_LIMIT) {
        o->rate = nl_be64v(&d[1]); o->unit = nl_be64v(&d[2]); o->burst = nl_be32(&d[3]); o->limit_type = nl_be32(&d[4]); o->limit_flags = nl_be32(&d[5]);
        if (!o->rate || !o->unit) { o->used = false; return -22; }
        o->tokens = 0; o->last_ns = 0;
    }
    userdata_store(&o->user, &a[8]);
    return 0;
}
static struct nft_obj* obj_find(struct ruleset* s, unsigned table, uint32_t type, const char* name) {
    for (unsigned i = 0; i < OBJ_MAX; i++) if (s->objects[i].used && s->objects[i].table == table && s->objects[i].type == type && !strcmp(s->objects[i].name, name)) return &s->objects[i];
    return NULL;
}

/* ====================================================================================================
 * Rule evaluation
 * ==================================================================================================== */
#define NFT_PAYLOAD_LL_HEADER 0
#define NFT_PAYLOAD_NETWORK_HEADER 1
#define NFT_PAYLOAD_TRANSPORT_HEADER 2
struct eval {
    struct nf_packet* p;
    uint8_t regs[64];
    struct ruleset* s;
    int verdict;               /* NF_ACCEPT/NF_DROP or NFT_* */
    char jump_chain[32];
    unsigned reject_type, reject_code; bool reject;
    bool nat_bound;            /* a NAT statement bound the connection (nat chains) */
    bool in_nat;               /* evaluating the nat chains of a NAT step */
};
/* Registers: NFT_REG_1..4 are 16 bytes, NFT_REG32_00..15 their 4-byte parts; a value may span consecutive registers
 * (concatenations), so the bytes available run to the end of the register file. */
static uint8_t* reg_ptr(struct eval* e, uint32_t reg, unsigned* avail) {
    unsigned at;
    if (reg >= 1 && reg <= 4) at = (reg - 1) * 16;
    else if (reg >= 8 && reg <= 23) at = (reg - 8) * 4;
    else { *avail = 0; return e->regs; }
    *avail = 64 - at; return e->regs + at;
}
/* connection tracking (conntrack.c) */
uint32_t conntrack_state_bits(const struct nf_packet* p);
uint32_t conntrack_direction(const struct nf_packet* p);
uint64_t conntrack_mark(const struct nf_packet* p);
void conntrack_set_mark(struct nf_packet* p, uint64_t value);
uint32_t conntrack_status(const struct nf_packet* p);
uint32_t conntrack_id(const struct nf_packet* p);
uint64_t conntrack_counter(const struct nf_packet* p, bool bytes, int direction);
uint32_t conntrack_expiration_ms(const struct nf_packet* p);
bool conntrack_tuple_field(const struct nf_packet* p, int direction, unsigned field, uint8_t* out, unsigned* length);
void conntrack_track(struct nf_packet* p);
void conntrack_lookup(struct nf_packet* p);
bool conntrack_nat_pending(const struct nf_packet* p, int manip);
void conntrack_nat_done(struct nf_packet* p, int manip);
int conntrack_nat_bind(struct nf_packet* p, int manip, const struct ip6a* low, const struct ip6a* high, uint16_t port_low, uint16_t port_high,
                       bool random, unsigned masq_ifindex);
bool conntrack_nat_translate(struct nf_packet* p, uint8_t* packet, unsigned length, int manip);
bool conntrack_any_nat(void);
static void store32(uint8_t* dst, unsigned avail, uint32_t v) { memset(dst, 0, avail < 4 ? avail : 4); memcpy(dst, &v, avail < 4 ? avail : 4); }
static void load_meta(struct eval* e, uint32_t key, uint8_t* dst, unsigned avail) {
    const struct nf_packet* p = e->p; memset(dst, 0, avail < 16 ? avail : 16);
    uint32_t v32 = 0; uint16_t v16 = 0;
    switch (key) {
    case 0: v32 = p->length; break;                                                            /* LEN */
    case 1: v16 = p->family == AF_INET6 ? 0x86DD : 0x0800; v16 = __builtin_bswap16(v16); memcpy(dst, &v16, avail < 2 ? avail : 2); return; /* PROTOCOL (BE) */
    case 2: v32 = p->priority; break;                                                          /* PRIORITY */
    case 3: v32 = p->mark; break;                                                              /* MARK */
    case 4: v32 = p->in ? p->in->index : 0; break;                                             /* IIF */
    case 5: v32 = p->out ? p->out->index : 0; break;                                           /* OIF */
    case 6: if (p->in) strncpy((char*)dst, p->in->name, avail < 16 ? avail : 16); return;      /* IIFNAME */
    case 7: if (p->out) strncpy((char*)dst, p->out->name, avail < 16 ? avail : 16); return;    /* OIFNAME */
    case 8: v16 = p->in ? p->in->type : 0; memcpy(dst, &v16, avail < 2 ? avail : 2); return;   /* IIFTYPE (host order) */
    case 9: v16 = p->out ? p->out->type : 0; memcpy(dst, &v16, avail < 2 ? avail : 2); return; /* OIFTYPE */
    case 10: v32 = p->has_socket ? p->socket_uid : 0; break;                                   /* SKUID */
    case 11: v32 = p->has_socket ? p->socket_gid : 0; break;                                   /* SKGID */
    case 15: dst[0] = p->family == AF_INET6 ? 10 : 2; return;                                  /* NFPROTO */
    case 16: dst[0] = p->l4_proto; return;                                                     /* L4PROTO */
    case 19: dst[0] = p->pkttype; return;                                                      /* PKTTYPE */
    case 20: v32 = 0; break;                                                                   /* CPU (the network runs under the kernel lock on the BSP) */
    case 21: v32 = p->in ? p->in->group : 0; break;                                            /* IIFGROUP */
    case 22: v32 = p->out ? p->out->group : 0; break;                                          /* OIFGROUP */
    case 24: { uint32_t r = 0; native_random(&r, 4); v32 = r; break; }                         /* PRANDOM */
    case 26: if (p->in) strncpy((char*)dst, p->in->kind[0] ? p->in->kind : "", avail < 16 ? avail : 16); return;   /* IIFKIND */
    case 27: if (p->out) strncpy((char*)dst, p->out->kind[0] ? p->out->kind : "", avail < 16 ? avail : 16); return;
    case 30: { uint64_t ns = native_realtime_ns(); memcpy(dst, &ns, avail < 8 ? avail : 8); return; }   /* TIME_NS */
    case 31: { uint64_t days = native_realtime_ns() / 86400000000000ULL; dst[0] = (uint8_t)((days + 4) % 7); return; }   /* TIME_DAY (0 = Sunday) */
    case 32: v32 = (uint32_t)((native_realtime_ns() / 1000000000ULL) % 86400); break;          /* TIME_HOUR: seconds into the (UTC) day */
    default: return;
    }
    store32(dst, avail, v32);
}
static void load_ct(struct eval* e, uint32_t key, int direction, uint8_t* dst, unsigned avail) {
    uint32_t v = 0; const struct nf_packet* p = e->p; memset(dst, 0, avail < 16 ? avail : 16);
    switch (key) {
    case 0: v = p->untracked ? 1u << 6 : conntrack_state_bits(p); break;   /* STATE (untracked: 64) */
    case 1: v = conntrack_direction(p); dst[0] = (uint8_t)v; return;        /* DIRECTION (u8) */
    case 2: v = conntrack_status(p); break;                                 /* STATUS */
    case 3: v = (uint32_t)conntrack_mark(p); break;                         /* MARK */
    case 5: v = conntrack_expiration_ms(p); break;                          /* EXPIRATION (ms) */
    case 7: case 8: case 9: case 10: case 11: case 12: case 19: case 20: case 21: case 22: {
        unsigned field = key == 7 ? 5 : key == 10 ? 4 : key == 11 ? 2 : key == 12 ? 3 : (key == 8 || key == 19 || key == 21) ? 0 : 1;
        uint8_t out[16]; unsigned n = 0;
        if (conntrack_tuple_field(p, direction < 0 ? 0 : direction, field, out, &n)) memcpy(dst, out, n < avail ? n : avail);
        return;
    }
    case 14: case 15: { uint64_t c = conntrack_counter(p, key == 15, direction); memcpy(dst, &c, avail < 8 ? avail : 8); return; }   /* PKTS, BYTES */
    case 23: v = __builtin_bswap32(conntrack_id(p)); break;                  /* ID */
    default: break;
    }
    store32(dst, avail, v);
}
static bool load_payload(struct eval* e, uint32_t base, uint32_t offset, uint32_t len, uint8_t* dst, unsigned avail) {
    const struct nf_packet* p = e->p; unsigned start;
    if (base == NFT_PAYLOAD_LL_HEADER) return false;                        /* no stored link header at these hooks */
    else if (base == NFT_PAYLOAD_NETWORK_HEADER) start = 0;
    else if (base == NFT_PAYLOAD_TRANSPORT_HEADER) start = p->l4_offset;
    else return false;
    if (p->fragment && base == NFT_PAYLOAD_TRANSPORT_HEADER) return false;
    if (len > avail || start + offset + len > p->length) return false;
    memcpy(dst, p->data + start + offset, len); return true;
}
static void csum_fold_update(uint8_t* sum, const uint8_t* from, const uint8_t* to, unsigned length, bool udp_zero) {
    uint16_t old = be16(sum); if (udp_zero && !old) return;
    uint32_t c = (uint16_t)~old; uint8_t a[2], b[2];
    for (unsigned i = 0; i < length; i += 2) {
        a[0] = from[i]; a[1] = i + 1 < length ? from[i + 1] : 0; b[0] = to[i]; b[1] = i + 1 < length ? to[i + 1] : 0;
        c += (uint16_t)~be16(a); c += be16(b);
    }
    while (c >> 16) c = (c & 0xFFFF) + (c >> 16);
    uint16_t v = (uint16_t)~c; if (udp_zero && !v) v = 0xFFFF; put16(sum, v);
}
/* payload write (mangle) with the requested checksum update (NFT_PAYLOAD_CSUM_INET, L4 pseudo-header) */
static bool write_payload(struct eval* e, uint32_t base, uint32_t offset, uint32_t len, const uint8_t* value, uint32_t csum_type,
                          uint32_t csum_offset, uint32_t csum_flags) {
    struct nf_packet* p = e->p; if (!p->writable || len > 16) return false;
    unsigned start = base == NFT_PAYLOAD_NETWORK_HEADER ? 0 : base == NFT_PAYLOAD_TRANSPORT_HEADER ? p->l4_offset : (unsigned)-1;
    if (start == (unsigned)-1 || (base == NFT_PAYLOAD_TRANSPORT_HEADER && p->fragment) || start + offset + len > p->length) return false;
    uint8_t* at = p->writable + start + offset; uint8_t old[16]; memcpy(old, at, len);
    if (!memcmp(old, value, len)) return true;
    /* keep 16-bit alignment for the incremental update: widen to the enclosing even span */
    unsigned lo = (start + offset) & ~1u, hi = (start + offset + len + 1) & ~1u; uint8_t before[20], after[20];
    if (hi - lo > sizeof(before)) return false;
    memcpy(before, p->writable + lo, hi - lo); memcpy(at, value, len); memcpy(after, p->writable + lo, hi - lo);
    if (csum_type == 1 && start + csum_offset + 2 <= p->length) csum_fold_update(p->writable + start + csum_offset, before, after, hi - lo, false);
    if ((csum_flags & 1) && !p->fragment) {   /* the field is in the L4 pseudo-header (addresses): fix TCP/UDP too */
        unsigned sum_at = p->l4_proto == IPPROTO_TCP ? p->l4_offset + 16 : p->l4_proto == IPPROTO_UDP ? p->l4_offset + 6 :
                          (p->family == AF_INET6 && p->l4_proto == IPPROTO_ICMPV6) ? p->l4_offset + 2 : 0;
        if (sum_at && sum_at + 2 <= p->length) csum_fold_update(p->writable + sum_at, before, after, hi - lo, p->l4_proto == IPPROTO_UDP && p->family == AF_INET);
    }
    if (p->family == AF_INET && base == NFT_PAYLOAD_NETWORK_HEADER && csum_type == 0 && offset < 20 && !(offset <= 10 && offset + len > 10)) {
        p->writable[10] = p->writable[11] = 0; uint16_t sum = ip_checksum(p->writable, (uint16_t)((p->writable[0] & 15) * 4)); memcpy(p->writable + 10, &sum, 2);
    }
    return true;
}
static int data_value(const struct nl_attr_view* data_attr, uint8_t* out, unsigned* out_len) {
    struct nl_attr_view d[2]; if (!nl_parse(data_attr->data, data_attr->length, d, 1)) return -22;
    if (!d[NFTA_DATA_VALUE].present || d[NFTA_DATA_VALUE].length > KEY_BYTES) return -22;
    *out_len = d[NFTA_DATA_VALUE].length; memcpy(out, d[NFTA_DATA_VALUE].data, *out_len); return 0;
}
static struct expr_state* rule_state(struct nft_rule* rule, uint32_t offset) {
    for (unsigned i = 0; i < rule->states; i++) if (rule->state[i].offset == offset) return &rule->state[i];
    if (rule->states >= RULE_STATES) return NULL;
    struct expr_state* st = &rule->state[rule->states++]; memset(st, 0, sizeof(*st)); st->offset = offset; return st;
}
/* Linux nft_limit token bucket: true when the packet is over the limit */
static bool limit_over(uint64_t* tokens, uint64_t* last, uint64_t rate, uint64_t unit, uint32_t burst, bool bytes, unsigned length) {
    if (!rate || !unit) return false;
    uint64_t nsecs = unit * 1000000000ULL, now = native_clock_ns();
    uint64_t cost = bytes ? (length && nsecs > ~0ULL / length ? ~0ULL : nsecs * length / rate) : nsecs / rate;
    uint64_t extra = bytes ? (uint64_t)burst : burst;
    uint64_t max = nsecs / rate * (rate + extra);                          /* (burst + rate) tokens' worth */
    if (bytes) max = nsecs + (extra ? (nsecs > ~0ULL / extra ? ~0ULL : nsecs * extra / rate) : 0);
    if (!*last) { *tokens = max; *last = now; }
    uint64_t have = *tokens + (now - *last); if (have > max || have < *tokens) have = max;
    *last = now;
    if (have >= cost) { *tokens = have - cost; return false; }
    *tokens = have; return true;
}
static bool elem_live(const struct nft_elem* el, uint64_t now) { return el->used && (!el->expires_ns || el->expires_ns > now); }
/* The element of `set` matching key (key_len bytes at `key`), or NULL. */
static struct nft_elem* set_match(struct ruleset* s, struct nft_set* set, const uint8_t* key) {
    unsigned slot = (unsigned)(set - s->sets), kl = set->key_len; uint64_t now = native_clock_ns();
    if (!(set->flags & NFT_SET_INTERVAL)) {
        for (unsigned i = 0; i < ELEM_MAX; i++) { struct nft_elem* el = &s->elems[i]; if (el->set == slot && elem_live(el, now) && el->key_len == kl && !memcmp(el->key, key, kl)) return el; }
        return NULL;
    }
    struct nft_elem* best = NULL;
    for (unsigned i = 0; i < ELEM_MAX; i++) {
        struct nft_elem* el = &s->elems[i]; if (el->set != slot || !elem_live(el, now) || el->key_len != kl) continue;
        if (el->has_end) { if (memcmp(el->key, key, kl) <= 0 && memcmp(key, el->key_end, kl) <= 0) return el; continue; }   /* [key, key_end] */
        if (memcmp(el->key, key, kl) <= 0 && (!best || memcmp(el->key, best->key, kl) > 0 ||
            (!memcmp(el->key, best->key, kl) && !(el->flags & NFT_SET_ELEM_INTERVAL_END)))) best = el;   /* rbtree: the boundary at or below */
    }
    return best && !(best->flags & NFT_SET_ELEM_INTERVAL_END) ? best : NULL;
}
static void set_verdict(struct eval* e, const uint8_t* data, const char* chain) {
    int32_t code; memcpy(&code, data, 4); e->verdict = code;
    if (code == NFT_JUMP || code == NFT_GOTO) strncpy(e->jump_chain, chain, sizeof(e->jump_chain) - 1);
}
static int nat_statement(struct eval* e, const char* name, const struct nl_attr_view* d);
/* A meter element's expressions (stored as NFTA_LIST_ELEM nests): false = the rule does not match. */
static bool element_exprs(struct eval* e, struct nft_elem* el) {
    bool match = true;
    for (unsigned at = 0; at + 4 <= el->expr_len;) {
        uint16_t size; memcpy(&size, el->expr + at, 2); if (size < 4 || size > el->expr_len - at) break;
        struct nl_attr_view ex[3]; char name[24] = "";
        if (nl_parse(el->expr + at + 4, size - 4u, ex, 2) && nl_string(&ex[1], name, sizeof(name))) {
            struct nl_attr_view d[8]; memset(d, 0, sizeof(d)); if (ex[2].present) nl_parse(ex[2].data, ex[2].length, d, 7);
            if (!strcmp(name, "counter")) { el->packets++; el->bytes += e->p->length; }
            else if (!strcmp(name, "limit")) {
                bool over = limit_over(&el->tokens, &el->last_ns, nl_be64v(&d[1]), nl_be64v(&d[2]), nl_be32(&d[3]), nl_be32(&d[4]) == 1, e->p->length);
                if (over != ((nl_be32(&d[5]) & 1) != 0)) match = false;
            } else if (!strcmp(name, "quota")) {
                el->consumed += e->p->length;
                if ((el->consumed >= nl_be64v(&d[1])) != ((nl_be32(&d[2]) & 1) != 0)) match = false;
            }
        }
        at += (size + 3u) & ~3u;
    }
    return match;
}
static bool fib_eval(struct eval* e, const struct nl_attr_view* d);
static bool exthdr_eval(struct eval* e, const struct nl_attr_view* d);
/* Evaluate one expression; returns false to break out of the rule (no match, or a verdict). */
static bool eval_expr(struct eval* e, const char* name, const struct nl_attr_view* d, struct nft_rule* rule, uint32_t offset) {
    unsigned avail; uint8_t* r;
    if (!strcmp(name, "immediate")) {
        uint32_t dreg = nl_be32(&d[1]);
        if (d[2].present) {
            struct nl_attr_view inner[3]; if (!nl_parse(d[2].data, d[2].length, inner, 2)) return true;
            if (inner[NFTA_DATA_VERDICT].present) {
                struct nl_attr_view v[3]; nl_parse(inner[NFTA_DATA_VERDICT].data, inner[NFTA_DATA_VERDICT].length, v, 2);
                e->verdict = (int)nl_be32(&v[NFTA_VERDICT_CODE]);
                if (e->verdict == NFT_JUMP || e->verdict == NFT_GOTO) nl_string(&v[NFTA_VERDICT_CHAIN], e->jump_chain, sizeof(e->jump_chain));
                return false;   /* a verdict ends the rule */
            }
            r = reg_ptr(e, dreg, &avail); uint8_t value[KEY_BYTES]; unsigned vl = 0;
            if (!data_value(&d[2], value, &vl) && vl <= avail) {   /* (NFTA_IMMEDIATE_DATA holds the NFTA_DATA_VALUE) */ memset(r, 0, vl < 16 ? (avail < 16 ? avail : 16) : vl); memcpy(r, value, vl); }
        }
        return true;
    }
    if (!strcmp(name, "payload")) {
        uint32_t base = nl_be32(&d[2]), off = nl_be32(&d[3]), len = nl_be32(&d[4]);
        if (d[5].present) {                                                  /* NFTA_PAYLOAD_SREG: mangle */
            uint8_t* src = reg_ptr(e, nl_be32(&d[5]), &avail); if (len > avail) return true;
            write_payload(e, base, off, len, src, nl_be32(&d[6]), nl_be32(&d[7]), nl_be32(&d[8]));
            return true;
        }
        r = reg_ptr(e, nl_be32(&d[1]), &avail);
        if (!load_payload(e, base, off, len, r, avail)) { e->verdict = NFT_BREAK; return false; }
        if (len < 4 && len < avail) memset(r + len, 0, (avail < 4 ? avail : 4) - len);
        return true;
    }
    if (!strcmp(name, "meta")) {
        uint32_t key = nl_be32(&d[2]);
        if (d[3].present) {                                                  /* NFTA_META_SREG: meta set */
            uint8_t* src = reg_ptr(e, nl_be32(&d[3]), &avail); uint32_t v; memcpy(&v, src, 4);
            if (key == 3) e->p->mark = v; else if (key == 2) e->p->priority = v; else if (key == 19) e->p->pkttype = src[0];
            return true;
        }
        r = reg_ptr(e, nl_be32(&d[1]), &avail); load_meta(e, key, r, avail); return true;
    }
    if (!strcmp(name, "ct")) {
        uint32_t key = nl_be32(&d[2]);
        if (d[4].present) { uint8_t* src = reg_ptr(e, nl_be32(&d[4]), &avail); uint32_t v; memcpy(&v, src, 4); if (key == 3) conntrack_set_mark(e->p, v); return true; }
        if (!e->p->ct && key != 0) { e->verdict = NFT_BREAK; return false; }   /* no connection: only `ct state` has a value */
        int direction = d[3].present ? d[3].data[0] : -1;
        r = reg_ptr(e, nl_be32(&d[1]), &avail); load_ct(e, key, direction, r, avail); return true;
    }
    if (!strcmp(name, "bitwise")) {
        uint32_t sreg = nl_be32(&d[1]), dreg = nl_be32(&d[2]), len = nl_be32(&d[3]), op = d[6].present ? nl_be32(&d[6]) : 0;
        unsigned sa, da; uint8_t* src = reg_ptr(e, sreg, &sa); uint8_t* dst = reg_ptr(e, dreg, &da);
        if (len > KEY_BYTES || len > sa || len > da) return true;
        uint8_t tmp[KEY_BYTES];
        if (op == 0) {
            uint8_t mask[KEY_BYTES], xor[KEY_BYTES]; unsigned ml = 0, xl = 0;
            if (d[4].present) data_value(&d[4], mask, &ml);
            if (d[5].present) data_value(&d[5], xor, &xl);
            for (unsigned i = 0; i < len; i++) tmp[i] = (uint8_t)((src[i] & (i < ml ? mask[i] : 0)) ^ (i < xl ? xor[i] : 0));
        } else {   /* shifts of host-order 32-bit words */
            uint8_t shift_bytes[KEY_BYTES]; unsigned sl = 0; uint32_t shift = 0;
            if (d[7].present && !data_value(&d[7], shift_bytes, &sl) && sl >= 4) memcpy(&shift, shift_bytes, 4);
            for (unsigned i = 0; i + 4 <= len; i += 4) { uint32_t w; memcpy(&w, src + i, 4); w = shift >= 32 ? 0 : op == 1 ? w << shift : w >> shift; memcpy(tmp + i, &w, 4); }
        }
        memcpy(dst, tmp, len); return true;
    }
    if (!strcmp(name, "byteorder")) {
        uint32_t sreg = nl_be32(&d[1]), dreg = nl_be32(&d[2]), len = nl_be32(&d[4]), size = nl_be32(&d[5]);
        unsigned sa, da; uint8_t* src = reg_ptr(e, sreg, &sa); uint8_t* dst = reg_ptr(e, dreg, &da);
        if (len > KEY_BYTES || len > sa || len > da || (size != 2 && size != 4 && size != 8)) return true;
        uint8_t tmp[KEY_BYTES];
        for (unsigned i = 0; i + size <= len; i += size) for (unsigned k = 0; k < size; k++) tmp[i + k] = src[i + size - 1 - k];
        memcpy(dst, tmp, len); return true;
    }
    if (!strcmp(name, "cmp")) {
        uint32_t sreg = nl_be32(&d[1]), op = nl_be32(&d[2]);
        uint8_t value[KEY_BYTES]; unsigned vl = 0; if (data_value(&d[3], value, &vl)) return true;
        unsigned sa; uint8_t* src = reg_ptr(e, sreg, &sa); if (vl > sa) { e->verdict = NFT_BREAK; return false; }
        int c = memcmp(src, value, vl);
        bool match = op == 0 ? c == 0 : op == 1 ? c != 0 : op == 2 ? c < 0 : op == 3 ? c <= 0 : op == 4 ? c > 0 : c >= 0;
        if (!match) { e->verdict = NFT_BREAK; return false; }
        return true;
    }
    if (!strcmp(name, "range")) {
        uint32_t sreg = nl_be32(&d[1]), op = nl_be32(&d[2]);
        uint8_t from[KEY_BYTES], to[KEY_BYTES]; unsigned fl = 0, tl = 0;
        if (data_value(&d[3], from, &fl) || data_value(&d[4], to, &tl) || fl != tl) return true;
        unsigned sa; uint8_t* src = reg_ptr(e, sreg, &sa); if (fl > sa) { e->verdict = NFT_BREAK; return false; }
        bool inside = memcmp(src, from, fl) >= 0 && memcmp(src, to, fl) <= 0;
        if (inside != (op == 0)) { e->verdict = NFT_BREAK; return false; }
        return true;
    }
    if (!strcmp(name, "lookup")) {
        char sname[32]; if (!nl_string(&d[1], sname, sizeof(sname))) return true;
        uint32_t sreg = nl_be32(&d[2]), flags = nl_be32(&d[5]);   /* NFTA_LOOKUP_SET_ID=4, NFTA_LOOKUP_FLAGS=5 */
        struct nft_set* set = set_find(e->s, (int)rule->table, sname); if (!set) { e->verdict = NFT_BREAK; return false; }
        unsigned sa; uint8_t* src = reg_ptr(e, sreg, &sa); if (set->key_len > sa) { e->verdict = NFT_BREAK; return false; }
        struct nft_elem* hit = set_match(e->s, set, src);
        if ((hit != NULL) == ((flags & 1) != 0)) { e->verdict = NFT_BREAK; return false; }   /* NFT_LOOKUP_F_INV */
        if (d[3].present && hit) {                                              /* map: the element's data to dreg (0: verdict) */
            uint32_t dreg = nl_be32(&d[3]);
            if (!dreg) { int32_t code; memcpy(&code, hit->data, 4); if (code == NFT_CONTINUE) return true; set_verdict(e, hit->data, hit->chain); return false; }
            r = reg_ptr(e, dreg, &avail); if (hit->data_len > avail) { e->verdict = NFT_BREAK; return false; }
            memcpy(r, hit->data, hit->data_len);
        }
        return true;
    }
    if (!strcmp(name, "dynset")) {
        char sname[32]; if (!nl_string(&d[1], sname, sizeof(sname))) return true;
        struct nft_set* set = set_find(e->s, (int)rule->table, sname); if (!set) { e->verdict = NFT_BREAK; return false; }
        uint32_t op = nl_be32(&d[3]); unsigned sa; uint8_t* key = reg_ptr(e, nl_be32(&d[4]), &sa); if (set->key_len > sa) return true;
        uint64_t timeout = d[6].present ? nl_be64v(&d[6]) : set->timeout_ms, now = native_clock_ns();
        struct nft_elem* hit = set_match(e->s, set, key); unsigned slot = (unsigned)(set - e->s->sets);
        if (op == 2) { if (hit) hit->used = false; return true; }                 /* delete */
        if (hit) {
            if (op == 1 && timeout) { hit->timeout_ms = timeout; hit->expires_ns = now + timeout * 1000000ULL; }   /* update refreshes */
            if (hit->expr_len && !element_exprs(e, hit)) { e->verdict = NFT_BREAK; return false; }
            return true;
        }
        unsigned count = 0; int free_slot = -1;
        for (unsigned i = 0; i < ELEM_MAX; i++) {
            if (e->s->elems[i].used && e->s->elems[i].set == slot) { if (elem_live(&e->s->elems[i], now)) count++; else if (free_slot < 0) free_slot = (int)i; }
            else if (!e->s->elems[i].used && free_slot < 0) free_slot = (int)i;
        }
        if ((set->size && count >= set->size) || free_slot < 0) { e->verdict = NFT_BREAK; return false; }   /* full: no match */
        struct nft_elem* el = &e->s->elems[free_slot]; memset(el, 0, sizeof(*el));
        el->used = true; el->set = slot; el->key_len = set->key_len; memcpy(el->key, key, set->key_len);
        if ((set->flags & NFT_SET_MAP) && d[5].present) { uint8_t* data = reg_ptr(e, nl_be32(&d[5]), &sa); el->data_len = set->data_len <= sa ? set->data_len : 0; memcpy(el->data, data, el->data_len); }
        el->timeout_ms = timeout; el->expires_ns = timeout ? now + timeout * 1000000ULL : 0;
        if (d[10].present && d[10].length <= sizeof(el->expr)) { memcpy(el->expr, d[10].data, d[10].length); el->expr_len = d[10].length; }
        else if (d[7].present && d[7].length + 4u <= sizeof(el->expr)) {   /* one expression: kept as a one-element list */
            uint16_t size = (uint16_t)(d[7].length + 4), type = NFTA_LIST_ELEM | 0x8000;
            memcpy(el->expr, &size, 2); memcpy(el->expr + 2, &type, 2); memcpy(el->expr + 4, d[7].data, d[7].length); el->expr_len = (uint16_t)((size + 3u) & ~3u);
        }
        if (el->expr_len && !element_exprs(e, el)) { e->verdict = NFT_BREAK; return false; }
        return true;
    }
    if (!strcmp(name, "counter")) { rule->packets++; rule->bytes += e->p->length; return true; }
    if (!strcmp(name, "limit")) {
        struct expr_state* st = rule_state(rule, offset); if (!st) return true;
        bool over = limit_over(&st->tokens, &st->last_ns, nl_be64v(&d[1]), nl_be64v(&d[2]), nl_be32(&d[3]), nl_be32(&d[4]) == 1, e->p->length);
        if (over != ((nl_be32(&d[5]) & 1) != 0)) { e->verdict = NFT_BREAK; return false; }
        return true;
    }
    if (!strcmp(name, "quota")) {
        struct expr_state* st = rule_state(rule, offset); if (!st) return true;
        if (!st->sequence) { st->sequence = 1; st->consumed = d[4].present ? nl_be64v(&d[4]) : 0; }
        st->consumed += e->p->length;
        bool over = st->consumed >= nl_be64v(&d[1]);
        if (over != ((nl_be32(&d[2]) & 1) != 0)) { e->verdict = NFT_BREAK; return false; }
        return true;
    }
    if (!strcmp(name, "objref")) {
        char oname[32]; if (!nl_string(&d[2], oname, sizeof(oname))) return true;
        struct nft_obj* o = obj_find(e->s, rule->table, nl_be32(&d[1]), oname); if (!o) { e->verdict = NFT_BREAK; return false; }
        if (o->type == NFT_OBJECT_COUNTER) { o->packets++; o->bytes += e->p->length; return true; }
        if (o->type == NFT_OBJECT_QUOTA) {
            o->consumed += e->p->length; bool over = o->consumed >= o->quota;
            if (over != ((o->quota_flags & 1) != 0)) { e->verdict = NFT_BREAK; return false; }
            return true;
        }
        bool over = limit_over(&o->tokens, &o->last_ns, o->rate, o->unit, o->burst, o->limit_type == 1, e->p->length);
        if (over != ((o->limit_flags & 1) != 0)) { e->verdict = NFT_BREAK; return false; }
        return true;
    }
    if (!strcmp(name, "numgen")) {
        uint32_t modulus = nl_be32(&d[2]), type = nl_be32(&d[3]), base = nl_be32(&d[4]); if (!modulus) return true;
        struct expr_state* st = rule_state(rule, offset); uint32_t v;
        if (type == 0 && st) { v = st->sequence++ % modulus; } else { uint32_t x = 0; native_random(&x, 4); v = x % modulus; }
        r = reg_ptr(e, nl_be32(&d[1]), &avail); store32(r, avail, v + base); return true;
    }
    if (!strcmp(name, "fib")) return fib_eval(e, d);
    if (!strcmp(name, "rt")) {
        uint32_t key = nl_be32(&d[2]); r = reg_ptr(e, nl_be32(&d[1]), &avail); memset(r, 0, avail < 16 ? avail : 16);
        const struct nf_packet* p = e->p; struct ip6a dst;
        if (p->family == AF_INET6) memcpy(dst.b, p->data + 24, 16); else dst = ip6a_from4(be32(p->data + 16));
        if (key == 3) { unsigned mtu = route_mtu(p->family == AF_INET6 ? AF_INET6 : AF_INET, &dst, NULL); uint16_t mss = (uint16_t)(mtu > 60 ? mtu - (p->family == AF_INET6 ? 60 : 40) : 0); memcpy(r, &mss, 2); return true; }
        if (key == 1 || key == 2) {
            struct route_result rr; if (route_lookup(p->family == AF_INET6 ? AF_INET6 : AF_INET, &dst, NULL, p->out ? p->out->index : 0, p->mark, &rr)) { e->verdict = NFT_BREAK; return false; }
            if (key == 1) { uint32_t v = __builtin_bswap32(ip6a_v4(&rr.nexthop)); memcpy(r, &v, 4); } else memcpy(r, rr.nexthop.b, 16);
        }
        return true;
    }
    if (!strcmp(name, "exthdr")) return exthdr_eval(e, d);
    if (!strcmp(name, "notrack")) { e->p->untracked = true; return true; }
    if (!strcmp(name, "nat") || !strcmp(name, "masq") || !strcmp(name, "redir")) {
        int v = nat_statement(e, name, d);
        if (v == NF_DROP) { e->verdict = NF_DROP; return false; }
        if (v == NF_ACCEPT) { e->verdict = NF_ACCEPT; e->nat_bound = true; return false; }
        return true;   /* not applicable to this packet (family): next expression */
    }
    if (!strcmp(name, "log")) {   /* the kernel log, as Linux's nf_log: prefix and the packet's addresses (rate-limited) */
        static uint64_t window; static unsigned logged; uint64_t now = native_clock_ns();
        if (now - window > 1000000000ULL) { window = now; logged = 0; }
        if (logged++ < 10) {
            char prefix[64] = ""; nl_string(&d[2], prefix, sizeof(prefix));
            struct ip6a src = e->p->family == AF_INET6 ? *(const struct ip6a*)(e->p->data + 8) : ip6a_from4(be32(e->p->data + 12));
            struct ip6a dst = e->p->family == AF_INET6 ? *(const struct ip6a*)(e->p->data + 24) : ip6a_from4(be32(e->p->data + 16));
            char line[256] = "[nft] "; strcat(line, prefix); strcat(line, " IN="); strcat(line, e->p->in ? e->p->in->name : "");
            strcat(line, " OUT="); strcat(line, e->p->out ? e->p->out->name : ""); net_log(line);
            net_log_addr(" SRC=", &src, ""); net_log_addr(" DST=", &dst, "");
            char proto[24] = " PROTO="; uint_to_str(e->p->l4_proto, proto + 7); net_log(proto);
            if (!e->p->fragment && (e->p->l4_proto == IPPROTO_TCP || e->p->l4_proto == IPPROTO_UDP) && e->p->l4_offset + 4 <= e->p->length) {
                char ports[48] = " SPT="; uint_to_str(be16(e->p->data + e->p->l4_offset), ports + 5); strcat(ports, " DPT=");
                uint_to_str(be16(e->p->data + e->p->l4_offset + 2), ports + strlen(ports)); net_log(ports);
            }
            net_log("\n");
        }
        return true;
    }
    if (!strcmp(name, "reject")) {
        e->reject = true; e->reject_type = nl_be32(&d[1]); e->reject_code = nl_u8(&d[2]); e->verdict = NF_DROP; return false;   /* ICMP_CODE is NLA_U8 */
    }
    return true;
}
/* Walk a rule's expression list. Sets e->verdict (default CONTINUE if no verdict expression ran). */
static void eval_rule(struct eval* e, struct nft_rule* rule) {
    e->verdict = NFT_CONTINUE;
    const uint8_t* list = rule->expr; unsigned length = rule->expr_len;
    for (unsigned at = 0; at + 4 <= length;) {
        uint16_t size, type; memcpy(&size, list + at, 2); memcpy(&type, list + at + 2, 2);
        if (size < 4 || size > length - at) break;
        if ((type & NLA_TYPE_MASK) == NFTA_LIST_ELEM) {
            struct nl_attr_view expr[3]; char name[24] = "";
            if (nl_parse(list + at + 4, size - 4, expr, 2) && nl_string(&expr[NFTA_EXPR_NAME], name, sizeof(name))) {
                struct nl_attr_view d[12]; if (expr[NFTA_EXPR_DATA].present) nl_parse(expr[NFTA_EXPR_DATA].data, expr[NFTA_EXPR_DATA].length, d, 11);
                else memset(d, 0, sizeof(d));
                if (!eval_expr(e, name, d, rule, at)) {
                    if (e->verdict == NFT_BREAK) e->verdict = NFT_CONTINUE;   /* no match: fall through to the next rule */
                    return;                                                   /* no match, or a real verdict: rule done */
                }
            }
        }
        at += (size + 3u) & ~3u;
    }
}
static int eval_chain(struct eval* e, struct nft_chain* chain, unsigned depth) {
    if (depth > 16) return NF_DROP;
    for (unsigned k = 0; k < e->s->ordered; k++) {
        struct nft_rule* rule = &e->s->rules[e->s->order[k]];
        if (!rule->used || rule->chain != (unsigned)(chain - e->s->chains)) continue;
        eval_rule(e, rule);
        int v = e->verdict;
        if (v == NF_ACCEPT || v == NF_DROP) return v;
        if (v == NFT_CONTINUE || v == NFT_BREAK) continue;
        if (v == NFT_RETURN) return NFT_RETURN;
        if (v == NFT_JUMP || v == NFT_GOTO) {
            struct nft_chain* target = chain_find(e->s, (int)chain->table, e->jump_chain);
            if (!target) return NF_DROP;
            int r = eval_chain(e, target, depth + 1);
            if (r == NF_ACCEPT || r == NF_DROP) return r;
            if (v == NFT_GOTO) return NFT_RETURN;   /* goto does not come back */
            /* jump: RETURN/continue falls through to the next rule here */
        }
    }
    return NFT_CONTINUE;
}
static void send_reject(struct eval* e);
static bool table_matches(uint8_t table_family, uint8_t family) { return table_family == 1 /* NFPROTO_INET */ || table_family == family; }
/* NAT of one hook (manip 1: destination at PREROUTING/OUTPUT, 0: source at POSTROUTING/INPUT): the nat chains run for a
 * new connection's first packet; then the packet is translated (every packet of a bound connection). */
static int nat_step(struct eval* e, struct nft_chain** chains, unsigned n, int manip) {
    struct nf_packet* p = e->p;
    if (n && conntrack_nat_pending(p, manip)) {
        e->nat_bound = false; e->in_nat = true;
        for (unsigned i = 0; i < n && !e->nat_bound; i++) {
            int v = eval_chain(e, chains[i], 0);
            if (v == NF_DROP) { e->in_nat = false; if (e->reject) send_reject(e); return NF_DROP; }
        }
        e->in_nat = false;
        if (!e->nat_bound) conntrack_nat_done(p, manip);   /* null binding */
    }
    if (p->writable && conntrack_nat_translate(p, p->writable, p->length, manip)) p->nat_changed = true;
    return NF_ACCEPT;
}
int nf_hook(struct nf_packet* p) {
    if (!live) return NF_ACCEPT;
    bool first = p->hook == NF_INET_PRE_ROUTING || p->hook == NF_INET_LOCAL_OUT;
    int manip = first ? 1 : (p->hook == NF_INET_POST_ROUTING || p->hook == NF_INET_LOCAL_IN) ? 0 : -1;
    if (!first && !p->ct) conntrack_lookup(p);   /* later hooks classify against the connection the first hook made */
    struct eval e; memset(&e, 0, sizeof(e)); e.p = p; e.s = live;
    /* the base chains of this hook (inet tables and the packet's family), lowest priority first (stable) */
    uint8_t family = p->family == AF_INET6 ? 10 : 2;
    struct nft_chain* order[CHAIN_MAX]; unsigned n = 0; struct nft_chain* nat[CHAIN_MAX]; unsigned nn = 0;
    for (unsigned i = 0; i < CHAIN_MAX; i++) {
        struct nft_chain* c = &e.s->chains[i];
        if (!c->used || !c->base || c->hooknum != p->hook || !table_matches(e.s->tables[c->table].family, family)) continue;
        bool is_nat = !strcmp(c->type, "nat");
        struct nft_chain** list = is_nat ? nat : order; unsigned* count = is_nat ? &nn : &n;
        unsigned at = (*count)++;
        while (at > 0 && list[at - 1]->priority > c->priority) { list[at] = list[at - 1]; at--; }
        list[at] = c;
    }
    bool tracked = !first, nat_done = manip < 0;
    int nat_priority = manip == 1 ? -100 : 100;   /* NF_IP_PRI_NAT_DST / NF_IP_PRI_NAT_SRC */
    for (unsigned i = 0; i < n; i++) {
        /* connection tracking runs at priority -200 (raw chains before it may say notrack), NAT at -100 / 100 */
        if (!tracked && order[i]->priority >= -200) { if (!p->untracked) conntrack_track(p); tracked = true; }
        if (!nat_done && order[i]->priority >= nat_priority) {
            nat_done = true;
            if (nat_step(&e, nat, nn, manip) == NF_DROP) return NF_DROP;
        }
        e.reject = false;
        int v = eval_chain(&e, order[i], 0);
        if (v == NF_DROP) { if (e.reject) send_reject(&e); return NF_DROP; }
        if (v == NF_ACCEPT) continue;   /* accept ends this chain only; the next base chain still sees the packet */
        if (order[i]->policy == NF_DROP) return NF_DROP;   /* fell off the end: the base chain policy */
    }
    if (!tracked && !p->untracked) conntrack_track(p);
    if (!nat_done && (nn || conntrack_any_nat())) { if (nat_step(&e, nat, nn, manip) == NF_DROP) return NF_DROP; }
    return NF_ACCEPT;
}
bool nf_active(void) { return any_base; }
/* any nat chain registered: the ingress paths give nf_hook a writable copy and decide again after it */
bool nf_nat_active(void) {
    if (!live) return false;
    for (unsigned i = 0; i < CHAIN_MAX; i++) if (live->chains[i].used && live->chains[i].base && !strcmp(live->chains[i].type, "nat")) return true;
    return conntrack_any_nat();
}

/* ---- NAT statements (nft_nat, nft_masq, nft_redir) ---- */
static int nat_statement(struct eval* e, const char* name, const struct nl_attr_view* d) {
    struct nf_packet* p = e->p; unsigned avail;
    if (!e->in_nat) return -1;                                              /* reached outside a NAT step: no effect */
    int manip = p->hook == NF_INET_PRE_ROUTING || p->hook == NF_INET_LOCAL_OUT ? 1 : 0;
    struct ip6a low, high; bool have_address = false; uint16_t port_low = 0, port_high = 0; uint32_t flags = 0; unsigned masq = 0;
    bool v6 = p->family == AF_INET6;
    uint32_t proto_min = 0, proto_max = 0;
    if (!strcmp(name, "nat")) {
        uint32_t type = nl_be32(&d[1]), family = nl_be32(&d[2]); flags = nl_be32(&d[7]);
        if ((type == 1) != (manip == 1)) return -1;                        /* dnat outside dst hooks / snat outside src hooks */
        if (family && family != (v6 ? 10u : 2u)) return -1;                 /* inet: a statement for the other family */
        if (d[3].present) {
            uint8_t* a = reg_ptr(e, nl_be32(&d[3]), &avail); uint8_t* b = d[4].present ? reg_ptr(e, nl_be32(&d[4]), &avail) : a;
            if (v6) { memcpy(low.b, a, 16); memcpy(high.b, b, 16); } else { low = ip6a_from4(be32(a)); high = ip6a_from4(be32(b)); }
            have_address = true;
        }
        proto_min = d[5].present ? nl_be32(&d[5]) : 0; proto_max = d[6].present ? nl_be32(&d[6]) : proto_min;
    } else if (!strcmp(name, "masq")) {
        if (manip != 0 || p->hook != NF_INET_POST_ROUTING || !p->out) return -1;
        flags = nl_be32(&d[1]); proto_min = d[2].present ? nl_be32(&d[2]) : 0; proto_max = d[3].present ? nl_be32(&d[3]) : proto_min;
        struct ip6a dst;
        if (v6) { memcpy(dst.b, p->data + 24, 16); if (!source_select6(&dst, p->out, 0, &low)) return NF_DROP; }
        else { uint32_t a = netdev_v4_primary(p->out); if (!a) return NF_DROP; low = ip6a_from4(a); }
        high = low; have_address = true; masq = p->out->index;
    } else {   /* redir: to the incoming interface's own address (127.0.0.1 / ::1 for local traffic) */
        if (manip != 1) return -1;
        proto_min = d[1].present ? nl_be32(&d[1]) : 0; proto_max = d[2].present ? nl_be32(&d[2]) : proto_min; flags = nl_be32(&d[3]);
        if (p->hook == NF_INET_LOCAL_OUT || !p->in) low = v6 ? ip6a_loopback_address : ip6a_from4(0x7F000001U);
        else if (v6) { struct ip6a src; memcpy(src.b, p->data + 8, 16); if (!source_select6(&src, p->in, 0, &low)) return NF_DROP; }
        else { uint32_t a = netdev_v4_primary(p->in); if (!a) return NF_DROP; low = ip6a_from4(a); }
        high = low; have_address = true;
    }
    if (proto_min) {
        uint8_t* a = reg_ptr(e, proto_min, &avail); uint8_t* b = reg_ptr(e, proto_max, &avail);
        port_low = be16(a); port_high = be16(b);
    }
    int error = conntrack_nat_bind(p, manip, have_address ? &low : NULL, have_address ? &high : NULL, port_low, port_high,
                                   (flags & (4 | 16)) != 0, masq);
    return error ? NF_DROP : NF_ACCEPT;
}
/* ---- fib: route lookups (rpfilter: fib saddr . iif oif missing; address types: fib daddr type local) ---- */
static bool fib_eval(struct eval* e, const struct nl_attr_view* d) {
    struct nf_packet* p = e->p; unsigned avail; uint8_t* r = reg_ptr(e, nl_be32(&d[1]), &avail);
    uint32_t result = nl_be32(&d[2]), flags = nl_be32(&d[3]);
    memset(r, 0, avail < 16 ? avail : 16);
    bool v6 = p->family == AF_INET6; struct ip6a address;
    const uint8_t* field = p->data + (v6 ? ((flags & 1) ? 8 : 24) : ((flags & 1) ? 12 : 16));
    if (v6) memcpy(address.b, field, 16); else address = ip6a_from4(be32(field));
    if (result == 3) {   /* ADDRTYPE (RTN_*) */
        uint32_t type = 1;
        if (v6) { if (ip6a_multicast(&address)) type = 5; else if (addrconf_local(&address, 0, true) || ip6a_loopback(&address)) type = 2; }
        else {
            uint32_t a = ip6a_v4(&address);
            if ((a >> 28) == 14) type = 5; else if (a == 0xFFFFFFFFU) type = 3;
            else if (netdev_v4_local(a) || (a >> 24) == 127) type = 2;
            else for (struct netdev* dev = netdevs; dev; dev = dev->next) for (unsigned i = 0; i < NETDEV_V4_MAX; i++) if (dev->v4[i].used && dev->v4[i].broadcast == a) type = 3;
        }
        if (flags & 32) type = type != 0;
        memcpy(r, &type, 4); return true;
    }
    struct route_result rr; unsigned oif = (flags & 16) && p->out ? p->out->index : 0;
    struct netdev* found = NULL;
    bool local = v6 ? addrconf_local(&address, 0, true) || ip6a_loopback(&address) : netdev_v4_local(ip6a_v4(&address)) || (ip6a_v4(&address) >> 24) == 127;
    if (local) found = netdev_lo;
    else if (!route_lookup(v6 ? AF_INET6 : AF_INET, &address, NULL, oif, (flags & 4) ? p->mark : 0, &rr)) found = rr.dev;
    if (found && (flags & 8) && p->in && found != p->in && !(local && (flags & 1))) found = NULL;   /* iif: the route must use the arrival interface */
    if (result == 1) { uint32_t index = found ? found->index : 0; if (flags & 32) index = index != 0; memcpy(r, &index, 4); }
    else if (found) strncpy((char*)r, found->name, avail < 16 ? avail : 16);
    return true;
}
/* ---- exthdr: IPv6 extension headers, TCP options (read; MSS write = clamping) ---- */
static bool exthdr_eval(struct eval* e, const struct nl_attr_view* d) {
    struct nf_packet* p = e->p; unsigned avail;
    uint8_t type = d[2].present ? d[2].data[0] : 0; uint32_t offset = nl_be32(&d[3]), len = nl_be32(&d[4]), flags = nl_be32(&d[5]), op = nl_be32(&d[6]);
    const uint8_t* data = p->data; unsigned found = 0, found_len = 0;
    if (op == 0) {   /* IPv6 extension header chain */
        if (p->family != AF_INET6) { e->verdict = NFT_BREAK; return false; }
        uint8_t next = data[6]; unsigned at = 40;
        for (unsigned n = 0; n < 8 && at + 8 <= p->length; n++) {
            if (next == type) { found = at; found_len = next == 44 ? 8 : (data[at + 1] + 1u) * 8u; break; }
            if (next != 0 && next != 43 && next != 60 && next != 44 && next != 51) break;
            uint8_t h = next; next = data[at]; at += h == 44 ? 8 : h == 51 ? (data[at + 1] + 2u) * 4u : (data[at + 1] + 1u) * 8u;
        }
    } else if (op == 1) {   /* TCP options */
        if (p->l4_proto != IPPROTO_TCP || p->fragment || p->l4_offset + 20 > p->length) { e->verdict = NFT_BREAK; return false; }
        unsigned th = p->l4_offset, end = th + (data[th + 12] >> 4) * 4u; if (end > p->length) end = p->length;
        for (unsigned at = th + 20; at < end;) {
            uint8_t kind = data[at];
            if (kind == 0) break;
            if (kind == 1) { at++; continue; }
            if (at + 1 >= end || data[at + 1] < 2 || at + data[at + 1] > end) break;
            if (kind == type) { found = at; found_len = data[at + 1]; break; }
            at += data[at + 1];
        }
    } else if (op == 2) {   /* IPv4 options */
        if (p->family != AF_INET) { e->verdict = NFT_BREAK; return false; }
        unsigned end = (data[0] & 15) * 4u;
        for (unsigned at = 20; at < end;) {
            uint8_t kind = data[at]; if (kind == 0) break; if (kind == 1) { at++; continue; }
            if (at + 1 >= end || data[at + 1] < 2 || at + data[at + 1] > end) break;
            if (kind == type) { found = at; found_len = data[at + 1]; break; }
            at += data[at + 1];
        }
    } else { e->verdict = NFT_BREAK; return false; }
    if (d[7].present) {   /* write: TCP maxseg, only ever lowered (nft_exthdr_tcp_set_eval) */
        if (!found || offset + len > found_len || !p->writable) return true;
        uint8_t* src = reg_ptr(e, nl_be32(&d[7]), &avail); uint16_t want = be16(src);   /* network order (nft's byteorder hton) */
        uint8_t* at = p->writable + found + offset; uint16_t old = be16(at);
        if (old <= want) return true;
        uint8_t before[2] = {at[0], at[1]}, after[2]; put16(after, want);
        csum_fold_update(p->writable + p->l4_offset + 16, before, after, 2, false); memcpy(at, after, 2);
        return true;
    }
    uint8_t* r = reg_ptr(e, nl_be32(&d[1]), &avail);
    if (flags & 1) { uint32_t present = found != 0; store32(r, avail, present); return true; }   /* NFT_EXTHDR_F_PRESENT */
    if (!found || offset + len > found_len || len > avail) { e->verdict = NFT_BREAK; return false; }
    memset(r, 0, avail < 4 ? avail : 4); memcpy(r, data + found + offset, len); return true;
}

/* reject: an ICMP(v6) unreachable or a TCP reset back to the source (the hooks already parsed the packet) */
void icmp6_error(struct netdev* dev, uint8_t type, uint8_t code, uint32_t pointer, const uint8_t* packet, unsigned length);
void icmp4_reject(const uint8_t* packet, unsigned length, uint8_t code, struct netdev* in);
void tcp_reset_for(const struct nf_packet* p);
static void send_reject(struct eval* e) {
    const struct nf_packet* p = e->p;
    if (e->reject_type == 1) {   /* NFT_REJECT_TCP_RST (nft only emits it after a tcp match) */
        if (p->l4_proto == IPPROTO_TCP) tcp_reset_for(p);
        return;
    }
    uint8_t code;
    if (e->reject_type == 2) {   /* NFT_REJECT_ICMPX_UNREACH: no-route / port / host / admin-prohibited */
        static const uint8_t v6[4] = {0, 4, 3, 1}, v4[4] = {0, 3, 1, 13};
        unsigned x = e->reject_code < 4 ? e->reject_code : 1;
        code = p->family == AF_INET6 ? v6[x] : v4[x];
    } else code = (uint8_t)e->reject_code;   /* NFT_REJECT_ICMP_UNREACH: the family's own code */
    if (p->family == AF_INET6) icmp6_error(p->in, 1, code, 0, p->data, p->length);
    else icmp4_reject(p->data, p->length, code, p->in);
}

/* ====================================================================================================
 * Dumps for `nft list ruleset`
 * ==================================================================================================== */
static void begin_nft(struct nl_builder* b, uint16_t msg, uint16_t flags, uint32_t seq, uint32_t portid, uint8_t family) {
    nl_begin(b, (uint16_t)(NFNL_SUBSYS_NFTABLES << 8 | msg), flags, seq, portid);
    struct nfgenmsg g = {family, 0, __builtin_bswap16((uint16_t)generation)}; nl_put(b, &g, sizeof(g));
}
static void dump_table(struct netlink_socket* nl, const struct nlmsghdr* rq, struct nft_table* t) {
    struct nl_builder* b = (void*)arena_alloc(sizeof(*b)); if (!b) return;
    begin_nft(b, NFT_MSG_NEWTABLE, NLM_F_MULTI, rq->seq, nl_portid(nl), t->family);
    nl_attr_string(b, NFTA_TABLE_NAME, t->name); nl_attr_be32(b, NFTA_TABLE_FLAGS, t->flags);
    nl_attr_be32(b, NFTA_TABLE_USE, 1); nl_attr_be64(b, NFTA_TABLE_HANDLE, t->handle); userdata_dump(b, 6, &t->user);
    nl_send(nl, b); arena_free((uint64_t)b, sizeof(*b));
}
static void dump_chain(struct netlink_socket* nl, const struct nlmsghdr* rq, struct ruleset* s, struct nft_chain* c) {
    struct nl_builder* b = (void*)arena_alloc(sizeof(*b)); if (!b) return;
    begin_nft(b, NFT_MSG_NEWCHAIN, NLM_F_MULTI, rq->seq, nl_portid(nl), s->tables[c->table].family);
    nl_attr_string(b, NFTA_CHAIN_TABLE, s->tables[c->table].name); nl_attr_string(b, NFTA_CHAIN_NAME, c->name);
    nl_attr_be64(b, NFTA_CHAIN_HANDLE, c->handle);
    if (c->base) {
        nl_nest_begin(b, NFTA_CHAIN_HOOK); nl_attr_be32(b, NFTA_HOOK_HOOKNUM, c->hooknum); nl_attr_be32(b, NFTA_HOOK_PRIORITY, (uint32_t)c->priority); nl_nest_end(b);
        nl_attr_be32(b, NFTA_CHAIN_POLICY, c->policy); nl_attr_string(b, NFTA_CHAIN_TYPE, c->type[0] ? c->type : "filter");
    }
    nl_attr_be32(b, NFTA_CHAIN_USE, 0); nl_attr_be32(b, NFTA_CHAIN_FLAGS, c->flags); userdata_dump(b, 12, &c->user);
    nl_send(nl, b); arena_free((uint64_t)b, sizeof(*b));
}
static void dump_rule(struct netlink_socket* nl, const struct nlmsghdr* rq, struct ruleset* s, struct nft_rule* r) {
    struct nl_builder* b = (void*)arena_alloc(sizeof(*b)); if (!b) return;
    begin_nft(b, NFT_MSG_NEWRULE, NLM_F_MULTI, rq->seq, nl_portid(nl), s->tables[r->table].family);
    nl_attr_string(b, NFTA_RULE_TABLE, s->tables[r->table].name); nl_attr_string(b, NFTA_RULE_CHAIN, s->chains[r->chain].name);
    nl_attr_be64(b, NFTA_RULE_HANDLE, r->handle);
    /* the expressions nft sent, byte for byte, except that every counter carries its live values (nft omits zero
     * counters' attributes when it loads a rule; Linux always dumps NFTA_COUNTER_BYTES/PACKETS) */
    nl_nest_begin(b, NFTA_RULE_EXPRESSIONS);
    for (unsigned at = 0; at + 4 <= r->expr_len;) {
        uint16_t size; memcpy(&size, r->expr + at, 2); if (size < 4 || size > r->expr_len - at) break;
        struct nl_attr_view expr[3]; char name[24] = "";
        if (nl_parse(r->expr + at + 4, size - 4, expr, 2) && nl_string(&expr[NFTA_EXPR_NAME], name, sizeof(name)) && !strcmp(name, "counter")) {
            nl_nest_begin(b, NFTA_LIST_ELEM); nl_attr_string(b, NFTA_EXPR_NAME, "counter");
            nl_nest_begin(b, NFTA_EXPR_DATA); nl_attr_be64(b, NFTA_COUNTER_BYTES, r->bytes); nl_attr_be64(b, NFTA_COUNTER_PACKETS, r->packets);
            nl_nest_end(b); nl_nest_end(b);
        } else nl_put(b, r->expr + at, size);
        at += (size + 3u) & ~3u;
    }
    nl_nest_end(b);
    userdata_dump(b, 7, &r->user);
    nl_send(nl, b); arena_free((uint64_t)b, sizeof(*b));
}
static void dump_set(struct netlink_socket* nl, const struct nlmsghdr* rq, struct ruleset* s, struct nft_set* set) {
    struct nl_builder* b = (void*)arena_alloc(sizeof(*b)); if (!b) return;
    begin_nft(b, NFT_MSG_NEWSET, NLM_F_MULTI, rq->seq, nl_portid(nl), s->tables[set->table].family);
    nl_attr_string(b, NFTA_SET_TABLE, s->tables[set->table].name); nl_attr_string(b, NFTA_SET_NAME, set->name);
    nl_attr_be32(b, NFTA_SET_FLAGS, set->flags); nl_attr_be32(b, NFTA_SET_KEY_TYPE, set->key_type);
    nl_attr_be32(b, NFTA_SET_KEY_LEN, set->key_len); nl_attr_be32(b, NFTA_SET_ID, (uint32_t)(set->id ? set->id : set->handle));   /* NLA_U32 (libnftnl rejects other sizes) */
    if (set->flags & NFT_SET_MAP) { nl_attr_be32(b, NFTA_SET_DATA_TYPE, set->data_type); nl_attr_be32(b, NFTA_SET_DATA_LEN, set->data_len); }
    if (set->policy) nl_attr_be32(b, 8, set->policy);
    if (set->size || set->field_count) {
        nl_nest_begin(b, 9);
        if (set->size) nl_attr_be32(b, 1, set->size);
        if (set->field_count) {
            nl_nest_begin(b, 2);
            for (unsigned i = 0; i < set->field_count; i++) { nl_nest_begin(b, NFTA_LIST_ELEM); nl_attr_be32(b, 1, set->fields[i]); nl_nest_end(b); }
            nl_nest_end(b);
        }
        nl_nest_end(b);
    }
    if (set->timeout_ms) nl_attr_be64(b, 11, set->timeout_ms);
    if (set->gc_interval) nl_attr_be32(b, 12, set->gc_interval);
    userdata_dump(b, 13, &set->user);
    nl_attr_be64(b, 16, set->handle);
    nl_send(nl, b); arena_free((uint64_t)b, sizeof(*b));
}
static void dump_setelems(struct netlink_socket* nl, const struct nlmsghdr* rq, struct ruleset* s, struct nft_set* set) {
    struct nl_builder* b = (void*)arena_alloc(sizeof(*b)); if (!b) return;
    uint64_t now = native_clock_ns(); unsigned slot = (unsigned)(set - s->sets), next = 0;
    while (next < ELEM_MAX) {   /* as many messages as the elements need (each fits one netlink message) */
        begin_nft(b, NFT_MSG_NEWSETELEM, NLM_F_MULTI, rq->seq, nl_portid(nl), s->tables[set->table].family);
        nl_attr_string(b, NFTA_SET_ELEM_LIST_TABLE, s->tables[set->table].name); nl_attr_string(b, NFTA_SET_ELEM_LIST_SET, set->name);
        nl_nest_begin(b, NFTA_SET_ELEM_LIST_ELEMENTS); unsigned in_message = 0;
        for (; next < ELEM_MAX && in_message < 64 && b->used < NL_BUILDER_BYTES - 640; next++) {
            struct nft_elem* el = &s->elems[next]; if (!elem_live(el, now) || el->set != slot) continue;
            nl_nest_begin(b, NFTA_LIST_ELEM);
            nl_nest_begin(b, NFTA_SET_ELEM_KEY); nl_attr(b, NFTA_DATA_VALUE, el->key, el->key_len); nl_nest_end(b);
            if (el->has_end) { nl_nest_begin(b, 10); nl_attr(b, NFTA_DATA_VALUE, el->key_end, el->key_len); nl_nest_end(b); }
            if (el->data_len) {
                nl_nest_begin(b, NFTA_SET_ELEM_DATA);
                if (set->data_type == NFT_DATA_VERDICT_TYPE) {
                    int32_t code; memcpy(&code, el->data, 4);
                    nl_nest_begin(b, NFTA_DATA_VERDICT); nl_attr_be32(b, NFTA_VERDICT_CODE, (uint32_t)code);
                    if (el->chain[0]) nl_attr_string(b, NFTA_VERDICT_CHAIN, el->chain);
                    nl_nest_end(b);
                } else nl_attr(b, NFTA_DATA_VALUE, el->data, el->data_len);
                nl_nest_end(b);
            }
            if (el->flags) nl_attr_be32(b, 3, el->flags);
            if (el->timeout_ms) { nl_attr_be64(b, 4, el->timeout_ms); nl_attr_be64(b, 5, el->expires_ns > now ? (el->expires_ns - now) / 1000000ULL : 0); }
            if (el->expr_len) { nl_nest_begin(b, 11); nl_put(b, el->expr, el->expr_len); nl_nest_end(b); }   /* NFTA_SET_ELEM_EXPRESSIONS */
            userdata_dump(b, 6, &el->user);
            nl_nest_end(b); in_message++;
        }
        nl_nest_end(b);
        nl_send(nl, b);
        if (!in_message) break;
    }
    arena_free((uint64_t)b, sizeof(*b));
}
static void dump_obj(struct netlink_socket* nl, const struct nlmsghdr* rq, struct ruleset* s, unsigned i) {
    struct nl_builder* b = (void*)arena_alloc(sizeof(*b)); if (!b) return;
    struct nft_obj* o = &s->objects[i];
    begin_nft(b, NFT_MSG_NEWOBJ, NLM_F_MULTI, rq->seq, nl_portid(nl), s->tables[o->table].family);
    nl_attr_string(b, NFTA_OBJ_TABLE, s->tables[o->table].name); nl_attr_string(b, NFTA_OBJ_NAME, o->name);
    nl_attr_be32(b, NFTA_OBJ_TYPE, o->type); nl_attr_be64(b, NFTA_OBJ_HANDLE, o->handle);
    nl_nest_begin(b, NFTA_OBJ_DATA);
    if (o->type == NFT_OBJECT_COUNTER) { nl_attr_be64(b, NFTA_COUNTER_BYTES, o->bytes); nl_attr_be64(b, NFTA_COUNTER_PACKETS, o->packets); }
    else if (o->type == NFT_OBJECT_QUOTA) { nl_attr_be64(b, 1, o->quota); nl_attr_be32(b, 2, o->quota_flags); nl_attr_be64(b, 4, o->consumed); }
    else { nl_attr_be64(b, 1, o->rate); nl_attr_be64(b, 2, o->unit); nl_attr_be32(b, 3, o->burst); nl_attr_be32(b, 4, o->limit_type); nl_attr_be32(b, 5, o->limit_flags); }
    nl_nest_end(b);
    userdata_dump(b, 8, &o->user);
    nl_send(nl, b); arena_free((uint64_t)b, sizeof(*b));
}
static void dump_all(struct netlink_socket* nl, const struct nlmsghdr* rq, int kind, uint8_t family, const char* table_name, const char* set_name,
                     const char* chain_name) {
    struct ruleset* s = live; if (!s) return;
    for (unsigned i = 0; i < TABLE_MAX; i++) {
        if (!s->tables[i].used || (family && s->tables[i].family != family)) continue;
        if (table_name && strcmp(s->tables[i].name, table_name)) continue;
        if (kind == NFT_MSG_GETTABLE) { dump_table(nl, rq, &s->tables[i]); continue; }
        if (kind == NFT_MSG_GETCHAIN) for (unsigned j = 0; j < CHAIN_MAX; j++) { if (s->chains[j].used && s->chains[j].table == i) dump_chain(nl, rq, s, &s->chains[j]); }
        if (kind == NFT_MSG_GETRULE) for (unsigned k = 0; k < s->ordered; k++) {
            struct nft_rule* r = &s->rules[s->order[k]];
            if (r->used && r->table == i && (!chain_name || !strcmp(s->chains[r->chain].name, chain_name))) dump_rule(nl, rq, s, r);
        }
        if (kind == NFT_MSG_GETSET) for (unsigned j = 0; j < SET_MAX; j++) { if (s->sets[j].used && s->sets[j].table == i) dump_set(nl, rq, s, &s->sets[j]); }
        if (kind == NFT_MSG_GETSETELEM) for (unsigned j = 0; j < SET_MAX; j++) { if (s->sets[j].used && s->sets[j].table == i && (!set_name || !strcmp(s->sets[j].name, set_name))) dump_setelems(nl, rq, s, &s->sets[j]); }
        if (kind == NFT_MSG_GETOBJ) for (unsigned j = 0; j < OBJ_MAX; j++) { if (s->objects[j].used && s->objects[j].table == i) dump_obj(nl, rq, s, j); }
    }
}

/* ---- batch commit / abort ---- */
static void build_order(struct ruleset* s) {
    s->ordered = 0;
    for (unsigned i = 0; i < RULE_MAX; i++) {
        if (!s->rules[i].used) continue;
        unsigned at = s->ordered++;
        while (at > 0) {
            struct nft_rule* prev = &s->rules[s->order[at - 1]], *r = &s->rules[i];
            if (prev->chain < r->chain || (prev->chain == r->chain && prev->position <= r->position)) break;
            s->order[at] = s->order[at - 1]; at--;
        }
        s->order[at] = (uint16_t)i;
    }
}
static void recount_base(void) {
    any_base = false;
    if (!live) return;
    build_order(live);
    for (unsigned i = 0; i < CHAIN_MAX; i++) if (live->chains[i].used && live->chains[i].base) { any_base = true; return; }
}
static bool rule_names_set(const struct nft_rule* r, const char* name) {
    unsigned n = (unsigned)strlen(name) + 1;
    for (unsigned i = 0; i + n <= r->expr_len; i++) if (!memcmp(r->expr + i, name, n)) return true;
    return false;
}
static void collect_anonymous(struct ruleset* s) {
    for (unsigned j = 0; j < SET_MAX; j++) {
        struct nft_set* set = &s->sets[j]; if (!set->used) continue;
        set->fresh = false;
        if (!(set->flags & 1)) continue;   /* NFT_SET_ANONYMOUS: lives as long as a rule uses it */
        bool used = false;
        for (unsigned i = 0; i < RULE_MAX && !used; i++) if (s->rules[i].used && s->rules[i].table == set->table && rule_names_set(&s->rules[i], set->name)) used = true;
        if (used) continue;
        for (unsigned i = 0; i < ELEM_MAX; i++) if (s->elems[i].used && s->elems[i].set == j) s->elems[i].used = false;
        set->used = false;
    }
}
static void commit(void) {
    if (!work) return;
    collect_anonymous(work);
    if (live) net_free(live, sizeof(*live));
    live = work; work = NULL; generation++; recount_base();
}
static void abort_batch(void) { if (work) { net_free(work, sizeof(*work)); work = NULL; } }

/* ====================================================================================================
 * nfnetlink entry point
 * ==================================================================================================== */
int nfnl_request(struct netlink_socket* nl, const struct nlmsghdr* rq, const uint8_t* payload, unsigned length,
                 bool* batch_active, uint32_t* batch_seq) {
    uint16_t type = rq->type;
    if (type == NFNL_MSG_BATCH_BEGIN) {
        if (!parse_nfgen(payload, length, &nfgen, &(const uint8_t*){0}, &(unsigned){0})) return -22;
        *batch_active = true; *batch_seq = rq->seq; abort_batch(); staging();
        batch_owner = nl; batch_failed = !work;
        return 1;   /* no ACK for the begin marker */
    }
    if (type == NFNL_MSG_BATCH_END) {
        bool mine = *batch_active && batch_owner == nl;
        if (mine && work && !batch_failed) commit(); else if (mine) abort_batch();
        if (mine) batch_owner = NULL;
        *batch_active = false;
        return 0;   /* an ACK ends the transaction (nft waits for it) */
    }
    uint8_t subsys = (uint8_t)(type >> 8), msg = (uint8_t)(type & 0xFF);
    if (subsys != NFNL_SUBSYS_NFTABLES) return -22;
    struct nfgenmsg g; const uint8_t* attrs; unsigned attr_len;
    if (!parse_nfgen(payload, length, &g, &attrs, &attr_len)) return -22;
    struct nl_attr_view a[18]; if (!nl_parse(attrs, attr_len, a, 17)) return -22;
    bool dump = (rq->flags & NLM_F_DUMP) == NLM_F_DUMP;
    switch (msg) {
    case NFT_MSG_GETGEN: {
        struct nl_builder* b = (void*)arena_alloc(sizeof(*b)); if (!b) return -12;
        begin_nft(b, NFT_MSG_NEWGEN, 0, rq->seq, nl_portid(nl), 0); nl_attr_be32(b, NFTA_GEN_ID, generation);
        nl_send(nl, b); arena_free((uint64_t)b, sizeof(*b)); return 1;
    }
    case NFT_MSG_GETTABLE: case NFT_MSG_GETCHAIN: case NFT_MSG_GETRULE: case NFT_MSG_GETSET: case NFT_MSG_GETOBJ: case NFT_MSG_GETSETELEM: {
        char table_name[32], set_name[32], chain_name[32];
        const char* tn = NULL; const char* sn = NULL; const char* cn = NULL;
        unsigned name_attr = msg == NFT_MSG_GETTABLE ? NFTA_TABLE_NAME : msg == NFT_MSG_GETCHAIN ? NFTA_CHAIN_TABLE :
                             msg == NFT_MSG_GETRULE ? NFTA_RULE_TABLE : msg == NFT_MSG_GETSETELEM ? NFTA_SET_ELEM_LIST_TABLE :
                             msg == NFT_MSG_GETOBJ ? NFTA_OBJ_TABLE : NFTA_SET_TABLE;
        if (a[name_attr].present && nl_string(&a[name_attr], table_name, sizeof(table_name))) tn = table_name;
        if (msg == NFT_MSG_GETSETELEM && a[NFTA_SET_ELEM_LIST_SET].present && nl_string(&a[NFTA_SET_ELEM_LIST_SET], set_name, sizeof(set_name))) sn = set_name;
        if (msg == NFT_MSG_GETRULE && a[NFTA_RULE_CHAIN].present && nl_string(&a[NFTA_RULE_CHAIN], chain_name, sizeof(chain_name))) cn = chain_name;
        nl_dump_begin(nl);
        dump_all(nl, rq, msg, g.family, tn, sn, cn);
        nl_send_done(nl, rq); nl_dump_end(nl);
        (void)dump; return 1;
    }
    case NFT_MSG_GETFLOWTABLE:   /* no flowtables: an empty dump */
        nl_dump_begin(nl); nl_send_done(nl, rq); nl_dump_end(nl); return 1;
    case NFT_MSG_GETOBJ_RESET: case NFT_MSG_GETRULE_RESET: case NFT_MSG_GETSETELEM_RESET:
        return -95;   /* `nft reset ...`: counters cannot be reset here */
    case NFT_MSG_NEWTABLE: case NFT_MSG_DELTABLE: case NFT_MSG_NEWCHAIN: case NFT_MSG_DELCHAIN:
    case NFT_MSG_NEWRULE: case NFT_MSG_DELRULE: case NFT_MSG_NEWSET: case NFT_MSG_DELSET:
    case NFT_MSG_NEWSETELEM: case NFT_MSG_DELSETELEM: case NFT_MSG_NEWOBJ: case NFT_MSG_DELOBJ:
    case NFT_MSG_DESTROYTABLE: case NFT_MSG_DESTROYCHAIN: case NFT_MSG_DESTROYRULE: case NFT_MSG_DESTROYSET:
    case NFT_MSG_DESTROYSETELEM: case NFT_MSG_DESTROYOBJ:
    case NFT_MSG_NEWFLOWTABLE: case NFT_MSG_DELFLOWTABLE: case NFT_MSG_DESTROYFLOWTABLE: {
        if (!*batch_active) return -95;   /* nf_tables changes only arrive in a transaction (Linux: NFNL_CB_BATCH) */
        if (batch_owner != nl) return -16;   /* another socket's batch replaced this one (EBUSY) */
        if (batch_failed) return 0;          /* an earlier message failed: the batch aborts at its end */
        struct ruleset* s = work; if (!s) return -12;
        int error = 0; bool excl = (rq->flags & NLM_F_EXCL) != 0;
        bool destroy = msg >= NFT_MSG_DESTROYTABLE && msg <= NFT_MSG_DESTROYOBJ;   /* DEL that ignores ENOENT */
        switch (msg) {
        case NFT_MSG_NEWTABLE: error = do_newtable(s, g.family, a, excl); break;
        case NFT_MSG_DELTABLE: case NFT_MSG_DESTROYTABLE: error = do_deltable(s, g.family, a); break;
        case NFT_MSG_NEWCHAIN: error = do_newchain(s, g.family, a, excl); break;
        case NFT_MSG_DELCHAIN: case NFT_MSG_DESTROYCHAIN: error = do_delchain(s, g.family, a); break;
        case NFT_MSG_NEWRULE: error = do_newrule(s, g.family, rq, a); break;
        case NFT_MSG_NEWSET: error = do_newset(s, g.family, a, excl); break;
        case NFT_MSG_DELSET: case NFT_MSG_DESTROYSET: error = do_delset(s, g.family, a); break;
        case NFT_MSG_NEWSETELEM: error = do_setelem(s, g.family, a, true, excl); break;
        case NFT_MSG_DELSETELEM: case NFT_MSG_DESTROYSETELEM: error = do_setelem(s, g.family, a, false, false); break;
        case NFT_MSG_NEWOBJ: error = do_newobj(s, g.family, a, excl); break;
        case NFT_MSG_DELRULE: case NFT_MSG_DESTROYRULE: error = do_delrule(s, g.family, a); break;
        case NFT_MSG_DELOBJ: case NFT_MSG_DESTROYOBJ: error = do_delobj(s, g.family, a); break;
        default: error = -95; break;   /* flowtables */
        }
        if (destroy && error == -2) error = 0;
        if (error < 0) { abort_batch(); batch_failed = true; }   /* nothing of this batch commits */
        return error;
    }
    default:   /* a message this kernel does not implement: fail it (and the batch it is in), never ACK it */
        if (*batch_active && batch_owner == nl && !batch_failed) { abort_batch(); batch_failed = true; }
        return -95;
    }
}
void nf_init(void) { live = NULL; work = NULL; generation = 1; any_base = false; batch_failed = false; batch_owner = NULL; }
/* expired elements of timeout sets free their slots (lookups already ignore them) */
void nf_poll(uint64_t now) {
    static uint64_t next; if (!live || now < next) return; next = now + 1000000000ULL;
    for (unsigned i = 0; i < ELEM_MAX; i++) if (live->elems[i].used && live->elems[i].expires_ns && live->elems[i].expires_ns <= now) live->elems[i].used = false;
}

/* IPv4 reject helper (ICMP destination/port unreachable back to the source) */
int ip4_output(uint32_t src, uint32_t dst, uint8_t protocol, const void* payload, unsigned length, const struct ip6_send_options* options);
extern int ip4_hold_unresolved;   /* net4.c: wait for ARP instead of losing the packet */
void icmp4_reject(const uint8_t* packet, unsigned length, uint8_t code, struct netdev* in) {
    (void)in;
    if (length < 20) return;
    uint32_t src = be32(packet + 12), dst = be32(packet + 16);
    if ((be32(packet + 16) >> 28) == 14 || dst == 0xFFFFFFFFU) return;   /* no reject to multicast/broadcast */
    unsigned ihl = (packet[0] & 15) * 4u, quote = length < ihl + 8 ? length : ihl + 8;
    uint8_t body[8 + 28]; memset(body, 0, 8); body[0] = 3; body[1] = code; memcpy(body + 8, packet, quote);
    ip4_hold_unresolved++; ip4_output(dst, src, IPPROTO_ICMP, body, 8 + quote, NULL); ip4_hold_unresolved--;
}
/* `reject with tcp reset`: answer the offending segment with an RST (never an RST to an RST), Linux nf_send_reset. */
void tcp_reset_for(const struct nf_packet* p) {
    if (p->fragment || p->l4_offset + 20 > p->length) return;
    const uint8_t* t = p->data + p->l4_offset; uint8_t flags = t[13];
    if (flags & 0x04) return;
    unsigned header = (t[12] >> 4) * 4u, payload = p->length - p->l4_offset > header ? p->length - p->l4_offset - header : 0;
    uint8_t rst[20]; memset(rst, 0, sizeof(rst));
    put16(rst, be16(t + 2)); put16(rst + 2, be16(t));
    if (flags & 0x10) { put32(rst + 4, be32(t + 8)); rst[13] = 0x04; }
    else { put32(rst + 8, be32(t + 4) + payload + ((flags & 0x02) ? 1 : 0) + ((flags & 0x01) ? 1 : 0)); rst[13] = 0x14; }
    rst[12] = 5 << 4;
    if (p->family == AF_INET6) {
        struct ip6a src, dst; memcpy(&src, p->data + 24, 16); memcpy(&dst, p->data + 8, 16);   /* we answer from the address it was sent to */
        put16(rst + 16, ip6_checksum(&src, &dst, IPPROTO_TCP, rst, 20));
        struct ip6_send_options o; ip6_options_default(&o); o.source = &src; if (p->in) o.oif = ip6a_linklocal(&dst) ? p->in->index : 0;
        ip6_send(&dst, IPPROTO_TCP, rst, 20, &o);
    } else {
        uint32_t src = be32(p->data + 16), dst = be32(p->data + 12);
        uint32_t sum = (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + IPPROTO_TCP + 20;
        put16(rst + 16, net_fold(net_sum(sum, rst, 20)));
        ip4_hold_unresolved++; ip4_output(src, dst, IPPROTO_TCP, rst, 20, NULL); ip4_hold_unresolved--;
    }
}
