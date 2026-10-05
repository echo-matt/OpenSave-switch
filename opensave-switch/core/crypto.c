#include "crypto.h"

#include <string.h>

/* ------------------------------------------------------------------ SHA-256 */

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t st[8], const uint8_t blk[64]) {
    uint32_t w[64], a, b, c, d, e, f, g, h;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)blk[i * 4] << 24) | ((uint32_t)blk[i * 4 + 1] << 16) |
               ((uint32_t)blk[i * 4 + 2] << 8) | (uint32_t)blk[i * 4 + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = st[0]; b = st[1]; c = st[2]; d = st[3];
    e = st[4]; f = st[5]; g = st[6]; h = st[7];
    for (i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
    st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

void os_sha256_init(os_sha256_ctx *c) {
    c->state[0] = 0x6a09e667; c->state[1] = 0xbb67ae85;
    c->state[2] = 0x3c6ef372; c->state[3] = 0xa54ff53a;
    c->state[4] = 0x510e527f; c->state[5] = 0x9b05688c;
    c->state[6] = 0x1f83d9ab; c->state[7] = 0x5be0cd19;
    c->bits = 0;
    c->buflen = 0;
}

void os_sha256_update(os_sha256_ctx *c, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    c->bits += (uint64_t)len * 8;
    if (c->buflen) {
        size_t take = 64 - c->buflen;
        if (take > len) take = len;
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += take;
        p += take;
        len -= take;
        if (c->buflen == 64) {
            sha256_block(c->state, c->buf);
            c->buflen = 0;
        }
    }
    while (len >= 64) {
        sha256_block(c->state, p);
        p += 64;
        len -= 64;
    }
    if (len) {
        memcpy(c->buf, p, len);
        c->buflen = len;
    }
}

void os_sha256_final(os_sha256_ctx *c, uint8_t out[OS_SHA256_LEN]) {
    uint64_t bits = c->bits;
    uint8_t pad[72];
    size_t padlen = (c->buflen < 56) ? (56 - c->buflen) : (120 - c->buflen);
    int i;
    memset(pad, 0, sizeof pad);
    pad[0] = 0x80;
    for (i = 0; i < 8; i++) pad[padlen + i] = (uint8_t)(bits >> (56 - 8 * i));
    os_sha256_update(c, pad, padlen + 8);
    for (i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(c->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(c->state[i]);
    }
}

void os_sha256(const void *data, size_t len, uint8_t out[OS_SHA256_LEN]) {
    os_sha256_ctx c;
    os_sha256_init(&c);
    os_sha256_update(&c, data, len);
    os_sha256_final(&c, out);
}

/* --------------------------------------------------------------------- HMAC */

void os_hmac_init(os_hmac_ctx *c, const uint8_t *key, size_t keylen) {
    uint8_t k[64], ipad[64];
    size_t i;
    memset(k, 0, sizeof k);
    if (keylen > 64) {
        os_sha256(key, keylen, k);
    } else if (keylen) {
        memcpy(k, key, keylen);
    }
    for (i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        c->opad[i] = k[i] ^ 0x5c;
    }
    os_sha256_init(&c->inner);
    os_sha256_update(&c->inner, ipad, 64);
}

void os_hmac_update(os_hmac_ctx *c, const void *data, size_t len) {
    os_sha256_update(&c->inner, data, len);
}

void os_hmac_final(os_hmac_ctx *c, uint8_t out[OS_SHA256_LEN]) {
    uint8_t ih[OS_SHA256_LEN];
    os_sha256_ctx o;
    os_sha256_final(&c->inner, ih);
    os_sha256_init(&o);
    os_sha256_update(&o, c->opad, 64);
    os_sha256_update(&o, ih, sizeof ih);
    os_sha256_final(&o, out);
}

/* --------------------------------------------------------------------- HKDF */

void os_hkdf32(const uint8_t *secret, size_t secretlen, const char *info, uint8_t out[32]) {
    uint8_t zero_salt[OS_SHA256_LEN] = {0};
    uint8_t prk[OS_SHA256_LEN];
    os_hmac_ctx h;
    const uint8_t one = 1;

    /* Extract: PRK = HMAC(salt, secret). A nil salt means HashLen zero bytes. */
    os_hmac_init(&h, zero_salt, sizeof zero_salt);
    os_hmac_update(&h, secret, secretlen);
    os_hmac_final(&h, prk);

    /* Expand, first block only: T(1) = HMAC(PRK, info || 0x01). */
    os_hmac_init(&h, prk, sizeof prk);
    os_hmac_update(&h, info, strlen(info));
    os_hmac_update(&h, &one, 1);
    os_hmac_final(&h, out);
}

/* ------------------------------------------------------------------- X25519 */
/* The field arithmetic below is the well-known compact formulation from
 * TweetNaCl (public domain, D. J. Bernstein et al.): sixteen 16-bit limbs held
 * in int64_t, constant-time swap, and a Montgomery ladder. It is slow next to
 * a tuned implementation and is only ever run twice per pairing. */

typedef int64_t gf[16];

static const gf C121665 = {0xDB41, 1};

static void car25519(gf o) {
    int i;
    int64_t c;
    for (i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c * 65536; /* not "c << 16": c may be negative */
    }
}

static void sel25519(gf p, gf q, int64_t b) {
    int64_t t, c = ~(b - 1);
    int i;
    for (i = 0; i < 16; i++) {
        t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void pack25519(uint8_t *o, const gf n) {
    int i, j;
    int64_t b;
    gf m, t;
    for (i = 0; i < 16; i++) t[i] = n[i];
    car25519(t);
    car25519(t);
    car25519(t);
    for (j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (i = 0; i < 16; i++) {
        o[2 * i] = (uint8_t)(t[i] & 0xff);
        o[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
}

static void unpack25519(gf o, const uint8_t *n) {
    int i;
    for (i = 0; i < 16; i++) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

static void fadd(gf o, const gf a, const gf b) {
    int i;
    for (i = 0; i < 16; i++) o[i] = a[i] + b[i];
}

static void fsub(gf o, const gf a, const gf b) {
    int i;
    for (i = 0; i < 16; i++) o[i] = a[i] - b[i];
}

static void fmul(gf o, const gf a, const gf b) {
    int64_t i, j, t[31];
    for (i = 0; i < 31; i++) t[i] = 0;
    for (i = 0; i < 16; i++)
        for (j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (i = 0; i < 16; i++) o[i] = t[i];
    car25519(o);
    car25519(o);
}

static void fsq(gf o, const gf a) { fmul(o, a, a); }

static void inv25519(gf o, const gf in) {
    gf c;
    int a;
    for (a = 0; a < 16; a++) c[a] = in[a];
    for (a = 253; a >= 0; a--) {
        fsq(c, c);
        if (a != 2 && a != 4) fmul(c, c, in);
    }
    for (a = 0; a < 16; a++) o[a] = c[a];
}

int os_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    uint8_t z[32];
    int64_t x[80], r, i;
    gf a, b, c, d, e, f;
    uint8_t acc = 0;

    for (i = 0; i < 31; i++) z[i] = scalar[i];
    z[31] = (scalar[31] & 127) | 64;
    z[0] &= 248;
    unpack25519(x, point);
    for (i = 0; i < 16; i++) {
        b[i] = x[i];
        d[i] = a[i] = c[i] = 0;
    }
    a[0] = d[0] = 1;
    for (i = 254; i >= 0; --i) {
        r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, r);
        sel25519(c, d, r);
        fadd(e, a, c);
        fsub(a, a, c);
        fadd(c, b, d);
        fsub(b, b, d);
        fsq(d, e);
        fsq(f, a);
        fmul(a, c, a);
        fmul(c, b, e);
        fadd(e, a, c);
        fsub(a, a, c);
        fsq(b, a);
        fsub(c, d, f);
        fmul(a, c, C121665);
        fadd(a, a, d);
        fmul(c, c, a);
        fmul(a, d, f);
        fmul(d, b, x);
        fsq(b, e);
        sel25519(a, b, r);
        sel25519(c, d, r);
    }
    for (i = 0; i < 16; i++) {
        x[i + 16] = a[i];
        x[i + 32] = c[i];
        x[i + 48] = b[i];
        x[i + 64] = d[i];
    }
    inv25519((int64_t *)(x + 32), (int64_t *)(x + 32));
    fmul((int64_t *)(x + 16), (int64_t *)(x + 16), (int64_t *)(x + 32));
    pack25519(out, (int64_t *)(x + 16));

    for (i = 0; i < 32; i++) acc |= out[i];
    return acc == 0 ? -1 : 0;
}

int os_x25519_public(uint8_t pub[32], const uint8_t priv[32]) {
    static const uint8_t base[32] = {9};
    return os_x25519(pub, priv, base);
}

#define OS_AUTH_INFO "opensave/e2ee/request-auth/v1"

int os_auth_key(uint8_t out[32], const uint8_t my_private[32], const uint8_t their_public[32]) {
    uint8_t secret[32];
    if (os_x25519(secret, my_private, their_public) != 0) return -1;
    os_hkdf32(secret, sizeof secret, OS_AUTH_INFO, out);
    memset(secret, 0, sizeof secret);
    return 0;
}

void os_fingerprint(char out[OS_FINGERPRINT_LEN], const uint8_t pub_a[32], const uint8_t pub_b[32]) {
    static const char label[] = "opensave/e2ee/save-payload/v1/fingerprint";
    const uint8_t *first = pub_a, *second = pub_b;
    uint8_t sum[OS_SHA256_LEN];
    char hex[25];
    os_sha256_ctx c;
    int i, o = 0;
    for (i = 0; i < 32; i++) {
        if (pub_a[i] != pub_b[i]) {
            if (pub_a[i] > pub_b[i]) {
                first = pub_b;
                second = pub_a;
            }
            break;
        }
    }
    os_sha256_init(&c);
    os_sha256_update(&c, label, sizeof label - 1);
    os_sha256_update(&c, first, 32);
    os_sha256_update(&c, second, 32);
    os_sha256_final(&c, sum);
    os_hex_encode(hex, sum, 12);
    for (i = 0; i < 24; i += 4) {
        if (i) out[o++] = ' ';
        memcpy(out + o, hex + i, 4);
        o += 4;
    }
    out[o] = '\0';
}

/* ------------------------------------------------------------------- Base64 */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t os_b64_encoded_len(size_t len) { return ((len + 2) / 3) * 4; }

size_t os_b64_encode(char *out, const uint8_t *in, size_t len) {
    size_t i = 0, o = 0;
    while (i + 2 < len) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = B64[(v >> 6) & 63];
        out[o++] = B64[v & 63];
        i += 3;
    }
    if (len - i == 1) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (len - i == 2) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = B64[(v >> 6) & 63];
        out[o++] = '=';
    }
    out[o] = '\0';
    return o;
}

static int b64val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

long os_b64_decode(uint8_t *out, const char *in, size_t len) {
    size_t i, o = 0;
    if (len % 4 != 0) return -1;
    for (i = 0; i < len; i += 4) {
        int v0 = b64val(in[i]), v1 = b64val(in[i + 1]);
        int v2, v3;
        int last = (i + 4 == len);
        if (v0 < 0 || v1 < 0) return -1;
        if (in[i + 2] == '=') {
            /* "xx==" is only valid as the final quantum. */
            if (!last || in[i + 3] != '=') return -1;
            if (v1 & 0x0f) return -1; /* non-canonical trailing bits */
            out[o++] = (uint8_t)((v0 << 2) | (v1 >> 4));
            break;
        }
        v2 = b64val(in[i + 2]);
        if (v2 < 0) return -1;
        if (in[i + 3] == '=') {
            if (!last) return -1;
            if (v2 & 0x03) return -1;
            out[o++] = (uint8_t)((v0 << 2) | (v1 >> 4));
            out[o++] = (uint8_t)((v1 << 4) | (v2 >> 2));
            break;
        }
        v3 = b64val(in[i + 3]);
        if (v3 < 0) return -1;
        out[o++] = (uint8_t)((v0 << 2) | (v1 >> 4));
        out[o++] = (uint8_t)((v1 << 4) | (v2 >> 2));
        out[o++] = (uint8_t)((v2 << 6) | v3);
    }
    return (long)o;
}

void os_hex_encode(char *out, const uint8_t *in, size_t len) {
    static const char H[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < len; i++) {
        out[i * 2] = H[in[i] >> 4];
        out[i * 2 + 1] = H[in[i] & 15];
    }
    out[len * 2] = '\0';
}
