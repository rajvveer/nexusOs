/* Native local-package window, also installed as xdg-open for browser downloads. */
#define _GNU_SOURCE
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "local_file.h"
#include "../../kernel/crypto.h"
#include "../../kernel/uifont.h"

enum phase {CHECKING,READY,INSTALLING,INSTALLED,RUNNING,FINISHED,FAILED};
enum operation {HASH_PACKAGE,INSTALL_PACKAGE,RUN_PROGRAM};
static enum phase phase=CHECKING;
static enum operation operation;
static char package[PATH_MAX],bundle[PATH_MAX],digest[65],message[256]="Checking package...";
static char output[32768];static size_t output_size;static bool output_truncated;
static int apps_fd=-1,bundle_fd=-1;
static char** programs;static size_t program_count,selected,scroll;
static pid_t worker=-1;static int worker_fd=-1,worker_status;static bool working,worker_done,closed,redraw=true,busy,configured;
static struct wl_display* display;
static struct wl_compositor* compositor;static struct wl_shm* shm;static struct xdg_wm_base* shell;
static struct wl_surface* surface;static struct xdg_surface* xdg;static struct xdg_toplevel* top;
static struct wl_buffer* buffer;static uint32_t* pixels;static size_t buffer_size;
static int width=960,height=720,buffer_width,buffer_height,mouse_x,mouse_y,focus_control;
static bool shift;
static int exit_status;
static uint64_t milliseconds(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static void paint(void);
static void activate(void);
static void fail(const char* text){snprintf(message,sizeof(message),"%s",text);phase=FAILED;focus_control=1;redraw=true;exit_status=1;dprintf(2,"[64] INSTALLER ERROR %s\n",text);}
static int applications(void){
    int fd=open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -1;
    const char* parts[]={"home","nexus","Applications"};
    for(unsigned i=0;i<3;i++){
        if(i==2&&mkdirat(fd,parts[i],0700)&&errno!=EEXIST){close(fd);return -1;}
        int next=openat(fd,parts[i],O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);close(fd);if(next<0)return -1;fd=next;
    }
    return fd;
}
static int relative_open(int root,const char* path){
    if(!*path||*path=='/'||strlen(path)>=PATH_MAX){errno=EINVAL;return -1;}
    char copy[PATH_MAX];strcpy(copy,path);int fd=fcntl(root,F_DUPFD_CLOEXEC,3);if(fd<0)return -1;
    char* part=copy;
    for(;;){char* slash=strchr(part,'/');if(slash)*slash=0;
        if(!*part||!strcmp(part,".")||!strcmp(part,"..")){close(fd);errno=EINVAL;return -1;}
        int next=openat(fd,part,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|(slash?O_DIRECTORY:0));close(fd);if(next<0)return -1;
        if(!slash)return next;fd=next;part=slash+1;
    }
}
static int executable(int fd){
    struct stat st;Elf64_Ehdr header;
    if(fstat(fd,&st))return -1;if(!S_ISREG(st.st_mode))return 0;
    ssize_t size=pread(fd,&header,sizeof(header),0);if(size<0)return -1;
    return size==sizeof(header)&&
        !memcmp(header.e_ident,ELFMAG,SELFMAG)&&header.e_ident[EI_CLASS]==ELFCLASS64&&header.e_ident[EI_DATA]==ELFDATA2LSB&&
        header.e_machine==EM_X86_64&&(header.e_type==ET_EXEC||header.e_type==ET_DYN)&&header.e_entry;
}
static int program_open(const char* relative){
    char full[PATH_MAX];if(snprintf(full,sizeof(full),"%s/%s",bundle,relative)>=(int)sizeof(full)){errno=ENAMETOOLONG;return -1;}
    char* real=realpath(full,NULL);if(!real)return -1;size_t prefix=strlen(bundle);
    if(strncmp(real,bundle,prefix)||real[prefix]!='/'){free(real);errno=EACCES;return -1;}
    int fd=relative_open(bundle_fd,real+prefix+1);free(real);
    if(fd>=0){int type=executable(fd);if(type<=0){int error=type<0?errno:ENOEXEC;close(fd);errno=error;return -1;}}return fd;
}
static bool scan_programs(int directory,const char* prefix){
    int copy=openat(directory,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(copy<0)return false;
    DIR* dir=fdopendir(copy);if(!dir){close(copy);return false;}bool success=true;
    for(;;){errno=0;struct dirent* entry=readdir(dir);if(!entry){if(errno)success=false;break;}
        if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
        char relative[PATH_MAX];if(snprintf(relative,sizeof(relative),"%s%s%s",prefix,*prefix?"/":"",entry->d_name)>=(int)sizeof(relative)){success=false;errno=ENAMETOOLONG;break;}
        struct stat st;if(fstatat(directory,entry->d_name,&st,AT_SYMLINK_NOFOLLOW)){success=false;break;}
        if(S_ISDIR(st.st_mode)){
            int child=openat(directory,entry->d_name,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(child<0){success=false;break;}
            success=scan_programs(child,relative);close(child);if(!success)break;
        }else if(S_ISREG(st.st_mode)||S_ISLNK(st.st_mode)){
            int fd=program_open(relative);if(fd<0){if(errno==ENOEXEC||errno==ENOENT||errno==ELOOP||errno==EACCES)continue;success=false;break;}close(fd);
            if(program_count>=SIZE_MAX/sizeof(*programs)-1){success=false;errno=ENOMEM;break;}
            char** next=realloc(programs,(program_count+1)*sizeof(*programs));if(!next){success=false;break;}programs=next;
            programs[program_count]=strdup(relative);if(!programs[program_count]){success=false;break;}program_count++;
        }
    }
    int error=errno;if(closedir(dir)&&success)return false;errno=error;return success;
}
static int compare_programs(const void* a,const void* b){return strcmp(*(char* const*)a,*(char* const*)b);}
static bool receipt_matches(int fd){
    int receipt=openat(fd,NEXUS_PACKAGE_RECEIPT,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(receipt<0)return false;
    char text[66];struct stat st;bool good=!fstat(receipt,&st)&&S_ISREG(st.st_mode)&&st.st_size==65&&read(receipt,text,sizeof(text))==65&&
        !memcmp(text,digest,64)&&text[64]=='\n';close(receipt);return good;
}
static bool load_installed(void){
    bundle_fd=openat(apps_fd,digest,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(bundle_fd<0)return false;
    if(!receipt_matches(bundle_fd)){close(bundle_fd);bundle_fd=-1;errno=EEXIST;return false;}
    if(!scan_programs(bundle_fd,""))return false;
    qsort(programs,program_count,sizeof(*programs),compare_programs);selected=scroll=0;phase=INSTALLED;
    snprintf(message,sizeof(message),program_count?"Installed. Choose a program to run.":"Installed files. No directly launchable 64-bit program was found.");
    printf("[64] INSTALLER READY TO RUN programs=%zu bundle=%s\n",program_count,bundle);fflush(stdout);return true;
}
static void hash_package(void){
    int fd=open(package,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);struct stat st;
    if(fd<0||fstat(fd,&st)){perror("Open package");_exit(1);}
    if(!S_ISREG(st.st_mode)){dprintf(2,"Choose a regular local package file.\n");_exit(1);}
    uint8_t data[65536],hash[32];ssize_t first=read(fd,data,8);
    if(first!=8||memcmp(data,"!<arch>\n",8)){dprintf(2,"This is not a supported Debian package.\n");_exit(1);}
    sha256_ctx_t context;sha256_init(&context);sha256_update(&context,data,8);uint64_t total=8;
    for(;;){ssize_t n=read(fd,data,sizeof(data));if(n<0&&errno==EINTR)continue;if(n<0){perror("Read package");_exit(1);}if(!n)break;sha256_update(&context,data,n);total+=n;}
    if(total!=(uint64_t)st.st_size||close(fd)){dprintf(2,"The package changed while it was being checked.\n");_exit(1);}
    sha256_final(&context,hash);char hex[66];for(unsigned i=0;i<32;i++){hex[i*2]="0123456789abcdef"[hash[i]>>4];hex[i*2+1]="0123456789abcdef"[hash[i]&15];}hex[64]='\n';hex[65]=0;
    if(write(1,hex,65)!=65)_exit(1);_exit(0);
}
extern char** environ;
static void launch_program(void){
    int fd=program_open(programs[selected]);if(fd<0){perror("Open installed program");_exit(126);}
    const char* name=strrchr(programs[selected],'/');name=name?name+1:programs[selected];
    char* args[]={(char*)name,NULL,NULL,NULL,NULL,NULL,NULL};
    char profile[PATH_MAX];
    if(!strcmp(programs[selected],"opt/google/chrome/chrome")){
        /* The verified Chrome backend on this hardware is software Wayland; other ELF programs use their normal arguments. */
        snprintf(profile,sizeof(profile),"--user-data-dir=/home/nexus/chrome-package-%s",digest);
        args[1]="--ozone-platform=wayland";args[2]="--disable-gpu";args[3]="--no-first-run";args[4]="--ipc-connection-timeout=120";args[5]=profile;
    }
    if(fchdir(bundle_fd)){perror("Open application folder");_exit(126);}
    fexecve(fd,args,environ);perror("Launch installed program");_exit(126);
}
static void start_job(enum operation op){
    if(working||closed)return;int pipefd[2];if(pipe2(pipefd,O_CLOEXEC)){fail("Could not create the worker channel.");return;}
    int read_fd=fcntl(pipefd[0],F_DUPFD_CLOEXEC,3),write_fd=fcntl(pipefd[1],F_DUPFD_CLOEXEC,3);close(pipefd[0]);close(pipefd[1]);
    if(read_fd<0||write_fd<0){if(read_fd>=0)close(read_fd);if(write_fd>=0)close(write_fd);fail("Could not prepare worker descriptors.");return;}
    pipefd[0]=read_fd;pipefd[1]=write_fd;
    if(fcntl(pipefd[0],F_SETFL,O_NONBLOCK)){close(pipefd[0]);close(pipefd[1]);fail("Could not read worker output.");return;}
    fflush(stdout);pid_t pid=fork();if(pid<0){close(pipefd[0]);close(pipefd[1]);fail("Could not start the worker.");return;}
    if(!pid){
        close(wl_display_get_fd(display));if(dup2(pipefd[1],1)!=1||dup2(pipefd[1],2)!=2)_exit(125);close(pipefd[0]);close(pipefd[1]);
        int null=open("/dev/null",O_RDONLY);if(null<0||dup2(null,0)!=0)_exit(125);if(null)close(null);
        if(op==HASH_PACKAGE)hash_package();
        if(op==INSTALL_PACKAGE){execl("/bin/nexus-install","nexus-install",package,bundle,digest,(char*)NULL);perror("Start installer");_exit(125);}
        launch_program();
    }
    close(pipefd[1]);worker_fd=pipefd[0];
    worker=pid;working=true;worker_done=false;operation=op;output_size=0;output[0]=0;output_truncated=false;
    phase=op==HASH_PACKAGE?CHECKING:op==INSTALL_PACKAGE?INSTALLING:RUNNING;focus_control=1;
    snprintf(message,sizeof(message),op==HASH_PACKAGE?"Checking package...":op==INSTALL_PACKAGE?"Installing application...":"Application is running.");redraw=true;
    printf("[64] INSTALLER JOB %d pid=%ld\n",op,(long)pid);fflush(stdout);
}
static void append_output(const char* data,size_t n){
    const size_t limit=sizeof(output)-1;if(n>=limit){data+=n-limit;n=limit;output_size=0;output_truncated=true;}
    if(n>limit-output_size){size_t remove=n-(limit-output_size);memmove(output,output+remove,output_size-remove);output_size-=remove;output_truncated=true;}
    memcpy(output+output_size,data,n);output_size+=n;output[output_size]=0;redraw=true;
}
static void finish_job(void){
    working=false;worker=-1;int code=WIFEXITED(worker_status)?WEXITSTATUS(worker_status):128+WTERMSIG(worker_status);
    printf("[64] INSTALLER JOB DONE %d status=%d output_bytes=%zu\n",operation,code,output_size);fflush(stdout);
    if(closed)return;focus_control=0;
    if(operation==HASH_PACKAGE){
        if(code||output_size!=65||output[64]!='\n'){fail("Could not check the package. See details below.");return;}
        for(unsigned i=0;i<64;i++)if(local_hex(output[i])<0){fail("Invalid package checksum result.");return;}
        memcpy(digest,output,64);digest[64]=0;snprintf(bundle,sizeof(bundle),"/home/nexus/Applications/%s",digest);
        output_size=0;output[0]=0;apps_fd=applications();if(apps_fd<0){fail("Could not open your Applications folder.");return;}
        if(load_installed())return;
        if(errno!=ENOENT){fail("An existing application could not be verified. Its files were left unchanged.");return;}
        phase=READY;strcpy(message,"Ready to install.");puts("[64] INSTALLER PACKAGE READY");fflush(stdout);
    }else if(operation==INSTALL_PACKAGE){
        if(code){fail("Installation failed. The package was not opened.");return;}
        if(!load_installed()){fail("Installed files could not be opened.");return;}
        output_size=0;output[0]=0;
    }else{phase=FINISHED;snprintf(message,sizeof(message),code?"The application exited with code %d. See its output below.":"Application finished successfully.",code);}
    redraw=true;
}
static void job_events(void){
    if(!working)return;
    if(worker_fd>=0){char data[4096];for(;;){ssize_t n=read(worker_fd,data,sizeof(data));if(n>0){append_output(data,n);continue;}if(n<0&&errno==EINTR)continue;
        if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK))break;
        if(n<0){const char error[]="\nCould not read all worker output.\n";append_output(error,sizeof(error)-1);}close(worker_fd);worker_fd=-1;break;}}
    if(!worker_done){pid_t result=waitpid(worker,&worker_status,WNOHANG);if(result==worker)worker_done=true;else if(result<0&&errno!=EINTR){worker_status=125<<8;worker_done=true;}}
    if(worker_done&&worker_fd<0)finish_job();
}
static void rectangle(int x,int y,int w,int h,uint32_t color){
    int right=x+w,bottom=y+h;if(x<0)x=0;if(y<0)y=0;if(right>width)right=width;if(bottom>height)bottom=height;
    for(int row=y;row<bottom;row++)for(int col=x;col<right;col++)pixels[(size_t)row*width+col]=color;
}
static void text(int x,int y,const char* value,const uifont_t* font,int scale,uint32_t color,int right){
    int pen=x*4;
    for(const unsigned char* p=(const unsigned char*)value;*p;p++){
        unsigned c=*p;if(c<font->first||c>=font->first+font->count)c='?';const uifont_glyph_t* g=&font->g[c-font->first];
        int gx=((pen+2)>>2)+g->xoff*scale,gy=y+(font->ascent+g->yoff)*scale;if(gx+(int)g->w*scale>right)break;
        for(unsigned row=0;row<g->h;row++)for(unsigned col=0;col<g->w;col++){
            uint8_t packed=font->bits[g->off+row*((g->w+1)/2)+col/2];unsigned alpha=((col&1)?packed>>4:packed&15)*17;
            for(int dy=0;dy<scale;dy++)for(int dx=0;dx<scale;dx++){int px=gx+col*scale+dx,py=gy+row*scale+dy;if(px<0||px>=width||py<0||py>=height)continue;
                uint32_t background=pixels[(size_t)py*width+px];unsigned r=(((color>>16)&255)*alpha+((background>>16)&255)*(255-alpha)+127)/255;
                unsigned green=(((color>>8)&255)*alpha+((background>>8)&255)*(255-alpha)+127)/255,b=((color&255)*alpha+(background&255)*(255-alpha)+127)/255;
                pixels[(size_t)py*width+px]=(r<<16)|(green<<8)|b;}
        }
        pen+=g->adv_q4*scale;
    }
}
static int card_x,card_y,card_w,card_h,list_y,button_y,visible_rows;
static bool primary_enabled(void){return phase==READY||((phase==INSTALLED||phase==FINISHED)&&program_count);}
static void layout(void){card_w=width-80;if(card_w>1000)card_w=1000;card_h=height-64;if(card_h>820)card_h=820;card_x=(width-card_w)/2;card_y=(height-card_h)/2;list_y=card_y+210;button_y=card_y+card_h-72;visible_rows=(button_y-list_y-190)/34;if(visible_rows<1)visible_rows=1;}
static void release_buffer(void* data,struct wl_buffer* b){(void)data;(void)b;busy=false;}
static const struct wl_buffer_listener buffer_listener={release_buffer};
static bool make_buffer(void){
    if(buffer&&buffer_width==width&&buffer_height==height)return true;
    if(buffer){wl_buffer_destroy(buffer);munmap(pixels,buffer_size);buffer=NULL;pixels=NULL;buffer_size=0;}
    if(width<400||height<400||(uint64_t)width*height>INT32_MAX/4)return false;
    buffer_size=(size_t)width*height*4;int fd=memfd_create("nexus-installer-window",MFD_CLOEXEC);if(fd<0||ftruncate(fd,buffer_size)){if(fd>=0)close(fd);return false;}
    pixels=mmap(NULL,buffer_size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);if(pixels==MAP_FAILED){close(fd);return false;}
    struct wl_shm_pool* pool=wl_shm_create_pool(shm,fd,buffer_size);if(!pool){close(fd);munmap(pixels,buffer_size);pixels=NULL;return false;}
    buffer=wl_shm_pool_create_buffer(pool,0,width,height,width*4,WL_SHM_FORMAT_XRGB8888);wl_shm_pool_destroy(pool);close(fd);if(!buffer){munmap(pixels,buffer_size);pixels=NULL;return false;}
    wl_buffer_add_listener(buffer,&buffer_listener,NULL);buffer_width=width;buffer_height=height;return true;
}
static void paint(void){
    if(!configured||closed||busy||!redraw)return;if(!make_buffer()){fail("Could not draw the installer window.");closed=true;return;}layout();
    rectangle(0,0,width,height,0x101827);rectangle(card_x+5,card_y+7,card_w,card_h,0x070d16);rectangle(card_x,card_y,card_w,card_h,0xf5f7fb);
    rectangle(card_x,card_y,card_w,5,0x3978ed);text(card_x+32,card_y+24,"NexusOS  /  Applications",&uf_ui,1,0x527091,card_x+card_w-32);
    text(card_x+32,card_y+54,"Install application",&uf_title,2,0x17273d,card_x+card_w-32);
    const char* name=strrchr(package,'/');text(card_x+32,card_y+122,name?name+1:package,&uf_uib,2,0x17273d,card_x+card_w-32);
    text(card_x+32,card_y+162,"Local package. Publisher identity is not verified.",&uf_ui,1,0x596b80,card_x+card_w-32);
    text(card_x+32,card_y+184,message,&uf_ui,1,phase==FAILED?0xb32632:0x356b55,card_x+card_w-32);
    if(phase==READY){text(card_x+32,list_y+36,"Installs application files into your Applications folder.",&uf_ui,1,0x596b80,card_x+card_w-32);
        text(card_x+32,list_y+62,"System-wide setup and dependency installation are not available yet.",&uf_ui,1,0x596b80,card_x+card_w-32);}
    if(program_count){
        text(card_x+32,list_y,"Programs",&uf_uib,1,0x263b55,card_x+card_w-32);
        if(focus_control==2&&!working)rectangle(card_x+26,list_y+24,card_w-52,visible_rows*34+4,0x3978ed);
        for(int row=0;row<visible_rows&&scroll+(size_t)row<program_count;row++){size_t index=scroll+row;int y=list_y+26+row*34;
            rectangle(card_x+28,y,card_w-56,32,index==selected?0xdce8fd:0xffffff);
            text(card_x+40,y+5,programs[index],&uf_ui,1,0x1d3656,card_x+card_w-40);
        }
    }
    int output_y=button_y-150;
    if(phase==RUNNING||phase==FINISHED||phase==FAILED){
        text(card_x+32,output_y-22,output_truncated?"Recent output (older lines omitted)":phase==FAILED?"Details":"Program output",&uf_ui,1,0x596b80,card_x+card_w-32);
        rectangle(card_x+28,output_y,card_w-56,128,0xe9edf4);
        const char* start=output+output_size;unsigned lines=0;while(start>output){start--;if(*start=='\n'&&start!=output+output_size-1&&++lines==6){start++;break;}}
        for(int line=0;*start&&line<6;line++){char row[256];size_t n=0;while(*start&&*start!='\n'){if(n<sizeof(row)-1)row[n++]=*start;start++;}if(*start=='\n')start++;row[n]=0;text(card_x+40,output_y+7+line*19,row,&uf_small,1,0x344960,card_x+card_w-40);}
    }else if(working){
        rectangle(card_x+32,list_y+36,card_w-64,8,0xdbe4f2);int range=card_w-192;
        rectangle(card_x+32+(milliseconds()/8)%range,list_y+36,128,8,0x3978ed);
        text(card_x+32,list_y+65,"You can close this window; the current operation will finish in the background.",&uf_ui,1,0x596b80,card_x+card_w-32);
    }
    bool enabled=primary_enabled();int primary_x=card_x+card_w-216;
    if(focus_control==0&&enabled)rectangle(primary_x-3,button_y-3,187,50,0x17273d);
    rectangle(primary_x,button_y,181,44,enabled?0x2866d5:0xa4b4cb);
    text(primary_x+22,button_y+6,phase==CHECKING||phase==READY?"Install":phase==INSTALLING?"Installing...":"Run selected",&uf_uib,1,0xffffff,primary_x+173);
    if(focus_control==1)rectangle(card_x+29,button_y-3,146,50,0x17273d);
    rectangle(card_x+32,button_y,140,44,0xe1e7f0);text(card_x+53,button_y+6,"Close",&uf_uib,1,0x213b59,card_x+164);
    wl_surface_attach(surface,buffer,0,0);wl_surface_damage_buffer(surface,0,0,width,height);wl_surface_commit(surface);busy=true;redraw=false;
    printf("[64] INSTALLER UI phase=%d primary=%d,%d close=%d,%d selected=%zu\n",phase,primary_x+90,button_y+22,card_x+102,button_y+22,selected);fflush(stdout);
}
static void activate(void){if(closed||!primary_enabled())return;if(phase==READY)start_job(INSTALL_PACKAGE);else start_job(RUN_PROGRAM);}
static void configure_surface(void* d,struct xdg_surface* s,uint32_t serial){(void)d;xdg_surface_ack_configure(s,serial);configured=true;redraw=true;}
static const struct xdg_surface_listener xdg_listener={configure_surface};
static void configure_top(void* d,struct xdg_toplevel* t,int32_t w,int32_t h,struct wl_array* states){(void)d;(void)t;(void)states;if(w>0)width=w;if(h>0)height=h;redraw=true;}
static void close_top(void* d,struct xdg_toplevel* t){(void)d;(void)t;closed=true;}
static const struct xdg_toplevel_listener top_listener={.configure=configure_top,.close=close_top};
static void ping(void* d,struct xdg_wm_base* base,uint32_t serial){(void)d;xdg_wm_base_pong(base,serial);}
static const struct xdg_wm_base_listener shell_listener={ping};
static void select_program(int64_t index){if(!program_count||working)return;if(index<0)index=0;if((uint64_t)index>=program_count)index=program_count-1;selected=index;if(selected<scroll)scroll=selected;if(selected>=scroll+(size_t)visible_rows)scroll=selected-visible_rows+1;redraw=true;}
static void keymap(void* d,struct wl_keyboard* k,uint32_t format,int fd,uint32_t size){(void)d;(void)k;(void)format;(void)size;close(fd);}
static void key_enter(void* d,struct wl_keyboard* k,uint32_t serial,struct wl_surface* s,struct wl_array* keys){(void)d;(void)k;(void)serial;(void)s;shift=false;uint32_t* key;wl_array_for_each(key,keys)if(*key==42||*key==54)shift=true;}
static void key_leave(void* d,struct wl_keyboard* k,uint32_t serial,struct wl_surface* s){(void)d;(void)k;(void)serial;(void)s;shift=false;}
static void key_event(void* d,struct wl_keyboard* k,uint32_t serial,uint32_t time,uint32_t code,uint32_t state){
    (void)d;(void)k;(void)serial;(void)time;if(code==42||code==54){shift=state!=0;return;}if(!state||closed)return;
    if(code==1){closed=true;return;}if(code==15){if(!primary_enabled())focus_control=1;else{int count=program_count?3:2;focus_control=(focus_control+(shift?count-1:1))%count;}redraw=true;return;}
    if(code==28||code==96||code==57){if(focus_control==1)closed=true;else activate();return;}
    if(code==103)select_program((int64_t)selected-1);if(code==108)select_program((int64_t)selected+1);
    if(code==104)select_program((int64_t)selected-visible_rows);if(code==109)select_program((int64_t)selected+visible_rows);
    if(code==102)select_program(0);if(code==107)select_program((int64_t)program_count-1);
}
static void modifiers(void* d,struct wl_keyboard* k,uint32_t serial,uint32_t a,uint32_t b,uint32_t c,uint32_t e){(void)d;(void)k;(void)serial;(void)a;(void)b;(void)c;(void)e;}
static void repeat(void* d,struct wl_keyboard* k,int32_t rate,int32_t delay){(void)d;(void)k;(void)rate;(void)delay;}
static const struct wl_keyboard_listener keyboard_listener={keymap,key_enter,key_leave,key_event,modifiers,repeat};
static void pointer_enter(void* d,struct wl_pointer* p,uint32_t serial,struct wl_surface* s,wl_fixed_t x,wl_fixed_t y){(void)d;(void)p;(void)serial;(void)s;mouse_x=wl_fixed_to_int(x);mouse_y=wl_fixed_to_int(y);}
static void pointer_leave(void* d,struct wl_pointer* p,uint32_t serial,struct wl_surface* s){(void)d;(void)p;(void)serial;(void)s;}
static void motion(void* d,struct wl_pointer* p,uint32_t time,wl_fixed_t x,wl_fixed_t y){(void)d;(void)p;(void)time;mouse_x=wl_fixed_to_int(x);mouse_y=wl_fixed_to_int(y);}
static void button(void* d,struct wl_pointer* p,uint32_t serial,uint32_t time,uint32_t code,uint32_t state){
    (void)d;(void)p;(void)serial;(void)time;if(!state||code!=272||closed)return;
    if(mouse_y>=button_y&&mouse_y<button_y+44){if(mouse_x>=card_x+32&&mouse_x<card_x+172){closed=true;return;}
        if(mouse_x>=card_x+card_w-216&&mouse_x<card_x+card_w-35){focus_control=0;activate();return;}}
    if(mouse_x>=card_x+28&&mouse_x<card_x+card_w-28&&mouse_y>=list_y+26&&mouse_y<list_y+26+visible_rows*34){focus_control=2;select_program(scroll+(mouse_y-list_y-26)/34);}
}
static void axis(void* d,struct wl_pointer* p,uint32_t time,uint32_t which,wl_fixed_t value){(void)d;(void)p;(void)time;if(!which)select_program((int64_t)selected+(value>0?1:-1));}
static void frame(void* d,struct wl_pointer* p){(void)d;(void)p;}
static const struct wl_pointer_listener pointer_listener={.enter=pointer_enter,.leave=pointer_leave,.motion=motion,.button=button,.axis=axis,.frame=frame};
static void capabilities(void* d,struct wl_seat* seat,uint32_t caps){(void)d;if(caps&1){struct wl_pointer* p=wl_seat_get_pointer(seat);wl_pointer_add_listener(p,&pointer_listener,NULL);}if(caps&2){struct wl_keyboard* k=wl_seat_get_keyboard(seat);wl_keyboard_add_listener(k,&keyboard_listener,NULL);}}
static void seat_name(void* d,struct wl_seat* s,const char* name){(void)d;(void)s;(void)name;}
static const struct wl_seat_listener seat_listener={capabilities,seat_name};
static void global(void* d,struct wl_registry* registry,uint32_t id,const char* name,uint32_t version){
    (void)d;(void)version;if(!strcmp(name,"wl_compositor"))compositor=wl_registry_bind(registry,id,&wl_compositor_interface,4);
    if(!strcmp(name,"wl_shm"))shm=wl_registry_bind(registry,id,&wl_shm_interface,1);
    if(!strcmp(name,"xdg_wm_base")){shell=wl_registry_bind(registry,id,&xdg_wm_base_interface,1);xdg_wm_base_add_listener(shell,&shell_listener,NULL);}
    if(!strcmp(name,"wl_seat")){struct wl_seat* seat=wl_registry_bind(registry,id,&wl_seat_interface,5);wl_seat_add_listener(seat,&seat_listener,NULL);}
}
static void global_remove(void* d,struct wl_registry* r,uint32_t id){(void)d;(void)r;(void)id;}
static const struct wl_registry_listener registry_listener={global,global_remove};
/* Windows programs and installers (a browser download opened from Chrome) run with Wine when it is installed; .exe/.com
 * need the MZ header (a text file named after a domain, such as example.com, is no program). */
static bool windows_file(const char* path){
    static const char* const kinds[]={".exe",".msi",".lnk",".bat",".cmd",".com"};size_t length=strlen(path);
    for(unsigned i=0;i<sizeof(kinds)/sizeof(*kinds);i++){size_t k=strlen(kinds[i]);if(length<=k||strcasecmp(path+length-k,kinds[i]))continue;
        if(i&&i+1<sizeof(kinds)/sizeof(*kinds))return true;
        char magic[2];int fd=open(path,O_RDONLY|O_CLOEXEC|O_NONBLOCK|O_NOCTTY);if(fd<0)return false;
        bool program=read(fd,magic,2)==2&&magic[0]=='M'&&magic[1]=='Z';close(fd);return program;}
    return false;
}
/* URLs (Wine's winebrowser for ShellExecute of links, Chrome for external schemes): http/https open in the installed
 * browser, detached: the first launch file whose program is executable (KEY=VALUE lines, the program, its options and
 * their values; the start page is replaced by the URL, and --no-remote is left out so a running Firefox takes it).
 * Other schemes have no handler here. Status as xdg-open's: 0 opened, 3 no handler, 4 failed. */
static bool url_scheme(const char* text){
    size_t n=0;while((text[n]>='a'&&text[n]<='z')||(text[n]>='A'&&text[n]<='Z')||(n&&((text[n]>='0'&&text[n]<='9')||text[n]=='+'||text[n]=='-'||text[n]=='.')))n++;
    return n>1&&text[n]==':'&&strncasecmp(text,"file:",5);
}
static int open_url(const char* url){
    if(strncasecmp(url,"http://",7)&&strncasecmp(url,"https://",8)){fprintf(stderr,"xdg-open: no application handles %s\n",url);return 3;}
    static const char* const files[]={"/nexus/chrome-desktop-gpu.args","/nexus/chrome-desktop.args","/nexus/apps/firefox.args"};
    for(unsigned i=0;i<sizeof(files)/sizeof(*files);i++){
        if(!i&&access("/dev/dri/renderD128",R_OK|W_OK))continue;
        FILE* file=fopen(files[i],"re");if(!file)continue;
        static char lines[64][4096];char* args[66];int count=0,program=-1;
        for(int n=0;n<64&&fgets(lines[n],sizeof(lines[n]),file);n++){char* line=lines[n];line[strcspn(line,"\r\n")]=0;
            if(!*line||(!count&&!strcmp(line,"--trace")))continue;
            if(program<0&&line[0]!='/'&&!strchr(line,'='))break;
            if(program<0&&line[0]=='/')program=count;
            else if(program>=0&&(url_scheme(line)||!strncasecmp(line,"file:",5)||!strcmp(line,"--no-remote")))continue;
            args[count++]=line;}
        fclose(file);if(program<0||access(args[program],X_OK))continue;
        args[count++]=(char*)url;args[count]=NULL;
        printf("[64] NEXUS OPEN URL browser=%s url=%s\n",args[program],url);fflush(stdout);
        pid_t pid=fork();if(pid<0){perror("xdg-open");return 4;}
        if(!pid){setsid();for(int k=0;k<program;k++)putenv(args[k]);execv(args[program],args+program);perror("xdg-open: start browser");_exit(127);}
        return 0;
    }
    fprintf(stderr,"xdg-open: no web browser is installed for %s\n",url);return 3;
}
int main(int argc,char** argv){
    if(argc!=2){fprintf(stderr,"Usage: xdg-open LOCAL-PACKAGE.deb | LOCAL-FILE | http(s)://URL\n");return 2;}
    if(url_scheme(argv[1]))return open_url(argv[1]);
    char cwd[PATH_MAX];bool valid=getcwd(cwd,sizeof(cwd))&&local_file(argv[1],cwd,package,sizeof(package));
    if(!valid){strcpy(package,"Local file");fail("Only local package paths or local file:// URLs are accepted.");}
    if(valid&&!access("/usr/bin/wine",X_OK)&&windows_file(package)){printf("[64] NEXUS OPEN WINDOWS path=%s\n",package);fflush(stdout);
        execl("/usr/bin/wine","wine","start","/unix",package,(char*)NULL);perror("Start Windows program with Wine");return 127;}
    display=wl_display_connect(NULL);if(!display){perror("Connect to Nexus desktop");return 3;}
    struct wl_registry* registry=wl_display_get_registry(display);wl_registry_add_listener(registry,&registry_listener,NULL);
    if(wl_display_roundtrip(display)<0||!compositor||!shm||!shell){fprintf(stderr,"Desktop interfaces unavailable\n");return 3;}
    surface=wl_compositor_create_surface(compositor);xdg=xdg_wm_base_get_xdg_surface(shell,surface);xdg_surface_add_listener(xdg,&xdg_listener,NULL);
    top=xdg_surface_get_toplevel(xdg);xdg_toplevel_add_listener(top,&top_listener,NULL);xdg_toplevel_set_title(top,"NexusOS Application Installer");wl_surface_commit(surface);
    printf("[64] NEXUS OPEN PACKAGE path=%s\n",package);fflush(stdout);if(valid)start_job(HASH_PACKAGE);
    bool disconnected=false;uint64_t animation=0;
    while(!closed||working){
        if(!closed){if(wl_display_dispatch_pending(display)<0){closed=true;}paint();if(wl_display_flush(display)<0&&errno!=EAGAIN)closed=true;}
        if(closed&&!disconnected){xdg_toplevel_destroy(top);xdg_surface_destroy(xdg);wl_surface_destroy(surface);wl_display_flush(display);wl_display_disconnect(display);disconnected=true;}
        struct pollfd fds[2];nfds_t count=0;if(!closed)fds[count++]=(struct pollfd){wl_display_get_fd(display),POLLIN,0};if(worker_fd>=0)fds[count++]=(struct pollfd){worker_fd,POLLIN,0};
        int ready=poll(fds,count,50);if(ready<0&&errno!=EINTR){closed=true;}
        if(!closed&&fds[0].revents&&(wl_display_dispatch(display)<0))closed=true;
        job_events();
        if(working&&(phase==CHECKING||phase==INSTALLING)&&milliseconds()-animation>=250){animation=milliseconds();redraw=true;}
    }
    if(!disconnected)wl_display_disconnect(display);if(buffer_size&&pixels&&pixels!=MAP_FAILED)munmap(pixels,buffer_size);
    for(size_t i=0;i<program_count;i++)free(programs[i]);free(programs);if(bundle_fd>=0)close(bundle_fd);if(apps_fd>=0)close(apps_fd);
    puts("[64] INSTALLER UI CLOSED");return exit_status;
}
