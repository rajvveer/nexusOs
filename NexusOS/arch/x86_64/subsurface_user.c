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
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

#define CHECK(test) do { if (!(test)) { fprintf(stderr,"[64] SUBSURFACE FAIL line=%d errno=%d\n",__LINE__,errno); exit(1); } } while (0)
struct connection { struct wl_display* display; struct wl_compositor* compositor; struct wl_subcompositor* subcompositor; struct wl_shm* shm; struct xdg_wm_base* shell; };
static struct connection connection;
static bool configured;
static volatile uint32_t* framebuffer;
static unsigned stride;
static struct wl_surface *input_root,*input_child,*input_grand,*pointer_target,*keyboard_target;
static int pointer_x,pointer_y,input_stage;
static unsigned input_keys;
static void pointer_position(struct wl_surface* target,wl_fixed_t x,wl_fixed_t y) {
    pointer_target=target;pointer_x=wl_fixed_to_int(x);pointer_y=wl_fixed_to_int(y);
    if(input_stage) {
        int kind=target==input_root?0:target==input_child?1:target==input_grand?2:-1;CHECK(kind>=0);
        printf("[64] SUBSURFACE POINTER %d %d %d\n",kind,pointer_x,pointer_y);fflush(stdout);
        if(input_stage==4&&target==input_root&&pointer_x==100&&pointer_y==100)input_stage=5;
    }
}
static void pointer_enter(void* d,struct wl_pointer* p,uint32_t serial,struct wl_surface* target,wl_fixed_t x,wl_fixed_t y) { (void)d;(void)p;(void)serial;pointer_position(target,x,y); }
static void pointer_leave(void* d,struct wl_pointer* p,uint32_t serial,struct wl_surface* target) { (void)d;(void)p;(void)serial;(void)target;pointer_target=NULL; }
static void pointer_motion(void* d,struct wl_pointer* p,uint32_t time,wl_fixed_t x,wl_fixed_t y) { (void)d;(void)p;(void)time;pointer_position(pointer_target,x,y); }
static void pointer_button(void* d,struct wl_pointer* p,uint32_t serial,uint32_t time,uint32_t button,uint32_t state) {
    (void)d;(void)p;(void)serial;(void)time;CHECK(button==272&&pointer_target==input_grand);
    if(state){CHECK(input_stage==1&&pointer_x==10&&pointer_y==10);input_stage=2;puts("[64] SUBSURFACE GRAB READY");}
    else {CHECK(input_stage==2&&pointer_x==310&&pointer_y==210&&input_keys==2);input_stage=3;puts("[64] SUBSURFACE GRAB PASSED");}
    fflush(stdout);
}
static void pointer_axis(void* d,struct wl_pointer* p,uint32_t t,uint32_t axis,wl_fixed_t value) { (void)d;(void)p;(void)t;(void)axis;(void)value; }
static void pointer_frame(void* d,struct wl_pointer* p) { (void)d;(void)p; }
static const struct wl_pointer_listener pointer_listener={.enter=pointer_enter,.leave=pointer_leave,.motion=pointer_motion,.button=pointer_button,.axis=pointer_axis,.frame=pointer_frame};
static void keyboard_map(void* d,struct wl_keyboard* k,uint32_t format,int fd,uint32_t size) { (void)d;(void)k;CHECK(format==1&&size&&close(fd)==0); }
static void keyboard_enter(void* d,struct wl_keyboard* k,uint32_t serial,struct wl_surface* target,struct wl_array* keys) { (void)d;(void)k;(void)serial;(void)keys;CHECK(target==input_root);keyboard_target=target; }
static void keyboard_leave(void* d,struct wl_keyboard* k,uint32_t serial,struct wl_surface* target) { (void)d;(void)k;(void)serial;(void)target;keyboard_target=NULL; }
static void keyboard_key(void* d,struct wl_keyboard* k,uint32_t serial,uint32_t time,uint32_t code,uint32_t state) {
    (void)d;(void)k;(void)serial;(void)time;CHECK(input_stage==2&&keyboard_target==input_root&&code==30&&state==(input_keys?0U:1U));
    if(++input_keys==2){puts("[64] SUBSURFACE KEYBOARD PASSED");fflush(stdout);}
}
static void keyboard_mods(void* d,struct wl_keyboard* k,uint32_t serial,uint32_t a,uint32_t b,uint32_t c,uint32_t e) { (void)d;(void)k;(void)serial;(void)a;(void)b;(void)c;(void)e; }
static void keyboard_repeat(void* d,struct wl_keyboard* k,int32_t rate,int32_t delay) { (void)d;(void)k;(void)rate;(void)delay; }
static const struct wl_keyboard_listener keyboard_listener={keyboard_map,keyboard_enter,keyboard_leave,keyboard_key,keyboard_mods,keyboard_repeat};
static void seat_capabilities(void* d,struct wl_seat* seat,uint32_t caps) {
    (void)d;CHECK((caps&3)==3);struct wl_pointer* p=wl_seat_get_pointer(seat);struct wl_keyboard* k=wl_seat_get_keyboard(seat);CHECK(p&&k);
    wl_pointer_add_listener(p,&pointer_listener,NULL);wl_keyboard_add_listener(k,&keyboard_listener,NULL);
}
static void seat_name(void* d,struct wl_seat* s,const char* name) { (void)d;(void)s;(void)name; }
static const struct wl_seat_listener seat_listener={seat_capabilities,seat_name};
static void global(void* data,struct wl_registry* registry,uint32_t id,const char* name,uint32_t version) {
    (void)version; struct connection* c=data;
    if(!strcmp(name,"wl_compositor"))c->compositor=wl_registry_bind(registry,id,&wl_compositor_interface,4);
    if(!strcmp(name,"wl_subcompositor"))c->subcompositor=wl_registry_bind(registry,id,&wl_subcompositor_interface,1);
    if(!strcmp(name,"wl_shm"))c->shm=wl_registry_bind(registry,id,&wl_shm_interface,1);
    if(!strcmp(name,"xdg_wm_base"))c->shell=wl_registry_bind(registry,id,&xdg_wm_base_interface,1);
    if(c==&connection&&!strcmp(name,"wl_seat")){struct wl_seat* seat=wl_registry_bind(registry,id,&wl_seat_interface,5);CHECK(seat);wl_seat_add_listener(seat,&seat_listener,NULL);}
}
static void remove_global(void* d,struct wl_registry* r,uint32_t id) { (void)d;(void)r;(void)id; }
static const struct wl_registry_listener registry_listener={global,remove_global};
static void connect_client(struct connection* c) {
    memset(c,0,sizeof(*c));c->display=wl_display_connect(NULL);CHECK(c->display);
    struct wl_registry* registry=wl_display_get_registry(c->display);CHECK(registry);
    wl_registry_add_listener(registry,&registry_listener,c);CHECK(wl_display_roundtrip(c->display)>=0);
    CHECK(c->compositor&&c->subcompositor&&c->shm&&c->shell);wl_registry_destroy(registry);
}
static void configure(void* d,struct xdg_surface* xdg,uint32_t serial) { (void)d;xdg_surface_ack_configure(xdg,serial);configured=true; }
static const struct xdg_surface_listener configure_listener={configure};
static void top_configure(void* d,struct xdg_toplevel* t,int32_t w,int32_t h,struct wl_array* states) { (void)d;(void)t;(void)w;(void)h;(void)states; }
static void top_close(void* d,struct xdg_toplevel* t) { (void)d;(void)t;CHECK(0); }
static const struct xdg_toplevel_listener top_listener={.configure=top_configure,.close=top_close};
static void settle(void) { CHECK(wl_display_roundtrip(connection.display)>=0);usleep(150000);CHECK(wl_display_roundtrip(connection.display)>=0); }
static void color(unsigned x,unsigned y,uint32_t value) {
    if((framebuffer[(size_t)y*stride+x]&0xFFFFFF)!=value) {
        fprintf(stderr,"pixel %u,%u expected=%06x actual=%06x\n",x,y,value,framebuffer[(size_t)y*stride+x]&0xFFFFFF);CHECK(0);
    }
}
static void upload(struct wl_surface* surface,int w,int h,uint32_t value) {
    int fd=memfd_create("subsurface-test",MFD_CLOEXEC);size_t size=(size_t)w*h*4;CHECK(fd>=0&&!ftruncate(fd,size));
    uint32_t* data=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);CHECK(data!=MAP_FAILED);
    for(size_t i=0;i<size/4;i++)data[i]=value;
    struct wl_shm_pool* pool=wl_shm_create_pool(connection.shm,fd,size);CHECK(pool);
    struct wl_buffer* buffer=wl_shm_pool_create_buffer(pool,0,w,h,w*4,WL_SHM_FORMAT_XRGB8888);CHECK(buffer);
    wl_surface_attach(surface,buffer,0,0);wl_surface_damage_buffer(surface,0,0,w,h);wl_surface_commit(surface);
    /* The server must own cached contents after this commit even if objects vanish. */
    wl_buffer_destroy(buffer);wl_shm_pool_destroy(pool);CHECK(wl_display_roundtrip(connection.display)>=0);
    CHECK(!munmap(data,size)&&!close(fd));
}
static unsigned frames;
static void frame_done(void* d,struct wl_callback* callback,uint32_t time) { (void)d;(void)time;frames++;wl_callback_destroy(callback); }
static const struct wl_callback_listener frame_listener={frame_done};
static void invalid(unsigned kind) {
    struct connection c;connect_client(&c);
    struct wl_surface *a=wl_compositor_create_surface(c.compositor),*b=wl_compositor_create_surface(c.compositor),*other=wl_compositor_create_surface(c.compositor);
    CHECK(a&&b&&other);
    struct wl_subsurface* child=wl_subcompositor_get_subsurface(c.subcompositor,b,a);CHECK(child);
    if(kind==0)wl_subcompositor_get_subsurface(c.subcompositor,a,b);
    if(kind==1)wl_subcompositor_get_subsurface(c.subcompositor,b,other);
    if(kind==2)wl_subsurface_place_above(child,other);
    if(kind==3)wl_subsurface_place_below(child,b);
    if(kind==4)xdg_wm_base_get_xdg_surface(c.shell,b);
    CHECK(wl_display_roundtrip(c.display)==-1&&wl_display_get_error(c.display)==EPROTO);
    const struct wl_interface* interface=NULL;uint32_t id=0;
    uint32_t error=wl_display_get_protocol_error(c.display,&interface,&id);CHECK(interface&&id);
    CHECK(error==(kind==0?WL_SUBCOMPOSITOR_ERROR_BAD_PARENT:kind==4?XDG_WM_BASE_ERROR_ROLE:0));
    wl_display_disconnect(c.display);settle();
}
int main(void) {
    connect_client(&connection);
    int fb=open("/dev/fb0",O_RDONLY|O_CLOEXEC);struct fb_fix_screeninfo fixed;struct fb_var_screeninfo variable;
    CHECK(fb>=0&&!ioctl(fb,FBIOGET_FSCREENINFO,&fixed)&&!ioctl(fb,FBIOGET_VSCREENINFO,&variable));
    framebuffer=mmap(NULL,fixed.smem_len,PROT_READ,MAP_SHARED,fb,0);CHECK(framebuffer!=MAP_FAILED&&!close(fb));stride=fixed.line_length/4;
    struct wl_surface* root=wl_compositor_create_surface(connection.compositor);CHECK(root);input_root=root;
    struct xdg_surface* xdg=xdg_wm_base_get_xdg_surface(connection.shell,root);CHECK(xdg);xdg_surface_add_listener(xdg,&configure_listener,NULL);
    struct xdg_toplevel* top=xdg_surface_get_toplevel(xdg);CHECK(top);xdg_toplevel_add_listener(top,&top_listener,NULL);
    wl_surface_commit(root);while(!configured)CHECK(wl_display_dispatch(connection.display)>=0);
    upload(root,640,480,0x112233);settle();color(84,84,0x112233);
    /* Create descendants first to exercise commit order independent of allocation order. */
    struct wl_surface* grand=wl_compositor_create_surface(connection.compositor);
    struct wl_surface* child=wl_compositor_create_surface(connection.compositor);
    struct wl_surface* sibling=wl_compositor_create_surface(connection.compositor);CHECK(grand&&child&&sibling);
    struct wl_subsurface* sub=wl_subcompositor_get_subsurface(connection.subcompositor,child,root);
    struct wl_subsurface* nested=wl_subcompositor_get_subsurface(connection.subcompositor,grand,child);
    struct wl_subsurface* other=wl_subcompositor_get_subsurface(connection.subcompositor,sibling,root);CHECK(sub&&nested&&other);
    wl_subsurface_set_position(sub,80,80);wl_subsurface_set_position(nested,-8,-8);wl_subsurface_set_position(other,90,90);
    struct wl_callback* callback=wl_surface_frame(grand);CHECK(callback);wl_callback_add_listener(callback,&frame_listener,NULL);
    upload(grand,32,32,0x00CC66);upload(child,64,64,0xDD3344);upload(sibling,64,64,0x3366DD);
    settle();CHECK(frames==0);color(84,84,0x112233);
    wl_surface_commit(root);settle();CHECK(frames==1);color(74,74,0x00CC66);color(84,84,0x00CC66);color(110,110,0x3366DD);
    wl_subsurface_place_above(sub,sibling);settle();color(110,110,0x3366DD);
    wl_surface_commit(root);settle();color(110,110,0xDD3344);
    wl_subsurface_place_below(sub,root);wl_surface_commit(root);settle();color(84,84,0x112233);color(110,110,0x3366DD);
    wl_subsurface_place_above(sub,sibling);wl_surface_commit(root);settle();color(84,84,0x00CC66);
    wl_subsurface_set_position(sub,120,120);wl_subsurface_set_position(nested,-16,-16);wl_surface_commit(child);
    wl_subsurface_set_position(nested,16,16);wl_surface_commit(root);settle();color(108,108,0x00CC66);color(144,144,0xDD3344);
    wl_surface_commit(child);wl_surface_commit(root);settle();color(108,108,0x3366DD);color(144,144,0x00CC66);
    wl_subsurface_set_desync(nested);upload(grand,32,32,0xAA55CC);settle();color(144,144,0x00CC66);
    wl_surface_commit(root);settle();color(144,144,0xAA55CC);
    upload(child,64,64,0xEE8833);settle();color(124,124,0xDD3344);
    wl_subsurface_set_desync(sub);settle();color(124,124,0xEE8833);
    upload(grand,32,32,0x55DDCC);settle();color(144,144,0x55DDCC);
    wl_subsurface_set_position(sub,-10,-10);wl_surface_commit(child);settle();color(124,124,0xEE8833);
    wl_surface_commit(root);settle();color(0,0,0xEE8833);color(10,10,0x55DDCC);
    wl_subsurface_set_sync(sub);upload(child,64,64,0xBB4477);upload(grand,32,32,0x99DD22);settle();color(0,0,0xEE8833);color(10,10,0x55DDCC);
    wl_surface_commit(root);settle();color(0,0,0xBB4477);color(10,10,0x99DD22);
    /* Destroy the parent wl_surface before descendants/role objects. */
    wl_surface_destroy(child);settle();color(10,10,0x112233);
    wl_subsurface_set_position(nested,100,100);wl_subsurface_set_desync(nested);wl_surface_commit(grand);settle();color(110,110,0x3366DD);
    wl_subsurface_destroy(nested);wl_surface_destroy(grand);wl_subsurface_destroy(sub);
    wl_subsurface_destroy(other);settle();color(110,110,0x112233);
    other=wl_subcompositor_get_subsurface(connection.subcompositor,sibling,root);CHECK(other);
    wl_subsurface_set_position(other,700,40);wl_surface_commit(root);settle();color(710,50,0x3366DD);
    wl_surface_attach(root,NULL,0,0);wl_surface_commit(root);settle();color(710,50,0x101522);
    configured=false;wl_surface_commit(root);while(!configured)CHECK(wl_display_dispatch(connection.display)>=0);
    upload(root,640,480,0x112233);settle();color(710,50,0x3366DD);
    wl_subsurface_destroy(other);wl_surface_destroy(sibling);
    upload(root,variable.xres,variable.yres,0x112233);
    input_child=wl_compositor_create_surface(connection.compositor);input_grand=wl_compositor_create_surface(connection.compositor);CHECK(input_child&&input_grand);
    sub=wl_subcompositor_get_subsurface(connection.subcompositor,input_child,root);
    nested=wl_subcompositor_get_subsurface(connection.subcompositor,input_grand,input_child);CHECK(sub&&nested);
    wl_subsurface_set_position(sub,80,80);wl_subsurface_set_position(nested,10,10);
    upload(input_grand,32,32,0x33CC77);upload(input_child,64,64,0xEE6633);wl_surface_commit(root);settle();
    input_stage=1;printf("[64] SUBSURFACE INPUT READY %u %u\n",variable.xres/2,variable.yres/2);fflush(stdout);
    while(input_stage<3||pointer_target!=root)CHECK(wl_display_dispatch(connection.display)>=0);
    struct wl_region* region=wl_compositor_create_region(connection.compositor);CHECK(region);
    wl_surface_set_input_region(input_grand,region);wl_surface_commit(input_grand);
    wl_region_add(region,0,0,64,64);wl_region_subtract(region,0,0,32,32);wl_surface_set_input_region(input_child,region);wl_region_destroy(region);
    wl_surface_commit(input_child);wl_surface_commit(root);settle();
    input_stage=4;puts("[64] SUBSURFACE REGION READY");fflush(stdout);
    while(input_stage<5)CHECK(wl_display_dispatch(connection.display)>=0);
    CHECK(pointer_target==root&&pointer_x==100&&pointer_y==100&&keyboard_target==root);
    input_stage=0;wl_surface_destroy(input_grand);wl_subsurface_destroy(nested);wl_subsurface_destroy(sub);wl_surface_destroy(input_child);settle();
    for(unsigned i=0;i<5;i++)invalid(i);
    xdg_toplevel_destroy(top);xdg_surface_destroy(xdg);wl_surface_destroy(root);settle();color(20,20,0x101522);
    wl_display_disconnect(connection.display);CHECK(!munmap((void*)framebuffer,fixed.smem_len));
    puts("[64] SUBSURFACE ALL PASSED: cached/nested commits, stacking, positions, desync, remap, frame timing, input/grabs/regions, keyboard focus, lifetime and protocol errors");return 0;
}
