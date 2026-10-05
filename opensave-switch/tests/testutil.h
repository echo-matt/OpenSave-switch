#ifndef OPENSAVE_TESTUTIL_H
#define OPENSAVE_TESTUTIL_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int t_failures = 0, t_checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        t_checks++;                                                          \
        if (!(cond)) {                                                       \
            t_failures++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                    \
    } while (0)

#define CHECK_STR(got, want)                                                           \
    do {                                                                               \
        t_checks++;                                                                    \
        if (strcmp((got), (want)) != 0) {                                              \
            t_failures++;                                                              \
            fprintf(stderr, "FAIL %s:%d: got \"%s\" want \"%s\"\n", __FILE__, __LINE__, \
                    (got), (want));                                                    \
        }                                                                              \
    } while (0)

static int t_finish(const char *name) {
    printf("%s: %d checks, %d failures\n", name, t_checks, t_failures);
    return t_failures ? 1 : 0;
}

/* hex "-" means empty. Returns length, or -1 on bad hex. */
static long __attribute__((unused)) t_unhex(unsigned char *out, const char *hex) {
    size_t n, i;
    if (strcmp(hex, "-") == 0) return 0;
    n = strlen(hex);
    if (n % 2) return -1;
    for (i = 0; i < n; i += 2) {
        unsigned v = 0;
        int k;
        for (k = 0; k < 2; k++) {
            char c = hex[i + k];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else return -1;
        }
        out[i / 2] = (unsigned char)v;
    }
    return (long)(n / 2);
}
#endif
