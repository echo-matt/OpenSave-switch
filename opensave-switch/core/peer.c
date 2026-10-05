#include "peer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth.h"
#include "crypto.h"
#include "json.h"
#include "platform.h"
#include "title.h"

#define NET_TIMEOUT_MS 8000
#define SYNC_TIMEOUT_MS 30000

static void seterr(char *err, size_t n, const char *msg) {
    if (err && n) snprintf(err, n, "%s", msg);
}

void os_url_escape(char *out, size_t outlen, const char *in) {
    static const char H[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < outlen; in++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = H[c >> 4];
            out[o++] = H[c & 15];
        }
    }
    if (outlen) out[o] = '\0';
}

const char *os_clock_hint(const os_state *s, char *buf, size_t buflen) {
    int64_t skew = s->clock_skew_s;
    if (!s->skew_known) {
        if (buflen) buf[0] = '\0';
        return buf;
    }
    if (skew < 0) skew = -skew;
    /* The limit is five minutes; warn well inside it. */
    if (skew < 120) {
        if (buflen) buf[0] = '\0';
        return buf;
    }
    snprintf(buf, buflen,
             "The Switch's clock is about %lld minute(s) %s the other device's. Requests are refused "
             "when the clocks differ by more than 5 minutes: set the Switch's date and time in System Settings.",
             (long long)(skew / 60), s->clock_skew_s > 0 ? "behind" : "ahead of");
    return buf;
}

static void note_skew(os_state *s, const os_http_resp *r) {
    if (r->server_time > 0) {
        s->clock_skew_s = r->server_time - os_now_ms() / 1000;
        s->skew_known = 1;
    }
}

/* Turns a non-200 reply into a message: the other device's own words when it
 * sent {"error": "..."}, which are written for a person. */
static void explain(os_state *s, const os_http_resp *r, char *err, size_t errlen) {
    char msg[200] = "", hint[260];
    if (r->body && r->bodylen) {
        os_json *d = os_json_parse(r->body, r->bodylen, NULL, 0);
        if (d) {
            const char *e = os_json_get_str(d, os_json_root(d), "error");
            if (!e) e = os_json_get_str(d, os_json_root(d), "message");
            if (e) snprintf(msg, sizeof msg, "%s", e);
            os_json_free(d);
        }
    }
    if (!msg[0]) snprintf(msg, sizeof msg, "the other device answered with HTTP %d", r->status);
    if (r->status == 401 && os_clock_hint(s, hint, sizeof hint)[0])
        snprintf(err, errlen, "%s %s", msg, hint);
    else
        snprintf(err, errlen, "%s", msg);
}

int os_peer_call(os_state *s, const os_peer *p, const char *method, const char *target,
                 const void *body, size_t bodylen, size_t max_body, int timeout_ms, os_http_resp *out,
                 int *status, char *err, size_t errlen) {
    char nonce[33], mac[OS_MAC_B64_LEN + 1], ms[24];
    int64_t now = os_now_ms();
    os_hdr h[4];
    char terr[160];

    if (status) *status = 0;
    if (os_new_nonce(nonce) != 0) {
        seterr(err, errlen, "could not generate a request nonce");
        return -1;
    }
    os_request_mac(mac, p->authkey, s->node_id, p->id, target, method, body, bodylen, nonce, now);
    snprintf(ms, sizeof ms, "%lld", (long long)now);
    h[0].name = "X-Opensave-Peer";
    h[0].value = s->node_id;
    h[1].name = "X-Opensave-Nonce";
    h[1].value = nonce;
    h[2].name = "X-Opensave-Auth-Ms";
    h[2].value = ms;
    h[3].name = "X-Opensave-Auth";
    h[3].value = mac;

    if (os_http_request(p->address, p->port, method, target, h, 4, body, bodylen, max_body, timeout_ms, out, terr,
                        sizeof terr) != 0) {
        snprintf(err, errlen, "%s", terr);
        return -1;
    }
    note_skew(s, out);
    if (status) *status = out->status;
    if (out->status != 200) {
        explain(s, out, err, errlen);
        os_http_resp_free(out);
        return -1;
    }
    return 0;
}

