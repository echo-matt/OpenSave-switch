#include "manifest.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "crypto.h"
#include "fsutil.h"

/* The Go walk never follows symlinks; where the platform can tell, neither do
 * we. The console's save file system has none. */
#ifdef __SWITCH__
#define OS_STAT stat
#else
#define OS_STAT lstat
#endif

static void seterr(char *err, size_t n, const char *fmt, const char *a) {
    if (err && n) snprintf(err, n, fmt, a ? a : "");
}

int os_block_size_for(int64_t file_size) {
    if (file_size > (int64_t)100 * 1024 * 1024) return OS_BLOCK_LARGE;
    if (file_size > (int64_t)20 * 1024 * 1024) return OS_BLOCK_MEDIUM;
    return OS_BLOCK_SMALL;
}

static int has_suffix(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && strcmp(s + a - b, suf) == 0;
}

int os_name_ignored(const char *rel) {
    const char *p = rel;
    for (;;) {
        if (*p == '.') return 1;
        p = strchr(p, '/');
        if (!p) break;
        p++;
    }
    return has_suffix(rel, ".opensave.tmp");
}

int os_path_valid(const char *rel, char *why, size_t whylen) {
    const char *p;
    size_t len = strlen(rel), seg = 0;
#define REFUSE(msg)                                      \
    do {                                                 \
        if (why && whylen) snprintf(why, whylen, "%s", msg); \
        return 0;                                        \
    } while (0)
    if (len == 0) REFUSE("empty path");
    if (len > OS_MAX_PATH_LEN) REFUSE("path too long");
    if (rel[0] == '/') REFUSE("absolute path");
    if (rel[len - 1] == '/') REFUSE("trailing slash");
    for (p = rel; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '/') {
            if (seg == 0) REFUSE("empty path segment");
            seg = 0;
            continue;
        }
        if (c < 0x20 || c == 0x7f) REFUSE("control character in name");
        if (c >= 0x80) REFUSE("non-ASCII name (not supported on the Switch)");
        if (c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            REFUSE("character not allowed in a Switch save file name");
        if (++seg > 255) REFUSE("name segment too long");
    }
    /* "." and ".." segments, and any dot-prefixed segment, which the sync
     * never carries anyway: refusing them here is what keeps a hostile
     * "../../x" from ever being joined onto the save root. */
    p = rel;
    for (;;) {
        if (*p == '.') REFUSE("name begins with a dot");
        p = strchr(p, '/');
        if (!p) break;
        p++;
    }
    {
        int depth = 1;
        for (p = rel; *p; p++)
            if (*p == '/') depth++;
        if (depth > OS_MAX_DEPTH) REFUSE("folders nested too deeply");
    }
    if (has_suffix(rel, ".opensave.tmp")) REFUSE("reserved temporary-file name");
    return 1;
#undef REFUSE
}

/* ---------------------------------------------------------------- building */

static void mfile_free(os_mfile *f) {
    free(f->path);
    free(f->blocks);
}

void os_manifest_free(os_manifest *m) {
    int i;
    for (i = 0; i < m->nfiles; i++) mfile_free(&m->files[i]);
    for (i = 0; i < m->ndirs; i++) free(m->dirs[i]);
    free(m->files);
    free(m->dirs);
    memset(m, 0, sizeof *m);
}

static int add_file(os_manifest *m, os_mfile *f) {
    if (m->nfiles == m->cap) {
        int nc = m->cap ? m->cap * 2 : 32;
        os_mfile *nf = (os_mfile *)realloc(m->files, (size_t)nc * sizeof *nf);
        if (!nf) return -1;
        m->files = nf;
        m->cap = nc;
    }
    m->files[m->nfiles++] = *f;
    return 0;
}

static int add_dir(os_manifest *m, const char *path) {
    char *c;
    if (m->ndirs == m->dcap) {
        int nc = m->dcap ? m->dcap * 2 : 16;
        char **nd = (char **)realloc(m->dirs, (size_t)nc * sizeof *nd);
        if (!nd) return -1;
        m->dirs = nd;
        m->dcap = nc;
    }
    c = strdup(path);
    if (!c) return -1;
    m->dirs[m->ndirs++] = c;
    return 0;
}

