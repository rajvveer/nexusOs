#include <assert.h>
#include <stdio.h>
#include <string.h>
#define NEXUS_HOSTED 1
#include "../arch/x86_64/fw_session.h"

static unsigned item, offset, reads;
static uint8_t directory[4+3*64], contents[8], signature[4];
static void select_item(uint16_t value) { item=value;offset=0; }
static uint8_t read_byte(void) {
    reads++;assert(reads<1000);
    const uint8_t* data=item==0?signature:item==0x19?directory:contents;
    unsigned size=item==0?sizeof(signature):item==0x19?sizeof(directory):sizeof(contents);
    return offset<size?data[offset++]:0;
}
static void entry(unsigned index,const char* name,unsigned size,unsigned selector) {
    uint8_t* e=directory+4+index*64;memset(e,0,64);e[3]=size;e[4]=selector>>8;e[5]=selector;
    assert(strlen(name)<56);strcpy((char*)e+8,name);
}
static void reset(void) {
    reads=0;memset(directory,0,sizeof(directory));memcpy(signature,"QEMU",4);memcpy(contents,"chrome\0",7);
    directory[3]=2;entry(0,"etc/boot",4,0x20);entry(1,"opt/nexus/session",6,0x21);
}
static bool probe(void) { reads=0;return fw_chrome_session(select_item,read_byte); }
int main(void) {
    reset();assert(probe());directory[4+64+3]=7;assert(probe());
    contents[6]='x';assert(!probe());reset();contents[0]='C';assert(!probe());
    reset();signature[0]=0xff;assert(!probe());reset();directory[3]=1;assert(!probe());
    reset();directory[0]=0xff;assert(!probe());reset();directory[4+64+3]=8;assert(!probe());
    reset();directory[4+64+5]=0x19;assert(!probe());reset();directory[4+64+4]=0x40;assert(!probe());
    reset();directory[4+64+6]=1;assert(!probe());reset();directory[4+64+8+17]='x';assert(!probe());
    reset();directory[3]=3;entry(2,"opt/nexus/session",6,0x22);assert(!probe());
    puts("PASS fw_cfg Chrome selector: exact names/value, optional NUL, absent device/file, bounded directory and invalid metadata");
}