/* --------------------------------------------------------------- pairing */

int os_peer_ping(os_state *s, const char *address, int port, os_ping_info *out, char *err, size_t errlen) {
    char target[128], terr[160];
    os_http_resp r;
    os_json *d;
    os_jn root;

    memset(out, 0, sizeof *out);
    snprintf(target, sizeof target, "/api/p2p/ping?from=%s", s->node_id);
    if (os_http_request(address, port, "GET", target, NULL, 0, NULL, 0, 64 * 1024, NET_TIMEOUT_MS, &r, terr,
                        sizeof terr) != 0) {
        snprintf(err, errlen, "%s", terr);
        return -1;
    }
    note_skew(s, &r);
    if (r.status != 200) {
        snprintf(err, errlen, "that address answered with HTTP %d, so it is not OpenSave", r.status);
        os_http_resp_free(&r);
        return -1;
    }
    d = os_json_parse(r.body, r.bodylen, NULL, 0);
    os_http_resp_free(&r);
    if (!d || !os_json_get_str(d, os_json_root(d), "status")) {
        os_json_free(d);
        seterr(err, errlen, "that address answered, but not like OpenSave");
        return -1;
    }
    root = os_json_root(d);
    snprintf(out->device_name, sizeof out->device_name, "%s", os_json_get_str(d, root, "deviceName") ? os_json_get_str(d, root, "deviceName") : "");
    snprintf(out->version, sizeof out->version, "%s", os_json_get_str(d, root, "appVersion") ? os_json_get_str(d, root, "appVersion") : "");
    out->paired = os_json_bool(d, os_json_get(d, root, "paired"), 0);
    os_json_free(d);
    return 0;
}

static void identity_body(const os_state *s, os_sb *sb) {
    char pub[64];
    os_b64_encode(pub, s->pub, 32);
    os_sb_puts(sb, "{\"peerId\":");
    os_sb_json_str(sb, s->node_id);
    os_sb_puts(sb, ",\"deviceName\":");
    os_sb_json_str(sb, s->device_name);
    os_sb_printf(sb, ",\"deviceType\":\"handheld\",\"port\":%d,\"publicKey\":\"%s\"}", s->port, pub);
}

static int post_identity(os_state *s, const char *address, int port, const char *route, char *err, size_t errlen) {
    os_sb sb;
    char *body, terr[160];
    size_t n;
    os_http_resp r;
    int rc;

    os_sb_init(&sb);
    identity_body(s, &sb);
    body = os_sb_take(&sb, &n);
    if (!body) {
        seterr(err, errlen, "out of memory");
        return -1;
    }
    rc = os_http_request(address, port, "POST", route, NULL, 0, body, n, 64 * 1024, NET_TIMEOUT_MS, &r, terr, sizeof terr);
    free(body);
    if (rc != 0) {
        snprintf(err, errlen, "%s", terr);
        return -1;
    }
    note_skew(s, &r);
    if (r.status != 200) {
        explain(s, &r, err, errlen);
        os_http_resp_free(&r);
        return -1;
    }
    os_http_resp_free(&r);
    return 0;
}

int os_peer_handshake(os_state *s, const char *address, int port, char *err, size_t errlen) {
    int i, slot = -1;
    /* Opened before the request, as the Go side does: the other device may
     * answer by calling /approve-confirm before this call even returns. */
    for (i = 0; i < OS_MAX_PENDING; i++)
        if (s->sent[i].in_use && strcmp(s->sent[i].address, address) == 0 && s->sent[i].port == port) slot = i;
    for (i = 0; slot < 0 && i < OS_MAX_PENDING; i++)
        if (!s->sent[i].in_use) slot = i;
    if (slot < 0) slot = 0; /* the oldest request makes way */
    snprintf(s->sent[slot].address, sizeof s->sent[slot].address, "%s", address);
    s->sent[slot].port = port;
    s->sent[slot].at_ms = os_now_ms();
    s->sent[slot].in_use = 1;
    if (post_identity(s, address, port, "/api/p2p/handshake", err, errlen) != 0) {
        s->sent[slot].in_use = 0;
        return -1;
    }
    return 0;
}

