/* ============================================================================
 * NexusOS — DHCP Client — Phase 30
 * ============================================================================
 * DISCOVER → OFFER → REQUEST → ACK handshake to auto-configure IP.
 * Uses UDP broadcast on ports 67 (server) / 68 (client).
 * ============================================================================ */

#include "dhcp.h"
#include "udp.h"
#include "ip.h"
#include "net.h"
#include "vga.h"
#include "string.h"
#include "keyboard.h"

/* System tick counter */
extern volatile uint32_t system_ticks;

/* DHCP state */
static dhcp_state_t dhcp_state = DHCP_IDLE;
static uint32_t dhcp_xid = 0;

/* Offered configuration */
static uint32_t offered_ip = 0;
static uint32_t offered_mask = 0;
static uint32_t offered_gateway = 0;
static uint32_t offered_dns = 0;
static uint32_t offered_server = 0;
static uint32_t offered_lease = 0;

/* --------------------------------------------------------------------------
 * dhcp_init
 * -------------------------------------------------------------------------- */
void dhcp_init(void) {
    dhcp_state = DHCP_IDLE;
    dhcp_xid = 0;
    offered_ip = 0;

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("DHCP client initialized\n");
}

/* --------------------------------------------------------------------------
 * add_option: Append a DHCP option to the options buffer
 * Returns new offset.
 * -------------------------------------------------------------------------- */
static int add_option(uint8_t* opts, int off, uint8_t code,
                      uint8_t len, const void* data) {
    opts[off++] = code;
    opts[off++] = len;
    if (data && len > 0) {
        memcpy(opts + off, data, len);
        off += len;
    }
    return off;
}

/* --------------------------------------------------------------------------
 * build_msg: Build a DHCP message (DISCOVER or REQUEST)
 * -------------------------------------------------------------------------- */
static int build_msg(dhcp_msg_t* msg, uint8_t msgtype) {
    memset(msg, 0, sizeof(dhcp_msg_t));

    msg->op    = 1;  /* BOOTREQUEST */
    msg->htype = 1;  /* Ethernet */
    msg->hlen  = 6;  /* MAC length */
    msg->hops  = 0;
    msg->xid   = htonl(dhcp_xid);
    msg->secs  = 0;
    msg->flags = htons(0x8000); /* Broadcast flag */

    /* Client hardware address (our MAC) */
    net_device_t* dev = net_get_device(0);
    if (dev) {
        memcpy(msg->chaddr, dev->mac.addr, 6);
    }

    /* Magic cookie */
    msg->cookie = htonl(DHCP_MAGIC_COOKIE);

    /* Options */
    int off = 0;

    /* Option 53: DHCP Message Type */
    off = add_option(msg->options, off, DHCP_OPT_MSGTYPE, 1, &msgtype);

    /* Option 55: Parameter Request List */
    if (msgtype == DHCP_DISCOVER || msgtype == DHCP_REQUEST) {
        uint8_t params[] = { DHCP_OPT_SUBNET, DHCP_OPT_ROUTER,
                             DHCP_OPT_DNS, DHCP_OPT_LEASE };
        off = add_option(msg->options, off, DHCP_OPT_PARAMLIST,
                         sizeof(params), params);
    }

    /* For REQUEST: include requested IP and server identifier */
    if (msgtype == DHCP_REQUEST && offered_ip != 0) {
        uint32_t req_ip = htonl(offered_ip);
        off = add_option(msg->options, off, DHCP_OPT_REQIP, 4, &req_ip);

        if (offered_server != 0) {
            uint32_t srv = htonl(offered_server);
            off = add_option(msg->options, off, DHCP_OPT_SERVERID, 4, &srv);
        }
    }

    /* Option 12: Hostname */
    const char* hostname = "NexusOS";
    off = add_option(msg->options, off, DHCP_OPT_HOSTNAME,
                     strlen(hostname), hostname);

    /* End */
    msg->options[off++] = DHCP_OPT_END;

    /* Return total message size (fixed header + options used) */
    return (int)(sizeof(dhcp_msg_t) - sizeof(msg->options) + off);
}

