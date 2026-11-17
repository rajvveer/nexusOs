#define _POSIX_C_SOURCE 200809L
#include "../arch/x86_64/aurora_graphics.h"
#include "../kernel/framebuffer.h"
#include "../kernel/gui.h"
#include "../kernel/gfx.h"
#include "../kernel/theme.h"
#include "../kernel/appui.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void* wallpaper;
static size_t wallpaper_bytes;
static bool fail_allocation;
static void* wallpaper_realloc(void* previous, size_t bytes) {
    assert(previous == wallpaper);
    if (fail_allocation) return NULL;
    void* next = realloc(previous, bytes);
    if (next) { wallpaper = next; wallpaper_bytes = bytes; }
    return next;
}
static void wallpaper_free(void* allocation) {
    assert(allocation == wallpaper);
    free(allocation); wallpaper = NULL; wallpaper_bytes = 0;
}
#define realloc wallpaper_realloc
#define free wallpaper_free
#include "../kernel/macui.c"
#undef realloc
#undef free

static unsigned dirty, presented;
void fb_mark_dirty(int x, int y, int w, int h) { (void)x; (void)y; if (w > 0 && h > 0) dirty++; }
void fb_mark_dirty_all(void) { dirty++; }
void fb_flip(void) { assert(fb_get_backbuffer()); presented++; }
bool window_content_px(int id, int* x, int* y, int* w, int* h) {
    if (id != 1) return false;
    *x = *y = 0; *w = (int)fb_get_width(); *h = (int)fb_get_height(); return true;
}

static void rectangles(void) {
    uint32_t guarded[98];
    for (unsigned i = 0; i < 98; i++) guarded[i] = 0xDEADBEEF;
    aurora_canvas(guarded + 1, 12, 8);
    assert(fb_is_vesa() && fb_get_width() == 12 && fb_get_height() == 8 && fb_get_pitch() == 48 && fb_get_bpp() == 32 && fb_get_lfb_size() == 384);
    fb_clear(0); assert(dirty);
    fb_fill_rect(-2, -1, 5, 4, 0x123456);
    for (int y = 0; y < 8; y++) for (int x = 0; x < 12; x++) assert(fb_getpixel(x, y) == (x < 3 && y < 3 ? 0x123456U : 0));
    fb_clear(0); fb_draw_rect(-1, -1, 4, 4, 7);
    for (int y = 0; y < 8; y++) for (int x = 0; x < 12; x++) assert(fb_getpixel(x, y) == ((x == 2 && y <= 2) || (y == 2 && x <= 2) ? 7U : 0));
    fb_fill_rect(INT_MAX, INT_MAX, INT_MAX, INT_MAX, 9);
    fb_draw_rect(INT_MIN, INT_MIN, INT_MAX, INT_MAX, 9);
    fb_fill_rect(0, 0, -1, 4, 9); assert(fb_getpixel(0, 0) == 0);
    fb_fill_rect(-1, -1, INT_MAX, INT_MAX, 0x123456);
    for (unsigned i = 1; i < 97; i++) assert(guarded[i] == 0x123456);
    uint32_t saved[20]; for (unsigned i = 0; i < 20; i++) saved[i] = 0xBAD;
    fb_save_rect(-2, -1, 5, 4, saved);
    for (unsigned i = 0; i < 20; i++) assert(saved[i] == (i / 5 >= 1 && i % 5 >= 2 ? 0x123456U : 0xBADU));
    fb_clear(0); fb_restore_rect(-2, -1, 5, 4, saved);
    for (int y = 0; y < 8; y++) for (int x = 0; x < 12; x++) assert(fb_getpixel(x, y) == (x < 3 && y < 3 ? 0x123456U : 0));
    fb_putpixel(11, 7, 0xFEED); fb_putpixel(12, 7, 1); fb_putpixel(-1, 0, 1);
    assert(fb_getpixel(11, 7) == 0xFEED && !fb_getpixel(-1, 0) && !fb_getpixel(12, 7));
    assert(guarded[0] == 0xDEADBEEF && guarded[97] == 0xDEADBEEF);
    aurora_canvas(NULL, 0, 0); assert(!fb_is_vesa() && !fb_get_backbuffer() && !fb_get_lfb_size());
}

