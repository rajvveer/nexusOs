/* Private-image test setup: preserve the prior bundle and reuse a guest download. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc,char** argv){
    const char* digest="4536aabbb75ec21ffe161099ee4b97274945770bdb0682e25ec322421211ca5e";
    const char* original="/home/nexus/Applications/4536aabbb75ec21ffe161099ee4b97274945770bdb0682e25ec322421211ca5e";
    const char* saved="/home/nexus/Applications/aurora-test-prior-hello";
    const char* copied="/home/nexus/Downloads/HELLO.DEB";
    struct stat st;char receipt[65],path[256],data[4096];
    if(argc!=2||strncmp(argv[1],"/home/nexus/Downloads/",22)){fputs("Expected the existing guest Hello download path\n",stderr);return 1;}
    if(!lstat(saved,&st)||errno!=ENOENT){fputs("Prior test bundle already exists or is inaccessible\n",stderr);return 1;}
    if(lstat(original,&st)||!S_ISDIR(st.st_mode)){perror("Original bundle");return 1;}
    snprintf(path,sizeof(path),"%s/.nexus-package-sha256",original);int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0){perror("Receipt");return 1;}ssize_t n=read(fd,receipt,sizeof(receipt));close(fd);
    if(n!=65||memcmp(receipt,digest,64)||receipt[64]!='\n'){fputs("Unexpected existing bundle receipt\n",stderr);return 1;}
    int input=open(argv[1],O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(input<0){perror("Guest download");return 1;}
    if(fstat(input,&st)||!S_ISREG(st.st_mode)||st.st_size!=53228){close(input);fputs("Unexpected pinned Hello package size\n",stderr);return 1;}
    int output=open(copied,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);if(output<0){close(input);perror("Test package copy");return 1;}
    size_t total=0;int error=0;
    while((n=read(input,data,sizeof(data)))!=0){
        if(n<0){if(errno==EINTR)continue;error=errno;break;}
        if(!total&&(n<8||memcmp(data,"!<arch>\n",8))){error=EINVAL;break;}
        for(ssize_t offset=0;offset<n;){ssize_t wrote=write(output,data+offset,(size_t)(n-offset));if(wrote<0&&errno==EINTR)continue;if(wrote<=0){error=wrote<0?errno:EIO;break;}offset+=wrote;}
        if(error)break;total+=(size_t)n;
    }
    if(!error&&total!=53228)error=EIO;
    if(!error&&fsync(output))error=errno;
    if(close(output)&&!error)error=errno;close(input);
    if(error){unlink(copied);errno=error;perror("Copy guest package");return 1;}
    if(rename(original,saved)){perror("Preserve prior bundle");return 1;}
    fd=open("/home/nexus/Applications",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(fd<0||fsync(fd)){if(fd>=0)close(fd);perror("Sync preserved bundle");return 1;}close(fd);
    puts("[64] AURORA FRESH INSTALL READY archive=/home/nexus/Downloads/HELLO.DEB prior_bundle_preserved=1");
    return 0;
}