int os_peer_approve_confirm(os_state *s, const char *address, int port, char *err, size_t errlen) {
    return post_identity(s, address, port, "/api/p2p/approve-confirm", err, errlen);
}

/* ------------------------------------------------------------- manifests */

void os_remote_manifest_free(os_remote_manifest *r) {
    os_manifest_free(&r->manifest);
    memset(r, 0, sizeof *r);
}

int os_peer_fetch_manifest(os_state *s, const os_peer *p, const char *game_id, const char *name,
                           const char *save_path, os_remote_manifest *out, char *err, size_t errlen) {
    char target[1024], en[256], es[256], eg[160];
    os_http_resp r;
    os_json *d;
    os_jn root;
    int status;

    memset(out, 0, sizeof *out);
    os_url_escape(eg, sizeof eg, game_id);
    os_url_escape(en, sizeof en, name ? name : "");
    os_url_escape(es, sizeof es, save_path ? save_path : "");
    snprintf(target, sizeof target, "/api/p2p/manifest/%s?name=%s&savePath=%s&isFile=false", eg, en, es);
    if (os_peer_call(s, p, "GET", target, NULL, 0, 16 * 1024 * 1024, SYNC_TIMEOUT_MS, &r, &status, err, errlen) != 0)
        return -1;
    d = os_json_parse(r.body, r.bodylen, err, errlen);
    os_http_resp_free(&r);
    if (!d) return -1;
    root = os_json_root(d);
    if (os_manifest_from_json(d, os_json_get(d, root, "manifest"), &out->manifest, err, errlen) != 0) {
        os_json_free(d);
        return -1;
    }
    snprintf(out->branch, sizeof out->branch, "%s", os_json_get_str(d, root, "activeBranch") ? os_json_get_str(d, root, "activeBranch") : "");
    os_json_free(d);
    return 0;
}

