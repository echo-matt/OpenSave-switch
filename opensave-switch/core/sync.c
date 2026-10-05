#include "sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crypto.h"
#include "fsutil.h"
#include "json.h"

static void seterr(char *err, size_t n, const char *fmt, const char *a) {
    if (err && n) snprintf(err, n, fmt, a ? a : "");
}

static void tick(const os_progress *pr, const char *stage, int64_t done, int64_t total) {
    if (pr && pr->progress) pr->progress(pr->ctx, stage, done, total);
}

static int stopped(const os_progress *pr) { return pr && pr->cancelled && pr->cancelled(pr->ctx); }

/* ------------------------------------------------------------------ compare */

void os_compare(os_state *s, const os_peer *p, const char *game_id, const char *name, const char *save_path,
                const char *save_root, int offer, os_cmp_result *out) {
    os_remote_manifest rm;
    os_manifest lm;
    char err[160], h1[65], h2[65];
    int missing, i;

    memset(out, 0, sizeof *out);
    if (os_peer_fetch_manifest(s, p, game_id, offer ? name : NULL, offer ? save_path : NULL, &rm, err, sizeof err) != 0) {
        /* The other device's own words say whether it simply does not have the
         * game or something is wrong; either way the person is told. */
        out->state = (strstr(err, "not found") || strstr(err, "folder for this game")) ? OS_CMP_PC_LACKS : OS_CMP_ERROR;
        snprintf(out->message, sizeof out->message, "%s", err);
        return;
    }
    out->remote_has_extra_roots = rm.manifest.has_extra_roots;
    out->remote_latest_ms = rm.manifest.latest_mtime;
    if (os_manifest_build(save_root, &lm, &missing, err, sizeof err) != 0) {
        out->state = OS_CMP_ERROR;
        snprintf(out->message, sizeof out->message, "could not read this Switch's save: %s", err);
        os_remote_manifest_free(&rm);
        return;
    }
    out->local_latest_ms = lm.latest_mtime;
    for (i = 0; i < rm.manifest.nfiles; i++) {
        const os_mfile *r = &rm.manifest.files[i], *l = os_manifest_find(&lm, r->path);
        if (!l) out->only_remote++;
        else if (memcmp(l->hash, r->hash, 32) != 0) out->differ++;
    }
    for (i = 0; i < lm.nfiles; i++)
        if (!os_manifest_find(&rm.manifest, lm.files[i].path)) out->only_local++;
    os_manifest_hash(&rm.manifest, h1);
    os_manifest_hash(&lm, h2);
    out->state = strcmp(h1, h2) == 0 ? OS_CMP_SAME : OS_CMP_DIFFERENT;
    os_manifest_free(&lm);
    os_remote_manifest_free(&rm);
}

/* --------------------------------------------------------------------- pull */

static int is_under(const char *path, const char *dir) {
    size_t n = strlen(dir);
    return strncmp(path, dir, n) == 0 && path[n] == '/';
}

/* Whether a local folder is still wanted: it is in the other device's list, or
 * holds something that is. */
static int dir_wanted(const os_manifest *remote, const char *dir) {
    int i;
    for (i = 0; i < remote->ndirs; i++)
        if (strcmp(remote->dirs[i], dir) == 0 || is_under(remote->dirs[i], dir)) return 1;
    for (i = 0; i < remote->nfiles; i++)
        if (is_under(remote->files[i].path, dir)) return 1;
    return 0;
}

static int same_content(const os_manifest *a, const os_manifest *b) {
    char h1[65], h2[65];
    os_manifest_hash(a, h1);
    os_manifest_hash(b, h2);
    return strcmp(h1, h2) == 0;
}

/* Puts the backup back: the save is emptied and the copy restored. */
static int restore(const char *save_root, const char *backup_dir, int had_backup) {
    if (os_clear_dir(save_root) != 0) return -1;
    if (had_backup && os_copy_tree(backup_dir, save_root) != 0) return -1;
    return 0;
}

/* Downloads one file into staging, verifying each block as it arrives and the
 * whole file at the end. */
