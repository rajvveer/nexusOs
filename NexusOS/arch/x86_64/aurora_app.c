/* Aurora applications use their original drawing callbacks in isolated processes. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "xdg-shell-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "aurora_graphics.h"
#include "aurora_extra.h"
#include "aurora_dialogs.h"
#include "aurora_authdialog.h"
#include "aurora_a11y_client.h"
#include "aurora_clipboard_app.h"
#include "aurora_notifications.h"
int aurora_network_open(void); int aurora_network_apply(void); bool aurora_network_tick(void);   /* aurora_network.c */
int aurora_wifi_cli(int argc,char** argv);   /* aurora_wifi.c: aurora-app network --wifi ... */
#include "../../kernel/window.h"
#include "../../kernel/macui.h"
#include "../../kernel/appui.h"
#include "../../kernel/calculator.h"
#include "../../kernel/calendar.h"
#include "../../kernel/paint.h"
#include "../../kernel/minesweeper.h"

volatile uint32_t system_ticks;
static window_t window;
static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_keyboard *keyboard;
static struct wl_pointer *pointer;
static struct xdg_wm_base *shell;
static struct wl_surface *surface;
static struct xdg_surface *xdg;
static struct xdg_toplevel *top;
static struct wl_buffer *buffer;
static struct xkb_context *key_context;
static struct xkb_keymap *key_map;
static struct xkb_state *key_state;
static uint32_t *pixels;
static size_t buffer_size;
static int width,height,buffer_width,buffer_height,mouse_x,mouse_y;
/* HiDPI: the scale (1/120) to render at: wp_fractional_scale_v1's preferred scale (drawn at the physical size, shown
 * through a wp_viewport at the logical size), else the largest wl_output scale the surface is on (buffer scale). */
static struct wp_fractional_scale_manager_v1 *fractional_manager;static struct wp_fractional_scale_v1 *fractional;
static struct wp_viewporter *viewporter;static struct wp_viewport *viewport;
static struct { struct wl_output *output;int scale;bool entered; } outputs[8];static int output_count,preferred_scale;
static int render_scale(void){
    if(preferred_scale>=120&&preferred_scale<=480&&viewport)return preferred_scale;
    int k=1;for(int i=0;i<output_count;i++)if(outputs[i].entered&&outputs[i].scale>k)k=outputs[i].scale;
    if(preferred_scale>120&&!viewport)k=(preferred_scale+119)/120;
    return k>4?480:k*120;
}
static int buffer_scale_used=120;static const struct wl_output_listener output_listener;
static bool busy,configured,closed,redraw=true,maximized,focused=true;
static const char *app;
static uint32_t repeating_key;
static int repeat_rate,repeat_delay;
static uint64_t repeat_at;
static bool traffic_hover(void){return mouse_y>=5&&mouse_y<23&&mouse_x>=6&&mouse_x<68;}
static void (*drag_hook)(int x,int y,int phase);static bool dragging;
void aurora_app_set_drag(void (*drag)(int x,int y,int phase)){drag_hook=drag;}
void fb_flip(void){redraw=true;}
void fb_flip_full(void){redraw=true;}
void fb_flip_legacy(void){redraw=true;}
void fb_flip_region(int x,int y,int w,int h){(void)x;(void)y;(void)w;(void)h;redraw=true;}
void fb_mark_dirty(int x,int y,int w,int h){(void)x;(void)y;(void)w;(void)h;redraw=true;}
void fb_mark_dirty_all(void){redraw=true;}
void aurora_child_disconnect(void){if(display)close(wl_display_get_fd(display));}

