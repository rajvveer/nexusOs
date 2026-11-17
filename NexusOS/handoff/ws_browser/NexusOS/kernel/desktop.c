/* ============================================================================
 * NexusOS - Desktop Compositor - Aurora (v4.0)
 * ============================================================================
 * macOS-style desktop: wallpaper, windows, the dock, the menu bar, Spotlight
 * and Launchpad, composited into the 32bpp back buffer every content frame.
 * Pointer-only motion takes a cheap save-under path; the clock tick repaints
 * just the menu bar.
 * ============================================================================ */

#include "desktop.h"
#include "gui.h"
#include "theme.h"
#include "window.h"
#include "mouse.h"
#include "keyboard.h"
#include "port.h"
#include "vga.h"
#include "string.h"
#include "framebuffer.h"
#include "rtc.h"
#include "memory.h"
#include "start_menu.h"
#include "calculator.h"
#include "filemgr.h"
#include "sysmon.h"
#include "settings.h"
#include "context_menu.h"
#include "notify.h"
#include "ramfs.h"
#include "vfs.h"
#include "switcher.h"
#include "screensaver.h"
#include "lockscreen.h"
#include "clipboard.h"
#include "palette.h"
#include "notepad.h"
#include "taskmgr.h"
#include "calendar.h"
#include "music.h"
#include "help.h"
#include "widgets.h"
#include "paint.h"
#include "speaker.h"
#include "accessibility.h"
#include "font.h"
#include "workspaces.h"
#include "minesweeper.h"
#include "recycle.h"
#include "sysinfo.h"
#include "todo.h"
#include "clock.h"
#include "pong.h"
#include "power.h"
#include "persist.h"   /* Phase 55 */
#include "search.h"
#include "tetris.h"
#include "hexview.h"
#include "contacts.h"
#include "colorpick.h"
#include "syslog.h"
#include "notifcenter.h"
#include "dock.h"
#include "clipmgr.h"
#include "shortcuts.h"
#include "appearance.h"
#include "fileops.h"
#include "net.h"
#include "ip.h"
#include "icmp.h"
#include "udp.h"
#include "tcp.h"
#include "arp.h"
#include "dns.h"
#include "http.h"
#include "browser.h"
#include "dhcp.h"
#include "ntp.h"
#include "httpd.h"
#include "rshell.h"
#include "vnc.h"
#include "sync.h"
#include "mobile.h"
#include "usb_hid.h"
#include "procfs.h"
#include "termios.h"
#include "posix.h"
#include "env.h"
#include "users.h"
#include "version.h"
#include "macui.h"
#include "menubar.h"
#include "spotlight.h"
#include "kguard.h"      /* Phase 55: crash containment */

static void about_draw(int,int,int,int,int);
static void about_key(int,char);
static void term_draw(int,int,int,int,int);
static void term_key(int,char);

static bool screen_dirty=true;
static uint32_t frame_count=0;

/* ---- Phase 56: dock / menu-bar hover fast path ---------------------------------
 * A full compose() of the 1080p desktop costs ~150-200 ms under TCG, and the dock
 * magnification used to trigger one on every pointer step over the dock (~4-5 fps).
 * compose() now saves the *base layer* (wallpaper + widgets + windows) under the
 * dock band once per full composite; a hover-only change restores that band, redraws
 * just the dock (and/or the menu bar) and presents only those rows. */
#define BAND_MAX_W 1920
#define DOCK_BAND_MAX_H 256
static uint32_t dock_under[BAND_MAX_W*DOCK_BAND_MAX_H];   /* base layer under the dock band */
static int  dock_band_y=0, dock_band_h=0;                   /* rows saved in dock_under */
static bool base_valid=false;                               /* dock_under matches the back buffer */
extern volatile uint32_t system_ticks;
static uint32_t last_tick=0,last_input_tick=0;
#define IDLE_TIMEOUT (18*300)          /* screensaver after ~5 minutes */

#define TBL 96
#define TBC 112
static const char TERM_PROMPT[]="nexus@os:~$ ";
static char tl[TBL][TBC];static int tlc=0;
static char ti[TBC];static int til=0;static int tscr=0;
/* Per-line color role for the terminal (gives output simple highlighting). */
enum { TC_DEF=0, TC_ECHO, TC_ERR, TC_BANNER, TC_HINT };
static uint8_t trole[TBL];

/* Bounded string building for terminal lines (every buffer is TBC bytes). */
static void tset(char*b,const char*s){strncpy(b,s,TBC-1);b[TBC-1]='\0';}
static void tjoin(char*b,const char*s){int n=(int)strlen(b);while(*s&&n<TBC-1)b[n++]=*s++;b[n]='\0';}

/* Add a line with an explicit color role. */
static void taddlc(const char*t,uint8_t role){
    if(tlc<TBL){strncpy(tl[tlc],t,TBC-1);tl[tlc][TBC-1]='\0';trole[tlc]=role;tlc++;}
    else{for(int i=0;i<TBL-1;i++){strcpy(tl[i],tl[i+1]);trole[i]=trole[i+1];}
         strncpy(tl[TBL-1],t,TBC-1);tl[TBL-1][TBC-1]='\0';trole[TBL-1]=role;}
    screen_dirty=true;
}

/* Add a line, auto-classifying its color (command echo dim, errors red). */
static void taddl(const char*t){
    uint8_t role=TC_DEF;
    if(!strncmp(t,TERM_PROMPT,strlen(TERM_PROMPT)))role=TC_ECHO;
    else if(strstr(t,"Unknown")||strstr(t,"Invalid")||strstr(t,"failed")||
            strstr(t,"Failed")||strstr(t,"Timeout")||strstr(t,"denied")||strstr(t,"Bad "))
        role=TC_ERR;
    taddlc(t,role);
}

/* Phase 54: the GUI terminal's current working directory (NULL == root). */
static fs_node_t* tcwd=NULL;
static fs_node_t* tcwd_node(void){return tcwd?tcwd:vfs_get_root();}

/* Read up to max-1 bytes of a /proc path or file; -1 = missing, -2 = denied. */
static int tload(const char*f,char*buf,int max){
    int len;
    if(procfs_is_proc(f)){len=procfs_read(f,buf,max-1);if(len<0)return -1;}
    else{fs_node_t*n=ramfs_resolve(f,tcwd_node());if(!n||n->type==FS_DIRECTORY)return -1;
        len=vfs_read(n,0,n->size<(uint32_t)(max-1)?n->size:(uint32_t)(max-1),(uint8_t*)buf);
        if(len<0)return -2;}
    buf[len]='\0';return len;
}

