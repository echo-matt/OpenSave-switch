#include "mcd.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "aes.h"
#include "crypto.h"
#include "fsutil.h"
#include "json.h"
#include "manifest.h"
#include "mcd_key.h"

#define MAX_SAVE_BYTES (3 * 1000 * 1000) /* far above any character */
#define MAX_CHARS 256
#define SW_PREFIX "Character"

static const uint8_t MAGIC[8] = {'D', '0', '0', '1', 0, 0, 0, 0};

static void seterr(char *err, size_t n, const char *msg) {
    if (err && n) snprintf(err, n, "%s", msg);
}

int os_mcd_supported(const char *title_id) { return title_id && strcasecmp(title_id, OS_MCD_TITLE) == 0; }

int os_mcd_is_dat(const uint8_t *b, size_t n) { return n > sizeof MAGIC && memcmp(b, MAGIC, sizeof MAGIC) == 0; }

int os_mcd_is_character(const uint8_t *json, size_t n) {
    os_json *d = os_json_parse((const char *)json, n, NULL, 0);
    int ok = 0;
    if (d) {
        os_jn root = os_json_root(d);
        ok = os_json_type(d, root) == J_OBJ &&
             (os_json_get(d, root, "uniqueSaveId") != OS_JN_NONE || os_json_get(d, root, "playerId") != OS_JN_NONE);
        os_json_free(d);
    }
    return ok;
}

/* ------------------------------------------------------------------ cipher */

int os_mcd_decrypt_dat(const uint8_t *dat, size_t n, uint8_t **json, size_t *jn, char *err, size_t errlen) {
    os_aes256 a;
    uint8_t *out;
    size_t body = n - sizeof MAGIC, i;
    if (!os_mcd_is_dat(dat, n)) {
        seterr(err, errlen, "that is not an encrypted Minecraft Dungeons save");
        return -1;
    }
    if (body % 16 != 0) {
        seterr(err, errlen, "that save is damaged (its size is not a whole number of cipher blocks)");
        return -1;
    }
    out = (uint8_t *)malloc(body + 1);
    if (!out) {
        seterr(err, errlen, "out of memory");
        return -1;
    }
    os_aes256_init(&a, OS_MCD_KEY);
    for (i = 0; i < body; i += 16) os_aes256_decrypt_block(&a, dat + sizeof MAGIC + i, out + i);
    /* The cipher pads with zeros, which are not part of the JSON. */
    while (body && out[body - 1] == 0) body--;
    out[body] = '\0';
    if (!os_mcd_is_character(out, body)) {
        free(out);
        return 1;
    }
    *json = out;
    *jn = body;
    return 0;
}

int os_mcd_encrypt_json(const uint8_t *json, size_t n, uint8_t **dat, size_t *dn, char *err, size_t errlen) {
    os_aes256 a;
    uint8_t *out, *back = NULL;
    size_t padded = (n + 15) / 16 * 16, i, bn = 0;
    int rc;
    if (!os_mcd_is_character(json, n)) {
        seterr(err, errlen, "that file is not a character save");
        return -1;
    }
    if (memchr(json, 0, n)) {
        /* Zero is the padding, so a zero inside the data could not be told from it. */
        seterr(err, errlen, "that character contains a zero byte and cannot be encrypted unambiguously");
        return -1;
    }
    out = (uint8_t *)calloc(1, sizeof MAGIC + padded);
    if (!out) {
        seterr(err, errlen, "out of memory");
        return -1;
    }
    memcpy(out, MAGIC, sizeof MAGIC);
    {
        uint8_t *plain = (uint8_t *)calloc(1, padded);
        if (!plain) {
            free(out);
            seterr(err, errlen, "out of memory");
            return -1;
        }
        memcpy(plain, json, n);
        os_aes256_init(&a, OS_MCD_KEY);
        for (i = 0; i < padded; i += 16) os_aes256_encrypt_block(&a, plain + i, out + sizeof MAGIC + i);
        free(plain);
    }
    /* Prove it reads back as what went in, as the game will have to. */
    rc = os_mcd_decrypt_dat(out, sizeof MAGIC + padded, &back, &bn, err, errlen);
    if (rc != 0 || bn != n || memcmp(back, json, n) != 0) {
        free(out);
        free(back);
        seterr(err, errlen, "the encrypted save did not decrypt back to the original, so it was not used");
        return -1;
    }
    free(back);
    *dat = out;
    *dn = sizeof MAGIC + padded;
    return 0;
}

