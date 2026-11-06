#include "version.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

int main(void) {
    char b[32];

    version_shorten("7.1.3-deluge", b, sizeof b); assert(strcmp(b, "7.1.3") == 0);
    version_shorten("6.6.0", b, sizeof b);         assert(strcmp(b, "6.6.0") == 0);
    version_shorten("5.10.1-rc2+g", b, sizeof b);  assert(strcmp(b, "5.10.1") == 0);
    version_shorten("", b, sizeof b);              assert(strcmp(b, "") == 0);
    version_shorten("dev", b, sizeof b);           assert(strcmp(b, "") == 0);

    /* respects buffer size, always NUL-terminated */
    version_shorten("1.2.3.4.5", b, 4);            assert(strcmp(b, "1.2") == 0);

    printf("test_version ok\n");
    return 0;
}