int os_peer_fetch_blocks(os_state *s, const os_peer *p, const char *game_id, const os_mfile *f, int first,
                         int count, uint8_t *dst, char *err, size_t errlen) {
    char target[200], eg[160];
    os_sb sb;
    char *body;
    size_t bn;
    os_http_resp r;
    os_json *d;
    os_jn blocks, it;
    int status, i, seen = 0;
    int64_t offset = (int64_t)first * f->block_size;
    unsigned char *seen_flags;
    uint8_t *scratch;

    if (count <= 0 || first < 0 || first + count > f->nblocks) {
        seterr(err, errlen, "block range out of bounds");
        return -1;
    }
    os_url_escape(eg, sizeof eg, game_id);
    snprintf(target, sizeof target, "/api/p2p/blocks/%s", eg);
    os_sb_init(&sb);
    os_sb_puts(&sb, "{\"relPath\":");
    os_sb_json_str(&sb, f->path);
    os_sb_puts(&sb, ",\"root\":\"\",\"blockIndices\":[");
    for (i = 0; i < count; i++) os_sb_printf(&sb, "%s%d", i ? "," : "", first + i);
    os_sb_printf(&sb, "],\"blockSize\":%d}", f->block_size);
    body = os_sb_take(&sb, &bn);
    if (!body) {
        seterr(err, errlen, "out of memory");
        return -1;
    }
    /* base64 inflates by a third, plus the JSON around it. */
    if (os_peer_call(s, p, "POST", target, body, bn, (size_t)count * (size_t)f->block_size * 2 + 65536,
                     SYNC_TIMEOUT_MS, &r, &status, err, errlen) != 0) {
        free(body);
        return -1;
    }
    free(body);
    d = os_json_parse(r.body, r.bodylen, err, errlen);
    os_http_resp_free(&r);
    if (!d) return -1;
    blocks = os_json_get(d, os_json_root(d), "blocks");
    if (os_json_type(d, blocks) != J_ARR) {
        os_json_free(d);
        seterr(err, errlen, "the reply has no blocks");
        return -1;
    }
    seen_flags = (unsigned char *)calloc((size_t)count, 1);
    /* Peer data is decoded here first, never straight into the destination: a
     * base64 string without its padding decodes to up to two bytes more than
     * the block it claims to be, and writing those in place would run past
     * the end of dst. */
    scratch = (uint8_t *)malloc((size_t)f->block_size + 3);
    if (!seen_flags || !scratch) {
        free(seen_flags);
        free(scratch);
        os_json_free(d);
        seterr(err, errlen, "out of memory");
        return -1;
    }
    for (it = os_json_first(d, blocks); it >= 0; it = os_json_next(d, it)) {
        int64_t idx = os_json_int(d, os_json_get(d, it, "index"), -1);
        const char *data = os_json_get_str(d, it, "data");
        const char *enc = os_json_get_str(d, it, "enc");
        const os_block *want;
        long got;
        uint8_t sum[32];
        size_t dl = data ? strlen(data) : 0;

        if (idx < first || idx >= first + count || seen_flags[idx - first]) {
            free(seen_flags);
            free(scratch);
            os_json_free(d);
            seterr(err, errlen, "the reply holds a block that was not asked for");
            return -1;
        }
        if (enc && *enc) {
            free(seen_flags);
            free(scratch);
            os_json_free(d);
            snprintf(err, errlen, "the reply uses an encoding (%s) this device did not ask for", enc);
            return -1;
        }
        want = &f->blocks[idx];
        /* Bounded before decoding: the destination holds exactly this block. */
        if (!data || dl % 4 != 0 || dl / 4 * 3 > (size_t)want->length + 2) {
            free(seen_flags);
            free(scratch);
            os_json_free(d);
            seterr(err, errlen, "a block has the wrong size");
            return -1;
        }
        got = os_b64_decode(scratch, data, dl);
        if (got != want->length) {
            free(seen_flags);
            free(scratch);
            os_json_free(d);
            seterr(err, errlen, "a block has the wrong size");
            return -1;
        }
        os_sha256(scratch, (size_t)got, sum);
        if (memcmp(sum, want->hash, 32) != 0) {
            free(seen_flags);
            free(scratch);
            os_json_free(d);
            seterr(err, errlen, "a block does not match the hash in the manifest (corrupted or tampered with)");
            return -1;
        }
        memcpy(dst + ((int64_t)idx * f->block_size - offset), scratch, (size_t)got);
        seen_flags[idx - first] = 1;
        seen++;
    }
    free(seen_flags);
    free(scratch);
    os_json_free(d);
    if (seen != count) {
        seterr(err, errlen, "the reply is missing blocks");
        return -1;
    }
    return 0;
}

/* ----------------------------------------------------------------- games */

int os_peer_list_games(os_state *s, const os_peer *p, os_remote_game **out, int *n, char *err, size_t errlen) {
    char target[160];
    os_http_resp r;
    os_json *d;
    os_jn root, it;
    int status, cnt = 0;
    os_remote_game *list;

    *out = NULL;
    *n = 0;
    snprintf(target, sizeof target, "/api/p2p/games?from=%s", s->node_id);
    if (os_peer_call(s, p, "GET", target, NULL, 0, 8 * 1024 * 1024, NET_TIMEOUT_MS, &r, &status, err, errlen) != 0)
        return -1;
    d = os_json_parse(r.body, r.bodylen, err, errlen);
    os_http_resp_free(&r);
    if (!d) return -1;
    root = os_json_root(d);
    if (os_json_type(d, root) != J_ARR) {
        os_json_free(d);
        seterr(err, errlen, "the game list is not a list");
        return -1;
    }
    list = (os_remote_game *)calloc(os_json_count(d, root) + 1, sizeof *list);
    if (!list) {
        os_json_free(d);
        seterr(err, errlen, "out of memory");
        return -1;
    }
    for (it = os_json_first(d, root); it >= 0; it = os_json_next(d, it)) {
        const char *id = os_json_get_str(d, it, "id"), *nm = os_json_get_str(d, it, "name"),
                   *sp = os_json_get_str(d, it, "savePath");
        if (!id) continue;
        snprintf(list[cnt].id, sizeof list[cnt].id, "%s", id);
        snprintf(list[cnt].name, sizeof list[cnt].name, "%s", nm ? nm : "");
        snprintf(list[cnt].save_path, sizeof list[cnt].save_path, "%s", sp ? sp : "");
        cnt++;
    }
    os_json_free(d);
    *out = list;
    *n = cnt;
    return 0;
}

