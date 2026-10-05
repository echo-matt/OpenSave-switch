#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth.h"
#include "crypto.h"
#include "fsutil.h"
#include "json.h"
#include "manifest.h"
#include "peer.h"
#include "platform.h"
#include "title.h"

#define PAIRING_WINDOW_MS (2 * 60 * 1000)
#define INCOMING_WINDOW_MS (5 * 60 * 1000)
#define MAX_BLOCKS_PER_REQUEST 256
#define MAX_RAW_REPLY_BYTES (8 * 1024 * 1024)

typedef struct {
    int status;
    char *body;
    size_t len;
} reply;

static void reply_error(reply *r, int status, const char *msg) {
    os_sb sb;
    os_sb_init(&sb);
    os_sb_puts(&sb, "{\"error\":");
    os_sb_json_str(&sb, msg);
    os_sb_putc(&sb, '}');
    r->status = status;
    r->body = os_sb_take(&sb, &r->len);
}

static void reply_json(reply *r, int status, os_sb *sb) {
    r->status = status;
    r->body = os_sb_take(sb, &r->len);
    if (!r->body) reply_error(r, 500, "out of memory");
}

static void reply_text(reply *r, int status, const char *json) {
    r->status = status;
    r->body = strdup(json);
    r->len = r->body ? strlen(r->body) : 0;
}

/* ------------------------------------------------------------ authentication */

/* Verifies that a request was signed by a paired device, returning that device.
 * On failure fills r and returns NULL. */
static os_peer *authenticate(os_server *srv, const os_http_req *req, reply *r) {
    char claimed[96], nonce[96], ms[40], mac[96];
    os_peer *peer;
    int64_t at, now = os_now_ms(), skew;
    char want[OS_MAC_B64_LEN + 1];
    char *e;

    if (os_http_req_header(req, "X-Opensave-Peer", claimed, sizeof claimed) != 0 ||
        os_http_req_header(req, "X-Opensave-Nonce", nonce, sizeof nonce) != 0 ||
        os_http_req_header(req, "X-Opensave-Auth-Ms", ms, sizeof ms) != 0 ||
        os_http_req_header(req, "X-Opensave-Auth", mac, sizeof mac) != 0) {
        reply_error(r, 401, "Unauthorized: this request was not signed with the key from pairing.");
        return NULL;
    }
    peer = os_state_find_peer(srv->st, claimed);
    if (!peer) {
        reply_error(r, 401, "Unauthorized: requesting device is not paired with this Switch.");
        return NULL;
    }
    at = strtoll(ms, &e, 10);
    if (*e || at <= 0) {
        reply_error(r, 401, "Unauthorized: bad request timestamp.");
        return NULL;
    }
    skew = now - at;
    if (skew < 0) skew = -skew;
    if (skew > OS_MAX_AUTH_SKEW_MS) {
        char buf[300];
        snprintf(buf, sizeof buf,
                 "Unauthorized: this request is timestamped %lld seconds away from the Switch's clock. "
                 "Set the Switch's date and time in System Settings.", (long long)(skew / 1000));
        reply_error(r, 401, buf);
        return NULL;
    }
    os_request_mac(want, peer->authkey, claimed, srv->st->node_id, req->target, req->method, req->body, req->bodylen,
                   nonce, at);
    if (!os_mac_equal(want, mac)) {
        reply_error(r, 401, "Unauthorized: request failed authentication.");
        return NULL;
    }
    /* Only after the MAC verifies: recording it first would let anyone burn a
     * real device's nonces and make its genuine requests look like replays. */
    if (!os_state_remember_nonce(srv->st, nonce, now)) {
        reply_error(r, 401, "Unauthorized: request replayed.");
        return NULL;
    }
    return peer;
}

/* -------------------------------------------------------------------- routes */

static void route_ping(os_server *srv, const os_http_req *req, reply *r) {
    char from[96] = "";
    const char *q = strchr(req->target, '?');
    int paired = 1;
    os_sb sb;
    if (q && strncmp(q, "?from=", 6) == 0) {
        size_t n = strcspn(q + 6, "&");
        if (n < sizeof from) {
            memcpy(from, q + 6, n);
            from[n] = '\0';
            paired = os_state_find_peer(srv->st, from) != NULL;
        }
    }
    os_sb_init(&sb);
    os_sb_puts(&sb, "{\"status\":\"ok\",\"paired\":");
    os_sb_puts(&sb, paired ? "true" : "false");
    os_sb_puts(&sb, ",\"deviceName\":");
    os_sb_json_str(&sb, srv->st->device_name);
    os_sb_printf(&sb, ",\"deviceType\":\"handheld\",\"appVersion\":\"%s\",\"buildTimeMs\":0}", os_version_string());
    reply_json(r, 200, &sb);
}