int os_hash_file(const char *path, os_mfile *out, char *err, size_t errlen) {
    struct stat st;
    FILE *f;
    unsigned char *buf;
    os_sha256_ctx whole;
    int bs, cap = 0;
    int64_t total = 0;

    memset(out, 0, sizeof *out);
    if (stat(path, &st) != 0) {
        seterr(err, errlen, "cannot stat %s", path);
        return -1;
    }
    bs = os_block_size_for((int64_t)st.st_size);
    f = fopen(path, "rb");
    if (!f) {
        seterr(err, errlen, "cannot open %s", path);
        return -1;
    }
    buf = (unsigned char *)malloc((size_t)bs);
    if (!buf) {
        fclose(f);
        seterr(err, errlen, "out of memory", NULL);
        return -1;
    }
    os_sha256_init(&whole);
    for (;;) {
        size_t n = fread(buf, 1, (size_t)bs, f);
        if (n > 0) {
            if (out->nblocks == cap) {
                int nc = cap ? cap * 2 : 8;
                os_block *nb = (os_block *)realloc(out->blocks, (size_t)nc * sizeof *nb);
                if (!nb) {
                    free(buf);
                    fclose(f);
                    free(out->blocks);
                    memset(out, 0, sizeof *out);
                    seterr(err, errlen, "out of memory", NULL);
                    return -1;
                }
                out->blocks = nb;
                cap = nc;
            }
            os_sha256(buf, n, out->blocks[out->nblocks].hash);
            out->blocks[out->nblocks].length = (int)n;
            out->nblocks++;
            os_sha256_update(&whole, buf, n);
            total += (int64_t)n;
        }
        if (n < (size_t)bs) {
            if (ferror(f)) {
                free(buf);
                fclose(f);
                free(out->blocks);
                memset(out, 0, sizeof *out);
                seterr(err, errlen, "read error in %s", path);
                return -1;
            }
            break;
        }
    }
    free(buf);
    fclose(f);
    os_sha256_final(&whole, out->hash);
    out->size = total;
    out->block_size = bs;
    out->mtime_ms = (int64_t)st.st_mtime * 1000;
    return 0;
}

static int cmp_file(const void *a, const void *b) {
    return strcmp(((const os_mfile *)a)->path, ((const os_mfile *)b)->path);
}
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int walk(const char *root, const char *rel, int depth, os_manifest *m, char *err, size_t errlen) {
    char dirpath[1024];
    DIR *d;
    struct dirent *e;
    if (depth > OS_MAX_DEPTH) {
        seterr(err, errlen, "folders nested too deeply", NULL);
        return -1;
    }
    if (rel[0]) os_path_join(dirpath, sizeof dirpath, root, rel);
    else snprintf(dirpath, sizeof dirpath, "%s", root);
    d = opendir(dirpath);
    if (!d) {
        seterr(err, errlen, "cannot read folder %s", dirpath);
        return -1;
    }
    while ((e = readdir(d)) != NULL) {
        char relchild[OS_MAX_PATH_LEN + 300], full[1400];
        struct stat st;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (e->d_name[0] == '.') continue; /* dot entries are never part of a save */
        if (rel[0]) snprintf(relchild, sizeof relchild, "%s/%s", rel, e->d_name);
        else snprintf(relchild, sizeof relchild, "%s", e->d_name);
        if (strlen(relchild) > OS_MAX_PATH_LEN) {
            closedir(d);
            seterr(err, errlen, "path too long: %s", relchild);
            return -1;
        }
        os_path_join(full, sizeof full, root, relchild);
        if (OS_STAT(full, &st) != 0) {
            if (errno == ENOENT) continue; /* vanished since it was listed */
            closedir(d);
            seterr(err, errlen, "cannot stat %s", full);
            return -1;
        }
        if (S_ISDIR(st.st_mode)) {
            if (add_dir(m, relchild) != 0 || walk(root, relchild, depth + 1, m, err, errlen) != 0) {
                closedir(d);
                if (err && !err[0]) seterr(err, errlen, "out of memory", NULL);
                return -1;
            }
        } else if (S_ISREG(st.st_mode)) {
            os_mfile f;
            if (has_suffix(relchild, ".opensave.tmp")) continue;
            if (m->nfiles >= OS_MAX_FILES) {
                closedir(d);
                seterr(err, errlen, "too many files", NULL);
                return -1;
            }
            /* A file that exists but cannot be read is an error, not an
             * omission: leaving it out would describe it as deleted. */
            if (os_hash_file(full, &f, err, errlen) != 0) {
                closedir(d);
                return -1;
            }
            f.path = strdup(relchild);
            if (!f.path || add_file(m, &f) != 0) {
                free(f.path);
                free(f.blocks);
                closedir(d);
                seterr(err, errlen, "out of memory", NULL);
                return -1;
            }
            if (f.mtime_ms > m->latest_mtime) m->latest_mtime = f.mtime_ms;
        }
    }
    closedir(d);
    return 0;
}

