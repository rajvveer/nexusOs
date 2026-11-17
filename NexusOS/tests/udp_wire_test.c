/* Production UDP parser/output: known checksum, malformed lengths, full MTU. */
#include "../kernel/types.h"
#undef NULL
#include <stdio.h>
#undef NDEBUG
#include <assert.h>
#define NEXUS_NATIVE64 1
#include "../kernel/udp.c"
static unsigned delivered, emitted, received_size;
static uint8_t output[1500], received[1500];
void vga_print(const char* text) { (void)text; }
void vga_print_color(const char* text,uint8_t color) { (void)text;(void)color; }
void inet_datagram_receive(uint32_t source,uint32_t destination,uint16_t source_port,uint16_t destination_port,const void* data,uint16_t size) {
    assert(source==0x0A000202&&destination==0x0A00020F&&source_port==53&&destination_port==48879&&size<=sizeof(received));
    delivered++;received_size=size;memcpy(received,data,size);
}
uint32_t ip_get_addr(void) { return 0x0A00020F; }
int ip_send_packet(uint32_t destination,uint8_t protocol,const void* data,uint16_t size) {
    assert(destination==0x0A000202&&protocol==17&&size<=sizeof(output));memcpy(output,data,size);emitted++;return 0;
}
uint32_t ip_source_address(uint32_t destination) { (void)destination;return 0x0A00020F; }
int ip_send_packet_from(uint32_t source,uint32_t destination,uint8_t protocol,const void* data,uint16_t size) {
    assert(source==0x0A00020F);return ip_send_packet(destination,protocol,data,size);
}
void native_dhcp_receive(const void* data,uint16_t size) { (void)data;(void)size;assert(0); }
int main(void) {
    udp_init();
    uint8_t packet[]={0,53,0xBE,0xEF,0,13,0xE4,0xCC,'h','e','l','l','o',0xAA,0xBB};
    udp_handle_packet(0x0A000202,0x0A00020F,packet,7);assert(!delivered);
    packet[5]=7;udp_handle_packet(0x0A000202,0x0A00020F,packet,13);assert(!delivered);
    packet[5]=14;udp_handle_packet(0x0A000202,0x0A00020F,packet,13);assert(!delivered);
    packet[5]=13;udp_handle_packet(0x0A000202,0x0A00020F,packet,sizeof(packet));
    assert(delivered==1&&received_size==5&&!memcmp(received,"hello",5));
    packet[12]^=1;udp_handle_packet(0x0A000202,0x0A00020F,packet,13);assert(delivered==1);packet[12]^=1;
    udp_handle_packet(0x0A000203,0x0A00020F,packet,13);assert(delivered==1);
    udp_handle_packet(0x0A000202,0xFFFFFFFF,packet,13);assert(delivered==1);
    packet[6]=packet[7]=0;udp_handle_packet(0x0A000202,0x0A00020F,packet,13);assert(delivered==2);
    packet[5]=8;udp_handle_packet(0x0A000202,0x0A00020F,packet,8);assert(delivered==3&&!received_size);
    assert(udp_send(0x0A000202,48879,53,"hello",5)==0&&emitted==1);
    assert(output[6]==0xE4&&output[7]==0xCC&&!memcmp(output+8,"hello",5));
    assert(udp_send(0x0A000202,48879,53,NULL,1)==-22&&emitted==1);
    assert(udp_send(0x0A000202,48879,53,received,1473)==-90&&emitted==1);
    memset(received,0xA7,1472);assert(udp_send(0x0A000202,48879,53,received,1472)==0&&emitted==2);
    assert(!udp_checksum(0x0A00020F,0x0A000202,output,1480)&&!memcmp(output+8,received,1472));
    assert(udp_send(0x0A000202,48879,53,NULL,0)==0&&emitted==3&&!udp_checksum(0x0A00020F,0x0A000202,output,8));
    puts("PASS UDP wire: known odd-byte checksum, corrupt/pseudo-header/length rejection, zero checksum/empty datagram, full MTU and output bounds");
}
