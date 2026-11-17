/* Production TCP, with simulated allocation, entropy, clock and IPv4 output. */
#include "../kernel/types.h"
#undef NULL
#include <stdio.h>
#include <stdlib.h>
#undef NDEBUG
#include <assert.h>
#define IRQFLAGS_H
static uintptr_t irq_save(void) { return 0; }
static void irq_restore(uintptr_t flags) { (void)flags; }
static bool irqs_enabled(void) { return false; }
#define NEXUS_NATIVE64 1
#include "../kernel/tcp.c"
static unsigned allocations,sent,random_calls;
static bool allocation_failure,entropy_failure;static uint64_t allocation_limit;
static uint8_t last_packet[1600];static unsigned last_size;
volatile uint32_t system_ticks;
uint64_t arena_alloc(uint64_t size) { if(allocation_failure||(allocation_limit&&size>allocation_limit))return 0;void* p=calloc(1,(size_t)size);if(p)allocations++;return (uint64_t)p; }
void arena_free(uint64_t address,uint64_t size) { (void)size;assert(allocations&&address);allocations--;free((void*)address); }
void panic(const char* reason) { fprintf(stderr,"panic: %s\n",reason);abort(); }
static uint64_t clock_ns=1000000000ULL;
uint64_t native_clock_ns(void) { return clock_ns; }
bool native_random(void* data,uint64_t size) { assert(size==4);if(entropy_failure)return false;*(uint32_t*)data=0xA1020000+ ++random_calls*7919;return true; }
void vga_print(const char* text) { (void)text; }
void vga_print_color(const char* text,uint8_t color) { (void)text;(void)color; }
void net_poll(void) { tcp_timer(); }
bool net_is_busy(void) { return false; }
uint32_t ip_get_addr(void) { return 0x0A00020F; }
/* IPv6 address handles never occur in these IPv4 segments; the route MSS is Ethernet's. */
uint16_t ip_route_mss(uint32_t local,uint32_t remote) { (void)local;assert(!tcp_handle6(remote));return 1460; }
uint16_t tcp_checksum6(uint32_t src,uint32_t dst,const void* data,uint16_t length) { (void)src;(void)dst;(void)data;(void)length;assert(0);return 0xFFFF; }
/* Optional wire capture: reassemble the payload of every sent data segment. */
static uint8_t wire[1<<20];static uint32_t wire_base;static bool capture;static unsigned data_segments;
int ip_send_packet(uint32_t destination,uint8_t protocol,const void* payload,uint16_t length) {
    assert(destination==0x0A000202&&protocol==6&&length<=sizeof(last_packet));memcpy(last_packet,payload,length);last_size=length;sent++;
    const tcp_header_t* h=payload;unsigned header=(h->data_off>>4)*4;
    if(capture&&length>header){uint32_t at=ntohl(h->seq_num)-wire_base;assert(at+length-header<=sizeof(wire));memcpy(wire+at,(const uint8_t*)payload+header,length-header);data_segments++;}
    return 0;
}
/* Source-addressed output: ring_full refuses everything (-11); 127/8 goes to a loopback ring model of loop_room slots. */
static bool ring_full;static unsigned refused,loop_room=128,loop_sent;
int ip_is_local(uint32_t ip) { return (ip>>24)==127; }
int ip_loopback_room(void) { return (int)loop_room; }
uint32_t ip_source_address(uint32_t destination) { return (destination>>24)==127?0x7F000001:0x0A00020F; }
int ip_send_packet_from(uint32_t source,uint32_t destination,uint8_t protocol,const void* payload,uint16_t length) {
    if((destination>>24)==127){
        assert(source==0x7F000001&&protocol==6&&length<=sizeof(last_packet));
        if(ring_full||!loop_room){refused++;return -11;}
        loop_room--;loop_sent++;memcpy(last_packet,payload,length);last_size=length;sent++;return 0;
    }
    assert(source==0x0A00020F);if(ring_full){refused++;return -11;}
    return ip_send_packet(destination,protocol,payload,length);
}
static void segment(tcp_conn_t* c,uint32_t seq,uint32_t ack,uint8_t flags,uint16_t window,const uint8_t* options,unsigned option_bytes,const uint8_t* data,unsigned count) {
    static uint8_t bytes[1600];memset(bytes,0,sizeof(bytes));unsigned header=20+option_bytes;assert(header+count<=sizeof(bytes)&&option_bytes%4==0);
    tcp_header_t* h=(void*)bytes;h->src_port=htons(c->remote_port);h->dst_port=htons(c->local_port);h->seq_num=htonl(seq);h->ack_num=htonl(ack);
    h->data_off=(uint8_t)((header/4)<<4);h->flags=flags;h->window=htons(window);
    if(option_bytes)memcpy(bytes+20,options,option_bytes);if(count)memcpy(bytes+header,data,count);
    h->checksum=tcp_checksum(c->remote_ip,c->local_ip,bytes,(uint16_t)(header+count));
    tcp_handle_packet_to(c->remote_ip,c->local_ip,bytes,(uint16_t)(header+count));
}
static uint8_t pattern(uint32_t i) { return (uint8_t)(i*37+11); }
static void patterned(uint8_t* out,uint32_t offset,unsigned count) { for(unsigned i=0;i<count;i++)out[i]=pattern(offset+i); }
static uint32_t last_ack(void) { return ntohl(((tcp_header_t*)last_packet)->ack_num); }
static uint32_t last_seq(void) { return ntohl(((tcp_header_t*)last_packet)->seq_num); }
static uint16_t last_window(void) { return ntohs(((tcp_header_t*)last_packet)->window); }
/* Peer sends `count` patterned bytes of stream offset `offset` (relative to base). */
static void data_at(tcp_conn_t* c,uint32_t base,uint32_t offset,unsigned count,uint8_t flags) {
    uint8_t bytes[1460];assert(count<=sizeof(bytes));patterned(bytes,offset,count);
    segment(c,base+offset,c->snd_nxt,(uint8_t)(TCP_FLAG_ACK|flags),65535,NULL,0,bytes,count);
}
static void drain_check(tcp_conn_t* c,uint32_t offset,uint32_t count) {
    static uint8_t out[1<<20];assert(count<=sizeof(out));
    assert(tcp_recv(c,out,count)==(int)count);
    for(uint32_t i=0;i<count;i++)assert(out[i]==pattern(offset+i));
}
static void scaling_reassembly_acks(void) {
    /* Window scaling: offered on our SYN, applied both ways once the SYN-ACK echoes it. */
    tcp_conn_t* c=tcp_connect_buffers(0x0A000202,16000,50001,1<<20,64*1024);assert(c);tcp_retain(c);
    assert(c->rx_cap==(1u<<20)&&c->tx_cap==64*1024&&c->rcv_wscale==5);
    assert(last_size==28&&last_packet[20]==2&&last_packet[24]==1&&last_packet[25]==3&&last_packet[26]==3&&last_packet[27]==5);
    assert(last_window()==65535&&(((tcp_header_t*)last_packet)->flags==TCP_FLAG_SYN));
    const uint8_t ws7[]={2,4,0x05,0xB4,1,3,3,7};
    clock_ns+=250000;segment(c,0x50000000,c->snd_nxt,TCP_FLAG_SYN|TCP_FLAG_ACK,1000,ws7,8,NULL,0);
    assert(c->state==TCP_ESTABLISHED&&c->wscale_ok&&c->snd_wscale==7&&c->rcv_wscale==5&&c->snd_wnd==1000&&c->srtt_us==250);
    assert(last_size==20&&last_window()==(1u<<20)>>5&&c->rcv_wnd==(1u<<20)&&c->rcv_adv==c->rcv_nxt+(1u<<20));
    segment(c,c->rcv_nxt,c->snd_nxt,TCP_FLAG_ACK,512,NULL,0,NULL,0);assert(c->snd_wnd==512u<<7);
    /* Upload through the zero-copy send ring: 60000 bytes go out at once in the 64 KiB scaled window. */
    capture=true;wire_base=c->snd_nxt;data_segments=0;uint32_t queued=0;uint8_t* span;
    while(queued<60000){uint32_t n=tcp_tx_span(c,0,&span);assert(n);if(n>60000-queued)n=60000-queued;patterned(span,queued,n);tcp_tx_commit(c,n);queued+=n;}
    assert(c->tx_unacked==60000&&data_segments==(60000+1459)/1460&&c->rtt_timing);
    /* Half is acknowledged: the ring head advances, the next 30000 bytes wrap around the 64 KiB ring. */
    clock_ns+=400000;segment(c,c->rcv_nxt,wire_base+30000,TCP_FLAG_ACK,512,NULL,0,NULL,0);
    assert(c->tx_len==30000&&c->tx_unacked==30000&&c->tx_head==30000&&c->bytes_acked==30000&&!c->rtt_timing&&c->srtt_us>250);
    while(queued<90000){uint32_t n=tcp_tx_span(c,0,&span);assert(n);if(n>90000-queued)n=90000-queued;patterned(span,queued,n);tcp_tx_commit(c,n);queued+=n;}
    assert(c->tx_unacked==60000&&tcp_send_space(c)==64*1024-60000);
    for(uint32_t i=0;i<90000;i++)assert(wire[i]==pattern(i));
    /* Three duplicate ACKs retransmit the head segment from the wrapped ring. */
    unsigned before=sent;memset(wire,0,1460);
    for(int i=0;i<3;i++)segment(c,c->rcv_nxt,wire_base+30000,TCP_FLAG_ACK,512,NULL,0,NULL,0);
    assert(sent==before+1&&last_seq()==wire_base+30000&&c->total_retrans==1);
    segment(c,c->rcv_nxt,wire_base+90000,TCP_FLAG_ACK,512,NULL,0,NULL,0);assert(!c->tx_len&&!c->tx_unacked&&c->bytes_acked==90000);
    capture=false;
    /* Receive: an in-order pair inside one batch is acknowledged once; every out-of-order
     * segment gets its own immediate duplicate ACK (the sender counts them). */
    uint32_t base=c->rcv_nxt;before=sent;
    tcp_batch_begin();data_at(c,base,0,1460,0);data_at(c,base,1460,1460,0);assert(sent==before);
    data_at(c,base,4380,1460,0);assert(c->ooo_count==1&&c->rx_len==2920&&c->rcv_nxt==base+2920&&sent==before+1&&last_ack()==base+2920);
    data_at(c,base,5840,1460,0);assert(c->ooo_count==1&&sent==before+2&&last_ack()==base+2920);
    data_at(c,base,4380,1460,0);assert(sent==before+3&&last_ack()==base+2920);   /* third duplicate */
    tcp_batch_end();assert(sent==before+3);
    /* The gap fills: the stored range is absorbed and acknowledged at once. */
    tcp_batch_begin();data_at(c,base,2920,1460,0);assert(sent==before+3);tcp_batch_end();
    assert(!c->ooo_count&&c->rcv_nxt==base+7300&&c->rx_len==7300&&sent==before+4&&last_ack()==base+7300&&c->ooo_packets==3);
    drain_check(c,0,7300);
    /* Several holes, a duplicate and an overlapping retransmission. */
    base=c->rcv_nxt;tcp_batch_begin();
    data_at(c,base,8000,1000,0);data_at(c,base,3000,1000,0);data_at(c,base,5000,1000,0);
    assert(c->ooo_count==3);data_at(c,base,3900,1100,0);
    assert(c->ooo_count==2&&c->ooo[0].start==base+3000&&c->ooo[0].end==base+6000&&c->ooo[1].start==base+8000);
    data_at(c,base,0,1460,0);data_at(c,base,1460,1460,0);data_at(c,base,2920,1460,0);data_at(c,base,0,1000,0);
    assert(c->rcv_nxt==base+6000&&c->ooo_count==1);
    data_at(c,base,6000,1460,0);data_at(c,base,7460,1460,0);tcp_batch_end();
    assert(c->rcv_nxt==base+9000&&!c->ooo_count&&last_ack()==base+9000);drain_check(c,0,9000);
    /* Delayed ACK for a lone small segment; PSH is acknowledged immediately. */
    base=c->rcv_nxt;before=sent;data_at(c,base,0,100,0);assert(sent==before&&c->delack_ns);
    clock_ns+=TCP_DELACK_NS/2;tcp_timer();assert(sent==before);
    clock_ns+=TCP_DELACK_NS;tcp_timer();assert(sent==before+1&&last_ack()==base+100&&!c->delack_ns);
    data_at(c,base,100,50,TCP_FLAG_PSH);assert(sent==before+2&&last_ack()==base+150);drain_check(c,0,150);
    /* A FIN beyond a gap waits for the gap. */
    base=c->rcv_nxt;data_at(c,base,1000,500,TCP_FLAG_FIN);assert(c->fin_pending&&!c->closed&&c->state==TCP_ESTABLISHED);
    data_at(c,base,0,1000,0);assert(c->closed&&c->state==TCP_CLOSE_WAIT&&c->rcv_nxt==base+1501&&!c->fin_pending&&last_ack()==base+1501);
    drain_check(c,0,1500);
    tcp_close(c);segment(c,c->rcv_nxt,c->snd_nxt,TCP_FLAG_ACK,512,NULL,0,NULL,0);assert(!c->active);
    tcp_release(c);tcp_collect();assert(!allocations);
}
static void small_window_updates_keepalive(void) {
    /* No scaling from the peer: windows stay unscaled; an 8 KiB ring closes and reopens promptly. */
    tcp_conn_t* c=tcp_connect_buffers(0x0A000202,16001,50002,8192,4096);assert(c);tcp_retain(c);
    segment(c,0x60000000,c->snd_nxt,TCP_FLAG_SYN|TCP_FLAG_ACK,8192,NULL,0,NULL,0);
    assert(c->connected&&!c->wscale_ok&&!c->rcv_wscale&&last_window()==8192);
    uint32_t base=c->rcv_nxt;
    for(uint32_t off=0;off<8192;off+=1460)data_at(c,base,off,8192-off<1460?8192-off:1460,0);
    tcp_timer();clock_ns+=TCP_DELACK_NS*2;tcp_timer();
    assert(c->rx_len==8192&&last_ack()==base+8192&&last_window()==0);
    data_at(c,base,8192,100,0);assert(c->rx_len==8192&&tcp_get_stats()->rx_full_drops>=100);
    unsigned before=sent;drain_check(c,0,1000);assert(sent==before);
    drain_check(c,1000,1000);assert(sent==before+1&&last_window()==2000);
    drain_check(c,2000,6192);assert(last_window()==8192);
    /* Keepalive: probes after the idle time, abort after keep_cnt unanswered ones. */
    tcp_set_keepalive(c,true,10,2,2);before=sent;
    clock_ns+=9000000000ULL;tcp_timer();assert(sent==before);
    clock_ns+=1000000000ULL;tcp_timer();assert(sent==before+1&&last_seq()==c->snd_nxt-1&&last_size==20);
    segment(c,c->rcv_nxt,c->snd_nxt,TCP_FLAG_ACK,8192,NULL,0,NULL,0);assert(!c->keep_probes);
    clock_ns+=10000000000ULL;tcp_timer();assert(sent==before+2);
    clock_ns+=2000000000ULL;tcp_timer();assert(sent==before+3&&c->keep_probes==2);
    clock_ns+=2000000000ULL;tcp_timer();assert(!c->active&&c->error==110);
    tcp_release(c);tcp_collect();assert(!allocations);
}
static void packet(tcp_conn_t* connection,uint32_t seq,uint32_t ack,uint8_t flags,const char* data,unsigned count) {
    uint8_t bytes[256]={0};assert(count<=sizeof(bytes)-20);
    tcp_header_t* header=(void*)bytes;header->src_port=htons(connection->remote_port);header->dst_port=htons(connection->local_port);
    header->seq_num=htonl(seq);header->ack_num=htonl(ack);header->data_off=5<<4;header->flags=flags;header->window=htons(8192);
    if(count)memcpy(bytes+20,data,count);header->checksum=tcp_checksum(connection->remote_ip,connection->local_ip,bytes,20+count);
    tcp_handle_packet_to(connection->remote_ip,connection->local_ip,bytes,20+count);
}
/* ---- socket listeners, request TCBs, full transmit rings ---- */
static unsigned children_of(const tcp_conn_t* l) { unsigned n=0;for(tcp_conn_t* c=next_connection(NULL);c;c=next_connection(c))if(c->active&&c->parent==l)n++;return n; }
static tcp_conn_t* child_from(const tcp_conn_t* l,uint16_t port) { for(tcp_conn_t* c=next_connection(NULL);c;c=next_connection(c))if(c->active&&c->parent==l&&c->remote_port==port)return c;return NULL; }
static void listener_children(void) {
    /* A socket listener queues at most `backlog` children; half-open ones use small rings and grow on completion. */
    unsigned base=allocations;tcp_conn_t* l=tcp_listen_socket(0,16100,3);assert(l&&l->listener&&l->state==TCP_LISTEN);tcp_retain(l);
    assert(slot_of(l)->bytes<4096);
    tcp_conn_t peer={0};peer.remote_ip=0x0A000202;peer.local_ip=0x0A00020F;peer.local_port=16100;
    /* no memory for a child: the SYN is dropped (no SYN-ACK), the listener lives on and the retry is served */
    unsigned before=sent;allocation_failure=true;peer.remote_port=30000;packet(&peer,0,0,TCP_FLAG_SYN,NULL,0);allocation_failure=false;
    assert(sent==before&&!l->children&&l->active&&allocations==base+1);
    const uint8_t ws7[]={2,4,0x05,0xB4,1,3,3,7};segment(&peer,0,0,TCP_FLAG_SYN,65535,ws7,8,NULL,0);
    assert(last_size==28&&last_packet[24]==1&&last_packet[25]==3&&last_packet[26]==3&&last_packet[27]==3&&last_window()==TCP_BUF_MIN);
    for(unsigned i=1;i<10;i++){peer.remote_port=(uint16_t)(30000+i);packet(&peer,1000*i,0,TCP_FLAG_SYN,NULL,0);}
    assert(l->children==3&&children_of(l)==3&&allocations==base+4&&!tcp_accept_ready(l)&&!tcp_accept(l));
    tcp_conn_t* a=child_from(l,30000);assert(a&&a->state==TCP_SYN_RECEIVED&&a->rx_cap==TCP_BUF_MIN&&a->rcv_wscale==3&&a->wscale_ok);
    peer.remote_port=30000;packet(&peer,1,a->snd_nxt,TCP_FLAG_ACK,"hi",2);
    a=child_from(l,30000);assert(a&&a->connected&&a->rx_cap==TCP_RX_BUF_SIZE&&a->tx_cap==TCP_TX_BUF_SIZE&&a->rx_len==2);
    tcp_collect();assert(allocations==base+4&&tcp_accept_ready(l)&&l->children==3);
    /* an ACK for no connection at the listener is answered with a RST; a RST frees a half-open child */
    before=sent;peer.remote_port=31000;packet(&peer,5,77,TCP_FLAG_ACK,NULL,0);
    assert(sent==before+1&&((tcp_header_t*)last_packet)->flags&TCP_FLAG_RST&&last_seq()==77&&l->children==3);
    peer.remote_port=30001;packet(&peer,1001,0,TCP_FLAG_RST,NULL,0);assert(l->children==2&&!child_from(l,30001));
    peer.remote_port=30005;packet(&peer,5000,0,TCP_FLAG_SYN,NULL,0);assert(l->children==3&&child_from(l,30005));
    tcp_conn_t* got=tcp_accept(l);assert(got==a&&!a->parent&&l->children==2);tcp_retain(got);
    /* half-open children are dropped, never turned into listeners, after the SYN_RECEIVED lifetime */
    system_ticks+=TCP_SYN_RCVD_TICKS+1;tcp_timer();tcp_collect();
    assert(l->children==0&&!children_of(l)&&allocations==base+2);
    for(tcp_conn_t* c=next_connection(NULL);c;c=next_connection(c))assert(!c->active||c==l||c==got);
    /* growing under fragmentation: smaller rings, or the small half-open rings kept when nothing fits */
    allocation_limit=70000;peer.remote_port=30100;packet(&peer,50,0,TCP_FLAG_SYN,NULL,0);
    tcp_conn_t* q=child_from(l,30100);packet(&peer,51,q->snd_nxt,TCP_FLAG_ACK,"grow",4);
    q=child_from(l,30100);assert(q&&q->connected&&q->rx_cap==32768&&q->tx_cap==32768&&q->rx_len==4);
    allocation_limit=20000;peer.remote_port=30101;packet(&peer,60,0,TCP_FLAG_SYN,NULL,0);
    q=child_from(l,30101);packet(&peer,61,q->snd_nxt,TCP_FLAG_ACK,"kept",4);allocation_limit=0;
    q=child_from(l,30101);assert(q&&q->connected&&q->rx_cap==TCP_BUF_MIN&&q->tx_cap==TCP_BUF_MIN&&q->rx_len==4);
    char text[8];assert(tcp_recv(q,text,sizeof(text))==4&&!memcmp(text,"kept",4));
    /* closing the listener resets the queued connections */
    assert(l->children==2&&tcp_accept_ready(l));before=sent;
    tcp_reset(l);assert(!l->active&&sent==before+2&&!children_of(l));tcp_release(l);tcp_collect();
    assert(allocations==base+1);tcp_reset(got);tcp_release(got);tcp_collect();assert(allocations==base);
    /* specific-address listeners: only their address matches; RSTs come from the address that was contacted */
    l=tcp_listen_socket(0x7F000001,16101,4);tcp_retain(l);before=sent;
    peer.local_port=16101;peer.remote_port=33000;packet(&peer,9,0,TCP_FLAG_SYN,NULL,0);
    assert(!l->children&&sent==before+1&&((tcp_header_t*)last_packet)->flags&TCP_FLAG_RST);
    tcp_close(l);tcp_release(l);tcp_collect();assert(allocations==base);
}
static void orphans_never_listen(void) {
    /* An orphan (legacy server busy) that times out half-open is freed, not reverted to LISTEN. */
    unsigned base=allocations;tcp_conn_t* k=tcp_listen(16200);assert(k);tcp_retain(k);
    tcp_conn_t peer={0};peer.remote_ip=0x0A000202;peer.local_ip=0x0A00020F;peer.local_port=16200;
    peer.remote_port=34000;packet(&peer,1,0,TCP_FLAG_SYN,NULL,0);assert(k->state==TCP_SYN_RECEIVED);
    peer.remote_port=34001;packet(&peer,1,0,TCP_FLAG_SYN,NULL,0);assert(allocations==base+2);
    system_ticks+=TCP_SYN_RCVD_TICKS+1;tcp_timer();tcp_collect();
    assert(k->state==TCP_LISTEN&&allocations==base+1);
    for(tcp_conn_t* c=next_connection(NULL);c;c=next_connection(c))assert(!c->active||c==k);
    tcp_close(k);tcp_release(k);tcp_collect();assert(allocations==base);
}
static tcp_conn_t* established(uint32_t remote,uint16_t port,uint16_t local,uint32_t ring,uint32_t isn) {
    const uint8_t ws7[]={2,4,0x05,0xB4,1,3,3,7};
    tcp_conn_t* c=tcp_connect_buffers(remote,port,local,ring,ring);assert(c);tcp_retain(c);
    segment(c,isn,c->snd_nxt,TCP_FLAG_SYN|TCP_FLAG_ACK,65535,ws7,8,NULL,0);assert(c->connected);tcp_set_nodelay(c,true);
    segment(c,c->rcv_nxt,c->snd_nxt,TCP_FLAG_ACK,65535,NULL,0,NULL,0);assert(c->snd_wnd==65535u<<7);return c;   /* scaled window */
}
static void full_ring_resume(void) {
    /* A full transmit ring keeps data unsent (nothing counted or offered) and tcp_resume sends it once it drains. */
    tcp_conn_t* c=established(0x0A000202,16300,50010,65536,0x70000000);
    uint32_t start=c->snd_nxt,adv=c->rcv_adv,segs=c->segs_out,total=tcp_get_stats()->segs_sent;ring_full=true;refused=0;
    uint8_t data[5000];patterned(data,0,sizeof(data));assert(tcp_send_nb(c,data,sizeof(data))==(int)sizeof(data));
    assert(refused==1&&c->snd_nxt==start&&!c->tx_unacked&&c->tx_blocked&&!c->rtx_timer&&!c->ack_now);
    assert(c->segs_out==segs&&!c->bytes_sent&&tcp_get_stats()->segs_sent==total&&c->rcv_adv==adv);
    tcp_batch_begin();tcp_batch_end();assert(refused==2&&c->snd_nxt==start);
    ring_full=false;unsigned before=sent;tcp_batch_begin();tcp_batch_end();
    assert(!c->tx_blocked&&c->snd_nxt==start+sizeof(data)&&sent==before+4&&c->rtx_timer&&c->bytes_sent==sizeof(data));
    /* an ACK that met a full ring is owed and sent by the timer pass */
    ring_full=true;data_at(c,c->rcv_nxt,0,100,TCP_FLAG_PSH);assert(c->ack_now&&c->tx_blocked);
    ring_full=false;before=sent;tcp_timer();assert(sent==before+1&&last_ack()==c->rcv_nxt&&!c->tx_blocked);
    /* a retransmission the ring refuses neither backs off nor counts; the next tick sends it */
    assert(tcp_send_nb(c,"rtx",3)==3&&c->tx_unacked==3&&c->rtx_timer);
    uint32_t rto=c->rto,retrans=c->total_retrans;uint64_t again=c->bytes_retrans;
    ring_full=true;refused=0;system_ticks+=rto;tcp_timer();
    assert(refused==1&&c->rto==rto&&!c->rtx_count&&c->total_retrans==retrans&&c->bytes_retrans==again&&!c->ack_now);
    ring_full=false;before=sent;tcp_timer();
    assert(sent==before+1&&last_seq()==c->snd_una&&c->rto==2*rto&&c->rtx_count==1&&c->total_retrans==retrans+1&&c->bytes_retrans==again+3);
    segment(c,c->rcv_nxt,c->snd_nxt,TCP_FLAG_ACK,65535,NULL,0,NULL,0);assert(!c->tx_unacked);
    /* a zero-window probe the ring refuses does not advance snd_nxt */
    segment(c,c->rcv_nxt,c->snd_nxt,TCP_FLAG_ACK,0,NULL,0,NULL,0);assert(!c->snd_wnd);
    assert(tcp_send_nb(c,"z",1)==1&&!c->tx_unacked&&c->probe_timer);start=c->snd_nxt;
    ring_full=true;system_ticks+=c->rto;tcp_timer();assert(c->snd_nxt==start&&!c->tx_unacked&&c->probe_timer);
    ring_full=false;before=sent;tcp_timer();assert(sent==before+1&&c->snd_nxt==start+1&&c->tx_unacked==1);
    tcp_reset(c);tcp_release(c);tcp_collect();
}
static void closing_sends_queued(void) {
    /* The peer's FIN arrives while our data and FIN still wait for the ring: FIN_WAIT_1 -> CLOSING keeps sending. */
    tcp_conn_t* c=established(0x0A000202,16302,50012,65536,0x72000000);
    capture=true;wire_base=c->snd_nxt;data_segments=0;memset(wire,0,4000);
    ring_full=true;uint8_t data[4000];patterned(data,0,sizeof(data));assert(tcp_send_nb(c,data,sizeof(data))==(int)sizeof(data));
    tcp_close(c);assert(c->state==TCP_FIN_WAIT_1&&!c->fin_sent&&!c->tx_unacked);
    segment(c,c->rcv_nxt,wire_base,TCP_FLAG_ACK|TCP_FLAG_FIN,65535,NULL,0,NULL,0);assert(c->state==TCP_CLOSING&&c->active&&!c->fin_sent);
    ring_full=false;unsigned before=sent;tcp_timer();
    assert(c->fin_sent&&c->tx_unacked==4000&&data_segments==3&&sent==before+4&&(((tcp_header_t*)last_packet)->flags&TCP_FLAG_FIN));
    for(unsigned i=0;i<4000;i++)assert(wire[i]==pattern(i));
    assert(last_ack()==c->rcv_nxt);
    segment(c,c->rcv_nxt,c->snd_nxt,TCP_FLAG_ACK,65535,NULL,0,NULL,0);assert(!c->active);
    capture=false;tcp_release(c);tcp_collect();
}
static void loopback_fairness(void) {
    /* TCP data leaves TCP_LOOP_RESERVE loopback slots to ACKs/handshakes/datagrams, and blocked senders take turns:
     * a bulk sender cannot starve an older connection, even while its ACKs keep arriving. */
    unsigned base=allocations;loop_room=128;
    tcp_conn_t* older=established(0x7F000001,16400,50020,65536,0x73000000);
    tcp_conn_t* bulk=established(0x7F000001,16401,50021,1<<20,0x74000000);
    loop_room=128;loop_sent=0;static uint8_t chunk[60000];uint32_t queued=0;
    while(queued<900000){int n=tcp_send_nb(bulk,chunk,sizeof(chunk));assert(n>0);queued+=(uint32_t)n;}
    assert(loop_room==TCP_LOOP_RESERVE&&loop_sent==128-TCP_LOOP_RESERVE&&bulk->tx_blocked&&bulk->tx_unacked<bulk->tx_len);
    assert(tcp_send_nb(older,"o",1)==1&&older->tx_blocked&&!older->tx_unacked&&loop_room==TCP_LOOP_RESERVE);
    unsigned before=sent;tcp_send_ack(older);assert(sent==before+1&&loop_room==TCP_LOOP_RESERVE-1);   /* control uses the reserve */
    loop_room=128;tcp_batch_begin();tcp_batch_end();   /* drained: the head (bulk) refills, the older one is next in turn */
    assert(loop_room==TCP_LOOP_RESERVE&&bulk->tx_blocked&&older->tx_blocked&&!older->tx_unacked);
    /* an ACK for the bulk sender while it waits for its turn sends nothing: it does not overtake */
    loop_room=128;segment(bulk,bulk->rcv_nxt,bulk->snd_una+50*1460,TCP_FLAG_ACK,65535,NULL,0,NULL,0);
    assert(loop_room==128&&bulk->tx_blocked);
    tcp_batch_begin();tcp_batch_end();
    assert(older->tx_unacked==1&&!older->tx_blocked&&bulk->tx_blocked&&loop_room==TCP_LOOP_RESERVE);
    tcp_reset(older);tcp_reset(bulk);tcp_release(older);tcp_release(bulk);tcp_collect();assert(allocations==base);
}
static void reuseport_and_tuples(void) {
    /* An address pair in use is reported (connect EADDRNOTAVAIL); SO_REUSEPORT listeners share the clients. */
    unsigned base=allocations;
    tcp_conn_t* c=tcp_connect_from(0x0A00020F,0x0A000202,16500,50030,0,0);assert(c);tcp_retain(c);
    assert(tcp_tuple_in_use(0x0A00020F,50030,0x0A000202,16500)&&!tcp_tuple_in_use(0x0A00020F,50030,0x0A000202,16501));
    assert(!tcp_tuple_in_use(0x7F000001,50030,0x0A000202,16500)&&tcp_port_in_use(50030,0)&&!tcp_port_in_use(50030,0x7F000001));
    tcp_reset(c);assert(!tcp_tuple_in_use(0x0A00020F,50030,0x0A000202,16500));tcp_release(c);tcp_collect();
    tcp_conn_t* a=tcp_listen_socket(0,16501,8),*b=tcp_listen_socket(0,16501,8);tcp_retain(a);tcp_retain(b);
    assert(!tcp_port_in_use(16501,0));
    tcp_conn_t peer={0};peer.remote_ip=0x0A000202;peer.local_ip=0x0A00020F;peer.local_port=16501;
    for(unsigned i=0;i<8;i++){peer.remote_port=(uint16_t)(35000+i);packet(&peer,100*i,0,TCP_FLAG_SYN,NULL,0);}
    assert(a->children+b->children==8&&a->children&&b->children);
    tcp_conn_t* exact=tcp_listen_socket(0x0A00020F,16501,8);tcp_retain(exact);   /* the bound address wins */
    for(unsigned i=0;i<4;i++){peer.remote_port=(uint16_t)(35100+i);packet(&peer,100*i,0,TCP_FLAG_SYN,NULL,0);}
    assert(exact->children==4&&a->children+b->children==8);
    tcp_reset(a);tcp_reset(b);tcp_reset(exact);tcp_release(a);tcp_release(b);tcp_release(exact);tcp_collect();assert(allocations==base);
}
int main(void) {
    tcp_init();tcp_conn_t* handles[64];system_ticks=10;
    for(unsigned i=0;i<64;i++){
        uint16_t port=tcp_ephemeral_port();assert(port>=49152);handles[i]=tcp_connect(0x0A000202,14000+i,port);assert(handles[i]);tcp_retain(handles[i]);
        assert(handles[i]->snd_una==0xA1020000+(i+1)*7919&&handles[i]->snd_nxt==handles[i]->snd_una+1);
        for(unsigned j=0;j<i;j++)assert(handles[j]->local_port!=port&&handles[j]!=handles[i]);
        packet(handles[i],0x30000000+i*65536,handles[i]->snd_nxt,TCP_FLAG_SYN|TCP_FLAG_ACK,NULL,0);
        assert(handles[i]->connected&&handles[i]->state==TCP_ESTABLISHED);
    }
    assert(allocations==64&&random_calls==64);
    tcp_conn_t* c=handles[0];uint32_t seq=c->rcv_nxt;unsigned before=sent;
    packet(c,seq+TCP_RX_BUF_SIZE+1,0,TCP_FLAG_RST,NULL,0);assert(c->active&&!c->closed&&sent==before);
    packet(c,seq-1,0,TCP_FLAG_RST,NULL,0);assert(c->active&&sent==before);
    packet(c,seq+1,0,TCP_FLAG_RST,NULL,0);assert(c->active&&!c->closed&&sent==before+1);
    assert(last_size==20&&((tcp_header_t*)last_packet)->flags==TCP_FLAG_ACK);
    packet(c,seq,c->snd_nxt,TCP_FLAG_ACK,"retained bytes",14);assert(c->rx_len==14);
    tcp_retain(c);packet(c,c->rcv_nxt,0,TCP_FLAG_RST,NULL,0);assert(!c->active&&c->closed&&c->error==104);
    tcp_collect();assert(allocations==64);
    char output[20]={0};assert(tcp_recv(c,output,sizeof(output))==14&&!memcmp(output,"retained bytes",14)&&tcp_recv(c,output,sizeof(output))==-1);
    tcp_release(c);tcp_collect();assert(allocations==64);tcp_release(c);tcp_collect();assert(allocations==63);
    for(unsigned i=1;i<64;i++){packet(handles[i],handles[i]->rcv_nxt,0,TCP_FLAG_RST,NULL,0);tcp_release(handles[i]);}
    tcp_collect();assert(!allocations&&!connections);
    before=sent;allocation_failure=true;assert(!tcp_connect(0x0A000202,15000,50000)&&sent==before);allocation_failure=false;
    entropy_failure=true;assert(!tcp_connect(0x0A000202,15000,50000)&&sent==before);tcp_collect();assert(!allocations);entropy_failure=false;
    c=tcp_listen(15000);assert(c);tcp_retain(c);
    c->remote_ip=0x0A000202;c->remote_port=51000;
    packet(c,123,0,TCP_FLAG_SYN,NULL,0);assert(c->state==TCP_SYN_RECEIVED);
    packet(c,125,0,TCP_FLAG_RST,NULL,0);assert(c->state==TCP_SYN_RECEIVED);
    packet(c,124,0,TCP_FLAG_RST,NULL,0);assert(c->state==TCP_LISTEN);
    tcp_close(c);tcp_release(c);tcp_collect();assert(!allocations);
    c=tcp_connect(0x0A000202,15000,50000);assert(c);tcp_retain(c);
    packet(c,0,c->snd_nxt,TCP_FLAG_RST|TCP_FLAG_ACK,NULL,0);
    assert(!c->active&&c->error==111);tcp_release(c);tcp_collect();assert(!allocations);
    c=tcp_connect(0x0A000202,15000,50000);assert(c);tcp_retain(c);
    for(unsigned tries=0;c->active&&tries<12;tries++){system_ticks+=c->rto+1;tcp_timer();}
    assert(!c->active&&c->error==110);tcp_release(c);tcp_collect();assert(!allocations);
    c=tcp_connect(0x0A000202,15000,50000);assert(c);tcp_retain(c);
    packet(c,0x80000000,c->snd_nxt,TCP_FLAG_SYN|TCP_FLAG_ACK,NULL,0);
    tcp_set_nodelay(c,false);before=sent;
    assert(tcp_send_nb(c,"a",1)==1&&sent==before+1);
    assert(tcp_send_nb(c,"b",1)==1&&sent==before+1&&c->tx_len==2&&c->tx_unacked==1);
    tcp_set_nodelay(c,true);assert(sent==before+2&&c->tx_unacked==2);
    packet(c,c->rcv_nxt,c->snd_nxt,TCP_FLAG_ACK,"discard",7);assert(c->rx_len==7);
    tcp_shutdown_read(c);assert(c->rx_len==7&&c->discard_rx);
    assert(tcp_recv(c,output,sizeof(output))==7&&!memcmp(output,"discard",7));
    seq=c->rcv_nxt;packet(c,seq,c->snd_nxt,TCP_FLAG_ACK,"ignored",7);
    assert(!c->rx_len&&c->rcv_nxt==seq+7&&c->last_adv_wnd==(TCP_RX_BUF_SIZE<65535?TCP_RX_BUF_SIZE:65535));
    tcp_close(c);assert(c->active&&c->fin_sent&&tcp_port_in_use(50000,0)&&!tcp_background_pages());
    tcp_release(c);assert(tcp_background_pages()==(slot_of(c)->bytes+4095)/4096&&slot_of(c)->bytes>=TCP_RX_BUF_SIZE+TCP_TX_BUF_SIZE);
    packet(c,c->rcv_nxt,0,TCP_FLAG_RST,NULL,0);tcp_collect();assert(!allocations&&!tcp_background_pages()&&!tcp_port_in_use(50000,0));
    scaling_reassembly_acks();small_window_updates_keepalive();
    listener_children();orphans_never_listen();full_ring_resume();closing_sends_queued();loopback_fairness();reuseport_and_tuples();
    assert(!allocations);
    puts("PASS native TCP: 64 live connections, per-open entropy, retained reset buffers, release/collection, OOM and reset sequence validation");
    puts("PASS native TCP: window scaling both ways, wrapped send/receive rings, out-of-order reassembly, batch/delayed ACKs, window updates, keepalive");
    puts("PASS native TCP: socket listener backlog/grow/fallback/reap/reset, orphans never listen, full-ring resume/retransmit/probe, CLOSING output, fair loopback, SO_REUSEPORT, address pairs");
}
