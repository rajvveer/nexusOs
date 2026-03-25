/* ============================================================================
 * NexusOS - Desktop Compositor - Phase 13 (v1.3)
 * ============================================================================ */

#include "desktop.h"
#include "gui.h"
#include "theme.h"
#include "window.h"
#include "taskbar.h"
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
#include "icons.h"
#include "context_menu.h"
#include "notify.h"
#include "ramfs.h"
#include "vfs.h"
#include "switcher.h"
#include "screensaver.h"
#include "lockscreen.h"
#include "clipboard.h"
#include "wallpaper.h"
#include "palette.h"
#include "notepad.h"
#include "taskmgr.h"
#include "calendar.h"
#include "music.h"
#include "help.h"
#include "widgets.h"
#include "paint.h"
#include "speaker.h"
#include "workspaces.h"
#include "minesweeper.h"
#include "recycle.h"
#include "sysinfo.h"
#include "todo.h"
#include "clock.h"
#include "pong.h"
#include "power.h"
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
#include "procfs.h"
#include "termios.h"
#include "posix.h"

static void about_draw(int,int,int,int,int);
static void about_key(int,char);
static void term_draw(int,int,int,int,int);
static void term_key(int,char);

static bool screen_dirty=true;
static uint32_t frame_count=0;
extern volatile uint32_t system_ticks;
static uint32_t last_tick=0,last_input_tick=0;
#define IDLE_TIMEOUT (18*60)

#define TBL 20
#define TBC 50
static char tl[TBL][TBC];static int tlc=0;
static char ti[TBC];static int til=0;static int tscr=0;

static void taddl(const char*t){
    if(tlc<TBL){strncpy(tl[tlc],t,TBC-1);tl[tlc][TBC-1]='\0';tlc++;}
    else{for(int i=0;i<TBL-1;i++)strcpy(tl[i],tl[i+1]);strncpy(tl[TBL-1],t,TBC-1);tl[TBL-1][TBC-1]='\0';}
    screen_dirty=true;
}