int os_manifest_build(const char *root, os_manifest *m, int *missing, char *err, size_t errlen) {
    struct stat st;
    memset(m, 0, sizeof *m);
    if (missing) *missing = 0;
    if (err && errlen) err[0] = '\0';
    if (stat(root, &st) != 0) {
        if (errno == ENOENT) {
            if (missing) *missing = 1;
            return 0;
        }
        seterr(err, errlen, "cannot read %s", root);
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        seterr(err, errlen, "%s is not a folder", root);
        return -1;
    }
    if (walk(root, "", 0, m, err, errlen) != 0) {
        if (err && errlen && !err[0]) snprintf(err, errlen, "could not read the save");
        os_manifest_free(m);
        return -1;
    }
    if (m->nfiles > 1) qsort(m->files, (size_t)m->nfiles, sizeof *m->files, cmp_file);
    if (m->ndirs > 1) qsort(m->dirs, (size_t)m->ndirs, sizeof *m->dirs, cmp_str);
    return 0;
}

const os_mfile *os_manifest_find(const os_manifest *m, const char *path) {
    int lo = 0, hi = m->nfiles - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(path, m->files[mid].path);
        if (c == 0) return &m->files[mid];
        if (c < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return NULL;
}

void os_manifest_hash(const os_manifest *m, char out[65]) {
    os_sha256_ctx c;
    uint8_t sum[32];
    char hex[65];
    char **dirs;
    int i;
    os_sha256_init(&c);
    for (i = 0; i < m->nfiles; i++) {
        os_hex_encode(hex, m->files[i].hash, 32);
        os_sha256_update(&c, m->files[i].path, strlen(m->files[i].path));
        os_sha256_update(&c, ":", 1);
        os_sha256_update(&c, hex, 64);
        os_sha256_update(&c, "\n", 1);
    }
    /* Directories hash in sorted order whatever order they arrived in. */
    dirs = m->ndirs ? (char **)malloc((size_t)m->ndirs * sizeof *dirs) : NULL;
    if (dirs) {
        memcpy(dirs, m->dirs, (size_t)m->ndirs * sizeof *dirs);
        if (m->ndirs > 1) qsort(dirs, (size_t)m->ndirs, sizeof *dirs, cmp_str);
        for (i = 0; i < m->ndirs; i++) {
            os_sha256_update(&c, "dir:", 4);
            os_sha256_update(&c, dirs[i], strlen(dirs[i]));
            os_sha256_update(&c, "\n", 1);
        }
        free(dirs);
    }
    os_sha256_final(&c, sum);
    os_hex_encode(out, sum, 32);
}

void os_manifest_to_json(const os_manifest *m, os_sb *sb) {
    time_t now = time(NULL);
    struct tm tmv;
    char ts[40];
    int i, j;
    gmtime_r(&now, &tmv);
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tmv);
    os_sb_printf(sb, "{\"timestamp\":\"%s\",\"latestMtime\":%lld,\"files\":{", ts, (long long)m->latest_mtime);
    for (i = 0; i < m->nfiles; i++) {
        const os_mfile *f = &m->files[i];
        char hex[65];
        if (i) os_sb_putc(sb, ',');
        os_sb_json_str(sb, f->path);
        os_hex_encode(hex, f->hash, 32);
        os_sb_printf(sb, ":{\"size\":%lld,\"hash\":\"%s\",\"blocks\":[", (long long)f->size, hex);
        for (j = 0; j < f->nblocks; j++) {
            os_hex_encode(hex, f->blocks[j].hash, 32);
            os_sb_printf(sb, "%s{\"index\":%d,\"hash\":\"%s\",\"length\":%d}", j ? "," : "", j, hex,
                         f->blocks[j].length);
        }
        os_sb_printf(sb, "],\"blockSize\":%d,\"mtime\":%lld}", f->block_size ? f->block_size : OS_BLOCK_SMALL,
                     (long long)f->mtime_ms);
    }
    os_sb_puts(sb, "},\"dirs\":[");
    for (i = 0; i < m->ndirs; i++) {
        if (i) os_sb_putc(sb, ',');
        os_sb_json_str(sb, m->dirs[i]);
    }
    os_sb_puts(sb, "]}");
}

