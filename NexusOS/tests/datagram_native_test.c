/* Production native datagram queues (arch/x86_64/inet.c) with deterministic allocation, output, clock and routes. */
#include "../kernel/types.h"
#undef NULL
#include <stdio.h>
#include <stdlib.h>
#undef NDEBUG
#include <assert.h>
#define NEXUS_NATIVE64 1
#include "../arch/x86_64/inet.c"
static uint64_t clock_ns;
static unsigned allocated, wakes, sent, random_value, local_errors;
static bool fail_allocation, invalid_checksum;
static int output_error;
uint64_t arena_alloc(uint64_t size) { if(fail_allocation)return 0;void* p=calloc(1,size);if(p)allocated+=(size+4095)/4096;return (uint64_t)p; }
void arena_free(uint64_t address,uint64_t size) { assert(address&&allocated>=(size+4095)/4096);allocated-=(size+4095)/4096;free((void*)address); }
uint64_t page_alloc(void) { return arena_alloc(4096); }
void page_free(uint64_t address) { arena_free(address,4096); }
void* net_alloc(unsigned size) { return (void*)arena_alloc(size); }
void net_free(void* p,unsigned size) { arena_free((uint64_t)p,size); }
void panic(const char* message) { fprintf(stderr,"panic: %s\n",message);abort(); }
uint64_t native_clock_ns(void) { return clock_ns; }
uint64_t native_realtime_ns(void) { return 1700000000000000000ULL+clock_ns; }
bool native_network_available(void) { return true; }
static uint64_t net_namespace;   /* CLONE_NEWNET (namespaces.c): non-zero = isolated, loopback only */
uint64_t task_net_namespace(void) { return net_namespace; }
bool native_random(void* data,uint64_t size) { assert(size==4);*(uint32_t*)data=++random_value;return true; }
uint32_t ip_get_addr(void) { return 0x0A00020F; }
uint16_t ip_checksum(const void* data,uint16_t length) { (void)data;assert(length==20);return invalid_checksum?1:0; }
const net_config_t* ip_get_config(void) { static const net_config_t config={0x0A00020F,0x0A000202,0xFFFFFF00,0x0A000203};return &config; }
int ip_is_local(uint32_t ip) { return (ip>>24)==127||ip==0x0A00020F; }
uint32_t ip_source_address(uint32_t ip) { return (ip>>24)==127?0x7F000001:0x0A00020F; }
/* IPv4 output of a UDP datagram "abc" to 10.0.2.2:9999 from an ephemeral port */
int ip4_output(uint32_t src,uint32_t dst,uint8_t protocol,const void* payload,unsigned length,const struct ip6_send_options* options) {
    const uint8_t* p=payload;assert(src==0x0A00020F&&dst==0x0A000202&&protocol==17&&length==11&&options&&!options->raw_header);
    assert(be16(p)>=32768&&be16(p+2)==9999&&be16(p+4)==11&&be16(p+6)&&!memcmp(p+8,"abc",3));
    if(output_error)return output_error;sent++;return 0;
}
int ip6_send(const struct ip6a* dst,uint8_t next,const void* payload,unsigned length,const struct ip6_send_options* options) { (void)dst;(void)next;(void)payload;(void)length;(void)options;assert(0);return 0; }
void ip6_options_default(struct ip6_send_options* o) { memset(o,0,sizeof(*o));o->hop_limit=-1;o->tclass=-1;o->mc_loop=true; }
int route_lookup(uint8_t family,const struct ip6a* dst,const struct ip6a* src,unsigned oif,uint32_t mark,struct route_result* out) {
    (void)src;(void)oif;(void)mark;memset(out,0,sizeof(*out));assert(family==AF_INET&&ip6a_v4(dst)==0x0A000202);out->mtu=1500;out->source=ip6a_from4(0x0A00020F);return 0;
}
unsigned route_mtu(uint8_t family,const struct ip6a* dst,struct netdev* dev) { (void)family;(void)dst;(void)dev;return 1500; }
bool source_select6(const struct ip6a* dst,struct netdev* dev,uint32_t preferences,struct ip6a* out) { (void)dst;(void)dev;(void)preferences;(void)out;assert(0);return false; }
bool addrconf_local(const struct ip6a* addr,unsigned scope,bool tentative) { (void)addr;(void)scope;(void)tentative;return false; }
uint16_t ip6_checksum(const struct ip6a* s,const struct ip6a* d,uint8_t next,const void* data,unsigned length) { (void)s;(void)d;(void)next;(void)data;(void)length;assert(0);return 0; }
uint32_t net_sum(uint32_t sum,const void* data,unsigned length) { const uint8_t* p=data;while(length>=2){sum+=(uint32_t)p[0]<<8|p[1];p+=2;length-=2;}if(length)sum+=(uint32_t)p[0]<<8;return sum; }
uint16_t net_fold(uint32_t sum) { while(sum>>16)sum=(sum&0xFFFF)+(sum>>16);return (uint16_t)~sum; }
uint32_t ip6h_get(const struct ip6a* a,uint32_t scope) { (void)a;(void)scope;assert(0);return 0; }
uint32_t ip6h_find(const struct ip6a* a,uint32_t scope) { (void)a;(void)scope;assert(0);return 0; }
bool ip6h_address(uint32_t h,struct ip6a* a,uint32_t* scope) { (void)h;(void)a;(void)scope;assert(0);return false; }
struct netdev* netdevs; struct netdev* netdev_eth0; struct netdev* netdev_rx_current;
const struct ip6a ip6a_unspecified = {{0}}, ip6a_loopback_address = {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}};
struct netdev* netdev_by_index(unsigned index) { (void)index;return NULL; }
struct netdev* netdev_by_name(const char* name) { (void)name;return NULL; }
struct netdev* netdev_v4_owner(uint32_t address) { (void)address;return NULL; }
bool netdev_v4_local(uint32_t address) { return ip_is_local(address); }
int mc6_join(struct netdev* d,const struct ip6a* g,bool k) { (void)d;(void)g;(void)k;assert(0);return 0; }
int mc6_leave(struct netdev* d,const struct ip6a* g,bool k) { (void)d;(void)g;(void)k;assert(0);return 0; }
int mc4_join(struct netdev* d,uint32_t g) { (void)d;(void)g;assert(0);return 0; }
int mc4_leave(struct netdev* d,uint32_t g) { (void)d;(void)g;assert(0);return 0; }
/* mcast.c (multicast memberships): no socket here joins a group */
int mc_sock_change(struct mc_membership** l,unsigned* c,int op,struct netdev* d,const struct ip6a* g,const struct ip6a* s) { (void)l;(void)c;(void)op;(void)d;(void)g;(void)s;assert(0);return 0; }
int mc_sock_set_filter(struct mc_membership** l,unsigned* c,struct netdev* d,const struct ip6a* g,uint8_t m,const struct ip6a* s,unsigned n) { (void)l;(void)c;(void)d;(void)g;(void)m;(void)s;(void)n;assert(0);return 0; }
int mc_sock_get_filter(struct mc_membership* l,unsigned i,const struct ip6a* g,uint8_t* m,struct ip6a* o,unsigned cap,unsigned* n) { (void)l;(void)i;(void)g;(void)m;(void)o;(void)cap;(void)n;assert(0);return 0; }
void mc_sock_drop_all(struct mc_membership** l,unsigned* c) { assert(!*l);(void)c; }
void mc_sock_dev_gone(struct mc_membership** l,unsigned* c,unsigned i) { (void)l;(void)c;(void)i; }
bool mc_sock_allows(struct mc_membership* l,bool all,const struct ip6a* g,unsigned i,const struct ip6a* s) { (void)l;(void)g;(void)i;(void)s;return all; }
void pmtu_update(uint8_t family,const struct ip6a* dst,unsigned mtu) { assert(family==AF_INET&&ip6a_v4(dst)==0x0A000202&&mtu==1300); }
bool inet_wireguard_port(uint16_t port,uint8_t family) { (void)port;(void)family;return false; }
bool wireguard_receive(const struct inet_rx_info* info,const void* data,unsigned length) { (void)info;(void)data;(void)length;return false; }
void icmp4_port_unreachable(uint32_t s,uint32_t d,uint16_t sp,uint16_t dp,const void* data,unsigned length) { (void)s;(void)d;(void)sp;(void)dp;(void)data;(void)length;local_errors++; }
void tcp_pmtu_update(uint32_t l,uint16_t lp,uint32_t r,uint16_t rp,unsigned mtu) { (void)l;(void)lp;(void)r;(void)rp;(void)mtu;assert(0); }
void tcp_icmp_error(uint32_t l,uint16_t lp,uint32_t r,uint16_t rp,uint32_t seq,int e) { (void)l;(void)lp;(void)r;(void)rp;(void)seq;(void)e;assert(0); }
void tcp_set_listen_family(tcp_conn_t* c,uint8_t f) { (void)c;(void)f;assert(0); }
int netif_ioctl_family(int family,uint64_t command,uint64_t argument) { (void)family;(void)command;(void)argument;assert(0);return 0; }
bool netlink_admin(void) { return false; }
bool net_raw_capable(void) { return false; }
void uint_to_str(uint32_t value,char* text) { sprintf(text,"%u",value); }
int32_t sysctl_ping_group_low=0,sysctl_ping_group_high=2147483647,sysctl_ip_default_ttl=64,sysctl_local_port_low=32768,sysctl_local_port_high=60999;
int32_t sysctl_ip_nonlocal_bind,sysctl_ipv6_nonlocal_bind,sysctl_bindv6only;
void task_wake_channel(void* channel) { assert(channel);wakes++; }
void event_source_changed(void* source,uint32_t events) { (void)events;assert(source); }
int task_wait_until(void* channel,uint64_t deadline) { assert(channel);clock_ns=deadline;return -110; }
bool accessible(uint64_t address,uint64_t size,bool write) { (void)write;return !size||address>4096; }
bool smap_enabled;   /* uaccess.h: SMAP is off in the host build */
uint64_t uaccess_copy(void* to,const void* from,uint64_t n) { if(n)memcpy(to,from,n);return 0; }
uint64_t uaccess_clear(void* to,uint64_t n) { if(n)memset(to,0,n);return 0; }
int64_t strncpy_from_user(char* out,uint64_t address,uint64_t capacity) {
    for(uint64_t i=0;i<capacity;i++){ out[i]=((const char*)address)[i]; if(!out[i])return (int64_t)i; } return -36; }