static void tproc(void){
    char e[TBC+4];strcpy(e,"> ");strcat(e,ti);taddl(e);
    if(!strcmp(ti,"help"))taddl("help clear about date mem ldd ldconfig dlsym xinfo");
    else if(!strcmp(ti,"help net"))taddl("ifconfig ping netstat arp dns wget browse");
    else if(!strcmp(ti,"help srv"))taddl("dhcp ntp httpd [stop] rshell [stop]");
    else if(!strcmp(ti,"help dl"))taddl("ldd ldconfig dlopen dlsym");
    else if(!strcmp(ti,"help x11"))taddl("xinfo xdemo");
    else if(!strcmp(ti,"clear"))tlc=0;
    else if(!strcmp(ti,"about"))taddl("NexusOS v3.3.0 - Phase 33");
    else if(!strcmp(ti,"date")){rtc_time_t t;rtc_read(&t);char s[16];rtc_format_time(&t,s);taddl(s);}
    else if(!strcmp(ti,"mem")){char b[30],n[12];strcpy(b,"Free: ");int_to_str(pmm_get_free_pages()*4,n);strcat(b,n);strcat(b," KB");taddl(b);}
    else if(!strncmp(ti,"theme ",6))taddl(theme_set_by_name(ti+6)?"Changed.":"Unknown.");
    else if(!strcmp(ti,"exit"))taddl("Use Esc.");
    else if(!strcmp(ti,"ifconfig")){
        const net_config_t*cfg=ip_get_config();char b[40],ip[16];
        int cnt=net_device_count();
        if(cnt>0){net_device_t*d=net_get_device(0);char ms[18];net_mac_to_string(&d->mac,ms);
            strcpy(b,d->name);strcat(b,d->link_up?" UP":" DOWN");taddl(b);
            strcpy(b,"MAC: ");strcat(b,ms);taddl(b);}
        ip_to_string(cfg->ip,ip);strcpy(b,"IP:  ");strcat(b,ip);taddl(b);
        ip_to_string(cfg->gateway,ip);strcpy(b,"GW:  ");strcat(b,ip);taddl(b);
        ip_to_string(cfg->dns,ip);strcpy(b,"DNS: ");strcat(b,ip);taddl(b);
    }
    else if(!strncmp(ti,"ping ",5)){
        uint32_t tgt=ip_parse(ti+5);
        if(!tgt){taddl("Invalid IP.");}
        else{
            char ip[16];ip_to_string(tgt,ip);
            char b[TBC];strcpy(b,"PING ");strcat(b,ip);taddl(b);
            ping_state_t*ps=icmp_get_ping_state();
            ps->active=true;ps->target_ip=tgt;ps->id=(uint16_t)(system_ticks&0xFFFF);
            ps->seq_next=1;ps->replies=0;ps->sent=0;ps->got_reply=false;
            for(int i=0;i<4;i++){
                ps->got_reply=false;icmp_send_echo(tgt,ps->id,ps->seq_next++);
                uint32_t st=system_ticks;while(!ps->got_reply&&(system_ticks-st)<36){net_poll();__asm__ volatile("hlt");}
                if(ps->got_reply){char n[12];uint32_t rtt=(ps->last_rtt_ticks*1000)/18;
                    strcpy(b,"Reply: time=");int_to_str(rtt,n);strcat(b,n);strcat(b,"ms");taddl(b);
                }else taddl("Timeout.");
            }
            char n[12];strcpy(b,"");int_to_str(ps->replies,n);strcat(b,n);strcat(b,"/");
            int_to_str(ps->sent,n);strcat(b,n);strcat(b," received");taddl(b);
            ps->active=false;
        }
    }
    else if(!strcmp(ti,"netstat")){
        int tc;const tcp_conn_t*conns=tcp_get_connections(&tc);bool any=false;
        for(int i=0;i<tc;i++){if(!conns[i].active)continue;any=true;
            char b[TBC],n[12],ip[16];strcpy(b,"TCP ");
            int_to_str(conns[i].local_port,n);strcat(b,n);strcat(b," -> ");
            ip_to_string(conns[i].remote_ip,ip);strcat(b,ip);strcat(b,":");
            int_to_str(conns[i].remote_port,n);strcat(b,n);strcat(b," ");
            strcat(b,tcp_state_name(conns[i].state));taddl(b);
        }
        int uc;const udp_binding_t*ub=udp_get_bindings(&uc);
        for(int i=0;i<uc;i++){if(!ub[i].active)continue;any=true;
            char b[TBC],n[12];strcpy(b,"UDP port ");
            int_to_str(ub[i].port,n);strcat(b,n);strcat(b," BOUND");taddl(b);
        }
        if(!any)taddl("No connections.");
    }
    else if(!strcmp(ti,"arp")){
        int ac;const arp_entry_t*cache=arp_get_cache(&ac);bool any=false;
        for(int i=0;i<ac;i++){if(!cache[i].valid)continue;any=true;
            char b[TBC],ip[16],ms[18];
            ip_to_string(cache[i].ip,ip);net_mac_to_string(&cache[i].mac,ms);
            strcpy(b,ip);strcat(b," -> ");strcat(b,ms);taddl(b);
        }
        if(!any)taddl("Cache empty.");
    }
    else if(!strncmp(ti,"dns ",4)){
        char b[TBC];strcpy(b,"Resolving ");strcat(b,ti+4);strcat(b,"...");taddl(b);
        uint32_t ip=dns_resolve(ti+4);
        if(ip){char is[16];ip_to_string(ip,is);strcpy(b,ti+4);strcat(b," -> ");strcat(b,is);taddl(b);}
        else taddl("DNS lookup failed.");
    }
    else if(!strncmp(ti,"wget ",5)){
        char b[TBC];strcpy(b,"Fetching ");strcat(b,ti+5);strcat(b,"...");taddl(b);
        http_response_t resp=http_get(ti+5);
        if(resp.success){char n[12];strcpy(b,"HTTP ");int_to_str(resp.status_code,n);strcat(b,n);
            strcat(b," (");int_to_str(resp.body_len,n);strcat(b,n);strcat(b," bytes)");taddl(b);
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
        }else{strcpy(b,"Request failed.");if(resp.status_code>0){char n[12];strcat(b," (HTTP ");
            int_to_str(resp.status_code,n);strcat(b,n);strcat(b,")");}taddl(b);}
    }
    else if(!strncmp(ti,"browse ",7)){
        /* Save URL before clearing ti */
        char burl[TBC];strcpy(burl,ti+7);
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
            ip_to_string(c->ip,ip);strcpy(b,"IP: ");strcat(b,ip);taddl(b);
            ip_to_string(c->gateway,ip);strcpy(b,"GW: ");strcat(b,ip);taddl(b);
            ip_to_string(c->dns,ip);strcpy(b,"DNS: ");strcat(b,ip);taddl(b);
        }else taddl("DHCP failed.");
        screen_dirty=true;return;
    }
    else if(!strcmp(ti,"ntp")){
        taddl("Syncing time...");
        ti[0]='\0';til=0;screen_dirty=true;
        bool ok=ntp_sync();
        if(ok){rtc_time_t t;rtc_read(&t);char s[16];rtc_format_time(&t,s);char b[TBC];strcpy(b,"Time: ");strcat(b,s);taddl(b);}
        else taddl("NTP sync failed.");
        screen_dirty=true;return;
    }
    else if(!strcmp(ti,"httpd")){httpd_start();taddl(httpd_is_running()?"HTTPD running on :8080":"HTTPD failed.");}
    else if(!strcmp(ti,"httpd stop")){httpd_stop();taddl("HTTPD stopped.");}
    else if(!strcmp(ti,"rshell")){rshell_start();taddl(rshell_is_running()?"Remote shell on :2323":"RSHELL failed.");}
    else if(!strcmp(ti,"rshell stop")){rshell_stop();taddl("Remote shell stopped.");}
    else if(!strcmp(ti,"uname"))taddl("NexusOS v3.2.0 i386 Phase 32");
    else if(!strcmp(ti,"whoami")){const char*u=env_get("USER");taddl(u?u:"root");}
    else if(!strcmp(ti,"env")){
        const char*vars[]={"PATH","HOME","USER","SHELL","TERM","PWD",NULL};
        for(int i=0;vars[i];i++){const char*v=env_get(vars[i]);
            if(v){char b[TBC];strcpy(b,vars[i]);strcat(b,"=");strcat(b,v);taddl(b);}}
    }
    else if(!strncmp(ti,"export ",7)){
        char*eq=NULL;for(int i=7;ti[i];i++)if(ti[i]=='='){eq=&ti[i];break;}
        if(eq){*eq='\0';env_set(ti+7,eq+1);char b[TBC];strcpy(b,ti+7);strcat(b,"=");strcat(b,eq+1);taddl(b);*eq='=';}
        else taddl("Usage: export KEY=VALUE");
    }
    else if(!strncmp(ti,"cat ",4)){
        const char*f=ti+4;
        if(procfs_is_proc(f)){char pb[512];int l=procfs_read(f,pb,sizeof(pb));if(l>0)taddl(pb);else taddl("Not found.");}
        else{fs_node_t*n=vfs_finddir(vfs_get_root(),f);
            if(n){char b[TBC];int rd=vfs_read(n,0,n->size<(uint32_t)(TBC-1)?n->size:(TBC-1),(uint8_t*)b);b[rd]='\0';taddl(b);}
            else taddl("Not found.");}
    }
    else if(!strncmp(ti,"wc ",3)){
        const char*f=ti+3;char buf[512];int len;
        if(procfs_is_proc(f)){len=procfs_read(f,buf,sizeof(buf));}
        else{fs_node_t*n=vfs_finddir(vfs_get_root(),f);
            if(!n){taddl("Not found.");}
            else{len=vfs_read(n,0,n->size<511?n->size:511,(uint8_t*)buf);buf[len]='\0';}}
        if(len>0){int li=0,w=0;bool iw=false;
            for(int i=0;i<len;i++){if(buf[i]=='\n')li++;if(buf[i]==' '||buf[i]=='\n')iw=false;else if(!iw){iw=true;w++;}}
            char r[TBC],nb[12];strcpy(r,"");int_to_str(li,nb);strcat(r,nb);strcat(r," lines ");
            int_to_str(w,nb);strcat(r,nb);strcat(r," words ");int_to_str(len,nb);strcat(r,nb);strcat(r," chars");taddl(r);}
    }
    else if(!strncmp(ti,"grep ",5)){
        char*sp=NULL;for(int i=5;ti[i];i++)if(ti[i]==' '){sp=&ti[i];break;}
        if(!sp){taddl("Usage: grep <pat> <file>");}
        else{*sp='\0';const char*pat=ti+5;const char*fn=sp+1;
            char buf[512];int len;
            if(procfs_is_proc(fn)){len=procfs_read(fn,buf,sizeof(buf));}
            else{fs_node_t*n=vfs_finddir(vfs_get_root(),fn);
                if(!n){taddl("Not found.");len=-1;}
                else{len=vfs_read(n,0,n->size<511?n->size:511,(uint8_t*)buf);buf[len]='\0';}}
            if(len>0){char*p=buf;bool any=false;
                while(*p){char line[TBC];int li=0;while(*p&&*p!='\n'&&li<TBC-1)line[li++]=*p++;line[li]='\0';if(*p=='\n')p++;
                    if(strstr(line,pat)){taddl(line);any=true;}}
                if(!any)taddl("No matches.");}
            *sp=' ';}
    }
    else if(!strcmp(ti,"ldd"))taddl("Dynamic linker: 26 builtins, 0 libs loaded.");
    else if(!strcmp(ti,"ldconfig"))taddl("26 libc symbols registered (use shell for details).");
    else if(!strncmp(ti,"dlopen ",7))taddl("dlopen: use text shell (Esc) for full dl commands.");
    else if(!strncmp(ti,"dlsym ",6))taddl("dlsym: use text shell (Esc) for full dl commands.");
    else if(!strcmp(ti,"xinfo"))taddl("X11 shim: NexusOS X11 v1.0 (use shell for xdemo).");
    else if(!strcmp(ti,"xdemo"))taddl("xdemo: use text shell (Esc) to launch X11 demo.");
    else if(ti[0])taddl("Unknown command. Try 'help'");
    ti[0]='\0';til=0;screen_dirty=true;
}

