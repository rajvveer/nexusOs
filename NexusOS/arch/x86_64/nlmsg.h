/* Netlink message construction shared by rtnetlink (netlink.c), generic netlink families (wireguard.c) and
 * nfnetlink (nft.c). A builder holds one message; nested attributes are opened and closed in place. */
#ifndef NEXUS_NLMSG_H
#define NEXUS_NLMSG_H
#include "netdev.h"
struct nlmsghdr { uint32_t len; uint16_t type, flags; uint32_t seq, pid; };
#define NLMSG_NOOP 1
#define NLMSG_ERROR 2
#define NLMSG_DONE 3
#define NLM_F_REQUEST 1
#define NLM_F_MULTI 2
#define NLM_F_ACK 4
#define NLM_F_ECHO 8
#define NLM_F_ROOT 0x100
#define NLM_F_MATCH 0x200
#define NLM_F_DUMP 0x300
#define NLM_F_REPLACE 0x100
#define NLM_F_EXCL 0x200
#define NLM_F_CREATE 0x400
#define NLM_F_APPEND 0x800
#define NLA_F_NESTED 0x8000
#define NLA_TYPE_MASK 0x3FFF
#define NL_BUILDER_BYTES 8192
struct nl_builder { uint8_t buffer[NL_BUILDER_BYTES]; unsigned used; bool overflow; unsigned nest[8], depth; };
void nl_begin(struct nl_builder* b, uint16_t type, uint16_t flags, uint32_t seq, uint32_t pid);
void nl_put(struct nl_builder* b, const void* data, unsigned size);   /* padded to 4 bytes */
void nl_attr(struct nl_builder* b, uint16_t type, const void* data, unsigned size);
void nl_attr8(struct nl_builder* b, uint16_t type, uint8_t value);
void nl_attr16(struct nl_builder* b, uint16_t type, uint16_t value);
void nl_attr32(struct nl_builder* b, uint16_t type, uint32_t value);
void nl_attr64(struct nl_builder* b, uint16_t type, uint64_t value);
void nl_attr_be32(struct nl_builder* b, uint16_t type, uint32_t value);
void nl_attr_be64(struct nl_builder* b, uint16_t type, uint64_t value);
void nl_attr_string(struct nl_builder* b, uint16_t type, const char* text);
void nl_nest_begin(struct nl_builder* b, uint16_t type);
void nl_nest_end(struct nl_builder* b);
void nl_finish(struct nl_builder* b);                                    /* sets nlmsg_len */
/* Attribute parsing: index attributes of a payload by type (up to `max`); false on a malformed list. */
struct nl_attr_view { const uint8_t* data; uint16_t length; bool present; };
bool nl_parse(const uint8_t* data, unsigned length, struct nl_attr_view* table, unsigned max);
uint32_t nl_u32(const struct nl_attr_view* a);
uint16_t nl_u16(const struct nl_attr_view* a);
uint8_t nl_u8(const struct nl_attr_view* a);
uint32_t nl_be32(const struct nl_attr_view* a);
bool nl_string(const struct nl_attr_view* a, char* out, unsigned capacity);
/* A socket being served (opaque to the families). */
struct netlink_socket;
void nl_send(struct netlink_socket* socket, struct nl_builder* b);      /* queue the built message to the requester */
void nl_send_error(struct netlink_socket* socket, const struct nlmsghdr* request, int error);
void nl_send_done(struct netlink_socket* socket, const struct nlmsghdr* request);
uint32_t nl_portid(struct netlink_socket* socket);
void nl_dump_begin(struct netlink_socket* socket);                      /* pack a multi-part reply; DONE closes it */
void nl_dump_end(struct netlink_socket* socket);
void nl_multicast(int protocol, unsigned group, struct nl_builder* b, struct netlink_socket* except);   /* to group members */
void nl_unicast(struct netlink_socket* socket, struct nl_builder* b);   /* an event to one socket (nl80211 frame registrations) */
/* Family hooks */
int genl_request(struct netlink_socket* socket, const struct nlmsghdr* request, const uint8_t* payload, unsigned length);
int nl80211_request(struct netlink_socket* socket, const struct nlmsghdr* request, const uint8_t* payload, unsigned length);   /* nl80211.c */
int nl80211_family_reply(struct netlink_socket* socket, const struct nlmsghdr* request);
bool nl80211_family_id(uint16_t id);
void nl80211_socket_closed(struct netlink_socket* socket);                 /* registrations / SOCKET_OWNER of a closing socket */
int nfnl_request(struct netlink_socket* socket, const struct nlmsghdr* request, const uint8_t* payload, unsigned length,
                 bool* batch_active, uint32_t* batch_seq);
#endif
