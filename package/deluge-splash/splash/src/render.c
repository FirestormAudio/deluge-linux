#include "fb.h"
#include "render.h"
#include "text.h"

/* App-loader fill formula: truncating (inner_w)*pct/100, no rounding. */
int bar_fill_width(int pct, int inner_w) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct * inner_w / 100;
}

void render_frame(struct fb *f, int pct, const char *version) {
    fb_clear(f);

    font_draw_centered(f, WORDMARK_Y, &FONT_METRIC9, SPLASH_NAME);

    fb_draw_rect(f, BAR_X, BAR_Y, BAR_W, BAR_H, 1);
    int fill = bar_fill_width(pct, BAR_INNER_W);
    if (fill > 0)
        fb_fill_rect(f, BAR_X + 1, BAR_Y + 1, fill, BAR_H - 2, 1);

    if (version && version[0])
        font_draw_centered(f, VERSION_Y, &FONT_APPLE7, version);
}