static void tproc(void){
    char e[TBC+24];strcpy(e,TERM_PROMPT);strcat(e,ti);taddl(e);
    if(!strcmp(ti,"help"))taddl("help clear about date mem ls pwd cd mkdir rmdir cat wc grep env dl x11 win32 pkg script macos");
    else if(!strcmp(ti,"help net"))taddl("ifconfig ping netstat arp dns wget browse");
    else if(!strcmp(ti,"help srv"))taddl("dhcp ntp httpd [stop] rshell [stop]");
    else if(!strcmp(ti,"help dl"))taddl("ldd ldconfig dlopen dlsym");
    else if(!strcmp(ti,"help x11"))taddl("xinfo xdemo");
    else if(!strcmp(ti,"help win32"))taddl("win32info regedit runexe");
    else if(!strcmp(ti,"help pkg"))taddl("npkg list install remove info installed update");
    else if(!strcmp(ti,"help macos"))taddl("machoinfo runmacho cocoademo");
    else if(!strcmp(ti,"clear"))tlc=0;
    else if(!strcmp(ti,"about"))taddl(NEXUS_VERSION_STRING);
    else if(!strcmp(ti,"date")){rtc_time_t t;rtc_read(&t);char s[16];rtc_format_time(&t,s);taddl(s);}
    else if(!strcmp(ti,"mem")){char b[30],n[12];strcpy(b,"Free: ");int_to_str(pmm_get_free_pages()*4,n);strcat(b,n);strcat(b," KB");taddl(b);}
    else if(!strncmp(ti,"theme ",6))taddl(theme_set_by_name(ti+6)?"Changed.":"Unknown.");
    else if(!strcmp(ti,"exit"))taddl("Use the NexusOS menu > Exit to Console (F12).");
    else if(!strcmp(ti,"crashtest")){*(volatile uint32_t*)0=0xDEAD;}   /* Phase 55: NULL write -> contained by term_key's kguard */
    else if(!strcmp(ti,"ifconfig")){
        const net_config_t*cfg=ip_get_config();char b[TBC],ip[16];
        int cnt=net_device_count();
        if(cnt>0){net_device_t*d=net_get_device(0);char ms[18];net_mac_to_string(&d->mac,ms);
            tset(b,d->name);tjoin(b,d->link_up?" UP":" DOWN");taddl(b);
            tset(b,"MAC: ");tjoin(b,ms);taddl(b);}
        ip_to_string(cfg->ip,ip);tset(b,"IP:  ");tjoin(b,ip);taddl(b);
        ip_to_string(cfg->gateway,ip);tset(b,"GW:  ");tjoin(b,ip);taddl(b);
        ip_to_string(cfg->dns,ip);tset(b,"DNS: ");tjoin(b,ip);taddl(b);
    }
    else if(!strncmp(ti,"ping ",5)){
        uint32_t tgt=ip_parse(ti+5);
        if(!tgt){taddl("Invalid IP.");}
        else{
            char ip[16];ip_to_string(tgt,ip);
            char b[TBC];tset(b,"PING ");tjoin(b,ip);taddl(b);
            ping_state_t*ps=icmp_get_ping_state();
            ps->active=true;ps->target_ip=tgt;ps->id=(uint16_t)(system_ticks&0xFFFF);
            ps->seq_next=1;ps->replies=0;ps->sent=0;ps->got_reply=false;
            for(int i=0;i<4;i++){
                ps->got_reply=false;icmp_send_echo(tgt,ps->id,ps->seq_next++);
                uint32_t st=system_ticks;while(!ps->got_reply&&(system_ticks-st)<36){net_poll();__asm__ volatile("hlt");}
                if(ps->got_reply){char n[12];uint32_t rtt=(ps->last_rtt_ticks*1000)/18;
                    tset(b,"Reply: time=");int_to_str(rtt,n);tjoin(b,n);tjoin(b,"ms");taddl(b);
                }else taddl("Timeout.");
            }
            char n[12];b[0]='\0';int_to_str(ps->replies,n);tjoin(b,n);tjoin(b,"/");
            int_to_str(ps->sent,n);tjoin(b,n);tjoin(b," received");taddl(b);
            ps->active=false;
        }
    }
    else if(!strcmp(ti,"netstat")){
        int tc;const tcp_conn_t*conns=tcp_get_connections(&tc);bool any=false;
        for(int i=0;i<tc;i++){if(!conns[i].active)continue;any=true;
            char b[TBC],n[12],ip[16];tset(b,"TCP ");
            int_to_str(conns[i].local_port,n);tjoin(b,n);tjoin(b," -> ");
            ip_to_string(conns[i].remote_ip,ip);tjoin(b,ip);tjoin(b,":");
            int_to_str(conns[i].remote_port,n);tjoin(b,n);tjoin(b," ");
            tjoin(b,tcp_state_name(conns[i].state));taddl(b);
        }
        int uc;const udp_binding_t*ub=udp_get_bindings(&uc);
        for(int i=0;i<uc;i++){if(!ub[i].active)continue;any=true;
            char b[TBC],n[12];tset(b,"UDP port ");
            int_to_str(ub[i].port,n);tjoin(b,n);tjoin(b," BOUND");taddl(b);
        }
        if(!any)taddl("No connections.");
    }
    else if(!strcmp(ti,"arp")){
        int ac;const arp_entry_t*cache=arp_get_cache(&ac);bool any=false;
        for(int i=0;i<ac;i++){if(!cache[i].valid)continue;any=true;
            char b[TBC],ip[16],ms[18];
            ip_to_string(cache[i].ip,ip);net_mac_to_string(&cache[i].mac,ms);
            tset(b,ip);tjoin(b," -> ");tjoin(b,ms);taddl(b);
        }
        if(!any)taddl("Cache empty.");
    }
    else if(!strncmp(ti,"dns ",4)){
        char b[TBC];tset(b,"Resolving ");tjoin(b,ti+4);tjoin(b,"...");taddl(b);
        uint32_t ip=dns_resolve(ti+4);
        if(ip){char is[16];ip_to_string(ip,is);tset(b,ti+4);tjoin(b," -> ");tjoin(b,is);taddl(b);}
        else taddl("DNS lookup failed.");
    }
    else if(!strncmp(ti,"wget ",5)){
        char b[TBC];tset(b,"Fetching ");tjoin(b,ti+5);tjoin(b,"...");taddl(b);
        http_response_t resp=http_get(ti+5);
        if(resp.success){char n[12];tset(b,"HTTP ");int_to_str(resp.status_code,n);tjoin(b,n);
            tjoin(b," (");int_to_str(resp.body_len,n);tjoin(b,n);tjoin(b," bytes)");taddl(b);
            /* Show first few lines of body */
            int pos=0;
            for(int line=0;line<5&&pos<(int)resp.body_len;line++){
                char ln[TBC];int li=0;
                while(pos<(int)resp.body_len&&li<TBC-1&&resp.body[pos]!='\n'){
                    if(resp.body[pos]>=' ')ln[li++]=resp.body[pos];
                    pos++;
                }
                ln[li]='\0';if(li>0)taddl(ln);if(pos<(int)resp.body_len)pos++;
            }
        }else{tset(b,"Request failed.");if(resp.status_code>0){char n[12];tjoin(b," (HTTP ");
            int_to_str(resp.status_code,n);tjoin(b,n);tjoin(b,")");}taddl(b);}
    }
    else if(!strncmp(ti,"browse ",7)){
        /* Save URL before clearing ti */
        char burl[TBC];tset(burl,ti+7);
        taddl("Opening browser...");
        ti[0]='\0';til=0;screen_dirty=true;
        browser_open(burl);
        screen_dirty=true;
        return;
    }
    else if(!strcmp(ti,"dhcp")){
        taddl("Running DHCP...");
        ti[0]='\0';til=0;screen_dirty=true;
        bool ok=dhcp_request();
        if(ok){const net_config_t*c=ip_get_config();char b[TBC],ip[16];
            ip_to_string(c->ip,ip);tset(b,"IP: ");tjoin(b,ip);taddl(b);
            ip_to_string(c->gateway,ip);tset(b,"GW: ");tjoin(b,ip);taddl(b);
            ip_to_string(c->dns,ip);tset(b,"DNS: ");tjoin(b,ip);taddl(b);
        }else taddl("DHCP failed.");
        screen_dirty=true;return;
    }
    else if(!strcmp(ti,"ntp")){
        taddl("Syncing time...");
        ti[0]='\0';til=0;screen_dirty=true;
        bool ok=ntp_sync();
        if(ok){rtc_time_t t;rtc_read(&t);char s[16];rtc_format_time(&t,s);char b[TBC];tset(b,"Time: ");tjoin(b,s);taddl(b);}
        else taddl("NTP sync failed.");
        screen_dirty=true;return;
    }
    else if(!strcmp(ti,"httpd")){httpd_start();taddl(httpd_is_running()?"HTTPD running on :8080":"HTTPD failed.");}
    else if(!strcmp(ti,"httpd stop")){httpd_stop();taddl("HTTPD stopped.");}
    else if(!strcmp(ti,"rshell")){rshell_start();taddl(rshell_is_running()?"Remote shell on :2323":"RSHELL failed.");}
    else if(!strcmp(ti,"rshell stop")){rshell_stop();taddl("Remote shell stopped.");}
    else if(!strcmp(ti,"uname"))taddl(NEXUS_VERSION_STRING " i386");
    else if(!strcmp(ti,"whoami"))taddl(users_current_name());
    else if(!strcmp(ti,"env")){
        const char*vars[]={"PATH","HOME","USER","SHELL","TERM","PWD",NULL};
        for(int i=0;vars[i];i++){const char*v=env_get(vars[i]);
            if(v){char b[TBC];tset(b,vars[i]);tjoin(b,"=");tjoin(b,v);taddl(b);}}
    }
    else if(!strncmp(ti,"export ",7)){
        char*eq=NULL;for(int i=7;ti[i];i++)if(ti[i]=='='){eq=&ti[i];break;}
        if(eq){*eq='\0';env_set(ti+7,eq+1);char b[TBC];tset(b,ti+7);tjoin(b,"=");tjoin(b,eq+1);taddl(b);*eq='=';}
        else taddl("Usage: export KEY=VALUE");
    }
    else if(!strcmp(ti,"ls")||!strncmp(ti,"ls ",3)){
        fs_node_t*dir=(ti[2]==' ')?ramfs_resolve(ti+3,tcwd_node()):tcwd_node();
        if(!dir||dir->type!=FS_DIRECTORY){taddl("No such directory.");}
        else{bool any=false;
            for(uint32_t i=0;;i++){fs_node_t*n=vfs_readdir(dir,i);if(!n)break;any=true;
                char b[TBC],nb[12];tset(b,n->name);
                if(n->type==FS_DIRECTORY)tjoin(b,"/");
                tjoin(b,"  (");int_to_str((int)n->size,nb);tjoin(b,nb);tjoin(b," bytes)");taddl(b);}
            if(!any)taddl("(empty)");}
    }
    else if(!strcmp(ti,"pwd")){char p[FS_PATH_MAX];ramfs_abspath(tcwd_node(),p,sizeof(p));taddl(p);}
    else if(!strncmp(ti,"cd",2)&&(ti[2]=='\0'||ti[2]==' ')){
        const char*tgt=(ti[2]==' ')?ti+3:"/";while(*tgt==' ')tgt++;if(!*tgt)tgt="/";
        fs_node_t*n=ramfs_resolve(tgt,tcwd_node());
        if(!n)taddl("No such directory.");
        else if(n->type!=FS_DIRECTORY)taddl("Not a directory.");
        else{tcwd=n;char p[FS_PATH_MAX];ramfs_abspath(tcwd_node(),p,sizeof(p));env_set("PWD",p);}
    }
    else if(!strncmp(ti,"mkdir ",6)){
        int rc=ramfs_mkdir(ti+6,tcwd_node());
        taddl(rc==0?"Directory created.":rc==-2?"Permission denied.":rc==-3?"No such parent.":"mkdir failed.");
    }
    else if(!strncmp(ti,"rmdir ",6)){
        int rc=ramfs_rmdir(ti+6,tcwd_node());
        taddl(rc==0?"Directory removed.":rc==-2?"Permission denied.":rc==-3?"Directory not empty.":rc==-4?"Not a directory.":"Directory not found.");
    }
    else if(!strncmp(ti,"cat ",4)){
        char b[TBC];int rd=tload(ti+4,b,TBC);
        if(rd==-1)taddl("Not found.");else if(rd<0)taddl("Permission denied.");else taddl(b);
    }
    else if(!strncmp(ti,"wc ",3)){
        char buf[512];int len=tload(ti+3,buf,sizeof(buf));
        if(len==-1)taddl("Not found.");else if(len<0)taddl("Permission denied.");
        else{int li=0,w=0;bool iw=false;
            for(int i=0;i<len;i++){if(buf[i]=='\n')li++;if(buf[i]==' '||buf[i]=='\n')iw=false;else if(!iw){iw=true;w++;}}
            char r[TBC],nb[12];r[0]='\0';int_to_str(li,nb);tjoin(r,nb);tjoin(r," lines ");
            int_to_str(w,nb);tjoin(r,nb);tjoin(r," words ");int_to_str(len,nb);tjoin(r,nb);tjoin(r," chars");taddl(r);}
    }
    else if(!strncmp(ti,"grep ",5)){
        char*sp=NULL;for(int i=5;ti[i];i++)if(ti[i]==' '){sp=&ti[i];break;}
        if(!sp){taddl("Usage: grep <pat> <file>");}
        else{*sp='\0';const char*pat=ti+5;const char*fn=sp+1;
            char buf[512];int len=tload(fn,buf,sizeof(buf));
            if(len==-1)taddl("Not found.");else if(len<0)taddl("Permission denied.");
            else{char*p=buf;bool any=false;
                while(*p){char line[TBC];int li=0;while(*p&&*p!='\n'&&li<TBC-1)line[li++]=*p++;line[li]='\0';if(*p=='\n')p++;
                    if(strstr(line,pat)){taddl(line);any=true;}}
                if(!any)taddl("No matches.");}
            *sp=' ';}
    }
    else if(!strcmp(ti,"ldd"))taddl("Dynamic linker: 26 builtins, 0 libs loaded.");
    else if(!strcmp(ti,"ldconfig"))taddl("26 libc symbols registered (use the console for details).");
    else if(!strncmp(ti,"dlopen ",7))taddl("dlopen: use the console (F12) for full dl commands.");
    else if(!strncmp(ti,"dlsym ",6))taddl("dlsym: use the console (F12) for full dl commands.");
    else if(!strcmp(ti,"xinfo"))taddl("X11 shim: NexusOS X11 v1.0 (use the console for xdemo).");
    else if(!strcmp(ti,"xdemo"))taddl("xdemo: use the console (F12) to launch the X11 demo.");
    else if(!strcmp(ti,"win32info"))taddl("win32info: use the console (F12) for details.");
    else if(!strcmp(ti,"regedit"))taddl("regedit: use the console (F12) for the registry.");
    else if(!strncmp(ti,"runexe",6))taddl("runexe: use the console (F12) to run PE32.");
    else if(!strncmp(ti,"npkg",4))taddl("npkg: use the console (F12) for the package manager.");
    else if(!strncmp(ti,"script",6))taddl("script: use the console (F12) to run NexusScript files.");
    else if(!strcmp(ti,"machoinfo"))taddl("machoinfo: use the console (F12) for the macOS shim.");
    else if(!strncmp(ti,"runmacho",8))taddl("runmacho: use the console (F12) to load Mach-O.");
    else if(!strcmp(ti,"cocoademo"))taddl("cocoademo: use the console (F12) to launch it.");
    else if(ti[0])taddl("Unknown command. Try 'help'");
    ti[0]='\0';til=0;screen_dirty=true;
}

