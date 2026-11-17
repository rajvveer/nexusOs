#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) { dprintf(2,"[64] UDP FAIL line=%d errno=%d\n",__LINE__,errno);_exit(99); } } while (0)
static struct sockaddr_in peer;
static uint64_t now(void) { struct timespec t;CHECK(clock_gettime(CLOCK_MONOTONIC,&t)==0);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
static void readable(int fd) { struct pollfd item={fd,POLLIN,0};CHECK(poll(&item,1,10000)==1&&(item.revents&POLLIN)); }
static int create_socket(int nonblock) { int fd=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC|(nonblock?SOCK_NONBLOCK:0),0);CHECK(fd>=0);return fd; }
static void send_packet(int fd,const void* data,size_t size) { CHECK(sendto(fd,data,size,0,(void*)&peer,sizeof(peer))==(ssize_t)size); }
static void close_fd(int fd) { CHECK(close(fd)==0); }
static void packets(void) {
    int fd=create_socket(1);send_packet(fd,"orphan",6);close_fd(fd);CHECK(poll(NULL,0,150)==0);
    fd=create_socket(1);struct sockaddr_in local={.sin_family=AF_INET};CHECK(bind(fd,(void*)&local,sizeof(local))==0);
    socklen_t size=sizeof(local);CHECK(getsockname(fd,(void*)&local,&size)==0&&size==16&&local.sin_port);
    int tcp=socket(AF_INET,SOCK_STREAM,0);CHECK(tcp>=0&&bind(tcp,(void*)&local,sizeof(local))==0);close_fd(tcp);
    int value;socklen_t option_size=sizeof(value);CHECK(getsockopt(fd,SOL_SOCKET,SO_TYPE,&value,&option_size)==0&&value==SOCK_DGRAM);
    CHECK(getsockopt(fd,SOL_SOCKET,SO_PROTOCOL,&value,&option_size)==0&&value==IPPROTO_UDP);
    value=8192;CHECK(setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&value,sizeof(value))==0);
    value=0;CHECK(getsockopt(fd,SOL_SOCKET,SO_RCVBUF,&value,&option_size)==0&&value==16384);
    CHECK(send(fd,"X",1,0)==-1&&errno==EDESTADDRREQ);
    uint8_t data[1473],output[1600];for(unsigned i=0;i<sizeof(data);i++)data[i]=(i*37+11)&255;
    /* Linux: beyond the path MTU a datagram is fragmented, unless IP_PMTUDISC_DO asks for EMSGSIZE */
    int discover=IP_PMTUDISC_DO;CHECK(setsockopt(fd,IPPROTO_IP,IP_MTU_DISCOVER,&discover,sizeof(discover))==0);
    CHECK(sendto(fd,data,1473,0,(void*)&peer,sizeof(peer))==-1&&errno==EMSGSIZE);
    discover=IP_PMTUDISC_WANT;CHECK(setsockopt(fd,IPPROTO_IP,IP_MTU_DISCOVER,&discover,sizeof(discover))==0);
    struct iovec vectors[3]={{data,17},{data+17,700},{data+717,755}};
    struct msghdr message={.msg_name=&peer,.msg_namelen=sizeof(peer),.msg_iov=vectors,.msg_iovlen=3};
    CHECK(sendmsg(fd,&message,0)==1472);readable(fd);
    CHECK(ioctl(fd,FIONREAD,&value)==0&&value==1472);
    CHECK(syscall(SYS_recvfrom,fd,(void*)1,1472,0,0,0)==-1&&errno==EFAULT);
    CHECK(recv(fd,output,17,MSG_PEEK|MSG_TRUNC)==1472&&!memcmp(output,data,17));
    struct sockaddr_in source;char control[64];
    vectors[0]=(struct iovec){output,7};vectors[1]=(struct iovec){output+7,10};
    message=(struct msghdr){.msg_name=&source,.msg_namelen=sizeof(source),.msg_iov=vectors,.msg_iovlen=2,.msg_control=control,.msg_controllen=sizeof(control)};
    CHECK(recvmsg(fd,&message,MSG_PEEK)==17&&(message.msg_flags&MSG_TRUNC)&&message.msg_controllen==0&&message.msg_namelen==16);
    CHECK(source.sin_family==AF_INET&&source.sin_port==peer.sin_port&&source.sin_addr.s_addr==peer.sin_addr.s_addr);
    size=2;memset(&source,0xAA,sizeof(source));CHECK(recvfrom(fd,output,sizeof(output),0,(void*)&source,&size)==1472&&size==16&&source.sin_family==AF_INET&&source.sin_port==0xAAAA);
    CHECK(!memcmp(output,data,1472)&&recv(fd,output,1,0)==-1&&errno==EAGAIN);
    send_packet(fd,data,1000);readable(fd);CHECK(recv(fd,output,9,0)==9&&recv(fd,output,1,0)==-1&&errno==EAGAIN);
    send_packet(fd,NULL,0);readable(fd);CHECK(ioctl(fd,FIONREAD,&value)==0&&value==0);
    CHECK(read(fd,output,0)==0);readable(fd);CHECK(recv(fd,NULL,0,0)==0&&recv(fd,output,1,0)==-1&&errno==EAGAIN);
    CHECK(connect(fd,(void*)&peer,sizeof(peer))==0);CHECK(write(fd,"filter",6)==6);readable(fd);
    CHECK(read(fd,output,sizeof(output))==4&&!memcmp(output,"good",4));
    struct sockaddr unspec={.sa_family=AF_UNSPEC};CHECK(connect(fd,&unspec,sizeof(unspec))==0);
    size=sizeof(source);CHECK(getpeername(fd,(void*)&source,&size)==-1&&errno==ENOTCONN);
    CHECK(getsockname(fd,(void*)&source,&size)==0&&source.sin_port==local.sin_port&&source.sin_addr.s_addr==0);
    CHECK(send(fd,"x",1,0)==-1&&errno==EDESTADDRREQ);
    CHECK(connect(fd,(void*)&peer,sizeof(peer))==0);
    struct iovec batches[2]={{"one",3},{"two",3}};struct mmsghdr messages[2]={0};
    for(unsigned i=0;i<2;i++){messages[i].msg_hdr.msg_iov=&batches[i];messages[i].msg_hdr.msg_iovlen=1;}
    CHECK(sendmmsg(fd,messages,2,0)==2&&messages[0].msg_len==3&&messages[1].msg_len==3);
    readable(fd);CHECK(recv(fd,output,sizeof(output),0)==3&&!memcmp(output,"one",3));
    readable(fd);CHECK(recv(fd,output,sizeof(output),0)==3&&!memcmp(output,"two",3));
    batches[1].iov_base=(void*)1;messages[1].msg_len=0x12345678;
    CHECK(sendmmsg(fd,messages,2,0)==1&&messages[1].msg_len==0x12345678);
    readable(fd);CHECK(recv(fd,output,sizeof(output),0)==3&&!memcmp(output,"one",3));
    CHECK(sendmmsg(fd,messages+1,1,0)==-1&&errno==EFAULT);
    CHECK(sendmmsg(fd,NULL,0,0)==0);
    struct iovec pair[2]={{"ab",2},{"cd",2}};CHECK(writev(fd,pair,2)==4);readable(fd);
    pair[0]=(struct iovec){output,2};pair[1]=(struct iovec){output+2,sizeof(output)-2};CHECK(readv(fd,pair,2)==4&&!memcmp(output,"abcd",4));
    CHECK(shutdown(fd,SHUT_RDWR)==0&&recv(fd,output,1,0)==0&&send(fd,"x",1,MSG_NOSIGNAL)==-1&&errno==EPIPE);
    close_fd(fd);
    puts("[64] UDP MTU, VECTORS, PEEK/TRUNCATION, EMPTY PACKETS, SOURCE AND CONNECT/DISCONNECT PASSED");
}
static void many_sockets(void) {
    int handles[32],ep=epoll_create1(0);CHECK(ep>=0);
    for(unsigned i=0;i<32;i++) {
        handles[i]=create_socket(1);CHECK(connect(handles[i],(void*)&peer,sizeof(peer))==0);
        struct epoll_event event={.events=EPOLLIN|EPOLLET,.data.u32=i};CHECK(epoll_ctl(ep,EPOLL_CTL_ADD,handles[i],&event)==0);
        uint32_t token=0xABCD0000|i;CHECK(send(handles[i],&token,sizeof(token),0)==sizeof(token));
    }
    uint32_t seen=0;
    while(seen!=UINT32_MAX) {
        struct epoll_event events[32];int count=epoll_wait(ep,events,32,10000);CHECK(count>0);
        for(int j=0;j<count;j++) { unsigned i=events[j].data.u32;uint32_t value;CHECK(i<32&&!(seen&(1U<<i)));
            CHECK(recv(handles[i],&value,sizeof(value),0)==sizeof(value)&&value==(0xABCD0000|i));seen|=1U<<i; }
    }
    for(unsigned i=0;i<32;i++)close_fd(handles[i]);close_fd(ep);
    puts("[64] UDP 32 CONCURRENT DESCRIPTORS AND EPOLL PASSED");
}
static void unix_batch(void) {
    int pair[2];CHECK(socketpair(AF_UNIX,SOCK_SEQPACKET,0,pair)==0);
    int file=open("/etc/native64.dat",O_RDONLY);CHECK(file>=0);
    union { struct cmsghdr align; char data[CMSG_SPACE(sizeof(int))]; } control={0};
    struct iovec vectors[2]={{"rights",6},{"next",4}};struct mmsghdr messages[2]={0};
    for(unsigned i=0;i<2;i++){messages[i].msg_hdr.msg_iov=&vectors[i];messages[i].msg_hdr.msg_iovlen=1;}
    messages[0].msg_hdr.msg_control=control.data;messages[0].msg_hdr.msg_controllen=sizeof(control.data);
    struct cmsghdr* c=CMSG_FIRSTHDR(&messages[0].msg_hdr);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(c),&file,sizeof(file));
    CHECK(sendmmsg(pair[0],messages,2,0)==2&&messages[0].msg_len==6&&messages[1].msg_len==4);close_fd(file);
    char data[8];struct iovec vector={data,sizeof(data)};struct msghdr received={.msg_iov=&vector,.msg_iovlen=1,.msg_control=control.data,.msg_controllen=sizeof(control.data)};
    CHECK(recvmsg(pair[1],&received,MSG_CMSG_CLOEXEC)==6&&!memcmp(data,"rights",6));
    c=CMSG_FIRSTHDR(&received);CHECK(c&&c->cmsg_type==SCM_RIGHTS);memcpy(&file,CMSG_DATA(c),sizeof(file));
    CHECK(fcntl(file,F_GETFD)==FD_CLOEXEC);unsigned char bytes[3];CHECK(read(file,bytes,3)==3&&bytes[0]==11&&bytes[1]==48&&bytes[2]==85);close_fd(file);
    CHECK(recv(pair[1],data,sizeof(data),0)==4&&!memcmp(data,"next",4));close_fd(pair[0]);close_fd(pair[1]);
    CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);struct mmsghdr* empty=calloc(1025,sizeof(*empty));CHECK(empty);
    empty[1024].msg_len=0xABABABAB;CHECK(sendmmsg(pair[0],empty,1025,0)==1024&&empty[1024].msg_len==0xABABABAB);
    free(empty);close_fd(pair[0]);close_fd(pair[1]);
    puts("[64] SENDMMSG UNIX RIGHTS, RECORD BOUNDARIES AND BATCH LIMIT PASSED");
}
static pthread_t parent_thread;
static volatile sig_atomic_t signalled;
static void handler(int number) { signalled=number; }
static void* interrupt_read(void* unused) { (void)unused;CHECK(poll(NULL,0,120)==0);CHECK(pthread_kill(parent_thread,SIGUSR1)==0);return NULL; }
static void timeouts(void) {
    int fd=create_socket(0);CHECK(connect(fd,(void*)&peer,sizeof(peer))==0);
    struct timeval timeout={.tv_usec=120000};CHECK(setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))==0);
    char data;uint64_t start=now();CHECK(recv(fd,&data,1,0)==-1&&errno==EAGAIN&&now()-start>=100000000);
    timeout.tv_usec=0;CHECK(setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))==0);
    struct sigaction action={.sa_handler=handler};CHECK(sigaction(SIGUSR1,&action,NULL)==0);parent_thread=pthread_self();pthread_t thread;
    CHECK(pthread_create(&thread,NULL,interrupt_read,NULL)==0);CHECK(recv(fd,&data,1,0)==-1&&errno==EINTR&&signalled==SIGUSR1);
    CHECK(pthread_join(thread,NULL)==0);close_fd(fd);puts("[64] UDP BLOCKING TIMEOUT AND SIGNAL PASSED");
}
static void resolver(void) {
    struct addrinfo hints={.ai_family=AF_INET,.ai_socktype=SOCK_STREAM},*addresses=NULL;
    int result=getaddrinfo("example.com","443",&hints,&addresses);
    if(result)fprintf(stderr,"[64] DNS error=%d %s\n",result,gai_strerror(result));
    CHECK(result==0&&addresses);unsigned count=0;
    for(struct addrinfo* a=addresses;a;a=a->ai_next) {
        CHECK(a->ai_family==AF_INET&&a->ai_addrlen==sizeof(struct sockaddr_in));struct sockaddr_in* address=(void*)a->ai_addr;
        CHECK(address->sin_addr.s_addr&&address->sin_port==htons(443));char text[INET_ADDRSTRLEN];CHECK(inet_ntop(AF_INET,&address->sin_addr,text,sizeof(text)));
        printf("[64] DNS example.com %s\n",text);count++;
    }
    freeaddrinfo(addresses);CHECK(count);
    hints.ai_family=AF_UNSPEC;addresses=NULL;result=getaddrinfo("example.com","443",&hints,&addresses);
    if(result)fprintf(stderr,"[64] DNS dual-family error=%d %s\n",result,gai_strerror(result));
    CHECK(result==0&&addresses);count=0;
    for(struct addrinfo* a=addresses;a;a=a->ai_next) { CHECK(a->ai_family==AF_INET||a->ai_family==AF_INET6);if(a->ai_family==AF_INET)count++; }
    freeaddrinfo(addresses);CHECK(count);
    result=getaddrinfo("nexus-no-such-name.invalid",NULL,&hints,&addresses);
    CHECK(result==EAI_NONAME);
    puts("[64] GLIBC DNS RESOLVER AND NXDOMAIN PASSED");
}
int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);uint16_t port;int config=open("/nexus/udp-port",O_RDONLY);CHECK(config>=0&&read(config,&port,2)==2);close_fd(config);
    peer=(struct sockaddr_in){.sin_family=AF_INET,.sin_port=htons(port),.sin_addr.s_addr=htonl(0x0A000202)};
    packets();many_sockets();unix_batch();timeouts();resolver();CHECK(poll(NULL,0,200)==0);
    puts("[64] UDP SOCKET CAMPAIGN PASSED");return 0;
}