/* ------------------------------------------------------------- bookkeeping */

void os_mcd_mirror_path(const char *convert_dir, char *out, size_t outlen) {
    os_path_join(out, outlen, convert_dir, "mirror");
}

/* What the Switch's characters held when they last matched the PC's. */
typedef struct {
    char guid[80];
    char hash[65];
    char rel[OS_MAX_PATH_LEN + 1]; /* where the PC keeps this character */
} cstate_entry;

typedef struct {
    cstate_entry e[MAX_CHARS];
    int n;
} cstate;

static cstate_entry *cs_find(cstate *c, const char *guid) {
    int i;
    for (i = 0; i < c->n; i++)
        if (strcmp(c->e[i].guid, guid) == 0) return &c->e[i];
    return NULL;
}

static cstate_entry *cs_put(cstate *c, const char *guid) {
    cstate_entry *e = cs_find(c, guid);
    if (e) return e;
    if (c->n >= MAX_CHARS) return NULL;
    e = &c->e[c->n++];
    memset(e, 0, sizeof *e);
    snprintf(e->guid, sizeof e->guid, "%s", guid);
    return e;
}

static int read_file(const char *path, uint8_t **out, size_t *n, size_t max) {
    FILE *f = fopen(path, "rb");
    long len;
    uint8_t *b;
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0 || (size_t)len > max) {
        fclose(f);
        return -1;
    }
    b = (uint8_t *)malloc((size_t)len + 1);
    if (!b || fread(b, 1, (size_t)len, f) != (size_t)len) {
        free(b);
        fclose(f);
        return -1;
    }
    fclose(f);
    b[len] = 0;
    *out = b;
    *n = (size_t)len;
    return 0;
}

