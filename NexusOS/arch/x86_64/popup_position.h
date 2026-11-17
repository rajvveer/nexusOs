#ifndef NEXUS_POPUP_POSITION_H
#define NEXUS_POPUP_POSITION_H
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
struct popup_rules { int32_t w,h,x,y,rw,rh,ox,oy; uint32_t anchor,gravity,adjust; bool reactive; };   /* reactive: xdg_positioner v3 */
struct popup_box { int32_t x,y,w,h; };
static int popup_direction(unsigned edge,bool vertical) {
    /* XDG wire values: none, top, bottom, left, right, top-left, bottom-left, top-right, bottom-right. */
    static const int horizontal[]={0,0,0,-1,1,-1,-1,1,1};
    static const int updown[]={0,-1,1,0,0,-1,1,-1,1};
    return vertical?updown[edge]:horizontal[edge];
}
static int64_t popup_axis(int64_t start,int64_t span,int64_t size,int anchor,int gravity,int64_t offset) {
    return start+(anchor<0?0:anchor>0?span:span/2)+offset-(gravity<0?size:gravity>0?0:size/2);
}
static bool popup_constrain(int64_t* position,int64_t* size,int64_t flipped,int64_t low,int64_t high,
                            int gravity,bool flip,bool slide,bool resize) {
    int64_t p=*position,n=*size,end,flipped_end;
    if(__builtin_add_overflow(p,n,&end)||__builtin_add_overflow(flipped,n,&flipped_end))return false;
    if(flip&&(p<low||end>high)&&flipped>=low&&flipped_end<=high)p=flipped;
    if(slide)for(unsigned pass=0;pass<2;pass++) {
        int64_t far;if(__builtin_sub_overflow(high,n,&far)||__builtin_add_overflow(p,n,&end))return false;
        bool positive=(gravity>=0)==(pass==0);
        if(positive&&p<low&&end<high){int64_t target=far<low?far:low;if(target>p)p=target;}
        if(!positive&&end>high&&p>low){int64_t target=far>low?far:low;if(target<p)p=target;}
    }
    if(resize){if(__builtin_add_overflow(p,n,&end))return false;if(p<low)p=low;if(end>high)end=high;
        if(p>=high)p=high-1;if(end<=p)end=p+1;n=end-p;}
    *position=p;*size=n;return true;
}
static bool popup_place(const struct popup_rules* r,int64_t low_x,int64_t low_y,int64_t high_x,int64_t high_y,struct popup_box* result) {
    if(r->w<=0||r->h<=0||r->rw<=0||r->rh<=0||r->anchor>8||r->gravity>8||(r->adjust&~63U)||low_x>=high_x||low_y>=high_y)return false;
    int ax=popup_direction(r->anchor,false),ay=popup_direction(r->anchor,true);
    int gx=popup_direction(r->gravity,false),gy=popup_direction(r->gravity,true);
    int64_t x=popup_axis(r->x,r->rw,r->w,ax,gx,r->ox),y=popup_axis(r->y,r->rh,r->h,ay,gy,r->oy),w=r->w,h=r->h;
    if(!popup_constrain(&x,&w,popup_axis(r->x,r->rw,w,-ax,-gx,r->ox),low_x,high_x,gx,r->adjust&4,r->adjust&1,r->adjust&16)||
       !popup_constrain(&y,&h,popup_axis(r->y,r->rh,h,-ay,-gy,r->oy),low_y,high_y,gy,r->adjust&8,r->adjust&2,r->adjust&32))return false;
    if(x<INT32_MIN||x>INT32_MAX||y<INT32_MIN||y>INT32_MAX||w<1||w>INT32_MAX||h<1||h>INT32_MAX)return false;
    *result=(struct popup_box){x,y,w,h};return true;
}
#endif