/* -------------------------------------------------------------- from a peer */

static int unhex32(uint8_t out[32], const char *s) {
    int i;
    if (!s || strlen(s) != 64) return -1;
    for (i = 0; i < 64; i++) {
        char c = s[i];
        unsigned v;
        if (c >= '0' && c <= '9') v = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v = (unsigned)(c - 'a' + 10);
        else return -1;
        if (i % 2 == 0) out[i / 2] = (uint8_t)(v << 4);
        else out[i / 2] |= (uint8_t)v;
    }
    return 0;
}

static void lower_ascii(char *s) {
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z') *s = (char)(*s + 32);
}

static int cmp_lower(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

#define FAIL(fmt, arg)                                  \
    do {                                                \
        if (err && errlen) snprintf(err, errlen, fmt, arg); \
        os_manifest_free(m);                            \
        return -1;                                      \
    } while (0)

int os_manifest_from_json(const os_json *d, os_jn node, os_manifest *m, char *err, size_t errlen) {
    os_jn files, dirs, extra, it;
    char why[96];
    int i;

    memset(m, 0, sizeof *m);
    if (os_json_type(d, node) != J_OBJ) FAIL("%s", "manifest is not an object");
    m->latest_mtime = os_json_int(d, os_json_get(d, node, "latestMtime"), 0);
    extra = os_json_get(d, node, "extraRoots");
    m->has_extra_roots = os_json_type(d, extra) == J_OBJ && os_json_count(d, extra) > 0;

    files = os_json_get(d, node, "files");
    if (files != OS_JN_NONE && os_json_type(d, files) != J_NULL && os_json_type(d, files) != J_OBJ)
        FAIL("%s", "manifest files is not an object");
    if (os_json_count(d, files) > OS_MAX_FILES) FAIL("%s", "manifest lists too many files");

    for (it = os_json_first(d, files); it >= 0; it = os_json_next(d, it)) {
        os_mfile f;
        os_jn blocks, b;
        const char *path = os_json_key(d, it), *h;
        int64_t size, bs, expect_blocks, covered = 0;
        int k = 0;

        memset(&f, 0, sizeof f);
        if (!path || !os_path_valid(path, why, sizeof why)) {
            if (err && errlen) snprintf(err, errlen, "unsafe file name \"%.60s\" from peer: %s", path ? path : "", why);
            os_manifest_free(m);
            return -1;
        }
        if (os_json_type(d, it) != J_OBJ) FAIL("file entry for %.60s is not an object", path);
        size = os_json_int(d, os_json_get(d, it, "size"), -1);
        if (size < 0 || size > OS_MAX_FILE_SIZE) FAIL("file %.60s has an unusable size", path);
        if (unhex32(f.hash, os_json_get_str(d, it, "hash")) != 0) FAIL("file %.60s has a bad hash", path);
        bs = os_json_int(d, os_json_get(d, it, "blockSize"), 0);
        if (bs == 0 && size == 0) bs = OS_BLOCK_SMALL;
        if (bs != OS_BLOCK_SMALL && bs != OS_BLOCK_MEDIUM && bs != OS_BLOCK_LARGE)
            FAIL("file %.60s uses an unsupported block size", path);
        expect_blocks = (size + bs - 1) / bs;

        blocks = os_json_get(d, it, "blocks");
        if (os_json_type(d, blocks) != J_ARR && !(size == 0 && os_json_type(d, blocks) != J_OBJ))
            FAIL("file %.60s has no block list", path);
        if ((int64_t)os_json_count(d, blocks) != expect_blocks) FAIL("file %.60s: block list does not match its size", path);

        f.size = size;
        f.block_size = (int)bs;
        f.mtime_ms = os_json_int(d, os_json_get(d, it, "mtime"), 0);
        f.nblocks = (int)expect_blocks;
        if (expect_blocks) {
            f.blocks = (os_block *)calloc((size_t)expect_blocks, sizeof(os_block));
            if (!f.blocks) FAIL("%s", "out of memory");
        }
        for (b = os_json_first(d, blocks); b >= 0; b = os_json_next(d, b), k++) {
            int64_t len = os_json_int(d, os_json_get(d, b, "length"), -1);
            int64_t want = (k == expect_blocks - 1) ? size - (int64_t)k * bs : bs;
            h = os_json_get_str(d, b, "hash");
            if (len != want || unhex32(f.blocks[k].hash, h) != 0 ||
                os_json_int(d, os_json_get(d, b, "index"), k) != k) {
                free(f.blocks);
                FAIL("file %.60s has an inconsistent block list", path);
            }
            f.blocks[k].length = (int)len;
            covered += len;
        }
        if (covered != size) {
            free(f.blocks);
            FAIL("file %.60s: its blocks do not add up to its size", path);
        }
        f.path = strdup(path);
        if (!f.path || add_file(m, &f) != 0) {
            free(f.path);
            free(f.blocks);
            FAIL("%s", "out of memory");
        }
    }
    if (m->nfiles > 1) qsort(m->files, (size_t)m->nfiles, sizeof *m->files, cmp_file);

    dirs = os_json_get(d, node, "dirs");
    if (dirs != OS_JN_NONE && os_json_type(d, dirs) != J_NULL && os_json_type(d, dirs) != J_ARR)
        FAIL("%s", "manifest dirs is not a list");
    for (it = os_json_first(d, dirs); it >= 0; it = os_json_next(d, it)) {
        const char *p = os_json_str(d, it);
        if (!p || !os_path_valid(p, why, sizeof why)) {
            if (err && errlen) snprintf(err, errlen, "unsafe folder name \"%.60s\" from peer", p ? p : "");
            os_manifest_free(m);
            return -1;
        }
        if (add_dir(m, p) != 0) FAIL("%s", "out of memory");
    }
    if (m->ndirs > 1) qsort(m->dirs, (size_t)m->ndirs, sizeof *m->dirs, cmp_str);

    /* Paths that would collide once written: the same name twice, names
     * differing only in case (a case-insensitive file system merges them), a
     * file that is also a folder. */
    {
        char **keys = m->nfiles ? (char **)calloc((size_t)m->nfiles, sizeof *keys) : NULL;
        int bad = 0;
        if (m->nfiles && !keys) FAIL("%s", "out of memory");
        for (i = 0; i < m->nfiles; i++) {
            keys[i] = strdup(m->files[i].path);
            if (!keys[i]) bad = 2;
            else lower_ascii(keys[i]);
        }
        if (!bad) {
            if (m->nfiles > 1) qsort(keys, (size_t)m->nfiles, sizeof *keys, cmp_lower);
            for (i = 1; i < m->nfiles; i++)
                if (strcmp(keys[i - 1], keys[i]) == 0) bad = 1;
        }
        for (i = 0; i < m->nfiles; i++) free(keys[i]);
        free(keys);
        if (bad == 2) FAIL("%s", "out of memory");
        if (bad) FAIL("%s", "peer sent two files whose names differ only in case");
    }
    for (i = 0; i < m->nfiles; i++) {
        char prefix[OS_MAX_PATH_LEN + 2];
        const char *p = m->files[i].path, *s;
        for (s = strchr(p, '/'); s; s = strchr(s + 1, '/')) {
            size_t n = (size_t)(s - p);
            memcpy(prefix, p, n);
            prefix[n] = '\0';
            if (os_manifest_find(m, prefix)) FAIL("%s", "peer sent a name that is both a file and a folder");
        }
    }
    for (i = 0; i < m->ndirs; i++)
        if (os_manifest_find(m, m->dirs[i])) FAIL("%s", "peer sent a name that is both a file and a folder");
    return 0;
}