static void registry(void) {
    int count; const ui_app_t* defaults = ui_apps(&count); int original = count;
    assert(count > 10 && ui_dock_app_count() == 10 && ui_app_by_title("Calculator"));
    const ui_app_t apps[] = {{"Chrome", 32, ICON_WEB, "Google Chrome"}, {"Notes", 10, ICON_NOTES, NULL}};
    assert(ui_set_apps(apps, 2, 1) && ui_apps(&count) == apps && count == 2 && ui_dock_app_count() == 1);
    assert(ui_app_by_title("Google Chrome") == apps && !ui_app_by_title("Calculator") && !ui_app_by_title(NULL));
    assert(!ui_set_apps(apps, -1, 0) && !ui_set_apps(apps, 2, -1) && !ui_set_apps(apps, 2, 3));
    assert(!ui_set_apps(NULL, 2, 0) && !ui_set_apps(NULL, 0, 1));
    const ui_app_t unnamed[] = {{NULL, 1, 0, NULL}, {"", 2, 0, NULL}};
    assert(!ui_set_apps(unnamed, 1, 0) && !ui_set_apps(unnamed + 1, 1, 0));
    assert(ui_apps(&count) == apps && count == 2);
    assert(ui_set_apps(apps, 0, 0) && ui_apps(&count) == apps && !count && !ui_dock_app_count());
    assert(ui_set_apps(NULL, 0, 0) && ui_apps(&count) == defaults && count == original && ui_dock_app_count() == 10);
}

static void toolkit(void) {
    static uint32_t small[96 * 64], large[192 * 128];
    aurora_canvas(small, 96, 64); ui_wallpaper_init();
    assert(wallpaper && wallpaper_bytes == sizeof(small));
    ui_wallpaper_draw(); assert(fb_getpixel(50, 30) == ui_wallpaper_at(50, 30) && fb_getpixel(0, 0) != fb_getpixel(0, 63));
    void* previous = wallpaper; fail_allocation = true;
    aurora_canvas(large, 192, 128); ui_wallpaper_init(); ui_wallpaper_draw();
    assert(wallpaper == previous && wallpaper_bytes == sizeof(small) && !ui_wallpaper_at(50, 30));
    assert(fb_getpixel(0, 0) != fb_getpixel(0, 127));
    fail_allocation = false; ui_wallpaper_init(); ui_wallpaper_draw();
    assert(wallpaper && wallpaper_bytes == sizeof(large) && fb_getpixel(50, 30) == ui_wallpaper_at(50, 30));
    ui_wallpaper_release(); ui_wallpaper_release(); assert(!wallpaper && !wallpaper_bytes && !ui_wallpaper_at(0, 0));
    gui_init(); assert(gui_is_vesa() && gui_cols == 24 && gui_rows == 8);
    uint32_t background = fb_getpixel(0, 0);
    gui_putchar(1, 1, 'A', 15); assert((gui_getchar(1, 1) & 255) == 'A');
    unsigned ink = 0; for (int y = 16; y < 32; y++) for (int x = 8; x < 16; x++) ink += fb_getpixel(x, y) != background;
    assert(ink > 10); gui_flip(); assert(presented == 1);
    gfx_fill_rect(40, 40, 12, 12, 0x112233); assert(fb_getpixel(45, 45) == 0x112233);
    appui_px_fill(60, 40, 12, 12, 0x223344); assert(fb_getpixel(65, 45) == 0x223344);
    assert(ICON_CHROME == ICON_GENERIC + 1 && ICON_WEB == 2);
    fb_clear(0x112233); ui_icon(ICON_CHROME, 32, 24, 64);
    assert(fb_getpixel(64, 56) == 0x4285F4 && fb_getpixel(64, 30) == 0xEA4335);
    assert(fb_getpixel(39, 65) == 0x34A853 && fb_getpixel(89, 65) == 0xFBBC05);
    assert(fb_getpixel(32, 24) == 0x112233 && fb_getpixel(31, 50) == 0x112233);
    ui_icon(ICON_CHROME, -20, -20, 32);
    theme_set(THEME_OCEAN); assert(theme_get_index() == THEME_OCEAN && !strcmp(theme_get_name(THEME_OCEAN), "ocean"));
    assert(vga_attr_fg(0xAF) == 0xF5F5F7 && vga_attr_bg(0xAF) == 0x32D75F);
    aurora_canvas(NULL, 0, 0);
}

