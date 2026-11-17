#include <assert.h>
#include <stdio.h>
#include "../arch/x86_64/popup_position.h"
int main(void){
    struct popup_rules r={.w=60,.h=40,.x=80,.y=70,.rw=20,.rh=10,.anchor=8,.gravity=8};struct popup_box b;
    assert(popup_place(&r,0,0,200,150,&b)&&b.x==100&&b.y==80&&b.w==60&&b.h==40);
    r.adjust=12;assert(popup_place(&r,0,0,120,90,&b)&&b.x==20&&b.y==30);
    r.adjust=3;assert(popup_place(&r,0,0,120,90,&b)&&b.x==60&&b.y==50);
    r.adjust=48;assert(popup_place(&r,0,0,120,90,&b)&&b.x==100&&b.y==80&&b.w==20&&b.h==10);
    r.adjust=63;r.w=300;r.h=200;assert(popup_place(&r,0,0,120,90,&b)&&b.w==120&&b.h==90&&b.x==0&&b.y==0);
    r=(struct popup_rules){.w=9,.h=7,.x=10,.y=20,.rw=5,.rh=3};
    assert(popup_place(&r,-200,-200,200,200,&b)&&b.x==8&&b.y==18);
    r.ox=INT32_MAX;r.x=INT32_MAX;assert(!popup_place(&r,0,0,200,200,&b));
    r=(struct popup_rules){.w=100,.h=80,.rw=1,.rh=1,.adjust=63};
    assert(!popup_place(&r,INT64_MIN,0,INT64_MIN+1,100,&b));
    r.anchor=9;assert(!popup_place(&r,0,0,10,10,&b));r.anchor=0;r.w=0;assert(!popup_place(&r,0,0,10,10,&b));
    /* Every legal edge and adjustment combination stays inside the output when all adjustments are enabled. */
    for(unsigned a=0;a<9;a++)for(unsigned g=0;g<9;g++)for(int x=-200;x<=200;x+=37){
        r=(struct popup_rules){.w=60,.h=40,.x=20,.y=20,.rw=20,.rh=10,.ox=x,.oy=-x,.anchor=a,.gravity=g,.adjust=63};
        assert(popup_place(&r,0,0,120,90,&b)&&b.x>=0&&b.y>=0&&(int64_t)b.x+b.w<=120&&(int64_t)b.y+b.h<=90);
    }
    puts("PASS popup geometry: anchor/gravity, flip-slide-resize precedence, clipping and integer boundaries");
}
