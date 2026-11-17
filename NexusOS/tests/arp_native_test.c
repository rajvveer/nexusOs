/* Native ARP: an entry in use is re-requested before it expires (the native
 * kernel resolves with IF=0, so a cache miss drops the packet). */
#include "../kernel/types.h"
#undef NULL
#include <stdio.h>
#undef NDEBUG
#include <assert.h>
#define NEXUS_NATIVE64 1
#define IRQFLAGS_H
static bool irqs_enabled(void) { return false; }
#include "../kernel/net.c"
#include "../kernel/arp.c"
volatile uint32_t system_ticks;
static unsigned frames, requests;
static uint8_t last[ETH_FRAME_MAX];
void vga_print(const char* text) { (void)text; }
void vga_print_color(const char* text,uint8_t color) { (void)text;(void)color; }
void tcp_timer(void) {}
void ip_handle_packet(const void* data,uint16_t size) { (void)data;(void)size; }
/* the native interface layer's eth0 taps (arch/x86_64/netdev.c): nothing consumed here */
bool netdev_receive_eth0(const void* data,unsigned length) { (void)data;(void)length;return false; }
void netdev_receive_done(void) {}
void netdev_transmit_tap(net_device_t* device,const void* data,unsigned length) { (void)device;(void)data;(void)length; }
uint32_t ip_get_addr(void) { return 0x0A00020F; }
static int send_frame(net_device_t* device,const void* data,uint16_t size) {
    (void)device;assert(size<=sizeof(last));memcpy(last,data,size);frames++;
    const arp_packet_t* arp=(const void*)(last+ETH_HEADER_LEN);if(ntohs(arp->opcode)==ARP_OP_REQUEST)requests++;
    return 0;
}
int main(void) {
    net_init();arp_init();system_ticks=100;
    net_device_t device={.name="test0",.send=send_frame,.mac={{0x52,0x54,0,1,2,3}}};assert(net_register_device(&device)==0);
    arp_packet_t reply={.hw_type=htons(1),.proto_type=htons(0x800),.hw_len=6,.proto_len=4,.opcode=htons(ARP_OP_REPLY),
        .sender_mac={0x52,0x55,10,0,2,2},.sender_ip=htonl(0x0A000202),.target_ip=htonl(0x0A00020F)};
    arp_handle_packet(&reply,sizeof(reply));
    mac_addr_t mac;assert(arp_resolve(0x0A000202,&mac)&&!frames&&!memcmp(mac.addr,reply.sender_mac,6));
    system_ticks+=ARP_CACHE_TTL/2*18+1;
    assert(arp_resolve(0x0A000202,&mac)&&frames==1&&requests==1&&!memcmp(last,reply.sender_mac,6));   /* unicast refresh */
    assert(arp_resolve(0x0A000202,&mac)&&frames==1);                                                     /* rate-limited */
    system_ticks+=19;assert(arp_resolve(0x0A000202,&mac)&&frames==2);
    arp_handle_packet(&reply,sizeof(reply));system_ticks+=19;assert(arp_resolve(0x0A000202,&mac)&&frames==2);   /* young again */
    system_ticks+=ARP_CACHE_TTL*18+18;assert(!arp_resolve(0x0A000202,&mac)&&requests==3);                 /* expired: miss never blocks */
    puts("PASS native ARP: in-use entries refresh before expiry (rate-limited unicast request), misses never block");
}
