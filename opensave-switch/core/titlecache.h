/* A cache of what is known about each installed game: its name and what kind of
 * save it keeps.
 *
 * Finding that out is the slow part of listing games: the system hands over a
 * game's whole control record, icon included (about 147 KB), one game at a time.
 * Names and save types do not change, so they are asked for once and kept on the
 * SD card; after the first launch only games installed since need asking about. */
#ifndef OPENSAVE_TITLECACHE_H
#define OPENSAVE_TITLECACHE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t id;
    char name[0x201];
    int user;   /* keeps a save per user account */
    int device; /* keeps a save for the whole console */
} os_tcache_entry;

typedef struct {
    os_tcache_entry *e;
    int n, cap;
    int dirty;
    char path[256];
} os_tcache;

/* Loads the file; a missing or unreadable one gives an empty cache (it is only
 * a cache). Always returns with a usable cache. */
void os_tcache_init(os_tcache *c, const char *path);
void os_tcache_free(os_tcache *c);

const os_tcache_entry *os_tcache_find(const os_tcache *c, uint64_t id);
/* Adds or replaces. Returns 0 on success. */
int os_tcache_put(os_tcache *c, const os_tcache_entry *e);
/* Forgets every entry whose id is not in ids[0..n): games that were removed. */
void os_tcache_retain(os_tcache *c, const uint64_t *ids, int n);
void os_tcache_clear(os_tcache *c);
/* Writes the file if anything changed. Returns 0 on success (or nothing to do). */
int os_tcache_save(os_tcache *c);

#endif
