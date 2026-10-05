#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../core/auth.h"
#include "../core/crypto.h"
#include "../core/fsutil.h"
#include "../core/state.h"
#include "testutil.h"

static void test_roundtrip(const char *dir) {
    char path[300], err[200];
    os_state a, b;
    uint8_t other_priv[32], other_pub[32];
    os_peer *p;
    snprintf(path, sizeof path, "%s/cfg/state.json", dir);

    CHECK(os_state_load(&a, path, err, sizeof err) == 0); /* first run creates it */
    CHECK(strncmp(a.node_id, "node_", 5) == 0 && strlen(a.node_id) == 37);
    CHECK_STR(a.device_name, "Nintendo Switch");
    CHECK(os_exists(path));

    os_random(other_priv, 32);
    os_x25519_public(other_pub, other_priv);
    p = os_state_add_peer(&a, "node_pc", "My PC", "192.168.1.5", 8383, other_pub);
    CHECK(p != NULL);
    CHECK(os_state_save(&a, err, sizeof err) == 0);

    CHECK(os_state_load(&b, path, err, sizeof err) == 0);
    CHECK_STR(b.node_id, a.node_id);
    CHECK(memcmp(b.priv, a.priv, 32) == 0 && memcmp(b.pub, a.pub, 32) == 0);
    p = os_state_find_peer(&b, "node_pc");
    CHECK(p != NULL);
    if (p) {
        uint8_t theirs_key[32];
        CHECK_STR(p->name, "My PC");
        CHECK_STR(p->address, "192.168.1.5");
        CHECK(p->port == 8383 && memcmp(p->pubkey, other_pub, 32) == 0);
        /* The derived key is what the other side derives from its half. */
        CHECK(os_auth_key(theirs_key, other_priv, b.pub) == 0);
        CHECK(memcmp(p->authkey, theirs_key, 32) == 0);
    }
    /* Links are remembered too. */
    CHECK(os_state_set_link(&a, "01006c100ec08000", "minecraft-dungeons-saved", "Minecraft Dungeons (Saved)") != NULL);
    CHECK(os_state_save(&a, err, sizeof err) == 0);
    {
        os_state c;
        os_link *l;
        CHECK(os_state_load(&c, path, err, sizeof err) == 0);
        l = os_state_find_link(&c, "01006C100EC08000"); /* title ids compare without case */
        CHECK(l != NULL && !strcmp(l->game_id, "minecraft-dungeons-saved") && !strcmp(l->name, "Minecraft Dungeons (Saved)"));
        CHECK(os_state_find_link_by_game(&c, "minecraft-dungeons-saved") == l);
        CHECK(os_state_set_link(&c, "01006C100EC08000", "other-id", "x") == l && !strcmp(l->game_id, "other-id")); /* replaces */
        os_state_remove_link(&c, "01006C100EC08000");
        CHECK(os_state_find_link(&c, "01006C100EC08000") == NULL);
    }
    CHECK(os_state_remove_peer(&b, "node_pc") == 1 && os_state_find_peer(&b, "node_pc") == NULL);
}

static void test_damaged(const char *dir) {
    char path[300], err[200];
    os_state s;
    FILE *f;
    snprintf(path, sizeof path, "%s/bad.json", dir);
    f = fopen(path, "wb");
    fputs("{ this is not json", f);
    fclose(f);
    /* A file that cannot be read must never be replaced: it holds the only
     * copy of the device key, and a fresh identity would unpair everything. */
    CHECK(os_state_load(&s, path, err, sizeof err) == -1);
    CHECK(strstr(err, "not replacing") != NULL);
    f = fopen(path, "rb");
    {
        char buf[64] = {0};
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = 0;
        CHECK_STR(buf, "{ this is not json");
    }
    fclose(f);
    f = fopen(path, "wb");
    fputs("{\"nodeId\":\"x\",\"privateKey\":\"short\"}", f);
    fclose(f);
    CHECK(os_state_load(&s, path, err, sizeof err) == -1);
}

static void test_nonces(void) {
    os_state s;
    int i;
    memset(&s, 0, sizeof s);
    CHECK(os_state_remember_nonce(&s, "n1", 1000) == 1);
    CHECK(os_state_remember_nonce(&s, "n1", 2000) == 0); /* replay inside the window */
    CHECK(os_state_remember_nonce(&s, "n2", 2000) == 1);
    CHECK(os_state_remember_nonce(&s, "n1", 1000 + 2 * OS_MAX_AUTH_SKEW_MS + 1) == 1); /* forgotten after its window */
    /* The cache is a ring: filling it pushes the oldest out, but never loses a
     * recent one. */
    for (i = 0; i < OS_NONCE_CACHE - 1; i++) {
        char n[16];
        snprintf(n, sizeof n, "x%d", i);
        CHECK(os_state_remember_nonce(&s, n, 5000) == 1);
    }
    CHECK(os_state_remember_nonce(&s, "x5", 5000) == 0);
}

static void test_peer_limit(void) {
    os_state s;
    uint8_t k[32], priv[32];
    int i;
    char id[16];
    memset(&s, 0, sizeof s);
    os_random(priv, 32);
    os_x25519_public(k, priv);
    os_random(s.priv, 32);
    for (i = 0; i < OS_MAX_PEERS; i++) {
        snprintf(id, sizeof id, "p%d", i);
        CHECK(os_state_add_peer(&s, id, "n", "1.1.1.1", 1, k) != NULL);
    }
    CHECK(os_state_add_peer(&s, "overflow", "n", "1.1.1.1", 1, k) == NULL);
    CHECK(os_state_add_peer(&s, "p3", "renamed", "2.2.2.2", 2, k) != NULL); /* re-pairing replaces */
    CHECK_STR(os_state_find_peer(&s, "p3")->name, "renamed");
    /* A low-order public key cannot be paired with: no usable shared secret. */
    {
        uint8_t zero[32] = {0};
        CHECK(os_state_remove_peer(&s, "p0") == 1); /* make room, so a refusal is about the key */
        CHECK(os_state_add_peer(&s, "evil", "n", "1.1.1.1", 1, zero) == NULL);
        CHECK(os_state_find_peer(&s, "evil") == NULL);
    }
}

static void test_join(void) {
    char out[64];
    os_path_join(out, sizeof out, "/a/b", "c");
    CHECK_STR(out, "/a/b/c");
    os_path_join(out, sizeof out, "save:/", "slot/1.dat");
    CHECK_STR(out, "save:/slot/1.dat"); /* not "save://slot/1.dat" */
    os_path_join(out, sizeof out, "sdmc:/switch/OpenSave/", "x");
    CHECK_STR(out, "sdmc:/switch/OpenSave/x");
}

int main(void) {
    char dir[] = "/tmp/os-state-XXXXXX";
    if (!mkdtemp(dir)) return 2;
    test_roundtrip(dir);
    test_damaged(dir);
    test_nonces();
    test_join();
    test_peer_limit();
    os_rm_rf(dir);
    return t_finish("state");
}
