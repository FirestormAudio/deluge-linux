#include "text.h"
#include "metric9.h"
#include "apple7.h"

const struct sfont FONT_METRIC9 = {
    METRIC9_BITMAP, METRIC9_GLYPHS, METRIC9_COUNT, METRIC9_HEIGHT, METRIC9_FIRST, 1, 1
};
const struct sfont FONT_APPLE7 = {
    APPLE7_BITMAP, APPLE7_GLYPHS, APPLE7_COUNT, APPLE7_HEIGHT, APPLE7_FIRST, 1, 1
};

/* char -> glyph index, or -1 if unsupported. */
static int glyph_index(const struct sfont *fo, unsigned char c) {
    if (fo->uppercase && c >= 'a' && c <= 'z') c -= 32;
    int idx = (int)c - fo->first;
    return (idx < 0 || idx >= fo->count) ? -1 : idx;
}

/* Draw glyph idx with top-left at (x,y); returns its pixel width. */
static int draw_glyph(struct fb *f, int x, int y, const struct sfont *fo, int idx) {
    const struct glyph *g = &fo->glyphs[idx];
    int bpr = (g->w + 7) / 8;
    for (int row = 0; row < fo->height; row++)
        for (int col = 0; col < g->w; col++) {
            unsigned char byte = fo->bitmap[g->off + row * bpr + (col >> 3)];
            if ((byte >> (col & 7)) & 1)
                fb_set_pixel(f, x + col, y + row, 1);
        }
    return g->w;
}

int font_text_width(const struct sfont *fo, const char *s) {
    int w = 0, first = 1;
    for (; *s; s++) {
        int idx = glyph_index(fo, (unsigned char)*s);
        if (idx < 0) continue;
        if (!first) w += fo->spacing;
        w += fo->glyphs[idx].w;
        first = 0;
    }
    return w;
}

void font_draw_centered(struct fb *f, int y, const struct sfont *fo, const char *s) {
    int x = (FB_W - font_text_width(fo, s)) / 2;
    int first = 1;
    for (; *s; s++) {
        int idx = glyph_index(fo, (unsigned char)*s);
        if (idx < 0) continue;
        if (!first) x += fo->spacing;
        x += draw_glyph(f, x, y, fo, idx);
        first = 0;
    }
}
