#include "text.h"
#include "fb.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

int main(void) {
    /* Known widths from the deluge-fonts descriptors. */
    assert(font_text_width(&FONT_METRIC9, "A") == 9);
    assert(font_text_width(&FONT_APPLE7,  "A") == 5);

    /* Spacing goes between glyphs: width("AA") == 2*w + spacing. */
    assert(font_text_width(&FONT_APPLE7, "AA") == 5 + 5 + FONT_APPLE7.spacing);

    /* Out-of-range chars contribute nothing (e.g. '~' past the 'Z' slot). */
    assert(font_text_width(&FONT_APPLE7, "A~") == font_text_width(&FONT_APPLE7, "A"));

    /* Uppercasing: lowercase renders identical to uppercase. */
    struct fb a, b;
    fb_clear(&a); fb_clear(&b);
    font_draw_centered(&a, 0, &FONT_METRIC9, "a");
    font_draw_centered(&b, 0, &FONT_METRIC9, "A");
    assert(memcmp(a.buf, b.buf, FB_SIZE) == 0);

    /* A glyph draws non-empty and stays within its height box. */
    struct fb g; fb_clear(&g);
    font_draw_centered(&g, 0, &FONT_APPLE7, "A");
    int count = 0;
    for (int y = 0; y < FB_H; y++)
        for (int x = 0; x < FB_W; x++)
            if (fb_get_pixel(&g, x, y)) { count++; assert(y < FONT_APPLE7.height); }
    assert(count > 0);

    printf("test_text OK\n");
    return 0;
}
