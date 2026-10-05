/* File-system helpers used by sync: nothing clever, but every function reports
 * failure rather than carrying on, because the caller is moving someone's
 * save. */
#ifndef OPENSAVE_FSUTIL_H
#define OPENSAVE_FSUTIL_H

#include <stddef.h>
#include <stdint.h>

/* dir + "/" + rel, without doubling the slash when dir already ends in one.
 * The console's save data is mounted as "save:/", and "save://x" is not a path
 * its file system is known to accept. */
void os_path_join(char *out, size_t outlen, const char *dir, const char *rel);

/* Creates dir and any missing parents. Returns 0 if it exists afterwards. */
int os_mkdir_p(const char *dir);
/* Creates the folders above a file's path. */
int os_mkdir_parent(const char *file);
/* Removes a file or a folder and everything in it. Missing is not an error. */
int os_rm_rf(const char *path);
/* Copies a file, truncating the destination. Returns 0 on success. */
int os_copy_file(const char *from, const char *to);
/* Copies a folder tree's regular files and sub-folders, dot entries included
 * (a backup must hold what is really there, not what the sync would carry). */
int os_copy_tree(const char *from, const char *to);
/* Removes everything inside dir, leaving dir itself. */
int os_clear_dir(const char *dir);
/* Whether the path exists. */
int os_exists(const char *path);
/* A path's size, or -1. */
int64_t os_file_size(const char *path);

#endif