static void clock_and_numbers(void) {
    struct timespec before, after; struct tm utc_before, utc_after; rtc_time_t actual;
    assert(!clock_gettime(CLOCK_REALTIME, &before)); rtc_read(&actual); assert(!clock_gettime(CLOCK_REALTIME, &after));
    assert(gmtime_r(&before.tv_sec, &utc_before) && gmtime_r(&after.tv_sec, &utc_after));
    bool matched = false;
    for (unsigned i = 0; i < 2; i++) {
        struct tm* utc = i ? &utc_after : &utc_before;
        matched |= actual.second == utc->tm_sec && actual.minute == utc->tm_min && actual.hour == utc->tm_hour &&
            actual.day == utc->tm_mday && actual.month == utc->tm_mon + 1 && actual.year == utc->tm_year + 1900;
    }
    assert(matched);
    char text[16]; rtc_time_t value = {.second=9,.minute=8,.hour=7,.day=6,.month=5,.year=2026};
    rtc_format_time(&value, text); assert(!strcmp(text, "07:08:09"));
    rtc_format_date(&value, text); assert(!strcmp(text, "06/05/2026"));
    int_to_str(INT_MIN, text); assert(!strcmp(text, "-2147483648"));
    int_to_str(INT_MAX, text); assert(!strcmp(text, "2147483647"));
    uint_to_str(UINT32_MAX, text); assert(!strcmp(text, "4294967295"));
    hex_to_str(UINT32_MAX, text); assert(!strcmp(text, "0xFFFFFFFF"));
    int_to_hex(0, text); assert(!strcmp(text, "0x00000000"));
}

