/* Moving a save between this device and a paired one.
 *
 * Pulling replaces the Switch's save with the other device's. It is built so a
 * failure — a dropped connection, a corrupt block, a full save, a crash — cannot
 * leave a half-written save:
 *
 *   1. The current save is copied aside and the copy is verified by hash.
 *   2. Everything that has to change is downloaded to a staging folder first,
 *      each block and each file checked against the manifest.
 *   3. Only then is the save touched, and the result is hashed and compared with
 *      what the other device described. If it differs, the backup is put back.
 *
 * On the console the caller commits the save only when this returns success;
 * until then the file system holds the changes uncommitted.
 */
#ifndef OPENSAVE_SYNC_H
#define OPENSAVE_SYNC_H

#include <stddef.h>
#include <stdint.h>

#include "peer.h"

typedef struct {
    void *ctx;
    /* stage: a short phrase; done/total in bytes (total may be 0 when unknown). */
    void (*progress)(void *ctx, const char *stage, int64_t done, int64_t total);
    /* Return nonzero to stop. Checked between blocks, and before anything in
     * the save is touched; once applying has begun it is finished. */
    int (*cancelled)(void *ctx);
} os_progress;

typedef enum {
    OS_CMP_SAME,      /* identical content */
    OS_CMP_DIFFERENT, /* both have a save, and they differ */
    OS_CMP_PC_LACKS,  /* the other device does not have this game (message says why) */
    OS_CMP_ERROR      /* could not tell (message says why) */
} os_cmp_state;

typedef struct {
    os_cmp_state state;
    int only_remote, only_local, differ;
    int64_t remote_latest_ms, local_latest_ms;
    int remote_has_extra_roots;
    char message[240];
} os_cmp_result;

/* Compares this Switch's save with the other device's. This only looks: a game
 * the other device does not track is reported as OS_CMP_PC_LACKS and nothing is
 * created there. With offer set, name and save_path are sent as well, which makes
 * a device that does not track the game list it as offered to its user (to pick
 * a folder for it) — do that only when the person asked for it. */
void os_compare(os_state *s, const os_peer *p, const char *game_id, const char *name, const char *save_path,
                const char *save_root, int offer, os_cmp_result *out);

typedef struct {
    int already_same;
    int files_downloaded, files_deleted;
    int64_t bytes_downloaded;
    int remote_has_extra_roots;
    char manifest_hash[65];
    char backup_path[256]; /* empty when there was nothing to back up */
} os_pull_result;

/* Replaces the save in save_root with the other device's. backup_dir and
 * staging_dir must be outside save_root. Returns 0 on success. On failure the
 * save is as it was (err says what happened, and if a restore itself failed it
 * says where the backup is). */
int os_pull(os_state *s, const os_peer *p, const char *game_id, const char *save_root, const char *backup_dir,
            const char *staging_dir, const os_progress *pr, os_pull_result *res, char *err, size_t errlen);

/* Puts a backup made by os_pull back: the save in save_root becomes exactly
 * what the backup holds. Returns 0 on success. The backup is verified against
 * the result and kept. */
int os_restore_backup(const char *save_root, const char *backup_dir, char *err, size_t errlen);

/* Tells the other device both sides now hold identical content, so it can
 * record that and tell later who changed what. Best effort. */
int os_report_in_sync(os_state *s, const os_peer *p, const char *game_id, const char *manifest_hash, char *err,
                      size_t errlen);

#endif