static void about_draw(int id,int cx,int cy,int cw,int ch){
    (void)id;(void)cw;const theme_t*t=theme_get();uint8_t tc=t->win_content,bg=(tc>>4)&0xF;int y=cy;
    if(y<cy+ch)gui_draw_text(cx+1,y++,"\x0F NexusOS \x0F",VGA_COLOR(VGA_LIGHT_CYAN,bg));
    if(y<cy+ch){for(int i=0;i<cw-2;i++)gui_putchar(cx+1+i,y,(char)0xC4,VGA_COLOR(VGA_DARK_GREY,bg));y++;}
    if(y<cy+ch)gui_draw_text(cx+1,y++,"Version 1.3.0",VGA_COLOR(VGA_WHITE,bg));
    if(y<cy+ch)gui_draw_text(cx+1,y++,"Phase 13: UX Supremacy",VGA_COLOR(VGA_YELLOW,bg));
    if(y<cy+ch)y++;
    if(y<cy+ch)gui_draw_text(cx+1,y++,"The Hybrid Operating System",tc);
    if(y<cy+ch)gui_draw_text(cx+1,y++,"Built from scratch in C & ASM",tc);
    if(y<cy+ch)y++;
    if(y<cy+ch)gui_draw_text(cx+1,y++,"\xFB 30+ Applications",VGA_COLOR(VGA_LIGHT_GREEN,bg));
    if(y<cy+ch)gui_draw_text(cx+1,y++,"\xFB App Dock & Notif Center",VGA_COLOR(VGA_LIGHT_GREEN,bg));
    if(y<cy+ch)gui_draw_text(cx+1,y++,"\xFB Clipboard History",VGA_COLOR(VGA_LIGHT_GREEN,bg));
    if(y<cy+ch)gui_draw_text(cx+1,y++,"\xFB Keyboard Shortcuts",VGA_COLOR(VGA_LIGHT_GREEN,bg));
    if(y<cy+ch)gui_draw_text(cx+1,y++,"\xFB Power Management",VGA_COLOR(VGA_LIGHT_GREEN,bg));
}
static void about_key(int id,char key){(void)id;(void)key;}

