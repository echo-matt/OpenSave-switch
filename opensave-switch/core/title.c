#include "title.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

static int is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

int os_title_valid(const char *s) {
    int i;
    if (!s || strlen(s) != 16) return 0;
    for (i = 0; i < 16; i++)
        if (!is_hex(s[i])) return 0;
    return 1;
}

static int starts_with(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

int os_title_from_game_id(const char *id, char out[OS_TITLE_LEN]) {
    size_t n, end, i;
    const char *idstart;
    if (!id) return 0;
    n = strlen(id);
    end = n;
    /* An optional "-<digits>" suffix. */
    for (i = n; i > 0 && id[i - 1] >= '0' && id[i - 1] <= '9'; i--) {}
    if (i < n && i > 0 && id[i - 1] == '-' && n - i < 6) end = i - 1;
    /* The title id is the 16 characters before that, after a '-'. */
    if (end < 17 || id[end - 17] != '-') return 0;
    idstart = id + end - 16;
    {
        char tmp[OS_TITLE_LEN];
        memcpy(tmp, idstart, 16);
        tmp[16] = '\0';
        if (!os_title_valid(tmp)) return 0;
    }
    /* What precedes it must be "switch" or "<something>-title-id". */
    {
        size_t prefix = (size_t)(idstart - id) - 1; /* excludes the '-' */
        if (!(prefix == 6 && starts_with(id, "switch"))) {
            int ok = 0;
            if (prefix > 9 && memcmp(id + prefix - 9, "-title-id", 9) == 0) {
                ok = 1;
                for (i = 0; i < prefix - 9; i++) {
                    char c = id[i];
                    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) ok = 0;
                }
                if (prefix - 9 == 0) ok = 0;
            }
            if (!ok) return 0;
        }
    }
    for (i = 0; i < 16; i++) {
        char c = idstart[i];
        out[i] = (c >= 'a' && c <= 'f') ? (char)(c - 32) : c;
    }
    out[16] = '\0';
    return 1;
}

int os_title_from_save_path(const char *path, char out[OS_TITLE_LEN]) {
    const char *seg[64];
    size_t len[64];
    int n = 0, i;
    const char *p = path, *start;
    if (!path) return 0;
    start = p;
    for (;; p++) {
        if (*p == '/' || *p == '\\' || *p == '\0') {
            if (p > start) {
                if (n == 64) {
                    /* keep only the last 64 segments */
                    memmove(seg, seg + 1, 63 * sizeof *seg);
                    memmove(len, len + 1, 63 * sizeof *len);
                    n = 63;
                }
                seg[n] = start;
                len[n++] = (size_t)(p - start);
            }
            start = p + 1;
            if (*p == '\0') break;
        }
    }
    if (n < 4) return 0;
    {
        char tmp[OS_TITLE_LEN];
        if (len[n - 1] != 16) return 0;
        memcpy(tmp, seg[n - 1], 16);
        tmp[16] = '\0';
        if (!os_title_valid(tmp)) return 0;
        if (len[n - 4] != 4 || strncasecmp(seg[n - 4], "save", 4) != 0) return 0;
        for (i = 0; i < 16; i++) out[i] = (tmp[i] >= 'a' && tmp[i] <= 'f') ? (char)(tmp[i] - 32) : tmp[i];
        out[16] = '\0';
    }
    return 1;
}

void os_game_id_for_title(char *out, size_t outlen, const char *title_id) {
    size_t i, o = 0;
    const char *pre = "switch-";
    for (i = 0; pre[i] && o + 1 < outlen; i++) out[o++] = pre[i];
    for (i = 0; title_id[i] && o + 1 < outlen; i++) {
        char c = title_id[i];
        out[o++] = (c >= 'A' && c <= 'F') ? (char)(c + 32) : c;
    }
    out[o] = '\0';
}

void os_save_path_for_title(char *out, size_t outlen, const char *title_id) {
    snprintf(out, outlen, "save/0000000000000000/00000000000000000000000000000000/%s", title_id);
}
