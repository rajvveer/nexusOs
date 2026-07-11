/* ============================================================================
 * NexusOS — Packet-Filter Firewall (Implementation) — Phase 44
 * ============================================================================
 * Stateless first-match rule list. Kept O(rules) and small (≤32) so it stays
 * cheap in net_poll/ip context (the RX path budgets only a few packets/tick).
 * ============================================================================ */

#include "firewall.h"
#include "vga.h"

static fw_rule_t rules[FW_MAX_RULES];
static bool      enabled = false;
static uint8_t   default_policy = FW_ACCEPT;
static uint32_t  stat_dropped = 0;
static uint32_t  stat_passed = 0;

void firewall_init(void) {
    for (int i = 0; i < FW_MAX_RULES; i++) rules[i].used = false;
    enabled = false;                 /* off by default — opt-in */
    default_policy = FW_ACCEPT;
    stat_dropped = stat_passed = 0;

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Firewall ready (packet filter, disabled by default)\n");
}

void firewall_set_enabled(bool on) { enabled = on; }
bool firewall_enabled(void) { return enabled; }
void firewall_set_default(uint8_t action) {
    default_policy = (action == FW_DROP) ? FW_DROP : FW_ACCEPT;
}
uint8_t firewall_default(void) { return default_policy; }

int firewall_add(uint8_t action, uint8_t direction, uint8_t proto,
                 uint32_t addr, uint16_t port) {
    for (int i = 0; i < FW_MAX_RULES; i++) {
        if (!rules[i].used) {
            rules[i].used = true;
            rules[i].action = (action == FW_DROP) ? FW_DROP : FW_ACCEPT;
            rules[i].direction = direction;
            rules[i].proto = proto;
            rules[i].addr = addr;
            rules[i].port = port;
            return i;
        }
    }
    return -1;   /* full */
}

bool firewall_del(int index) {
    if (index < 0 || index >= FW_MAX_RULES || !rules[index].used) return false;
    rules[index].used = false;
    return true;
}

void firewall_clear(void) {
    for (int i = 0; i < FW_MAX_RULES; i++) rules[i].used = false;
}

int firewall_count(void) {
    int n = 0;
    for (int i = 0; i < FW_MAX_RULES; i++) if (rules[i].used) n++;
    return n;
}

const fw_rule_t* firewall_rule(int index) {
    if (index < 0 || index >= FW_MAX_RULES || !rules[index].used) return NULL;
    return &rules[index];
}

uint32_t firewall_dropped(void) { return stat_dropped; }
uint32_t firewall_passed(void) { return stat_passed; }

/* Does this rule apply to a packet with the given direction/proto/remote/port? */
static bool rule_matches(const fw_rule_t* r, uint8_t direction, uint8_t proto,
                         uint32_t remote, uint16_t port) {
    if (r->direction != FW_BOTH && r->direction != direction) return false;
    if (r->proto != FW_PROTO_ANY && r->proto != proto) return false;
    if (r->addr != 0 && r->addr != remote) return false;
    if (r->port != 0 && r->port != port) return false;
    return true;
}

bool firewall_check(uint8_t direction, uint8_t proto, uint32_t remote, uint16_t port) {
    if (!enabled) return false;      /* pass everything when disabled */

    for (int i = 0; i < FW_MAX_RULES; i++) {
        if (!rules[i].used) continue;
        if (rule_matches(&rules[i], direction, proto, remote, port)) {
            if (rules[i].action == FW_DROP) { stat_dropped++; return true; }
            stat_passed++;
            return false;            /* explicit ACCEPT */
        }
    }
    /* No rule matched — apply default policy. */
    if (default_policy == FW_DROP) { stat_dropped++; return true; }
    stat_passed++;
    return false;
}
