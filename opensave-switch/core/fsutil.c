#include "fsutil.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void os_path_join(char *out, size_t outlen, const char *dir, const char *rel) {
    size_t n = strlen(dir);
    if (n && dir[n - 1] == '/') snprintf(out, outlen, "%s%s", dir, rel);
    else snprintf(out, outlen, "%s/%s", dir, rel);
}

int os_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int64_t os_file_size(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (int64_t)st.st_size : -1;
}

int os_mkdir_p(const char *dir) {
    char tmp[1024];
    size_t n = strlen(dir), i;
    struct stat st;
    if (n == 0 || n >= sizeof tmp) return -1;
    memcpy(tmp, dir, n + 1);
    while (n > 1 && tmp[n - 1] == '/') tmp[--n] = '\0';
    for (i = 1; i <= n; i++) {
        if (tmp[i] != '/' && tmp[i] != '\0') continue;
        {
            char c = tmp[i];
            tmp[i] = '\0';
            /* A device prefix such as "sdmc:" or "save:" is not a folder. */
            if (!(i > 0 && tmp[i - 1] == ':') && mkdir(tmp, 0777) != 0 && errno != EEXIST) {
                if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;
            }
            tmp[i] = c;
        }
    }
    return (stat(tmp, &st) == 0 && S_ISDIR(st.st_mode)) ? 0 : -1;
}

int os_mkdir_parent(const char *file) {
    char tmp[1024];
    char *slash;
    size_t n = strlen(file);
    if (n >= sizeof tmp) return -1;
    memcpy(tmp, file, n + 1);
    slash = strrchr(tmp, '/');
    if (!slash || slash == tmp) return 0;
    *slash = '\0';
    return os_mkdir_p(tmp);
}

int os_rm_rf(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *e;
        int rc = 0;
        if (!d) return -1;
        while ((e = readdir(d)) != NULL) {
            char child[1400];
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            os_path_join(child, sizeof child, path, e->d_name);
            if (os_rm_rf(child) != 0) rc = -1;
        }
        closedir(d);
        if (rmdir(path) != 0) rc = -1;
        return rc;
    }
    return remove(path) == 0 ? 0 : -1;
}

int os_clear_dir(const char *dir) {
    DIR *d = opendir(dir);
    struct dirent *e;
    int rc = 0;
    if (!d) return -1;
    while ((e = readdir(d)) != NULL) {
        char child[1400];
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        os_path_join(child, sizeof child, dir, e->d_name);
        if (os_rm_rf(child) != 0) rc = -1;
    }
    closedir(d);
    return rc;
}

int os_copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb"), *out;
    char *buf;
    size_t n;
    int rc = 0;
    if (!in) return -1;
    out = fopen(to, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    buf = (char *)malloc(64 * 1024);
    if (!buf) {
        fclose(in);
        fclose(out);
        return -1;
    }
    while ((n = fread(buf, 1, 64 * 1024, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            rc = -1;
            break;
        }
    }
    if (ferror(in)) rc = -1;
    free(buf);
    fclose(in);
    /* fclose is where a full file system reports the write that failed. */
    if (fclose(out) != 0) rc = -1;
    return rc;
}

int os_copy_tree(const char *from, const char *to) {
    DIR *d = opendir(from);
    struct dirent *e;
    int rc = 0;
    if (!d) return -1;
    if (os_mkdir_p(to) != 0) {
        closedir(d);
        return -1;
    }
    while ((e = readdir(d)) != NULL) {
        char src[1400], dst[1400];
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        os_path_join(src, sizeof src, from, e->d_name);
        os_path_join(dst, sizeof dst, to, e->d_name);
        if (stat(src, &st) != 0) {
            rc = -1;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            if (os_copy_tree(src, dst) != 0) {
                rc = -1;
                break;
            }
        } else if (S_ISREG(st.st_mode)) {
            if (os_copy_file(src, dst) != 0) {
                rc = -1;
                break;
            }
        }
    }
    closedir(d);
    return rc;
}