/* --------------------------------------------------------------------------
 * parse_options: Parse DHCP options from response
 * -------------------------------------------------------------------------- */
static uint8_t parse_options(const dhcp_msg_t* msg, int msg_len) {
    uint8_t msgtype = 0;
    int opt_len = msg_len - (int)(sizeof(dhcp_msg_t) - sizeof(msg->options));
    if (opt_len <= 0) opt_len = sizeof(msg->options);

    int i = 0;
    while (i < opt_len && i < (int)sizeof(msg->options)) {
        uint8_t code = msg->options[i++];
        if (code == DHCP_OPT_END) break;
        if (code == 0) continue;  /* Padding */

        if (i >= opt_len) break;
        uint8_t len = msg->options[i++];
        if (i + len > opt_len) break;

        switch (code) {
            case DHCP_OPT_MSGTYPE:
                if (len >= 1) msgtype = msg->options[i];
                break;
            case DHCP_OPT_SUBNET:
                if (len >= 4) {
                    offered_mask = ((uint32_t)msg->options[i] << 24) |
                                   ((uint32_t)msg->options[i+1] << 16) |
                                   ((uint32_t)msg->options[i+2] << 8) |
                                   msg->options[i+3];
                }
                break;
            case DHCP_OPT_ROUTER:
                if (len >= 4) {
                    offered_gateway = ((uint32_t)msg->options[i] << 24) |
                                      ((uint32_t)msg->options[i+1] << 16) |
                                      ((uint32_t)msg->options[i+2] << 8) |
                                      msg->options[i+3];
                }
                break;
            case DHCP_OPT_DNS:
                if (len >= 4) {
                    offered_dns = ((uint32_t)msg->options[i] << 24) |
                                  ((uint32_t)msg->options[i+1] << 16) |
                                  ((uint32_t)msg->options[i+2] << 8) |
                                  msg->options[i+3];
                }
                break;
            case DHCP_OPT_LEASE:
                if (len >= 4) {
                    offered_lease = ((uint32_t)msg->options[i] << 24) |
                                    ((uint32_t)msg->options[i+1] << 16) |
                                    ((uint32_t)msg->options[i+2] << 8) |
                                    msg->options[i+3];
                }
                break;
            case DHCP_OPT_SERVERID:
                if (len >= 4) {
                    offered_server = ((uint32_t)msg->options[i] << 24) |
                                     ((uint32_t)msg->options[i+1] << 16) |
                                     ((uint32_t)msg->options[i+2] << 8) |
                                     msg->options[i+3];
                }
                break;
        }
        i += len;
    }
    return msgtype;
}

/* --------------------------------------------------------------------------
 * wait_for_reply: Wait for a DHCP reply on port 68
 * Returns msgtype received, or 0 on timeout.
 * -------------------------------------------------------------------------- */
static uint8_t wait_for_reply(dhcp_msg_t* reply, int* reply_len) {
    uint32_t start = system_ticks;

    while ((system_ticks - start) < DHCP_TIMEOUT_TICKS) {
        /* Check for Escape abort */
        if (keyboard_has_key()) {
            char k = keyboard_getchar();
            if (k == 27) return 0;
        }

        udp_datagram_t* dgram = udp_recv(DHCP_CLIENT_PORT);
        if (!dgram) {
            net_poll(); /* Process packets — not in timer ISR */
            __asm__ volatile("hlt");
            continue;
        }

        /* Check minimum size */
        if (dgram->length < 240) continue;

        /* Copy to reply buffer */
        int len = dgram->length;
        if (len > (int)sizeof(dhcp_msg_t)) len = sizeof(dhcp_msg_t);
        memcpy(reply, dgram->data, len);
        *reply_len = len;

        /* Verify transaction ID */
        if (ntohl(reply->xid) != dhcp_xid) continue;

        /* Verify it's a reply */
        if (reply->op != 2) continue;

        /* Extract offered IP */
        offered_ip = ntohl(reply->yiaddr);

        /* Parse options */
        return parse_options(reply, len);
    }

    return 0; /* Timeout */
}

/* --------------------------------------------------------------------------
 * dhcp_request: Perform full DHCP handshake
 * -------------------------------------------------------------------------- */
