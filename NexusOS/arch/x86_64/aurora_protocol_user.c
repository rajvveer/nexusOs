/* Native Aurora regression client; the host supplies only physical PS/2 input.
 * After AUTOMATIC PASSED, use the second window's latest AURORA WINDOW origin:
 * 1. Press/release at local 80,80; wait FORGED REJECTED before releasing.
 * 2. Press there, wait MOVE GRABBED, move +80,+40, release; wait MOVE PASSED.
 * 3. Press at local 80,80, wait RESIZE GRABBED, move +400,+300, release,
 *    then return to local 80,80 inside the still-old 360x240 buffer. The fixture
 *    verifies pointer re-entry before drawing the newly configured buffer.
 * 4. At MENU READY hold Shift, click Nexus menu (16,14), wait MENU INACTIVE;
 *    release Shift, press/release Escape; wait MENU PASSED.
 * 5. Press window two at local 80,80, wait UNMAP CANCELLED, release, type u.
 *    Window one underneath must never receive the unmatched button release.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"[64] AURORA PROTOCOL FAIL line=%d\n",__LINE__); exit(1); } } while (0)
enum phase { AUTOMATIC, FORGED_READY, FORGED_PENDING, FORGED_RELEASE, MOVE_READY, MOVING,
             RESIZE_READY, RESIZING, MENU_READY, MENU_WAIT, CANCEL_READY, CANCELLING, CANCEL_WAIT, FINISHED };
struct window {
    struct wl_surface* surface; struct xdg_surface* xdg; struct xdg_toplevel* top; struct wl_buffer* buffer;
    uint32_t* pixels; size_t bytes;
    int width,height,configured_w,configured_h;
    unsigned configurations; bool active,fullscreen,maximized,resizing,busy,left_down;
};
static struct window windows[2];
static struct wl_display* display;
static struct wl_compositor* compositor;
static struct wl_shm* shm;
static struct xdg_wm_base* shell;
static struct wl_seat* seat;
static struct wl_pointer* pointer;
static struct wl_keyboard* keyboard;
static struct wl_registry* registry;
static struct window *pointer_window,*keyboard_window;
static enum phase phase;
static unsigned pointer_leaves,forged_configurations;
static bool interactive_left,interactive_entered,saw_resizing,menu_inactive,menu_left,menu_entered,menu_clean_modifiers;

static void mark(const char* text) { printf("[64] AURORA PROTOCOL %s\n",text); fflush(stdout); }
static void settle(void) { for (unsigned i=0;i<4;i++) CHECK(wl_display_roundtrip(display)>=0); }
static struct window* from_surface(struct wl_surface* surface) {
    for (unsigned i=0;i<2;i++) if (windows[i].surface==surface) return windows+i;
    CHECK(false); return NULL;
}
static void released(void* data,struct wl_buffer* buffer) { (void)buffer; ((struct window*)data)->busy=false; }
static const struct wl_buffer_listener buffer_listener={released};
static void draw(struct window* w,int width,int height,uint32_t color) {
    CHECK(!w->busy && width>0 && height>0 && width<=1920 && height<=1080);
    if (w->buffer && (width!=w->width || height!=w->height)) {
        wl_buffer_destroy(w->buffer); CHECK(!munmap(w->pixels,w->bytes)); w->buffer=NULL;
    }
    if (!w->buffer) {
        w->width=width; w->height=height; w->bytes=(size_t)width*height*4;
        int fd=memfd_create("aurora-protocol",MFD_CLOEXEC); CHECK(fd>=0 && !ftruncate(fd,w->bytes));
        w->pixels=mmap(NULL,w->bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0); CHECK(w->pixels!=MAP_FAILED);
        struct wl_shm_pool* pool=wl_shm_create_pool(shm,fd,(int)w->bytes); CHECK(pool);
        w->buffer=wl_shm_pool_create_buffer(pool,0,width,height,width*4,WL_SHM_FORMAT_XRGB8888); CHECK(w->buffer);
        wl_buffer_add_listener(w->buffer,&buffer_listener,w); wl_shm_pool_destroy(pool); CHECK(!close(fd));
    }
    for (int y=0;y<height;y++) for (int x=0;x<width;x++) w->pixels[(size_t)y*width+x]=y<28?0xF0F0F0:color;
    xdg_surface_set_window_geometry(w->xdg,0,0,width,height);
    wl_surface_attach(w->surface,w->buffer,0,0); wl_surface_damage_buffer(w->surface,0,0,width,height); wl_surface_commit(w->surface); w->busy=true;
}
static void configured(void* data,struct xdg_surface* surface,uint32_t serial) {
    struct window* w=data; xdg_surface_ack_configure(surface,serial); w->configurations++;
}
static const struct xdg_surface_listener xdg_listener={configured};
static void top_configured(void* data,struct xdg_toplevel* top,int32_t width,int32_t height,struct wl_array* states) {
    (void)top; struct window* w=data; CHECK(width>=0 && height>=0);
    w->configured_w=width; w->configured_h=height; w->active=w->fullscreen=w->maximized=w->resizing=false;
    uint32_t* state; wl_array_for_each(state,states) {
        if (*state==XDG_TOPLEVEL_STATE_ACTIVATED) w->active=true;
        if (*state==XDG_TOPLEVEL_STATE_FULLSCREEN) w->fullscreen=true;
        if (*state==XDG_TOPLEVEL_STATE_MAXIMIZED) w->maximized=true;
        if (*state==XDG_TOPLEVEL_STATE_RESIZING) w->resizing=true;
    }
    if (phase==RESIZING && w==windows+1 && w->resizing) saw_resizing=true;
    if (phase==MENU_WAIT && w==windows+1 && !w->active) menu_inactive=true;
}
static void top_closed(void* data,struct xdg_toplevel* top) { (void)data;(void)top;CHECK(phase==FINISHED); }
static const struct xdg_toplevel_listener top_listener={.configure=top_configured,.close=top_closed};
static void ping(void* data,struct xdg_wm_base* wm,uint32_t serial) { (void)data;xdg_wm_base_pong(wm,serial); }
static const struct xdg_wm_base_listener shell_listener={ping};
static void keymap(void* data,struct wl_keyboard* k,uint32_t format,int fd,uint32_t size) {
    (void)data;(void)k; CHECK(format==WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && size>1 && !close(fd));
}
static void keyboard_enter(void* data,struct wl_keyboard* k,uint32_t serial,struct wl_surface* surface,struct wl_array* held) {
    (void)data;(void)k;(void)serial; keyboard_window=from_surface(surface);
    if (phase==MENU_WAIT && menu_left) {
        CHECK(keyboard_window==windows+1);
        uint32_t* key; wl_array_for_each(key,held) CHECK(*key!=42 && *key!=54);
        menu_entered=true;
    }
}
static void keyboard_leave(void* data,struct wl_keyboard* k,uint32_t serial,struct wl_surface* surface) {
    (void)data;(void)k;(void)serial; if (phase==FINISHED) { keyboard_window=NULL;return; }
    CHECK(keyboard_window==from_surface(surface)); keyboard_window=NULL;
    if (phase==MENU_WAIT) menu_left=true;
}
static void keyboard_key(void* data,struct wl_keyboard* k,uint32_t serial,uint32_t time,uint32_t key,uint32_t state) {
    (void)data;(void)k;(void)serial;(void)time; CHECK(keyboard_window);
    if (phase==MENU_READY && key==42 && state) { phase=MENU_WAIT; mark("SHIFT DOWN"); }
    if (phase==CANCEL_WAIT && key==22 && state) { CHECK(keyboard_window==windows && !windows[0].left_down); phase=FINISHED; }
}
static void keyboard_modifiers(void* data,struct wl_keyboard* k,uint32_t serial,uint32_t depressed,uint32_t latched,uint32_t locked,uint32_t group) {
    (void)data;(void)k;(void)serial;(void)group;
    if (phase==MENU_WAIT && menu_entered) { CHECK(!depressed && !latched && !locked); menu_clean_modifiers=true; }
}
static void repeat(void* data,struct wl_keyboard* k,int32_t rate,int32_t delay) { (void)data;(void)k;CHECK(rate>0 && delay>0); }
static const struct wl_keyboard_listener keyboard_listener={keymap,keyboard_enter,keyboard_leave,keyboard_key,keyboard_modifiers,repeat};
static void pointer_enter(void* data,struct wl_pointer* p,uint32_t serial,struct wl_surface* surface,wl_fixed_t x,wl_fixed_t y) {
    (void)data;(void)p;(void)serial;(void)x;(void)y; pointer_window=from_surface(surface);
    if ((phase==MOVING || phase==RESIZING) && interactive_left) { CHECK(pointer_window==windows+1); interactive_entered=true; }
}
static void pointer_leave(void* data,struct wl_pointer* p,uint32_t serial,struct wl_surface* surface) {
    (void)data;(void)p;(void)serial; if (phase==FINISHED) { pointer_window=NULL;return; }
    CHECK(pointer_window==from_surface(surface));
    pointer_window->left_down=false; pointer_window=NULL; pointer_leaves++;
    if (phase==MOVING || phase==RESIZING || phase==CANCELLING) {
        interactive_left=true; mark(phase==MOVING?"MOVE GRABBED":phase==RESIZING?"RESIZE GRABBED":"CANCEL GRABBED");
    }
}
static void motion(void* data,struct wl_pointer* p,uint32_t time,wl_fixed_t x,wl_fixed_t y) {
    (void)data;(void)p;(void)time;(void)x;(void)y; CHECK(pointer_window);
    CHECK(!((phase==MOVING || phase==RESIZING || phase==CANCELLING) && interactive_left && !interactive_entered));
}
static void button(void* data,struct wl_pointer* p,uint32_t serial,uint32_t time,uint32_t button,uint32_t state) {
    (void)data;(void)p;(void)time; CHECK(pointer_window); if (button!=272) return;
    if (!state) {
        CHECK(pointer_window->left_down); pointer_window->left_down=false;
        if (phase==FORGED_RELEASE) { phase=MOVE_READY; mark("MOVE READY local=80,80 delta=80,40"); }
        return;
    }
    pointer_window->left_down=true;
    if (phase==FORGED_READY) {
        CHECK(pointer_window==windows+1); forged_configurations=windows[1].configurations;
        pointer_leaves=0; xdg_toplevel_move(windows[1].top,seat,serial+1234);
        xdg_toplevel_resize(windows[1].top,seat,serial+2345,XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT); phase=FORGED_PENDING;
    } else if (phase==MOVE_READY || phase==RESIZE_READY || phase==CANCEL_READY) {
        CHECK(pointer_window==windows+1); interactive_left=interactive_entered=false;
        if (phase==RESIZE_READY) { phase=RESIZING;saw_resizing=false;xdg_toplevel_resize(windows[1].top,seat,serial,XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT); }
        else { phase=phase==MOVE_READY?MOVING:CANCELLING;xdg_toplevel_move(windows[1].top,seat,serial); }
    }
}
static void axis(void* data,struct wl_pointer* p,uint32_t time,uint32_t axis,wl_fixed_t value) { (void)data;(void)p;(void)time;(void)axis;(void)value; }
static void pointer_frame(void* data,struct wl_pointer* p) { (void)data;(void)p; }
static const struct wl_pointer_listener pointer_listener={.enter=pointer_enter,.leave=pointer_leave,.motion=motion,.button=button,.axis=axis,.frame=pointer_frame};
static void capabilities(void* data,struct wl_seat* s,uint32_t caps) {
    (void)data;CHECK((caps&3)==3);
    if (!keyboard) { keyboard=wl_seat_get_keyboard(s);CHECK(keyboard);wl_keyboard_add_listener(keyboard,&keyboard_listener,NULL); }
    if (!pointer) { pointer=wl_seat_get_pointer(s);CHECK(pointer);wl_pointer_add_listener(pointer,&pointer_listener,NULL); }
}
static void seat_name(void* data,struct wl_seat* s,const char* name) { (void)data;(void)s;CHECK(name && *name); }
static const struct wl_seat_listener seat_listener={capabilities,seat_name};
static void global(void* data,struct wl_registry* r,uint32_t name,const char* interface,uint32_t version) {
    (void)data;
    if (!strcmp(interface,"wl_compositor")) { CHECK(version>=4);compositor=wl_registry_bind(r,name,&wl_compositor_interface,4); }
    if (!strcmp(interface,"wl_shm")) shm=wl_registry_bind(r,name,&wl_shm_interface,1);
    if (!strcmp(interface,"xdg_wm_base")) { shell=wl_registry_bind(r,name,&xdg_wm_base_interface,1);xdg_wm_base_add_listener(shell,&shell_listener,NULL); }
    if (!strcmp(interface,"wl_seat")) { CHECK(version>=5);seat=wl_registry_bind(r,name,&wl_seat_interface,5);wl_seat_add_listener(seat,&seat_listener,NULL); }
}
static void removed(void* data,struct wl_registry* r,uint32_t name) { (void)data;(void)r;(void)name; }
static const struct wl_registry_listener registry_listener={global,removed};
static void create_window(struct window* w,const char* name) {
    w->surface=wl_compositor_create_surface(compositor);CHECK(w->surface);
    w->xdg=xdg_wm_base_get_xdg_surface(shell,w->surface);CHECK(w->xdg);xdg_surface_add_listener(w->xdg,&xdg_listener,w);
    w->top=xdg_surface_get_toplevel(w->xdg);CHECK(w->top);xdg_toplevel_add_listener(w->top,&top_listener,w);
    xdg_toplevel_set_title(w->top,name);xdg_toplevel_set_app_id(w->top,name);wl_surface_commit(w->surface);settle();
    CHECK(w->configurations && !w->configured_w && !w->configured_h && !w->maximized && !w->fullscreen);
    draw(w,360,240,w==windows?0x285A9F:0x24866A);settle();CHECK(w->active && keyboard_window==w);
}
int main(void) {
    display=wl_display_connect(NULL);CHECK(display);registry=wl_display_get_registry(display);CHECK(registry);
    wl_registry_add_listener(registry,&registry_listener,NULL);settle();CHECK(compositor && shm && shell && seat && pointer && keyboard);
    create_window(windows,"nexus.protocol.one");create_window(windows+1,"nexus.protocol.two");
    CHECK(!windows[0].active && windows[1].active);mark("ACTIVATION PASSED");
    unsigned configurations=windows[1].configurations;
    xdg_toplevel_resize(windows[1].top,seat,0,XDG_TOPLEVEL_RESIZE_EDGE_NONE);settle();
    CHECK(windows[1].configurations==configurations && !windows[1].resizing);mark("RESIZE NONE PASSED");
    xdg_toplevel_set_min_size(windows[1].top,600,400);xdg_toplevel_set_max_size(windows[1].top,700,500);
    xdg_toplevel_set_fullscreen(windows[1].top,NULL);settle();CHECK(windows[1].fullscreen);
    wl_surface_attach(windows[1].surface,NULL,0,0);wl_surface_commit(windows[1].surface);settle();
    CHECK(windows[0].active && keyboard_window==windows);
    configurations=windows[1].configurations;wl_surface_commit(windows[1].surface);settle();
    CHECK(windows[1].configurations>configurations && !windows[1].configured_w && !windows[1].configured_h && !windows[1].fullscreen && !windows[1].maximized && !windows[1].resizing);
    xdg_toplevel_set_title(windows[1].top,"nexus.protocol.two");xdg_toplevel_set_app_id(windows[1].top,"nexus.protocol.two");
    draw(windows+1,360,240,0x24866A);settle();CHECK(windows[1].active && !windows[0].active);
    mark("UNMAP RESET PASSED");phase=FORGED_READY;mark("AUTOMATIC PASSED; FORGED READY local=80,80");
    bool inactive_marked=false;
    while (phase!=FINISHED) {
        CHECK(wl_display_dispatch(display)>=0);
        if (phase==FORGED_PENDING) {
            settle();CHECK(!pointer_leaves && pointer_window==windows+1 && !windows[1].resizing && windows[1].configurations==forged_configurations);
            phase=FORGED_RELEASE;mark("FORGED REJECTED");
        } else if (phase==MOVING && interactive_entered) {
            settle();CHECK(interactive_left && !windows[1].resizing);phase=RESIZE_READY;mark("MOVE PASSED; RESIZE READY local=80,80 delta=400,300");
        } else if (phase==RESIZING && interactive_entered) {
            settle();CHECK(saw_resizing && !windows[1].resizing && windows[1].configured_w==760 && windows[1].configured_h==540);
            draw(windows+1,760,540,0x24866A);settle();phase=MENU_READY;mark("RESIZE PASSED; MENU READY");
        } else if (phase==MENU_WAIT && menu_inactive && menu_left && !inactive_marked) {
            inactive_marked=true;mark("MENU INACTIVE");
        }
        if (phase==MENU_WAIT && menu_entered && menu_clean_modifiers) {
            settle();CHECK(menu_inactive && windows[1].active && !windows[0].active);
            phase=CANCEL_READY;mark("MENU PASSED; CANCEL READY local=80,80");
        }
        if (phase==CANCELLING && interactive_left) {
            phase=CANCEL_WAIT;
            wl_surface_attach(windows[1].surface,NULL,0,0);wl_surface_commit(windows[1].surface);settle();
            CHECK(windows[0].active && keyboard_window==windows);mark("UNMAP CANCELLED; RELEASE THEN TYPE u");
        }
    }
    for (unsigned i=0;i<2;i++) {
        xdg_toplevel_destroy(windows[i].top);xdg_surface_destroy(windows[i].xdg);wl_surface_destroy(windows[i].surface);
        wl_buffer_destroy(windows[i].buffer);CHECK(!munmap(windows[i].pixels,windows[i].bytes));
    }
    settle();wl_pointer_release(pointer);wl_keyboard_release(keyboard);wl_seat_destroy(seat);
    xdg_wm_base_destroy(shell);wl_shm_destroy(shm);wl_compositor_destroy(compositor);wl_registry_destroy(registry);settle();wl_display_disconnect(display);
    mark("ALL PASSED: activation, valid edge 0, forged serials, move/resize focus, coalesced size, menu modifiers and unmap reset/cancellation");
    return 0;
}
