#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do { if(!(x)){dprintf(2,"[64] INSTALL TEST FAIL line=%d errno=%d\n",__LINE__,errno);_exit(99);} } while(0)
int main(void) {
    CHECK(!mkdir("/home/nexus/Applications",0700));
    int fd=open("/home/nexus/Applications/existing",O_WRONLY|O_CREAT|O_EXCL,0600);CHECK(fd>=0&&write(fd,"preserve",8)==8&&!close(fd));
    CHECK(!symlink("/home/nexus", "/home/nexus/Applications/existing-link"));
    FILE* tests=fopen("/nexus/install-tests","r");CHECK(tests);char name[64],digest[65],destination[64];int expected;unsigned count=0;
    while(fscanf(tests,"%63s %63s %d %64s",name,destination,&expected,digest)==4){
        struct statvfs before,after;CHECK(!statvfs("/home/nexus/Applications",&before));
        int filler=-1;unsigned long available=before.f_bfree;
        if(!strcmp(name,"receipt-full")){
            filler=open("/home/nexus/receipt-filler",O_WRONLY|O_CREAT|O_EXCL,0600);CHECK(filler>=0&&before.f_bfree>1);
            char zero[4096]={0};unsigned long long remaining=(before.f_bfree-1)*before.f_frsize;
            while(remaining){size_t bytes=remaining<sizeof(zero)?remaining:sizeof(zero);CHECK(write(filler,zero,bytes)==(ssize_t)bytes);remaining-=bytes;}
            CHECK(!fsync(filler)&&!statvfs("/home/nexus/Applications",&before)&&before.f_bfree==1);
        }
        char path[256],target[256];snprintf(path,sizeof(path),"/home/nexus/installer-test/%s.deb",name);snprintf(target,sizeof(target),"/home/nexus/Applications/%s",destination);
        pid_t child=fork();CHECK(child>=0);if(!child){execl("/bin/nexus-install","nexus-install",path,target,digest,(char*)NULL);perror("exec installer");_exit(98);}
        int status;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==expected);
        struct stat info;
        if(!expected)CHECK(!lstat(target,&info)&&S_ISDIR(info.st_mode));
        else if(strncmp(destination,"existing",8)&&strcmp(destination,"good-gnu"))CHECK(lstat(target,&info)==-1&&errno==ENOENT);
        DIR* dir=opendir("/home/nexus/Applications");CHECK(dir);struct dirent* entry;
        while((entry=readdir(dir)))CHECK(strncmp(entry->d_name,".nexus-install-",15));CHECK(!closedir(dir));
        CHECK(!statvfs("/home/nexus/Applications",&after));if(expected)CHECK(before.f_bfree==after.f_bfree);
        if(filler>=0){CHECK(!ftruncate(filler,0)&&!close(filler)&&!unlink("/home/nexus/receipt-filler"));CHECK(!statvfs("/home/nexus/Applications",&after)&&after.f_bfree==available);}
        printf("[64] INSTALL CASE PASSED %s -> %s expected=%d\n",name,destination,expected);fflush(stdout);count++;
    }
    CHECK(feof(tests)&&!fclose(tests));
    char data[8];fd=open("/home/nexus/Applications/existing",O_RDONLY);CHECK(fd>=0&&read(fd,data,8)==8&&!memcmp(data,"preserve",8)&&!close(fd));
    char link[256];ssize_t length=readlink("/home/nexus/Applications/existing-link",link,sizeof(link));CHECK(length==11&&!memcmp(link,"/home/nexus",11));
    fd=open("/home/nexus/outside",O_RDONLY);CHECK(fd>=0&&read(fd,data,8)==8&&!memcmp(data,"preserve",8)&&!close(fd));
    int output[2];CHECK(!pipe(output));pid_t child=fork();CHECK(child>=0);
    if(!child){CHECK(dup2(output[1],1)==1);close(output[0]);close(output[1]);execl("/home/nexus/Applications/hello/usr/bin/hello","hello",(char*)NULL);_exit(98);}
    close(output[1]);char greeting[64];size_t used=0;
    for(;;){ssize_t n=read(output[0],greeting+used,sizeof(greeting)-used);if(n<0&&errno==EINTR)continue;CHECK(n>=0);if(!n)break;used+=n;CHECK(used<sizeof(greeting));}
    CHECK(!close(output[0]));int status;CHECK(waitpid(child,&status,0)==child&&status==0);
    CHECK(used==14&&!memcmp(greeting,"Hello, world!\n",14));puts("[64] INSTALLED GNU HELLO EXECUTED: Hello, world!");
    child=fork();CHECK(child>=0);if(!child){execl("/bin/install-session","install-session",(char*)NULL);_exit(98);}
    CHECK(waitpid(child,&status,0)==child&&status==0);
    printf("[64] INSTALL TEST ALL PASSED cases=%u\n",count);return 0;
}