static int fetch_file(os_state *s, const os_peer *p, const char *game_id, const os_mfile *f, const char *staging_dir,
                      const os_progress *pr, int64_t *done, int64_t total, char *err, size_t errlen) {
    char dest[1024];
    FILE *out;
    uint8_t *buf;
    os_sha256_ctx whole;
    uint8_t sum[32];
    int per_batch = OS_BLOCK_BATCH_BYTES / f->block_size, b = 0;
    int rc = 0;

    if (per_batch < 1) per_batch = 1;
    os_path_join(dest, sizeof dest, staging_dir, f->path);
    if (os_mkdir_parent(dest) != 0) {
        seterr(err, errlen, "could not create a staging folder for %s", f->path);
        return -1;
    }
    out = fopen(dest, "wb");
    if (!out) {
        seterr(err, errlen, "could not create the staging file for %s", f->path);
        return -1;
    }
    buf = (uint8_t *)malloc((size_t)per_batch * (size_t)f->block_size);
    if (!buf) {
        fclose(out);
        seterr(err, errlen, "out of memory", NULL);
        return -1;
    }
    os_sha256_init(&whole);
    while (b < f->nblocks) {
        int n = f->nblocks - b < per_batch ? f->nblocks - b : per_batch;
        size_t bytes = 0;
        int k;
        if (stopped(pr)) {
            seterr(err, errlen, "cancelled", NULL);
            rc = -1;
            break;
        }
        if (os_peer_fetch_blocks(s, p, game_id, f, b, n, buf, err, errlen) != 0) {
            rc = -1;
            break;
        }
        for (k = 0; k < n; k++) bytes += (size_t)f->blocks[b + k].length;
        os_sha256_update(&whole, buf, bytes);
        if (fwrite(buf, 1, bytes, out) != bytes) {
            seterr(err, errlen, "could not write the staging file (is the SD card full?)", NULL);
            rc = -1;
            break;
        }
        *done += (int64_t)bytes;
        tick(pr, f->path, *done, total);
        b += n;
    }
    free(buf);
    if (fclose(out) != 0 && rc == 0) {
        seterr(err, errlen, "could not finish writing the staging file", NULL);
        rc = -1;
    }
    if (rc != 0) return rc;
    os_sha256_final(&whole, sum);
    if (memcmp(sum, f->hash, 32) != 0) {
        seterr(err, errlen, "%s does not match its hash after download", f->path);
        return -1;
    }
    if (os_file_size(dest) != f->size) {
        seterr(err, errlen, "%s has the wrong size after download", f->path);
        return -1;
    }
    return 0;
}

