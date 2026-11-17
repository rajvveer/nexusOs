#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "inspect_json.h"
#include "../../kernel/crypto.h"

static int command_fd,reply_fd;
static unsigned sequence;
static bool startup_only,interactive,https_check,reject_certificate,download_check;
static char* incoming;
static size_t used,capacity;
static uint64_t milliseconds(void) { struct timespec time;clock_gettime(CLOCK_MONOTONIC,&time);return (uint64_t)time.tv_sec*1000+time.tv_nsec/1000000; }
static bool ready(int fd,short events,uint64_t deadline) {
    for(;;){uint64_t now=milliseconds();if(now>=deadline){errno=ETIMEDOUT;return false;}
        int timeout=deadline-now>1000?1000:(int)(deadline-now);struct pollfd descriptor={fd,events,0};
        int result=poll(&descriptor,1,timeout);if(result<0&&errno==EINTR)continue;
        if(result<0)return false;if(result>0)return true;
    }
}
static bool send_command(const char* message,uint64_t deadline) {
    printf("[64] CDP SEND %s\n",message);fflush(stdout);
    size_t length=strlen(message)+1,offset=0;
    while(offset<length){ssize_t count=send(command_fd,message+offset,length-offset,MSG_NOSIGNAL);
        if(count>0){offset+=(size_t)count;continue;}
        if(count<0&&errno==EINTR)continue;
        if(count<0&&(errno==EAGAIN||errno==EWOULDBLOCK)&&ready(command_fd,POLLOUT,deadline))continue;
        return false;
    }
    return true;
}
static char* receive_frame(uint64_t deadline) {
    for(;;){
        char* end=used?memchr(incoming,0,used):NULL;
        if(end){size_t size=(size_t)(end-incoming)+1;char* frame=malloc(size);if(!frame)return NULL;
            memcpy(frame,incoming,size);memmove(incoming,incoming+size,used-size);used-=size;return frame;}
        if(used==capacity){if(capacity>SIZE_MAX/2){errno=ENOMEM;return NULL;}size_t next=capacity?capacity*2:4096;char* data=realloc(incoming,next);if(!data)return NULL;incoming=data;capacity=next;}
        if(!ready(reply_fd,POLLIN,deadline))return NULL;
        ssize_t count=read(reply_fd,incoming+used,capacity-used);
        if(count>0){used+=(size_t)count;continue;}if(count<0&&(errno==EAGAIN||errno==EINTR))continue;
        if(!count)errno=EPIPE;return NULL;
    }
}
static char* request(const char* method,const char* params,const char* session,unsigned seconds) {
    unsigned id=++sequence;char* message=NULL;
    if(asprintf(&message,"{\"id\":%u,\"method\":\"%s\",\"params\":%s%s%s%s}",id,method,params?params:"{}",session?",\"sessionId\":\"":"",session?session:"",session?"\"":"")<0)return NULL;
    uint64_t deadline=milliseconds()+(uint64_t)seconds*1000;
    bool sent=send_command(message,deadline);free(message);if(!sent)return NULL;
    for(;;){char* frame=receive_frame(deadline);if(!frame){fprintf(stderr,"[64] CDP WAIT FAILED id=%u method=%s errno=%d\n",id,method,errno);return NULL;}
        if(!json_valid(frame)){fprintf(stderr,"[64] CDP malformed JSON\n");free(frame);errno=EPROTO;return NULL;}
        printf("[64] CDP RECV %s\n",frame);fflush(stdout);
        struct json_span object={frame,frame+strlen(frame)};
        if(json_id(json_member(object,"id"),id)){
            if(json_member(object,"error").begin||!json_member(object,"result").begin){free(frame);errno=EPROTO;return NULL;}
            return frame;
        }
        free(frame);
    }
}
static bool page_target(const char* message,char target[129]) {
    struct json_span object={message,message+strlen(message)},result=json_member(object,"result"),array=json_member(result,"targetInfos");
    if(!array.begin||*array.begin!='[')return false;
    const char* p=array.begin+1;json_space(&p,array.end);
    while(p<array.end&&*p!=']'){
        const char* begin=p;if(!json_skip(&p,array.end,0))return false;object=(struct json_span){begin,p};
        if(json_equal(json_member(object,"type"),"page")&&json_identifier(json_member(object,"targetId"),target,129))return true;
        json_space(&p,array.end);if(p<array.end&&*p==','){p++;json_space(&p,array.end);}else break;
    }
    return false;
}
static struct json_span runtime_value(const char* reply) {
    struct json_span object={reply,reply+strlen(reply)};
    return json_member(json_member(json_member(object,"result"),"result"),"value");
}
static bool download_file(const char* session) {
    char name[128],expected_hex[65],extra;unsigned long long expected_size;
    FILE* config=fopen("/nexus/download.expect","r");if(!config){perror("download manifest");return false;}
    int fields=fscanf(config,"%127s %llu %64s %c",name,&expected_size,expected_hex,&extra);fclose(config);
    if(fields!=3||!expected_size||strlen(expected_hex)!=64||!isalnum((unsigned char)name[0])||strstr(name,".."))return false;
    for(const char* p=name;*p;p++)if(!isalnum((unsigned char)*p)&&*p!='.'&&*p!='_'&&*p!='-')return false;
    uint8_t expected[32];
    for(unsigned i=0;i<64;i++) {
        unsigned char c=expected_hex[i];if(!isxdigit(c))return false;
        unsigned value=c<='9'?c-'0':(unsigned)tolower(c)-'a'+10;
        if(i&1)expected[i/2]|=value;else expected[i/2]=value<<4;
    }
    if(mkdir("/home/nexus/Downloads",0755)&&errno!=EEXIST){perror("download directory");return false;}
    char path[256],partial[272];snprintf(path,sizeof(path),"/home/nexus/Downloads/%s",name);snprintf(partial,sizeof(partial),"%s.crdownload",path);
    struct stat info;
    if(!lstat(path,&info)||errno!=ENOENT){fprintf(stderr,"[64] download target already exists or cannot be checked\n");return false;}
    char* reply=request("Runtime.evaluate","{\"expression\":\"(()=>{let a=document.getElementById('download').getBoundingClientRect();return {point:[Math.round(a.x+a.width/2),Math.round(a.y+a.height/2)],url:document.getElementById('download').href}})()\",\"returnByValue\":true}",session,10);
    if(!reply)return false;struct json_span layout=runtime_value(reply);
    if(!layout.begin||!json_member(layout,"point").begin){free(reply);return false;}
    printf("[64] CDP DOWNLOAD READY %.*s\n",(int)(layout.end-layout.begin),layout.begin);fflush(stdout);free(reply);
    uint64_t deadline=milliseconds()+900000,last_report=0;
    while(milliseconds()<deadline) {
        if(!stat(path,&info)) {
            if(!S_ISREG(info.st_mode)||(uint64_t)info.st_size>expected_size){fprintf(stderr,"[64] download target has unexpected type/size\n");return false;}
            /* Chrome may expose a reserved or growing final name before finishing the download. */
            if((uint64_t)info.st_size==expected_size){
            int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0){perror("download open");return false;}
            sha256_ctx_t hash;sha256_init(&hash);uint8_t data[65536],digest[32];uint64_t bytes=0;bool valid=true;
            for(;;){ssize_t count=read(fd,data,sizeof(data));if(count<0&&errno==EINTR)continue;if(count<0){valid=false;break;}if(!count)break;
                sha256_update(&hash,data,count);bytes+=count;if(bytes>expected_size){valid=false;break;}
                if(!(bytes%(16*1024*1024))){printf("[64] DOWNLOAD HASH bytes=%llu\n",(unsigned long long)bytes);fflush(stdout);}}
            sha256_final(&hash,digest);if(fsync(fd))valid=false;if(close(fd))valid=false;
            if(!valid||bytes!=expected_size||crypto_memeq(digest,expected,32)){fprintf(stderr,"[64] download hash/persistence mismatch\n");return false;}
            printf("[64] CHROME DOWNLOAD VERIFIED path=%s bytes=%llu sha256=%s\n",path,(unsigned long long)bytes,expected_hex);fflush(stdout);sleep(12);return true;
            }
        }
        else if(errno!=ENOENT){perror("download stat");return false;}
        if(milliseconds()-last_report>=5000){
            if(!stat(partial,&info))printf("[64] DOWNLOAD PROGRESS bytes=%llu\n",(unsigned long long)info.st_size);
            else printf("[64] DOWNLOAD WAITING for %s\n",name);
            fflush(stdout);last_report=milliseconds();
        }
        reply=receive_frame(milliseconds()+100);
        if(reply){printf("[64] CDP RECV %s\n",reply);free(reply);}else if(errno==EPIPE){fprintf(stderr,"[64] browser closed during download\n");return false;}
        usleep(200000);
    }
    fprintf(stderr,"[64] download deadline exceeded\n");return false;
}
static bool user_interaction(const char* session) {
    char* reply=request("Runtime.evaluate","{\"expression\":\"(()=>{let a=document.getElementById('text').getBoundingClientRect(),b=document.querySelector('button').getBoundingClientRect();return {input:[Math.round(a.x+a.width/2),Math.round(a.y+a.height/2)],button:[Math.round(b.x+b.width/2),Math.round(b.y+b.height/2)],viewport:[innerWidth,innerHeight],outer:[outerWidth,outerHeight],screen:[screenX,screenY],dpr:devicePixelRatio}})()\",\"returnByValue\":true}",session,10);
    if(!reply)return false;struct json_span layout=runtime_value(reply);
    if(!layout.begin||!json_member(layout,"input").begin){free(reply);return false;}
    printf("[64] CDP UI LAYOUT %.*s\n",(int)(layout.end-layout.begin),layout.begin);fflush(stdout);free(reply);
    uint64_t deadline=milliseconds()+60000;bool focused=false,typed=false;
    do {
        reply=request("Runtime.evaluate","{\"expression\":\"({active:document.activeElement.id,value:document.getElementById('text').value,color:getComputedStyle(document.body).backgroundColor})\",\"returnByValue\":true}",session,10);
        bool clicked=false;
        if(reply){struct json_span value=runtime_value(reply);
            if(!focused&&json_equal(json_member(value,"active"),"text")){focused=true;puts("[64] CDP UI INPUT FOCUSED");fflush(stdout);}
            if(!typed&&json_equal(json_member(value,"value"),"nexus")){typed=true;puts("[64] CDP UI TEXT VERIFIED");fflush(stdout);}
            clicked=typed&&json_equal(json_member(value,"color"),"rgb(24, 199, 132)");}
        free(reply);
        if(clicked){puts("[64] CDP USER INTERACTION VERIFIED (physical PS/2 input; DevTools only read DOM state)");fflush(stdout);if(download_check)return download_file(session);sleep(8);return true;}
        usleep(200000);
    }while(milliseconds()<deadline);
    fprintf(stderr,"[64] CDP user interaction not verified\n");return false;
}
static bool inspect(void) {
    char *reply=request("Browser.getVersion",NULL,NULL,240);if(!reply)return false;free(reply);
    char target[129]={0},session[129]={0};uint64_t deadline=milliseconds()+30000;
    do {reply=request("Target.getTargets",NULL,NULL,10);if(!reply)return false;bool found=page_target(reply,target);free(reply);if(found)break;usleep(100000);}while(milliseconds()<deadline);
    if(!*target){fprintf(stderr,"[64] CDP no page target\n");return false;}
    char params[256];snprintf(params,sizeof(params),"{\"targetId\":\"%s\",\"flatten\":true}",target);
    reply=request("Target.attachToTarget",params,NULL,30);if(!reply)return false;
    struct json_span object={reply,reply+strlen(reply)};
    bool attached=json_identifier(json_member(json_member(object,"result"),"sessionId"),session,sizeof(session));free(reply);if(!attached)return false;
    reply=request("Page.enable",NULL,session,30);bool passed=reply!=NULL;free(reply);
    reply=request("Page.getFrameTree",NULL,session,30);passed=passed&&reply;free(reply);
    reply=request("Runtime.evaluate","{\"expression\":\"JSON.stringify({url:location.href,state:document.readyState,title:document.title,html:document.documentElement.outerHTML,width:innerWidth,height:innerHeight,sum:123*456})\",\"returnByValue\":true}",session,45);
    passed=passed&&reply;free(reply);
    if(startup_only){
        uint64_t until=milliseconds()+90000;
        do {
            reply=request("Runtime.evaluate","{\"expression\":\"({url:location.href,state:document.readyState,title:document.title,html:document.documentElement.outerHTML,visible:document.visibilityState,width:innerWidth,height:innerHeight,secure:String(isSecureContext),example:String(document.body.textContent.includes('documentation examples')),rejected:String(document.body.innerText.toUpperCase().includes('NET::ERR_CERT_AUTHORITY_INVALID'))})\",\"returnByValue\":true}",session,10);
            bool loaded=false;
            if(reply){object=(struct json_span){reply,reply+strlen(reply)};struct json_span state=json_member(json_member(json_member(object,"result"),"result"),"value");
                loaded=json_equal(json_member(state,"title"),https_check?"Example Domain":"NexusOS interactive test")&&json_equal(json_member(state,"state"),"complete");
                if(https_check)loaded=loaded&&json_equal(json_member(state,"url"),"https://example.com/")&&json_equal(json_member(state,"secure"),"true")&&json_equal(json_member(state,"example"),"true");
                if(reject_certificate)loaded=json_equal(json_member(state,"state"),"complete")&&json_equal(json_member(state,"rejected"),"true");}
            free(reply);
            if(loaded){puts(reject_certificate?"[64] CDP UNTRUSTED CERTIFICATE REJECTED":https_check?"[64] CDP HTTPS NAVIGATION VERIFIED (public hostname, secure context, original document)":"[64] CDP STARTUP NAVIGATION VERIFIED (no retry or document injection)");fflush(stdout);if(interactive)return passed&&user_interaction(session);sleep(https_check||reject_certificate?20:8);return passed;}
            sleep(3);
        }while(milliseconds()<until);
        fprintf(stderr,"[64] CDP STARTUP NAVIGATION STILL PENDING\n");return false;
    }
    snprintf(params,sizeof(params),"{\"targetId\":\"%s\"}",target);
    reply=request("Browser.getWindowForTarget",params,NULL,10);free(reply);
    reply=request("Page.bringToFront",NULL,session,10);free(reply);
    reply=request("Runtime.evaluate","{\"expression\":\"({url:location.href,state:document.readyState,title:document.title,visible:document.visibilityState,focus:document.hasFocus()})\",\"returnByValue\":true}",session,10);free(reply);
    reply=request("Page.stopLoading",NULL,session,10);free(reply);
    reply=request("Runtime.evaluate","{\"expression\":\"document.body.style.cssText='background:#18c784;color:#0b1220;font:32px sans-serif;padding:40px';document.body.innerHTML='<h1>Native renderer diagnostic</h1><p>JavaScript: '+(123*456)+'</p>';document.title='Nexus renderer diagnostic';JSON.stringify({url:location.href,state:document.readyState,visible:document.visibilityState,focus:document.hasFocus(),html:document.documentElement.outerHTML})\",\"returnByValue\":true}",session,30);
    if(reply){puts("[64] CDP RENDER DIAGNOSTIC READY (injected document, not a navigation pass)");fflush(stdout);sleep(8);}free(reply);
    reply=request("Network.enable",NULL,session,10);free(reply);
    reply=request("Page.navigate","{\"url\":\"data:text/html,<title>Nexus navigation</title><h1>Native navigation</h1><script>document.title='Nexus'+(123*456)</script>\"}",session,45);free(reply);
    reply=request("Runtime.evaluate","{\"expression\":\"({url:location.href,state:document.readyState,title:document.title,html:document.documentElement.outerHTML,visible:document.visibilityState,width:innerWidth,height:innerHeight,sum:123*456})\",\"returnByValue\":true}",session,30);
    bool navigated=false;
    if(reply){object=(struct json_span){reply,reply+strlen(reply)};struct json_span result=json_member(json_member(json_member(object,"result"),"result"),"value");
        navigated=json_equal(json_member(result,"title"),"Nexus56088")&&json_equal(json_member(result,"state"),"complete")&&json_id(json_member(result,"sum"),56088);
        if(navigated){puts("[64] CDP NAVIGATION VERIFIED (normal data URL and page JavaScript; visual verification separate)");fflush(stdout);sleep(8);}}
    free(reply);passed=passed&&navigated;
    reply=request("Target.getTargets",NULL,NULL,10);free(reply);
    return passed;
}
static bool parser_selftest(void) {
    const char* valid[]={"{}"," {\"x\":[null,true,false,-12.5e+3,\"a\\\"b\"]} ","[0,\"\\u1234\"]"};
    const char* invalid[]={"", "{", "[1,]", "{\"a\":1,}", "[01]", "[1e]", "{\"a\":\"\\q\"}", "true false", "\"x\n\""};
    for(size_t i=0;i<sizeof(valid)/sizeof(*valid);i++)if(!json_valid(valid[i]))return false;
    for(size_t i=0;i<sizeof(invalid)/sizeof(*invalid);i++)if(json_valid(invalid[i]))return false;
    const char* text="{\"nested\":{\"id\":1},\"id\":7,\"sessionId\":\"ABC-012\"}";struct json_span object={text,text+strlen(text)};char id[32];
    if(!json_id(json_member(object,"id"),7)||!json_identifier(json_member(object,"sessionId"),id,sizeof(id))||strcmp(id,"ABC-012"))return false;
    const char* target="{\"result\":{\"targetInfos\":[{\"type\":\"worker\",\"targetId\":\"SKIP\"},{\"type\":\"page\",\"targetId\":\"TEST\",\"title\":\"targetId: \\\"fake\\\"\"}]}}";char page[129];
    return json_valid(target)&&page_target(target,page)&&!strcmp(page,"TEST");
}
int main(int argc,char** argv) {
    if(!parser_selftest()){fprintf(stderr,"CDP parser self-test failed\n");return 1;}
    if(argc<2){fprintf(stderr,"usage: chrome-inspect PROGRAM [ARGUMENTS]\n");return 1;}
    if(!strcmp(argv[1],"--startup")||!strcmp(argv[1],"--interactive")||!strcmp(argv[1],"--https")||!strcmp(argv[1],"--reject-certificate")||!strcmp(argv[1],"--download")){startup_only=true;download_check=!strcmp(argv[1],"--download");interactive=download_check||!strcmp(argv[1],"--interactive");https_check=!strcmp(argv[1],"--https");reject_certificate=!strcmp(argv[1],"--reject-certificate");for(int i=1;i<argc;i++)argv[i]=argv[i+1];argc--;if(argc<2)return 1;}
    int input[2],output[2];
    /* Chrome shuts these endpoints down on exit; socketpairs preserve that contract. */
    if(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,input)||socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,output)){perror("CDP socketpair");return 1;}
    pid_t child=fork();if(child<0){perror("CDP fork");return 1;}
    if(!child){
        int in=fcntl(input[1],F_DUPFD_CLOEXEC,10),out=fcntl(output[1],F_DUPFD_CLOEXEC,10);
        if(in<0||out<0)_exit(127);
        close(input[0]);close(input[1]);close(output[0]);close(output[1]);
        if(dup2(in,3)!=3||dup2(out,4)!=4)_exit(127);close(in);close(out);
        char** args=calloc((size_t)argc+1,sizeof(*args));if(!args)_exit(127);
        for(int i=1;i<argc;i++)args[i-1]=argv[i];args[argc-1]="--remote-debugging-pipe";
        execv(args[0],args);perror("CDP exec");_exit(127);
    }
    close(input[1]);close(output[1]);command_fd=input[0];reply_fd=output[0];
    if(fcntl(command_fd,F_SETFL,O_NONBLOCK)||fcntl(reply_fd,F_SETFL,O_NONBLOCK)){perror("CDP nonblock");return 1;}
    bool passed=inspect();char* reply=request("Browser.close",NULL,NULL,10);free(reply);
    int status;uint64_t deadline=milliseconds()+30000;
    for(;;){pid_t done=waitpid(child,&status,WNOHANG);if(done==child)break;if(done<0){perror("CDP wait");return 1;}
        if(milliseconds()>=deadline){fprintf(stderr,"[64] CDP normal browser shutdown timed out\n");return 1;}
        reply=receive_frame(milliseconds()+100);if(reply){printf("[64] CDP RECV %s\n",reply);fflush(stdout);free(reply);}else if(errno==EPIPE)usleep(10000);
    }
    close(command_fd);close(reply_fd);free(incoming);
    printf("[64] CDP DIAGNOSTIC DONE responses=%s child_status=%d (GUI/download/install gates remain separate)\n",passed?"complete":"incomplete",status);
    return passed&&WIFEXITED(status)&&WEXITSTATUS(status)==0?0:1;
}
