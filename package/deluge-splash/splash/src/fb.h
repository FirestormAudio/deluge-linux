#ifndef DELUGE_FB_H
#define DELUGE_FB_H

#define FB_W 128
#define FB_H 43
#define FB_STRIDE (FB_W / 8)        /* 16 */
#define FB_SIZE (FB_STRIDE * FB_H)  /* 688 */

struct fb { unsigned char buf[FB_SIZE]; };

void fb_clear(struct fb *f);
void fb_set_pixel(struct fb *f, int x, int y, int on);
int  fb_get_pixel(const struct fb *f, int x, int y);
void fb_fill_rect(struct fb *f, int x, int y, int w, int h, int on);
void fb_draw_rect(struct fb *f, int x, int y, int w, int h, int on);
void fb_blit(struct fb *f, int x, int y, const unsigned char *bm, int bw, int bh);
int  fb_flush(struct fb *f, int fd);

#endif
