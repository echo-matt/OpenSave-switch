/* Request and response authentication, the C side of internal/e2ee/auth.go.
 *
 * A MAC over every field that decides what a request does, keyed with the
 * secret only the two paired devices can derive. Each variable-length field is
 * length-prefixed so two different requests can never share an input. */
#ifndef OPENSAVE_AUTH_H
#define OPENSAVE_AUTH_H

#include <stddef.h>
#include <stdint.h>

#define OS_MAC_B64_LEN 44            /* base64 of a 32-byte MAC */
#define OS_MAX_AUTH_SKEW_MS (5 * 60 * 1000)

/* out receives a NUL-terminated base64 MAC; it must hold OS_MAC_B64_LEN + 1. */
void os_request_mac(char *out, const uint8_t key[32], const char *from, const char *to,
                    const char *route, const char *method, const void *body, size_t bodylen,
                    const char *nonce, int64_t unix_ms);

void os_response_mac(char *out, const uint8_t key[32], const char *from, const char *to,
                     const char *msg_id, int status, const void *body, size_t bodylen,
                     const char *nonce, int64_t unix_ms);

/* Constant-time comparison of a presented MAC against the expected one. */
int os_mac_equal(const char *a, const char *b);

/* A random 32-hex-character nonce, as e2ee.NewNonce. Returns 0 on success. */
int os_new_nonce(char out[33]);

#endif