/* --------------------------------------------------------------------------
 * About This NexusOS — drawn in pixels inside its window's content area
 * -------------------------------------------------------------------------- */
static void about_row(int x, int w, int y, const char* k, const char* v){
    int kw=ui_text_width(k,UF_UIB);
    ui_text(x+w/2-6-kw,y,k,UF_UIB,UI_RGB(236,236,240));
    ui_text(x+w/2+6,y,v,UF_UI,UI_RGB(170,170,176));
}

static void about_draw(int id,int cx,int cy,int cw,int ch){
    (void)id;(void)ch;
    int x=cx*8, y=cy*16, w=cw*8;
    int mx=x+w/2;
    /* artwork: the hills wallpaper in a rounded "display" tile */
    int tw=168, th=104, tx=mx-tw/2, ty=y+6;
    for(int j=0;j<th;j++){
        int wy=(j*(int)fb_get_height())/th;
        for(int i=0;i<tw;i++){
            int wx=(i*(int)fb_get_width())/tw;
            fb_putpixel(tx+i,ty+j,ui_wallpaper_at(wx,wy));
        }
    }
    ui_rrect_stroke(tx,ty,tw,th,10,UI_RGB(30,30,32),255);
    ui_rrect_stroke(tx+1,ty+1,tw-2,th-2,9,UI_RGB(30,30,32),255);
    ui_rrect_stroke(tx-1,ty-1,tw+2,th+2,11,UI_RGB(90,90,96),255);
    ui_logo(mx-14,ty+th/2-17,34,UI_RGB(250,250,252));

    ui_text_center(mx,ty+th+16,"NexusOS",UF_TITLE,0xFFFFFF);
    ui_text_center(mx,ty+th+46,"Version " NEXUS_VERSION " (" NEXUS_CODENAME ")",UF_UI,UI_RGB(150,150,156));

    char mem[24],res[24],n[12];
    uint32_t mb=pmm_get_total_pages()/256;          /* 4 KB pages -> MB */
    if(mb<1024){int_to_str((int)mb,n);strcpy(mem,n);strcat(mem," MB");}
    else{int_to_str((int)(mb/1024),n);strcpy(mem,n);strcat(mem," GB");}
    int_to_str((int)fb_get_width(),n);strcpy(res,n);strcat(res," x ");int_to_str((int)fb_get_height(),n);strcat(res,n);

    int ry=ty+th+82;
    about_row(x,w,ry,     "Chip",    "Intel x86 (i386)");
    about_row(x,w,ry+22,  "Memory",  mem);
    about_row(x,w,ry+44,  "Display", res);
    about_row(x,w,ry+66,  "Kernel",  "Nexus " NEXUS_VERSION);
    about_row(x,w,ry+88,  "Startup", "Nexus HD");

    int bw=120,bh=26,bx=mx-bw/2,by=ry+124;
    ui_rrect(bx,by,bw,bh,7,UI_RGB(92,92,98),255);
    ui_rrect_stroke(bx,by,bw,bh,7,UI_RGB(120,120,126),160);
    ui_text_center(mx,by+(bh-ui_font_height(UF_UI))/2+1,"More Info...",UF_UI,0xFFFFFF);
    ui_text_center(mx,by+bh+22,"Built from scratch in C and x86 assembly.",UF_SMALL,UI_RGB(120,120,126));
}
static void about_key(int id,char key){(void)id;(void)key;}

