#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x) do { if(!(x)){dprintf(2,"[64] ATA RECOVERY FAIL line=%d errno=%d\n",__LINE__,errno);_exit(99);} } while(0)
static unsigned char data[8192],expected[8192];
int main(void){
    for(unsigned i=0;i<sizeof(expected);i++)expected[i]=(unsigned char)(i*37+11);
    int fd=open("/home/nexus/ata-recovery.dat",O_RDWR);CHECK(fd>=0);
    CHECK(pread(fd,data,sizeof(data),0)==sizeof(data));
    if(!memcmp(data,expected,sizeof(data))){CHECK(!close(fd));puts("[64] ATA RECOVERY REBOOT PASSED");return 0;}
    for(unsigned i=0;i<sizeof(data);i++)CHECK(data[i]==0xa5);
    puts("[64] ATA RECOVERY WRITE READY");fflush(stdout);usleep(2000000);
    CHECK(pwrite(fd,expected,sizeof(expected),0)==sizeof(expected));CHECK(!fsync(fd));
    puts("[64] ATA RECOVERY WRITE PASSED");puts("[64] ATA RECOVERY READ READY");fflush(stdout);usleep(2000000);
    CHECK(pread(fd,data,sizeof(data),0)==sizeof(data)&&!memcmp(data,expected,sizeof(data)));
    CHECK(!close(fd));puts("[64] ATA RECOVERY ALL PASSED");return 0;
}
