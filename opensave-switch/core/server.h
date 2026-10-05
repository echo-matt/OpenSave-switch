/* The server half of the LAN protocol: the routes another OpenSave device
 * calls on this one.
 *
 * Everything that reads or writes a save requires a request signed with the key
 * from pairing; only ping and the two pairing messages are open, as on every
 * other OpenSave device.
 */
#ifndef OPENSAVE_SERVER_H
#define OPENSAVE_SERVER_H

#include <stddef.h>

#include "http.h"
#include "state.h"

typedef struct {
    /* Finds the folder holding a title's save, making it readable (on the
     * console, mounting it). Returns 0 and fills root, or -1 with a reason a
     * person can read. */
    int (*open_save)(void *ctx, const char *title_id, char *root, size_t rootlen, char *err, size_t errlen);

    /* For a title linked to a game on the PC: the folder the PC should pull from
     * (the mirror of its own files plus this Switch's changes), not the save. */
    int (*open_linked)(void *ctx, const os_link *l, char *root, size_t rootlen, char *err, size_t errlen);

    /* Notifications for the interface. All optional. */
    void (*on_pairing_request)(void *ctx, const os_incoming *req);
    void (*on_paired)(void *ctx, const os_peer *peer);
    void (*on_unpaired)(void *ctx, const char *peer_id);
    /* A paired device says it holds newer content for a game. Nothing is
     * written: the person decides whether to take it. */
    void (*on_peer_update)(void *ctx, const os_peer *peer, const char *game_id, const char *title_id);
    void (*on_served)(void *ctx, const char *what);
    void *ctx;
} os_server_hooks;

typedef struct {
    os_state *st;
    os_server_hooks hooks;
    os_http_server *http;
} os_server;

int os_server_start(os_server *s, os_state *st, const os_server_hooks *hooks, int port, char *err, size_t errlen);
/* Serves at most one request, waiting up to timeout_ms for one to arrive.
 * Returns 1 if one was served. */
int os_server_poll(os_server *s, int timeout_ms);
void os_server_stop(os_server *s);
int os_server_port(const os_server *s);

/* Accepting a pairing request somebody sent us: stores the device and tells it
 * we agreed. Returns 0 on success. */
int os_server_approve(os_server *s, const char *peer_id, char *err, size_t errlen);
void os_server_reject(os_server *s, const char *peer_id);

#endif
