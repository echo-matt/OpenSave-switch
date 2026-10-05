/* The console's side of what the portable core asks of its platform. */
#include <switch.h>

#include "../core/crypto.h"

/* randomGet draws on the system's secure random number service. */
int os_random(void *buf, size_t len) {
    randomGet(buf, len);
    return 0;
}
