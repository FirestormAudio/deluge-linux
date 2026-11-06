#include "fb.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    struct fb f;

    /* clear -> all zero */
    fb_clear(&f);
    for (int i = 0; i < FB_SIZE; i++) assert(f.buf[i] == 0);

    /* MSB-first: leftmost pixel of a byte is bit7 */
    fb_clear(&f);
    fb_set_pixel(&f, 0, 0, 1);
    assert(f.buf[0] == 0x80);
    fb_set_pixel(&f, 7, 0, 1);
    assert(f.buf[0] == 0x81);
    fb_set_pixel(&f, 8, 0, 1);          /* next byte, leftmost */
    assert(f.buf[1] == 0x80);

    /* second row starts at stride */
    fb_clear(&f);
    fb_set_pixel(&f, 0, 1, 1);
    assert(f.buf[FB_STRIDE] == 0x80);

    /* get_pixel round-trip + clearing a pixel */
    assert(fb_get_pixel(&f, 0, 1) == 1);
    fb_set_pixel(&f, 0, 1, 0);
    assert(fb_get_pixel(&f, 0, 1) == 0);

    /* out-of-bounds is a no-op, never crashes */
    fb_clear(&f);
    fb_set_pixel(&f, -1, 0, 1);
    fb_set_pixel(&f, FB_W, 0, 1);
    fb_set_pixel(&f, 0, FB_H, 1);
    for (int i = 0; i < FB_SIZE; i++) assert(f.buf[i] == 0);
    assert(fb_get_pixel(&f, -5, -5) == 0);

    /* fill_rect sets exactly the covered pixels */
    fb_clear(&f);
    fb_fill_rect(&f, 2, 3, 4, 2, 1);
    assert(fb_get_pixel(&f, 2, 3) == 1);
    assert(fb_get_pixel(&f, 5, 4) == 1);
    assert(fb_get_pixel(&f, 6, 3) == 0);   /* just past width */
    assert(fb_get_pixel(&f, 2, 5) == 0);   /* just past height */

    /* draw_rect outlines only */
    fb_clear(&f);
    fb_draw_rect(&f, 0, 0, 4, 4, 1);
    assert(fb_get_pixel(&f, 0, 0) == 1);   /* corner */
    assert(fb_get_pixel(&f, 3, 3) == 1);   /* corner */
    assert(fb_get_pixel(&f, 1, 1) == 0);   /* interior empty */

    /* blit: a 1px-wide, 1px-tall bitmap (bit7 set) lands at (x,y) */
    fb_clear(&f);
    unsigned char one = 0x80;              /* 1 wide, MSB-first */
    fb_blit(&f, 10, 20, &one, 1, 1);
    assert(fb_get_pixel(&f, 10, 20) == 1);

    printf("test_fb ok\n");
    return 0;
}
