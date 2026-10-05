#include "saves.h"
#include "../core/titlecache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_RECORDS 2048

static int g_mounted;
static u64 g_mounted_id;
static AccountUid g_mounted_uid;
static int g_mounted_device;
static char g_mounted_tid[17];

int saves_init(void) {
    Result rc = nsInitialize();
    if (R_FAILED(rc)) return -1;
    rc = accountInitialize(AccountServiceType_Application);
    if (R_FAILED(rc)) {
        nsExit();
        return -1;
    }
    return 0;
}

void saves_exit(void) {
    saves_unmount();
    accountExit();
    nsExit();
}

int saves_users(saves_user *out, int max) {
    AccountUid uids[ACC_USER_LIST_SIZE];
    s32 total = 0;
    int i, n = 0;
    if (R_FAILED(accountListAllUsers(uids, ACC_USER_LIST_SIZE, &total))) return 0;
    for (i = 0; i < total && n < max; i++) {
        AccountProfile prof;
        AccountProfileBase base;
        AccountUserData ud;
        memset(&base, 0, sizeof base);
        out[n].uid = uids[i];
        snprintf(out[n].nickname, sizeof out[n].nickname, "User %d", i + 1);
        if (R_SUCCEEDED(accountGetProfile(&prof, uids[i]))) {
            if (R_SUCCEEDED(accountProfileGet(&prof, &ud, &base))) {
                snprintf(out[n].nickname, sizeof out[n].nickname, "%s", base.nickname);
            }
            accountProfileClose(&prof);
        }
        n++;
    }
    return n;
}

static int by_name(const void *a, const void *b) {
    return strcasecmp(((const saves_title *)a)->name, ((const saves_title *)b)->name);
}

#define TITLE_CACHE_PATH "sdmc:/config/opensave/titles.json"

/* Reads one game's name and save kinds from the system. Only the first part of
 * the control record is needed (the rest is the icon), so a small buffer is
 * tried first; if the service refuses that, the whole record is asked for. */
static int read_control(NsApplicationControlData *ctrl, u64 id, os_tcache_entry *out) {
    u64 got = 0;
    NacpLanguageEntry *le = NULL;
    Result rc;

    memset(&ctrl->nacp, 0, sizeof ctrl->nacp);
    rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, id, ctrl, sizeof ctrl->nacp, &got);
    if (R_FAILED(rc) || got < sizeof ctrl->nacp)
        rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, id, ctrl, sizeof *ctrl, &got);
    if (R_FAILED(rc) || got < sizeof ctrl->nacp) return -1;

    memset(out, 0, sizeof *out);
    out->id = id;
    snprintf(out->name, sizeof out->name, "%016lX", (unsigned long)id);
    if (R_SUCCEEDED(nacpGetLanguageEntry(&ctrl->nacp, &le)) && le && le->name[0])
        snprintf(out->name, sizeof out->name, "%s", le->name);
    out->user = ctrl->nacp.user_account_save_data_size > 0;
    out->device = ctrl->nacp.device_save_data_size > 0;
    return 0;
}