static void term_draw(int id,int cx,int cy,int cw,int ch){
    (void)id;const theme_t*t=theme_get();uint8_t tc=t->win_content,bg=(tc>>4)&0xF;
    int vis=ch-1,st=tlc-vis-tscr;if(st<0)st=0;
    for(int i=0;i<vis&&(st+i)<tlc;i++){int j=0;while(tl[st+i][j]&&j<cw-1){gui_putchar(cx+j,cy+i,tl[st+i][j],tc);j++;}}
    int ir=cy+ch-1;gui_putchar(cx,ir,'>',VGA_COLOR(VGA_LIGHT_GREEN,bg));
    for(int i=0;i<til&&i<cw-3;i++)gui_putchar(cx+2+i,ir,ti[i],tc);
    if(til<cw-3)gui_putchar(cx+2+til,ir,(frame_count%8<4)?'_':' ',VGA_COLOR(VGA_WHITE,bg));
}
static void term_key(int id,char key){
    (void)id;
    if(key=='\n')tproc();
    else if(key=='\b'){if(til>0)ti[--til]='\0';}
    else if(key==22){const char*c=clipboard_paste();if(c){clipmgr_record(c);while(*c&&til<TBC-1){ti[til++]=*c++;}ti[til]='\0';}}
    else if(key>=32&&key<127&&til<TBC-1){ti[til++]=key;ti[til]='\0';}
    screen_dirty=true;
}