static int term_puts(int x,int y,int max,const char*s,uint8_t col){
    int n=0;while(s[n]&&n<max){gui_putchar(x+n,y,s[n],col);n++;}return n;
}
static uint8_t term_line_color(uint8_t role,int bg){
    if(role==TC_ECHO)return VGA_COLOR(VGA_DARK_GREY,bg);
    if(role==TC_ERR)return VGA_COLOR(VGA_LIGHT_RED,bg);
    if(role==TC_BANNER)return VGA_COLOR(VGA_LIGHT_CYAN,bg);
    if(role==TC_HINT)return VGA_COLOR(VGA_DARK_GREY,bg);
    return VGA_COLOR(VGA_LIGHT_GREY,bg);
}
static int term_draw_prompt_prefix(int x,int y,int max,int bg){
    int p=0;
    p+=term_puts(x+p,y,max-p,"nexus",VGA_COLOR(VGA_LIGHT_GREEN,bg));
    p+=term_puts(x+p,y,max-p,"@os",VGA_COLOR(VGA_LIGHT_CYAN,bg));
    p+=term_puts(x+p,y,max-p,":~",VGA_COLOR(VGA_LIGHT_BLUE,bg));
    p+=term_puts(x+p,y,max-p,"$ ",VGA_COLOR(VGA_WHITE,bg));
    return p;
}
static void term_draw_current_prompt(int x,int y,int max,int bg){
    int p=term_draw_prompt_prefix(x,y,max,bg);
    int input_max=max-p;if(input_max<1)return;
    int start=0;if(til>input_max)start=til-input_max;
    int drawn=0;
    for(int i=start;i<til&&drawn<input_max;i++,drawn++)
        gui_putchar(x+p+drawn,y,ti[i],VGA_COLOR(VGA_LIGHT_GREY,bg));
    if(drawn<input_max)
        gui_putchar(x+p+drawn,y,(char)0xDB,VGA_COLOR(VGA_DARK_GREY,bg));
}
static void term_draw_line(int x,int y,int max,const char*line,uint8_t role,int bg){
    if(role==TC_ECHO&&!strncmp(line,TERM_PROMPT,strlen(TERM_PROMPT))){
        int p=term_draw_prompt_prefix(x,y,max,bg);
        term_puts(x+p,y,max-p,line+strlen(TERM_PROMPT),VGA_COLOR(VGA_DARK_GREY,bg));
        return;
    }
    term_puts(x,y,max,line,term_line_color(role,bg));
}
static void term_draw(int id,int cx,int cy,int cw,int ch){
    (void)id;
    int bg=VGA_BLACK;
    uint8_t fill=VGA_COLOR(VGA_LIGHT_GREY,bg);
    for(int y=0;y<ch;y++)for(int x=0;x<cw;x++)gui_putchar(cx+x,cy+y,' ',fill);
    int rows=ch,prompt_index=tlc,st=prompt_index-rows+1-tscr;
    if(st<0)st=0;
    if(st>prompt_index)st=prompt_index;
    int tx=cx+1,tw=cw-2;if(tw<1)return;
    for(int r=0;r<rows;r++){
        int idx=st+r;if(idx<tlc)term_draw_line(tx,cy+r,tw,tl[idx],trole[idx],bg);
        else if(idx==tlc)term_draw_current_prompt(tx,cy+r,tw,bg);
    }
}
static void term_key(int id,char key){
    (void)id;
    kguard_ctx_t kg;
    if(key=='\n'){
        /* ---- Phase 55: a fault inside a terminal command must not take the
         * Terminal (or the desktop) down: run it in a kguard and report. ---- */
        int krc=kguard_try(&kg,"gui terminal");
        if(krc==0){tproc();kguard_end();}
        else{char d[48];kguard_describe(krc,d,sizeof(d));
             taddlc("Command crashed - recovered",TC_ERR);taddlc(d,TC_ERR);
             ti[0]='\0';til=0;}
        /* ---- end Phase 55 ---- */
    }
    else if(key=='\b'){if(til>0)ti[--til]='\0';}
    else if(key==22){const char*c=clipboard_paste();if(c){clipmgr_record(c);while(*c&&til<TBC-1){ti[til++]=*c++;}ti[til]='\0';}}
    else if(key>=32&&key<127&&til<TBC-1){ti[til++]=key;ti[til]='\0';}
    screen_dirty=true;
}

