/* Exercise the production native adapter without doing host port I/O or DMA. */
#define NEXUS_NATIVE64 1
#define NEXUS_HOST_TEST 1   /* simulated device: pointers are bus addresses */
#include "../arch/x86_64/native.h"
#include "../kernel/pci.h"
#undef NULL
#include <stdio.h>
#include <stdlib.h>
#undef NDEBUG
#include <assert.h>
#define PORT_H
static uint8_t registers[256];
static uint8_t master_mask=255,slave_mask=255;
static bool reset_stuck,rx_ready,device_present=true;
static unsigned writes,received,freed,attached,detached,registered;
static int attach_result=0x2B,register_result=-1;
static uint64_t now,allocation;
static uint16_t pci_command;
static pci_device_t pci_device={.bar={0x1001},.irq_line=11};
static uint8_t port_byte_in(uint16_t port) {
    if(port==0x21)return master_mask;if(port==0xA1)return slave_mask;
    assert(port>=0x1000&&port<0x1100);unsigned reg=port-0x1000;
    if(reg==0x37){if(registers[reg]&0x10){if(reset_stuck)return 0x10;registers[reg]=0;}return registers[reg]|(rx_ready?0:1);}
    return registers[reg];
}
static uint16_t port_word_in(uint16_t port) { return port_byte_in(port)|(uint16_t)port_byte_in(port+1)<<8; }
static uint32_t port_dword_in(uint16_t port) { return port_word_in(port)|(uint32_t)port_word_in(port+2)<<16; }
static void port_byte_out(uint16_t port,uint8_t value) {
    writes++;if(port==0x21){master_mask=value;return;}if(port==0xA1){slave_mask=value;return;}
    assert(port>=0x1000&&port<0x1100);registers[port-0x1000]=value;
}
static void port_word_out(uint16_t port,uint16_t value) { port_byte_out(port,value);port_byte_out(port+1,value>>8);if(port==0x1038)rx_ready=false; }
static void port_dword_out(uint16_t port,uint32_t value) { port_word_out(port,value);port_word_out(port+2,value>>16); }
#include "../arch/x86_64/rtl8139.c"
uint64_t native_clock_ns(void) { return now+=1000000; }
uint64_t dma32_alloc(uint64_t size) { assert(size==DMA_ALLOCATION);return allocation; }
void arena_free(uint64_t address,uint64_t size) { assert(address==allocation&&size==DMA_ALLOCATION);freed++; }
pci_device_t* pci_find_device(uint16_t vendor,uint16_t device) { assert(vendor==RTL8139_VENDOR_ID&&device==RTL8139_DEVICE_ID);return device_present?&pci_device:NULL; }
uint16_t pci_read16(uint8_t b,uint8_t d,uint8_t f,uint8_t offset) { (void)b;(void)d;(void)f;assert(offset==PCI_COMMAND);return pci_command; }
void pci_write32(uint8_t b,uint8_t d,uint8_t f,uint8_t offset,uint32_t value) { (void)b;(void)d;(void)f;assert(offset==PCI_COMMAND&&value<65536);pci_command=value; }
int net_register_device(net_device_t* device) { assert(device==&nic.device&&nic.vector==(unsigned)attach_result);registered++;return register_result; }
int pm_register(const struct pm_ops* ops) { assert(ops==&rtl_pm&&nic.active);return 0; }   /* system sleep registry (pm.c) */
int irq_attach_isa(unsigned line,unsigned flags,irq_handler_t handler,void* context) {
    assert(line==11&&flags==IRQ_PCI_INTX&&handler==nic_vector&&!context);attached++;return attach_result;
}
void irq_mask(unsigned line) { assert(line==11);detached++; }
int irq_unregister(unsigned vector,irq_handler_t handler,void* context) { assert(vector==(unsigned)attach_result&&handler==nic_vector&&!context);detached++;return 0; }
void net_deliver_packet(const void* data,uint16_t length) { assert(length==60);for(unsigned i=0;i<length;i++)assert(((const uint8_t*)data)[i]==0xA5);received++; }
static void set_cbr(uint16_t value) { registers[RTL_CBR]=(uint8_t)value;registers[RTL_CBR+1]=(uint8_t)(value>>8); }
void vga_print(const char* message) { (void)message; }
int main(void) {
    device_present=false;assert(!native_nic_init()&&!writes);device_present=true;
    pci_device.irq_line=16;assert(!native_nic_init()&&!writes);pci_device.irq_line=11;
    pci_device.bar[0]=0x1000;assert(!native_nic_init()&&!writes);pci_device.bar[0]=0x1001;
    reset_stuck=true;assert(!native_nic_init()&&now>=100000000&&!(pci_command&4)&&!freed);reset_stuck=false;
    allocation=0;assert(!native_nic_init()&&!freed&&!(pci_command&4));
    /* IRQ attach and device registration failures release the DMA area, bus mastering and the vector. */
    uint8_t* dma=malloc(DMA_ALLOCATION);assert(dma);allocation=(uint64_t)dma;
    attach_result=-16;assert(!native_nic_init()&&attached==1&&!registered&&freed==1&&!detached&&!(pci_command&4));attach_result=0x2B;
    assert(!native_nic_init()&&attached==2&&registered==1&&freed==2&&detached==2&&!(pci_command&4));
    assert(inl(RTL_RBSTART)==(uint32_t)allocation&&inl(RTL_TSAD0)==(uint32_t)(allocation+RX_ALLOCATION));
    register_result=0;assert(native_nic_init()&&nic.active&&registered==2&&freed==2&&(pci_command&4)&&inw(RTL_IMR));
    memset(dma,0x5A,DMA_ALLOCATION);memset(registers+RTL_TSAD0,0,16);   /* transmit must not program TSAD */
    nic.dma=(uint64_t)dma;nic.port=0x1000;nic.active=true;nic.device.link_up=true;nic.tx=0;
    uint8_t bytes[8]={1,2,3,4,5,6,7,8};unsigned before=writes;
    memset(registers+RTL_TSD0,0,16);
    assert(transmit(&nic.device,bytes,sizeof(bytes))==-11&&writes==before&&nic.tx==0&&dma[RX_ALLOCATION]==0x5A);
    registers[RTL_TSD0+1]=0x20;
    assert(transmit(&nic.device,bytes,sizeof(bytes))==0&&nic.tx==1&&!memcmp(dma+RX_ALLOCATION,bytes,sizeof(bytes)));
    for(unsigned i=8;i<60;i++)assert(dma[RX_ALLOCATION+i]==0);
    assert(inl(RTL_TSD0)==60&&!inl(RTL_TSAD0));   /* TSAD is programmed once at init, not per frame */
    before=writes;assert(transmit(&nic.device,bytes,ETH_FRAME_MAX+1)==-90&&transmit(&nic.device,NULL,1)==-22&&writes==before);
    nic.device.link_up=false;assert(transmit(&nic.device,bytes,8)==-100);nic.device.link_up=true;
    nic.offset=RX_RING_BYTES-8;uint8_t* packet=dma+nic.offset;packet[0]=1;packet[1]=0;packet[2]=64;packet[3]=0;memset(packet+4,0xA5,60);
    set_cbr(60);receive();assert(received==1&&nic.offset==60&&inw(RTL_CAPR)==44);
    nic.offset=0;packet=dma;packet[0]=1;packet[1]=0;packet[2]=0xF0;packet[3]=0xFF;set_cbr(0x100);before=writes;
    receive();assert(nic.offset==0&&writes==before&&received==1);
    packet[2]=0xFF;packet[3]=0x7F;set_cbr(0x127F);
    receive();assert(nic.offset==0x127C&&nic.device.stats.rx_errors==1&&received==1);
    nic.offset=0;packet[0]=RTL_RX_CRC;packet[1]=0;packet[2]=64;packet[3]=0;set_cbr(68);
    receive();assert(received==1&&nic.offset==68&&nic.device.stats.rx_errors==2);
    /* A batch of frames costs one CBR read per pass and a single CAPR write. */
    for(unsigned i=0;i<3;i++){packet=dma+68+i*68;packet[0]=1;packet[1]=0;packet[2]=64;packet[3]=0;memset(packet+4,0xA5,60);}
    set_cbr(68+3*68);before=writes;receive();assert(received==4&&nic.offset==272&&writes==before+2&&inw(RTL_CAPR)==256);
    before=writes;receive();assert(writes==before&&received==4);   /* nothing new: one register read only */
    /* IRQ: ack ISR and flag work; other vectors and an idle ISR touch nothing else. */
    nic.irq=11;nic.vector=32+11;registers[RTL_ISR]=RTL_INT_ROK;before=writes;
    native_nic_irq(32+12);assert(writes==before&&!native_nic_rx_pending());
    native_nic_irq(32+11);assert(writes==before+2&&native_nic_rx_pending());
    registers[RTL_ISR]=0;registers[RTL_ISR+1]=0;before=writes;native_nic_irq(32+11);assert(writes==before);
    native_nic_poll();assert(!native_nic_rx_pending()&&received==4);
    registers[RTL_ISR]=RTL_INT_TOK;native_nic_irq(32+11);assert(!native_nic_rx_pending());   /* TX-only status: no RX work */
    free(dma);puts("PASS native RTL8139: reset deadline, unavailable hardware, DMA OOM/IRQ/registration cleanup, TX ownership/padding/fixed TSAD, RX wrap/incomplete/corrupt headers, CBR batches, IRQ ack/pending");
}
