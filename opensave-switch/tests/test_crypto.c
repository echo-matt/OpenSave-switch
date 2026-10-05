/* Known-answer tests: every vector in vectors.txt was produced by the Go
 * implementation in internal/e2ee (regenerate with tools/vectors). */
#include <stdint.h>
#include <stdlib.h>

#include "../core/auth.h"
#include "../core/crypto.h"
#include "testutil.h"

static void hexcheck(const uint8_t *got, size_t n, const char *want_hex) {
    char *h = malloc(n * 2 + 1);
    os_hex_encode(h, got, n);
    CHECK_STR(h, want_hex);
    free(h);
}

static void test_fixed(void) {
    uint8_t out[32];
    /* FIPS 180-2 "abc". */
    os_sha256("abc", 3, out);
    hexcheck(out, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    os_sha256("", 0, out);
    hexcheck(out, 32, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    /* RFC 4231 test case 2. */
    {
        os_hmac_ctx h;
        os_hmac_init(&h, (const uint8_t *)"Jefe", 4);
        os_hmac_update(&h, "what do ya want ", 16);
        os_hmac_update(&h, "for nothing?", 12);
        os_hmac_final(&h, out);
        hexcheck(out, 32, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    }
    /* RFC 7748 section 6.1 (Alice/Bob). */
    {
        uint8_t a[32], b[32], pa[32], pb[32], k1[32], k2[32];
        t_unhex(a, "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
        t_unhex(b, "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
        CHECK(os_x25519_public(pa, a) == 0);
        CHECK(os_x25519_public(pb, b) == 0);
        hexcheck(pa, 32, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
        hexcheck(pb, 32, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
        CHECK(os_x25519(k1, a, pb) == 0);
        CHECK(os_x25519(k2, b, pa) == 0);
        hexcheck(k1, 32, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
        CHECK(memcmp(k1, k2, 32) == 0);
    }
}

static void test_vectors(const char *path) {
    FILE *f = fopen(path, "r");
    static char line[4096];
    int n = 0;
    CHECK(f != NULL);
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *tok[16];
        int nt = 0;
        char *p = strtok(line, " \r\n");
        while (p && nt < 16) {
            tok[nt++] = p;
            p = strtok(NULL, " \r\n");
        }
        if (nt == 0) continue;
        n++;
        if (!strcmp(tok[0], "sha256")) {
            static unsigned char msg[1024];
            uint8_t out[32];
            long len = t_unhex(msg, tok[1]);
            os_sha256(msg, (size_t)len, out);
            hexcheck(out, 32, tok[2]);
            /* The same message fed in awkward pieces must agree. */
            {
                os_sha256_ctx c;
                long i = 0, step = 1;
                os_sha256_init(&c);
                while (i < len) {
                    long take = step < len - i ? step : len - i;
                    os_sha256_update(&c, msg + i, (size_t)take);
                    i += take;
                    step = step * 2 + 1;
                }
                os_sha256_final(&c, out);
                hexcheck(out, 32, tok[2]);
            }
        } else if (!strcmp(tok[0], "b64")) {
            static unsigned char msg[1024], back[1024];
            static char enc[2048];
            long len = t_unhex(msg, tok[1]);
            const char *want = strcmp(tok[2], "-") ? tok[2] : "";
            os_b64_encode(enc, msg, (size_t)len);
            CHECK_STR(enc, want);
            CHECK(os_b64_decode(back, want, strlen(want)) == len);
            CHECK(len == 0 || memcmp(back, msg, (size_t)len) == 0);
        } else if (!strcmp(tok[0], "hmac")) {
            static unsigned char key[256], msg[1024];
            uint8_t out[32];
            os_hmac_ctx h;
            long kl = t_unhex(key, tok[1]), ml = t_unhex(msg, tok[2]);
            os_hmac_init(&h, key, (size_t)kl);
            os_hmac_update(&h, msg, (size_t)ml);
            os_hmac_final(&h, out);
            hexcheck(out, 32, tok[3]);
        } else if (!strcmp(tok[0], "authkey")) {
            uint8_t priv[32], pub[32], theirs[32], key[32], mypub[32];
            t_unhex(priv, tok[1]);
            t_unhex(pub, tok[2]);
            t_unhex(theirs, tok[3]);
            CHECK(os_x25519_public(mypub, priv) == 0);
            CHECK(memcmp(mypub, pub, 32) == 0);
            CHECK(os_auth_key(key, priv, theirs) == 0);
            hexcheck(key, 32, tok[4]);
        } else if (!strcmp(tok[0], "reqmac")) {
            static unsigned char key[32], body[1024];
            char mac[OS_MAC_B64_LEN + 1];
            long bl;
            t_unhex(key, tok[1]);
            bl = t_unhex(body, tok[6]);
            os_request_mac(mac, key, tok[2], tok[3], tok[4], tok[5], body, (size_t)bl, tok[7],
                           atoll(tok[8]));
            CHECK_STR(mac, tok[9]);
            CHECK(os_mac_equal(mac, tok[9]));
        } else if (!strcmp(tok[0], "respmac")) {
            static unsigned char key[32], body[1024];
            char mac[OS_MAC_B64_LEN + 1];
            long bl;
            t_unhex(key, tok[1]);
            bl = t_unhex(body, tok[6]);
            os_response_mac(mac, key, tok[2], tok[3], tok[4], atoi(tok[5]), body, (size_t)bl,
                            tok[7], atoll(tok[8]));
            CHECK_STR(mac, tok[9]);
        } else if (!strcmp(tok[0], "x25519")) {
            uint8_t sc[32], pt[32], out[32];
            t_unhex(sc, tok[1]);
            t_unhex(pt, tok[2]);
            CHECK(os_x25519(out, sc, pt) == 0);
            hexcheck(out, 32, tok[3]);
        } else if (!strcmp(tok[0], "fingerprint")) {
            uint8_t a[32], b[32], c[32];
            char got[OS_FINGERPRINT_LEN], want[OS_FINGERPRINT_LEN], swapped[OS_FINGERPRINT_LEN];
            char *u;
            t_unhex(a, tok[1]);
            t_unhex(b, tok[2]);
            (void)c;
            strcpy(want, tok[3]);
            while ((u = strchr(want, '_')) != NULL) *u = ' ';
            os_fingerprint(got, a, b);
            CHECK_STR(got, want);
            os_fingerprint(swapped, b, a); /* both devices must agree */
            CHECK_STR(swapped, want);
        } else if (!strcmp(tok[0], "x25519zero")) {
            uint8_t sc[32], pt[32], out[32];
            t_unhex(sc, tok[1]);
            t_unhex(pt, tok[2]);
            CHECK(os_x25519(out, sc, pt) == -1); /* low-order point refused */
        }
    }
    fclose(f);
    CHECK(n > 700);
}

static void test_b64_rejects(void) {
    uint8_t out[16];
    CHECK(os_b64_decode(out, "AAA", 3) == -1);        /* length */
    CHECK(os_b64_decode(out, "AA=A", 4) == -1);       /* pad in middle */
    CHECK(os_b64_decode(out, "A===", 4) == -1);
    CHECK(os_b64_decode(out, "AB==", 4) == -1);       /* non-canonical bits */
    CHECK(os_b64_decode(out, "AAAA====", 8) == -1);
    CHECK(os_b64_decode(out, "AA!A", 4) == -1);
    CHECK(os_b64_decode(out, "QQ==", 4) == 1 && out[0] == 'A');
}

int main(int argc, char **argv) {
    test_fixed();
    test_b64_rejects();
    test_vectors(argc > 1 ? argv[1] : "tests/vectors.txt");
    return t_finish("crypto");
}