static void open_about(void){window_create("About NexusOS",25,4,30,18,about_draw,about_key);syslog_add("About");notifcenter_add("Opened About",'\x01');screen_dirty=true;}
static void open_terminal(void){
    tlc=0;til=0;ti[0]='\0';tscr=0;
    taddl("NexusOS Terminal v1.3.0");taddl("Type 'help' for commands.");taddl("");
    window_create("Terminal",16,3,42,17,term_draw,term_key);syslog_add("Terminal");notifcenter_add("Terminal opened",'\xB2');screen_dirty=true;
}

static void handle_action(int a){
    switch(a){
        case 1:open_terminal();break;
        case 2:filemgr_open();syslog_add("Files");notifcenter_add("File Manager",'\xE8');break;
        case 3:calculator_open();syslog_add("Calc");break;
        case 4:sysmon_open();syslog_add("SysMon");break;
        case 5:settings_open();syslog_add("Settings");break;
        case 6:break;
        case 7:settings_open();break;
        case 8:lockscreen_run();last_input_tick=system_ticks;break;
        case 9:theme_cycle();syslog_add("Theme");notifcenter_add("Theme changed",'\xFE');break;
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
        case 22:search_open();syslog_add("Search");break;
        case 23:tetris_open();syslog_add("Tetris");break;
        case 24:hexview_open();syslog_add("HexView");break;
        case 25:contacts_open();syslog_add("Contacts");break;
        case 26:colorpick_open();syslog_add("ColorPick");break;
        case 27:syslog_open();break;
        case 28:clipmgr_open();syslog_add("ClipMgr");break;
        case 29:appearance_open();syslog_add("Appearance");break;
        case 30:power_menu_open();break;
        case 31:fileops_open();syslog_add("FileOps");break;
        case 32:browser_open("http://example.com");syslog_add("Browser");notifcenter_add("Web Browser",'\xEB');screen_dirty=true;break;
    }
    screen_dirty=true;
}

static void handle_ctx(int a){
    switch(a){
        case CMENU_NEW_FILE:{static int n=0;char nm[32],nb[8];strcpy(nm,"new_file");int_to_str(n++,nb);strcat(nm,nb);strcat(nm,".txt");ramfs_create(nm,FS_FILE);syslog_add("Created file");notifcenter_add("File created",'\xE8');break;}
        case CMENU_REFRESH:syslog_add("Refreshed");break;
        case CMENU_THEME_NEXT:theme_cycle();syslog_add("Theme");break;
        case CMENU_ABOUT:open_about();break;
        case CMENU_SETTINGS:settings_open();syslog_add("Settings");break;
    }
    screen_dirty=true;
}

