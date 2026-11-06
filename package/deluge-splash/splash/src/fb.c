#include "fb.h"
#include <string.h>
#include <unistd.h>

void fb_clear(struct fb *f) { memset(f->buf, 0, FB_SIZE); }

static int in_bounds(int x, int y) {
    return x >= 0 && x < FB_W && y >= 0 && y < FB_H;
}

void fb_set_pixel(struct fb *f, int x, int y, int on) {
    if (!in_bounds(x, y)) return;
    unsigned idx = (unsigned)y * FB_STRIDE + ((unsigned)x >> 3);
    unsigned char mask = (unsigned char)(0x80u >> (x & 7));
    if (on) f->buf[idx] |= mask;
    else    f->buf[idx] &= (unsigned char)~mask;
}

int fb_get_pixel(const struct fb *f, int x, int y) {
    if (!in_bounds(x, y)) return 0;
    unsigned idx = (unsigned)y * FB_STRIDE + ((unsigned)x >> 3);
    return (f->buf[idx] >> (7 - (x & 7))) & 1;
}

void fb_fill_rect(struct fb *f, int x, int y, int w, int h, int on) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            fb_set_pixel(f, x + i, y + j, on);
}

void fb_draw_rect(struct fb *f, int x, int y, int w, int h, int on) {
    for (int i = 0; i < w; i++) {
        fb_set_pixel(f, x + i, y, on);
        fb_set_pixel(f, x + i, y + h - 1, on);
    }
    for (int j = 0; j < h; j++) {
        fb_set_pixel(f, x, y + j, on);
        fb_set_pixel(f, x + w - 1, y + j, on);
    }
}

void fb_blit(struct fb *f, int x, int y, const unsigned char *bm, int bw, int bh) {
    int stride = (bw + 7) / 8;
    for (int j = 0; j < bh; j++)
        for (int i = 0; i < bw; i++) {
            int bit = (bm[j * stride + (i >> 3)] >> (7 - (i & 7))) & 1;
            if (bit) fb_set_pixel(f, x + i, y + j, 1);
        }
}

int fb_flush(struct fb *f, int fd) {
    if (lseek(fd, 0, SEEK_SET) < 0) return -1;
    ssize_t n = write(fd, f->buf, FB_SIZE);
    return (n == (ssize_t)FB_SIZE) ? 0 : -1;
}
