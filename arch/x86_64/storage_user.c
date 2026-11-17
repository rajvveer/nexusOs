#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define SIZE (16U*1024*1024)
#define CHECK(x) do { if(!(x)){dprintf(2,"[64] STORAGE BENCH FAIL line=%d errno=%d\n",__LINE__,errno);_exit(99);} } while(0)
static uint64_t now(void) { struct timespec t;CHECK(clock_gettime(CLOCK_MONOTONIC,&t)==0);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
int main(void) {
    uint8_t data[65536];for(unsigned i=0;i<sizeof(data);i++)data[i]=(i*37+11)&255;
    uint64_t start=now();int fd=open("/home/nexus/storage-bench.bin",O_WRONLY|O_CREAT|O_EXCL,0600);
    if(fd>=0) {
        for(unsigned at=0;at<SIZE;at+=sizeof(data)) {
            unsigned done=0;while(done<sizeof(data)){ssize_t n=write(fd,data+done,sizeof(data)-done);if(n<0&&errno==EINTR)continue;CHECK(n>0);done+=n;}
        }
        uint64_t written=now();CHECK(fsync(fd)==0);uint64_t synced=now(),elapsed=synced-start;CHECK(close(fd)==0);
        printf("[64] STORAGE WRITE bytes=%u ms=%llu\n",SIZE,(unsigned long long)(elapsed/1000000));
        printf("[64] STORAGE FSYNC ms=%llu\n",(unsigned long long)((synced-written)/1000000));
    } else CHECK(errno==EEXIST);
    int cold=fd<0;uint64_t read_start=now();
    fd=open("/home/nexus/storage-bench.bin",O_RDONLY);CHECK(fd>=0);struct stat st;CHECK(fstat(fd,&st)==0&&st.st_size==SIZE);
    unsigned total=0;
    for(;;){ssize_t n=read(fd,data,sizeof(data));if(n<0&&errno==EINTR)continue;CHECK(n>=0);if(!n)break;
        for(ssize_t i=0;i<n;i++)CHECK(data[i]==(uint8_t)(((total+i)*37+11)&255));total+=n;}
    CHECK(total==SIZE&&close(fd)==0);
    /* After a reboot the file is not cached: this times a cold sequential read (plus verification). */
    printf("[64] STORAGE READ bytes=%u ms=%llu cold=%d\n",SIZE,(unsigned long long)((now()-read_start)/1000000),cold);
    puts("[64] STORAGE DATA VERIFIED");return 0;
}