static const uint8_t cursor_bitmap[18][12] = {
    {1,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0,0},
    {1,2,2,2,2,2,2,2,2,1,0,0},
    {1,2,2,2,2,2,2,2,2,2,1,0},
    {1,2,2,2,2,2,2,1,1,1,1,0},
    {1,2,2,1,2,2,1,0,0,0,0,0},
    {1,2,1,0,1,2,2,1,0,0,0,0},
    {1,1,0,0,1,2,2,1,0,0,0,0},
    {0,0,0,0,0,1,2,2,1,0,0,0},
    {0,0,0,0,0,1,2,2,1,0,0,0},
    {0,0,0,0,0,0,1,1,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0}
};

static void dcursor(int mx, int my, int px, int py){
    if (fb_is_vesa()) {
        for (int y = 0; y < 18; y++) {
            for (int x = 0; x < 12; x++) {
                if (cursor_bitmap[y][x] == 1) {
                    fb_putpixel(px + x, py + y, 0xFFFFFF);
                } else if (cursor_bitmap[y][x] == 2) {
                    fb_putpixel(px + x, py + y, 0x000000);
                }
            }
        }
    } else {
        if(mx>=0&&mx<GUI_WIDTH&&my>=0&&my<GUI_HEIGHT){
            uint16_t e=gui_getchar(mx,my);uint8_t ch=e&0xFF,co=(e>>8)&0xFF;
            gui_putchar(mx,my,ch?ch:' ',VGA_COLOR((co>>4)&0xF,co&0xF));
        }
    }
}

