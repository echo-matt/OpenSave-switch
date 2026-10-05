/* Usage: test_manifest <fixture dir> <go-built.json> <ours-out.json>
 *
 * The fixture is built by run_manifest_test.sh; go-built.json is what the real
 * Go delta package made of it. Ours must match, file by file and block by
 * block, and the parts of this that read peer input are exercised with
 * hostile manifests. */
#include <stdlib.h>
#include "../core/manifest.h"
#include "testutil.h"

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    char *b;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    b[n] = 0;
    fclose(f);
    if (len) *len = (size_t)n;
    return b;
}

static void test_against_go(const char *dir, const char *gojson, const char *outjson) {
    size_t gl;
    char *gtext = slurp(gojson, &gl), err[200], h1[65];
    os_json *gd;
    os_manifest ours, theirs;
    int i, missing;

    CHECK(gtext != NULL);
    if (!gtext) return;
    gd = os_json_parse(gtext, gl, err, sizeof err);
    CHECK(gd != NULL);
    if (!gd) { fprintf(stderr, "%s\n", err); return; }

    /* The parser reads Go's own output... */
    CHECK(os_manifest_from_json(gd, os_json_get(gd, 0, "manifest"), &theirs, err, sizeof err) == 0);
    if (t_failures) fprintf(stderr, "from_json: %s\n", err);
    /* ...and the build matches it. */
    CHECK(os_manifest_build(dir, &ours, &missing, err, sizeof err) == 0 && !missing);

    CHECK(ours.nfiles == theirs.nfiles);
    CHECK(ours.ndirs == theirs.ndirs);
    for (i = 0; i < ours.nfiles && i < theirs.nfiles; i++) {
        const os_mfile *a = &ours.files[i], *b = &theirs.files[i];
        int k;
        CHECK_STR(a->path, b->path);
        CHECK(a->size == b->size);
        CHECK(memcmp(a->hash, b->hash, 32) == 0);
        CHECK(a->block_size == b->block_size);
        CHECK(a->nblocks == b->nblocks);
        for (k = 0; k < a->nblocks && k < b->nblocks; k++) {
            CHECK(a->blocks[k].length == b->blocks[k].length);
            CHECK(memcmp(a->blocks[k].hash, b->blocks[k].hash, 32) == 0);
        }
        /* mtime is stored to the second here; Go's is to the millisecond. */
        CHECK(a->mtime_ms / 1000 == b->mtime_ms / 1000);
    }
    for (i = 0; i < ours.ndirs && i < theirs.ndirs; i++) CHECK_STR(ours.dirs[i], theirs.dirs[i]);

    os_manifest_hash(&ours, h1);
    CHECK_STR(h1, os_json_get_str(gd, 0, "hash"));
    printf("manifest hash %s\n", h1);

    /* What we emit must be readable by Go: written out for the script to feed
     * to `gomanifest hash`. */
    {
        os_sb sb;
        char *out;
        size_t n;
        FILE *f;
        os_sb_init(&sb);
        os_manifest_to_json(&ours, &sb);
        out = os_sb_take(&sb, &n);
        CHECK(out != NULL);
        f = fopen(outjson, "wb");
        if (f && out) { fwrite(out, 1, n, f); fclose(f); }
        /* and we can read our own output back */
        {
            os_json *rd = os_json_parse(out, n, err, sizeof err);
            os_manifest again;
            char h2[65];
            CHECK(rd != NULL);
            if (rd) {
                CHECK(os_manifest_from_json(rd, os_json_root(rd), &again, err, sizeof err) == 0);
                os_manifest_hash(&again, h2);
                CHECK_STR(h2, h1);
                os_manifest_free(&again);
                os_json_free(rd);
            }
        }
        free(out);
    }
    os_manifest_free(&ours);
    os_manifest_free(&theirs);
    os_json_free(gd);
    free(gtext);
}

/* The console mounts a save as "save:/", so a root with a trailing slash has to
 * describe the same folder as one without. */
static void test_trailing_slash(const char *dir) {
    char slashed[600], err[200], h1[65], h2[65];
    os_manifest a, b;
    int missing;
    snprintf(slashed, sizeof slashed, "%s/", dir);
    CHECK(os_manifest_build(dir, &a, &missing, err, sizeof err) == 0);
    CHECK(os_manifest_build(slashed, &b, &missing, err, sizeof err) == 0);
    os_manifest_hash(&a, h1);
    os_manifest_hash(&b, h2);
    CHECK_STR(h1, h2);
    CHECK(a.nfiles == b.nfiles && a.nfiles > 0);
    os_manifest_free(&a);
    os_manifest_free(&b);
}

static void test_missing_and_unreadable(const char *dir) {
    os_manifest m;
    char err[200];
    int missing = 0;
    char path[512];
    snprintf(path, sizeof path, "%s/does-not-exist", dir);
    CHECK(os_manifest_build(path, &m, &missing, err, sizeof err) == 0);
    CHECK(missing == 1 && m.nfiles == 0); /* a missing save is reported as missing... */
    os_manifest_free(&m);
    snprintf(path, sizeof path, "%s/small.bin", dir); /* ...and a file is not a save folder */
    CHECK(os_manifest_build(path, &m, &missing, err, sizeof err) == -1);
}

static int parse_manifest(const char *text, char *err) {
    os_json *d = os_json_parse(text, strlen(text), err, 200);
    os_manifest m;
    int rc;
    if (!d) return -2;
    rc = os_manifest_from_json(d, os_json_root(d), &m, err, 200);
    if (rc == 0) os_manifest_free(&m);
    os_json_free(d);
    return rc;
}

