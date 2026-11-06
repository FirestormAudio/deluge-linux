#ifndef RENDER_H
#define RENDER_H

#include "fb.h"

/* Displayed name shown by the splash (the kernel draws the droplet earlier,
   and the splash replaces it with this). Override at build time with
   -DSPLASH_NAME='"my/name"' (Makefile NAME=, or BR2_PACKAGE_DELUGE_SPLASH_NAME). */
#ifndef SPLASH_NAME
#define SPLASH_NAME "deluge/linux"
#endif
/* The wordmark/bar/version group is vertically centered in the 43-row panel:
   content spans rows 4..37, leaving 4 blank rows above and 5 below. */
#define WORDMARK_Y 4                 /* Metric-9 wordmark: rows 4..12 */

/* Progress bar - matches the app-loader (ui.rs show_progress): x=8, w=WIDTH-16,
   h=10, 1px outline + inner fill. */
#define BAR_X 8
#define BAR_Y 17
#define BAR_W 112
#define BAR_H 10
#define BAR_INNER_W (BAR_W - 2)      /* 110 */

/* Apple-7 version, centered below the bar: rows 31..37. */
#define VERSION_Y 31

int  bar_fill_width(int pct, int inner_w);
void render_frame(struct fb *f, int pct, const char *version);

#endif
