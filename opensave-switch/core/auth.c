#include "auth.h"

#include <stdio.h>
#include <string.h>

#include "crypto.h"

/* One length-prefixed field: "<len>:<bytes>". */
static void write_field(os_hmac_ctx *h, const char *s) {
    char pre[24];
    int n = snprintf(pre, sizeof pre, "%zu:", strlen(s));
    os_hmac_update(h, pre, (size_t)n);
    os_hmac_update(h, s, strlen(s));
}

static void mac_over(char *out, const uint8_t key[32], const char *tag, const char *from,
                     const char *to, const char *third, const char *fourth, const void *payload,
                     size_t paylen, const char *nonce, int64_t unix_ms) {
    os_hmac_ctx h;
    uint8_t sum[OS_SHA256_LEN];
    char ms[32], pre[24];
    int n;

    snprintf(ms, sizeof ms, "%lld", (long long)unix_ms);
    os_hmac_init(&h, key, 32);
    write_field(&h, tag);
    write_field(&h, from);
    write_field(&h, to);
    write_field(&h, third);
    write_field(&h, fourth);
    write_field(&h, nonce);
    write_field(&h, ms);
    n = snprintf(pre, sizeof pre, "%zu:", paylen);
    os_hmac_update(&h, pre, (size_t)n);
    if (paylen) os_hmac_update(&h, payload, paylen);
    os_hmac_final(&h, sum);
    os_b64_encode(out, sum, sizeof sum);
}

void os_request_mac(char *out, const uint8_t key[32], const char *from, const char *to,
                    const char *route, const char *method, const void *body, size_t bodylen,
                    const char *nonce, int64_t unix_ms) {
    mac_over(out, key, "request", from, to, route, method, body, bodylen, nonce, unix_ms);
}

void os_response_mac(char *out, const uint8_t key[32], const char *from, const char *to,
                     const char *msg_id, int status, const void *body, size_t bodylen,
                     const char *nonce, int64_t unix_ms) {
    char st[16];
    snprintf(st, sizeof st, "%d", status);
    mac_over(out, key, "response", from, to, msg_id, st, body, bodylen, nonce, unix_ms);
}

int os_mac_equal(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b), i;
    unsigned char diff = (unsigned char)(la != lb);
    size_t n = la < lb ? la : lb;
    for (i = 0; i < n; i++) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

int os_new_nonce(char out[33]) {
    uint8_t raw[16];
    if (os_random(raw, sizeof raw) != 0) return -1;
    os_hex_encode(out, raw, sizeof raw);
    return 0;
}
