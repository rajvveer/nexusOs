#define _GNU_SOURCE
#include <cpuid.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x) do { if(!(x)){dprintf(2,"[64] CPUINFO FAIL line=%d errno=%d\n",__LINE__,errno);_exit(99);} } while(0)
int main(void) {
    int fd=open("/proc/cpuinfo",O_RDONLY|O_CLOEXEC);CHECK(fd>=0);
    struct stat a,b;CHECK(!fstat(fd,&a)&&!stat("/proc/cpuinfo",&b)&&a.st_ino==b.st_ino&&S_ISREG(a.st_mode)&&!(a.st_mode&0222));
    char text[2048];size_t used=0;
    for(;;){ssize_t n=read(fd,text+used,7);CHECK(n>=0);if(!n)break;used+=n;CHECK(used+7<sizeof(text));}text[used]=0;
    unsigned ca,cb,cc,cd;__cpuid(0,ca,cb,cc,cd);char vendor[13];memcpy(vendor,&cb,4);memcpy(vendor+4,&cd,4);memcpy(vendor+8,&cc,4);vendor[12]=0;
    CHECK(strstr(text,vendor)&&strstr(text,"processor\t: 0\n")&&!strstr(text,"processor\t: 1\n"));
    __cpuid(1,ca,cb,cc,cd);char expected[64];snprintf(expected,sizeof(expected),"apicid\t: %u\n",cb>>24);CHECK(strstr(text,expected));
    unsigned family=(ca>>8)&15,model=(ca>>4)&15;if(family==6||family==15)model|=((ca>>16)&15)<<4;if(family==15)family+=(ca>>20)&255;
    snprintf(expected,sizeof(expected),"cpu family\t: %u\n",family);CHECK(strstr(text,expected));
    snprintf(expected,sizeof(expected),"model\t: %u\n",model);CHECK(strstr(text,expected));
    CHECK((strstr(text," rdrand")!=NULL)==((cc&(1u<<30))!=0));CHECK(!strstr(text," avx")&&!strstr(text," xsave"));
    CHECK((strstr(text," aes")!=NULL)==((cc&(1u<<25))!=0));
    char copy[2048];CHECK(pread(fd,copy,used,0)==(ssize_t)used&&!memcmp(copy,text,used));CHECK(pread(fd,copy,sizeof(copy),used)==0);
    CHECK(lseek(fd,0,SEEK_SET)==0&&read(fd,copy,used)==(ssize_t)used&&!memcmp(copy,text,used));
    char path[64],target[128];snprintf(path,sizeof(path),"/proc/self/fd/%d",fd);ssize_t n=readlink(path,target,sizeof(target));CHECK(n==13&&!memcmp(target,"/proc/cpuinfo",13));
    CHECK(!close(fd));errno=0;CHECK(open("/proc/cpuinfo",O_WRONLY)==-1&&errno==EACCES);
    errno=0;CHECK(open("/proc/cpuinfo",O_RDONLY|O_DIRECTORY)==-1&&errno==ENOTDIR);
    errno=0;CHECK(open("/proc/cpuinfo/child",O_RDONLY)==-1&&errno==ENOTDIR);
    DIR* proc=opendir("/proc");CHECK(proc);struct dirent* item;int found=0;while((item=readdir(proc)))if(!strcmp(item->d_name,"cpuinfo")){CHECK(item->d_type==DT_REG);found++;}CHECK(found==1&&!closedir(proc));
    puts(text);puts("[64] CPUINFO ALL PASSED: actual CPUID, readonly metadata, partial reads, seek, directory and descriptor paths");return 0;
}