static bool cursor_contains(int px, int py, int x, int y, bool arrow) {
    int offset = arrow ? 2 : 6;
    return px >= x - offset && py >= y - offset && px < x - offset + (arrow ? 20 : 13) && py < y - offset + (arrow ? 26 : 13);
}
static size_t cursor_bytes(int width, int height, int x, int y, bool arrow) {
    size_t pixels = 0;
    for (int row = 0; row < height; row++) for (int col = 0; col < width; col++) pixels += cursor_contains(col, row, x, y, arrow);
    return pixels * 4;
}
static void cursor_reference(uint32_t* result, const uint32_t* source, int width, int height, int x, int y, bool arrow) {
    memcpy(result, source, (size_t)width * height * 4); aurora_canvas(result, width, height);
    if (arrow) ui_cursor_draw(x - 1, y - 1);
    else for (int row = 0; row < height; row++) for (int col = 0; col < width; col++)
        if ((row == y && abs(col - x) <= 6) || (col == x && abs(row - y) <= 6)) result[row * width + col] = 0xFFFFFF;
}
static void cursor_presentation(void) {
    enum { W=67, H=53, STRIDE=74, GUARD=8 };
    const uint32_t sentinel = 0xD15EA5ED;
    uint32_t source[GUARD+W*H+GUARD], saved[W*H], expected[W*H], video[GUARD+STRIDE*H+GUARD];
    uint32_t* pixels=source+GUARD;uint32_t* display=video+GUARD;
    const int positions[][2]={{0,0},{1,1},{2,1},{W-1,0},{W-1,H-1},{0,H-1},{3,4},{3,4},{32,24},{33,25},{0,0}};
    for (unsigned style=0;style<2;style++) {
        bool arrow=style!=0;int old_x=0,old_y=0;
        for (unsigned pass=0;pass<2;pass++) {
            for (unsigned i=0;i<sizeof(source)/sizeof(*source);i++) source[i]=sentinel;
            for (int y=0;y<H;y++) for (int x=0;x<W;x++) pixels[y*W+x]=(uint32_t)(x*1703+y*593+pass*29713)&0xFFFFFF;
            memcpy(saved,pixels,sizeof(saved));
            for (unsigned i=0;i<sizeof(video)/sizeof(*video);i++) video[i]=sentinel;
            for (int y=0;y<H;y++) memcpy(display+y*STRIDE,pixels+y*W,W*4);
            for (unsigned i=0;i<sizeof(positions)/sizeof(*positions);i++) {
                int x=positions[i][0],y=positions[i][1];bool full=i==0;
                cursor_reference(expected,pixels,W,H,x,y,arrow);aurora_canvas(pixels,W,H);
                size_t bytes=aurora_present_cursor(display,STRIDE,full?INT_MIN:old_x,full?INT_MAX:old_y,x,y,full,arrow);
                assert(bytes==cursor_bytes(W,H,x,y,arrow)+(full?0:cursor_bytes(W,H,old_x,old_y,arrow)));
                assert(bytes<=(arrow?4160U:1352U) && !memcmp(pixels,saved,sizeof(saved)));
                for (int row=0;row<H;row++) {
                    assert(!memcmp(display+row*STRIDE,expected+row*W,W*4));
                    for (int col=W;col<STRIDE;col++) assert(display[row*STRIDE+col]==sentinel);
                }
                for (unsigned k=0;k<GUARD;k++) assert(source[k]==sentinel && source[GUARD+W*H+k]==sentinel && video[k]==sentinel && video[GUARD+STRIDE*H+k]==sentinel);
                old_x=x;old_y=y;
            }
        }
        for (unsigned full=0;full<2;full++) {
            for (unsigned i=0;i<sizeof(video)/sizeof(*video);i++) video[i]=sentinel;
            cursor_reference(expected,pixels,W,H,11,11,arrow);aurora_canvas(pixels,W,H);
            aurora_present_cursor(display,STRIDE,10,10,11,11,full!=0,arrow);
            for (int y=0;y<H;y++) for (int x=0;x<W;x++) {
                bool copied=cursor_contains(x,y,11,11,arrow)||(!full&&cursor_contains(x,y,10,10,arrow));
                assert(display[y*STRIDE+x]==(copied?expected[y*W+x]:sentinel));
            }
            assert(!memcmp(pixels,saved,sizeof(saved)));
        }
    }
    uint32_t tiny=0x123456,tiny_reference,guarded[]={0xAABBCCDD,0x123456,0xAABBCCDD};
    for (unsigned arrow=0;arrow<2;arrow++) {
        cursor_reference(&tiny_reference,&tiny,1,1,0,0,arrow!=0);aurora_canvas(&tiny,1,1);guarded[1]=tiny;
        assert(aurora_present_cursor(guarded+1,1,0,0,0,0,true,arrow!=0)==4);
        assert(tiny==0x123456 && guarded[1]==tiny_reference && guarded[0]==0xAABBCCDD && guarded[2]==0xAABBCCDD);
    }
    aurora_canvas(NULL,0,0);
    puts("PASS actual cursor presenter: full-reference pixels, unchanged scene, corners/tiny canvas/overlap/long jumps/stationary repaint, padded-stride canaries and clipped-copy byte counts for arrow/cross");
}