int os_pull(os_state *s, const os_peer *p, const char *game_id, const char *save_root, const char *backup_dir,
            const char *staging_dir, const os_progress *pr, os_pull_result *res, char *err, size_t errlen) {
    os_remote_manifest rm;
    os_manifest lm, after;
    int missing, i, rc = -1, had_backup = 0, applying = 0;
    int *want = NULL;      /* index into remote files to download */
    int nwant = 0;
    int64_t total = 0, done = 0;
    char path[1400], staged[1400], local_err[240];

    memset(res, 0, sizeof *res);
    memset(&lm, 0, sizeof lm);
    memset(&after, 0, sizeof after);
    if (err && errlen) err[0] = '\0';

    tick(pr, "Asking the PC what it has", 0, 0);
    if (os_peer_fetch_manifest(s, p, game_id, NULL, NULL, &rm, err, errlen) != 0) return -1;
    res->remote_has_extra_roots = rm.manifest.has_extra_roots;
    os_manifest_hash(&rm.manifest, res->manifest_hash);

    if (os_manifest_build(save_root, &lm, &missing, local_err, sizeof local_err) != 0) {
        seterr(err, errlen, "could not read this Switch's save: %s", local_err);
        goto out;
    }
    if (missing) {
        seterr(err, errlen, "this Switch has no save folder for that game (play it once first)", NULL);
        goto out;
    }
    if (same_content(&lm, &rm.manifest)) {
        res->already_same = 1;
        rc = 0;
        goto out;
    }
    /* An empty copy on the other device is not an instruction to empty this
     * one. The PC holds a game back in the same situation until its owner says
     * the emptying was meant; this side simply declines. */
    if (rm.manifest.nfiles == 0 && lm.nfiles > 0) {
        seterr(err, errlen,
               "The PC's copy of this save has no files. Refusing to replace the Switch's save with nothing.", NULL);
        goto out;
    }

    /* What has to come across. */
    want = rm.manifest.nfiles ? (int *)malloc((size_t)rm.manifest.nfiles * sizeof *want) : NULL;
    if (rm.manifest.nfiles && !want) {
        seterr(err, errlen, "out of memory", NULL);
        goto out;
    }
    for (i = 0; i < rm.manifest.nfiles; i++) {
        const os_mfile *r = &rm.manifest.files[i], *l = os_manifest_find(&lm, r->path);
        if (!l || memcmp(l->hash, r->hash, 32) != 0) {
            want[nwant++] = i;
            total += r->size;
        }
    }

    /* 1. Back up what is there, and prove the copy before relying on it. */
    if (lm.nfiles > 0 || lm.ndirs > 0) {
        os_manifest bm;
        char berr[200];
        int bmissing;
        tick(pr, "Backing up the Switch's save", 0, 0);
        os_rm_rf(backup_dir);
        if (os_copy_tree(save_root, backup_dir) != 0) {
            seterr(err, errlen, "could not back up the save to the SD card, so nothing was changed", NULL);
            goto out;
        }
        if (os_manifest_build(backup_dir, &bm, &bmissing, berr, sizeof berr) != 0 || bmissing ||
            !same_content(&bm, &lm)) {
            os_manifest_free(&bm);
            seterr(err, errlen, "the backup did not verify, so nothing was changed", NULL);
            goto out;
        }
        os_manifest_free(&bm);
        had_backup = 1;
        snprintf(res->backup_path, sizeof res->backup_path, "%s", backup_dir);
    }

    /* 2. Download to staging. The save is not touched yet. */
    os_rm_rf(staging_dir);
    for (i = 0; i < nwant; i++) {
        const os_mfile *r = &rm.manifest.files[want[i]];
        if (fetch_file(s, p, game_id, r, staging_dir, pr, &done, total, err, errlen) != 0) goto out;
        res->files_downloaded++;
    }
    res->bytes_downloaded = done;
    if (stopped(pr)) {
        seterr(err, errlen, "cancelled", NULL);
        goto out;
    }

    /* 3. Apply. From here a failure is answered by putting the backup back. */
    tick(pr, "Writing the save", 0, 0);
    applying = 1;
    for (i = 0; i < lm.nfiles; i++) {
        if (os_manifest_find(&rm.manifest, lm.files[i].path)) continue;
        os_path_join(path, sizeof path, save_root, lm.files[i].path);
        if (remove(path) != 0) {
            seterr(err, errlen, "could not remove %s", lm.files[i].path);
            goto out;
        }
        res->files_deleted++;
    }
    for (i = lm.ndirs - 1; i >= 0; i--) {
        if (dir_wanted(&rm.manifest, lm.dirs[i])) continue;
        os_path_join(path, sizeof path, save_root, lm.dirs[i]);
        if (os_rm_rf(path) != 0) {
            seterr(err, errlen, "could not remove the folder %s", lm.dirs[i]);
            goto out;
        }
    }
    for (i = 0; i < rm.manifest.ndirs; i++) {
        os_path_join(path, sizeof path, save_root, rm.manifest.dirs[i]);
        if (os_mkdir_p(path) != 0) {
            seterr(err, errlen, "could not create the folder %s", rm.manifest.dirs[i]);
            goto out;
        }
    }
    for (i = 0; i < nwant; i++) {
        const os_mfile *r = &rm.manifest.files[want[i]];
        os_path_join(path, sizeof path, save_root, r->path);
        os_path_join(staged, sizeof staged, staging_dir, r->path);
        if (os_mkdir_parent(path) != 0 || os_copy_file(staged, path) != 0) {
            seterr(err, errlen, "could not write %s (is the save full?)", r->path);
            goto out;
        }
    }

    /* 4. Prove the result is exactly what the other device described. */
    tick(pr, "Checking the result", 0, 0);
    if (os_manifest_build(save_root, &after, &missing, local_err, sizeof local_err) != 0 || missing ||
        !same_content(&after, &rm.manifest)) {
        seterr(err, errlen, "the save did not match the PC's after writing", NULL);
        goto out;
    }
    rc = 0;

out:
    if (rc != 0 && applying) {
        if (restore(save_root, backup_dir, had_backup) != 0) {
            char msg[400];
            snprintf(msg, sizeof msg,
                     "%s — AND the automatic restore failed. Your original save is safe at %s on the SD card.",
                     err && err[0] ? err : "the pull failed", had_backup ? backup_dir : "(nothing was backed up: the save was empty)");
            if (err && errlen) snprintf(err, errlen, "%s", msg);
        } else if (err && errlen && err[0]) {
            char msg[400];
            snprintf(msg, sizeof msg, "%s The save was put back as it was.", err);
            snprintf(err, errlen, "%s", msg);
        }
    }
    os_rm_rf(staging_dir);
    free(want);
    os_manifest_free(&lm); /* safe on a struct that was never filled */
    os_manifest_free(&after);
    os_remote_manifest_free(&rm);
    return rc;
}

int os_restore_backup(const char *save_root, const char *backup_dir, char *err, size_t errlen) {
    os_manifest want, got;
    char local_err[160];
    int missing, rc = -1;

    if (os_manifest_build(backup_dir, &want, &missing, local_err, sizeof local_err) != 0 || missing) {
        if (!missing) os_manifest_free(&want);
        seterr(err, errlen, "that backup cannot be read", NULL);
        return -1;
    }
    if (restore(save_root, backup_dir, 1) != 0) {
        seterr(err, errlen, "could not restore the backup (is the save full or in use?)", NULL);
        goto out;
    }
    if (os_manifest_build(save_root, &got, &missing, local_err, sizeof local_err) != 0 || missing ||
        !same_content(&got, &want)) {
        os_manifest_free(&got);
        seterr(err, errlen, "the restored save does not match the backup", NULL);
        goto out;
    }
    os_manifest_free(&got);
    rc = 0;
out:
    os_manifest_free(&want);
    return rc;
}

int os_report_in_sync(os_state *s, const os_peer *p, const char *game_id, const char *manifest_hash, char *err,
                      size_t errlen) {
    char data[200];
    snprintf(data, sizeof data, "{\"manifestHash\":\"%s\"}", manifest_hash);
    return os_peer_send_event(s, p, game_id, "in-sync", data, err, errlen);
}
