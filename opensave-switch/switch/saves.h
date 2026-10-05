/* Finding games and opening their save data on the console. */
#ifndef OPENSAVE_SAVES_H
#define OPENSAVE_SAVES_H

#include <stddef.h>
#include <switch.h>

typedef struct {
    u64 id;
    char tid[17]; /* 16 hex digits, upper case */
    char name[0x201];
    int has_user_save;   /* the game keeps a save per user account */
    int has_device_save; /* ...and/or one for the whole console */
} saves_title;

typedef struct {
    AccountUid uid;
    char nickname[0x21];
} saves_user;

/* The name the save is mounted under: paths read "save:/...". */
#define SAVES_MOUNT_ROOT "save:/"

int saves_init(void);
void saves_exit(void);

/* Returns how many user accounts there are (at most max). */
int saves_users(saves_user *out, int max);

/* Installed games that keep save data, sorted by name. *out is malloc'd. */
int saves_titles(saves_title **out, int *count);

/* Mounts a game's save data at SAVES_MOUNT_ROOT, replacing whatever was
 * mounted. For a game with a per-user save, u is the account. Returns 0 on
 * success, else -1 with a sentence for the person in err. */
int saves_mount(const saves_title *t, const saves_user *u, char *err, size_t errlen);

/* Makes everything written since the mount permanent. Until this succeeds the
 * file system holds the changes uncommitted, and unmounting discards them. */
int saves_commit(char *err, size_t errlen);

void saves_unmount(void);
const char *saves_mounted_tid(void);

#endif