static void scene_presentation(void) {
    enum { W=137, H=33, STRIDE=144, GUARD=8 };
    const uint32_t sentinel=0xD15EA5ED;
    uint32_t source[GUARD+W*H+GUARD], previous[GUARD+W*H+GUARD], video[GUARD+STRIDE*H+GUARD];
    uint32_t expected[W*H], saved[W*H];
    uint32_t* pixels=source+GUARD;uint32_t* shadow=previous+GUARD;uint32_t* display=video+GUARD;
    for(unsigned i=0;i<sizeof(source)/sizeof(*source);i++)source[i]=previous[i]=sentinel;
    for(unsigned i=0;i<sizeof(video)/sizeof(*video);i++)video[i]=sentinel;
    for(int i=0;i<W*H;i++)pixels[i]=(uint32_t)i*1703&0xFFFFFF;
    aurora_canvas(pixels,W,H);
    assert(aurora_present_scene(display,STRIDE,shadow,true)==sizeof(saved));
    assert(!memcmp(pixels,shadow,sizeof(saved)) && aurora_present_scene(display,STRIDE,shadow,false)==0);
    const int points[][2]={{0,0},{63,1},{64,2},{127,3},{128,4},{W-1,H-1}};
    for(unsigned i=0;i<sizeof(points)/sizeof(*points);i++) {
        int x=points[i][0],y=points[i][1];pixels[y*W+x]^=0xF0F0F0;
        memcpy(saved,pixels,sizeof(saved));
        assert(aurora_present_scene(display,STRIDE,shadow,false)==(x<128?256U:36U));
        assert(!memcmp(pixels,saved,sizeof(saved)) && !memcmp(shadow,saved,sizeof(saved)));
        for(int row=0;row<H;row++)assert(!memcmp(display+row*STRIDE,saved+row*W,W*4));
        assert(aurora_present_scene(display,STRIDE,shadow,false)==0);
    }
    pixels[10*W+2]^=1;pixels[10*W+12]^=1;pixels[10*W+64]^=1;
    assert(aurora_present_scene(display,STRIDE,shadow,false)==512);

    /* Poison unchanged video pixels to prove the helper writes only reported chunks. */
    for(unsigned i=0;i<sizeof(video)/sizeof(*video);i++)video[i]=sentinel;
    assert(aurora_present_scene(display,STRIDE,shadow,false)==0);
    for(unsigned i=0;i<sizeof(video)/sizeof(*video);i++)assert(video[i]==sentinel);
    pixels[3*W+65]^=1;pixels[(H-1)*W+W-1]^=1;
    assert(aurora_present_scene(display,STRIDE,shadow,false)==256+36);
    assert(!memcmp(shadow,pixels,sizeof(saved)));
    for(int y=0;y<H;y++)for(int x=0;x<STRIDE;x++) {
        bool copied=(y==3&&x>=64&&x<128)||(y==H-1&&x>=128&&x<W);
        assert(display[y*STRIDE+x]==(copied?pixels[y*W+x]:sentinel));
    }
    assert(aurora_present_scene(display,STRIDE,shadow,true)==sizeof(saved));
    for(int i=0;i<W*H;i++)pixels[i]^=0xFFFFFF;
    assert(aurora_present_scene(display,STRIDE,shadow,false)==sizeof(saved));
    for(int row=0;row<H;row++)assert(!memcmp(display+row*STRIDE,pixels+row*W,W*4));

    const int positions[][2]={{0,0},{64,12},{W-1,H-1},{5,3}};
    for(unsigned style=0;style<2;style++) {
        int old_x=0,old_y=0;
        for(unsigned frame=0;frame<sizeof(positions)/sizeof(*positions);frame++) {
            int x=positions[frame][0],y=positions[frame][1];
            if(frame==1||frame==3)pixels[old_y*W+old_x]^=0x13579B;
            memcpy(saved,pixels,sizeof(saved));
            cursor_reference(expected,pixels,W,H,x,y,style!=0);aurora_canvas(pixels,W,H);
            size_t bytes=aurora_present_scene(display,STRIDE,shadow,frame==0);
            if(frame==2)assert(bytes==0);
            aurora_present_cursor(display,STRIDE,old_x,old_y,x,y,frame==0,style!=0);
            assert(!memcmp(pixels,saved,sizeof(saved))&&!memcmp(shadow,saved,sizeof(saved)));
            for(int row=0;row<H;row++)assert(!memcmp(display+row*STRIDE,expected+row*W,W*4));
            old_x=x;old_y=y;
        }
    }
    for(int row=0;row<H;row++)for(int x=W;x<STRIDE;x++)assert(display[row*STRIDE+x]==sentinel);
    for(unsigned i=0;i<GUARD;i++)assert(source[i]==sentinel&&source[GUARD+W*H+i]==sentinel&&
        previous[i]==sentinel&&previous[GUARD+W*H+i]==sentinel&&video[i]==sentinel&&video[GUARD+STRIDE*H+i]==sentinel);
    uint32_t tiny[]={sentinel,0x123456,sentinel},tiny_shadow[]={sentinel,0,sentinel},tiny_video[]={sentinel,0,sentinel};
    aurora_canvas(tiny+1,1,1);
    assert(aurora_present_scene(tiny_video+1,1,tiny_shadow+1,true)==4&&tiny_video[1]==tiny[1]&&tiny_shadow[1]==tiny[1]);
    assert(aurora_present_scene(tiny_video+1,1,tiny_shadow+1,false)==0);
    tiny[1]^=1;assert(aurora_present_scene(tiny_video+1,1,tiny_shadow+1,false)==4&&tiny_video[1]==tiny[1]&&tiny_shadow[1]==tiny[1]);
    assert(tiny[0]==sentinel&&tiny[2]==sentinel&&tiny_shadow[0]==sentinel&&tiny_shadow[2]==sentinel&&tiny_video[0]==sentinel&&tiny_video[2]==sentinel);
    aurora_canvas(NULL,0,0);
    puts("PASS actual scene presenter: full-reference pixels, unchanged zero writes, <=256-byte single-pixel updates, row tails, guarded strides/shadows and cursor-free delta composition without ghosts");
}