static void open_about(void){
    int c=(int)fb_get_width()/16, r=(int)fb_get_height()/16;
    window_create("",c-17,r/2-13,34,26,about_draw,about_key);
    syslog_add("About");screen_dirty=true;
}
static void open_terminal(void){
    tlc=0;til=0;ti[0]='\0';tscr=0;
    taddlc("Last login: tty0 on console",TC_HINT);taddl("");
    window_create("Terminal",22,6,92,30,term_draw,term_key);syslog_add("Terminal");screen_dirty=true;
}

/* Bring an app's window forward if it is already open (dock behaviour). */
static bool focus_app_window(int action){
    int count;const ui_app_t*apps=ui_apps(&count);
    for(int i=0;i<count;i++){
        if(apps[i].action!=action||!apps[i].title)continue;
        for(int w=0;w<MAX_WINDOWS;w++){window_t*ww=window_get(w);
            if(ww&&!strcmp(ww->title,apps[i].title)){
                if(ww->flags&WIN_MINIMIZED)window_restore(w);else window_focus(w);
                return true;}}
    }
    return false;
}

static bool run=true;
static void handle_action(int a){
    switch(a){
        case 1:open_terminal();break;
        case 2:filemgr_open();syslog_add("Files");break;
        case 3:calculator_open();syslog_add("Calc");break;
        case 4:sysmon_open();syslog_add("SysMon");break;
        case 5:settings_open();syslog_add("Settings");break;
        case 6:break;
        case 7:settings_open();break;
        case 8:lockscreen_run();last_input_tick=system_ticks;break;
        case 9:theme_cycle();syslog_add("Theme");break;
        case 10:notepad_open();syslog_add("Notepad");break;
        case 11:taskmgr_open();syslog_add("TaskMgr");break;
        case 12:calendar_open();syslog_add("Calendar");break;
        case 13:music_open();syslog_add("Music");break;
        case 14:help_open();syslog_add("Help");break;
        case 15:paint_open();syslog_add("Paint");break;
        case 16:minesweeper_open();syslog_add("Minesweeper");break;
        case 17:recycle_open();syslog_add("Recycle");break;
        case 18:sysinfo_open();syslog_add("SysInfo");break;
        case 19:todo_open();syslog_add("Todo");break;
        case 20:clock_open();syslog_add("Clock");break;
        case 21:pong_open();syslog_add("Pong");break;
        case 22:spotlight_open();break;
        case 23:tetris_open();syslog_add("Tetris");break;
        case 24:hexview_open();syslog_add("HexView");break;
        case 25:contacts_open();syslog_add("Contacts");break;
        case 26:colorpick_open();syslog_add("ColorPick");break;
        case 27:syslog_open();break;
        case 28:clipmgr_open();syslog_add("ClipMgr");break;
        case 29:appearance_open();syslog_add("Appearance");break;
        case 30:power_menu_open();break;
        case 31:fileops_open();syslog_add("FileOps");break;
        case 32:browser_open(NULL);syslog_add("Browser");screen_dirty=true;break;   /* Phase 57: Nexus Browser (about:home) */
        case UI_ACT_LAUNCHPAD:launchpad_open();break;
        case UI_ACT_ABOUT:open_about();break;
        case UI_ACT_SPOTLIGHT:spotlight_open();break;
        case UI_ACT_LOCK:lockscreen_run();last_input_tick=system_ticks;break;
        case UI_ACT_RESTART:syslog_add("Restart");power_restart();break;
        case UI_ACT_SHUTDOWN:syslog_add("Shutdown");power_shutdown();break;
        case UI_ACT_CONSOLE:run=false;break;
        case UI_ACT_SLEEP:screensaver_run();last_input_tick=system_ticks;break;
        case UI_ACT_CLOSEWIN:{int f=window_get_focused();if(f>=0)window_destroy(f);break;}
        case UI_ACT_MINIMIZE:{int f=window_get_focused();if(f>=0)window_minimize(f);break;}
        case UI_ACT_ZOOM:{int f=window_get_focused();if(f>=0)window_maximize(f);break;}
        case UI_ACT_SHORTCUTS:shortcuts_open();break;
        case UI_ACT_NOTIFS:notifcenter_toggle();break;
        case UI_ACT_NEWFILE:{static int n=0;char nm[32],nb[8];strcpy(nm,"untitled");int_to_str(n++,nb);strcat(nm,nb);strcat(nm,".txt");
            ramfs_create(nm,FS_FILE);syslog_add("Created file");notify_push("New file created");notifcenter_add(nm,0);break;}
    }
    screen_dirty=true;
}

static void handle_ctx(int a){
    switch(a){
        case CMENU_NEW_FILE:handle_action(UI_ACT_NEWFILE);break;
        case CMENU_REFRESH:syslog_add("Refreshed");break;
        case CMENU_THEME_NEXT:appearance_open();break;
        case CMENU_ABOUT:open_about();break;
        case CMENU_SETTINGS:settings_open();syslog_add("Settings");break;
    }
    screen_dirty=true;
}

/* Name shown in the menu bar for the focused window ("Finder" if none). */
static const char* active_app_name(void){
    int f=window_get_focused();
    window_t*w=f>=0?window_get(f):NULL;
    if(!w||(w->flags&WIN_MINIMIZED))return "Finder";
    if(!w->title[0])return "NexusOS";
    int count;const ui_app_t*apps=ui_apps(&count);
    for(int i=0;i<count;i++)if(apps[i].title&&!strcmp(apps[i].title,w->title))return apps[i].name;
    return w->title;
}

