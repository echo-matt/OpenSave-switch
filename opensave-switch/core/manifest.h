/* Save-folder manifests, byte-compatible with internal/delta.
 *
 * A manifest lists every file under a save root with its size, a whole-file
 * SHA-256, and a SHA-256 per block; blocks are 64 KiB, 512 KiB above 20 MiB and
 * 2 MiB above 100 MiB. Two devices compare manifests to decide what differs, so
 * the hashes here must be exactly what the Go side computes — tests/ checks it
 * against a real daemon.
 *
 * Everything a peer sends is treated as hostile: os_manifest_from_json refuses
 * paths that could escape the save folder, block lists that do not add up to the
 * file, and sizes that would exhaust the console's memory.
 */
#ifndef OPENSAVE_MANIFEST_H
#define OPENSAVE_MANIFEST_H

#include <stddef.h>
#include <stdint.h>

#include "json.h"

#define OS_BLOCK_SMALL (64 * 1024)
#define OS_BLOCK_MEDIUM (512 * 1024)
#define OS_BLOCK_LARGE (2 * 1024 * 1024)

/* Limits on what is accepted from a peer. A Switch save is at most a few
 * hundred MiB and has a few hundred files; these are far above that and exist
 * only to bound memory and recursion. */
#define OS_MAX_FILES 20000
#define OS_MAX_FILE_SIZE ((int64_t)1 << 30)
#define OS_MAX_PATH_LEN 400
#define OS_MAX_DEPTH 16

typedef struct {
    uint8_t hash[32];
    int length;
} os_block;

typedef struct {
    char *path; /* relative to the save root, '/' separated */
    int64_t size;
    uint8_t hash[32];
    int block_size;
    int nblocks;
    os_block *blocks;
    int64_t mtime_ms;
} os_mfile;

typedef struct {
    os_mfile *files; /* sorted by path (byte order) */
    int nfiles, cap;
    char **dirs; /* sorted */
    int ndirs, dcap;
    int64_t latest_mtime;
    int has_extra_roots; /* the peer's game has save locations beyond the main one */
} os_manifest;

int os_block_size_for(int64_t file_size);

/* Names the sync never carries: anything with a dot-prefixed segment (the Go
 * walk skips them) and the temporary files of an interrupted write. */
int os_name_ignored(const char *rel);

/* Whether a path from a peer may be written under the save root. On refusal,
 * why receives a short reason. */
int os_path_valid(const char *rel, char *why, size_t whylen);

/* Walks root and describes what is there. Returns 0 on success. A root that
 * cannot be read is an error, never an empty manifest: an empty manifest says
 * every file was deleted. A root that does not exist is reported through
 * *missing (when non-NULL) and yields an empty manifest. */
int os_manifest_build(const char *root, os_manifest *m, int *missing, char *err, size_t errlen);

void os_manifest_free(os_manifest *m);
const os_mfile *os_manifest_find(const os_manifest *m, const char *path);

/* The digest two devices compare to know they hold identical content
 * (ManifestHash in Go): SHA-256 over sorted "path:hash\n" lines then "dir:d\n"
 * lines. out receives 64 hex characters and a NUL. */
void os_manifest_hash(const os_manifest *m, char out[65]);

/* Writes the manifest as the JSON object a peer expects under "manifest". */
void os_manifest_to_json(const os_manifest *m, os_sb *sb);

/* Reads and validates a peer's manifest object. Returns 0 on success. */
int os_manifest_from_json(const os_json *d, os_jn node, os_manifest *m, char *err, size_t errlen);

/* Hashes one file the way the manifest does. */
int os_hash_file(const char *path, os_mfile *out, char *err, size_t errlen);

#endif