static void route_handshake(os_server *srv, const os_http_req *req, reply *r) {
    os_json *d = os_json_parse(req->body ? req->body : "", req->bodylen, NULL, 0);
    os_jn root;
    const char *id, *name, *key;
    os_incoming *slot = NULL, tmp;
    int i;
    int64_t now = os_now_ms();

    if (!d) {
        reply_error(r, 400, "peerId is required");
        return;
    }
    root = os_json_root(d);
    id = os_json_get_str(d, root, "peerId");
    name = os_json_get_str(d, root, "deviceName");
    key = os_json_get_str(d, root, "publicKey");
    if (!id || !*id || strlen(id) > 63) {
        os_json_free(d);
        reply_error(r, 400, "peerId is required");
        return;
    }
    memset(&tmp, 0, sizeof tmp);
    snprintf(tmp.peer_id, sizeof tmp.peer_id, "%s", id);
    snprintf(tmp.name, sizeof tmp.name, "%s", name ? name : "Unknown device");
    snprintf(tmp.address, sizeof tmp.address, "%s", req->client_ip);
    tmp.port = (int)os_json_int(d, os_json_get(d, root, "port"), OS_DEFAULT_PORT);
    if (tmp.port <= 0 || tmp.port > 65535) tmp.port = OS_DEFAULT_PORT;
    if (key && strlen(key) == 44 && os_b64_decode(tmp.pubkey, key, 44) == 32) tmp.has_key = 1;
    tmp.at_ms = now;
    tmp.in_use = 1;
    os_json_free(d);

    /* Replace this device's earlier request, else take a free or expired slot,
     * else the oldest — so nothing on the network can fill the queue for good. */
    for (i = 0; i < OS_MAX_PENDING; i++)
        if (srv->st->incoming[i].in_use && strcmp(srv->st->incoming[i].peer_id, id) == 0) slot = &srv->st->incoming[i];
    for (i = 0; !slot && i < OS_MAX_PENDING; i++)
        if (!srv->st->incoming[i].in_use || now - srv->st->incoming[i].at_ms > INCOMING_WINDOW_MS) slot = &srv->st->incoming[i];
    if (!slot) {
        slot = &srv->st->incoming[0];
        for (i = 1; i < OS_MAX_PENDING; i++)
            if (srv->st->incoming[i].at_ms < slot->at_ms) slot = &srv->st->incoming[i];
    }
    *slot = tmp;
    if (srv->hooks.on_pairing_request) srv->hooks.on_pairing_request(srv->hooks.ctx, slot);
    reply_text(r, 200, "{\"status\":\"pending\",\"message\":\"Pairing request received. Waiting for approval on the Switch.\"}");
}

static void route_approve_confirm(os_server *srv, const os_http_req *req, reply *r) {
    os_json *d = os_json_parse(req->body ? req->body : "", req->bodylen, NULL, 0);
    os_jn root;
    const char *id, *name, *key;
    uint8_t pub[32];
    int port, i, allowed = 0;
    int64_t now = os_now_ms();
    os_peer *p;
    char err[96];

    if (!d) {
        reply_error(r, 400, "peerId is required");
        return;
    }
    root = os_json_root(d);
    id = os_json_get_str(d, root, "peerId");
    name = os_json_get_str(d, root, "deviceName");
    key = os_json_get_str(d, root, "publicKey");
    port = (int)os_json_int(d, os_json_get(d, root, "port"), OS_DEFAULT_PORT);
    if (!id || !*id || strlen(id) > 63) {
        os_json_free(d);
        reply_error(r, 400, "peerId is required");
        return;
    }
    /* This device only ever pairs with a key: every request it accepts is
     * signed, and there is nothing to sign with otherwise. */
    if (!key || strlen(key) != 44 || os_b64_decode(pub, key, 44) != 32) {
        os_json_free(d);
        reply_error(r, 400, "Pairing confirmation rejected: a public key is required.");
        return;
    }
    /* Accepted only if this device asked for it: we sent a handshake to that
     * address a moment ago. Anything else is somebody on the network trying to
     * be paired without anyone having agreed.
     *
     * The address alone is matched, not the port, as the other OpenSave
     * devices do: the port a device reports for itself is its own setting and
     * need not be the one we dialed (a port forward, a container). */
    for (i = 0; i < OS_MAX_PENDING; i++) {
        os_sent *sn = &srv->st->sent[i];
        if (sn->in_use && now - sn->at_ms < PAIRING_WINDOW_MS && strcmp(sn->address, req->client_ip) == 0) {
            allowed = 1;
            sn->in_use = 0;
            break;
        }
    }
    if (!allowed) {
        os_json_free(d);
        reply_error(r, 400, "Pairing confirmation rejected: no matching handshake initiated.");
        return;
    }
    p = os_state_add_peer(srv->st, id, name ? name : "Unknown device", req->client_ip, port, pub);
    os_json_free(d);
    if (!p) {
        reply_error(r, 500, "could not store the pairing (is the device list full?)");
        return;
    }
    if (os_state_save(srv->st, err, sizeof err) != 0) {
        /* Paired in memory but not on disk: say so rather than let it vanish. */
        reply_error(r, 500, err);
        return;
    }
    if (srv->hooks.on_paired) srv->hooks.on_paired(srv->hooks.ctx, p);
    reply_text(r, 200, "{\"success\":true,\"message\":\"Pairing confirmed.\"}");
}

