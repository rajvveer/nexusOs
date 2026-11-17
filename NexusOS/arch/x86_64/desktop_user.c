#define _GNU_SOURCE
#include <stdbool.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "xdg-shell-client-protocol.h"

#define CHECK(x) do { if(!(x)){fprintf(stderr,"[64] WAYLAND CLIENT FAIL line=%d\n",__LINE__);exit(1);} }while(0)
static struct wl_display* display;
static struct wl_compositor* compositor;
static struct wl_shm* shm;
static struct xdg_wm_base* shell;
static struct wl_seat* seat;
static struct wl_output* output;
static struct wl_data_device_manager* data_manager;
static struct wl_data_device* data_device;
static struct wl_data_source* clipboard_source;
static int clipboard_fd=-1;
static bool transfer_sent,copied;
static struct wl_surface* surface;
static struct wl_buffer* buffer;
static struct xdg_surface* xdg;
static uint32_t* pixels;
static int width=640,height=480;
static bool running=true,busy,redraw,typed,clicked;
static unsigned keymaps,enters,keys,clicks,frames,output_enters;
static struct xkb_context* context;
static const char clipboard_text[]="Nexus clipboard passes the real pipe descriptor";
static void source_target(void* d,struct wl_data_source* s,const char* mime){(void)d;(void)s;(void)mime;}
static void source_send(void* d,struct wl_data_source* s,const char* mime,int32_t fd){(void)d;(void)s;CHECK(!strcmp(mime,"text/plain;charset=utf-8"));CHECK(write(fd,clipboard_text,sizeof(clipboard_text))==sizeof(clipboard_text)&&!close(fd));transfer_sent=true;}
static void source_cancelled(void* d,struct wl_data_source* s){(void)d;(void)s;}
static const struct wl_data_source_listener source_listener={.target=source_target,.send=source_send,.cancelled=source_cancelled};
static void mime_offer(void* d,struct wl_data_offer* o,const char* mime){(void)d;(void)o;CHECK(!strcmp(mime,"text/plain;charset=utf-8"));}
static const struct wl_data_offer_listener offer_listener={.offer=mime_offer};
static void data_offer(void* d,struct wl_data_device* device,struct wl_data_offer* offer){(void)d;(void)device;wl_data_offer_add_listener(offer,&offer_listener,NULL);}
static void selection(void* d,struct wl_data_device* device,struct wl_data_offer* offer){(void)d;(void)device;if(!offer)return;CHECK(clipboard_fd<0);int pipe[2];CHECK(!pipe2(pipe,O_CLOEXEC|O_NONBLOCK));clipboard_fd=pipe[0];wl_data_offer_receive(offer,"text/plain;charset=utf-8",pipe[1]);CHECK(!close(pipe[1]));}
static const struct wl_data_device_listener device_listener={.data_offer=data_offer,.selection=selection};
static void output_geometry(void* d,struct wl_output* o,int32_t x,int32_t y,int32_t w,int32_t h,int32_t sub,const char* make,const char* model,int32_t transform){(void)d;(void)o;(void)x;(void)y;(void)w;(void)h;(void)sub;(void)make;(void)model;CHECK(transform==WL_OUTPUT_TRANSFORM_NORMAL);}
static void output_mode(void* d,struct wl_output* o,uint32_t flags,int32_t w,int32_t h,int32_t refresh){(void)d;(void)o;CHECK((flags&WL_OUTPUT_MODE_CURRENT)&&w>=640&&h>=480&&refresh>0);}
static void output_done(void* d,struct wl_output* o){(void)d;(void)o;}
static void output_scale(void* d,struct wl_output* o,int32_t scale){(void)d;(void)o;CHECK(scale==1);}
static const struct wl_output_listener output_listener={.geometry=output_geometry,.mode=output_mode,.done=output_done,.scale=output_scale};
static void surface_enter(void* d,struct wl_surface* s,struct wl_output* o){(void)d;CHECK(s==surface&&o==output);output_enters++;}
static void surface_leave(void* d,struct wl_surface* s,struct wl_output* o){(void)d;(void)s;CHECK(o==output);}
static const struct wl_surface_listener surface_listener={.enter=surface_enter,.leave=surface_leave};
static void paint(void);
static void release(void* data,struct wl_buffer* b){(void)data;(void)b;busy=false;if(redraw)paint();}
static const struct wl_buffer_listener buffer_listener={release};
static void paint(void){
    if(busy){redraw=true;return;}
    if(!buffer){int fd=memfd_create("nexus-window-test",MFD_CLOEXEC);CHECK(fd>=0&&!ftruncate(fd,(size_t)width*height*4));
        pixels=mmap(NULL,(size_t)width*height*4,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);CHECK(pixels!=MAP_FAILED);
        struct wl_shm_pool* pool=wl_shm_create_pool(shm,fd,width*height*4);CHECK(pool);buffer=wl_shm_pool_create_buffer(pool,0,width,height,width*4,WL_SHM_FORMAT_XRGB8888);CHECK(buffer);wl_shm_pool_destroy(pool);close(fd);wl_buffer_add_listener(buffer,&buffer_listener,NULL);}
    for(int y=0;y<height;y++)for(int x=0;x<width;x++)pixels[(size_t)y*width+x]=x>=64&&x<256&&y>=64&&y<192?(typed?0x18C784:0xD04040):x>=64&&x<256&&y>=256&&y<384?(clicked?0x2878DD:0xE59B35):0x0B1220;
    wl_surface_attach(surface,buffer,0,0);wl_surface_damage_buffer(surface,0,0,width,height);wl_surface_commit(surface);busy=true;redraw=false;frames++;
    printf("[64] WAYLAND CLIENT FRAME %u typed=%d clicked=%d\n",frames,typed,clicked);fflush(stdout);
}
static void configure_surface(void* data,struct xdg_surface* s,uint32_t serial){(void)data;xdg_surface_ack_configure(s,serial);paint();}
static const struct xdg_surface_listener xdg_listener={configure_surface};
static void configure_top(void* data,struct xdg_toplevel* top,int32_t w,int32_t h,struct wl_array* states){(void)data;(void)top;(void)states;CHECK(!buffer);if(w>0)width=w;if(h>0)height=h;}
static void close_top(void* data,struct xdg_toplevel* top){(void)data;(void)top;running=false;}
static const struct xdg_toplevel_listener top_listener={.configure=configure_top,.close=close_top};
static void ping(void* data,struct xdg_wm_base* wm,uint32_t serial){(void)data;xdg_wm_base_pong(wm,serial);}
static const struct xdg_wm_base_listener shell_listener={ping};
static void keymap(void* data,struct wl_keyboard* k,uint32_t format,int fd,uint32_t size){(void)data;(void)k;CHECK(format==WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1&&size>1);char* text=mmap(NULL,size,PROT_READ,MAP_PRIVATE,fd,0);CHECK(text!=MAP_FAILED&&text[size-1]==0);struct xkb_keymap* map=xkb_keymap_new_from_string(context,text,XKB_KEYMAP_FORMAT_TEXT_V1,XKB_KEYMAP_COMPILE_NO_FLAGS);CHECK(map);xkb_keymap_unref(map);CHECK(!munmap(text,size)&&!close(fd));keymaps++;}
static void enter(void* data,struct wl_keyboard* k,uint32_t serial,struct wl_surface* s,struct wl_array* held){(void)data;(void)k;(void)serial;(void)held;CHECK(s==surface);enters++;}
static void leave(void* data,struct wl_keyboard* k,uint32_t serial,struct wl_surface* s){(void)data;(void)k;(void)serial;(void)s;}
static void key(void* data,struct wl_keyboard* k,uint32_t serial,uint32_t time,uint32_t code,uint32_t state){(void)data;(void)k;(void)time;keys++;if(code==30&&state){typed=true;paint();clipboard_source=wl_data_device_manager_create_data_source(data_manager);CHECK(clipboard_source);wl_data_source_add_listener(clipboard_source,&source_listener,NULL);wl_data_source_offer(clipboard_source,"text/plain;charset=utf-8");wl_data_device_set_selection(data_device,clipboard_source,serial);puts("[64] WAYLAND CLIENT KEY PASSED");fflush(stdout);}}
static void modifiers(void* data,struct wl_keyboard* k,uint32_t serial,uint32_t a,uint32_t b,uint32_t c,uint32_t d){(void)data;(void)k;(void)serial;(void)a;(void)b;(void)c;(void)d;}
static void repeat(void* data,struct wl_keyboard* k,int32_t rate,int32_t delay){(void)data;(void)k;CHECK(rate>0&&delay>0);}
static const struct wl_keyboard_listener keyboard_listener={keymap,enter,leave,key,modifiers,repeat};
static void pointer_enter(void* d,struct wl_pointer* p,uint32_t s,struct wl_surface* w,wl_fixed_t x,wl_fixed_t y){(void)d;(void)p;(void)s;(void)x;(void)y;CHECK(w==surface);}
static void pointer_leave(void* d,struct wl_pointer* p,uint32_t s,struct wl_surface* w){(void)d;(void)p;(void)s;(void)w;}
static void motion(void* d,struct wl_pointer* p,uint32_t time,wl_fixed_t x,wl_fixed_t y){(void)d;(void)p;(void)time;(void)x;(void)y;}
static void button(void* d,struct wl_pointer* p,uint32_t serial,uint32_t time,uint32_t button,uint32_t state){(void)d;(void)p;(void)serial;(void)time;if(button==272&&state){clicked=true;clicks++;paint();puts("[64] WAYLAND CLIENT POINTER PASSED");fflush(stdout);}}
static void axis(void* d,struct wl_pointer* p,uint32_t time,uint32_t axis,wl_fixed_t value){(void)d;(void)p;(void)time;(void)axis;(void)value;}
static void pointer_frame(void* d,struct wl_pointer* p){(void)d;(void)p;}
static const struct wl_pointer_listener pointer_listener={.enter=pointer_enter,.leave=pointer_leave,.motion=motion,.button=button,.axis=axis,.frame=pointer_frame};
static void capabilities(void* d,struct wl_seat* s,uint32_t caps){(void)d;CHECK((caps&3)==3);struct wl_keyboard* k=wl_seat_get_keyboard(s);struct wl_pointer* p=wl_seat_get_pointer(s);CHECK(k&&p);wl_keyboard_add_listener(k,&keyboard_listener,NULL);wl_pointer_add_listener(p,&pointer_listener,NULL);}
static void seat_name(void* d,struct wl_seat* s,const char* name){(void)d;(void)s;CHECK(name&&*name);}
static const struct wl_seat_listener seat_listener={capabilities,seat_name};
static void global(void* data,struct wl_registry* registry,uint32_t name,const char* interface,uint32_t version){(void)data;(void)version;
    if(!strcmp(interface,"wl_compositor"))compositor=wl_registry_bind(registry,name,&wl_compositor_interface,4);
    if(!strcmp(interface,"wl_shm"))shm=wl_registry_bind(registry,name,&wl_shm_interface,1);
    if(!strcmp(interface,"xdg_wm_base")){shell=wl_registry_bind(registry,name,&xdg_wm_base_interface,1);xdg_wm_base_add_listener(shell,&shell_listener,NULL);}
    if(!strcmp(interface,"wl_seat")){seat=wl_registry_bind(registry,name,&wl_seat_interface,5);wl_seat_add_listener(seat,&seat_listener,NULL);}
    if(!strcmp(interface,"wl_data_device_manager"))data_manager=wl_registry_bind(registry,name,&wl_data_device_manager_interface,1);
    if(!strcmp(interface,"wl_output")){output=wl_registry_bind(registry,name,&wl_output_interface,3);wl_output_add_listener(output,&output_listener,NULL);}
}
static void remove_global(void* data,struct wl_registry* registry,uint32_t name){(void)data;(void)registry;(void)name;}
static const struct wl_registry_listener registry_listener={global,remove_global};
int main(void){
    context=xkb_context_new(XKB_CONTEXT_NO_FLAGS);CHECK(context);display=wl_display_connect(NULL);CHECK(display);
    struct wl_registry* registry=wl_display_get_registry(display);CHECK(registry);wl_registry_add_listener(registry,&registry_listener,NULL);CHECK(wl_display_roundtrip(display)>=0&&compositor&&shm&&shell&&seat&&data_manager);
    data_device=wl_data_device_manager_get_data_device(data_manager,seat);CHECK(data_device);wl_data_device_add_listener(data_device,&device_listener,NULL);
    surface=wl_compositor_create_surface(compositor);CHECK(surface&&output);wl_surface_add_listener(surface,&surface_listener,NULL);xdg=xdg_wm_base_get_xdg_surface(shell,surface);CHECK(xdg);xdg_surface_add_listener(xdg,&xdg_listener,NULL);
    struct xdg_toplevel* top=xdg_surface_get_toplevel(xdg);CHECK(top);xdg_toplevel_add_listener(top,&top_listener,NULL);xdg_toplevel_set_title(top,"NexusOS protocol test");wl_surface_commit(surface);
    while(running){CHECK(wl_display_dispatch(display)>=0);if(transfer_sent&&!copied){char text[sizeof(clipboard_text)];CHECK(read(clipboard_fd,text,sizeof(text))==sizeof(text)&&!memcmp(text,clipboard_text,sizeof(text)));CHECK(!close(clipboard_fd));copied=true;puts("[64] WAYLAND CLIENT CLIPBOARD PASSED");fflush(stdout);}}
    CHECK(keymaps==1&&enters>=1&&typed&&clicked&&copied&&keys>=2&&clicks==1&&frames>=3&&output_enters==1);
    xdg_toplevel_destroy(top);xdg_surface_destroy(xdg);wl_surface_destroy(surface);wl_buffer_destroy(buffer);CHECK(wl_display_roundtrip(display)>=0);wl_display_disconnect(display);
    CHECK(!munmap(pixels,(size_t)width*height*4));xkb_context_unref(context);
    puts("[64] WAYLAND CLIENT ALL PASSED: configure/output, real SHM frames, keymap FD, keyboard, pointer, clipboard, close and cleanup");return 0;
}
