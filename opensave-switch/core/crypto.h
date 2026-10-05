/* Cryptographic primitives for the OpenSave Switch client.
 *
 * Only what the LAN pairing protocol needs, and each piece is checked against
 * the Go implementation in internal/e2ee (see tests/test_crypto.c and
 * tools/vectors.go):
 *
 *   SHA-256, HMAC-SHA-256   request authentication and block hashing
 *   HKDF-SHA-256            deriving the request-auth key from the X25519 secret
 *   X25519                  key agreement with a paired device
 *   Base64                  keys, MACs and block payloads on the wire
 *
 * There is deliberately no encryption here. The LAN protocol is plain HTTP
 * whose requests are authenticated, not sealed; sealing exists only for the
 * relay path, which this client does not speak.
 */
#ifndef OPENSAVE_CRYPTO_H
#define OPENSAVE_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#define OS_SHA256_LEN 32
#define OS_KEY_LEN 32

typedef struct {
    uint32_t state[8];
    uint64_t bits;
    uint8_t buf[64];
    size_t buflen;
} os_sha256_ctx;

void os_sha256_init(os_sha256_ctx *c);
void os_sha256_update(os_sha256_ctx *c, const void *data, size_t len);
void os_sha256_final(os_sha256_ctx *c, uint8_t out[OS_SHA256_LEN]);
void os_sha256(const void *data, size_t len, uint8_t out[OS_SHA256_LEN]);

/* Streaming HMAC-SHA-256, because the request MAC covers a body that is
 * written field by field. */
typedef struct {
    os_sha256_ctx inner;
    uint8_t opad[64];
} os_hmac_ctx;

void os_hmac_init(os_hmac_ctx *c, const uint8_t *key, size_t keylen);
void os_hmac_update(os_hmac_ctx *c, const void *data, size_t len);
void os_hmac_final(os_hmac_ctx *c, uint8_t out[OS_SHA256_LEN]);

/* HKDF-SHA-256 with an empty salt, producing 32 bytes — one output block,
 * which is all this protocol derives. Same as Go's hkdf.New(sha256.New,
 * secret, nil, info) read for 32 bytes. */
void os_hkdf32(const uint8_t *secret, size_t secretlen,
               const char *info, uint8_t out[32]);

/* X25519. Returns 0 on success, -1 if the result is all zero (a low-order
 * point, which Go's curve25519.X25519 also refuses). */
int os_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
int os_x25519_public(uint8_t pub[32], const uint8_t priv[32]);

/* The key two paired devices derive for authenticating requests; see
 * e2ee.AuthKey. Returns 0 on success. */
int os_auth_key(uint8_t out[32], const uint8_t my_private[32], const uint8_t their_public[32]);

/* The short string both devices show for a pairing, for comparing out of band;
 * see e2ee.Fingerprint. The two keys are ordered before hashing so each side
 * computes the same value. out receives "xxxx xxxx xxxx xxxx xxxx xxxx". */
#define OS_FINGERPRINT_LEN 30
void os_fingerprint(char out[OS_FINGERPRINT_LEN], const uint8_t pub_a[32], const uint8_t pub_b[32]);

/* Base64, standard alphabet with padding, as Go's base64.StdEncoding.
 * os_b64_encode writes a NUL-terminated string and returns its length;
 * out must hold ((len+2)/3)*4 + 1 bytes. os_b64_decode returns the decoded
 * length, or -1 on malformed input; out must hold (len/4)*3 bytes. */
size_t os_b64_encode(char *out, const uint8_t *in, size_t len);
size_t os_b64_encoded_len(size_t len);
long os_b64_decode(uint8_t *out, const char *in, size_t len);

void os_hex_encode(char *out, const uint8_t *in, size_t len); /* NUL-terminated */

/* Fills buf with cryptographically secure random bytes. Returns 0 on success.
 * Supplied by the platform: /dev/urandom on a host, the system's random
 * service on the console. */
int os_random(void *buf, size_t len);

#endif
