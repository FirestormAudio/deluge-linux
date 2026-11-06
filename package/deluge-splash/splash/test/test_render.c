#include "render.h"
#include "fb.h"
#include "text.h"
#include <assert.h>
#include <stdio.h>

static int rows_have_pixels(const struct fb *f, int y0, int y1) {
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = 0; x < FB_W; x++)
            if (fb_get_pixel(f, x, y)) n++;
    return n;
}

int main(void) {
    /* App-loader fill formula (truncating). */
    assert(bar_fill_width(0, BAR_INNER_W) == 0);
    assert(bar_fill_width(50, BAR_INNER_W) == 55);   /* 110*50/100 */
    assert(bar_fill_width(100, BAR_INNER_W) == 110);

    struct fb f;
    render_frame(&f, 50, "7.1.3");

    /* Bar outline corners present. */
    assert(fb_get_pixel(&f, BAR_X, BAR_Y));
    assert(fb_get_pixel(&f, BAR_X + BAR_W - 1, BAR_Y));
    assert(fb_get_pixel(&f, BAR_X, BAR_Y + BAR_H - 1));

    /* Fill reaches 55px and stops. */
    assert(fb_get_pixel(&f, BAR_X + 1, BAR_Y + 1));
    assert(fb_get_pixel(&f, BAR_X + 55, BAR_Y + 1));
    assert(!fb_get_pixel(&f, BAR_X + 56, BAR_Y + 1));

    /* Wordmark and version rows are non-empty. */
    assert(rows_have_pixels(&f, WORDMARK_Y, WORDMARK_Y + FONT_METRIC9.height) > 0);
    assert(rows_have_pixels(&f, VERSION_Y, VERSION_Y + FONT_APPLE7.height) > 0);

    printf("test_render OK\n");
    return 0;
}