bool dhcp_request(void) {
    net_device_t* dev = net_get_device(0);
    if (!dev || !dev->link_up) {
        vga_print_color("  DHCP: No network device\n",
                        VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        dhcp_state = DHCP_FAILED;
        return false;
    }

    /* Generate transaction ID */
    dhcp_xid = system_ticks * 0x1234 + 0xDEAD;

    /* Bind client port */
    udp_bind(DHCP_CLIENT_PORT);

    /* Save old config and set IP to 0.0.0.0 for broadcast */
    const net_config_t* old_cfg = ip_get_config();
    uint32_t saved_ip = old_cfg->ip;
    uint32_t saved_gw = old_cfg->gateway;
    uint32_t saved_mask = old_cfg->subnet_mask;
    uint32_t saved_dns = old_cfg->dns;

    /* Reset offers */
    offered_ip = 0;
    offered_mask = 0xFFFFFF00;
    offered_gateway = 0;
    offered_dns = 0;
    offered_server = 0;
    offered_lease = 0;

    /* --- Phase 1: DISCOVER --- */
    vga_print_color("  DHCP: Discovering...\n",
                    VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    dhcp_state = DHCP_DISCOVERING;

    dhcp_msg_t msg;
    int msg_len = build_msg(&msg, DHCP_DISCOVER);
    udp_send(0xFFFFFFFF, DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
             &msg, msg_len);

    /* Wait for OFFER */
    dhcp_msg_t reply;
    int reply_len = 0;
    uint8_t rtype = wait_for_reply(&reply, &reply_len);

    if (rtype != DHCP_OFFER) {
        vga_print_color("  DHCP: No offer received\n",
                        VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        /* Restore config */
        ip_set_config(saved_ip, saved_gw, saved_mask, saved_dns);
        udp_unbind(DHCP_CLIENT_PORT);
        dhcp_state = DHCP_FAILED;
        return false;
    }

    char ip_str[16];
    ip_to_string(offered_ip, ip_str);
    vga_print_color("  DHCP: Offered ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print(ip_str);
    vga_print("\n");

    /* --- Phase 2: REQUEST --- */
    vga_print_color("  DHCP: Requesting...\n",
                    VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    dhcp_state = DHCP_REQUESTING;

    msg_len = build_msg(&msg, DHCP_REQUEST);
    udp_send(0xFFFFFFFF, DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
             &msg, msg_len);

    /* Wait for ACK */
    rtype = wait_for_reply(&reply, &reply_len);

    if (rtype != DHCP_ACK) {
        vga_print_color("  DHCP: Request rejected\n",
                        VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        ip_set_config(saved_ip, saved_gw, saved_mask, saved_dns);
        udp_unbind(DHCP_CLIENT_PORT);
        dhcp_state = DHCP_FAILED;
        return false;
    }

    /* --- Apply configuration --- */
    if (offered_mask == 0) offered_mask = 0xFFFFFF00;
    if (offered_gateway == 0) offered_gateway = saved_gw;
    if (offered_dns == 0) offered_dns = saved_dns;

    ip_set_config(offered_ip, offered_gateway, offered_mask, offered_dns);
    dhcp_state = DHCP_BOUND;

    /* Print result */
    vga_print_color("  DHCP: Bound to ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    ip_to_string(offered_ip, ip_str);
    vga_print(ip_str);
    vga_print("\n");

    vga_print_color("  GW: ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    ip_to_string(offered_gateway, ip_str);
    vga_print(ip_str);

    vga_print_color("  DNS: ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    ip_to_string(offered_dns, ip_str);
    vga_print(ip_str);

    if (offered_lease > 0) {
        char n[12];
        vga_print_color("  Lease: ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
        int_to_str(offered_lease, n);
        vga_print(n);
        vga_print("s");
    }
    vga_print("\n");

    udp_unbind(DHCP_CLIENT_PORT);
    return true;
}

/* --------------------------------------------------------------------------
 * dhcp_get_state
 * -------------------------------------------------------------------------- */
dhcp_state_t dhcp_get_state(void) {
    return dhcp_state;
}
