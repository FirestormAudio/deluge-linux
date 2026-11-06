#include "version.h"
#include <sys/utsname.h>

static int is_ver_char(char c) {
    return (c >= '0' && c <= '9') || c == '.';
}

void version_shorten(const char *release, char *out, size_t n) {
    size_t i = 0;
    if (n == 0) return;
    while (release[i] && is_ver_char(release[i]) && i + 1 < n) {
        out[i] = release[i];
        i++;
    }
    out[i] = '\0';
}

void kernel_version_short(char *out, size_t n) {
    struct utsname u;
    if (uname(&u) == 0) version_shorten(u.release, out, n);
    else if (n) out[0] = '\0';
}
