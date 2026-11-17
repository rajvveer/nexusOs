/* Protocol dispatch happens at scheduler/syscall boundaries, never in the NIC IRQ.
 * native_network_poll() runs on every scheduler pass, so it must be nearly free
 * when nothing happened: NIC registers are touched only after the NIC IRQ
 * flagged receive work, and protocol timers / socket wakeups run at most once
 * per millisecond (or right after received frames). The interface layer
 * (netdev.c: lo, eth0, tun/tap, WireGuard; IPv6, routing, the packet filter)
 * starts after the legacy IPv4 modules it wraps. */
#include "netdev.h"
#include "wgcrypto.h"
#include "../../kernel/net.h"
#include "../../kernel/pci.h"
#include "../../kernel/arp.h"
#include "../../kernel/ip.h"
#include "../../kernel/icmp.h"
#include "../../kernel/udp.h"
#include "../../kernel/tcp.h"
#include "../../kernel/firewall.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

volatile uint32_t system_ticks;
static bool ready,stack;   /* ready: a NIC drives the wire; stack: IP/TCP/UDP are up (loopback works without a NIC) */
bool native_network_available(void) { return ready; }
uint64_t native_network_background_pages(void) { tcp_collect(); return tcp_background_pages() + inet_background_pages() + net_background_pages(); }
void igmp_poll(uint64_t now);
void ip4_hold_poll(uint64_t now);   /* net4.c: one-shot packets waiting for ARP */
void native_network_init(void) {
    pci_init();net_init();
    ready=native_nic_init();
    if(ready)arp_init();else{vga_print("[64] No native Ethernet device: loopback only\n");ip_set_config(0,0,0,0);}   /* only 127/8 is ours */
    ip_init();icmp_init();udp_init();tcp_init();firewall_init();
    if(!wgcrypto_selftest())panic("network crypto self-test (BLAKE2s/X25519/ChaCha20-Poly1305 vectors)");
    netdev_init();stack=true;
    vga_print("[64] Network interfaces: lo");if(netdev_eth0){vga_print(", ");vga_print(netdev_eth0->name);}
    vga_print("; IPv6, routing policy, neighbour discovery ready\n");
    if(!ready)return;
    arp_announce();
    native_dhcp_start();
}
#define TIMER_PERIOD_NS 1000000ULL
static uint64_t next_timers;
void native_network_poll(void) {
    if(!stack||net_is_busy())return;
    bool frames=ready&&native_nic_rx_pending(),loop=ip_loopback_pending(),air=wifi_rx_pending();   /* air: the virtual Wi-Fi medium */
    uint64_t now=native_clock_ns();
    if(!frames&&!loop&&!air&&now<next_timers)return;
    system_ticks=(uint32_t)(now/(1000000000ULL*65536/1193182));
    if(frames||loop){tcp_batch_begin();if(frames)native_nic_poll();if(loop)ip_loopback_poll();tcp_batch_end();}
    if(air){tcp_batch_begin();wifi_rx_flush();tcp_batch_end();} /* NIC frames, loopback (ip.c) */
    if(now>=next_timers){next_timers=now+TIMER_PERIOD_NS;tcp_timer();tcp_collect();if(ready)native_dhcp_poll();netdev_poll(now);igmp_poll(now);ip4_hold_poll(now);}
    inet_poll();
}
/* Tickless idle (tick.c): when this poll needs the boot CPU next while the system idles: received frames and loopback
 * packets at once, the 1 ms protocol timer while a TCP connection has a timer running, else the slow timers (DHCP,
 * neighbour discovery, IGMP, WireGuard, NIC link watchdog, TIME_WAIT / orphan reaping, keepalives) every NET_IDLE_NS. */
#define NET_IDLE_NS 250000000ULL
bool tcp_timers_idle(void);
uint64_t native_network_next_event(uint64_t now) {
    if(!stack) return ~0ULL;
    if(net_is_busy()||(ready&&native_nic_rx_pending())||ip_loopback_pending()) return now;
    return tcp_timers_idle()?next_timers+NET_IDLE_NS:next_timers;
}
/* A loopback packet was queued: the BSP's scheduler loop delivers it (wake it when it halts on another CPU's behalf). */
void native_network_kick(void) { if(cpu_count>1&&cpu_self()->index!=0&&cpus[0].idle)smp_send_ipi(0,VEC_WAKE); }
bool native_network_selftest(uint16_t port) {
    if(!ready||!port)return false;
    native_network_poll();
    uint64_t before=memory_used_pages();
    tcp_conn_t* connection=tcp_connect(0x0A000202,port,tcp_ephemeral_port());
    if(!connection)return false;
    tcp_retain(connection);
    uint64_t deadline=native_clock_ns()+90000000000ULL;
    while(!connection->connected&&!connection->closed&&native_clock_ns()<deadline){native_network_poll();__asm__ volatile("sti; hlt; cli" ::: "memory");}
    const char request[]="NEXUS64-NET\n";
    bool success=connection->connected&&tcp_send_nb(connection,request,sizeof(request)-1)==sizeof(request)-1;
    uint64_t total=0;uint8_t buffer[4096];bool eof=false;
    while(success&&!eof&&native_clock_ns()<deadline){
        native_network_poll();int size;
        while((size=tcp_recv(connection,buffer,sizeof(buffer)))>0){
            for(int i=0;i<size;i++)if(buffer[i]!=(uint8_t)(((total+(unsigned)i)*37+11)&255))success=false;
            total+=(unsigned)size;if(total>2*1024*1024)success=false;
        }
        if(size<0)eof=true;
        if(!eof)__asm__ volatile("sti; hlt; cli" ::: "memory");
    }
    success=success&&eof&&total==2*1024*1024;
    tcp_close(connection);deadline=native_clock_ns()+5000000000ULL;
    while(connection->active&&native_clock_ns()<deadline){native_network_poll();__asm__ volatile("sti; hlt; cli" ::: "memory");}
    success=success&&!connection->active;tcp_release(connection);tcp_collect();
    if(memory_used_pages()!=before)success=false;
    vga_print(success?"[64] NATIVE NETWORK 2 MIB STREAM PASSED\n":"[64] NATIVE NETWORK STREAM FAILED\n");
    return success;
}
