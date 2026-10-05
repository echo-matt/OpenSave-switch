#include "saves.h"

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

int saves_titles(saves_title **out, int *count) {
    NsApplicationRecord *recs = (NsApplicationRecord *)malloc(sizeof(NsApplicationRecord) * MAX_RECORDS);
    NsApplicationControlData *ctrl = (NsApplicationControlData *)malloc(sizeof(NsApplicationControlData));
    saves_title *list = (saves_title *)calloc(MAX_RECORDS, sizeof(saves_title));
    s32 n = 0;
    int i, kept = 0;

    *out = NULL;
    *count = 0;
    if (!recs || !ctrl || !list) {
        free(recs);
        free(ctrl);
        free(list);
        return -1;
    }
    if (R_FAILED(nsListApplicationRecord(recs, MAX_RECORDS, 0, &n))) {
        free(recs);
        free(ctrl);
        free(list);
        return -1;
    }
    for (i = 0; i < n; i++) {
        u64 got = 0;
        saves_title *t = &list[kept];
        NacpLanguageEntry *le = NULL;

        t->id = recs[i].application_id;
        snprintf(t->tid, sizeof t->tid, "%016lX", (unsigned long)t->id);
        snprintf(t->name, sizeof t->name, "%s", t->tid);
        memset(ctrl, 0, sizeof *ctrl);
        if (R_FAILED(nsGetApplicationControlData(NsApplicationControlSource_Storage, t->id, ctrl,
                                                 sizeof *ctrl, &got)) ||
            got < sizeof ctrl->nacp) {
            continue; /* no control data: cannot tell what it saves, so skip it */
        }
        if (R_SUCCEEDED(nacpGetLanguageEntry(&ctrl->nacp, &le)) && le && le->name[0])
            snprintf(t->name, sizeof t->name, "%s", le->name);
        t->has_user_save = ctrl->nacp.user_account_save_data_size > 0;
        t->has_device_save = ctrl->nacp.device_save_data_size > 0;
        if (t->has_user_save || t->has_device_save) kept++;
    }
    free(recs);
    free(ctrl);
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