void desktop_run(void){
    gui_init();icons_init();workspaces_init();recycle_init();syslog_init();dock_init();
    /* Phase 14: Set pixel wallpaper as default in VESA mode */
    if (gui_is_vesa()) { wallpaper_set(WP_PHOTO); }
    syslog_add("NexusOS v1.7 booted");notifcenter_add("System started",'\x0F');
    open_terminal();
    bool drag=false;int dwn=-1,dox=0,doy=0;
    bool pl=false,pr=false;int pmx=-1,pmy=-1;
    port_byte_out(0x3D4,0x0A);port_byte_out(0x3D5,0x20);
    start_menu_close();context_menu_close();palette_close();
    last_tick=system_ticks;last_input_tick=system_ticks;
    screen_dirty=true;frame_count=0;notify_push("NexusOS v1.3");

    bool run=true;
    while(run){
        bool inp=false;
        mouse_state_t ms=mouse_get_state();
        if(ms.x!=pmx||ms.y!=pmy){inp=true;pmx=ms.x;pmy=ms.y;}
        bool ln=(ms.buttons&MOUSE_LEFT)!=0,rn=(ms.buttons&MOUSE_RIGHT)!=0;
        bool lc=ln&&!pl,lr=!ln&&pl,rc=rn&&!pr;

        if(lc){inp=true;
            if(shortcuts_is_open()){shortcuts_close();screen_dirty=true;}
            else if(power_menu_is_open()){power_menu_close();screen_dirty=true;}
            else if(notifcenter_is_open()){notifcenter_toggle();screen_dirty=true;}
            else if(palette_is_open())palette_close();
            else if(switcher_is_open())switcher_close();
            else if(context_menu_is_open()&&context_menu_hit(ms.x,ms.y)){int a=context_menu_handle_click(ms.x,ms.y);if(a)handle_ctx(a);}
            else if(context_menu_is_open())context_menu_close();
            else if(start_menu_is_open()&&start_menu_hit(ms.x,ms.y)){int a=start_menu_handle_click(ms.x,ms.y);if(a)handle_action(a);}
            else if(dock_hit(ms.x,ms.y)){int a=dock_handle_click(ms.x);if(a)handle_action(a);}
            else if(taskbar_hit(ms.x,ms.y)){
                int tr=taskbar_tray_hit(ms.x);
                if(tr==3){lockscreen_run();last_input_tick=system_ticks;}
                else if(tr==2)notifcenter_toggle();
                else if(tr==1){notifcenter_toggle();}
                else if(taskbar_start_hit(ms.x))start_menu_toggle();
                else{start_menu_close();int w=taskbar_handle_click(ms.x);if(w>=0){window_t*ww=window_get(w);if(ww&&(ww->flags&WIN_MINIMIZED))window_restore(w);else window_focus(w);}}
            }else{
                start_menu_close();
                int h = fb_is_vesa() ? window_hit_test_px(ms.px, ms.py) : window_hit_test(ms.x, ms.y);
                if(h>=0){window_focus(h);
                    if(fb_is_vesa() ? window_close_hit_px(h,ms.px,ms.py) : window_close_hit(h,ms.x,ms.y))window_destroy(h);
                    else if(fb_is_vesa() ? window_minimize_hit_px(h,ms.px,ms.py) : window_minimize_hit(h,ms.x,ms.y))window_minimize(h);
                    else if(fb_is_vesa() ? window_maximize_hit_px(h,ms.px,ms.py) : window_maximize_hit(h,ms.x,ms.y))window_maximize(h);
                    else if(fb_is_vesa() ? window_titlebar_hit_px(h,ms.px,ms.py) : window_titlebar_hit(h,ms.x,ms.y)){
                        window_t*w=window_get(h);
                        if(w&&(w->flags&WIN_MOVABLE)&&!(w->flags&WIN_MAXIMIZED)){
                            drag=true;dwn=h;
                            if(fb_is_vesa()){ dox=ms.px-w->px; doy=ms.py-w->py; }
                            else { dox=ms.x-w->x; doy=ms.y-w->y; }
                        }
                    }
                }else{int ia=icons_handle_click(ms.x,ms.y);if(ia)handle_action(ia);}
            }
        }
        if(rc){inp=true;
            bool hit = fb_is_vesa() ? (window_hit_test_px(ms.px, ms.py) >= 0) : (window_hit_test(ms.x, ms.y) >= 0);
            if(!taskbar_hit(ms.x,ms.y) && !hit){
                start_menu_close();context_menu_open(ms.x,ms.y);
            }
        }
        if(drag&&ln){
            if(fb_is_vesa()) window_move_px(dwn,ms.px-dox,ms.py-doy);
            else window_move(dwn,ms.x-dox,ms.y-doy);
            inp=true;
        }
        if(lr){drag=false;dwn=-1;}
        pl=ln;pr=rn;mouse_clear_events();

        while(keyboard_has_key()){
            char key=keyboard_getchar();inp=true;
            if(shortcuts_is_open()){shortcuts_handle_key(key);screen_dirty=true;continue;}
            if(power_menu_is_open()){
                int pa=power_menu_handle_key(key);
                if(pa==POWER_SHUTDOWN){syslog_add("Shutdown");power_shutdown();}
                if(pa==POWER_RESTART){syslog_add("Restart");power_restart();}
                if(pa==POWER_LOCK){lockscreen_run();last_input_tick=system_ticks;}
                screen_dirty=true;continue;
            }
            if(notifcenter_is_open()){notifcenter_handle_key(key);screen_dirty=true;continue;}
            if(palette_is_open()){int a=palette_handle_key(key);if(a)handle_action(a);screen_dirty=true;continue;}
            if(switcher_is_open()){if(key=='\t')switcher_next();else if(key=='\n')switcher_close();else if(key==27)switcher_cancel();screen_dirty=true;continue;}
            if(context_menu_is_open()){if(key==27)context_menu_close();else{int a=context_menu_handle_key(key);if(a)handle_ctx(a);}screen_dirty=true;continue;}
            if(start_menu_is_open()){if(key==27)start_menu_close();else{int a=start_menu_handle_key(key);if(a)handle_action(a);}screen_dirty=true;continue;}

            if(key==27){run=false;beep_stop();break;}
            if(key=='\t'){switcher_open();screen_dirty=true;continue;}
            if(key==16){palette_open();screen_dirty=true;continue;}
            if(key==17){power_menu_open();screen_dirty=true;continue;}
            if((unsigned char)key==0x3B){shortcuts_open();screen_dirty=true;continue;} /* F1 */
            if(key==1){open_about();continue;}
            if(key==20){open_terminal();continue;}
            if(key==13){start_menu_toggle();screen_dirty=true;continue;}
            if(key==6){filemgr_open();syslog_add("Files");screen_dirty=true;continue;}
            if(key==11){calculator_open();syslog_add("Calc");screen_dirty=true;continue;}
            if(key==19){sysmon_open();syslog_add("SysMon");screen_dirty=true;continue;}
            if(key==14){notifcenter_toggle();screen_dirty=true;continue;} /* Ctrl+N = notif center */
            if(key==12){lockscreen_run();screen_dirty=true;last_input_tick=system_ticks;continue;}
            if(key==2){notepad_open();syslog_add("Notepad");screen_dirty=true;continue;}
            if(key==7){taskmgr_open();syslog_add("TaskMgr");screen_dirty=true;continue;}
            if(key==4){calendar_open();syslog_add("Calendar");screen_dirty=true;continue;}
            if((unsigned char)key==0x3C){help_open();syslog_add("Help");screen_dirty=true;continue;} /* F2 */
            if(key==9){widgets_toggle();screen_dirty=true;continue;}
            if(key==23){int f=window_get_focused();if(f>=0)window_destroy(f);screen_dirty=true;continue;}
            if(key==22){const char*c=clipboard_paste();if(c)clipmgr_record(c);window_send_key(key);screen_dirty=true;continue;}

            if((unsigned char)key==0x88){workspaces_switch(0);syslog_add("Desktop 1");notifcenter_add("Desktop 1",'\x0F');screen_dirty=true;continue;}
            if((unsigned char)key==0x89){workspaces_switch(1);syslog_add("Desktop 2");notifcenter_add("Desktop 2",'\x0F');screen_dirty=true;continue;}
            if((unsigned char)key==0x8A){workspaces_switch(2);syslog_add("Desktop 3");screen_dirty=true;continue;}
            if((unsigned char)key==0x8B){workspaces_switch(3);syslog_add("Desktop 4");screen_dirty=true;continue;}
            if((unsigned char)key==0x84){int f=window_get_focused();if(f>=0)window_snap_left(f);screen_dirty=true;continue;}
            if((unsigned char)key==0x85){int f=window_get_focused();if(f>=0)window_snap_right(f);screen_dirty=true;continue;}
            if((unsigned char)key==0x86){int f=window_get_focused();if(f>=0)window_maximize(f);screen_dirty=true;continue;}
            if((unsigned char)key==0x87){int f=window_get_focused();if(f>=0)window_restore(f);screen_dirty=true;continue;}

            if(window_get_focused()<0){int ia=icons_handle_key(key);if(ia)handle_action(ia);}
            else window_send_key(key);
        }

        if(inp){screen_dirty=true;last_input_tick=system_ticks;}
        if(system_ticks-last_tick>=18){last_tick=system_ticks;screen_dirty=true;frame_count++;notify_update();}
        if(system_ticks-last_input_tick>=IDLE_TIMEOUT){screensaver_run();last_input_tick=system_ticks;screen_dirty=true;}

        if(screen_dirty){
            wallpaper_draw();
            dock_draw();
            icons_draw();
            if(widgets_visible())widgets_draw();
            window_draw_all();taskbar_draw();
            if(start_menu_is_open())start_menu_draw();
            if(context_menu_is_open())context_menu_draw();
            if(switcher_is_open())switcher_draw();
            if(palette_is_open())palette_draw();
            if(power_menu_is_open())power_menu_draw();
            if(notifcenter_is_open())notifcenter_draw();
            if(shortcuts_is_open())shortcuts_draw();
            notify_draw();dcursor(ms.x,ms.y,ms.px,ms.py);
            gui_flip();screen_dirty=false;
        }
        __asm__ volatile("hlt");
    }

    syslog_add("Desktop exit");
    start_menu_close();context_menu_close();palette_close();beep_stop();
    for(int i=0;i<MAX_WINDOWS;i++)window_destroy(i);
    port_byte_out(0x3D4,0x0A);port_byte_out(0x3D5,14);
    port_byte_out(0x3D4,0x0B);port_byte_out(0x3D5,15);
    vga_init();
}
