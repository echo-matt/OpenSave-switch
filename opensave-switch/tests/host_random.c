/* os_random for host builds: the kernel's CSPRNG. */
#include <stdio.h>
#include "../core/crypto.h"

int os_random(void *buf, size_t len) {
    FILE *f = fopen("/dev/urandom", "rb");
    size_t got;
    if (!f) return -1;
    got = fread(buf, 1, len, f);
    fclose(f);
    return got == len ? 0 : -1;
}