int64_t native_random_user(uint64_t address,uint64_t length) { memset((void*)address,0,length);return (int64_t)length; }
bool iov_access(const struct iovec64* vectors,unsigned count,uint64_t skip,uint64_t length,bool write) {
    assert(count==1&&!skip);return length<=vectors[0].size&&accessible(vectors[0].address,length,write);
}
void iov_copy(const struct iovec64* vectors,unsigned count,uint64_t skip,uint8_t* data,unsigned length,bool write) {
    assert(count==1&&!skip&&length<=vectors[0].size);if(!length)return;
    if(write)memcpy(data,(void*)vectors[0].address,length);else memcpy((void*)vectors[0].address,data,length);
}
#define UNUSED(...) assert(0)
int tcp_send_space(const tcp_conn_t* connection) { (void)connection;assert(0);return 0; }
bool tcp_port_in_use(uint16_t port,uint32_t ip) { (void)port;(void)ip;assert(0);return false; }
bool tcp_tuple_in_use(uint32_t local,uint16_t port,uint32_t ip,uint16_t remote) { (void)local;(void)port;(void)ip;(void)remote;assert(0);return false; }
tcp_conn_t* tcp_listen_socket(uint32_t ip,uint16_t port,unsigned backlog) { (void)ip;(void)port;(void)backlog;assert(0);return NULL; }
tcp_conn_t* tcp_accept(tcp_conn_t* listener) { (void)listener;assert(0);return NULL; }
bool tcp_accept_ready(const tcp_conn_t* listener) { (void)listener;assert(0);return false; }
tcp_conn_t* tcp_connect_from(uint32_t from,uint32_t ip,uint16_t remote,uint16_t local,uint32_t receive,uint32_t send) { (void)from;(void)ip;(void)remote;(void)local;(void)receive;(void)send;assert(0);return NULL; }
void tcp_reset(tcp_conn_t* connection) { (void)connection;assert(0); }
void tcp_shutdown_read(tcp_conn_t* connection) { (void)connection;assert(0); }
void tcp_close(tcp_conn_t* connection) { (void)connection;assert(0); }
void tcp_release(tcp_conn_t* connection) { (void)connection;assert(0); }
void tcp_collect(void) { assert(0); }
void tcp_retain(tcp_conn_t* c) { (void)c;assert(0); }
void tcp_set_nodelay(tcp_conn_t* c,bool enabled) { (void)c;(void)enabled;assert(0); }
uint32_t tcp_buffer_size(uint64_t requested,uint32_t fallback) { (void)requested;(void)fallback;assert(0);return 0; }
uint32_t tcp_rx_span(const tcp_conn_t* c,uint32_t offset,const uint8_t** data) { (void)c;(void)offset;(void)data;assert(0);return 0; }
void tcp_rx_consume(tcp_conn_t* c,uint32_t bytes) { (void)c;(void)bytes;assert(0); }
uint32_t tcp_tx_span(const tcp_conn_t* c,uint32_t offset,uint8_t** data) { (void)c;(void)offset;(void)data;assert(0);return 0; }
void tcp_tx_commit(tcp_conn_t* c,uint32_t bytes) { (void)c;(void)bytes;assert(0); }
void tcp_set_keepalive(tcp_conn_t* c,bool enabled,uint32_t idle,uint32_t interval,uint32_t count) { (void)c;(void)enabled;(void)idle;(void)interval;(void)count;assert(0); }
struct signal_state* signals_current(void) { assert(0);return NULL; }
uint32_t task_pid(void) { assert(0);return 0; }
uint32_t task_visible_pid(uint32_t pid) { (void)pid;assert(0);return 0; }
uint32_t task_kernel_uid(void) { return 1000; }
int signals_enqueue(struct signal_state* state,uint64_t number,int code,uint32_t sender,uint32_t uid,int status,bool group) {
    (void)state;(void)number;(void)code;(void)sender;(void)uid;(void)status;(void)group;assert(0);return 0;
}
int fd_create_inet(struct inet_socket* socket,uint64_t flags) { (void)socket;(void)flags;assert(0);return -1; }
struct open_file* fd_lookup(uint64_t fd) { (void)fd;assert(0);return NULL; }
struct inet_socket* fd_inet(struct open_file* file,uint64_t* flags) { (void)file;(void)flags;assert(0);return NULL; }
void fd_pin(struct open_file* file) { (void)file;assert(0); }
void fd_unpin(struct open_file* file) { (void)file;assert(0); }
struct credentials* credentials_current(void) { return NULL; }
uint32_t credentials_kernel_id(struct credentials* c,bool group,unsigned which) { (void)c;(void)group;(void)which;return 1000; }
uint32_t credentials_visible_id(uint32_t id,bool group) { (void)group;return id; }
struct pid_namespace* pid_namespace_root(void) { assert(0);return NULL; }
bool credentials_capable(struct credentials* credentials,struct user_namespace* space,unsigned capability) {
    (void)credentials;(void)space;(void)capability;assert(0);return false;
}
static struct inet_socket* endpoint(void) {
    struct inet_socket* s=(void*)page_alloc();assert(s);s->family=AF_INET;s->kind=KIND_DGRAM;s->protocol=17;s->datagram=true;s->send_limit=s->receive_limit=8192;
    s->local=s->bound=s->remote=ip6a_from4(0);s->ttl=-1;s->mcast_ttl=1;s->pmtudisc4=1;s->checksum_offset=-1;
    s->local_port=unused_port(s);s->next=sockets;sockets=s;return s;
}
static void receive(uint32_t source,uint16_t port,const char* text,unsigned length,uint16_t to) {
    inet_datagram_receive(source,ip_get_addr(),port,to,text,(uint16_t)length);
}
int main(void) {
    struct inet_socket* s=endpoint();struct endpoint peer={.address=ip6a_from4(0x0A000202),.port=9999,.family=AF_INET};
    struct iovec64 vector={(uint64_t)"abc",3};
    net_namespace=1;assert(datagram_send(s,&vector,1,3,DONTWAIT,&peer,NULL)==-101&&!s->send_used&&!sent);net_namespace=0;   /* isolated netns: ENETUNREACH */
    output_error=-2;s->send_limit=4100;
    assert(datagram_send(s,&vector,1,3,DONTWAIT,&peer,NULL)==3&&s->send_used==4096&&!sent&&!(inet_ready(s)&4));s->send_limit=8192;
    assert(datagram_send(s,&vector,1,3,DONTWAIT,&peer,NULL)==3&&s->send_used==8192&&!(inet_ready(s)&4));
    assert(datagram_send(s,&vector,1,3,DONTWAIT,&peer,NULL)==-11);
    output_error=0;clock_ns+=50000000;inet_poll();assert(sent==2&&!s->transmit&&!s->send_used&&(inet_ready(s)&4));
    fail_allocation=true;assert(datagram_send(s,&vector,1,3,DONTWAIT,&peer,NULL)==-12);fail_allocation=false;
    /* IP_PMTUDISC_DO: beyond the 1500-byte path MTU at once (EMSGSIZE); the default fragments instead */
    static char big[1473];struct iovec64 large={(uint64_t)big,sizeof(big)};s->pmtudisc4=2;
    assert(datagram_send(s,&large,1,sizeof(big),DONTWAIT,&peer,NULL)==-90&&!s->transmit);s->pmtudisc4=1;
    s->remote=ip6a_from4(0x0A000202);s->remote_port=9999;
    /* another peer reaches no socket (the only one is connected elsewhere): port unreachable, as Linux */
    receive(0x0A000202,9998,"wrong",5,s->local_port);assert(!s->received);
    receive(0x0A000203,9999,"wrong",5,s->local_port);assert(!s->received&&local_errors==2);local_errors=0;
    receive(0x0A000202,9999,"hello",5,s->local_port);
    receive(0x0A000202,9999,"second",6,s->local_port);
    receive(0x0A000202,9999,"dropped",7,s->local_port);assert(s->receive_used==8192&&allocated==3);
    char output[16]={0};vector=(struct iovec64){(uint64_t)output,3};struct inet_address source;struct message64 message={.name=(uint64_t)&source,.name_size=sizeof(source)};
    assert(datagram_read(s,&vector,1,3,DONTWAIT|PEEK|TRUNCATE,&message)==5&&message.flags==TRUNCATE&&message.name_size==16&&source.port==htons(9999)&&s->receive_used==8192&&!memcmp(output,"hel",3));
    vector.address=1;assert(datagram_read(s,&vector,1,3,DONTWAIT,NULL)==-14&&s->receive_used==8192);
    vector.address=(uint64_t)output;assert(datagram_read(s,&vector,1,3,DONTWAIT,NULL)==3&&s->receive_used==4096);
    vector.size=sizeof(output);assert(datagram_read(s,&vector,1,sizeof(output),DONTWAIT,NULL)==6&&!memcmp(output,"second",6)&&!s->received&&allocated==1);
    fail_allocation=true;receive(0x0A000202,9999,"oom",3,s->local_port);assert(!s->received);fail_allocation=false;
    receive(0x0A000202,9999,NULL,0,s->local_port);
    assert((inet_ready(s)&1)&&!inet_available(s));assert(datagram_read(s,&vector,1,0,DONTWAIT,NULL)==0&&!s->received);
    assert(datagram_read(s,&vector,1,1,DONTWAIT,NULL)==-11);
    s->receive_timeout=120000000;assert(datagram_read(s,&vector,1,1,0,NULL)==-11);
    /* no socket for a datagram: ICMP port unreachable (net4.c) */
    receive(0x0A000202,9999,"none",4,s->local_port+1);assert(local_errors==1);
    vector=(struct iovec64){(uint64_t)"abc",3};output_error=-2;s->transmit_retry=0;
    assert(datagram_send(s,&vector,1,3,DONTWAIT,&peer,NULL)==3);inet_close(s);assert(inet_background_pages()==2&&allocated==2);
    output_error=0;clock_ns+=50000000;inet_poll();assert(!allocated&&!sockets&&sent==3);
    s=endpoint();output_error=-2;assert(datagram_send(s,&vector,1,3,DONTWAIT,&peer,NULL)==3);inet_close(s);
    clock_ns+=5000000000ULL;inet_poll();assert(!allocated&&!sockets&&sent==3);
    s=endpoint();s->receive_errors=true;s->remote=ip6a_from4(0x0A000202);s->remote_port=9999;
    uint8_t quote[39]={3,3};ip_header_t* ip=(void*)(quote+8);udp_header_t* udp=(void*)(quote+28);
    ip->ver_ihl=0x45;ip->protocol=17;ip->src_ip=htonl(ip_get_addr());ip->dst_ip=htonl(0x0A000202);ip->total_len=htons(31);ip->flags_frag=htons(0x4000);
    udp->src_port=htons(s->local_port);udp->dst_port=htons(9999);udp->length=htons(11);memcpy(quote+36,"abc",3);
    inet_icmp_error(0x0A000202,quote,27);assert(!s->errors);
    invalid_checksum=true;inet_icmp_error(0x0A000202,quote,sizeof(quote));assert(!s->errors);invalid_checksum=false;
    ip->src_ip=htonl(0x0A000210);inet_icmp_error(0x0A000202,quote,sizeof(quote));assert(!s->errors);ip->src_ip=htonl(ip_get_addr());
    udp->dst_port=htons(9998);inet_icmp_error(0x0A000202,quote,sizeof(quote));assert(!s->errors);udp->dst_port=htons(9999);
    ip->flags_frag=htons(1);inet_icmp_error(0x0A000202,quote,sizeof(quote));assert(!s->errors);ip->flags_frag=htons(0x4000);
    inet_icmp_error(0x0A000202,quote,sizeof(quote));assert(s->error==111&&s->errors&&(inet_ready(s)&8));
    assert(take_error(s)==111&&(inet_ready(s)&8));
    uint8_t ancillary[64];message=(struct message64){.name=(uint64_t)&source,.name_size=sizeof(source),.control=(uint64_t)ancillary,.control_size=sizeof(ancillary)};
    vector=(struct iovec64){(uint64_t)output,sizeof(output)};
    message.control=1;assert(datagram_read(s,&vector,1,sizeof(output),ERROR_QUEUE,&message)==-14&&s->errors);
    message.control=(uint64_t)ancillary;message.control_size=sizeof(ancillary);
    assert(datagram_read(s,&vector,1,sizeof(output),ERROR_QUEUE,&message)==3&&!memcmp(output,"abc",3)&&message.flags==ERROR_QUEUE&&message.control_size==48);
    assert(*(uint64_t*)ancillary==48&&*(int32_t*)(ancillary+8)==0&&*(int32_t*)(ancillary+12)==11);
    struct extended_error* report=(void*)(ancillary+16);assert(report->number==111&&report->origin==2&&report->type==3&&report->code==3);
    struct inet_address* offender=(void*)(ancillary+32);assert(offender->family==2&&offender->ip==htonl(0x0A000202)&&source.port==htons(9999));
    assert(!(inet_ready(s)&8)&&datagram_read(s,&vector,1,10,ERROR_QUEUE,&message)==-11);
    quote[1]=4;quote[6]=5;quote[7]=0x14;inet_icmp_error(0x0A000202,quote,sizeof(quote));assert(s->error==90&&s->errors->error.info==1300);
    message=(struct message64){0};assert(datagram_read(s,&vector,1,1,ERROR_QUEUE|PEEK|TRUNCATE,&message)==1&&(message.flags&(ERROR_QUEUE|TRUNCATE|8))==(ERROR_QUEUE|TRUNCATE|8));
    assert(!s->errors&&!s->error);
    /* sockets sharing a port on different addresses: the error goes to the one bound to the quoted source address */
    struct inet_socket* other=endpoint();other->local_port=s->local_port;other->local=other->bound=ip6a_from4(0x7F000001);other->receive_errors=true;
    s->local=s->bound=ip6a_from4(ip_get_addr());quote[1]=3;assert(sockets==other);
    inet_icmp_error(0x0A000202,quote,sizeof(quote));assert(!other->errors&&!other->error&&s->errors&&s->error==111);
    inet_close(other);
    inet_close(s);assert(!allocated&&!sockets);
    assert(wakes);puts("PASS native datagrams: queued ARP retry, send/receive memory pressure, OOM, PMTUDISC_DO EMSGSIZE, peer filter, peek/truncation/empty packets, timeout, port unreachable, orphan delivery/reaping, ICMP errors by local address");
}