static int write_file(const char *path, const uint8_t *b, size_t n) {
    FILE *f;
    if (os_mkdir_parent(path) != 0) return -1;
    f = fopen(path, "wb");
    if (!f) return -1;
    if (n && fwrite(b, 1, n, f) != n) {
        fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

static void hash_hex(const uint8_t *b, size_t n, char out[65]) {
    uint8_t h[32];
    os_sha256(b, n, h);
    os_hex_encode(out, h, 32);
}

static void cs_path(const char *convert_dir, char *out, size_t n) { os_path_join(out, n, convert_dir, "state.json"); }

static void cs_load(const char *convert_dir, cstate *c) {
    char path[600];
    uint8_t *b;
    size_t n;
    os_json *d;
    os_jn it;
    memset(c, 0, sizeof *c);
    cs_path(convert_dir, path, sizeof path);
    if (read_file(path, &b, &n, 1000000) != 0) return;
    d = os_json_parse((const char *)b, n, NULL, 0);
    free(b);
    if (!d) return; /* damaged: treated as never synced, which only costs a re-check */
    for (it = os_json_first(d, os_json_get(d, os_json_root(d), "characters")); it >= 0; it = os_json_next(d, it)) {
        const char *g = os_json_get_str(d, it, "id"), *h = os_json_get_str(d, it, "hash"),
                   *r = os_json_get_str(d, it, "path");
        cstate_entry *e;
        if (!g || !h || !r || strlen(h) != 64 || strlen(g) >= sizeof e->guid || strlen(r) > OS_MAX_PATH_LEN) continue;
        e = cs_put(c, g);
        if (!e) break;
        snprintf(e->hash, sizeof e->hash, "%s", h);
        snprintf(e->rel, sizeof e->rel, "%s", r);
    }
    os_json_free(d);
}

static int cs_save(const char *convert_dir, const cstate *c) {
    os_sb sb;
    char path[600], tmp[620];
    char *text;
    size_t n;
    int i, rc;
    os_sb_init(&sb);
    os_sb_puts(&sb, "{\"characters\":[");
    for (i = 0; i < c->n; i++) {
        os_sb_printf(&sb, "%s{\"id\":", i ? "," : "");
        os_sb_json_str(&sb, c->e[i].guid);
        os_sb_printf(&sb, ",\"hash\":\"%s\",\"path\":", c->e[i].hash);
        os_sb_json_str(&sb, c->e[i].rel);
        os_sb_putc(&sb, '}');
    }
    os_sb_puts(&sb, "]}");
    text = os_sb_take(&sb, &n);
    if (!text) return -1;
    cs_path(convert_dir, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    rc = write_file(tmp, (const uint8_t *)text, n);
    free(text);
    if (rc != 0) return -1;
    if (rename(tmp, path) != 0) {
        remove(path);
        if (rename(tmp, path) != 0) return -1;
    }
    return 0;
}

/* The id in a Windows file's path ("Saves/x/<id>.dat" -> "<id>"), if it is one
 * that can safely name a Switch file. */
static int guid_of(const char *rel, char *guid, size_t gl) {
    const char *base = strrchr(rel, '/'), *dot;
    size_t n, i;
    base = base ? base + 1 : rel;
    dot = strrchr(base, '.');
    if (!dot || strcasecmp(dot, ".dat") != 0) return 0;
    n = (size_t)(dot - base);
    if (n < 8 || n >= gl) return 0;
    for (i = 0; i < n; i++)
        if (!isalnum((unsigned char)base[i])) return 0;
    memcpy(guid, base, n);
    guid[n] = '\0';
    return 1;
}

static void dir_of(const char *rel, char *out, size_t n) {
    const char *slash = strrchr(rel, '/');
    if (!slash) out[0] = '\0';
    else {
        size_t l = (size_t)(slash - rel);
        if (l >= n) l = n - 1;
        memcpy(out, rel, l);
        out[l] = '\0';
    }
}

/* Switch characters in the save: "Character<id>" files at its top level. */
typedef struct {
    char guid[80];
    char path[600];
} swchar;

static int list_switch_chars(const char *save_root, swchar *out, int max) {
    os_manifest m;
    int missing, i, n = 0;
    char err[80];
    if (os_manifest_build(save_root, &m, &missing, err, sizeof err) != 0) return -1;
    for (i = 0; i < m.nfiles && n < max; i++) {
        const char *name = m.files[i].path;
        if (strchr(name, '/') || strncmp(name, SW_PREFIX, strlen(SW_PREFIX)) != 0 || strchr(name, '.')) continue;
        if (strlen(name) - strlen(SW_PREFIX) >= sizeof out[n].guid || strlen(name) == strlen(SW_PREFIX)) continue;
        snprintf(out[n].guid, sizeof out[n].guid, "%s", name + strlen(SW_PREFIX));
        os_path_join(out[n].path, sizeof out[n].path, save_root, name);
        n++;
    }
    os_manifest_free(&m);
    return n;
}

/* ----------------------------------------------------------------- compare */

void os_mcd_compare(os_state *s, const os_peer *p, const os_link *l, const char *save_root, const char *convert_dir,
                    os_cmp_result *out) {
    os_remote_manifest rm;
    os_manifest mm;
    char err[160], mirror[600], h1[65], h2[65];
    cstate cs;
    swchar *chars;
    int i, missing, nchars;

    memset(out, 0, sizeof *out);
    /* No name or path: looking must not make the PC list anything. */
    if (os_peer_fetch_manifest(s, p, l->game_id, NULL, NULL, &rm, err, sizeof err) != 0) {
        out->state = (strstr(err, "not found")) ? OS_CMP_PC_LACKS : OS_CMP_ERROR;
        snprintf(out->message, sizeof out->message, "%s", err);
        return;
    }
    out->remote_latest_ms = rm.manifest.latest_mtime;
    os_mcd_mirror_path(convert_dir, mirror, sizeof mirror);
    if (os_manifest_build(mirror, &mm, &missing, err, sizeof err) != 0) {
        out->state = OS_CMP_ERROR;
        snprintf(out->message, sizeof out->message, "could not read the local copy: %s", err);
        os_remote_manifest_free(&rm);
        return;
    }
    for (i = 0; i < rm.manifest.nfiles; i++) {
        const os_mfile *r = &rm.manifest.files[i], *m = os_manifest_find(&mm, r->path);
        if (!m) out->only_remote++;
        else if (memcmp(m->hash, r->hash, 32) != 0) out->differ++;
    }
    for (i = 0; i < mm.nfiles; i++)
        if (!os_manifest_find(&rm.manifest, mm.files[i].path)) out->only_local++;
    os_manifest_hash(&rm.manifest, h1);
    os_manifest_hash(&mm, h2);

    /* Edits made on the Switch since the last receive or send. */
    cs_load(convert_dir, &cs);
    chars = (swchar *)calloc(MAX_CHARS, sizeof *chars);
    nchars = chars ? list_switch_chars(save_root, chars, MAX_CHARS) : -1;
    for (i = 0; i < nchars; i++) {
        uint8_t *b;
        size_t n;
        char h[65];
        cstate_entry *e;
        if (read_file(chars[i].path, &b, &n, MAX_SAVE_BYTES) != 0) continue;
        hash_hex(b, n, h);
        free(b);
        e = cs_find(&cs, chars[i].guid);
        if (!e) out->only_local++;
        else if (strcmp(e->hash, h) != 0) out->differ++;
    }
    free(chars);
    out->state = (out->only_remote || out->only_local || out->differ || strcmp(h1, h2) != 0) ? OS_CMP_DIFFERENT : OS_CMP_SAME;
    os_manifest_free(&mm);
    os_remote_manifest_free(&rm);
}

/* -------------------------------------------------------------------- pull */

typedef struct {
    char guid[80];
    char rel[OS_MAX_PATH_LEN + 1];
    uint8_t *json;
    size_t jn;
} conv;

static void free_convs(conv *c, int n) {
    int i;
    for (i = 0; i < n; i++) free(c[i].json);
    free(c);
}

int os_mcd_pull(os_state *s, const os_peer *p, const os_link *l, const char *save_root, const char *convert_dir,
                const char *backup_dir, const os_progress *pr, os_mcd_result *res, char *err, size_t errlen) {
    os_remote_manifest rm;
    os_manifest mm, lm, after;
    cstate cs;
    char mirror[600], staging[600], path[1400], local_err[200], h1[65];
    int *want = NULL, nwant = 0, i, j, rc = -1, missing, had_backup = 0, applying = 0, nconv = 0;
    conv *convs = NULL;
    int64_t total = 0, done = 0;

    memset(res, 0, sizeof *res);
    memset(&mm, 0, sizeof mm);
    memset(&lm, 0, sizeof lm);
    memset(&after, 0, sizeof after);
    if (err && errlen) err[0] = '\0';
    os_mcd_mirror_path(convert_dir, mirror, sizeof mirror);
    os_path_join(staging, sizeof staging, convert_dir, "staging");

    if (os_peer_fetch_manifest(s, p, l->game_id, NULL, NULL, &rm, err, errlen) != 0) return -1;
    os_manifest_hash(&rm.manifest, res->manifest_hash);
    if (os_manifest_build(mirror, &mm, &missing, local_err, sizeof local_err) != 0) {
        snprintf(err, errlen, "could not read the local copy of the PC's files: %s", local_err);
        goto out;
    }
    if (os_manifest_build(save_root, &lm, &missing, local_err, sizeof local_err) != 0 || missing) {
        snprintf(err, errlen, "could not read this Switch's save: %s", missing ? "no save folder (play the game once first)" : local_err);
        goto out;
    }
    cs_load(convert_dir, &cs);

    /* What has to come across: the PC's files that are new or changed. */
    want = rm.manifest.nfiles ? (int *)malloc((size_t)rm.manifest.nfiles * sizeof *want) : NULL;
    if (rm.manifest.nfiles && !want) {
        seterr(err, errlen, "out of memory");
        goto out;
    }
    for (i = 0; i < rm.manifest.nfiles; i++) {
        const os_mfile *r = &rm.manifest.files[i], *m = os_manifest_find(&mm, r->path);
        if (!m || memcmp(m->hash, r->hash, 32) != 0) {
            want[nwant++] = i;
            total += r->size;
        }
    }
    os_rm_rf(staging);
    if (os_fetch_files(s, p, l->game_id, &rm.manifest, want, nwant, staging, pr, total, &done, err, errlen) != 0) goto out;
    res->files = nwant;
    res->bytes = done;

    /* Work out every Switch file to write, decrypting first, so that
     * a failure leaves nothing touched. */
    convs = (conv *)calloc((size_t)(rm.manifest.nfiles ? rm.manifest.nfiles : 1), sizeof *convs);
    if (!convs) {
        seterr(err, errlen, "out of memory");
        goto out;
    }
    for (i = 0; i < rm.manifest.nfiles; i++) {
        const os_mfile *r = &rm.manifest.files[i];
        char guid[80], src[1400], swpath[1400], cur[65];
        uint8_t *dat = NULL, *json = NULL;
        size_t dn = 0, jn = 0;
        int changed = 0, rcd;
        cstate_entry *e;
        uint8_t *sw = NULL;
        size_t swn = 0;

        if (!guid_of(r->path, guid, sizeof guid)) continue;
        for (j = 0; j < nwant; j++)
            if (want[j] == i) changed = 1;
        /* An unchanged file whose Switch copy still matches what was written last
         * time needs nothing: nothing to encrypt again. */
        {
            char name[200];
            snprintf(name, sizeof name, "%s%s", SW_PREFIX, guid);
            os_path_join(swpath, sizeof swpath, save_root, name);
        }
        e = cs_find(&cs, guid);
        if (!changed && e && read_file(swpath, &sw, &swn, MAX_SAVE_BYTES) == 0) {
            hash_hex(sw, swn, cur);
            free(sw);
            if (strcmp(cur, e->hash) == 0) continue;
        }
        os_path_join(src, sizeof src, changed ? staging : mirror, r->path);
        if (read_file(src, &dat, &dn, MAX_SAVE_BYTES) != 0) {
            snprintf(err, errlen, "could not read %s", r->path);
            goto out;
        }
        if (!os_mcd_is_dat(dat, dn)) {
            free(dat);
            res->skipped++;
            continue;
        }
        if (pr && pr->progress) pr->progress(pr->ctx, "Converting a character", 0, 0);
        rcd = os_mcd_decrypt_dat(dat, dn, &json, &jn, NULL, 0);
        free(dat);
        if (rcd != 0) { /* damaged, or not a character: left alone, and the rest still convert */
            res->skipped++;
            continue;
        }
        snprintf(convs[nconv].guid, sizeof convs[nconv].guid, "%s", guid);
        snprintf(convs[nconv].rel, sizeof convs[nconv].rel, "%s", r->path);
        convs[nconv].json = json;
        convs[nconv].jn = jn;
        nconv++;
    }
    if (pr && pr->cancelled && pr->cancelled(pr->ctx)) {
        seterr(err, errlen, "cancelled");
        goto out;
    }

    {   /* Anything to do at all? */
        int removed = 0;
        for (i = 0; i < mm.nfiles; i++)
            if (!os_manifest_find(&rm.manifest, mm.files[i].path)) removed++;
        if (nwant == 0 && nconv == 0 && removed == 0) {
            res->already_same = 1;
            rc = 0;
            goto out;
        }
    }

    /* Back up the Switch save and prove the copy. */
    if (lm.nfiles > 0 || lm.ndirs > 0) {
        os_manifest bm;
        int bmissing;
        char berr[160], hb[65], hl[65];
        if (pr && pr->progress) pr->progress(pr->ctx, "Backing up the Switch's save", 0, 0);
        os_rm_rf(backup_dir);
        if (os_copy_tree(save_root, backup_dir) != 0) {
            seterr(err, errlen, "could not back up the save to the SD card, so nothing was changed");
            goto out;
        }
        if (os_manifest_build(backup_dir, &bm, &bmissing, berr, sizeof berr) != 0 || bmissing) {
            os_manifest_free(&bm);
            seterr(err, errlen, "the backup did not verify, so nothing was changed");
            goto out;
        }
        os_manifest_hash(&bm, hb);
        os_manifest_hash(&lm, hl);
        os_manifest_free(&bm);
        if (strcmp(hb, hl) != 0) {
            seterr(err, errlen, "the backup did not verify, so nothing was changed");
            goto out;
        }
        had_backup = 1;
        snprintf(res->backup_path, sizeof res->backup_path, "%s", backup_dir);
    }

    /* Write the Switch's characters. Only these files are touched. */
    applying = 1;
    for (i = 0; i < nconv; i++) {
        char name[200];
        uint8_t *chk = NULL;
        size_t cn = 0;
        snprintf(name, sizeof name, "%s%s", SW_PREFIX, convs[i].guid);
        os_path_join(path, sizeof path, save_root, name);
        if (write_file(path, convs[i].json, convs[i].jn) != 0) {
            snprintf(err, errlen, "could not write %s (is the save full?)", name);
            goto out;
        }
        if (read_file(path, &chk, &cn, MAX_SAVE_BYTES) != 0 || cn != convs[i].jn || memcmp(chk, convs[i].json, cn) != 0) {
            free(chk);
            snprintf(err, errlen, "%s did not read back correctly after writing", name);
            goto out;
        }
        free(chk);
        res->converted++;
    }

    /* The Switch save is done; now the mirror becomes exactly the PC's files. */
    for (i = 0; i < mm.nfiles; i++)
        if (!os_manifest_find(&rm.manifest, mm.files[i].path)) {
            os_path_join(path, sizeof path, mirror, mm.files[i].path);
            remove(path);
        }
    for (j = 0; j < nwant; j++) {
        char src[1400], dst[1400];
        os_path_join(src, sizeof src, staging, rm.manifest.files[want[j]].path);
        os_path_join(dst, sizeof dst, mirror, rm.manifest.files[want[j]].path);
        if (os_mkdir_parent(dst) != 0 || os_copy_file(src, dst) != 0) {
            os_rm_rf(mirror); /* an incomplete mirror must not be served to the PC */
            seterr(err, errlen, "the characters were converted but the local copy of the PC's files could not be updated; receive again");
            goto out;
        }
    }
    if (os_manifest_build(mirror, &after, &missing, local_err, sizeof local_err) != 0 || missing) {
        os_rm_rf(mirror);
        seterr(err, errlen, "the local copy of the PC's files could not be checked; receive again");
        goto out;
    }
    os_manifest_hash(&after, h1);
    if (strcmp(h1, res->manifest_hash) != 0) {
        os_rm_rf(mirror);
        seterr(err, errlen, "the local copy of the PC's files does not match the PC's; receive again");
        goto out;
    }
    for (i = 0; i < nconv; i++) {
        cstate_entry *e = cs_put(&cs, convs[i].guid);
        if (e) {
            hash_hex(convs[i].json, convs[i].jn, e->hash);
            snprintf(e->rel, sizeof e->rel, "%s", convs[i].rel);
        }
    }
    cs_save(convert_dir, &cs);
    rc = 0;

out:
    if (rc != 0 && applying) {
        /* Put the save back: from its backup, or — if it was empty — by emptying it. */
        int back = had_backup ? os_restore_backup(save_root, backup_dir, local_err, sizeof local_err)
                              : os_clear_dir(save_root);
        if (back != 0) {
            char msg[400];
            snprintf(msg, sizeof msg, "%s — AND the automatic restore failed.%s%s%s",
                     err && err[0] ? err : "the receive failed", had_backup ? " Your save is safe at " : "",
                     had_backup ? backup_dir : "", had_backup ? " on the SD card." : "");
            snprintf(err, errlen, "%s", msg);
        } else if (err && errlen && err[0]) {
            char msg[400];
            snprintf(msg, sizeof msg, "%s The save was put back as it was.", err);
            snprintf(err, errlen, "%s", msg);
        }
    }
    os_rm_rf(staging);
    free(want);
    free_convs(convs, nconv);
    os_manifest_free(&mm);
    os_manifest_free(&lm);
    os_manifest_free(&after);
    os_remote_manifest_free(&rm);
    return rc;
}

/* -------------------------------------------------------------------- send */

int os_mcd_prepare_send(const char *save_root, const char *convert_dir, int *prepared, char *err, size_t errlen) {
    cstate cs;
    swchar *chars;
    int n, i, ok = -1;
    char mirror[600], chardir[OS_MAX_PATH_LEN + 1] = "";
    struct out_item {
        char guid[80], rel[OS_MAX_PATH_LEN + 1], hash[65];
        uint8_t *dat;
        size_t dn;
    } *items = NULL;
    int nitems = 0;

    *prepared = 0;
    os_mcd_mirror_path(convert_dir, mirror, sizeof mirror);
    cs_load(convert_dir, &cs);
    if (!os_exists(mirror) || cs.n == 0) {
        seterr(err, errlen, "Receive from the PC first, so this Switch knows where the PC keeps its characters.");
        return -1;
    }
    /* New characters go where the PC's existing ones live. */
    dir_of(cs.e[0].rel, chardir, sizeof chardir);

    chars = (swchar *)calloc(MAX_CHARS, sizeof *chars);
    items = (struct out_item *)calloc(MAX_CHARS, sizeof *items);
    if (!chars || !items) {
        free(chars);
        free(items);
        seterr(err, errlen, "out of memory");
        return -1;
    }
    n = list_switch_chars(save_root, chars, MAX_CHARS);
    if (n < 0) {
        seterr(err, errlen, "could not read this Switch's save");
        goto out;
    }
    /* Encrypt everything first; the mirror is only written once all of it worked. */
    for (i = 0; i < n; i++) {
        uint8_t *json = NULL;
        size_t jn = 0;
        char h[65];
        cstate_entry *e;
        if (read_file(chars[i].path, &json, &jn, MAX_SAVE_BYTES) != 0) continue;
        hash_hex(json, jn, h);
        e = cs_find(&cs, chars[i].guid);
        if (e && strcmp(e->hash, h) == 0) {
            free(json);
            continue; /* unchanged since it was received */
        }
        if (!os_mcd_is_character(json, jn)) {
            free(json);
            continue;
        }
        snprintf(items[nitems].guid, sizeof items[nitems].guid, "%s", chars[i].guid);
        snprintf(items[nitems].hash, sizeof items[nitems].hash, "%s", h);
        if (e) snprintf(items[nitems].rel, sizeof items[nitems].rel, "%s", e->rel);
        else {
            /* A path that would not fit is refused, never cut short into a different one. */
            int w = chardir[0] ? snprintf(items[nitems].rel, sizeof items[nitems].rel, "%s/%s.dat", chardir, chars[i].guid)
                               : snprintf(items[nitems].rel, sizeof items[nitems].rel, "%s.dat", chars[i].guid);
            if (w < 0 || (size_t)w >= sizeof items[nitems].rel) {
                free(json);
                seterr(err, errlen, "a character's folder path on the PC is too long");
                goto out;
            }
        }
        if (os_mcd_encrypt_json(json, jn, &items[nitems].dat, &items[nitems].dn, err, errlen) != 0) {
            free(json);
            goto out;
        }
        free(json);
        nitems++;
    }
    for (i = 0; i < nitems; i++) {
        char path[1400];
        cstate_entry *e;
        os_path_join(path, sizeof path, mirror, items[i].rel);
        if (write_file(path, items[i].dat, items[i].dn) != 0) {
            seterr(err, errlen, "could not write to the local copy of the PC's files");
            goto out;
        }
        e = cs_put(&cs, items[i].guid);
        if (e) {
            snprintf(e->hash, sizeof e->hash, "%s", items[i].hash);
            snprintf(e->rel, sizeof e->rel, "%s", items[i].rel);
        }
    }
    if (nitems && cs_save(convert_dir, &cs) != 0) {
        seterr(err, errlen, "could not save the bookkeeping");
        goto out;
    }
    *prepared = nitems;
    ok = 0;
out:
    for (i = 0; i < nitems; i++) free(items[i].dat);
    free(items);
    free(chars);
    return ok;
}
