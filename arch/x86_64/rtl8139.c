/* Native DMA/IRQ adapter using the existing NexusOS RTL8139 register definitions.
 * Port I/O is a VM exit under WHPX, so the hot paths touch few registers: the
 * IRQ handler only reads+acks ISR and flags pending receive work; the receive
 * drain reads CBR once per batch, hands every completed frame to the protocol
 * stack straight out of the DMA ring and returns the space with one CAPR
 * write; transmit uses per-slot TSAD addresses programmed at init (TSD read +
 * write only) and never waits. RX overflow is left out of IMR so QEMU applies
 * flow control (queues frames) instead of dropping them while the ring is full. */
#include "native.h"
#include "power.h"
#include "../../kernel/rtl8139.h"
#include "../../kernel/pci.h"
#include "../../kernel/port.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

#define RX_RING_BYTES 32768U
#define RX_ALLOCATION 36864U
#define DMA_ALLOCATION (RX_ALLOCATION + 8192U)
#define RX_IRQ_BITS (RTL_INT_ROK | RTL_INT_RER | RTL_INT_RXOVW | RTL_INT_FOVW)
#define RX_BATCH_FRAMES 256U
static struct {
    net_device_t device;
    uint64_t dma;
    uint16_t port, offset;
    uint8_t irq, tx;
    unsigned vector;    /* from irq_attach_isa: PIC 0x20+line or an IOAPIC-routed vector */
    bool active;
    volatile bool rx_pending;
} nic;
static uint8_t inb(unsigned reg) { return port_byte_in(nic.port + reg); }
static uint16_t inw(unsigned reg) { return port_word_in(nic.port + reg); }
static uint32_t inl(unsigned reg) { return port_dword_in(nic.port + reg); }
static void outb(unsigned reg,uint8_t value) { port_byte_out(nic.port + reg,value); }
static void outw(unsigned reg,uint16_t value) { port_word_out(nic.port + reg,value); }
static void outl(unsigned reg,uint32_t value) { port_dword_out(nic.port + reg,value); }
static bool reset(void) {
    outb(RTL_CMD,RTL_CMD_RESET);uint64_t deadline=native_clock_ns()+100000000;
    while(inb(RTL_CMD)&RTL_CMD_RESET) {
        if(native_clock_ns()>=deadline)return false;
        __asm__ volatile("pause");
    }
    return true;
}
/* Never waits: a busy descriptor drops the frame and TCP/ARP retransmit. */
static int transmit(net_device_t* device,const void* packet,uint16_t length) {
    (void)device;
    if(!nic.active||!nic.device.link_up)return -100;
    if(!packet||!length)return -22;
    if(length>ETH_FRAME_MAX)return -90;
    unsigned slot=nic.tx;uint32_t status=inl(RTL_TSD0+slot*4);
    if(!(status&RTL_TX_OWN))return -11;
    uint64_t buffer=nic.dma+RX_ALLOCATION+slot*2048;
    memcpy((void*)buffer,packet,length);
    if(length<60){memset((void*)(buffer+length),0,60-length);length=60;}
    __asm__ volatile("mfence" ::: "memory");
    outl(RTL_TSD0+slot*4,length);   /* TSAD<slot> was programmed once at init */
    nic.tx=(slot+1)%RTL_TX_DESC_COUNT;return 0;
}
/* Deliver every frame the NIC has completed (up to CBR), then return the space;
 * the CAPR write lets QEMU flush queued frames, so look at CBR again. */