/* Pointer save-under: erase by restoring, stamp by saving then drawing. */
static uint32_t cursor_under[UI_CURSOR_W * UI_CURSOR_H];
static int  cur_px = -1, cur_py = -1;
static bool cur_shown = false;

static void cursor_stamp(int px, int py){
    fb_save_rect(px - 1, py - 1, UI_CURSOR_W, UI_CURSOR_H, cursor_under);
    ui_cursor_draw(px, py);
    cur_px = px; cur_py = py; cur_shown = true;
}
static void cursor_erase(void){
    if (!cur_shown) return;
    fb_restore_rect(cur_px - 1, cur_py - 1, UI_CURSOR_W, UI_CURSOR_H, cursor_under);
    cur_shown = false;
}
static void cursor_mark(int px, int py){ fb_mark_dirty(px - 1, py - 1, UI_CURSOR_W, UI_CURSOR_H); }

/* Full recomposite of the desktop into the back buffer. */
static void compose(int mx, int my){
    if (launchpad_is_open()) {
        launchpad_draw();
        base_valid=false;
    } else {
        ui_wallpaper_draw();
        if(widgets_visible())widgets_draw();
        window_set_hover(mx,my);
        window_draw_all();
        /* Phase 56: snapshot the base layer under the dock band (icons grow ~40 px
         * above the shelf when magnified and the name bubble sits above that). */
        {
            int W=(int)fb_get_width(), H=(int)fb_get_height();
            dock_band_y=dock_top()-150; if(dock_band_y<MENUBAR_H) dock_band_y=MENUBAR_H;
            dock_band_h=H-dock_band_y;
            base_valid=(W<=BAND_MAX_W && dock_band_h>0 && dock_band_h<=DOCK_BAND_MAX_H);
            if(base_valid) fb_save_rect(0,dock_band_y,W,dock_band_h,dock_under);
        }
        dock_draw(mx,my);
        menubar_draw(active_app_name());
        if(context_menu_is_open())context_menu_draw();
        if(switcher_is_open())switcher_draw();
        if(power_menu_is_open())power_menu_draw();
        if(notifcenter_is_open())notifcenter_draw();
        if(shortcuts_is_open())shortcuts_draw();
        menubar_draw_menu();
        spotlight_draw();
        notify_draw();
    }
    cur_shown = false;
    cursor_stamp(mx, my);
}

/* ---- Phase 55: desktop-level crash containment -----------------------------
 * The whole per-frame body of desktop_run (compose, click/key dispatch, app
 * launches) runs inside a kguard. App callbacks have their own tighter guard
 * in window.c (which closes just that window); this one is the safety net for
 * faults elsewhere in the frame. Overlays left half-open by the abandoned
 * frames are closed; a repeat offender loses the focused window, and a crash
 * loop (4 faults within ~3 s) exits to the console instead of spinning. */
static int kg_streak=0;static uint32_t kg_last_tick=0;
static void desktop_crash_recover(int rc){
    char d[48];kguard_describe(rc,d,sizeof(d));
    if(system_ticks-kg_last_tick>54)kg_streak=0;
    kg_last_tick=system_ticks;kg_streak++;
    start_menu_close();context_menu_close();palette_close();menubar_close();spotlight_close();launchpad_close();
    if(kg_streak>=2){int f=window_get_focused();if(f>=0)window_destroy(f);}
    if(kg_streak>=4){syslog_add("Desktop: crash loop, exiting");run=false;return;}
    notify_push(kg_streak>=2?"Desktop fault recovered - window closed":"Desktop fault recovered");
    notify_push(d);
    char m[40];strcpy(m,"Desktop crash ");strcat(m,kguard_exc_short(rc));syslog_add(m);
    screen_dirty=true;
}
/* ---- end Phase 55 ---- */

/* Does the pointer sit over any window's traffic-light group? */
static int lights_hover_window(int px, int py){
    for(int i=0;i<MAX_WINDOWS;i++){window_t*w=window_get(i);
        if(!w||(w->flags&WIN_MINIMIZED))continue;
        if(px>=w->px+8&&px<w->px+76&&py>=w->py+4&&py<w->py+WIN_TITLEBAR_H-4)return i;}
    return -1;
}

