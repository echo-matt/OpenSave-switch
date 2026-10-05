#include <stdlib.h>
#include <unistd.h>

#include "../core/fsutil.h"
#include "../core/titlecache.h"
#include "testutil.h"

static os_tcache_entry mk(uint64_t id, const char *name, int u, int d) {
    os_tcache_entry e;
    memset(&e, 0, sizeof e);
    e.id = id;
    snprintf(e.name, sizeof e.name, "%s", name);
    e.user = u;
    e.device = d;
    return e;
}

int main(void) {
    char dir[] = "/tmp/os-tc-XXXXXX", path[300];
    os_tcache c, d;
    os_tcache_entry e;
    const os_tcache_entry *f;
    uint64_t keep[2] = {0x0100F2C0115B6000ULL, 0x0100000000010000ULL};
    FILE *fp;

    if (!mkdtemp(dir)) return 2;
    snprintf(path, sizeof path, "%s/cache/titles.json", dir);

    os_tcache_init(&c, path); /* no file yet: empty, not an error */
    CHECK(c.n == 0 && !c.dirty);
    e = mk(0x0100F2C0115B6000ULL, "Zelda: \"Tears\" of the Kingdom \xe3\x82\xbc", 1, 0);
    CHECK(os_tcache_put(&c, &e) == 0);
    e = mk(0x0100000000010000ULL, "Super Mario Odyssey", 1, 1);
    os_tcache_put(&c, &e);
    e = mk(0x01009999999999000ULL & 0xFFFFFFFFFFFFFFFFULL, "Has no save", 0, 0);
    os_tcache_put(&c, &e);
    CHECK(c.n == 3 && c.dirty);
    CHECK(os_tcache_save(&c) == 0 && !c.dirty);
    CHECK(os_exists(path));
    CHECK(os_tcache_save(&c) == 0); /* nothing changed: nothing written */

    os_tcache_init(&d, path);
    CHECK(d.n == 3 && !d.dirty);
    f = os_tcache_find(&d, 0x0100F2C0115B6000ULL);
    CHECK(f != NULL);
    if (f) {
        CHECK_STR(f->name, "Zelda: \"Tears\" of the Kingdom \xe3\x82\xbc");
        CHECK(f->user == 1 && f->device == 0);
    }
    CHECK(os_tcache_find(&d, 0x0100000000010000ULL)->device == 1);
    CHECK(os_tcache_find(&d, 42) == NULL);

    /* A game that was uninstalled drops out; one that was kept is untouched. */
    os_tcache_retain(&d, keep, 2);
    CHECK(d.n == 2 && d.dirty);
    CHECK(os_tcache_find(&d, 0x01009999999999000ULL) == NULL);
    CHECK(os_tcache_save(&d) == 0);

    /* Replacing an entry, not duplicating it. */
    e = mk(0x0100000000010000ULL, "Renamed", 1, 0);
    os_tcache_put(&d, &e);
    CHECK(d.n == 2 && !strcmp(os_tcache_find(&d, 0x0100000000010000ULL)->name, "Renamed"));

    os_tcache_clear(&d);
    CHECK(d.n == 0 && d.dirty);
    os_tcache_free(&d);
    os_tcache_free(&c);

    /* A damaged file, or one from another version, is ignored rather than trusted. */
    fp = fopen(path, "wb");
    fputs("{\"v\":1,\"titles\":[{\"id\":\"zzzz\",\"name\":\"x\"", fp);
    fclose(fp);
    os_tcache_init(&c, path);
    CHECK(c.n == 0);
    os_tcache_free(&c);
    fp = fopen(path, "wb");
    fputs("{\"v\":99,\"titles\":[{\"id\":\"0100F2C0115B6000\",\"name\":\"x\",\"u\":1,\"d\":0}]}", fp);
    fclose(fp);
    os_tcache_init(&c, path);
    CHECK(c.n == 0);
    os_tcache_free(&c);
    /* A bad entry among good ones is skipped, not fatal. */
    fp = fopen(path, "wb");
    fputs("{\"v\":1,\"titles\":[{\"id\":\"12\",\"name\":\"short id\"},{\"id\":\"0100F2C0115B6000\",\"name\":\"ok\",\"u\":1,\"d\":0}]}", fp);
    fclose(fp);
    os_tcache_init(&c, path);
    CHECK(c.n == 1 && !strcmp(c.e[0].name, "ok"));
    os_tcache_free(&c);

    os_rm_rf(dir);
    return t_finish("titlecache");
}