static void damage_presentation(void) {
    enum { W=67, H=41, STRIDE=72, GUARD=8 };
    const uint32_t sentinel=0xD15EA5ED;
    uint32_t source[W*H], saved[W*H], expected[W*H], under[UI_CURSOR_W*UI_CURSOR_H], video[GUARD+STRIDE*H+GUARD];
    uint32_t* display=video+GUARD;
    for(int i=0;i<W*H;i++)source[i]=(uint32_t)(i*2654435761u)&0xFFFFFF;
    memcpy(saved,source,sizeof(saved));aurora_canvas(source,W,H);
    const int rects[][4]={{0,0,W,H},{5,7,9,3},{-4,-6,10,10},{W-3,H-2,40,40},{20,30,0,5},{3,3,-1,4},{INT_MIN,INT_MIN,INT_MAX,INT_MAX},{W,0,5,5},{0,H,5,5}};
    for(unsigned k=0;k<sizeof(rects)/sizeof(*rects);k++){
        for(unsigned i=0;i<sizeof(video)/sizeof(*video);i++)video[i]=sentinel;
        int x=rects[k][0],y=rects[k][1],w=rects[k][2],h=rects[k][3];
        size_t bytes=aurora_present_rect(display,STRIDE,x,y,w,h),count=0;
        for(int row=0;row<H;row++)for(int col=0;col<STRIDE;col++){
            bool inside=col<W&&w>0&&h>0&&(int64_t)col>=x&&(int64_t)col<(int64_t)x+w&&(int64_t)row>=y&&(int64_t)row<(int64_t)y+h;
            assert(display[row*STRIDE+col]==(inside?source[row*W+col]:sentinel));count+=inside;
        }
        assert(bytes==count*4);
        for(unsigned g=0;g<GUARD;g++)assert(video[g]==sentinel&&video[GUARD+STRIDE*H+g]==sentinel);
    }
    for(unsigned arrow=0;arrow<2;arrow++){
        const int points[][2]={{0,0},{33,20},{W-1,H-1},{1,H-1}};
        for(unsigned p=0;p<sizeof(points)/sizeof(*points);p++){
            int px=points[p][0],py=points[p][1],bx,by,bw,bh;
            aurora_cursor_bounds(px,py,arrow!=0,&bx,&by,&bw,&bh);
            assert(bw==(arrow?UI_CURSOR_W:13)&&bh==(arrow?UI_CURSOR_H:13)&&bx==px-(arrow?2:6)&&by==py-(arrow?2:6));
            cursor_reference(expected,saved,W,H,px,py,arrow!=0);aurora_canvas(source,W,H);
            aurora_cursor_draw(px,py,arrow!=0,under);
            assert(!memcmp(source,expected,sizeof(expected)));
            for(unsigned i=0;i<sizeof(video)/sizeof(*video);i++)video[i]=sentinel;
            aurora_present_rect(display,STRIDE,bx,by,bw,bh);
            for(int row=0;row<H;row++)for(int col=0;col<W;col++)
                assert(display[row*STRIDE+col]==(col>=bx&&col<bx+bw&&row>=by&&row<by+bh?expected[row*W+col]:sentinel));
            aurora_cursor_restore(px,py,arrow!=0,under);
            assert(!memcmp(source,saved,sizeof(saved)));
        }
    }
    aurora_canvas(NULL,0,0);
    puts("PASS damage presenter: clipped rectangles copy exactly the reported bytes with padded-stride canaries; cursor draw/restore leaves the canvas unchanged and matches the reference sprite");
}