void desktop_run(void){
    gui_init();workspaces_init();recycle_init();syslog_init();dock_init();
    ui_wallpaper_init();
    syslog_add(NEXUS_VERSION_STRING " desktop started");
    bool drag=false;int dwn=-1,dox=0,doy=0;
    int rsz=-1,redge=RESIZE_NONE,rx0=0,rw0=0,rh0=0,rmx=0,rmy=0;   /* edge resize */
    int last_tb_win=-1;uint32_t last_tb_tick=0;                                 /* titlebar double-click */
    bool pl=false,pr=false;int pmx=-1,pmy=-1,last_lights=-1;
    port_byte_out(0x3D4,0x0A);port_byte_out(0x3D5,0x20);
    start_menu_close();context_menu_close();palette_close();menubar_close();spotlight_close();launchpad_close();
    last_tick=system_ticks;last_input_tick=system_ticks;
    screen_dirty=true;frame_count=0;
    { char wb[48]; strcpy(wb,"Welcome to "); strcat(wb,NEXUS_VERSION_STRING); notify_push(wb); notifcenter_add(wb,0); }

    run=true;
    kguard_ctx_t kg;                                           /* Phase 55 */
    while(run){
        /* ---- Phase 55: one kguard per desktop iteration (see desktop_crash_recover) ---- */
        {int krc=kguard_try(&kg,"desktop");if(krc){desktop_crash_recover(krc);continue;}}
        bool inp=false;
        bool cursor_moved=false;
        bool dock_dirty=false,bar_dirty=false;                    /* Phase 56: hover-only changes */
        mouse_state_t ms=mouse_get_state();
        if(ms.px!=pmx||ms.py!=pmy){
            cursor_moved=true;pmx=ms.px;pmy=ms.py;
            if(!fb_is_vesa())screen_dirty=true;
            if(menubar_hover(ms.px,ms.py))bar_dirty=true;                       /* Phase 56: band redraw */
            if(context_menu_hover(ms.px,ms.py))screen_dirty=true;
            if(!launchpad_is_open()&&dock_hover_changed(ms.px,ms.py))dock_dirty=true;   /* Phase 56 */
            int lh=lights_hover_window(ms.px,ms.py);
            if(lh!=last_lights){last_lights=lh;screen_dirty=true;}
        }
        bool ln=(ms.buttons&MOUSE_LEFT)!=0,rn=(ms.buttons&MOUSE_RIGHT)!=0;
        /* A press+release that both happened since the last frame still counts */
        bool lc=(ln&&!pl)||(ms.clicked&&!ln&&!pl),lr=!ln&&pl,rc=rn&&!pr;
        int px=ms.px,py=ms.py;

        if(lc){inp=true;
            if(launchpad_is_open()){int a=launchpad_click(px,py);if(a)handle_action(a);}
            else if(spotlight_is_open()){int a=spotlight_hit(px,py)?spotlight_click(px,py):0;if(!spotlight_hit(px,py))spotlight_close();if(a)handle_action(a);}
            else if(menubar_menu_open()){
                if(menubar_menu_hit(px,py)){int a=menubar_menu_click(px,py);if(a)handle_action(a);}
                else if(menubar_hit(px,py))menubar_click(px,py);
                else menubar_close();
            }
            else if(shortcuts_is_open()){shortcuts_close();}
            else if(power_menu_is_open()){power_menu_close();}
            else if(notifcenter_is_open()){notifcenter_toggle();}
            else if(switcher_is_open())switcher_close();
            else if(context_menu_is_open()&&context_menu_hit(px,py)){int a=context_menu_handle_click(px,py);if(a)handle_ctx(a);}
            else if(context_menu_is_open())context_menu_close();
            else if(menubar_hit(px,py)){int a=menubar_click(px,py);if(a)handle_action(a);}
            else if(dock_hit(px,py)){int a=dock_click(px,py);if(a&&!focus_app_window(a))handle_action(a);}
            else{
                int h = fb_is_vesa() ? window_hit_test_px(px, py) : window_hit_test(ms.x, ms.y);
                if(h>=0){window_focus(h);
                    if(fb_is_vesa() ? window_close_hit_px(h,px,py) : window_close_hit(h,ms.x,ms.y))window_destroy(h);
                    else if(fb_is_vesa() ? window_minimize_hit_px(h,px,py) : window_minimize_hit(h,ms.x,ms.y))window_minimize(h);
                    else if(fb_is_vesa() ? window_maximize_hit_px(h,px,py) : window_maximize_hit(h,ms.x,ms.y))window_maximize(h);
                    else if(fb_is_vesa()&&window_resize_hit(h,px,py)!=RESIZE_NONE&&!window_titlebar_hit_px(h,px,py)
                            &&!(window_get(h)->flags&WIN_MAXIMIZED)){
                        window_t*w=window_get(h);
                        rsz=h;redge=window_resize_hit(h,px,py);rx0=w->px;rw0=w->pw;rh0=w->ph;rmx=px;rmy=py;
                    }
                    else if(fb_is_vesa() ? window_titlebar_hit_px(h,px,py) : window_titlebar_hit(h,ms.x,ms.y)){
                        window_t*w=window_get(h);
                        if(h==last_tb_win&&system_ticks-last_tb_tick<10){window_maximize(h);last_tb_win=-1;}
                        else{last_tb_win=h;last_tb_tick=system_ticks;}
                        if(w&&(w->flags&WIN_MOVABLE)&&!(w->flags&WIN_MAXIMIZED)&&last_tb_win==h){
                            drag=true;dwn=h;
                            if(fb_is_vesa()){ dox=px-w->px; doy=py-w->py; }
                            else { dox=ms.x-w->x; doy=ms.y-w->y; }
                        }
                    }
                    else if(fb_is_vesa()){ window_send_click(h,px,py); }   /* content click -> app */
                }else{
                    /* Click on the bare desktop: no window has focus (Finder) */
                    int f=window_get_focused();if(f>=0){window_t*w=window_get(f);if(w)w->flags&=~WIN_FOCUSED;}
                }
            }
        }
        if(rc){inp=true;
            bool hit = fb_is_vesa() ? (window_hit_test_px(px, py) >= 0) : (window_hit_test(ms.x, ms.y) >= 0);
            if(!menubar_hit(px,py) && !dock_hit(px,py) && !hit && !launchpad_is_open()){
                menubar_close();spotlight_close();context_menu_open(px,py);
            }
        }
        if(drag&&ln){
            if(fb_is_vesa()) window_move_px(dwn,px-dox,py-doy);
            else window_move(dwn,ms.x-dox,ms.y-doy);
            inp=true;
        }
        if(rsz>=0&&ln){
            /* Resize from the side/bottom edges and corners, in whole cells */
            int dx=px-rmx,dy=py-rmy,nx=rx0,nw=rw0,nh=rh0;
            if(redge==RESIZE_E||redge==RESIZE_SE||redge==RESIZE_NE)nw=rw0+dx;
            if(redge==RESIZE_W||redge==RESIZE_SW||redge==RESIZE_NW){nw=rw0-dx;nx=rx0+dx;}
            if(redge==RESIZE_S||redge==RESIZE_SE||redge==RESIZE_SW)nh=rh0+dy;
            nw=(nw/8)*8;nh=(nh/16)*16;
            if(nw<WIN_MIN_W)nw=(WIN_MIN_W+7)/8*8;
            if(nh<WIN_MIN_H)nh=(WIN_MIN_H+15)/16*16;
            if(nx!=rx0)nx=rx0+rw0-nw;
            window_t*w=window_get(rsz);
            if(w){if(nx!=w->px)window_move_px(rsz,nx,w->py);window_resize_px(rsz,nw,nh);}
            inp=true;
        }
        if(lr){drag=false;dwn=-1;rsz=-1;}
        pl=ln;pr=rn;mouse_clear_events();

        while(keyboard_has_key()){
            char key=keyboard_getchar();unsigned char k=(unsigned char)key;inp=true;
            if(k==KEYC_SPOTLIGHT||k==16){launchpad_close();menubar_close();
                if(spotlight_is_open())spotlight_close();else spotlight_open();continue;}
            if(launchpad_is_open()){int a=launchpad_key(key);if(a)handle_action(a);continue;}
            if(spotlight_is_open()){int a=spotlight_key(key);if(a)handle_action(a);continue;}
            if(menubar_menu_open()){int a=menubar_handle_key(key);if(a)handle_action(a);continue;}
            if(shortcuts_is_open()){shortcuts_handle_key(key);continue;}
            if(power_menu_is_open()){
                int pa=power_menu_handle_key(key);
                if(pa==POWER_SHUTDOWN){syslog_add("Shutdown");power_shutdown();}
                if(pa==POWER_RESTART){syslog_add("Restart");power_restart();}
                if(pa==POWER_LOCK){lockscreen_run();last_input_tick=system_ticks;}
                continue;
            }
            if(notifcenter_is_open()){notifcenter_handle_key(key);continue;}
            if(palette_is_open()){int a=palette_handle_key(key);if(a)handle_action(a);continue;}
            if(switcher_is_open()){if(key=='\t')switcher_next();else if(key=='\n')switcher_close();else if(key==27)switcher_cancel();continue;}
            if(context_menu_is_open()){if(key==27)context_menu_close();else{int a=context_menu_handle_key(key);if(a)handle_ctx(a);}continue;}
            if(start_menu_is_open()){start_menu_close();continue;}

            /* ---- Phase 57: the browser claims Tab (link cycling), Ctrl+L (address bar), F5 ---- */
            if(browser_claims_key(key)){window_send_key(key);continue;}
            /* ---- end Phase 57 ---- */
            if(k==KEYC_F(12)){run=false;beep_stop();break;}          /* Exit to Console */
            if(k==KEYC_F(1)){shortcuts_open();continue;}
            if(k==KEYC_F(2)){help_open();syslog_add("Help");continue;}
            if(k==KEYC_F(3)){switcher_open();continue;}
            if(k==KEYC_F(4)){launchpad_open();continue;}
            if(key=='\t'){switcher_open();continue;}
            if(key==17||key==23){int f=window_get_focused();if(f>=0)window_destroy(f);continue;}   /* Ctrl+Q / Ctrl+W */
            if(key==1){open_about();continue;}
            if(key==20){open_terminal();continue;}
            if(key==13){launchpad_open();continue;}
            if(key==6){filemgr_open();syslog_add("Files");continue;}
            if(key==11){calculator_open();syslog_add("Calc");continue;}
            if(key==19){sysmon_open();syslog_add("SysMon");continue;}
            if(key==14){notifcenter_toggle();continue;}
            if(key==12){lockscreen_run();last_input_tick=system_ticks;continue;}
            if(key==2){notepad_open();syslog_add("Notepad");continue;}
            if(key==7){taskmgr_open();syslog_add("TaskMgr");continue;}
            if(key==4){calendar_open();syslog_add("Calendar");continue;}
            if(key==9){widgets_toggle();continue;}
            if(key==22){const char*c=clipboard_paste();if(c)clipmgr_record(c);window_send_key(key);continue;}

            if(k>=KEYC_DESKTOP(0)&&k<=KEYC_DESKTOP(3)){int d=k-KEYC_DESKTOP(0);workspaces_switch(d);
                char m[16];strcpy(m,"Desktop ");char nb[4];int_to_str(d+1,nb);strcat(m,nb);syslog_add(m);continue;}
            if(k==KEYC_SNAP_LEFT){int f=window_get_focused();if(f>=0)window_snap_left(f);continue;}
            if(k==KEYC_SNAP_RIGHT){int f=window_get_focused();if(f>=0)window_snap_right(f);continue;}
            if(k==KEYC_MAXIMIZE){int f=window_get_focused();if(f>=0)window_maximize(f);continue;}
            if(k==KEYC_RESTORE){int f=window_get_focused();if(f>=0)window_restore(f);continue;}
            if(key==27)continue;                          /* Esc closes overlays only */

            if(window_get_focused()>=0)window_send_key(key);
        }

        /* Phase 43: emit any deferred accessibility earcon (IRQ hooks defer
         * sound here), and force a redraw if a global hotkey changed the theme
         * or text scale out from under us. */
        accessibility_poll();

        /* Phase 45: service backgrounded network servers (VNC remote desktop +
         * cloud sync) once per frame in normal context. net_poll() pumps RX
         * through the protocol stack; the server polls then act on it. */
        net_poll();
        vnc_poll();
        sync_poll();
        rshell_poll();    /* Phase 54: authenticated remote shell */
        httpd_poll();     /* Phase 55: HTTP file server */
        usb_hid_poll();   /* Phase 54: USB HID boot kbd/mouse */

        /* Phase 47: recognize touch gestures from the mouse (no-op unless
         * touch mode is on) — observes mouse state only, doesn't change it. */
        mobile_poll();
        persist_poll();   /* Phase 55: debounced ramfs snapshot autosave */
        /* ---- Phase 57: Nexus Browser - pump netstreams / image decode / link hover ---- */
        if(browser_poll())screen_dirty=true;
        /* ---- end Phase 57 ---- */

        { static int acc_theme=-2, acc_scale=-2;
          int th=theme_get_index(), fs=font_get_scale();
          if(th!=acc_theme||fs!=acc_scale){acc_theme=th;acc_scale=fs;screen_dirty=true;last_input_tick=system_ticks;} }

        if(inp){screen_dirty=true;last_input_tick=system_ticks;}
        if(cursor_moved){last_input_tick=system_ticks;}

        /* Idle tick: repaint only the menu bar (clock) unless a toast needs
         * the whole frame. */
        bool clock_tick=false;
        if(system_ticks-last_tick>=(uint32_t)mobile_redraw_interval()){
            last_tick=system_ticks;frame_count++;
            bool toast_changed=notify_update();
            if(toast_changed||notify_any_active()) screen_dirty=true;
            else if(!screen_dirty) clock_tick=true;
        }
        if(system_ticks-last_input_tick>=IDLE_TIMEOUT){screensaver_run();last_input_tick=system_ticks;screen_dirty=true;}

        /* Phase 56: a hover-only change (dock magnification / menu-bar highlight) can
         * skip the full composite when nothing else is on screen that the base
         * snapshot does not contain (overlays, menus, toasts, Launchpad). */
        bool partial_ok = base_valid && fb_is_vesa() && !screen_dirty
            && !launchpad_is_open() && !spotlight_is_open() && !menubar_menu_open()
            && !context_menu_is_open() && !switcher_is_open() && !power_menu_is_open()
            && !notifcenter_is_open() && !shortcuts_is_open() && !notify_any_active();
        if(screen_dirty || ((dock_dirty||bar_dirty) && !partial_ok)){
            compose(ms.px,ms.py);
            if(fb_is_vesa()) fb_flip_full();
            else gui_flip();
            screen_dirty=false;
        } else if(dock_dirty||bar_dirty){
            int W=(int)fb_get_width();
            bool had_old=cur_shown; int ox=cur_px,oy=cur_py;
            cursor_erase();
            if(had_old) cursor_mark(ox,oy);
            if(dock_dirty){
                fb_restore_rect(0,dock_band_y,W,dock_band_h,dock_under);
                dock_draw(ms.px,ms.py);
                fb_mark_dirty(0,dock_band_y,W,dock_band_h);
            }
            if(bar_dirty){
                menubar_draw(active_app_name());
                fb_mark_dirty(0,0,W,MENUBAR_H);
            }
            cursor_stamp(ms.px,ms.py);
            cursor_mark(ms.px,ms.py);
            gui_flip();
        } else if(clock_tick && fb_is_vesa() && !launchpad_is_open()){
            bool had_old=cur_shown; int ox=cur_px,oy=cur_py;
            cursor_erase();
            if(had_old) cursor_mark(ox,oy);
            menubar_draw(active_app_name());
            if(menubar_menu_open())menubar_draw_menu();
            fb_mark_dirty(0,0,(int)fb_get_width(),MENUBAR_H);
            cursor_stamp(ms.px,ms.py);
            cursor_mark(ms.px,ms.py);
            gui_flip();
        } else if(cursor_moved && fb_is_vesa()){
            bool had_old=cur_shown;
            int ox=cur_px,oy=cur_py;
            cursor_erase();
            if(had_old) cursor_mark(ox,oy);
            cursor_stamp(ms.px,ms.py);
            cursor_mark(ms.px,ms.py);
            gui_flip();
        }
        kguard_end();                                          /* Phase 55 */
        __asm__ volatile("hlt");
    }

    syslog_add("Desktop exit");
    start_menu_close();context_menu_close();palette_close();menubar_close();spotlight_close();launchpad_close();beep_stop();
    for(int i=0;i<MAX_WINDOWS;i++)window_destroy(i);
    port_byte_out(0x3D4,0x0A);port_byte_out(0x3D5,14);
    port_byte_out(0x3D4,0x0B);port_byte_out(0x3D5,15);
    vga_init();
    vga_flush();
}
