#include "aes.h"

#include <string.h>

/* The S-box and its inverse are computed rather than typed in, from the field
 * arithmetic that defines them, so there is no table to mistype. */
static uint8_t sbox[256], inv_sbox[256];
static int tables_ready;

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }

static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    int i;
    for (i = 0; i < 8; i++) {
        if (b & 1) p ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return p;
}

static void make_tables(void) {
    int i;
    uint8_t p = 1, q = 1;
    if (tables_ready) return;
    /* Walk the multiplicative group: p runs over 3^k and q over its inverse. */
    do {
        p = (uint8_t)(p ^ (p << 1) ^ ((p & 0x80) ? 0x1b : 0));
        q ^= (uint8_t)(q << 1);
        q ^= (uint8_t)(q << 2);
        q ^= (uint8_t)(q << 4);
        if (q & 0x80) q ^= 0x09;
        {
            uint8_t x = (uint8_t)(q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) ^ ((q << 3) | (q >> 5)) ^
                                  ((q << 4) | (q >> 4)));
            sbox[p] = (uint8_t)(x ^ 0x63);
        }
    } while (p != 1);
    sbox[0] = 0x63;
    for (i = 0; i < 256; i++) inv_sbox[sbox[i]] = (uint8_t)i;
    tables_ready = 1;
}

void os_aes256_init(os_aes256 *a, const uint8_t key[32]) {
    uint8_t w[240];
    int i;
    uint8_t rcon = 1;
    make_tables();
    memcpy(w, key, 32);
    for (i = 32; i < 240; i += 4) {
        uint8_t t[4];
        memcpy(t, w + i - 4, 4);
        if (i % 32 == 0) {
            uint8_t k = t[0];
            t[0] = (uint8_t)(sbox[t[1]] ^ rcon);
            t[1] = sbox[t[2]];
            t[2] = sbox[t[3]];
            t[3] = sbox[k];
            rcon = xtime(rcon);
        } else if (i % 32 == 16) {
            t[0] = sbox[t[0]];
            t[1] = sbox[t[1]];
            t[2] = sbox[t[2]];
            t[3] = sbox[t[3]];
        }
        w[i] = (uint8_t)(w[i - 32] ^ t[0]);
        w[i + 1] = (uint8_t)(w[i - 31] ^ t[1]);
        w[i + 2] = (uint8_t)(w[i - 30] ^ t[2]);
        w[i + 3] = (uint8_t)(w[i - 29] ^ t[3]);
    }
    memcpy(a->rk, w, 240);
}

static void add_round_key(uint8_t s[16], const uint8_t k[16]) {
    int i;
    for (i = 0; i < 16; i++) s[i] ^= k[i];
}

void os_aes256_encrypt_block(const os_aes256 *a, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16], t[16];
    int r, i;
    memcpy(s, in, 16);
    add_round_key(s, a->rk[0]);
    for (r = 1; r <= 14; r++) {
        for (i = 0; i < 16; i++) s[i] = sbox[s[i]];
        /* ShiftRows (state is column-major: s[4*col + row]). */
        for (i = 0; i < 16; i++) t[i] = s[(i + 4 * (i % 4)) % 16];
        memcpy(s, t, 16);
        if (r != 14) {
            for (i = 0; i < 16; i += 4) {
                uint8_t a0 = s[i], a1 = s[i + 1], a2 = s[i + 2], a3 = s[i + 3];
                s[i] = (uint8_t)(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
                s[i + 1] = (uint8_t)(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
                s[i + 2] = (uint8_t)(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
                s[i + 3] = (uint8_t)((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
            }
        }
        add_round_key(s, a->rk[r]);
    }
    memcpy(out, s, 16);
}

void os_aes256_decrypt_block(const os_aes256 *a, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16], t[16];
    int r, i;
    memcpy(s, in, 16);
    add_round_key(s, a->rk[14]);
    for (r = 13; r >= 0; r--) {
        /* InvShiftRows */
        for (i = 0; i < 16; i++) t[(i + 4 * (i % 4)) % 16] = s[i];
        memcpy(s, t, 16);
        for (i = 0; i < 16; i++) s[i] = inv_sbox[s[i]];
        add_round_key(s, a->rk[r]);
        if (r != 0) {
            for (i = 0; i < 16; i += 4) {
                uint8_t a0 = s[i], a1 = s[i + 1], a2 = s[i + 2], a3 = s[i + 3];
                s[i] = (uint8_t)(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
                s[i + 1] = (uint8_t)(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
                s[i + 2] = (uint8_t)(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
                s[i + 3] = (uint8_t)(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
            }
        }
    }
    memcpy(out, s, 16);
}