/* Maps the game id in a path to a title and its mounted save. */
static int resolve_game(os_server *srv, const char *game_id, char title[OS_TITLE_LEN], char *root, size_t rootlen,
                        reply *r) {
    char err[200] = "";
    /* A game linked to a PC game is known by the PC's own id, which says nothing
     * about a Switch title: look the link up first. */
    os_link *link = os_state_find_link_by_game(srv->st, game_id);
    if (link) {
        snprintf(title, OS_TITLE_LEN, "%s", link->title);
        if (!srv->hooks.open_linked || srv->hooks.open_linked(srv->hooks.ctx, link, root, rootlen, err, sizeof err) != 0) {
            reply_error(r, 404, err[0] ? err : "Nothing has been received for this game yet.");
            return -1;
        }
        return 0;
    }
    if (!os_title_from_game_id(game_id, title)) {
        reply_error(r, 404, "Game not found.");
        return -1;
    }
    if (!srv->hooks.open_save ||
        srv->hooks.open_save(srv->hooks.ctx, title, root, rootlen, err, sizeof err) != 0) {
        reply_error(r, 404, err[0] ? err : "This Switch has no save data for that game.");
        return -1;
    }
    return 0;
}

static void route_manifest(os_server *srv, const char *game_id, reply *r) {
    char title[OS_TITLE_LEN], root[256], err[200];
    os_manifest m;
    os_sb sb;
    int missing;

    if (resolve_game(srv, game_id, title, root, sizeof root, r) != 0) return;
    if (os_manifest_build(root, &m, &missing, err, sizeof err) != 0) {
        reply_error(r, 500, err);
        return;
    }
    if (missing) {
        reply_error(r, 404, "This Switch has no save data for that game.");
        return;
    }
    os_sb_init(&sb);
    os_sb_puts(&sb, "{\"manifest\":");
    os_manifest_to_json(&m, &sb);
    os_sb_puts(&sb, ",\"activeBranch\":\"main\",\"latestSnapshot\":null}");
    os_manifest_free(&m);
    reply_json(r, 200, &sb);
    if (srv->hooks.on_served) srv->hooks.on_served(srv->hooks.ctx, "Sent the save's file list");
}

