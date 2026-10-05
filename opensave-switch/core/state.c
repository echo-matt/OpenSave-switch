#include "state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "auth.h"
#include "fsutil.h"
#include "json.h"
#include "platform.h"

static void seterr(char *err, size_t n, const char *msg) {
    if (err && n) snprintf(err, n, "%s", msg);
}

static void copy_str(char *dst, size_t n, const char *src) {
    if (!src) src = "";
    snprintf(dst, n, "%s", src);
}

static int new_identity(os_state *s) {
    uint8_t raw[16];
    char hex[33];
    if (os_random(s->priv, sizeof s->priv) != 0 || os_x25519_public(s->pub, s->priv) != 0) return -1;
    if (os_random(raw, sizeof raw) != 0) return -1;
    os_hex_encode(hex, raw, sizeof raw);
    snprintf(s->node_id, sizeof s->node_id, "node_%s", hex);
    return 0;
}

os_peer *os_state_find_peer(os_state *s, const char *id) {
    int i;
    if (!id || !*id) return NULL;
    for (i = 0; i < OS_MAX_PEERS; i++)
        if (s->peers[i].in_use && strcmp(s->peers[i].id, id) == 0) return &s->peers[i];
    return NULL;
}

os_peer *os_state_add_peer(os_state *s, const char *id, const char *name, const char *address, int port,
                           const uint8_t pubkey[32]) {
    os_peer *p = os_state_find_peer(s, id);
    int i;
    if (!p) {
        for (i = 0; i < OS_MAX_PEERS; i++)
            if (!s->peers[i].in_use) {
                p = &s->peers[i];
                break;
            }
    }
    if (!p) return NULL;
    memset(p, 0, sizeof *p);
    copy_str(p->id, sizeof p->id, id);
    copy_str(p->name, sizeof p->name, name);
    copy_str(p->address, sizeof p->address, address);
    p->port = port;
    memcpy(p->pubkey, pubkey, 32);
    if (os_auth_key(p->authkey, s->priv, p->pubkey) != 0) {
        memset(p, 0, sizeof *p);
        return NULL;
    }
    p->in_use = 1;
    return p;
}

int os_state_remove_peer(os_state *s, const char *id) {
    os_peer *p = os_state_find_peer(s, id);
    if (!p) return 0;
    memset(p, 0, sizeof *p);
    return 1;
}

os_link *os_state_find_link(os_state *s, const char *title) {
    int i;
    for (i = 0; title && i < OS_MAX_LINKS; i++)
        if (s->links[i].in_use && strcasecmp(s->links[i].title, title) == 0) return &s->links[i];
    return NULL;
}

os_link *os_state_find_link_by_game(os_state *s, const char *game_id) {
    int i;
    for (i = 0; game_id && i < OS_MAX_LINKS; i++)
        if (s->links[i].in_use && strcmp(s->links[i].game_id, game_id) == 0) return &s->links[i];
    return NULL;
}

os_link *os_state_set_link(os_state *s, const char *title, const char *game_id, const char *name) {
    os_link *l = os_state_find_link(s, title);
    int i;
    for (i = 0; !l && i < OS_MAX_LINKS; i++)
        if (!s->links[i].in_use) l = &s->links[i];
    if (!l) return NULL;
    memset(l, 0, sizeof *l);
    snprintf(l->title, sizeof l->title, "%s", title);
    snprintf(l->game_id, sizeof l->game_id, "%s", game_id);
    snprintf(l->name, sizeof l->name, "%s", name ? name : "");
    l->in_use = 1;
    return l;
}

void os_state_remove_link(os_state *s, const char *title) {
    os_link *l = os_state_find_link(s, title);
    if (l) memset(l, 0, sizeof *l);
}

int os_state_remember_nonce(os_state *s, const char *nonce, int64_t now_ms) {
    int i;
    for (i = 0; i < OS_NONCE_CACHE; i++)
        if (s->seen[i].expires_ms >= now_ms && strcmp(s->seen[i].nonce, nonce) == 0) return 0;
    /* A ring: the oldest entry makes way. Held for twice the skew window, as
     * the Go side holds them, so a request stamped as far ahead as is accepted
     * cannot be replayed once it has been forgotten. */
    snprintf(s->seen[s->seen_next].nonce, sizeof s->seen[0].nonce, "%s", nonce);
    s->seen[s->seen_next].expires_ms = now_ms + 2 * OS_MAX_AUTH_SKEW_MS;
    s->seen_next = (s->seen_next + 1) % OS_NONCE_CACHE;
    return 1;
}