int os_peer_find_title(os_state *s, const os_peer *p, const char *title_id, os_remote_game *out, int *found,
                       char *err, size_t errlen) {
    os_remote_game *list;
    int n, i, hits = 0;
    char t[OS_TITLE_LEN];

    *found = 0;
    memset(out, 0, sizeof *out);
    if (os_peer_list_games(s, p, &list, &n, err, errlen) != 0) return -1;
    for (i = 0; i < n; i++) {
        /* The save path decides first, then the id, as switchtitle.Of does. */
        int ok = os_title_from_save_path(list[i].save_path, t) || os_title_from_game_id(list[i].id, t);
        if (ok && strcmp(t, title_id) == 0) {
            if (hits == 0) *out = list[i];
            hits++;
        }
    }
    free(list);
    *found = hits == 0 ? 0 : hits == 1 ? 1 : 2;
    return 0;
}

/* ---------------------------------------------------------------- events */

int os_peer_send_event(os_state *s, const os_peer *p, const char *game_id, const char *event,
                       const char *data_json, char *err, size_t errlen) {
    char target[200], eg[160];
    os_sb sb;
    char *body;
    size_t n;
    os_http_resp r;
    int status, rc;
    os_url_escape(eg, sizeof eg, game_id);
    snprintf(target, sizeof target, "/api/p2p/sync-event/%s", eg);
    os_sb_init(&sb);
    os_sb_puts(&sb, "{\"eventType\":");
    os_sb_json_str(&sb, event);
    os_sb_printf(&sb, ",\"data\":%s}", data_json && *data_json ? data_json : "{}");
    body = os_sb_take(&sb, &n);
    if (!body) {
        seterr(err, errlen, "out of memory");
        return -1;
    }
    rc = os_peer_call(s, p, "POST", target, body, n, 64 * 1024, NET_TIMEOUT_MS, &r, &status, err, errlen);
    free(body);
    if (rc == 0) os_http_resp_free(&r);
    return rc;
}

int os_peer_trigger_sync(os_state *s, const os_peer *p, const char *game_id, char *err, size_t errlen) {
    char target[200], eg[160];
    os_http_resp r;
    int status;
    os_url_escape(eg, sizeof eg, game_id);
    snprintf(target, sizeof target, "/api/sync/trigger/%s", eg);
    if (os_peer_call(s, p, "GET", target, NULL, 0, 64 * 1024, NET_TIMEOUT_MS, &r, &status, err, errlen) != 0) return -1;
    os_http_resp_free(&r);
    return 0;
}

int os_peer_unpair(os_state *s, const os_peer *p, char *err, size_t errlen) {
    os_sb sb;
    char *body;
    size_t n;
    os_http_resp r;
    int status, rc;
    os_sb_init(&sb);
    os_sb_puts(&sb, "{\"peerId\":");
    os_sb_json_str(&sb, s->node_id);
    os_sb_putc(&sb, '}');
    body = os_sb_take(&sb, &n);
    if (!body) {
        seterr(err, errlen, "out of memory");
        return -1;
    }
    rc = os_peer_call(s, p, "POST", "/api/p2p/unpair", body, n, 64 * 1024, NET_TIMEOUT_MS, &r, &status, err, errlen);
    free(body);
    if (rc == 0) os_http_resp_free(&r);
    return rc;
}

