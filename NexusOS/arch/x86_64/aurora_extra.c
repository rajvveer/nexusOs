/* Native filesystem and process backends for the Aurora desktop tools. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "aurora_extra.h"
#include "aurora_places.h"
#include "../../kernel/window.h"
#include "../../kernel/appui.h"
#include "../../kernel/macui.h"

enum tool {NONE,NOTES,FILES,TERMINAL,HELP};
static enum tool tool;
static char message[256],path[PATH_MAX];
static int scroll,view_rows=1,view_cols=1;
static bool dirty,load_failed,discard;
static char *text;
static size_t length,capacity,cursor;
static struct stat loaded;
static bool existed;
static char **entries;
static size_t entry_count,selected;
static size_t last_clicked=SIZE_MAX;
static uint64_t last_click_time;
static pid_t worker=-1;
static int worker_input=-1,worker_output=-1;
static char output[65536],command[4096];
static size_t output_len,command_len;
extern void aurora_child_disconnect(void);

static void error(const char *operation){snprintf(message,sizeof(message),"%s: %s",operation,strerror(errno));}
static bool reserve(size_t size){
    if(size<=capacity)return true;if(size>SIZE_MAX/2){errno=ENOMEM;return false;}
    size_t next=capacity?capacity:4096;while(next<size){if(next>SIZE_MAX/2){errno=ENOMEM;return false;}next*=2;}
    char *p=realloc(text,next);if(!p)return false;text=p;capacity=next;return true;
}
static void notes_load(void){
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0){if(errno!=ENOENT){error("Open notes");load_failed=true;}else strcpy(message,"New note. Ctrl+S saves to disk.");return;}
    existed=true;
    if(fstat(fd,&loaded)||!S_ISREG(loaded.st_mode)){close(fd);strcpy(message,"Choose a regular text file.");load_failed=true;return;}
    for(;;){if(length>SIZE_MAX-4097||!reserve(length+4097)){error("Read notes");load_failed=true;break;}
        ssize_t n=read(fd,text+length,4096);if(n<0&&errno==EINTR)continue;if(n<0){error("Read notes");load_failed=true;break;}if(!n)break;
        if(memchr(text+length,0,n)){strcpy(message,"This file contains binary data and cannot be edited in Notes.");load_failed=true;break;}length+=(size_t)n;
    }
    if(close(fd)&&!load_failed){error("Read notes");load_failed=true;}
    if(text)text[length]=0;
    if(!load_failed)snprintf(message,sizeof(message),"%zu bytes loaded. Ctrl+S saves.",length);
}
static bool unchanged(void){
    struct stat st;if(lstat(path,&st)){if(errno==ENOENT&&!existed)return true;return false;}
    if(!existed||!S_ISREG(st.st_mode)||st.st_ino!=loaded.st_ino||st.st_size!=loaded.st_size||
       st.st_mtim.tv_sec!=loaded.st_mtim.tv_sec||st.st_mtim.tv_nsec!=loaded.st_mtim.tv_nsec){errno=EBUSY;return false;}return true;
}
static bool notes_save(void){
    if(load_failed)return false;if(!unchanged()){strcpy(message,"File changed outside Notes. Reopen it before saving.");return false;}
    char temporary[PATH_MAX];if(snprintf(temporary,sizeof(temporary),"%s.aurora-XXXXXX",path)>=(int)sizeof(temporary)){errno=ENAMETOOLONG;error("Save notes");return false;}
    int fd=mkstemp(temporary);if(fd<0){error("Save notes");return false;}bool good=true;size_t done=0;
    if(existed){struct stat temporary_stat;
        if(fstat(fd,&temporary_stat))good=false;
        /* A filesystem with fixed permissions needs no chmod when they already match. */
        else if((temporary_stat.st_mode&0777)!=(loaded.st_mode&0777)&&fchmod(fd,loaded.st_mode&0777))good=false;
    }
    while(good&&done<length){ssize_t n=write(fd,text+done,length-done);if(n<0&&errno==EINTR)continue;if(n<=0){if(!n)errno=EIO;good=false;break;}done+=(size_t)n;}
    if(good&&fsync(fd))good=false;int saved_errno=errno;if(close(fd)&&good){good=false;saved_errno=errno;}
    if(good&&!unchanged()){good=false;saved_errno=errno;}
    if(good&&rename(temporary,path)){good=false;saved_errno=errno;}
    if(!good){unlink(temporary);errno=saved_errno;error("Save notes");return false;}
    if(lstat(path,&loaded)){error("Verify saved notes");load_failed=true;return false;}existed=true;dirty=false;
    char directory[PATH_MAX];strcpy(directory,path);char *slash=strrchr(directory,'/');if(slash==directory)slash[1]=0;else if(slash)*slash=0;else strcpy(directory,".");
    int dir=open(directory,O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(dir<0||fsync(dir)){error("Saved; directory sync failed");dirty=true;if(dir>=0)close(dir);return false;}close(dir);
    snprintf(message,sizeof(message),"Saved %zu bytes.",length);printf("[64] AURORA NOTES SAVED bytes=%zu path=%s\n",length,path);fflush(stdout);return true;
}
static size_t line_start(size_t at){while(at&&text[at-1]!='\n')at--;return at;}
static size_t line_end(size_t at){while(at<length&&text[at]!='\n')at++;return at;}
static void notes_key(int id,char key){
    (void)id;if(key==19){notes_save();return;}
    if(key==4){discard=true;window_destroy(0);return;}
    if(load_failed)return;unsigned char k=(unsigned char)key;
    if(k==0x82){if(cursor)cursor--;return;}if(k==0x83){if(cursor<length)cursor++;return;}
    if(k==0x84){cursor=line_start(cursor);return;}if(k==0x85){cursor=line_end(cursor);return;}
    if(k==0x80||k==0x81){size_t start=line_start(cursor),col=cursor-start,target;
        if(k==0x80){if(!start)return;target=line_start(start-1);}else{target=line_end(cursor);if(target==length)return;target++;}
        size_t end=line_end(target);cursor=target+(col<end-target?col:end-target);return;}
    if(key=='\b'){if(cursor){memmove(text+cursor-1,text+cursor,length-cursor+1);cursor--;length--;dirty=true;}}
    else if(k==127){if(cursor<length){memmove(text+cursor,text+cursor+1,length-cursor);length--;dirty=true;}}
    else if(key=='\n'||key=='\t'||(key>=32&&key<127)){
        if(!reserve(length+2)){error("Edit notes");return;}memmove(text+cursor+1,text+cursor,length-cursor+1);text[cursor++]=key;length++;dirty=true;
    }
}
static void notes_draw(int id,int a,int b,int c,int d){
    (void)a;(void)b;(void)c;(void)d;int x,y,w,h;window_content_px(id,&x,&y,&w,&h);appui_px_fill(x,y,w,h,APPUI_BG);
    appui_px_text(x+16,y+10,dirty?"Notes *":"Notes",APPUI_F_BOLD,APPUI_TEXT);
    appui_px_text(x+16,y+36,path,APPUI_F_SMALL,APPUI_SUBTLE);appui_px_sep(x,y+60,w);
    view_rows=(h-118)/18;if(view_rows<1)view_rows=1;view_cols=(w-32)/8;if(view_cols<1)view_cols=1;
    size_t cursor_line=0,cursor_col=0;for(size_t i=0;i<cursor;i++){if(text[i]=='\n'){cursor_line++;cursor_col=0;}else cursor_col++;}
    if(cursor_line<(size_t)scroll)scroll=(int)cursor_line;if(cursor_line>=(size_t)(scroll+view_rows))scroll=(int)cursor_line-view_rows+1;
    size_t column_offset=cursor_col>=(size_t)view_cols?cursor_col-view_cols+1:0;
    size_t row=0,col=0;for(size_t i=0;i<length&&row<(size_t)(scroll+view_rows);i++){
        if(col==0&&row>=(size_t)scroll){   /* a line with non-ASCII text (UTF-8): drawn whole by the text engine (any script) */
            size_t end=line_end(i);bool wide=false;for(size_t k=i;k<end;k++)if((unsigned char)text[k]>=0x80)wide=true;
            if(wide){ui_text_n(x+16,y+70+((int)row-scroll)*18,text+i,(int)(end-i),UF_UI,APPUI_TEXT);i=end;row++;col=0;if(i>=length)break;continue;}
        }
        if(text[i]=='\n'){row++;col=0;continue;}
        if(row>=(size_t)scroll&&col>=column_offset&&col-column_offset<(size_t)view_cols){
            ui_cell_glyph(x+16+(int)(col-column_offset)*8,y+70+((int)row-scroll)*18,(unsigned char)text[i],APPUI_TEXT,APPUI_BG);
        }col++;
    }
    if(!load_failed)ui_blend_rect(x+16+(int)(cursor_col-column_offset)*8,y+70+((int)cursor_line-scroll)*18,2,16,APPUI_ACCENT,255);
    appui_px_sep(x,y+h-40,w);appui_px_text(x+12,y+h-36,message,APPUI_F_SMALL,load_failed?APPUI_ORANGE:APPUI_SUBTLE);
    appui_px_text(x+12,y+h-18,"Ctrl+S Save    Ctrl+D Discard and close    Home/End Move",APPUI_F_SMALL,APPUI_FAINT);
}
static void free_entries(void){for(size_t i=0;i<entry_count;i++)free(entries[i]);free(entries);entries=NULL;entry_count=0;}
static int compare_entries(const void *a,const void *b){return strcmp(*(char *const *)a,*(char *const *)b);}
static void files_refresh(void){
    DIR *dir=opendir(path);if(!dir){error("Open folder");return;}char **next=NULL;size_t count=0;int failed=0;
    for(;;){errno=0;struct dirent *entry=readdir(dir);if(!entry){failed=errno;break;}if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
        if(count>=SIZE_MAX/sizeof(*next)-1){failed=ENOMEM;break;}char **more=realloc(next,(count+1)*sizeof(*next));if(!more){failed=errno;break;}next=more;
        next[count]=strdup(entry->d_name);if(!next[count]){failed=errno;break;}count++;
    }
    if(closedir(dir)&&!failed)failed=errno;
    if(failed){for(size_t i=0;i<count;i++)free(next[i]);free(next);errno=failed;error("Read folder");return;}
    free_entries();entries=next;entry_count=count;qsort(entries,count,sizeof(*entries),compare_entries);selected=0;scroll=0;last_clicked=SIZE_MAX;
    snprintf(message,sizeof(message),"%zu items. Select a file and Open, or double-click.",count);
}
static bool child_open(const char *program,const char *argument,const char *extra){
    pid_t pid=fork();if(pid<0){error("Open application");return false;}
    if(!pid){aurora_child_disconnect();if(extra)execl(program,program,argument,extra,(char *)NULL);else execl(program,program,argument,(char *)NULL);perror("Open application");_exit(127);}return true;
}
/* Windows programs and installers open with Wine (wine_install.py); wine start /unix picks exe, msiexec, cmd or the shortcut target. */
static bool windows_file(const char *name){static const char *const kinds[]={".exe",".msi",".lnk",".bat",".cmd",".com"};size_t n=strlen(name);
    for(unsigned i=0;i<sizeof(kinds)/sizeof(*kinds);i++){size_t k=strlen(kinds[i]);if(n>k&&!strcasecmp(name+n-k,kinds[i]))return true;}return false;}