static void route_blocks(os_server *srv, const char *game_id, const os_http_req *req, reply *r) {
    char title[OS_TITLE_LEN], root[256], why[96];
    os_json *d = os_json_parse(req->body ? req->body : "", req->bodylen, NULL, 0);
    os_jn rp, idx, it;
    const char *rel, *rootname;
    int64_t bs;
    char path[1024];
    FILE *f = NULL;
    os_sb sb;
    unsigned char *buf = NULL;
    char *b64 = NULL;
    int64_t fsize, total = 0;
    int first = 1;

    if (!d) {
        reply_error(r, 400, "relPath is required");
        return;
    }
    rp = os_json_root(d);
    rel = os_json_get_str(d, rp, "relPath");
    rootname = os_json_get_str(d, rp, "root");
    bs = os_json_int(d, os_json_get(d, rp, "blockSize"), 0);
    idx = os_json_get(d, rp, "blockIndices");
    if (!rel || !*rel) {
        os_json_free(d);
        reply_error(r, 400, "relPath is required");
        return;
    }
    /* This device has one save location per game. A request for another is
     * refused rather than answered from the main one — the same rule the PC
     * applies — so a file is never served from the wrong place. */
    if (rootname && *rootname) {
        os_json_free(d);
        reply_error(r, 404, "This device has no save location with that name for that game.");
        return;
    }
    if (!os_path_valid(rel, why, sizeof why) || os_name_ignored(rel)) {
        os_json_free(d);
        reply_error(r, 400, "invalid path");
        return;
    }
    if (bs != OS_BLOCK_SMALL && bs != OS_BLOCK_MEDIUM && bs != OS_BLOCK_LARGE) {
        os_json_free(d);
        reply_error(r, 400, "unsupported block size");
        return;
    }
    if (os_json_type(d, idx) != J_ARR || os_json_count(d, idx) > MAX_BLOCKS_PER_REQUEST ||
        (int64_t)os_json_count(d, idx) * bs > MAX_RAW_REPLY_BYTES) {
        os_json_free(d);
        reply_error(r, 400, "too many blocks requested at once");
        return;
    }
    if (resolve_game(srv, game_id, title, root, sizeof root, r) != 0) {
        os_json_free(d);
        return;
    }
    os_path_join(path, sizeof path, root, rel);
    fsize = os_file_size(path);
    f = fsize >= 0 ? fopen(path, "rb") : NULL;
    if (!f) {
        os_json_free(d);
        reply_error(r, 404, "file not found");
        return;
    }
    buf = (unsigned char *)malloc((size_t)bs);
    b64 = (char *)malloc(os_b64_encoded_len((size_t)bs) + 1);
    if (!buf || !b64) {
        free(buf);
        free(b64);
        fclose(f);
        os_json_free(d);
        reply_error(r, 500, "out of memory");
        return;
    }
    os_sb_init(&sb);
    os_sb_puts(&sb, "{\"blocks\":[");
    for (it = os_json_first(d, idx); it >= 0; it = os_json_next(d, it)) {
        int64_t i = os_json_int(d, it, -1), off, len;
        size_t got;
        if (i < 0 || i * bs >= fsize) continue; /* past the end: nothing to send, as the Go side does */
        off = i * bs;
        len = fsize - off < bs ? fsize - off : bs;
        if (fseek(f, (long)off, SEEK_SET) != 0 || (got = fread(buf, 1, (size_t)len, f)) != (size_t)len) {
            os_sb_free(&sb);
            free(buf);
            free(b64);
            fclose(f);
            os_json_free(d);
            reply_error(r, 500, "read failed");
            return;
        }
        os_b64_encode(b64, buf, got);
        os_sb_printf(&sb, "%s{\"index\":%lld,\"data\":\"%s\",\"length\":%zu}", first ? "" : ",", (long long)i, b64, got);
        first = 0;
        total += (int64_t)got;
    }
    os_sb_puts(&sb, "]}");
    free(buf);
    free(b64);
    fclose(f);
    os_json_free(d);
    reply_json(r, 200, &sb);
    if (srv->hooks.on_served) srv->hooks.on_served(srv->hooks.ctx, "Sending save data");
    (void)total;
}

static void route_unpair(os_server *srv, os_peer *peer, reply *r) {
    /* The device that signed this is the one unpairing itself; whatever peerId
     * the body names is not consulted. */
    char id[64];
    snprintf(id, sizeof id, "%s", peer->id);
    os_state_remove_peer(srv->st, id);
    os_state_save(srv->st, NULL, 0);
    if (srv->hooks.on_unpaired) srv->hooks.on_unpaired(srv->hooks.ctx, id);
    reply_text(r, 200, "{\"success\":true}");
}

static void route_trigger(os_server *srv, os_peer *peer, const char *game_id, reply *r) {
    char title[OS_TITLE_LEN];
    if (os_title_from_game_id(game_id, title) && srv->hooks.on_peer_update)
        srv->hooks.on_peer_update(srv->hooks.ctx, peer, game_id, title);
    reply_text(r, 200, "{\"status\":\"triggered\"}");
}

