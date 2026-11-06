#define _DEFAULT_SOURCE
#include "fb.h"
#include "render.h"
#include "version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#define STEPS 8
#define STEP_US 18000   /* ~150 ms total over STEPS */

static int clamp_pct(int p) { return p < 0 ? 0 : (p > 100 ? 100 : p); }

static const char *fb_path(void) {
    const char *e = getenv("SPLASH_FB");
    return e ? e : "/dev/fb0";
}
static const char *pct_path(void) {
    const char *e = getenv("SPLASH_PCT");
    return e ? e : "/run/splash.pct";
}

static int read_pct(void) {
    FILE *fp = fopen(pct_path(), "r");
    if (!fp) return 0;
    int v = 0;
    if (fscanf(fp, "%d", &v) != 1) v = 0;
    fclose(fp);
    return clamp_pct(v);
}
static void write_pct(int v) {
    FILE *fp = fopen(pct_path(), "w");
    if (!fp) return;
    fprintf(fp, "%d\n", v);
    fclose(fp);
}

static int dump_pbm(const char *path, int pct, const char *version) {
    struct fb f;
    render_frame(&f, clamp_pct(pct), version);
    FILE *fp = fopen(path, "w");
    if (!fp) return 1;
    fprintf(fp, "P1\n%d %d\n", FB_W, FB_H);
    for (int y = 0; y < FB_H; y++) {
        for (int x = 0; x < FB_W; x++)
            fputc(fb_get_pixel(&f, x, y) ? '1' : '0', fp);
        fputc('\n', fp);
    }
    fclose(fp);
    return 0;
}

int main(int argc, char **argv) {
    char version[32];
    kernel_version_short(version, sizeof version);

    if (argc >= 3 && strcmp(argv[1], "--dump") == 0)
        return dump_pbm(argv[2], argc >= 4 ? atoi(argv[3]) : 0, version);

    if (argc < 2) {
        fprintf(stderr, "usage: splash <pct> | splash --dump <file> <pct>\n");
        return 2;
    }

    int target = clamp_pct(atoi(argv[1]));
    int prev = read_pct();

    const char *fbp = fb_path();
    int flags = O_RDWR | (getenv("SPLASH_FB") ? O_CREAT : 0);
    int fd = open(fbp, flags, 0644);
    if (fd < 0) return 0;               /* never block boot */

    struct fb f;
    for (int i = 1; i <= STEPS; i++) {
        int p = prev + (target - prev) * i / STEPS;   /* i==STEPS -> exactly target */
        render_frame(&f, p, version);
        if (fb_flush(&f, fd) != 0) break;
        if (i < STEPS) usleep(STEP_US);               /* no trailing sleep */
    }
    close(fd);

    write_pct(target);
    return 0;
}