/* Opening: .exe/.com also need the MZ header, so a text file named after a domain (example.com) still opens in Notes. */
static bool windows_program(const char *full){
    if(!windows_file(full))return false;size_t n=strlen(full);if(strcasecmp(full+n-4,".exe")&&strcasecmp(full+n-4,".com"))return true;
    char magic[2];int fd=open(full,O_RDONLY|O_CLOEXEC|O_NONBLOCK|O_NOCTTY);if(fd<0)return false;
    bool program=read(fd,magic,2)==2&&magic[0]=='M'&&magic[1]=='Z';close(fd);return program;
}
static void wine_open(const char *full){
    if(access("/usr/bin/wine",X_OK)){strcpy(message,"Windows programs need Wine: install it with wine_install.py.");return;}
    pid_t pid=fork();if(pid<0){error("Start Windows program");return;}
    if(!pid){aurora_child_disconnect();if(chdir(path)){}setsid();execl("/usr/bin/wine","wine","start","/unix",full,(char *)NULL);perror("Start Windows program");_exit(127);}
    printf("[64] AURORA FILES WINE pid=%ld path=%s\n",(long)pid,full);fflush(stdout);strcpy(message,"Starting the Windows program with Wine...");
}
static void files_open(void){
    if(selected>=entry_count)return;char full[PATH_MAX];if(snprintf(full,sizeof(full),"%s/%s",path,entries[selected])>=(int)sizeof(full)){errno=ENAMETOOLONG;error("Open file");return;}
    struct stat st;if(stat(full,&st)){error("Open file");return;}
    if(S_ISDIR(st.st_mode)){char real[PATH_MAX];if(!realpath(full,real)){error("Open folder");return;}strcpy(path,real);files_refresh();return;}
    if(!S_ISREG(st.st_mode)){strcpy(message,"This item is not a regular file or folder.");return;}
    if(windows_program(full)){wine_open(full);return;}
    size_t n=strlen(full);if(n>=4&&!strcasecmp(full+n-4,".deb")){if(child_open("/bin/xdg-open",full,NULL))strcpy(message,"Opened application installer.");}
    else if(child_open("/bin/aurora-app","notes",full))strcpy(message,"Opened file in Notes.");
}
static void files_parent(void){char *slash=strrchr(path,'/');if(slash&&slash!=path)*slash=0;else strcpy(path,"/");files_refresh();}
static void files_key(int id,char key){(void)id;unsigned char k=(unsigned char)key;if(k==0x80&&selected)selected--;if(k==0x81&&selected+1<entry_count)selected++;if(key=='\n')files_open();if(key=='\b')files_parent();if(key==18)files_refresh();}
static void files_click(int id,int x,int y){(void)id;
    if(x<PLACES_WIDTH){char next[PATH_MAX],note[256];strcpy(next,path);int hit=places_click(x,y,next,sizeof(next),note,sizeof(note));if(hit&1){strcpy(path,next);files_refresh();printf("[64] AURORA FILES FOLDER path=%s items=%zu\n",path,entry_count);fflush(stdout);}if(hit&2)snprintf(message,sizeof(message),"%s",note);return;}
    x-=PLACES_WIDTH; /* the drives sidebar (aurora_places.c) is on the left */
    if(y>=8&&y<38){if(x>=12&&x<92)files_parent();else if(x>=102&&x<192)files_refresh();else if(x>=202&&x<282)files_open();return;}
    if(y>=82&&y<82+view_rows*28){size_t row=(size_t)((y-82)/28+scroll);if(row<entry_count){struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);uint64_t time=(uint64_t)now.tv_sec*1000+now.tv_nsec/1000000;
        selected=row;bool double_click=row==last_clicked&&time-last_click_time<400;last_clicked=row;last_click_time=time;if(double_click){last_clicked=SIZE_MAX;files_open();}
    }}
}
static void files_draw(int id,int a,int b,int c,int d){
    (void)a;(void)b;(void)c;(void)d;int x,y,w,h;window_content_px(id,&x,&y,&w,&h);appui_px_fill(x,y,w,h,APPUI_BG);
    places_draw(x,y,h-28,path);x+=PLACES_WIDTH;w-=PLACES_WIDTH;
    appui_px_button(x+12,y+8,80,30,"Up",false);appui_px_button(x+102,y+8,90,30,"Refresh",false);appui_px_button(x+202,y+8,80,30,"Open",true);
    appui_px_text(x+14,y+48,path,APPUI_F_BODY,APPUI_TEXT);appui_px_sep(x,y+76,w);
    view_rows=(h-118)/28;if(view_rows<1)view_rows=1;if(selected<(size_t)scroll)scroll=(int)selected;if(selected>=(size_t)(scroll+view_rows))scroll=(int)selected-view_rows+1;
    for(int r=0;r<view_rows&&(size_t)(scroll+r)<entry_count;r++){
        size_t n=(size_t)(scroll+r);char full[PATH_MAX];struct stat st;bool directory=snprintf(full,sizeof(full),"%s/%s",path,entries[n])<(int)sizeof(full)&&!stat(full,&st)&&S_ISDIR(st.st_mode);
        appui_px_row(x+8,y+82+r*28,w-16,26,n==selected);ui_icon(directory?ICON_FOLDER:windows_file(entries[n])?ICON_GENERIC:ICON_NOTES,x+14,y+84+r*28,20);
        appui_px_text(x+44,y+84+r*28,entries[n],APPUI_F_BODY,APPUI_TEXT);
    }
    appui_px_sep(x,y+h-28,w);appui_px_text(x+12,y+h-22,message,APPUI_F_SMALL,APPUI_SUBTLE);
}
static void append_output(const char *data,size_t n){
    size_t limit=sizeof(output)-1;if(n>=limit){data+=n-limit;n=limit;output_len=0;}if(n>limit-output_len){size_t remove=n-(limit-output_len);memmove(output,output+remove,output_len-remove);output_len-=remove;}
    memcpy(output+output_len,data,n);output_len+=n;output[output_len]=0;
}
/* Command arguments support quotes and escapes; shell expansion is intentionally absent. */
static int arguments(char *line,char **argv,size_t max){
    char *read=line,*write=line;size_t n=0;
    while(*read){while(*read==' '||*read=='\t')read++;if(!*read)break;if(n+1>=max){errno=E2BIG;return -1;}argv[n++]=write;char quote=0;
        while(*read&&(quote||(*read!=' '&&*read!='\t'))){char ch=*read++;if(ch=='\\'&&quote!='\''){if(!*read){errno=EINVAL;return -1;}*write++=*read++;}
            else if(ch=='\''||ch=='"'){if(quote==ch)quote=0;else if(!quote)quote=ch;else *write++=ch;}else *write++=ch;}
        if(quote){errno=EINVAL;return -1;}if(*read)read++;*write++=0;
    }argv[n]=NULL;return (int)n;
}
static void builtin(int argc,char **argv){
    if(!strcmp(argv[0],"pwd")){char cwd[PATH_MAX];if(!getcwd(cwd,sizeof(cwd))){perror("pwd");exit(1);}puts(cwd);exit(0);}
    if(!strcmp(argv[0],"echo")){for(int i=1;i<argc;i++)printf("%s%s",i>1?" ":"",argv[i]);putchar('\n');exit(0);}
    if(!strcmp(argv[0],"ls")){if(argc>2){fputs("Usage: ls [directory]\n",stderr);exit(2);}DIR *dir=opendir(argc==2?argv[1]:".");if(!dir){perror("ls");exit(1);}int error_code=0;
        for(;;){errno=0;struct dirent *entry=readdir(dir);if(!entry){error_code=errno;break;}if(strcmp(entry->d_name,".")&&strcmp(entry->d_name,".."))puts(entry->d_name);}
        if(closedir(dir)&&!error_code)error_code=errno;if(error_code){errno=error_code;perror("ls");exit(1);}exit(0);}
    if(!strcmp(argv[0],"cat")){if(argc!=2){fputs("Usage: cat FILE\n",stderr);exit(2);}int fd=open(argv[1],O_RDONLY);if(fd<0){perror("cat");exit(1);}char data[4096];
        for(;;){ssize_t n=read(fd,data,sizeof(data));if(n<0&&errno==EINTR)continue;if(n<0){perror("cat");exit(1);}if(!n)break;size_t done=0;while(done<(size_t)n){ssize_t w=write(1,data+done,(size_t)n-done);if(w<0&&errno==EINTR)continue;if(w<=0){perror("cat");exit(1);}done+=(size_t)w;}}close(fd);exit(0);}
    execvp(argv[0],argv);perror(argv[0]);_exit(127);
}
static void terminal_run(void){
    if(worker>0){
        if(worker_input<0){strcpy(message,"The running process has closed its input.");return;}
        command[command_len]='\n';ssize_t sent=write(worker_input,command,command_len+1);command[command_len]=0;
        if(sent!=(ssize_t)command_len+1){if(sent<0)error("Send input");else strcpy(message,"Input was only partly accepted; remaining text was kept.");if(sent>0){memmove(command,command+sent,command_len-(size_t)sent);command_len-=(size_t)sent;}return;}
        append_output(command,command_len);append_output("\n",1);command_len=0;command[0]=0;return;
    }
    if(worker_output>=0){strcpy(message,"Finishing the previous command's output...");return;}
    if(!command_len)return;append_output("$ ",2);append_output(command,command_len);append_output("\n",1);
    char copy[sizeof(command)],*args[128];strcpy(copy,command);int argc=arguments(copy,args,128);
    if(argc<0){error("Parse command");return;}command[0]=0;command_len=0;if(!argc)return;
    if(!strcmp(args[0],"help")){const char *help="pwd, cd [dir], ls [dir], cat FILE, echo TEXT, clear; or an executable path.\nQuotes and escapes work. Pipelines, shell expansion and terminal-control programs are not supported.\nCtrl+C interrupts the child; Ctrl+D closes its input.\n";append_output(help,strlen(help));return;}
    if(!strcmp(args[0],"clear")){output_len=0;output[0]=0;return;}
    if(!strcmp(args[0],"cd")){if(argc>2){strcpy(message,"Usage: cd [directory]");return;}if(chdir(argc==2?args[1]:getenv("HOME")&&getenv("HOME")[0]=='/'?getenv("HOME"):"/home/nexus"))error("cd");else{char cwd[PATH_MAX];if(getcwd(cwd,sizeof(cwd)))snprintf(message,sizeof(message),"Folder: %.220s",cwd);}return;}
    int in[2],out[2];if(pipe2(in,O_CLOEXEC)){error("Create input channel");return;}if(pipe2(out,O_CLOEXEC)){close(in[0]);close(in[1]);error("Create output channel");return;}
    if(fcntl(in[1],F_SETFL,O_NONBLOCK)||fcntl(out[0],F_SETFL,O_NONBLOCK)){int e=errno;close(in[0]);close(in[1]);close(out[0]);close(out[1]);errno=e;error("Configure process channels");return;}
    fflush(NULL);pid_t pid=fork();if(pid<0){int e=errno;close(in[0]);close(in[1]);close(out[0]);close(out[1]);errno=e;error("Start command");return;}
    if(!pid){aurora_child_disconnect();close(in[1]);close(out[0]);if(dup2(in[0],0)!=0||dup2(out[1],1)!=1||dup2(out[1],2)!=2)_exit(125);if(in[0]>2)close(in[0]);if(out[1]>2)close(out[1]);signal(SIGPIPE,SIG_DFL);builtin(argc,args);}
    close(in[0]);close(out[1]);worker=pid;worker_input=in[1];worker_output=out[0];strcpy(message,"Process running. Type a line and press Enter to send input.");
    printf("[64] AURORA COMMAND pid=%ld name=%s\n",(long)pid,args[0]);fflush(stdout);
}
static void terminal_key(int id,char key){(void)id;
    if(key==3){if(worker>0){if(kill(worker,SIGINT))error("Interrupt process");else strcpy(message,"Interrupt sent.");}else{command_len=0;command[0]=0;}return;}
    if(key==4){if(worker_input>=0){close(worker_input);worker_input=-1;strcpy(message,"Process input closed.");}return;}
    if(key=='\n'){terminal_run();return;}if(key=='\b'){if(command_len)command[--command_len]=0;return;}
    if(key>=32&&key<127){if(command_len<sizeof(command)-2){command[command_len++]=key;command[command_len]=0;}else strcpy(message,"Command line is full.");}
}
static void terminal_draw(int id,int a,int b,int c,int d){
    (void)a;(void)b;(void)c;(void)d;int x,y,w,h;window_content_px(id,&x,&y,&w,&h);appui_px_fill(x,y,w,h,0x17191f);
    view_cols=(w-24)/8;if(view_cols<1)view_cols=1;view_rows=(h-90)/16;if(view_rows<1)view_rows=1;
    size_t lines=1,col=0;for(size_t i=0;i<output_len;i++){if(output[i]=='\n'||++col>=(size_t)view_cols){lines++;col=0;}}
    size_t first=lines>(size_t)view_rows?lines-view_rows:0,row=0;col=0;
    for(size_t i=0;i<output_len;i++){
        unsigned char ch=(unsigned char)output[i];if(ch!='\n'&&row>=first&&row-first<(size_t)view_rows)ui_cell_glyph(x+12+(int)col*8,y+10+(int)(row-first)*16,ch>=32?ch:' ',0xd8e5d9,0x17191f);
        if(ch=='\n'||++col>=(size_t)view_cols){row++;col=0;}
    }
    appui_px_sep(x,y+h-72,w);appui_px_text(x+12,y+h-65,worker>0?"stdin >":"$",APPUI_F_BODY,APPUI_GREEN);
    size_t visible=(size_t)(view_cols-8);if(view_cols<9)visible=1;const char *input=command+(command_len>visible?command_len-visible:0);appui_px_text(x+76,y+h-65,input,APPUI_F_BODY,APPUI_TEXT);
    appui_px_text(x+12,y+h-39,message,APPUI_F_SMALL,APPUI_SUBTLE);appui_px_text(x+12,y+h-20,"Command runner: type help. Shows latest 64 KiB. Interactive TTY apps are not supported.",APPUI_F_SMALL,APPUI_FAINT);
}
static void help_draw(int id,int a,int b,int c,int d){
    (void)a;(void)b;(void)c;(void)d;int x,y,w,h;window_content_px(id,&x,&y,&w,&h);appui_px_fill(x,y,w,h,APPUI_BG);
    const char *lines[]={"NexusOS Aurora","Your desktop and Chrome run on the NexusOS custom kernel.","Use the Dock or Launchpad to open apps. Alt+Tab switches windows.","Window buttons: close, minimize, maximize. Drag the title to move.","Chrome downloads: Ctrl+J, open a supported .deb, Install, Run.","Notes: Ctrl+S saves; Ctrl+D discards unsaved edits and closes.","Files: Enter opens a selection; Backspace goes to the parent folder.","Terminal runs commands and captures output; it does not emulate a TTY.","Calculator, Calendar, Paint and Minesweeper use the original Aurora apps.","Application compatibility and browser sandbox protections remain incomplete."};
    for(size_t i=0;i<sizeof(lines)/sizeof(lines[0]);i++)appui_px_text(x+18,y+18+(int)i*34,lines[i],i?APPUI_F_BODY:APPUI_F_TITLE,i?APPUI_SUBTLE:APPUI_TEXT);
}
static const char* home_dir(void){const char* h=getenv("HOME");return h&&h[0]=='/'&&strlen(h)<200?h:"/home/nexus";}   /* the session user's (aurora_session.h) */
int aurora_extra_open(const char *name,const char *requested){
    strcpy(message,"");if(!strcmp(name,"notes")){tool=NOTES;if(!reserve(1))return -1;text[0]=0;
        static char notes_file[256];snprintf(notes_file,sizeof(notes_file),"%s/notes.txt",home_dir());const char *file=requested?requested:notes_file;if(strlen(file)>=sizeof(path)){errno=ENAMETOOLONG;error("Open notes");load_failed=true;}else{strcpy(path,file);notes_load();}
        return window_create("Notes",0,0,88,36,notes_draw,notes_key);
    }
    if(!strcmp(name,"files")){tool=FILES;const char *folder=requested?requested:home_dir();if(!realpath(folder,path)){snprintf(path,sizeof(path),"%s",home_dir());error("Open folder");}files_refresh();int id=window_create("Files",0,0,94,38,files_draw,files_key);window_set_on_click(id,files_click);return id;}
    if(!strcmp(name,"terminal")){tool=TERMINAL;if(chdir(requested?requested:home_dir()))error("Open working folder");else strcpy(message,"Ready. Commands run as native processes.");signal(SIGPIPE,SIG_IGN);return window_create("Terminal",0,0,110,36,terminal_draw,terminal_key);}
    if(!strcmp(name,"help")||!strcmp(name,"about")){tool=HELP;return window_create("About NexusOS",0,0,110,29,help_draw,NULL);}return -1;
}
bool aurora_extra_tick(void){
    bool changed=false;
    if(worker_output>=0){char data[4096];for(int i=0;i<16;i++){ssize_t n=read(worker_output,data,sizeof(data));if(n>0){append_output(data,(size_t)n);changed=true;continue;}if(n<0&&errno==EINTR)continue;if(!n||(n<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK)){close(worker_output);worker_output=-1;}break;}}
    if(worker>0){int status;pid_t done=waitpid(worker,&status,WNOHANG);if(done==worker){int code=WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);snprintf(message,sizeof(message),"Process exited with status %d.",code);printf("[64] AURORA COMMAND DONE status=%d\n",code);fflush(stdout);worker=-1;if(worker_input>=0)close(worker_input);worker_input=-1;changed=true;}}
    else if(tool==FILES){if(places_tick())changed=true;int status;while(waitpid(-1,&status,WNOHANG)>0){if(!WIFEXITED(status)||WEXITSTATUS(status)){snprintf(message,sizeof(message),"Application closed with status %d.",WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status));}changed=true;}}
    return changed;
}
bool aurora_extra_close(void){
    if(tool==NOTES&&dirty&&!discard){strcpy(message,"Unsaved changes: Ctrl+S saves; Ctrl+D discards and closes.");return false;}
    if(tool==TERMINAL&&worker>0){strcpy(message,"Process is still running. Ctrl+C interrupts; Ctrl+D closes its input.");return false;}return true;
}
void aurora_extra_scroll(int direction){if(tool==FILES&&entry_count){long next=(long)selected+direction;if(next<0)next=0;if((size_t)next>=entry_count)next=(long)entry_count-1;selected=(size_t)next;}}
void aurora_extra_cleanup(void){free_entries();free(text);if(worker_input>=0)close(worker_input);if(worker_output>=0)close(worker_output);}