static int handle(void *vctx, const os_http_req *req, int *status, char **body, size_t *len) {
    os_server *srv = (os_server *)vctx;
    reply r = {500, NULL, 0};
    const char *path = req->path;
    int is_get = strcmp(req->method, "GET") == 0, is_post = strcmp(req->method, "POST") == 0;
    os_peer *peer;

    /* Open routes. */
    if (is_get && strcmp(path, "/api/p2p/ping") == 0) route_ping(srv, req, &r);
    else if (is_post && strcmp(path, "/api/p2p/handshake") == 0) route_handshake(srv, req, &r);
    else if (is_post && strcmp(path, "/api/p2p/approve-confirm") == 0) route_approve_confirm(srv, req, &r);
    /* Everything else is for paired devices only. */
    else if (strncmp(path, "/api/p2p/", 9) == 0 || strncmp(path, "/api/sync/trigger/", 18) == 0) {
        peer = authenticate(srv, req, &r);
        if (peer) {
            if (is_get && strncmp(path, "/api/p2p/manifest/", 18) == 0) route_manifest(srv, path + 18, &r);
            else if (is_post && strncmp(path, "/api/p2p/blocks/", 16) == 0) route_blocks(srv, path + 16, req, &r);
            else if (is_post && strcmp(path, "/api/p2p/unpair") == 0) route_unpair(srv, peer, &r);
            else if (is_get && strncmp(path, "/api/sync/trigger/", 18) == 0) route_trigger(srv, peer, path + 18, &r);
            else if (is_get && strcmp(path, "/api/p2p/games") == 0) reply_text(&r, 200, "[]");
            else if (is_post && strncmp(path, "/api/p2p/sync-event/", 20) == 0) reply_text(&r, 200, "{\"success\":true}");
            else if (is_post && (strcmp(path, "/api/p2p/untrack") == 0 || strcmp(path, "/api/p2p/retrack") == 0))
                reply_text(&r, 200, "{\"success\":true}");
            else if (is_post && strncmp(path, "/api/p2p/delete-file/", 21) == 0)
                /* Another device never deletes files here: this device's
                 * saves change only when its owner says so. */
                reply_error(&r, 403, "This Switch does not accept remote deletions.");
            else reply_error(&r, 404, "not found");
        }
    } else reply_error(&r, 404, "not found");

    *status = r.status;
    *body = r.body;
    *len = r.len;
    return 0;
}

int os_server_start(os_server *s, os_state *st, const os_server_hooks *hooks, int port, char *err, size_t errlen) {
    memset(s, 0, sizeof *s);
    s->st = st;
    if (hooks) s->hooks = *hooks;
    s->http = os_http_server_start(port, err, errlen);
    return s->http ? 0 : -1;
}

int os_server_poll(os_server *s, int timeout_ms) {
    /* Error screens are shown before the server exists; polling then is a no-op. */
    if (!s->http) return 0;
    return os_http_server_poll(s->http, timeout_ms, handle, s);
}

void os_server_stop(os_server *s) {
    if (s->http) os_http_server_stop(s->http);
    s->http = NULL;
}

int os_server_port(const os_server *s) { return s->http ? os_http_server_port(s->http) : 0; }

int os_server_approve(os_server *s, const char *peer_id, char *err, size_t errlen) {
    int i;
    os_incoming *in = NULL, copy;
    os_peer *p;
    for (i = 0; i < OS_MAX_PENDING; i++)
        if (s->st->incoming[i].in_use && strcmp(s->st->incoming[i].peer_id, peer_id) == 0) in = &s->st->incoming[i];
    if (!in) {
        snprintf(err, errlen, "that pairing request is gone");
        return -1;
    }
    if (!in->has_key) {
        in->in_use = 0;
        snprintf(err, errlen, "that device sent no key, so it cannot be paired with");
        return -1;
    }
    copy = *in;
    in->in_use = 0;
    p = os_state_add_peer(s->st, copy.peer_id, copy.name, copy.address, copy.port, copy.pubkey);
    if (!p) {
        snprintf(err, errlen, "could not store the pairing (is the device list full?)");
        return -1;
    }
    if (os_state_save(s->st, err, errlen) != 0) return -1;
    /* Stored first and told second, as the other side does it: the device
     * will start talking the moment it hears. */
    if (os_peer_approve_confirm(s->st, copy.address, copy.port, err, errlen) != 0) {
        char inner[160];
        snprintf(inner, sizeof inner, "%s", err);
        snprintf(err, errlen,
                 "paired here, but could not tell that device (%s). Check that it allows incoming connections, then pair again from it.",
                 inner);
        return -1;
    }
    if (s->hooks.on_paired) s->hooks.on_paired(s->hooks.ctx, p);
    return 0;
}

void os_server_reject(os_server *s, const char *peer_id) {
    int i;
    for (i = 0; i < OS_MAX_PENDING; i++)
        if (s->st->incoming[i].in_use && strcmp(s->st->incoming[i].peer_id, peer_id) == 0) s->st->incoming[i].in_use = 0;
}