int saves_titles(saves_title **out, int *count, int rescan, saves_progress_fn progress, void *ctx) {
    NsApplicationRecord *recs = (NsApplicationRecord *)malloc(sizeof(NsApplicationRecord) * MAX_RECORDS);
    NsApplicationControlData *ctrl = NULL;
    saves_title *list = (saves_title *)calloc(MAX_RECORDS, sizeof(saves_title));
    u64 *ids = (u64 *)malloc(sizeof(u64) * MAX_RECORDS);
    os_tcache cache;
    s32 n = 0;
    int i, kept = 0, misses = 0, done = 0;

    *out = NULL;
    *count = 0;
    if (!recs || !list || !ids || R_FAILED(nsListApplicationRecord(recs, MAX_RECORDS, 0, &n))) {
        free(recs);
        free(list);
        free(ids);
        return -1;
    }
    os_tcache_init(&cache, TITLE_CACHE_PATH);
    if (rescan) os_tcache_clear(&cache);

    for (i = 0; i < n; i++) {
        ids[i] = recs[i].application_id;
        if (!os_tcache_find(&cache, ids[i])) misses++;
    }
    if (misses) {
        ctrl = (NsApplicationControlData *)malloc(sizeof(NsApplicationControlData));
        if (!ctrl) misses = -1; /* cannot look anything new up; the cache still serves what it has */
    }

    for (i = 0; i < n; i++) {
        const os_tcache_entry *c = os_tcache_find(&cache, ids[i]);
        os_tcache_entry fresh;
        if (!c) {
            if (misses <= 0) continue;
            if (progress) progress(ctx, done, misses);
            done++;
            if (read_control(ctrl, ids[i], &fresh) != 0) continue; /* not cached: tried again next time */
            os_tcache_put(&cache, &fresh);
            c = os_tcache_find(&cache, ids[i]);
            if (!c) continue;
        }
        if (!c->user && !c->device) continue;
        list[kept].id = c->id;
        snprintf(list[kept].tid, sizeof list[kept].tid, "%016lX", (unsigned long)c->id);
        snprintf(list[kept].name, sizeof list[kept].name, "%s", c->name);
        list[kept].has_user_save = c->user;
        list[kept].has_device_save = c->device;
        kept++;
    }
    if (progress && misses > 0) progress(ctx, misses, misses);

    /* Games that are gone leave the cache; a failed write only costs speed. */
    os_tcache_retain(&cache, (const uint64_t *)ids, n);
    os_tcache_save(&cache);
    os_tcache_free(&cache);
    free(recs);
    free(ctrl);
    free(ids);
    qsort(list, (size_t)kept, sizeof *list, by_name);
    *out = list;
    *count = kept;
    return 0;
}

void saves_unmount(void) {
    if (g_mounted) {
        fsdevUnmountDevice("save");
        g_mounted = 0;
    }
}

const char *saves_mounted_tid(void) { return g_mounted ? g_mounted_tid : ""; }

int saves_mount(const saves_title *t, const saves_user *u, char *err, size_t errlen) {
    FsSaveDataAttribute attr;
    FsFileSystem fs;
    Result rc;
    int device = !t->has_user_save && t->has_device_save;

    if (g_mounted && g_mounted_id == t->id && g_mounted_device == device &&
        (device || memcmp(&g_mounted_uid, &u->uid, sizeof g_mounted_uid) == 0))
        return 0;
    saves_unmount();

    memset(&attr, 0, sizeof attr);
    attr.application_id = t->id;
    if (device) {
        attr.save_data_type = FsSaveDataType_Device;
    } else {
        attr.save_data_type = FsSaveDataType_Account;
        attr.uid = u->uid;
    }
    rc = fsOpenSaveDataFileSystem(&fs, FsSaveDataSpaceId_User, &attr);
    if (R_FAILED(rc)) {
        snprintf(err, errlen,
                 "could not open the save data (error 0x%x). The game may have no save for this user yet, or is running.",
                 (unsigned)rc);
        return -1;
    }
    if (fsdevMountDevice("save", fs) < 0) {
        fsFsClose(&fs);
        snprintf(err, errlen, "could not mount the save data");
        return -1;
    }
    g_mounted = 1;
    g_mounted_id = t->id;
    g_mounted_device = device;
    g_mounted_uid = u->uid;
    snprintf(g_mounted_tid, sizeof g_mounted_tid, "%s", t->tid);
    return 0;
}

int saves_commit(char *err, size_t errlen) {
    Result rc;
    if (!g_mounted) {
        snprintf(err, errlen, "no save is mounted");
        return -1;
    }
    rc = fsdevCommitDevice("save");
    if (R_FAILED(rc)) {
        snprintf(err, errlen, "the save could not be committed (error 0x%x), so the change was NOT kept", (unsigned)rc);
        return -1;
    }
    return 0;
}
