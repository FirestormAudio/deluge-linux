#ifndef VERSION_H
#define VERSION_H

#include <stddef.h>

/* Copy the leading [0-9.] run of `release` into `out` (NUL-terminated, at most
   n-1 chars), stopping at the first other char. "7.1.3-deluge" -> "7.1.3". */
void version_shorten(const char *release, char *out, size_t n);

/* uname() the running kernel and version_shorten its release into out. */
void kernel_version_short(char *out, size_t n);

#endif
