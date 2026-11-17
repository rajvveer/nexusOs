#include "../kernel/types.h"
#undef NULL
#include <stdio.h>
#undef NDEBUG
#include <assert.h>
#define IRQFLAGS_H
static bool irqs_enabled(void) { return false; }
#include "../kernel/net.c"
#include "../kernel/arp.c"
#include "../kernel/ip.c"
#include "../kernel/firewall.c"
volatile uint32_t system_ticks;
static unsigned packets,deliveries;
static uint8_t sent[ETH_FRAME_MAX];static uint16_t sent_size;
void vga_print(const char* text) { (void)text; }
void vga_print_color(const char* text,uint8_t color) { (void)text;(void)color; }
void int_to_str(int value,char* text) { sprintf(text,"%d",value); }
void uint_to_str(uint32_t value,char* text) { sprintf(text,"%u",value); }
void tcp_timer(void) {}
void tcp_handle_packet(uint32_t ip,const void* data,uint16_t size) { assert(ip==0x0A000202&&size==4&&!memcmp(data,"TEST",4));deliveries++; }
void tcp_handle_packet_to(uint32_t ip,uint32_t destination,const void* data,uint16_t size) { assert(destination==0x0A00020F);tcp_handle_packet(ip,data,size); }
void udp_handle_packet(uint32_t ip,uint32_t destination,const void* data,uint16_t size) { (void)ip;(void)destination;(void)data;(void)size;assert(0); }
void icmp_handle_packet(uint32_t ip,const void* data,uint16_t size) { (void)ip;(void)data;(void)size;assert(0); }
static int send_frame(net_device_t* device,const void* data,uint16_t size) { (void)device;assert(size<=sizeof(sent));memcpy(sent,data,size);sent_size=size;packets++;return 0; }
int main(void) {
    net_init();arp_init();ip_init();firewall_init();system_ticks=1;
    net_device_t device={.name="test0",.send=send_frame,.mac={{0x52,0x54,0,1,2,3}}};assert(net_register_device(&device)==0);
    arp_packet_t arp={.hw_type=htons(1),.proto_type=htons(0x800),.hw_len=6,.proto_len=4,.opcode=htons(2),.sender_mac={0x52,0x54,0,4,5,6},.sender_ip=htonl(0x0A000202),.target_ip=htonl(0x0A00020F)};
    mac_addr_t mac;
    arp.hw_len=5;arp_handle_packet(&arp,sizeof(arp));assert(!arp_lookup(0x0A000202,&mac));arp.hw_len=6;
    arp.proto_len=5;arp_handle_packet(&arp,sizeof(arp));assert(!arp_lookup(0x0A000202,&mac));arp.proto_len=4;
    arp.opcode=htons(9);arp_handle_packet(&arp,sizeof(arp));assert(!arp_lookup(0x0A000202,&mac));arp.opcode=htons(2);
    arp_handle_packet(&arp,sizeof(arp)-1);assert(!arp_lookup(0x0A000202,&mac));
    arp_handle_packet(&arp,sizeof(arp));assert(arp_lookup(0x0A000202,&mac)&&!memcmp(mac.addr,arp.sender_mac,6));
    uint8_t frame[ETH_HEADER_LEN+24]={0};eth_header_t* eth=(void*)frame;eth->ethertype=htons(ETH_TYPE_IPV4);
    ip_header_t* ip=(void*)(frame+ETH_HEADER_LEN);ip->ver_ihl=0x45;ip->total_len=htons(24);ip->ttl=64;ip->protocol=6;ip->src_ip=htonl(0x0A000202);ip->dst_ip=htonl(ip_get_addr());memcpy(frame+ETH_HEADER_LEN+20,"TEST",4);
    const uint16_t fragments[]={0x2000,1,0x8000,0x6000};
    for(unsigned i=0;i<sizeof(fragments)/sizeof(*fragments);i++){ip->flags_frag=htons(fragments[i]);ip->checksum=0;ip->checksum=ip_checksum(ip,20);net_receive_packet(frame,sizeof(frame));net_poll();assert(!deliveries);}
    ip->flags_frag=htons(0x4000);ip->checksum=0;ip->checksum=ip_checksum(ip,20);
    net_receive_packet(frame,sizeof(frame));net_poll();assert(deliveries==1);
    ip->checksum^=1;net_receive_packet(frame,sizeof(frame));net_poll();assert(deliveries==1);ip->checksum^=1;
    ip->total_len=htons(19);ip->checksum=0;ip->checksum=ip_checksum(ip,20);net_receive_packet(frame,sizeof(frame));net_poll();assert(deliveries==1);
    assert(ip_send_packet(0x0A000202,6,"TEST",4)==0&&packets==1&&sent_size==ETH_HEADER_LEN+24);
    assert(!memcmp(((eth_header_t*)sent)->dest,arp.sender_mac,6));
    assert(ip_send_packet(0x0A000202,6,NULL,1)==-1&&ip_send_packet(0x0A000202,6,"TEST",65535)==-1&&packets==1);
    for(unsigned i=0;i<NET_RX_RING_SIZE;i++)net_receive_packet(frame,sizeof(frame));
    assert(device.stats.rx_dropped==1);
    net_packet_t* packet=net_get_rx_packet();assert(packet&&packet->length==sizeof(frame)&&!memcmp(packet->data,frame,sizeof(frame)));
    puts("PASS network wire checks: ARP shape/opcode, IPv4 fragments/checksum/length, output bounds and RX overflow");
}
