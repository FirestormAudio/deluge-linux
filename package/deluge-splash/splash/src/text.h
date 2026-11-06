#ifndef TEXT_H
#define TEXT_H

#include "fb.h"

/* One glyph: pixel width and starting byte offset into a font bitmap. */
struct glyph { unsigned char w; unsigned short off; };

/* A proportional bitmap font. Glyphs are indexed char-`first`; rows are
   `bytes_per_row = (w+7)/8`, bit 0 = leftmost pixel. `uppercase` forces
   A-Z lookup (Metric/Apple have no lowercase glyphs). */
struct sfont {
    const unsigned char *bitmap;
    const struct glyph  *glyphs;
    int count, height, first, spacing, uppercase;
};

extern const struct sfont FONT_METRIC9;   /* uppercase DELUGE/LINUX wordmark */
extern const struct sfont FONT_APPLE7;    /* 5x7 version number */

/* Pixel width of s in font fo (sum of glyph widths + inter-glyph spacing). */
int  font_text_width(const struct sfont *fo, const char *s);

/* Draw s horizontally centered on the 128px width, top row at y. */
void font_draw_centered(struct fb *f, int y, const struct sfont *fo, const char *s);

#endif
