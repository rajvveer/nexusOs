/* Install a verified Debian application payload into a new private bundle. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <lzma.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../kernel/crypto.h"

struct input { int fd; uint64_t bytes; sha256_ctx_t hash; };
struct decoder { struct input* input; uint64_t left,total; lzma_stream stream; bool ended; uint8_t buffer[65536]; };
struct link { struct link* next; char name[PATH_MAX],target[PATH_MAX]; };
static int invalid(const char* reason) { fprintf(stderr,"nexus-install: %s\n",reason);errno=EINVAL;return -1; }
static ssize_t raw_read(struct input* input,void* data,size_t size) {
    ssize_t n;do { n=read(input->fd,data,size); } while(n<0&&errno==EINTR);
    if(n>0){sha256_update(&input->hash,data,(uint32_t)n);input->bytes+=(uint64_t)n;}return n;
}
static int exact(struct input* input,void* data,size_t size) {
    while(size){ssize_t n=raw_read(input,data,size);if(n<=0)return n? -1:invalid("truncated package");data=(uint8_t*)data+n;size-=n;}return 0;
}
static int number(const uint8_t* field,size_t size,unsigned base,uint64_t* value) {
    uint64_t result=0;size_t i=0;bool digit=false;
    while(i<size&&field[i]==' ')i++;
    for(;i<size&&field[i]>='0'&&field[i]<'0'+base;i++){
        unsigned d=field[i]-'0';if(result>(UINT64_MAX-d)/base)return invalid("archive number overflow");result=result*base+d;digit=true;
    }
    for(;i<size;i++)if(field[i]&&field[i]!=' ')return invalid("invalid archive number");
    if(!digit)return invalid("empty archive number");*value=result;return 0;
}
static int string_field(char* out,size_t capacity,const uint8_t* field,size_t size) {
    size_t n=0;while(n<size&&field[n])n++;
    if(n>=capacity)return invalid("archive name too long");memcpy(out,field,n);out[n]=0;return 0;
}
static int clean_name(char* name) {
    while(!strncmp(name,"./",2))memmove(name,name+2,strlen(name+2)+1);
    size_t n=strlen(name);while(n&&name[n-1]=='/')name[--n]=0;
    if(!strcmp(name,"."))name[0]=0;
    if(name[0]=='/')return invalid("absolute archive path");
    for(char* p=name;*p;){char* end=strchr(p,'/');size_t length=end?(size_t)(end-p):strlen(p);
        if(!length||(length==1&&*p=='.')||(length==2&&!memcmp(p,"..",2)))return invalid("archive path traversal");
        for(size_t i=0;i<length;i++)if((unsigned char)p[i]<32||p[i]=='\\'||p[i]==':')return invalid("invalid archive path");
        if(!end)break;p=end+1;
    }
    return 0;
}
/* Walk every component with NOFOLLOW; archive links never become parent directories. */
static int parent_at(int root,const char* path,char leaf[PATH_MAX],bool create) {
    if(strlen(path)>=PATH_MAX){errno=ENAMETOOLONG;return -1;}strcpy(leaf,path);
    int fd=openat(root,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -1;
    char* part=leaf;
    for(char* slash;(slash=strchr(part,'/'));part=slash+1){
        *slash=0;
        if(create&&mkdirat(fd,part,0700)&&errno!=EEXIST){close(fd);return -1;}
        int next=openat(fd,part,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);close(fd);if(next<0)return -1;fd=next;
    }
    memmove(leaf,part,strlen(part)+1);return fd;
}
static int write_all(int fd,const uint8_t* data,size_t size) {
    while(size){ssize_t n=write(fd,data,size);if(n<0&&errno==EINTR)continue;if(n<=0){if(!n)errno=EIO;return -1;}data+=n;size-=n;}return 0;
}
static ssize_t decoded(struct decoder* d,void* data,size_t size) {
    if(d->ended)return 0;
    d->stream.next_out=data;d->stream.avail_out=size;
    while(d->stream.avail_out){
        if(!d->stream.avail_in&&d->left){size_t count=d->left<sizeof(d->buffer)?(size_t)d->left:sizeof(d->buffer);
            if(exact(d->input,d->buffer,count))return -1;d->left-=count;d->stream.next_in=d->buffer;d->stream.avail_in=count;
        }
        size_t before_in=d->stream.avail_in,before_out=d->stream.avail_out;
        lzma_ret result=lzma_code(&d->stream,d->left?LZMA_RUN:LZMA_FINISH);
        if(result==LZMA_STREAM_END){d->ended=true;break;}
        if(result==LZMA_MEMLIMIT_ERROR){fprintf(stderr,"nexus-install: XZ needs %llu bytes of decoder memory\n",(unsigned long long)lzma_memusage(&d->stream));errno=ENOMEM;return -1;}
        if(result!=LZMA_OK)return invalid("XZ stream failed integrity/decompression checks");
        if(before_in==d->stream.avail_in&&before_out==d->stream.avail_out)return invalid("truncated XZ stream");
    }
    size_t count=size-d->stream.avail_out;d->total+=count;return (ssize_t)count;
}
static int block(struct decoder* d,void* data,size_t size) {
    ssize_t count=decoded(d,data,size);return count<0?-1:(size_t)count==size?0:invalid("truncated tar payload");
}
static bool zeroes(const uint8_t* data,size_t size) { for(size_t i=0;i<size;i++)if(data[i])return false;return true; }
static int confined_link(const char* name,const char* original,char target[PATH_MAX]) {
    char path[PATH_MAX],canonical[PATH_MAX];size_t used=0;
    if(original[0]=='/'){
        if(snprintf(path,sizeof(path),"%s",original+1)>=(int)sizeof(path))return invalid("link too long");
    }else{
        const char* slash=strrchr(name,'/');int prefix=slash?(int)(slash-name+1):0;
        if(snprintf(path,sizeof(path),"%.*s%s",prefix,name,original)>=(int)sizeof(path))return invalid("link too long");
    }
    if(!*original)return invalid("empty link target");
    for(char* p=path;*p;){char* end=strchr(p,'/');size_t n=end?(size_t)(end-p):strlen(p);
        if(n==2&&!memcmp(p,"..",2)){
            if(!used)return invalid("link escapes application bundle");while(used&&canonical[used-1]!='/')used--;if(used)used--;
        }else if(n&&!(n==1&&*p=='.')){
            for(size_t i=0;i<n;i++)if((unsigned char)p[i]<32||p[i]=='\\'||p[i]==':')return invalid("invalid link target");
            if(used)canonical[used++]='/';if(used+n>=sizeof(canonical))return invalid("link too long");memcpy(canonical+used,p,n);used+=n;
        }
        if(!end)break;p=end+1;
    }
    canonical[used]=0;size_t prefix=0;
    for(const char* p=name;*p;p++)if(*p=='/'){if(prefix+3>=PATH_MAX)return invalid("link too deep");memcpy(target+prefix,"../",3);prefix+=3;}
    if(prefix+used+1>=PATH_MAX)return invalid("link too long");strcpy(target+prefix,used?canonical:".");return 0;
}
static int untar(struct decoder* d,int root,unsigned* entries) {
    uint8_t header[512],data[65536];struct link* links=NULL;int result=-1,fd=-1,parent=-1;
    for(;;){
        if(block(d,header,sizeof(header)))goto done;
        if(zeroes(header,sizeof(header))){
            if(block(d,header,sizeof(header))||!zeroes(header,sizeof(header))){invalid("missing tar end marker");goto done;}
            for(;;){ssize_t count=decoded(d,data,sizeof(data));if(count<0)goto done;if(!count)break;
                if(!zeroes(data,(size_t)count)){invalid("data after tar end marker");goto done;}}
            if(d->total%512||d->left||d->stream.avail_in){invalid("invalid archive tail");goto done;}break;
        }
        uint64_t checksum,size,mode;unsigned sum=0;
        for(unsigned i=0;i<sizeof(header);i++)sum+=(i>=148&&i<156)?' ':header[i];
        if(number(header+148,8,8,&checksum)||checksum!=sum){invalid("tar header checksum mismatch");goto done;}
        if(number(header+124,12,8,&size)||number(header+100,8,8,&mode))goto done;
        bool posix=!memcmp(header+257,"ustar\00000",8),gnu=!memcmp(header+257,"ustar  \0",8);
        if(!posix&&!gnu){invalid("unsupported tar format");goto done;}
        char name[PATH_MAX],prefix[156],leaf[PATH_MAX];
        if(string_field(name,sizeof(name),header,100))goto done;
        if(posix){if(string_field(prefix,sizeof(prefix),header+345,155))goto done;
            if(*prefix){char joined[PATH_MAX];if(snprintf(joined,sizeof(joined),"%s/%s",prefix,name)>=(int)sizeof(joined)){invalid("name too long");goto done;}strcpy(name,joined);}}
        if(clean_name(name))goto done;
        unsigned type=header[156];
        if(type!='0'&&type&&type!='5'&&type!='2'){invalid("unsupported archive member type");goto done;}
        if((type=='5'||type=='2')&&size){invalid("non-file member has data");goto done;}
        if(!*name){if(type=='5')continue;invalid("empty member name");goto done;}
        parent=parent_at(root,name,leaf,true);if(parent<0)goto done;
        if(type=='5'){
            if(mkdirat(parent,leaf,0700)&&errno!=EEXIST)goto done;
            fd=openat(parent,leaf,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)goto done;close(fd);fd=-1;
        }else if(type=='2'){
            char original[101];if(string_field(original,sizeof(original),header+157,100))goto done;
            struct link* link=calloc(1,sizeof(*link));if(!link)goto done;
            if(confined_link(name,original,link->target)){free(link);goto done;}
            strcpy(link->name,name);link->next=links;links=link;
        }else{
            /* FAT currently enforces owner-only bundle access; it cannot preserve POSIX modes. */
            fd=openat(parent,leaf,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,(mode&0111)?0700:0600);if(fd<0)goto done;
            uint64_t left=size;
            while(left){size_t n=left<sizeof(data)?(size_t)left:sizeof(data);if(block(d,data,n)||write_all(fd,data,n))goto done;left-=n;
                if((size-left)%(16*1024*1024)==0){printf("[64] INSTALL PROGRESS %s bytes=%llu\n",name,(unsigned long long)(size-left));fflush(stdout);}}
            if(fsync(fd))goto done;if(close(fd)){fd=-1;goto done;}fd=-1;
            unsigned pad=(512-size%512)%512;if(pad&&block(d,data,pad))goto done;
        }
        close(parent);parent=-1;(*entries)++;
        printf("[64] INSTALL MEMBER %s bytes=%llu\n",name,(unsigned long long)size);fflush(stdout);
    }
    for(struct link* link=links;link;link=link->next){char leaf[PATH_MAX];parent=parent_at(root,link->name,leaf,false);if(parent<0)goto done;
        if(symlinkat(link->target,parent,leaf))goto done;close(parent);parent=-1;
    }
    result=0;
done:
    {int error=errno;
        /* Rollback explicitly discards the new file's unwritten cached tail before close. */
        if(fd>=0){if(result&&ftruncate(fd,0))perror("nexus-install: discard partial output");close(fd);}
        if(parent>=0)close(parent);while(links){struct link* next=links->next;free(links);links=next;}errno=error;
    }
    return result;
}
static int payload(struct input* input,int root,unsigned* entries) {
    uint8_t magic[8],header[60],data[65536];if(exact(input,magic,8)||memcmp(magic,"!<arch>\n",8))return invalid("invalid Debian archive");
    const char* names[]={"debian-binary","control.tar.xz","data.tar.xz"};
    // shortcut: current Chrome/hello use ar + XZ + ustar/GNU; add other codecs/types with format fixtures when needed.
    for(unsigned part=0;part<3;part++){
        if(exact(input,header,60)||memcmp(header+58,"`\n",2))return invalid("invalid ar header");
        char name[17];memcpy(name,header,16);name[16]=0;size_t n=strlen(name);while(n&&(name[n-1]==' '||name[n-1]=='/'))name[--n]=0;
        uint64_t size;if(strcmp(name,names[part])||number(header+48,10,10,&size))return invalid("unsupported Debian member sequence");
        if(!part){if(size!=4||exact(input,data,4)||memcmp(data,"2.0\n",4))return invalid("unsupported Debian format version");}
        else if(part==1){uint64_t left=size;while(left){size_t count=left<sizeof(data)?(size_t)left:sizeof(data);if(exact(input,data,count))return -1;left-=count;}}
        else{
            struct decoder* d=calloc(1,sizeof(*d));if(!d)return -1;d->input=input;d->left=size;d->stream=(lzma_stream)LZMA_STREAM_INIT;
            /* Chrome uses a 256 MiB dictionary plus decoder state; leave most of the 2 GiB guest free. */
            lzma_ret initialized=lzma_stream_decoder(&d->stream,512ULL*1024*1024,LZMA_CONCATENATED);
            int status=initialized==LZMA_OK?untar(d,root,entries):invalid("cannot initialize XZ decoder");lzma_end(&d->stream);free(d);if(status)return -1;
        }
        if(size&1){if(exact(input,data,1)||*data!='\n')return invalid("invalid ar padding");}
    }
    ssize_t extra=raw_read(input,data,1);return extra<0?-1:extra?invalid("unexpected package trailer"):0;
}
static int remove_contents(int root) {
    int fd=openat(root,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return -1;
    DIR* dir=fdopendir(fd);if(!dir){close(fd);return -1;}int result=0;
    for(;;){errno=0;struct dirent* entry=readdir(dir);if(!entry){if(errno)result=-1;break;}
        if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
        struct stat st;if(fstatat(root,entry->d_name,&st,AT_SYMLINK_NOFOLLOW)){result=-1;break;}
        if(S_ISDIR(st.st_mode)){int child=openat(root,entry->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if(child<0){result=-1;break;}int status=remove_contents(child);close(child);
            if(status||unlinkat(root,entry->d_name,AT_REMOVEDIR)){result=-1;break;}
        }else if(unlinkat(root,entry->d_name,0)){result=-1;break;}
    }
    int error=errno;if(closedir(dir)&&!result)return -1;errno=error;return result;
}
int main(int argc,char** argv) {
    if(argc!=4){fprintf(stderr,"Usage: nexus-install PACKAGE.deb /home/nexus/Applications/NAME SHA256\n");return 2;}
    uint8_t expected[32];if(strlen(argv[3])!=64)return 2;
    for(unsigned i=0;i<64;i++){char c=argv[3][i];unsigned value=c>='0'&&c<='9'?(unsigned)(c-'0'):c>='a'&&c<='f'?(unsigned)(c-'a'+10):16;
        if(value>15)return 2;if(i&1)expected[i/2]|=value;else expected[i/2]=value<<4;
    }
    const char* base="/home/nexus/Applications/";
    if(strncmp(argv[2],base,strlen(base))||!argv[2][strlen(base)]||strchr(argv[2]+strlen(base),'/'))return invalid("destination must be a new application bundle"),2;
    char destination[PATH_MAX];if(strlen(argv[2])>=sizeof(destination))return 2;strcpy(destination,argv[2]+1);if(clean_name(destination))return 2;
    struct input input={.fd=-1};int parent=-1,stage=-1;bool owned=false;char leaf[PATH_MAX],temporary[80]={0};unsigned entries=0;int result=1;
    input.fd=open(argv[1],O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(input.fd<0)goto done;
    struct stat st;if(fstat(input.fd,&st)||!S_ISREG(st.st_mode)){invalid("package is not a regular file");goto done;}
    int root=open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(root<0)goto done;
    parent=parent_at(root,destination,leaf,false);close(root);if(parent<0)goto done;
    if(!fstatat(parent,leaf,&st,AT_SYMLINK_NOFOLLOW)||errno!=ENOENT){invalid("destination exists or cannot be checked");goto done;}
    for(unsigned attempt=0;attempt<100;attempt++){
        snprintf(temporary,sizeof(temporary),".nexus-install-%ld-%u",(long)getpid(),attempt);
        if(!mkdirat(parent,temporary,0700)){owned=true;break;}if(errno!=EEXIST)goto done;
    }
    if(!owned){errno=EEXIST;goto done;}stage=openat(parent,temporary,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(stage<0)goto done;
    sha256_init(&input.hash);if(payload(&input,stage,&entries))goto done;
    uint8_t digest[32];sha256_final(&input.hash,digest);if(crypto_memeq(expected,digest,32)){invalid("package SHA-256 mismatch; installation rejected");goto done;}
    if(close(input.fd)){input.fd=-1;goto done;}input.fd=-1;
    int receipt=openat(stage,".nexus-package-sha256",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(receipt<0)goto done;
    int receipt_error=write_all(receipt,(const uint8_t*)argv[3],64)||write_all(receipt,(const uint8_t*)"\n",1)||fsync(receipt);
    if(receipt_error&&ftruncate(receipt,0))perror("nexus-install: discard partial receipt");
    if(close(receipt))receipt_error=1;if(receipt_error)goto done;
    if(renameat2(parent,temporary,parent,leaf,RENAME_NOREPLACE))goto done;owned=false;result=0;
    printf("[64] NEXUS INSTALL VERIFIED path=%s members=%u package_bytes=%llu sha256=%s\n",argv[2],entries,(unsigned long long)input.bytes,argv[3]);
done:
    if(result)perror("nexus-install");
    if(owned){int status=stage>=0?remove_contents(stage):0;
        if(status||unlinkat(parent,temporary,AT_REMOVEDIR))fprintf(stderr,"nexus-install: incomplete staging cleanup: %s\n",temporary);
    }
    if(stage>=0)close(stage);if(parent>=0)close(parent);if(input.fd>=0)close(input.fd);return result;
}
