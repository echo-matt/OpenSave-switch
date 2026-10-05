/* This device's identity and the devices it is paired with, kept in one small
 * JSON file. */
#ifndef OPENSAVE_STATE_H
#define OPENSAVE_STATE_H

#include <stddef.h>
#include <stdint.h>

#include "crypto.h"

#define OS_MAX_PEERS 8
#define OS_MAX_PENDING 4
#define OS_NONCE_CACHE 256
#define OS_DEFAULT_PORT 8383

typedef struct {
    char id[64];
    char name[64];
    char address[48]; /* dotted IPv4 */
    int port;
    uint8_t pubkey[32];
    uint8_t authkey[32]; /* derived, not stored */
    int in_use;
} os_peer;

/* A handshake we sent and are waiting on the other side to approve. */
typedef struct {
    char address[48];
    int port;
    int64_t at_ms;
    int in_use;
} os_sent;

/* A handshake another device sent us, waiting on the person at the console. */
typedef struct {
    char peer_id[64];
    char name[64];
    char address[48];
    int port;
    uint8_t pubkey[32];
    int has_key;
    int64_t at_ms;
    int in_use;
} os_incoming;

/* A Switch game whose save lives on the PC in another format: the PC game
 * (by the PC's own id) that stands in for it. */
typedef struct {
    char title[17];    /* Switch title id, upper case */
    char game_id[128]; /* the PC's id for its game */
    char name[128];    /* the PC game's name, for display */
    int in_use;
} os_link;

#define OS_MAX_LINKS 8

typedef struct {
    char node_id[64];
    char device_name[64];
    int port;
    uint8_t priv[32];
    uint8_t pub[32];
    os_peer peers[OS_MAX_PEERS];
    os_link links[OS_MAX_LINKS];
    int convert_consent; /* the person agreed to send saves to the conversion service */
    char convert_url[200];

    /* Not persisted. */
    os_sent sent[OS_MAX_PENDING];
    os_incoming incoming[OS_MAX_PENDING];
    struct {
        char nonce[40];
        int64_t expires_ms;
    } seen[OS_NONCE_CACHE];
    int seen_next;
    int64_t clock_skew_s; /* peer's clock minus ours, from the last reply that carried a Date */
    int skew_known;
    char path[256];
} os_state;

/* Loads the state file, creating it — with a fresh identity — if it does not
 * exist. Returns 0 on success. */
int os_state_load(os_state *s, const char *path, char *err, size_t errlen);
int os_state_save(const os_state *s, char *err, size_t errlen);

os_peer *os_state_find_peer(os_state *s, const char *id);
/* Adds or replaces a paired device and derives its authentication key. */
os_peer *os_state_add_peer(os_state *s, const char *id, const char *name, const char *address, int port,
                           const uint8_t pubkey[32]);
int os_state_remove_peer(os_state *s, const char *id);

os_link *os_state_find_link(os_state *s, const char *title);
os_link *os_state_find_link_by_game(os_state *s, const char *game_id);
/* Adds or replaces the link for a title. Returns NULL if the list is full. */
os_link *os_state_set_link(os_state *s, const char *title, const char *game_id, const char *name);
void os_state_remove_link(os_state *s, const char *title);

/* Records a nonce; returns 0 if it was already seen inside its window (a replay). */
int os_state_remember_nonce(os_state *s, const char *nonce, int64_t now_ms);

#endif
