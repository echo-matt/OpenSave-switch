/* Talking to a paired OpenSave device: the client half of the LAN protocol. */
#ifndef OPENSAVE_PEER_H
#define OPENSAVE_PEER_H

#include <stddef.h>
#include <stdint.h>

#include "http.h"
#include "manifest.h"
#include "state.h"

#define OS_BLOCK_BATCH_BYTES (1024 * 1024) /* raw bytes requested per round trip */

typedef struct {
    char device_name[64];
    char version[48];
    int paired; /* the device says it is paired with us */
} os_ping_info;

/* ------------------------------------------------------------ unpaired */

/* Is an OpenSave device answering there? from is this device's id, so the other
 * side can say whether it still counts it as paired. Returns 0 on success. */
int os_peer_ping(os_state *s, const char *address, int port, os_ping_info *out, char *err, size_t errlen);

/* Asks a device to pair with us. It shows a request that someone there must
 * approve; the answer arrives later as a call to our server's /approve-confirm.
 * Opens the window in which that confirmation is accepted. */
int os_peer_handshake(os_state *s, const char *address, int port, char *err, size_t errlen);

/* Tells a device we approved its pairing request, once the person at the
 * console has said yes. The caller has already stored the peer. */
int os_peer_approve_confirm(os_state *s, const char *address, int port, char *err, size_t errlen);

/* ------------------------------------------------------------ paired */

/* One signed request. Returns 0 on a 200 reply with the response in *out; any
 * other outcome is -1 with the other device's own explanation in err when it
 * gave one, and the HTTP status (0 for transport failure) in *status. */
int os_peer_call(os_state *s, const os_peer *p, const char *method, const char *target,
                 const void *body, size_t bodylen, size_t max_body, int timeout_ms, os_http_resp *out,
                 int *status, char *err, size_t errlen);

typedef struct {
    os_manifest manifest;
    char branch[64];
} os_remote_manifest;

void os_remote_manifest_free(os_remote_manifest *r);

/* The other device's manifest for a game. name and save_path let it offer the
 * game to its user when it does not track it yet; title_id is the Switch title. */
int os_peer_fetch_manifest(os_state *s, const os_peer *p, const char *game_id, const char *name,
                           const char *save_path, os_remote_manifest *out, char *err, size_t errlen);

/* Fetches blocks [first, first+count) of a file into dst, verifying each
 * against the hashes in f. dst must hold the bytes of those blocks. Any block
 * that does not match its hash fails the call: nothing unverified is returned. */
int os_peer_fetch_blocks(os_state *s, const os_peer *p, const char *game_id, const os_mfile *f, int first,
                         int count, uint8_t *dst, char *err, size_t errlen);

typedef struct {
    char id[128];
    char name[128];
    char save_path[300];
} os_remote_game;

/* The games the other device tracks. *out is malloc'd; free it. */
int os_peer_list_games(os_state *s, const os_peer *p, os_remote_game **out, int *n, char *err, size_t errlen);

/* Finds the other device's own id for a Switch title. Every request about a
 * game should use that id: the device looks games up by the id in the URL, and
 * while it follows aliases for some requests, a report that the games match
 * is only recorded under the id it knows. *found is 0 (it has none — use
 * "switch-<id>" and let it offer the game), 1 (out filled), or 2 (it tracks the
 * title more than once, so which one is the person's choice). */
int os_peer_find_title(os_state *s, const os_peer *p, const char *title_id, os_remote_game *out, int *found,
                       char *err, size_t errlen);

/* Reports an event ("in-sync", "sync-complete", ...) to the other device. */
int os_peer_send_event(os_state *s, const os_peer *p, const char *game_id, const char *event,
                       const char *data_json, char *err, size_t errlen);

/* Tells the device we hold newer content, so it starts a sync that pulls from us. */
int os_peer_trigger_sync(os_state *s, const os_peer *p, const char *game_id, char *err, size_t errlen);

/* Says goodbye to a device we are unpairing. Best effort. */
int os_peer_unpair(os_state *s, const os_peer *p, char *err, size_t errlen);

/* Percent-encodes s for a query value. */
void os_url_escape(char *out, size_t outlen, const char *in);

/* If our clock and the other device's disagree enough to break request
 * signing, returns a sentence saying so; otherwise "". */
const char *os_clock_hint(const os_state *s, char *buf, size_t buflen);

#endif
