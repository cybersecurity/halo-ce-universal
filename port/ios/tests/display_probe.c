#include "halo_display.h"
#include <assert.h>
#include <stdio.h>

static void check(long dw,long dh,long logical,long aspect,long height,long limit,long w,long h) {
    struct halo_display_size size=halo_display_render_size(dw,dh,logical,aspect,height,limit);
    assert(size.width==w && size.height==h);
}
int main(void) {
    /* Native must use the full physical drawable, including when the Xbox
       layout has rounded the display's ratio to an even column count. */
    check(2752,2064,640,0,0,16384,2752,2064);
    check(2868,1320,1042,0,0,16384,2868,1320);
    check(2266,1488,730,0,0,16384,2266,1488);
    /* Performance presets preserve shape; original 4:3 must letterbox. */
    check(2752,2064,640,0,1080,16384,1440,1080);
    check(2868,1320,1042,0,720,16384,1564,720);
    check(2868,1320,640,640,0,16384,1760,1320);
    check(2752,2064,854,854,480,16384,854,480);
    /* Never supersample accidentally or exceed the GPU's texture limit. */
    check(1280,720,854,0,1080,16384,1280,720);
    check(7680,4320,854,0,0,4096,4096,2304);
    check(2752,2064,640,0,-1,16384,2752,2064);
    check(2752,2064,640,0,1,16384,320,240);
    check(0,0,640,0,0,16384,640,480);
    puts("PASS: native pixels, aspect preservation, resolution presets and GPU size limits");
    return 0;
}
