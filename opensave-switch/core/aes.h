/* AES-256, one 16-byte block at a time (ECB), which is how Minecraft Dungeons
 * Windows saves are encrypted. Checked against Go's crypto/aes and the FIPS-197
 * example in tests/test_crypto.c. */
#ifndef OPENSAVE_AES_H
#define OPENSAVE_AES_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t rk[15][16]; /* round keys */
} os_aes256;

void os_aes256_init(os_aes256 *a, const uint8_t key[32]);
void os_aes256_encrypt_block(const os_aes256 *a, const uint8_t in[16], uint8_t out[16]);
void os_aes256_decrypt_block(const os_aes256 *a, const uint8_t in[16], uint8_t out[16]);

#endif