int main(int argc, char** argv) {
    if (argc > 1) {
        uint32_t pixels[16], video[16], previous[16];
        if (!strncmp(argv[1], "scene-", 6)) {
            aurora_canvas(pixels,4,4);
            uint32_t* display=!strcmp(argv[1],"scene-canvas")?pixels:video;
            uint32_t* shadow=!strcmp(argv[1],"scene-shadow")?pixels+1:!strcmp(argv[1],"scene-video")?video+1:previous;
            if(!strcmp(argv[1],"scene-null"))shadow=NULL;
            if(!strcmp(argv[1],"scene-range"))shadow=(uint32_t*)(UINTPTR_MAX-3);
            if(!strcmp(argv[1],"scene-detached"))aurora_canvas(NULL,0,0);
            size_t stride=!strcmp(argv[1],"scene-stride")?3:!strcmp(argv[1],"scene-overflow")?SIZE_MAX:4;
            aurora_present_scene(display,stride,shadow,true);
        } else if (!strncmp(argv[1], "rect-", 5)) {
            aurora_canvas(pixels,4,4);
            if(!strcmp(argv[1],"rect-detached"))aurora_canvas(NULL,0,0);
            aurora_present_rect(!strcmp(argv[1],"rect-alias")?pixels+2:video,!strcmp(argv[1],"rect-stride")?3:4,0,0,4,4);
        } else if (!strcmp(argv[1], "alias") || !strcmp(argv[1], "stride") || !strcmp(argv[1], "position")) {
            aurora_canvas(pixels,4,4);
            aurora_present_cursor(!strcmp(argv[1], "alias")?pixels:video,!strcmp(argv[1], "stride")?3:4,0,0,!strcmp(argv[1], "position")?4:0,0,false,true);
        } else aurora_canvas(pixels, !strcmp(argv[1], "negative") ? -1 : INT_MAX, INT_MAX);
        return 1;
    }
    rectangles(); registry(); toolkit(); clock_and_numbers(); cursor_presentation(); scene_presentation(); damage_presentation();
    puts("PASS Aurora graphics: clipped guarded canvas, original toolkit pixels, registry validation, owned wallpaper resize/failure/release, real UTC clock and integer boundaries");
    return 0;
}