static uint64_t milliseconds(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
int window_create(const char *title,int x,int y,int w,int h,win_draw_fn draw,win_key_fn key){
    (void)x;(void)y;if(window.active)return -1;
    window=(window_t){.active=true,.id=0,.w=w,.h=h,.pw=w*8,.ph=h*16,.flags=WIN_VISIBLE|WIN_FOCUSED|WIN_MOVABLE|WIN_CLOSABLE|WIN_RESIZABLE,.draw=draw,.on_key=key};
    snprintf(window.title,sizeof(window.title),"%s",title);width=window.pw;height=window.ph;return 0;
}
window_t *window_get(int id){return id==0&&window.active?&window:NULL;}
int window_get_focused(void){return focused&&window.active?0:-1;}
int window_count(void){return window.active?1:0;}
void window_set_on_click(int id,win_click_fn click){if(window_get(id))window.on_click=click;}
bool window_content_px(int id,int *x,int *y,int *w,int *h){if(!window_get(id))return false;if(x)*x=1;if(y)*y=WIN_TITLEBAR_H+1;if(w)*w=width-2;if(h)*h=height-WIN_TITLEBAR_H-2;return true;}
void window_send_key(char key){if(window.on_key)window.on_key(0,key);redraw=true;}
void window_send_click(int id,int x,int y){if(window_get(id)&&window.on_click)window.on_click(id,x,y);redraw=true;}
void window_destroy(int id){if(window_get(id)&&aurora_extra_close())closed=true;redraw=true;}
void window_minimize(int id){if(window_get(id)&&top)xdg_toplevel_set_minimized(top);}
void window_maximize(int id){if(window_get(id)&&top)xdg_toplevel_set_maximized(top);}
void window_restore(int id){if(window_get(id)&&top)xdg_toplevel_unset_maximized(top);}
static void release_buffer(void *data,struct wl_buffer *b){(void)data;(void)b;busy=false;}
static const struct wl_buffer_listener buffer_listener={release_buffer};
static bool make_buffer(void){
    int scale=render_scale(),bw=(int)(((int64_t)width*scale+119)/120),bh=(int)(((int64_t)height*scale+119)/120);
    if(buffer&&buffer_width==bw&&buffer_height==bh&&buffer_scale_used==scale)return true;
    if(buffer){wl_buffer_destroy(buffer);munmap(pixels,buffer_size);buffer=NULL;pixels=NULL;buffer_size=0;}
    if(width<120||height<80||(uint64_t)bw*bh>INT32_MAX/4)return false;
    buffer_size=(size_t)bw*bh*4;int fd=memfd_create("aurora-app",MFD_CLOEXEC);
    if(fd<0||ftruncate(fd,buffer_size)){if(fd>=0)close(fd);return false;}
    pixels=mmap(NULL,buffer_size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    if(pixels==MAP_FAILED){pixels=NULL;close(fd);return false;}
    struct wl_shm_pool *pool=wl_shm_create_pool(shm,fd,buffer_size);
    if(!pool){close(fd);munmap(pixels,buffer_size);pixels=NULL;return false;}
    buffer=wl_shm_pool_create_buffer(pool,0,bw,bh,bw*4,WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);close(fd);if(!buffer){munmap(pixels,buffer_size);pixels=NULL;return false;}
    wl_buffer_add_listener(buffer,&buffer_listener,NULL);buffer_width=bw;buffer_height=bh;buffer_scale_used=scale;
    if(viewport)wp_viewport_set_destination(viewport,width,height);else wl_surface_set_buffer_scale(surface,scale/120);
    aurora_canvas_scaled(pixels,bw,bh,scale);gui_init();
    if(scale!=120){printf("[64] AURORA APP SCALE app=%s scale=%d buffer=%dx%d size=%dx%d\n",app,scale,bw,bh,width,height);fflush(stdout);}
    return true;
}
static void paint(void){
    if(!configured||closed||busy||!redraw)return;
    if(!make_buffer()){perror("Draw Aurora window");closed=true;return;}
    if(viewport)wp_viewport_set_destination(viewport,width,height);
    window.w=width/8;window.h=height/16;window.pw=width;window.ph=height;
    ui_blend_rect(0,0,width,height,APPUI_BG,255);
    aurora_a11y_paint_begin();if(window.draw)window.draw(0,1,2,window.w-2,window.h-3);aurora_a11y_paint_end(app,window.title,focused);   /* the screen reader's view (aurora_a11y_client.h) */
    ui_blend_rect(0,0,width,WIN_TITLEBAR_H,focused?0x36363b:0x29292e,255);
    ui_blend_rect(0,WIN_TITLEBAR_H,width,1,0x121216,255);
    ui_rrect_stroke(0,0,width,height,1,0x707078,120);
    ui_text_center(width/2,5,window.title,UF_UIB,focused?0xf3f3f5:0x9999a0);
    const uint32_t colors[]={0xff5f57,0xfebc2e,0x28c840};
    for(int i=0;i<3;i++)ui_circle(15+i*21,14,6,colors[i],255);
    if(traffic_hover()){
        ui_line(12,11,18,17,1,0x65231f,255);ui_line(18,11,12,17,1,0x65231f,255);
        ui_line(33,14,39,14,1,0x6d4b0c,255);ui_line(54,14,60,14,1,0x10561a,255);ui_line(57,11,57,17,1,0x10561a,255);
    }
    wl_surface_attach(surface,buffer,0,0);wl_surface_damage_buffer(surface,0,0,buffer_width,buffer_height);wl_surface_commit(surface);busy=true;redraw=false;
}
static void configure_surface(void *d,struct xdg_surface *s,uint32_t serial){(void)d;xdg_surface_ack_configure(s,serial);configured=true;redraw=true;}
static const struct xdg_surface_listener xdg_listener={configure_surface};
static void configure_top(void *d,struct xdg_toplevel *t,int32_t w,int32_t h,struct wl_array *states){
    (void)d;(void)t;maximized=false;uint32_t *state;wl_array_for_each(state,states)if(*state==XDG_TOPLEVEL_STATE_MAXIMIZED)maximized=true;
    if(w>0)width=w;if(h>0)height=h;redraw=true;
}
static void close_top(void *d,struct xdg_toplevel *t){(void)d;(void)t;window_destroy(0);}
static const struct xdg_toplevel_listener top_listener={.configure=configure_top,.close=close_top};
static void ping(void *d,struct xdg_wm_base *base,uint32_t serial){(void)d;xdg_wm_base_pong(base,serial);}
static const struct xdg_wm_base_listener shell_listener={ping};
static void keymap(void *d,struct wl_keyboard *k,uint32_t format,int fd,uint32_t size){
    (void)d;(void)k;if(format!=WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1||!size){close(fd);return;}
    char *data=mmap(NULL,size,PROT_READ,MAP_PRIVATE,fd,0);close(fd);if(data==MAP_FAILED)return;
    struct xkb_keymap *map=data[size-1]==0?xkb_keymap_new_from_string(key_context,data,XKB_KEYMAP_FORMAT_TEXT_V1,0):NULL;munmap(data,size);if(!map)return;
    struct xkb_state *state=xkb_state_new(map);if(!state){xkb_keymap_unref(map);return;}
    if(key_state)xkb_state_unref(key_state);if(key_map)xkb_keymap_unref(key_map);key_map=map;key_state=state;
}
static void key_enter(void *d,struct wl_keyboard *k,uint32_t serial,struct wl_surface *s,struct wl_array *keys){(void)d;(void)k;(void)serial;(void)s;(void)keys;focused=true;redraw=true;}
static void key_leave(void *d,struct wl_keyboard *k,uint32_t serial,struct wl_surface *s){(void)d;(void)k;(void)serial;(void)s;focused=false;repeat_at=0;redraw=true;}
static void deliver_key(uint32_t code){
    if(closed||!key_state)return;
    xkb_keysym_t sym=xkb_state_key_get_one_sym(key_state,code+8);char key=0;
    if(sym==XKB_KEY_Up)key=(char)0x80;else if(sym==XKB_KEY_Down)key=(char)0x81;
    else if(sym==XKB_KEY_Left)key=(char)0x82;else if(sym==XKB_KEY_Right)key=(char)0x83;
    else if(sym==XKB_KEY_Home)key=(char)0x84;else if(sym==XKB_KEY_End)key=(char)0x85;
    else if(sym==XKB_KEY_Delete)key=127;else if(sym==XKB_KEY_Return||sym==XKB_KEY_KP_Enter)key='\n';
    else if(sym==XKB_KEY_BackSpace)key='\b';else if(sym==XKB_KEY_Page_Up){aurora_extra_scroll(-10);redraw=true;return;}
    else if(sym==XKB_KEY_Page_Down){aurora_extra_scroll(10);redraw=true;return;}
    else{uint32_t ch=xkb_state_key_get_utf32(key_state,code+8);if(ch&&ch<128)key=(char)ch;}
    if(key)window_send_key(key);
}
static void key_event(void *d,struct wl_keyboard *k,uint32_t serial,uint32_t time,uint32_t code,uint32_t state){
    (void)d;(void)k;(void)serial;(void)time;
    if(!state){if(code==repeating_key)repeat_at=0;return;}
    deliver_key(code);if(key_map&&repeat_rate>0&&xkb_keymap_key_repeats(key_map,code+8)){repeating_key=code;repeat_at=milliseconds()+(uint64_t)repeat_delay;}
}
static void modifiers(void *d,struct wl_keyboard *k,uint32_t serial,uint32_t depressed,uint32_t latched,uint32_t locked,uint32_t group){(void)d;(void)k;(void)serial;if(key_state)xkb_state_update_mask(key_state,depressed,latched,locked,0,0,group);}
static void repeat(void *d,struct wl_keyboard *k,int32_t rate,int32_t delay){(void)d;(void)k;repeat_rate=rate>0?(rate>1000?1000:rate):0;repeat_delay=delay>0?delay:0;if(!repeat_rate)repeat_at=0;}
static const struct wl_keyboard_listener keyboard_listener={keymap,key_enter,key_leave,key_event,modifiers,repeat};
static void pointer_enter(void *d,struct wl_pointer *p,uint32_t serial,struct wl_surface *s,wl_fixed_t x,wl_fixed_t y){(void)d;(void)p;(void)serial;(void)s;bool hover=traffic_hover();mouse_x=wl_fixed_to_int(x);mouse_y=wl_fixed_to_int(y);if(hover!=traffic_hover())redraw=true;}
static void pointer_leave(void *d,struct wl_pointer *p,uint32_t serial,struct wl_surface *s){(void)d;(void)p;(void)serial;(void)s;bool hover=traffic_hover();mouse_x=mouse_y=-1;if(hover)redraw=true;}
static void motion(void *d,struct wl_pointer *p,uint32_t time,wl_fixed_t x,wl_fixed_t y){(void)d;(void)p;(void)time;bool hover=traffic_hover();mouse_x=wl_fixed_to_int(x);mouse_y=wl_fixed_to_int(y);if(hover!=traffic_hover())redraw=true;
    if(dragging&&drag_hook){drag_hook(mouse_x-1,mouse_y-WIN_TITLEBAR_H-1,1);redraw=true;}}
static void button(void *d,struct wl_pointer *p,uint32_t serial,uint32_t time,uint32_t code,uint32_t state){
    (void)d;(void)p;(void)time;
    if(code==272&&!state&&dragging){dragging=false;if(drag_hook&&!closed){drag_hook(mouse_x-1,mouse_y-WIN_TITLEBAR_H-1,2);redraw=true;}return;}
    if(!state||code!=272||closed)return;
    if(drag_hook&&mouse_y>=WIN_TITLEBAR_H+1){dragging=true;drag_hook(mouse_x-1,mouse_y-WIN_TITLEBAR_H-1,0);}
    if(mouse_y<WIN_TITLEBAR_H){
        if(mouse_x>=6&&mouse_x<24)window_destroy(0);
        else if(mouse_x>=27&&mouse_x<45)window_minimize(0);
        else if(mouse_x>=48&&mouse_x<66){if(maximized)window_restore(0);else window_maximize(0);}
        else xdg_toplevel_move(top,seat,serial);return;
    }
    if(mouse_x>=width-7&&mouse_y>=height-7){xdg_toplevel_resize(top,seat,serial,XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT);return;}
    if(mouse_y>=WIN_TITLEBAR_H+1)window_send_click(0,mouse_x-1,mouse_y-WIN_TITLEBAR_H-1);
}
static void axis(void *d,struct wl_pointer *p,uint32_t time,uint32_t which,wl_fixed_t value){(void)d;(void)p;(void)time;if(!which){aurora_extra_scroll(value>0?3:-3);redraw=true;}}
static void pointer_frame(void *d,struct wl_pointer *p){(void)d;(void)p;}
static const struct wl_pointer_listener pointer_listener={.enter=pointer_enter,.leave=pointer_leave,.motion=motion,.button=button,.axis=axis,.frame=pointer_frame};
static void capabilities(void *d,struct wl_seat *s,uint32_t caps){(void)d;if((caps&WL_SEAT_CAPABILITY_POINTER)&&!pointer){pointer=wl_seat_get_pointer(s);wl_pointer_add_listener(pointer,&pointer_listener,NULL);}if((caps&WL_SEAT_CAPABILITY_KEYBOARD)&&!keyboard){keyboard=wl_seat_get_keyboard(s);wl_keyboard_add_listener(keyboard,&keyboard_listener,NULL);}}
static void seat_name(void *d,struct wl_seat *s,const char *name){(void)d;(void)s;(void)name;}
static const struct wl_seat_listener seat_listener={capabilities,seat_name};
static void global(void *d,struct wl_registry *registry,uint32_t id,const char *name,uint32_t version){
    (void)d;if(!strcmp(name,"wl_compositor"))compositor=wl_registry_bind(registry,id,&wl_compositor_interface,version<4?version:4);
    else if(!strcmp(name,"wl_shm"))shm=wl_registry_bind(registry,id,&wl_shm_interface,1);
    else if(!strcmp(name,"xdg_wm_base")){shell=wl_registry_bind(registry,id,&xdg_wm_base_interface,1);xdg_wm_base_add_listener(shell,&shell_listener,NULL);}
    else if(!strcmp(name,"wl_seat")){seat=wl_registry_bind(registry,id,&wl_seat_interface,version<5?version:5);wl_seat_add_listener(seat,&seat_listener,NULL);}
    else if(!strcmp(name,"wp_fractional_scale_manager_v1"))fractional_manager=wl_registry_bind(registry,id,&wp_fractional_scale_manager_v1_interface,1);
    else if(!strcmp(name,"wp_viewporter"))viewporter=wl_registry_bind(registry,id,&wp_viewporter_interface,1);
    else if(!strcmp(name,"wl_output")&&version>=2&&output_count<8){outputs[output_count].output=wl_registry_bind(registry,id,&wl_output_interface,2);outputs[output_count].scale=1;
        wl_output_add_listener(outputs[output_count].output,&output_listener,NULL);output_count++;}
}
static void global_remove(void *d,struct wl_registry *r,uint32_t id){(void)d;(void)r;(void)id;}
static void preferred(void *d,struct wp_fractional_scale_v1 *f,uint32_t scale){(void)d;(void)f;if(scale>=120&&scale<=480&&(int)scale!=preferred_scale){preferred_scale=(int)scale;redraw=true;}}
static const struct wp_fractional_scale_v1_listener fractional_listener={preferred};
static void output_geometry(void *d,struct wl_output *o,int32_t x,int32_t y,int32_t pw,int32_t ph,int32_t sub,const char *make,const char *model,int32_t t){(void)d;(void)o;(void)x;(void)y;(void)pw;(void)ph;(void)sub;(void)make;(void)model;(void)t;}
static void output_mode(void *d,struct wl_output *o,uint32_t f,int32_t w,int32_t h,int32_t r){(void)d;(void)o;(void)f;(void)w;(void)h;(void)r;}
static void output_done(void *d,struct wl_output *o){(void)d;(void)o;redraw=true;}
static void output_scale(void *d,struct wl_output *o,int32_t s){(void)d;for(int i=0;i<output_count;i++)if(outputs[i].output==o)outputs[i].scale=s>0&&s<=4?s:1;}
static const struct wl_output_listener output_listener={.geometry=output_geometry,.mode=output_mode,.done=output_done,.scale=output_scale};
static void surface_enter(void *d,struct wl_surface *s,struct wl_output *o){(void)d;(void)s;for(int i=0;i<output_count;i++)if(outputs[i].output==o)outputs[i].entered=true;redraw=true;}
static void surface_leave(void *d,struct wl_surface *s,struct wl_output *o){(void)d;(void)s;for(int i=0;i<output_count;i++)if(outputs[i].output==o)outputs[i].entered=false;redraw=true;}
static const struct wl_surface_listener surface_listener={.enter=surface_enter,.leave=surface_leave};
static const struct wl_registry_listener registry_listener={global,global_remove};
bool settings_request(const char* request,char* reply,size_t size);   /* aurora_settings.c: the control socket */
int main(int argc,char **argv){
    if(argc>=3&&!strcmp(argv[1],"ctl")){   /* aurora-app ctl COMMAND [ARGS]: one control-socket line (aurora_control.h), e.g. lock, prepare-sleep */
        char request[512]="",reply[8192];for(int i=2;i<argc;i++){if(strlen(request)+strlen(argv[i])+2>=sizeof(request))return 2;strcat(request,argv[i]);strcat(request,i+1<argc?" ":"\n");}
        bool ok=settings_request(request,reply,sizeof(reply));fputs(reply,ok?stdout:stderr);if(reply[0]&&reply[strlen(reply)-1]!='\n')fputc('\n',ok?stdout:stderr);
        printf("[64] AURORA CTL %s -> %s\n",argv[2],ok?"OK":"ERROR");return ok?0:1;
    }
    if(argc>=3&&!strcmp(argv[1],"network")&&!strcmp(argv[2],"--wifi"))return aurora_wifi_cli(argc-3,argv+3);   /* Wi-Fi without the window */
    if(argc<2||(argc>3&&strcmp(argv[1],"authenticate"))){fprintf(stderr,"Usage: aurora-app calculator|calendar|paint|minesweeper|notes|files|terminal [PATH]\n");return 2;}
    app=argv[1];system_ticks=(uint32_t)(milliseconds()*1193182/65536000);
    if(!strcmp(app,"network")&&argc==3&&!strcmp(argv[2],"--apply"))return aurora_network_apply();   /* boot step, no window */
    {FILE* a11y=fopen("/home/nexus/.aurora/a11y.conf","re");char line[64];if(a11y){while(fgets(line,sizeof(line),a11y))if(!strcmp(line,"larger_text=1\n"))ui_set_text_scale(125);fclose(a11y);}}   /* Larger text (aurora_a11y.h) */
    if(!strcmp(app,"calculator"))calculator_open();else if(!strcmp(app,"calendar"))calendar_open();
    else if(!strcmp(app,"paint"))paint_open();else if(!strcmp(app,"minesweeper"))minesweeper_open();
    else if(!strcmp(app,"network"))aurora_network_open();
    else if(!strcmp(app,"filechooser")||!strcmp(app,"prompt"))aurora_dialog_open(app);   /* the desktop portal's dialogs (aurora_dialogs.h) */
    else if(!strcmp(app,"notifications"))aurora_notifications_open();   /* the Notification Centre */
    else if(!strcmp(app,"clipboard"))aurora_clipboard_open();   /* the clipboard history (Super+V) */
    else if(!strcmp(app,"authenticate"))aurora_authdialog_main(argc,argv);   /* the authentication agent (aurora_authdialog.h) */
    else if(!strcmp(app,"settings")){if(aurora_settings_open(argc==3?argv[2]:NULL)<0){fprintf(stderr,"Settings window unavailable\n");return 2;}}
    else if(aurora_extra_open(app,argc==3?argv[2]:NULL)<0){fprintf(stderr,"Unknown application: %s\n",app);return 2;}
    display=wl_display_connect(NULL);if(!display){perror("Connect Aurora desktop");return 3;}
    key_context=xkb_context_new(0);if(!key_context)return 3;
    struct wl_registry *registry=wl_display_get_registry(display);wl_registry_add_listener(registry,&registry_listener,NULL);
    if(wl_display_roundtrip(display)<0||!compositor||!shm||!shell){fprintf(stderr,"Desktop interfaces unavailable\n");return 3;}
    surface=wl_compositor_create_surface(compositor);wl_surface_add_listener(surface,&surface_listener,NULL);
    if(fractional_manager&&viewporter&&!getenv("AURORA_APP_NO_FRACTIONAL")){fractional=wp_fractional_scale_manager_v1_get_fractional_scale(fractional_manager,surface);wp_fractional_scale_v1_add_listener(fractional,&fractional_listener,NULL);viewport=wp_viewporter_get_viewport(viewporter,surface);}
    xdg=xdg_wm_base_get_xdg_surface(shell,surface);xdg_surface_add_listener(xdg,&xdg_listener,NULL);
    top=xdg_surface_get_toplevel(xdg);xdg_toplevel_add_listener(top,&top_listener,NULL);xdg_toplevel_set_title(top,window.title);
    char app_id[80];snprintf(app_id,sizeof(app_id),"nexus.%s",app);xdg_toplevel_set_app_id(top,app_id);xdg_toplevel_set_min_size(top,width,height);wl_surface_commit(surface);
    printf("[64] AURORA APP OPEN app=%s pid=%ld\n",app,(long)getpid());fflush(stdout);uint64_t animation=0;
    unsigned animation_interval=!strcmp(app,"calendar")?1000:!strcmp(app,"calculator")?500:0;
    int status=0;
    while(!closed){
        if(wl_display_dispatch_pending(display)<0){status=3;break;}uint64_t now=milliseconds();system_ticks=(uint32_t)(now*1193182/65536000);
        if(repeat_at&&now>=repeat_at){deliver_key(repeating_key);repeat_at=now+1000/(unsigned)repeat_rate;}
        if(aurora_extra_tick()||aurora_network_tick())redraw=true;if(!strcmp(app,"settings")&&aurora_settings_tick())redraw=true;if(!strcmp(app,"notifications")&&aurora_notifications_tick())redraw=true;if(!strcmp(app,"clipboard")&&aurora_clipboard_tick())redraw=true;if(!strcmp(app,"authenticate")&&aurora_authdialog_tick())redraw=true;if(aurora_a11y_tick())redraw=true;
        if(animation_interval&&now-animation>=animation_interval){animation=now;redraw=true;}paint();
        if(wl_display_flush(display)<0&&errno!=EAGAIN){status=3;break;}
        struct pollfd fd={wl_display_get_fd(display),POLLIN,0};int ready=poll(&fd,1,50);
        if(ready<0&&errno!=EINTR){status=3;break;}if(ready>0&&fd.revents&&wl_display_dispatch(display)<0){status=3;break;}
    }
    aurora_extra_cleanup();aurora_canvas(NULL,0,0);if(viewport)wp_viewport_destroy(viewport);if(fractional)wp_fractional_scale_v1_destroy(fractional);xdg_toplevel_destroy(top);xdg_surface_destroy(xdg);wl_surface_destroy(surface);wl_display_flush(display);wl_display_disconnect(display);
    if(pixels)munmap(pixels,buffer_size);if(key_state)xkb_state_unref(key_state);if(key_map)xkb_keymap_unref(key_map);xkb_context_unref(key_context);
    if(!strcmp(app,"authenticate")&&!status){puts("[64] AURORA AUTH CANCELLED (window closed)");status=1;}   /* closing the dialog is no authorization */
    printf("[64] AURORA APP CLOSED app=%s status=%d\n",app,status);return status;
}
