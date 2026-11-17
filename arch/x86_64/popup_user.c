#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#define CHECK(x) do{if(!(x)){dprintf(2,"[64] POPUP FAIL line=%d errno=%d\n",__LINE__,errno);exit(99);}}while(0)
struct connection {struct wl_display* display;struct wl_compositor* compositor;struct wl_shm* shm;struct xdg_wm_base* shell;struct wl_seat* seat;};
struct window {struct wl_surface* surface;struct xdg_surface* xdg;struct xdg_toplevel* top;struct xdg_popup* popup;int x,y,w,h;bool configured,done;};
static struct connection connection;
static struct window root,menu,nested;
static struct wl_surface *pointer_target,*keyboard_target;
static volatile uint32_t* framebuffer;static unsigned stride,screen_w,screen_h;
static unsigned stage,root_down,root_up,menu_down,menu_up,keys;static uint32_t key_press;
static bool open_menu,open_nested;
static void popup_config(void* data,struct xdg_popup* p,int32_t x,int32_t y,int32_t w,int32_t h){(void)p;struct window* window=data;window->x=x;window->y=y;window->w=w;window->h=h;}
static void popup_done(void* data,struct xdg_popup* p){(void)p;((struct window*)data)->done=true;}
static const struct xdg_popup_listener popup_listener={.configure=popup_config,.popup_done=popup_done};
static void configured(void* data,struct xdg_surface* x,uint32_t serial){xdg_surface_ack_configure(x,serial);((struct window*)data)->configured=true;}
static const struct xdg_surface_listener xdg_listener={configured};
static void top_config(void* d,struct xdg_toplevel* t,int32_t w,int32_t h,struct wl_array* states){(void)d;(void)t;(void)w;(void)h;(void)states;}
static void top_close(void* d,struct xdg_toplevel* t){(void)d;(void)t;CHECK(0);}
static const struct xdg_toplevel_listener top_listener={.configure=top_config,.close=top_close};
static void pointer_position(struct wl_surface* target,wl_fixed_t x,wl_fixed_t y){pointer_target=target;if(stage){printf("[64] POPUP POINTER %d %d %d\n",target==root.surface?0:target==menu.surface?1:target==nested.surface?2:-1,wl_fixed_to_int(x),wl_fixed_to_int(y));fflush(stdout);}}
static void pointer_enter(void* d,struct wl_pointer* p,uint32_t s,struct wl_surface* target,wl_fixed_t x,wl_fixed_t y){(void)d;(void)p;(void)s;pointer_position(target,x,y);}
static void pointer_leave(void* d,struct wl_pointer* p,uint32_t s,struct wl_surface* target){(void)d;(void)p;(void)s;(void)target;pointer_target=NULL;}
static void pointer_motion(void* d,struct wl_pointer* p,uint32_t time,wl_fixed_t x,wl_fixed_t y){(void)d;(void)p;(void)time;pointer_position(pointer_target,x,y);}
static void pointer_button(void* d,struct wl_pointer* p,uint32_t serial,uint32_t time,uint32_t button,uint32_t state){
    (void)d;(void)p;(void)time;CHECK(button==272);
    if(pointer_target==root.surface){if(state){root_down++;CHECK(stage==1);key_press=serial;open_menu=true;}else root_up++;}
    else if(pointer_target==menu.surface){if(state)menu_down++;else{menu_up++;if(stage==8){stage=9;puts("[64] POPUP UNGRABBED CLICKED");}else{CHECK(stage==2&&menu_down==1);stage=3;puts("[64] POPUP CLICK PASSED");}fflush(stdout);}}
    else CHECK(0);
}
static void axis(void* d,struct wl_pointer* p,uint32_t t,uint32_t axis,wl_fixed_t v){(void)d;(void)p;(void)t;(void)axis;(void)v;}
static void pointer_frame(void* d,struct wl_pointer* p){(void)d;(void)p;}
static const struct wl_pointer_listener pointer_listener={.enter=pointer_enter,.leave=pointer_leave,.motion=pointer_motion,.button=pointer_button,.axis=axis,.frame=pointer_frame};
static void keyboard_map(void* d,struct wl_keyboard* k,uint32_t fmt,int fd,uint32_t size){(void)d;(void)k;CHECK(fmt==1&&size&&!close(fd));}
static void keyboard_enter(void* d,struct wl_keyboard* k,uint32_t serial,struct wl_surface* s,struct wl_array* keys){(void)d;(void)k;(void)serial;(void)keys;keyboard_target=s;}
static void keyboard_leave(void* d,struct wl_keyboard* k,uint32_t serial,struct wl_surface* s){(void)d;(void)k;(void)serial;(void)s;keyboard_target=NULL;}
static void keyboard_key(void* d,struct wl_keyboard* k,uint32_t serial,uint32_t time,uint32_t code,uint32_t state){
    (void)d;(void)k;(void)time;
    if(code==1)return;
    if(stage==3){CHECK(keyboard_target==menu.surface&&code==30);if(state)key_press=serial;else{keys++;open_nested=true;}}
    else if(stage==4){CHECK(keyboard_target==nested.surface&&code==48);if(!state){keys++;puts("[64] POPUP NESTED KEY PASSED");fflush(stdout);}}
    else if(stage==6){CHECK(keyboard_target==root.surface&&code==46&&root_down==1&&root_up==1);if(!state)stage=7;}
    else if(stage==9){CHECK(keyboard_target==root.surface&&code==32);if(!state)stage=10;}
    else CHECK(0);
}
static void mods(void* d,struct wl_keyboard* k,uint32_t serial,uint32_t a,uint32_t b,uint32_t c,uint32_t e){(void)d;(void)k;(void)serial;(void)a;(void)b;(void)c;(void)e;}
static void repeat(void* d,struct wl_keyboard* k,int32_t a,int32_t b){(void)d;(void)k;(void)a;(void)b;}
static const struct wl_keyboard_listener keyboard_listener={keyboard_map,keyboard_enter,keyboard_leave,keyboard_key,mods,repeat};
static void caps(void* d,struct wl_seat* seat,uint32_t capabilities){if(d!=&connection)return;CHECK((capabilities&3)==3);struct wl_pointer* p=wl_seat_get_pointer(seat);struct wl_keyboard* k=wl_seat_get_keyboard(seat);CHECK(p&&k);wl_pointer_add_listener(p,&pointer_listener,NULL);wl_keyboard_add_listener(k,&keyboard_listener,NULL);}
static void seat_name(void* d,struct wl_seat* s,const char* name){(void)d;(void)s;(void)name;}
static const struct wl_seat_listener seat_listener={caps,seat_name};
static void global(void* data,struct wl_registry* registry,uint32_t id,const char* name,uint32_t version){
    (void)version;struct connection* c=data;
    if(!strcmp(name,"wl_compositor"))c->compositor=wl_registry_bind(registry,id,&wl_compositor_interface,4);
    if(!strcmp(name,"wl_shm"))c->shm=wl_registry_bind(registry,id,&wl_shm_interface,1);
    if(!strcmp(name,"xdg_wm_base"))c->shell=wl_registry_bind(registry,id,&xdg_wm_base_interface,1);
    if(!strcmp(name,"wl_seat")){c->seat=wl_registry_bind(registry,id,&wl_seat_interface,5);wl_seat_add_listener(c->seat,&seat_listener,c);}
}
static void global_remove(void* d,struct wl_registry* r,uint32_t id){(void)d;(void)r;(void)id;}
static const struct wl_registry_listener registry_listener={global,global_remove};
static void connect_client(struct connection* c){memset(c,0,sizeof(*c));c->display=wl_display_connect(NULL);CHECK(c->display);struct wl_registry* r=wl_display_get_registry(c->display);wl_registry_add_listener(r,&registry_listener,c);CHECK(wl_display_roundtrip(c->display)>=0&&c->compositor&&c->shm&&c->shell&&c->seat);wl_registry_destroy(r);}
static void roundtrip(struct connection* c){CHECK(wl_display_roundtrip(c->display)>=0);}
static void settle(void){roundtrip(&connection);usleep(150000);roundtrip(&connection);}
static void create_window(struct connection* c,struct window* w){memset(w,0,sizeof(*w));w->surface=wl_compositor_create_surface(c->compositor);w->xdg=xdg_wm_base_get_xdg_surface(c->shell,w->surface);CHECK(w->surface&&w->xdg);xdg_surface_add_listener(w->xdg,&xdg_listener,w);}
static void create_top(struct connection* c,struct window* w){create_window(c,w);w->top=xdg_surface_get_toplevel(w->xdg);CHECK(w->top);xdg_toplevel_add_listener(w->top,&top_listener,w);wl_surface_commit(w->surface);while(!w->configured)CHECK(wl_display_dispatch(c->display)>=0);}
static void upload(struct connection* c,struct window* w,int width,int height,uint32_t color){
    size_t size=(size_t)width*height*4;int fd=memfd_create("popup",MFD_CLOEXEC);CHECK(fd>=0&&!ftruncate(fd,size));
    uint32_t* data=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);CHECK(data!=MAP_FAILED);for(size_t i=0;i<size/4;i++)data[i]=color;
    struct wl_shm_pool* pool=wl_shm_create_pool(c->shm,fd,size);struct wl_buffer* buffer=wl_shm_pool_create_buffer(pool,0,width,height,width*4,WL_SHM_FORMAT_XRGB8888);CHECK(pool&&buffer);
    wl_surface_attach(w->surface,buffer,0,0);wl_surface_damage_buffer(w->surface,0,0,width,height);wl_surface_commit(w->surface);
    wl_buffer_destroy(buffer);wl_shm_pool_destroy(pool);roundtrip(c);CHECK(!munmap(data,size)&&!close(fd));
}
static struct xdg_positioner* positioner(struct connection* c,int x,int y,int width,int height){
    struct xdg_positioner* p=xdg_wm_base_create_positioner(c->shell);CHECK(p);xdg_positioner_set_size(p,width,height);xdg_positioner_set_anchor_rect(p,x,y,10,10);
    xdg_positioner_set_anchor(p,XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT);xdg_positioner_set_gravity(p,XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);return p;
}
static void make_popup(struct connection* c,struct window* w,struct window* parent,struct xdg_positioner* p){create_window(c,w);w->popup=xdg_surface_get_popup(w->xdg,parent?parent->xdg:NULL,p);CHECK(w->popup);xdg_popup_add_listener(w->popup,&popup_listener,w);}
static void map_popup(struct connection* c,struct window* w,uint32_t color){wl_surface_commit(w->surface);while(!w->configured)CHECK(wl_display_dispatch(c->display)>=0);upload(c,w,w->w,w->h,color);}
static void destroy_window(struct window* w){if(w->popup)xdg_popup_destroy(w->popup);if(w->top)xdg_toplevel_destroy(w->top);xdg_surface_destroy(w->xdg);wl_surface_destroy(w->surface);memset(w,0,sizeof(*w));}
static void color(unsigned x,unsigned y,uint32_t wanted){CHECK(x<screen_w&&y<screen_h);if((framebuffer[(size_t)y*stride+x]&0xffffff)!=wanted)dprintf(2,"pixel %u,%u wanted=%x actual=%x\n",x,y,wanted,framebuffer[(size_t)y*stride+x]);CHECK((framebuffer[(size_t)y*stride+x]&0xffffff)==wanted);}
static void invalid(unsigned kind){
    struct connection c;connect_client(&c);struct window parent,popup,child;create_top(&c,&parent);upload(&c,&parent,200,200,0x556677);
    struct xdg_positioner* p=positioner(&c,20,20,50,40);const struct wl_interface* expected=&xdg_positioner_interface;uint32_t code=0;
    if(kind<5){if(kind==0)xdg_positioner_set_size(p,0,1);if(kind==1)xdg_positioner_set_anchor_rect(p,0,0,-1,5);if(kind==2)xdg_positioner_set_anchor(p,9);if(kind==3)xdg_positioner_set_gravity(p,9);if(kind==4)xdg_positioner_set_constraint_adjustment(p,64);}
    else{
        expected=&xdg_wm_base_interface;
        if(kind==5){xdg_positioner_destroy(p);p=xdg_wm_base_create_positioner(c.shell);make_popup(&c,&popup,&parent,p);code=XDG_WM_BASE_ERROR_INVALID_POSITIONER;}
        if(kind==6){make_popup(&c,&popup,NULL,p);code=XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT;}
        if(kind==7){make_popup(&c,&popup,&parent,p);map_popup(&c,&popup,0x123456);make_popup(&c,&child,&popup,p);xdg_popup_destroy(popup.popup);code=XDG_WM_BASE_ERROR_NOT_THE_TOPMOST_POPUP;}
        if(kind==8){make_popup(&c,&popup,&parent,p);map_popup(&c,&popup,0x123456);xdg_popup_grab(popup.popup,c.seat,1);expected=&xdg_popup_interface;code=XDG_POPUP_ERROR_INVALID_GRAB;}
        if(kind==9){make_popup(&c,&popup,&parent,p);map_popup(&c,&popup,0x123456);make_popup(&c,&child,&popup,p);xdg_popup_grab(child.popup,c.seat,1);code=XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT;}
        if(kind==10){make_popup(&c,&popup,&parent,p);xdg_surface_destroy(popup.xdg);expected=&xdg_surface_interface;code=XDG_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT;}
        if(kind==11){xdg_wm_base_destroy(c.shell);code=XDG_WM_BASE_ERROR_DEFUNCT_SURFACES;}
        if(kind==12){xdg_positioner_set_anchor_rect(p,199,199,10,10);make_popup(&c,&popup,&parent,p);wl_surface_commit(popup.surface);code=XDG_WM_BASE_ERROR_INVALID_POSITIONER;}
    }
    CHECK(wl_display_roundtrip(c.display)==-1&&wl_display_get_error(c.display)==EPROTO);
    const struct wl_interface* interface=NULL;uint32_t id=0;uint32_t actual=wl_display_get_protocol_error(c.display,&interface,&id);
    printf("[64] POPUP ERROR CASE %u code=%u id=%u interface=%s\n",kind,actual,id,interface?interface->name:"destroyed");fflush(stdout);
    CHECK(actual==code&&(id||kind==10||kind==11));
    CHECK(interface==expected||(!interface&&(kind==10||kind==11)));
    wl_display_disconnect(c.display);settle();color(10,10,0x112233);
}
int main(void){
    connect_client(&connection);int fd=open("/dev/fb0",O_RDONLY|O_CLOEXEC);struct fb_fix_screeninfo fix;struct fb_var_screeninfo var;
    CHECK(fd>=0&&!ioctl(fd,FBIOGET_FSCREENINFO,&fix)&&!ioctl(fd,FBIOGET_VSCREENINFO,&var));framebuffer=mmap(NULL,fix.smem_len,PROT_READ,MAP_SHARED,fd,0);CHECK(framebuffer!=MAP_FAILED&&!close(fd));stride=fix.line_length/4;screen_w=var.xres;screen_h=var.yres;
    create_top(&connection,&root);xdg_surface_set_window_geometry(root.xdg,10,20,600,440);wl_surface_commit(root.surface);roundtrip(&connection);
    upload(&connection,&root,640,480,0x112233);settle();
    struct xdg_positioner* p=positioner(&connection,30,40,80,60);xdg_positioner_set_anchor_rect(p,0,0,0,0);xdg_positioner_set_anchor_rect(p,30,40,10,10);make_popup(&connection,&menu,&root,p);
    xdg_positioner_set_anchor_rect(p,300,300,10,10);xdg_positioner_destroy(p);wl_surface_commit(menu.surface);while(!menu.configured)CHECK(wl_display_dispatch(connection.display)>=0);
    CHECK(menu.x==40&&menu.y==50&&menu.w==80&&menu.h==60);xdg_surface_set_window_geometry(menu.xdg,5,5,80,60);upload(&connection,&menu,90,70,0xdd3344);settle();color(46,66,0xdd3344);color(40,60,0x112233);
    p=positioner(&connection,30,20,40,25);make_popup(&connection,&nested,&menu,p);xdg_positioner_destroy(p);map_popup(&connection,&nested,0x00cc66);settle();CHECK(nested.x==40&&nested.y==30);color(91,101,0x00cc66);
    destroy_window(&nested);destroy_window(&menu);settle();color(91,101,0x112233);
    xdg_surface_set_window_geometry(root.xdg,20,30,600,440);p=positioner(&connection,30,40,80,60);make_popup(&connection,&menu,&root,p);xdg_positioner_destroy(p);map_popup(&connection,&menu,0xdd3344);settle();color(51,71,0xdd3344);color(45,65,0x112233);
    destroy_window(&menu);wl_surface_commit(root.surface);settle();
    p=positioner(&connection,30,40,80,60);make_popup(&connection,&menu,&root,p);xdg_positioner_destroy(p);map_popup(&connection,&menu,0xdd3344);settle();color(61,81,0xdd3344);color(51,71,0x112233);destroy_window(&menu);settle();
    puts("[64] POPUP GEOMETRY PASSED: immutable rules, nested pixels, shadows and committed parent geometry");fflush(stdout);
    for(unsigned i=0;i<13;i++)invalid(i);
    /* Forged input serials deny the grab, and abrupt disconnect retires all retained objects. */
    struct connection c;connect_client(&c);struct window other,denied;create_top(&c,&other);upload(&c,&other,200,200,0x556677);
    p=positioner(&c,20,20,40,30);make_popup(&c,&denied,&other,p);xdg_popup_grab(denied.popup,c.seat,0xdeadbeef);roundtrip(&c);CHECK(denied.done);wl_display_disconnect(c.display);settle();color(10,10,0x112233);
    xdg_surface_set_window_geometry(root.xdg,0,0,screen_w,screen_h);upload(&connection,&root,screen_w,screen_h,0x112233);settle();
    p=positioner(&connection,screen_w-20,screen_h-20,80,60);xdg_positioner_set_constraint_adjustment(p,12);make_popup(&connection,&menu,&root,p);xdg_positioner_destroy(p);map_popup(&connection,&menu,0xdd3344);settle();
    CHECK(menu.x==(int)screen_w-100&&menu.y==(int)screen_h-80);color(screen_w-90,screen_h-70,0xdd3344);destroy_window(&menu);settle();
    stage=1;printf("[64] POPUP INPUT READY %u %u\n",screen_w/2,screen_h/2);fflush(stdout);
    while(stage!=7){
        CHECK(wl_display_dispatch(connection.display)>=0);
        if(open_menu){open_menu=false;p=positioner(&connection,95,95,100,80);make_popup(&connection,&menu,&root,p);xdg_popup_grab(menu.popup,connection.seat,key_press);xdg_positioner_destroy(p);map_popup(&connection,&menu,0x18c784);settle();CHECK(!menu.done&&keyboard_target==menu.surface);stage=2;puts("[64] POPUP GRAB MAPPED");fflush(stdout);}
        if(open_nested){open_nested=false;p=positioner(&connection,10,10,40,30);make_popup(&connection,&nested,&menu,p);xdg_popup_grab(nested.popup,connection.seat,key_press);xdg_positioner_destroy(p);map_popup(&connection,&nested,0x3366dd);settle();CHECK(!nested.done&&keyboard_target==nested.surface);color(130,130,0x3366dd);stage=4;puts("[64] POPUP NESTED MAPPED");fflush(stdout);}
        if(stage==4&&nested.done){CHECK(keys==2);struct window late;p=positioner(&connection,1,1,20,10);make_popup(&connection,&late,&nested,p);xdg_popup_grab(late.popup,connection.seat,key_press);xdg_positioner_destroy(p);roundtrip(&connection);CHECK(late.done);destroy_window(&late);destroy_window(&nested);settle();CHECK(keyboard_target==menu.surface&&!menu.done);color(130,130,0x18c784);stage=5;puts("[64] POPUP NESTED DISMISSED");fflush(stdout);}
        if(stage==5&&menu.done){destroy_window(&menu);settle();CHECK(keyboard_target==root.surface);color(130,130,0x112233);stage=6;puts("[64] POPUP OUTSIDE DISMISSED");fflush(stdout);}
    }
    CHECK(root_down==1&&root_up==1&&menu_down==1&&menu_up==1);
    p=positioner(&connection,95,95,100,80);make_popup(&connection,&menu,&root,p);xdg_positioner_destroy(p);map_popup(&connection,&menu,0x18c784);settle();stage=8;puts("[64] POPUP UNGRABBED READY");fflush(stdout);
    while(stage!=10)CHECK(wl_display_dispatch(connection.display)>=0);
    CHECK(menu_down==2&&menu_up==2&&keyboard_target==root.surface);destroy_window(&menu);destroy_window(&root);roundtrip(&connection);wl_display_disconnect(connection.display);
    puts("[64] POPUP ALL PASSED: placement, pixels, errors, explicit grabs, nested focus and dismissal without click-through");return 0;
}