static void receive(void) {
    unsigned budget=RX_BATCH_FRAMES;
    for(;;) {
        uint16_t end=inw(RTL_CBR)%RX_RING_BYTES;
        if(end==nic.offset)return;
        __asm__ volatile("lfence" ::: "memory");
        bool resync=false;uint16_t start=nic.offset;
        while(nic.offset!=end&&budget) {
            const volatile uint8_t* header=(void*)(nic.dma+nic.offset);
            uint16_t status=header[0]|((uint16_t)header[1]<<8),length=header[2]|((uint16_t)header[3]<<8);
            if(length==0xFFF0)break; /* Hardware is still completing this packet. */
            if(length<4||length>ETH_FRAME_MAX+4) {
                nic.device.stats.rx_errors++;nic.offset=end&(RX_RING_BYTES-4);resync=true;break;
            }
            if((status&RTL_RX_ROK)&&!(status&(RTL_RX_FAE|RTL_RX_CRC|RTL_RX_LONG|RTL_RX_RUNT|RTL_RX_ISE))&&length>=ETH_HEADER_LEN+4)
                net_deliver_packet((const void*)(nic.dma+nic.offset+4),length-4);
            else nic.device.stats.rx_errors++;
            nic.offset=(nic.offset+length+7)&~3U;nic.offset%=RX_RING_BYTES;budget--;
        }
        if(nic.offset!=start)outw(RTL_CAPR,(uint16_t)(nic.offset-16));
        if(!budget){nic.rx_pending=true;return;}   /* finish on the next poll */
        if(resync||nic.offset!=end)return;
    }
}
bool native_nic_rx_pending(void) { return nic.rx_pending; }
void native_nic_poll(void) {
    if(!nic.active)return;
    nic.rx_pending=false;
    receive();
}
/* IRQ context: acknowledge (the PCI line is level-triggered) and flag work. */
void native_nic_irq(unsigned vector) {
    if(!nic.active||vector!=nic.vector)return;
    uint16_t status=inw(RTL_ISR);
    if(!status||status==0xFFFF)return;
    outw(RTL_ISR,status);
    if(status&RTL_INT_TER)nic.device.stats.tx_errors++;
    if(status&(RTL_INT_RXOVW|RTL_INT_FOVW))nic.device.stats.rx_dropped++;
    if(status&RTL_INT_PUN)nic.device.link_up=(inb(RTL_MSR)&4)==0;
    if(status&RX_IRQ_BITS)nic.rx_pending=true;
}
static void nic_vector(unsigned vector,void* context){(void)context;native_nic_irq(vector);}
/* Receive filter (netdev.c): promiscuous (AAP), all multicast or the joined groups. The 64-bit hash filter is set
 * to accept every group whenever any is joined; netdev.c filters exactly in software. */
