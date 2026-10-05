#include "platform.h"

#include <time.h>

int64_t os_now_ms(void) { return (int64_t)time(NULL) * 1000; }

const char *os_version_string(void) { return "switch-0.1.0"; }