#define H64 "0000000000000000000000000000000000000000000000000000000000000000"
#define FILE_OK(path) "{\"files\":{\"" path "\":{\"size\":0,\"hash\":\"" H64 "\",\"blocks\":null,\"blockSize\":65536,\"mtime\":1}},\"dirs\":[]}"

static void test_hostile(void) {
    char err[200];
    const char *bad_paths[] = {"../x", "a/../../x", "/etc/passwd", "a//b", "a/", ".hidden", "a/.git/x",
                               "a\\\\b", "a:b", "x.opensave.tmp", "caf\\u00e9", "a\\u0000b", "..", "."};
    int i;
    CHECK(parse_manifest(FILE_OK("ok/file.sav"), err) == 0);
    for (i = 0; i < (int)(sizeof bad_paths / sizeof *bad_paths); i++) {
        char text[512];
        snprintf(text, sizeof text, "{\"files\":{\"%s\":{\"size\":0,\"hash\":\"" H64 "\",\"blocks\":null,\"blockSize\":65536,\"mtime\":1}},\"dirs\":[]}", bad_paths[i]);
        if (parse_manifest(text, err) == 0) fprintf(stderr, "accepted unsafe path %s\n", bad_paths[i]);
        CHECK(parse_manifest(text, err) != 0);
    }
    /* unsafe directory names are refused too */
    CHECK(parse_manifest("{\"files\":{},\"dirs\":[\"../up\"]}", err) != 0);
    /* sizes and block lists that don't add up */
    CHECK(parse_manifest("{\"files\":{\"a\":{\"size\":-1,\"hash\":\"" H64 "\",\"blocks\":[],\"blockSize\":65536}}}", err) != 0);
    CHECK(parse_manifest("{\"files\":{\"a\":{\"size\":99999999999,\"hash\":\"" H64 "\",\"blocks\":[],\"blockSize\":65536}}}", err) != 0);
    CHECK(parse_manifest("{\"files\":{\"a\":{\"size\":10,\"hash\":\"" H64 "\",\"blocks\":[],\"blockSize\":65536}}}", err) != 0);
    CHECK(parse_manifest("{\"files\":{\"a\":{\"size\":10,\"hash\":\"" H64 "\",\"blocks\":[{\"index\":0,\"hash\":\"" H64 "\",\"length\":9}],\"blockSize\":65536}}}", err) != 0);
    CHECK(parse_manifest("{\"files\":{\"a\":{\"size\":10,\"hash\":\"" H64 "\",\"blocks\":[{\"index\":0,\"hash\":\"" H64 "\",\"length\":10}],\"blockSize\":7}}}", err) != 0);
    CHECK(parse_manifest("{\"files\":{\"a\":{\"size\":10,\"hash\":\"zz\",\"blocks\":[{\"index\":0,\"hash\":\"" H64 "\",\"length\":10}],\"blockSize\":65536}}}", err) != 0);
    CHECK(parse_manifest("{\"files\":{\"a\":{\"size\":10,\"hash\":\"" H64 "\",\"blocks\":[{\"index\":0,\"hash\":\"" H64 "\",\"length\":10}],\"blockSize\":65536}}}", err) == 0);
    /* a name colliding with another once written */
    CHECK(parse_manifest("{\"files\":{\"Save\":{\"size\":0,\"hash\":\"" H64 "\",\"blocks\":null,\"blockSize\":65536},"
                         "\"save\":{\"size\":0,\"hash\":\"" H64 "\",\"blocks\":null,\"blockSize\":65536}}}", err) != 0);
    CHECK(parse_manifest("{\"files\":{\"a\":{\"size\":0,\"hash\":\"" H64 "\",\"blocks\":null,\"blockSize\":65536},"
                         "\"a/b\":{\"size\":0,\"hash\":\"" H64 "\",\"blocks\":null,\"blockSize\":65536}}}", err) != 0);
    CHECK(parse_manifest("{\"files\":{\"a\":{\"size\":0,\"hash\":\"" H64 "\",\"blocks\":null,\"blockSize\":65536}},\"dirs\":[\"a\"]}", err) != 0);
    /* not an object at all */
    CHECK(parse_manifest("[]", err) != 0);
    CHECK(parse_manifest("{\"files\":[]}", err) != 0);
}

static void test_blocksize(void) {
    CHECK(os_block_size_for(0) == OS_BLOCK_SMALL);
    CHECK(os_block_size_for(20 * 1024 * 1024) == OS_BLOCK_SMALL);
    CHECK(os_block_size_for(20 * 1024 * 1024 + 1) == OS_BLOCK_MEDIUM);
    CHECK(os_block_size_for(100 * 1024 * 1024) == OS_BLOCK_MEDIUM);
    CHECK(os_block_size_for(100 * 1024 * 1024 + 1) == OS_BLOCK_LARGE);
    CHECK(os_name_ignored(".x") && os_name_ignored("a/.b/c") && os_name_ignored("f.opensave.tmp"));
    CHECK(!os_name_ignored("a/b.sav") && !os_name_ignored("a.b/c"));
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s dir go.json out.json\n", argv[0]); return 2; }
    test_blocksize();
    test_hostile();
    test_against_go(argv[1], argv[2], argv[3]);
    test_trailing_slash(argv[1]);
    test_missing_and_unreadable(argv[1]);
    return t_finish("manifest");
}
