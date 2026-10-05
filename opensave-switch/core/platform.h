/* The few things the core asks of the platform. */
#ifndef OPENSAVE_PLATFORM_H
#define OPENSAVE_PLATFORM_H

#include <stdint.h>

/* Wall-clock time in milliseconds since the Unix epoch. Request signatures
 * carry it, and a peer refuses one more than five minutes away from its own
 * clock, so a Switch whose clock is wrong cannot sync. */
int64_t os_now_ms(void);

/* What this client reports as its version in answers to a ping. */
const char *os_version_string(void);

#endif