static uint32_t rx_filter_rcr;   /* AM/AAP bits of the last filter, programmed again after a system resume */
void native_nic_rx_mode(bool promiscuous,bool all_multicast,const uint8_t* groups,unsigned count) {
    (void)groups;
    if(!nic.active)return;
    bool multicast=all_multicast||count;
    rx_filter_rcr=(multicast?4U:0)|(promiscuous?1U:0);
    for(unsigned i=0;i<8;i++)outb(RTL_MAR0+i,multicast?0xFF:0);
    outl(RTL_RCR,RTL_RCR_APM|RTL_RCR_AB|rx_filter_rcr|RTL_RCR_WRAP|RTL_RCR_RBLEN_32K|(7U<<8)|(7U<<13));
}
/* System sleep (power.h): stop; after the power loss reset and program the ring/slot addresses again. */
static int rtl_pm_suspend(void* context,unsigned state){(void)context;(void)state;if(nic.active){outw(RTL_IMR,0);outb(RTL_CMD,0);}return 0;}
static void rtl_pm_resume(void* context,unsigned state) {
    (void)context;(void)state;
    if(!nic.active)return;
    outw(RTL_IMR,0);outb(RTL_CONFIG1,0);
    if(!reset()){vga_print("[64] RTL8139 did not leave reset after resume\n");return;}
    uint64_t dma_physical=virt_to_phys((void*)nic.dma);
    outl(RTL_RBSTART,(uint32_t)dma_physical);outw(RTL_CAPR,0xFFF0);nic.offset=0;nic.tx=0;
    for(unsigned i=0;i<RTL_TX_DESC_COUNT;i++)outl(RTL_TSAD0+i*4,(uint32_t)(dma_physical+RX_ALLOCATION+i*2048));
    for(unsigned i=0;i<8;i++)outb(RTL_MAR0+i,(rx_filter_rcr&4U)?0xFF:0);
    outl(RTL_RCR,RTL_RCR_APM|RTL_RCR_AB|rx_filter_rcr|RTL_RCR_WRAP|RTL_RCR_RBLEN_32K|(7U<<8)|(7U<<13));
    outl(RTL_TCR,RTL_TCR_IFG96|RTL_TCR_MXDMA_2048);
    outb(RTL_CMD,RTL_CMD_RX_ENABLE|RTL_CMD_TX_ENABLE);outw(RTL_ISR,0xFFFF);
    nic.device.link_up=(inb(RTL_MSR)&4)==0;
    outw(RTL_IMR,RTL_INT_ROK|RTL_INT_RER|RTL_INT_TER|RTL_INT_PUN|RTL_INT_FOVW);
}
static const struct pm_ops rtl_pm={"rtl8139",rtl_pm_suspend,rtl_pm_resume,NULL};
bool native_nic_init(void) {
    pci_device_t* device=pci_find_device(RTL8139_VENDOR_ID,RTL8139_DEVICE_ID);
    if(!device)return false;
    uint32_t base=device->bar[0]&~3U;
    if(!(device->bar[0]&1)||!base||base>0xFF00||device->irq_line<3||device->irq_line>=16)return false;
    nic.port=(uint16_t)base;nic.irq=device->irq_line;
    uint16_t command=pci_read16(device->bus,device->device,device->function,PCI_COMMAND);
    pci_write32(device->bus,device->device,device->function,PCI_COMMAND,(command|5U)&~0x400U);
    outw(RTL_IMR,0);outb(RTL_CONFIG1,0);
    if(!reset())goto failed;
    nic.dma=dma32_alloc(DMA_ALLOCATION);
    if(!nic.dma)goto failed;
    uint64_t dma_physical=virt_to_phys((void*)nic.dma);
    for(unsigned i=0;i<ETH_ALEN;i++)nic.device.mac.addr[i]=inb(RTL_MAC0+i);
    nic.device.name="eth0";nic.device.send=transmit;nic.device.link_up=(inb(RTL_MSR)&4)==0;
    outl(RTL_RBSTART,(uint32_t)dma_physical);outw(RTL_CAPR,0xFFF0);
    for(unsigned i=0;i<RTL_TX_DESC_COUNT;i++)outl(RTL_TSAD0+i*4,(uint32_t)(dma_physical+RX_ALLOCATION+i*2048));
    outl(RTL_RCR,RTL_RCR_APM|RTL_RCR_AB|RTL_RCR_WRAP|RTL_RCR_RBLEN_32K|(7U<<8)|(7U<<13));
    outl(RTL_TCR,RTL_TCR_IFG96|RTL_TCR_MXDMA_2048);
    outb(RTL_CMD,RTL_CMD_RX_ENABLE|RTL_CMD_TX_ENABLE);outw(RTL_ISR,0xFFFF);
    /* PCI INTx on the firmware-assigned ISA line: level-triggered through the IOAPIC (or the PIC). */
    int vector=irq_attach_isa(nic.irq,IRQ_PCI_INTX,nic_vector,NULL);
    if(vector<0)goto failed;
    nic.vector=(unsigned)vector;
    if(net_register_device(&nic.device)<0){irq_mask(nic.irq);irq_unregister(nic.vector,nic_vector,NULL);goto failed;}
    nic.active=true;pm_register(&rtl_pm);
    /* No TOK (transmit never waits for completions) and no RXOVW (see above). */
    outw(RTL_IMR,RTL_INT_ROK|RTL_INT_RER|RTL_INT_TER|RTL_INT_PUN|RTL_INT_FOVW);
    vga_print("[64] RTL8139 native DMA/IRQ ready\n");return true;
failed:
    outw(RTL_IMR,0);outb(RTL_CMD,0);
    pci_write32(device->bus,device->device,device->function,PCI_COMMAND,command&~4U);
    if(nic.dma)arena_free(nic.dma,DMA_ALLOCATION);
    nic.dma=0;vga_print("[64] RTL8139 initialization failed\n");return false;
}
