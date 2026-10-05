#include "titlecache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fsutil.h"
#include "json.h"

#define CACHE_VERSION 1

void os_tcache_init(os_tcache *c, const char *path) {
    FILE *f;
    long n;
    char *text;
    os_json *d;
    os_jn root, it;

    memset(c, 0, sizeof *c);
    snprintf(c->path, sizeof c->path, "%s", path);
    f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 8 * 1024 * 1024) {
        fclose(f);
        return;
    }
    text = (char *)malloc((size_t)n + 1);
    if (!text || fread(text, 1, (size_t)n, f) != (size_t)n) {
        free(text);
        fclose(f);
        return;
    }
    fclose(f);
    d = os_json_parse(text, (size_t)n, NULL, 0);
    free(text);
    if (!d) return; /* damaged: start again, it is only a cache */
    root = os_json_root(d);
    if (os_json_int(d, os_json_get(d, root, "v"), 0) != CACHE_VERSION) {
        os_json_free(d);
        return;
    }
    for (it = os_json_first(d, os_json_get(d, root, "titles")); it >= 0; it = os_json_next(d, it)) {
        os_tcache_entry e;
        const char *id = os_json_get_str(d, it, "id"), *name = os_json_get_str(d, it, "name");
        char *end;
        memset(&e, 0, sizeof e);
        if (!id || !name) continue;
        e.id = strtoull(id, &end, 16);
        if (*end || strlen(id) != 16) continue;
        snprintf(e.name, sizeof e.name, "%s", name);
        e.user = os_json_int(d, os_json_get(d, it, "u"), 0) != 0;
        e.device = os_json_int(d, os_json_get(d, it, "d"), 0) != 0;
        os_tcache_put(c, &e);
    }
    os_json_free(d);
    c->dirty = 0; /* what was just read is what is on disk */
}

void os_tcache_free(os_tcache *c) {
    free(c->e);
    memset(c, 0, sizeof *c);
}

const os_tcache_entry *os_tcache_find(const os_tcache *c, uint64_t id) {
    int i;
    for (i = 0; i < c->n; i++)
        if (c->e[i].id == id) return &c->e[i];
    return NULL;
}

int os_tcache_put(os_tcache *c, const os_tcache_entry *e) {
    int i;
    for (i = 0; i < c->n; i++)
        if (c->e[i].id == e->id) {
            c->e[i] = *e;
            c->dirty = 1;
            return 0;
        }
    if (c->n == c->cap) {
        int nc = c->cap ? c->cap * 2 : 128;
        os_tcache_entry *ne = (os_tcache_entry *)realloc(c->e, (size_t)nc * sizeof *ne);
        if (!ne) return -1;
        c->e = ne;
        c->cap = nc;
    }
    c->e[c->n++] = *e;
    c->dirty = 1;
    return 0;
}

void os_tcache_retain(os_tcache *c, const uint64_t *ids, int n) {
    int i, j, keep = 0;
    for (i = 0; i < c->n; i++) {
        int present = 0;
        for (j = 0; j < n && !present; j++) present = ids[j] == c->e[i].id;
        if (present) {
            if (keep != i) c->e[keep] = c->e[i];
            keep++;
        }
    }
    if (keep != c->n) {
        c->n = keep;
        c->dirty = 1;
    }
}

void os_tcache_clear(os_tcache *c) {
    if (c->n) c->dirty = 1;
    c->n = 0;
}

int os_tcache_save(os_tcache *c) {
    os_sb sb;
    char *text, tmp[300];
    size_t len;
    FILE *f;
    int i;

    if (!c->dirty) return 0;
    os_sb_init(&sb);
    os_sb_printf(&sb, "{\"v\":%d,\"titles\":[", CACHE_VERSION);
    for (i = 0; i < c->n; i++) {
        os_sb_printf(&sb, "%s{\"id\":\"%016llX\",\"name\":", i ? "," : "", (unsigned long long)c->e[i].id);
        os_sb_json_str(&sb, c->e[i].name);
        os_sb_printf(&sb, ",\"u\":%d,\"d\":%d}", c->e[i].user, c->e[i].device);
    }
    os_sb_puts(&sb, "]}");
    text = os_sb_take(&sb, &len);
    if (!text) return -1;
    if (os_mkdir_parent(c->path) != 0) {
        free(text);
        return -1;
    }
    snprintf(tmp, sizeof tmp, "%s.new", c->path);
    f = fopen(tmp, "wb");
    if (!f || fwrite(text, 1, len, f) != len) {
        if (f) fclose(f);
        free(text);
        return -1;
    }
    free(text);
    if (fclose(f) != 0) return -1;
    if (rename(tmp, c->path) != 0) {
        remove(c->path);
        if (rename(tmp, c->path) != 0) return -1;
    }
    c->dirty = 0;
    return 0;
}