int os_state_load(os_state *s, const char *path, char *err, size_t errlen) {
    FILE *f;
    char *text;
    long n;
    os_json *d;
    os_jn root, peers, it;
    char jerr[80];

    memset(s, 0, sizeof *s);
    copy_str(s->path, sizeof s->path, path);
    f = fopen(path, "rb");
    if (!f) {
        /* First run: a new identity, written out so it is the same next time. */
        if (new_identity(s) != 0) {
            seterr(err, errlen, "could not generate this device's key");
            return -1;
        }
        copy_str(s->device_name, sizeof s->device_name, "Nintendo Switch");
        s->port = OS_DEFAULT_PORT;
        return os_state_save(s, err, errlen);
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 256 * 1024) {
        fclose(f);
        seterr(err, errlen, "state file is not usable");
        return -1;
    }
    text = (char *)malloc((size_t)n + 1);
    if (!text || fread(text, 1, (size_t)n, f) != (size_t)n) {
        free(text);
        fclose(f);
        seterr(err, errlen, "could not read the state file");
        return -1;
    }
    fclose(f);
    d = os_json_parse(text, (size_t)n, jerr, sizeof jerr);
    free(text);
    if (!d) {
        /* Never overwrite what cannot be read: it holds the only copy of this
         * device's key, and starting afresh would unpair every device. */
        if (err && errlen) snprintf(err, errlen, "state file is damaged (%s); not replacing it", jerr);
        return -1;
    }
    root = os_json_root(d);
    copy_str(s->node_id, sizeof s->node_id, os_json_get_str(d, root, "nodeId"));
    copy_str(s->device_name, sizeof s->device_name, os_json_get_str(d, root, "deviceName"));
    s->port = (int)os_json_int(d, os_json_get(d, root, "port"), OS_DEFAULT_PORT);
    if (s->port <= 0 || s->port > 65535) s->port = OS_DEFAULT_PORT;
    if (!s->device_name[0]) copy_str(s->device_name, sizeof s->device_name, "Nintendo Switch");
    {
        const char *pk = os_json_get_str(d, root, "privateKey");
        if (!s->node_id[0] || !pk || strlen(pk) != 44 || os_b64_decode(s->priv, pk, 44) != 32 ||
            os_x25519_public(s->pub, s->priv) != 0) {
            os_json_free(d);
            seterr(err, errlen, "state file has no valid device key; not replacing it");
            return -1;
        }
    }
    for (it = os_json_first(d, os_json_get(d, root, "links")); it >= 0; it = os_json_next(d, it)) {
        const char *t = os_json_get_str(d, it, "title"), *g = os_json_get_str(d, it, "gameId");
        if (t && g && strlen(t) == 16) os_state_set_link(s, t, g, os_json_get_str(d, it, "name"));
    }
    peers = os_json_get(d, root, "peers");
    for (it = os_json_first(d, peers); it >= 0; it = os_json_next(d, it)) {
        uint8_t pub[32];
        const char *pk = os_json_get_str(d, it, "publicKey");
        if (!pk || strlen(pk) != 44 || os_b64_decode(pub, pk, 44) != 32) continue;
        os_state_add_peer(s, os_json_get_str(d, it, "id"), os_json_get_str(d, it, "name"),
                          os_json_get_str(d, it, "address"), (int)os_json_int(d, os_json_get(d, it, "port"), OS_DEFAULT_PORT),
                          pub);
    }
    os_json_free(d);
    return 0;
}

int os_state_save(const os_state *s, char *err, size_t errlen) {
    os_sb sb;
    char b64[64], tmp[300];
    char *text;
    size_t n;
    FILE *f;
    int i, first = 1;

    os_sb_init(&sb);
    os_sb_puts(&sb, "{\"nodeId\":");
    os_sb_json_str(&sb, s->node_id);
    os_sb_puts(&sb, ",\"deviceName\":");
    os_sb_json_str(&sb, s->device_name);
    os_sb_printf(&sb, ",\"port\":%d,\"privateKey\":\"", s->port);
    os_b64_encode(b64, s->priv, 32);
    os_sb_puts(&sb, b64);
    os_sb_puts(&sb, "\",\"peers\":[");
    for (i = 0; i < OS_MAX_PEERS; i++) {
        const os_peer *p = &s->peers[i];
        if (!p->in_use) continue;
        if (!first) os_sb_putc(&sb, ',');
        first = 0;
        os_sb_puts(&sb, "{\"id\":");
        os_sb_json_str(&sb, p->id);
        os_sb_puts(&sb, ",\"name\":");
        os_sb_json_str(&sb, p->name);
        os_sb_puts(&sb, ",\"address\":");
        os_sb_json_str(&sb, p->address);
        os_b64_encode(b64, p->pubkey, 32);
        os_sb_printf(&sb, ",\"port\":%d,\"publicKey\":\"%s\"}", p->port, b64);
    }
    os_sb_puts(&sb, "],\"links\":[");
    first = 1;
    for (i = 0; i < OS_MAX_LINKS; i++) {
        const os_link *l = &s->links[i];
        if (!l->in_use) continue;
        if (!first) os_sb_putc(&sb, ',');
        first = 0;
        os_sb_puts(&sb, "{\"title\":");
        os_sb_json_str(&sb, l->title);
        os_sb_puts(&sb, ",\"gameId\":");
        os_sb_json_str(&sb, l->game_id);
        os_sb_puts(&sb, ",\"name\":");
        os_sb_json_str(&sb, l->name);
        os_sb_putc(&sb, '}');
    }
    os_sb_puts(&sb, "]");
    os_sb_putc(&sb, '}');
    text = os_sb_take(&sb, &n);
    if (!text) {
        seterr(err, errlen, "out of memory");
        return -1;
    }
    if (os_mkdir_parent(s->path) != 0) {
        free(text);
        seterr(err, errlen, "could not create the settings folder");
        return -1;
    }
    /* Written beside the real file and moved into place, so a failure part-way
     * never leaves the only copy of the device key half-written. */
    snprintf(tmp, sizeof tmp, "%s.new", s->path);
    f = fopen(tmp, "wb");
    if (!f || fwrite(text, 1, n, f) != n) {
        if (f) fclose(f);
        free(text);
        seterr(err, errlen, "could not write the settings file");
        return -1;
    }
    free(text);
    if (fclose(f) != 0) {
        seterr(err, errlen, "could not write the settings file");
        return -1;
    }
    if (rename(tmp, s->path) != 0) {
        /* FAT will not rename over an existing file. */
        remove(s->path);
        if (rename(tmp, s->path) != 0) {
            seterr(err, errlen, "could not replace the settings file");
            return -1;
        }
    }
    return 0;
}
