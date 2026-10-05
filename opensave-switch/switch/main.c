/* OpenSave for Nintendo Switch.
 *
 * A console interface over the portable core in ../core: pair with the OpenSave
 * app on a PC, then take a game's save from the PC onto this Switch, or send
 * this Switch's save to the PC.
 *
 * Needs custom firmware (Atmosphere): only homebrew running with full file
 * system access can open the save data of other games.
 */
#include <arpa/inet.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <switch.h>

#include "../core/fsutil.h"
#include "../core/peer.h"
#include "../core/platform.h"
#include "../core/server.h"
#include "../core/state.h"
#include "../core/sync.h"
#include "../core/title.h"
#include "saves.h"

#define CONFIG_PATH "sdmc:/config/opensave/state.json"
#define BACKUP_ROOT "sdmc:/switch/OpenSave/backups"
#define STAGING_ROOT "sdmc:/switch/OpenSave/staging"
#define KEEP_BACKUPS 3
#define PAGE_ROWS 20

#define C_RED "\x1b[31;1m"
#define C_GRN "\x1b[32;1m"
#define C_YEL "\x1b[33;1m"
#define C_CYN "\x1b[36;1m"
#define C_RST "\x1b[0m"

typedef enum { SCR_MAIN, SCR_PAIR, SCR_WAIT_PAIR, SCR_GAMES, SCR_GAME } screen_t;

static os_state g_st;
static os_server g_srv;
static PadState g_pad;
static int g_dirty = 1;
static char g_banner[300];        /* a line shown on the main screen: what last happened */
static int g_paired_event;        /* the server reports a pairing completed */
static char g_served[80];         /* what the server last did, for the transfer screens */

static saves_user g_users[ACC_USER_LIST_SIZE];
static int g_nusers, g_user;
static saves_title *g_titles;
static int g_ntitles, g_sel, g_top;

/* ----------------------------------------------------------------- helpers */

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    consoleUpdate(NULL);
}

static os_peer *the_peer(void) {
    int i;
    for (i = 0; i < OS_MAX_PEERS; i++)
        if (g_st.peers[i].in_use) return &g_st.peers[i];
    return NULL;
}

static const char *local_ip(char *buf, size_t n) {
    struct in_addr a;
    a.s_addr = (in_addr_t)gethostid();
    if (a.s_addr == 0 || a.s_addr == htonl(INADDR_LOOPBACK)) {
        snprintf(buf, n, "not connected");
    } else {
        snprintf(buf, n, "%s", inet_ntoa(a));
    }
    return buf;
}

static void wait_for_a(void) {
    say("\nPress A to continue.\n");
    while (appletMainLoop()) {
        padUpdate(&g_pad);
        if (padGetButtonsDown(&g_pad) & (HidNpadButton_A | HidNpadButton_B)) return;
        os_server_poll(&g_srv, 0);
        consoleUpdate(NULL);
    }
}

/* Yes/no: A confirms, B declines. */
static int confirm(void) {
    while (appletMainLoop()) {
        u64 d;
        padUpdate(&g_pad);
        d = padGetButtonsDown(&g_pad);
        if (d & HidNpadButton_A) return 1;
        if (d & HidNpadButton_B) return 0;
        os_server_poll(&g_srv, 0);
        consoleUpdate(NULL);
    }
    return 0;
}

/* Serves the PC for a few seconds, so it can call back after a transfer. */
static void linger(int seconds) {
    time_t end = time(NULL) + seconds;
    while (appletMainLoop() && time(NULL) < end) {
        os_server_poll(&g_srv, 100);
        consoleUpdate(NULL);
    }
}

/* ------------------------------------------------------------ server hooks */

static int hook_open_save(void *ctx, const char *tid, char *root, size_t rootlen, char *err, size_t errlen) {
    int i;
    (void)ctx;
    for (i = 0; i < g_ntitles; i++) {
        if (strcasecmp(g_titles[i].tid, tid) == 0) {
            if (g_nusers == 0 && g_titles[i].has_user_save) {
                snprintf(err, errlen, "no user account on this Switch");
                return -1;
            }
            if (saves_mount(&g_titles[i], &g_users[g_user], err, errlen) != 0) return -1;
            snprintf(root, rootlen, "%s", SAVES_MOUNT_ROOT);
            return 0;
        }
    }
    snprintf(err, errlen, "This Switch has no save data for that game.");
    return -1;
}

static void hook_pairing_request(void *ctx, const os_incoming *req) {
    (void)ctx;
    (void)req;
    g_dirty = 1;
}

static void hook_paired(void *ctx, const os_peer *p) {
    (void)ctx;
    (void)p;
    g_paired_event = 1;
    g_dirty = 1;
}

static void hook_unpaired(void *ctx, const char *id) {
    (void)ctx;
    (void)id;
    snprintf(g_banner, sizeof g_banner, "The PC unpaired this Switch.");
    g_dirty = 1;
}

static void hook_peer_update(void *ctx, const os_peer *p, const char *game, const char *tid) {
    int i;
    (void)ctx;
    (void)p;
    (void)game;
    for (i = 0; i < g_ntitles; i++)
        if (strcasecmp(g_titles[i].tid, tid) == 0) {
            snprintf(g_banner, sizeof g_banner, "The PC has newer progress for %s.", g_titles[i].name);
            g_dirty = 1;
            return;
        }
    snprintf(g_banner, sizeof g_banner, "The PC has newer progress for a game (%s).", tid);
    g_dirty = 1;
}

static void hook_served(void *ctx, const char *what) {
    (void)ctx;
    snprintf(g_served, sizeof g_served, "%s", what);
}

/* ----------------------------------------------------------------- screens */

static void draw_main(void) {
    char ip[32], hint[300];
    os_peer *p = the_peer();
    time_t now = time(NULL);
    struct tm tmv;

    consoleClear();
    printf(C_CYN "OpenSave for Switch" C_RST "  v0.1.0\n");
    printf("------------------------------------------------------------\n");
    printf("This Switch : %s\n", g_st.device_name);
    printf("Address     : %s : %d\n", local_ip(ip, sizeof ip), os_server_port(&g_srv));
    gmtime_r(&now, &tmv);
    printf("Clock (UTC) : %04d-%02d-%02d %02d:%02d\n", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
           tmv.tm_min);
    if (os_clock_hint(&g_st, hint, sizeof hint)[0]) printf(C_RED "%s\n" C_RST, hint);
    if (p) {
        char fp[OS_FINGERPRINT_LEN];
        os_fingerprint(fp, g_st.pub, p->pubkey);
        printf("Paired with : " C_GRN "%s" C_RST " (%s:%d)\n", p->name, p->address, p->port);
        printf("Fingerprint : %s\n", fp);
    } else {
        printf("Paired with : " C_YEL "nobody yet" C_RST "\n");
    }
    printf("------------------------------------------------------------\n\n");
    if (g_banner[0]) printf(C_YEL "%s\n\n" C_RST, g_banner);
    if (p) {
        printf(" A  Games: send or receive a save\n");
        printf(" Y  Pair with a different PC / unpair\n");
    } else {
        printf(" A  Pair with a PC\n");
    }
    printf(" +  Exit\n\n");
    if (!p) {
        printf("On the PC, open OpenSave and check that it is running.\n");
        printf("Then press A here, enter the PC's address, and approve\n");
        printf("this Switch under Devices in the OpenSave app.\n");
    }
    consoleUpdate(NULL);
}

/* Offers a pairing request somebody sent this Switch. */
static void handle_incoming(void) {
    int i;
    for (i = 0; i < OS_MAX_PENDING; i++) {
        os_incoming *in = &g_st.incoming[i];
        char fp[OS_FINGERPRINT_LEN], id[64], err[200];
        if (!in->in_use) continue;
        if (in->has_key) os_fingerprint(fp, g_st.pub, in->pubkey);
        else snprintf(fp, sizeof fp, "(no key)");
        snprintf(id, sizeof id, "%s", in->peer_id);
        consoleClear();
        printf(C_CYN "Pairing request" C_RST "\n\n");
        printf("\"%s\" at %s wants to pair with this Switch.\n\n", in->name, in->address);
        printf("Its fingerprint: " C_YEL "%s" C_RST "\n", fp);
        printf("Check that the PC shows the same one.\n\n");
        printf(" A  Approve      B  Reject\n");
        consoleUpdate(NULL);
        if (confirm()) {
            if (os_server_approve(&g_srv, id, err, sizeof err) != 0) snprintf(g_banner, sizeof g_banner, "%s", err);
            else snprintf(g_banner, sizeof g_banner, "Paired.");
        } else {
            os_server_reject(&g_srv, id);
        }
        g_dirty = 1;
    }
}

/* Address entry: four numbers and a port, edited with the D-pad. */
static int g_ip[4] = {192, 168, 1, 2};
static int g_port = OS_DEFAULT_PORT, g_cursor;

static void draw_pair(void) {
    int i;
    consoleClear();
    printf(C_CYN "Pair with a PC" C_RST "\n\n");
    printf("Enter the PC's address (shown in OpenSave on the PC, under Devices).\n\n   ");
    for (i = 0; i < 4; i++) {
        printf("%s%3d%s%s", g_cursor == i ? C_YEL "[" : " ", g_ip[i], g_cursor == i ? "]" C_RST : " ", i < 3 ? "." : "");
    }
    printf(" :%s%5d%s\n\n", g_cursor == 4 ? C_YEL "[" : " ", g_port, g_cursor == 4 ? "]" C_RST : " ");
    printf(" Left/Right  choose a part      Up/Down  +1 / -1\n");
    printf(" L / R       -10 / +10          A        send request\n");
    printf(" B           back\n");
    consoleUpdate(NULL);
}

static int pair_input(u64 down) {
    int *v, hi, lo;
    if (down & HidNpadButton_Left) g_cursor = (g_cursor + 4) % 5;
    if (down & HidNpadButton_Right) g_cursor = (g_cursor + 1) % 5;
    v = g_cursor < 4 ? &g_ip[g_cursor] : &g_port;
    hi = g_cursor < 4 ? 255 : 65535;
    lo = g_cursor < 4 ? 0 : 1;
    if (down & HidNpadButton_Up) *v += 1;
    if (down & HidNpadButton_Down) *v -= 1;
    if (down & HidNpadButton_R) *v += g_cursor < 4 ? 10 : 100;
    if (down & HidNpadButton_L) *v -= g_cursor < 4 ? 10 : 100;
    if (*v > hi) *v = lo;
    if (*v < lo) *v = hi;
    return down != 0;
}

/* Sends the request and waits for the PC to approve it. */
static void do_pair(void) {
    char addr[48], err[300];
    os_ping_info info;
    time_t end;
    os_peer *p;

    snprintf(addr, sizeof addr, "%d.%d.%d.%d", g_ip[0], g_ip[1], g_ip[2], g_ip[3]);
    consoleClear();
    printf("Contacting %s:%d ...\n", addr, g_port);
    consoleUpdate(NULL);
    if (os_peer_ping(&g_st, addr, g_port, &info, err, sizeof err) != 0) {
        printf(C_RED "\nCould not reach OpenSave there:\n%s\n" C_RST, err);
        wait_for_a();
        return;
    }
    printf("Found \"%s\" (OpenSave %s).\n", info.device_name, info.version);
    consoleUpdate(NULL);
    g_paired_event = 0;
    if (os_peer_handshake(&g_st, addr, g_port, err, sizeof err) != 0) {
        printf(C_RED "\nThe pairing request failed:\n%s\n" C_RST, err);
        wait_for_a();
        return;
    }
    printf("\nRequest sent. " C_YEL "Approve this Switch in OpenSave on the PC" C_RST "\n(Devices tab). Waiting...  (B cancels)\n");
    consoleUpdate(NULL);
    end = time(NULL) + 120;
    while (appletMainLoop() && !g_paired_event && time(NULL) < end) {
        padUpdate(&g_pad);
        if (padGetButtonsDown(&g_pad) & HidNpadButton_B) break;
        os_server_poll(&g_srv, 100);
        consoleUpdate(NULL);
    }
    p = the_peer();
    if (g_paired_event && p) {
        char fp[OS_FINGERPRINT_LEN];
        os_fingerprint(fp, g_st.pub, p->pubkey);
        consoleClear();
        printf(C_GRN "Paired with %s." C_RST "\n\n", p->name);
        printf("Fingerprint: " C_YEL "%s" C_RST "\n\n", fp);
        printf("The PC shows the same code for this pairing. If the two codes\n");
        printf("differ, someone on the network is interfering: unpair on both.\n");
        wait_for_a();
        snprintf(g_banner, sizeof g_banner, "Paired with %s.", p->name);
    } else {
        printf("\nNo approval arrived.\n");
        wait_for_a();
    }
}

static void do_unpair(void) {
    os_peer *p = the_peer();
    char err[200];
    if (!p) return;
    consoleClear();
    printf("Unpair from %s?\n\n A  Unpair    B  Cancel\n", p->name);
    consoleUpdate(NULL);
    if (!confirm()) return;
    if (os_peer_unpair(&g_st, p, err, sizeof err) != 0) printf("(the PC could not be told: %s)\n", err);
    os_state_remove_peer(&g_st, p->id);
    os_state_save(&g_st, err, sizeof err);
    snprintf(g_banner, sizeof g_banner, "Unpaired.");
}

/* ------------------------------------------------------------------- games */

static int load_games(void) {
    free(g_titles);
    g_titles = NULL;
    g_ntitles = 0;
    consoleClear();
    printf("Reading the installed games...\n");
    consoleUpdate(NULL);
    if (saves_titles(&g_titles, &g_ntitles) != 0) {
        printf(C_RED "Could not list the installed games.\n" C_RST);
        wait_for_a();
        return -1;
    }
    g_sel = g_top = 0;
    return 0;
}

static void draw_games(void) {
    int i;
    consoleClear();
    printf(C_CYN "Games" C_RST "   user: %s\n", g_nusers ? g_users[g_user].nickname : "(none)");
    printf("------------------------------------------------------------\n");
    if (g_ntitles == 0) printf("No installed games with save data.\n");
    for (i = g_top; i < g_ntitles && i < g_top + PAGE_ROWS; i++)
        printf("%s %-.52s\n", i == g_sel ? C_YEL ">" : " ", g_titles[i].name);
    printf(C_RST "------------------------------------------------------------\n");
    printf(" A  open    ZL/ZR  change user    B  back\n");
    consoleUpdate(NULL);
}

typedef struct {
    const char *stage;
    int last_pct;
    char last_stage[64];
} prog_t;

static void on_progress(void *ctx, const char *stage, int64_t done, int64_t total) {
    prog_t *p = (prog_t *)ctx;
    int pct = total > 0 ? (int)(done * 100 / total) : -1;
    if (strcmp(p->last_stage, stage) == 0 && pct == p->last_pct) return;
    snprintf(p->last_stage, sizeof p->last_stage, "%s", stage);
    p->last_pct = pct;
    if (pct >= 0) printf("\r%-48.48s %3d%%", stage, pct);
    else printf("\r%-48.48s     ", stage);
    consoleUpdate(NULL);
}

static int on_cancel(void *ctx) {
    (void)ctx;
    padUpdate(&g_pad);
    return (padGetButtons(&g_pad) & HidNpadButton_B) != 0;
}

/* The newest backup folder for a title, or "" if there is none. */
static void newest_backup(const char *tid, char *out, size_t n) {
    char dir[300];
    DIR *d;
    struct dirent *e;
    out[0] = '\0';
    snprintf(dir, sizeof dir, "%s/%s", BACKUP_ROOT, tid);
    d = opendir(dir);
    if (!d) return;
    while ((e = readdir(d)) != NULL) {
        char cand[400];
        if (e->d_name[0] == '.') continue;
        snprintf(cand, sizeof cand, "%s/%s", dir, e->d_name);
        /* Folder names are timestamps, so the greatest sorts newest. */
        if (!out[0] || strcmp(cand, out) > 0) snprintf(out, n, "%s", cand);
    }
    closedir(d);
}

/* Keeps the newest KEEP_BACKUPS backups of a title and removes the rest. */
static void prune_backups(const char *tid) {
    char dir[300];
    for (;;) {
        DIR *d;
        struct dirent *e;
        int count = 0;
        char oldest[400] = "";
        snprintf(dir, sizeof dir, "%s/%s", BACKUP_ROOT, tid);
        d = opendir(dir);
        if (!d) return;
        while ((e = readdir(d)) != NULL) {
            char cand[400];
            if (e->d_name[0] == '.') continue;
            snprintf(cand, sizeof cand, "%s/%s", dir, e->d_name);
            count++;
            if (!oldest[0] || strcmp(cand, oldest) < 0) snprintf(oldest, sizeof oldest, "%s", cand);
        }
        closedir(d);
        if (count <= KEEP_BACKUPS || !oldest[0]) return;
        os_rm_rf(oldest);
    }
}

static void do_pull(const saves_title *t, const char *game_id, const char *name, const char *save_path) {
    char backup[400], staging[300], err[400], root[16], stamp[32];
    os_pull_result res;
    os_progress pr;
    prog_t prog;
    time_t now = time(NULL);
    struct tm tmv;
    os_peer *p = the_peer();

    if (!p) return;
    gmtime_r(&now, &tmv);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tmv);
    snprintf(backup, sizeof backup, "%s/%s/%s", BACKUP_ROOT, t->tid, stamp);
    snprintf(staging, sizeof staging, "%s/%s", STAGING_ROOT, t->tid);
    snprintf(root, sizeof root, "%s", SAVES_MOUNT_ROOT);

    consoleClear();
    printf(C_CYN "Receiving %s" C_RST "\n(hold B to cancel)\n\n", name);
    consoleUpdate(NULL);
    memset(&prog, 0, sizeof prog);
    prog.last_pct = -2;
    pr.ctx = &prog;
    pr.progress = on_progress;
    pr.cancelled = on_cancel;

    if (os_pull(&g_st, p, game_id, name, save_path, root, backup, staging, &pr, &res, err, sizeof err) != 0) {
        /* Nothing is committed: the save is as it was. */
        saves_unmount();
        printf(C_RED "\n\nNot changed.\n%s\n" C_RST, err);
        wait_for_a();
        return;
    }
    if (res.already_same) {
        printf(C_GRN "\n\nAlready identical to the PC's. Nothing to do.\n" C_RST);
        wait_for_a();
        return;
    }
    if (saves_commit(err, sizeof err) != 0) {
        saves_unmount();
        printf(C_RED "\n\n%s\n" C_RST, err);
        wait_for_a();
        return;
    }
    prune_backups(t->tid);
    printf(C_GRN "\n\nDone: %d file(s) received, %d removed.\n" C_RST, res.files_downloaded, res.files_deleted);
    if (res.backup_path[0]) printf("The previous save is backed up at:\n  %s\n", res.backup_path);
    if (res.remote_has_extra_roots)
        printf(C_YEL "The PC tracks extra save folders for this game; only the main one was received.\n" C_RST);
    consoleUpdate(NULL);
    if (os_report_in_sync(&g_st, p, game_id, res.manifest_hash, err, sizeof err) != 0)
        printf("(could not tell the PC: %s)\n", err);
    else
        linger(5); /* the PC calls back to confirm */
    wait_for_a();
}

static void do_restore(const saves_title *t) {
    char backup[400], err[300];
    newest_backup(t->tid, backup, sizeof backup);
    consoleClear();
    if (!backup[0]) {
        printf("There is no backup of this game's save.\n");
        wait_for_a();
        return;
    }
    printf(C_CYN "Restore the previous save?" C_RST "\n\nThis replaces the Switch's current save with:\n  %s\n\n A  Restore    B  Cancel\n", backup);
    consoleUpdate(NULL);
    if (!confirm()) return;
    if (os_restore_backup(SAVES_MOUNT_ROOT, backup, err, sizeof err) != 0) {
        saves_unmount();
        printf(C_RED "\n%s\n" C_RST, err);
        wait_for_a();
        return;
    }
    if (saves_commit(err, sizeof err) != 0) {
        saves_unmount();
        printf(C_RED "\n%s\n" C_RST, err);
        wait_for_a();
        return;
    }
    printf(C_GRN "\nRestored.\n" C_RST);
    wait_for_a();
}

static void do_push(const char *game_id) {
    char err[300];
    os_peer *p = the_peer();
    time_t end;
    if (!p) return;
    consoleClear();
    if (os_peer_trigger_sync(&g_st, p, game_id, err, sizeof err) != 0) {
        printf(C_RED "The PC could not be asked to sync:\n%s\n" C_RST, err);
        wait_for_a();
        return;
    }
    printf(C_GRN "Asked the PC to take this Switch's save.\n" C_RST);
    printf("\nThe PC now reads the save from this Switch. Keep this screen\n");
    printf("open until it finishes. If both sides changed, OpenSave on the\n");
    printf("PC asks which to keep. (B to stop waiting)\n\n");
    consoleUpdate(NULL);
    g_served[0] = '\0';
    end = time(NULL) + 90;
    while (appletMainLoop() && time(NULL) < end) {
        padUpdate(&g_pad);
        if (padGetButtonsDown(&g_pad) & HidNpadButton_B) break;
        if (os_server_poll(&g_srv, 100) == 1 && g_served[0]) {
            printf("\r%-60.60s", g_served);
            consoleUpdate(NULL);
            end = time(NULL) + 15; /* it is working: allow it time to finish */
        }
        consoleUpdate(NULL);
    }
    printf("\n");
}

static const char *cmp_text(const os_cmp_result *r, char *buf, size_t n) {
    switch (r->state) {
    case OS_CMP_SAME: return C_GRN "Identical to the PC's save." C_RST;
    case OS_CMP_DIFFERENT:
        snprintf(buf, n, C_YEL "Different: %d only on the PC, %d only here, %d changed." C_RST, r->only_remote,
                 r->only_local, r->differ);
        return buf;
    case OS_CMP_PC_LACKS:
        snprintf(buf, n, C_YEL "The PC does not have this game yet.\n  %s\n  It has been offered to OpenSave on the PC: place it there first." C_RST,
                 r->message);
        return buf;
    default: snprintf(buf, n, C_RED "Could not compare: %s" C_RST, r->message); return buf;
    }
}

static void game_screen(const saves_title *t) {
    char err[300], game_id[128], name[128], path[160], txt[600];
    os_cmp_result cmp;
    os_remote_game rg;
    os_peer *p = the_peer();
    int found = 0, refresh = 1;

    if (!p) return;
    for (;;) {
        u64 d;
        if (refresh) {
            consoleClear();
            printf(C_CYN "%s" C_RST "\n%s\n\n", t->name, t->tid);
            printf("Opening the save...\n");
            consoleUpdate(NULL);
            if (saves_mount(t, &g_users[g_user], err, sizeof err) != 0) {
                printf(C_RED "\n%s\n" C_RST, err);
                wait_for_a();
                return;
            }
            printf("Asking the PC...\n");
            consoleUpdate(NULL);
            os_game_id_for_title(game_id, sizeof game_id, t->tid);
            snprintf(name, sizeof name, "%s", t->name);
            os_save_path_for_title(path, sizeof path, t->tid);
            if (os_peer_find_title(&g_st, p, t->tid, &rg, &found, err, sizeof err) != 0) {
                printf(C_RED "\nCould not reach the PC:\n%s\n" C_RST, err);
                wait_for_a();
                return;
            }
            if (found == 2) {
                printf(C_YEL "\nThe PC tracks this game more than once. Link the right one\nin OpenSave on the PC (the game's Manage tab), then try again.\n" C_RST);
                wait_for_a();
                return;
            }
            if (found == 1) snprintf(game_id, sizeof game_id, "%s", rg.id); /* use the PC's own id */
            os_compare(&g_st, p, game_id, name, path, SAVES_MOUNT_ROOT, &cmp);
            consoleClear();
            printf(C_CYN "%s" C_RST "\n%s   PC: %s\n\n", t->name, t->tid, p->name);
            printf("%s\n\n", cmp_text(&cmp, txt, sizeof txt));
            if (cmp.remote_has_extra_roots)
                printf(C_YEL "The PC keeps extra save folders for this game; only the main one is synced.\n\n" C_RST);
            if (cmp.state == OS_CMP_DIFFERENT || cmp.state == OS_CMP_SAME) {
                printf(" A  Receive: replace this Switch's save with the PC's\n");
                printf("      (the current save is backed up first)\n");
                printf(" X  Send: give the PC this Switch's save\n");
            }
            printf(" Y  Restore the save from the last backup\n");
            printf(" R  Check again          B  Back\n");
            consoleUpdate(NULL);
            refresh = 0;
        }
        padUpdate(&g_pad);
        os_server_poll(&g_srv, 0);
        d = padGetButtonsDown(&g_pad);
        if (d & HidNpadButton_B) return;
        if (d & HidNpadButton_R) refresh = 1;
        if ((d & HidNpadButton_A) && (cmp.state == OS_CMP_DIFFERENT || cmp.state == OS_CMP_SAME)) {
            consoleClear();
            printf(C_RED "Replace this Switch's save for\n%s\nwith the PC's?" C_RST "\n\n", t->name);
            printf("The current save is backed up on the SD card first.\n\n A  Replace    B  Cancel\n");
            consoleUpdate(NULL);
            if (confirm()) do_pull(t, game_id, name, path);
            refresh = 1;
        }
        if ((d & HidNpadButton_X) && (cmp.state == OS_CMP_DIFFERENT || cmp.state == OS_CMP_SAME)) {
            do_push(game_id);
            refresh = 1;
        }
        if (d & HidNpadButton_Y) {
            do_restore(t);
            refresh = 1;
        }
        consoleUpdate(NULL);
    }
}

/* -------------------------------------------------------------------- main */

int main(void) {
    screen_t scr = SCR_MAIN;
    char err[300];
    os_server_hooks hooks;

    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);

    if (R_FAILED(socketInitializeDefault())) {
        printf(C_RED "Could not start networking.\n" C_RST);
        goto fail;
    }
    if (saves_init() != 0) {
        printf(C_RED "Could not start the game and account services.\n" C_RST);
        goto fail;
    }
    g_nusers = saves_users(g_users, ACC_USER_LIST_SIZE);
    {
        AccountUid pre;
        int i;
        if (R_SUCCEEDED(accountGetPreselectedUser(&pre)))
            for (i = 0; i < g_nusers; i++)
                if (memcmp(&g_users[i].uid, &pre, sizeof pre) == 0) g_user = i;
    }
    if (os_state_load(&g_st, CONFIG_PATH, err, sizeof err) != 0) {
        printf(C_RED "Settings problem: %s\n" C_RST, err);
        goto fail;
    }
    memset(&hooks, 0, sizeof hooks);
    hooks.open_save = hook_open_save;
    hooks.on_pairing_request = hook_pairing_request;
    hooks.on_paired = hook_paired;
    hooks.on_unpaired = hook_unpaired;
    hooks.on_peer_update = hook_peer_update;
    hooks.on_served = hook_served;
    if (os_server_start(&g_srv, &g_st, &hooks, g_st.port, err, sizeof err) != 0) {
        printf(C_RED "Could not listen on port %d: %s\n" C_RST, g_st.port, err);
        goto fail;
    }
    load_games();

    while (appletMainLoop()) {
        u64 down;
        padUpdate(&g_pad);
        down = padGetButtonsDown(&g_pad);
        os_server_poll(&g_srv, 0);
        handle_incoming();

        switch (scr) {
        case SCR_MAIN:
            if (g_dirty) {
                draw_main();
                g_dirty = 0;
            }
            if (down & HidNpadButton_Plus) goto done;
            if (down & HidNpadButton_A) {
                scr = the_peer() ? SCR_GAMES : SCR_PAIR;
                g_dirty = 1;
            }
            if ((down & HidNpadButton_Y) && the_peer()) {
                do_unpair();
                scr = SCR_PAIR; /* "pair with a different PC" */
                g_dirty = 1;
            }
            break;
        case SCR_PAIR:
            if (g_dirty) {
                draw_pair();
                g_dirty = 0;
            }
            if (down & HidNpadButton_B) {
                scr = SCR_MAIN;
                g_dirty = 1;
            } else if (down & HidNpadButton_A) {
                do_pair();
                scr = SCR_MAIN;
                g_dirty = 1;
            } else if (pair_input(down)) {
                g_dirty = 1;
            }
            break;
        case SCR_GAMES:
            if (g_dirty) {
                draw_games();
                g_dirty = 0;
            }
            if (down & HidNpadButton_B) {
                saves_unmount();
                scr = SCR_MAIN;
                g_dirty = 1;
            }
            if ((down & HidNpadButton_Down) && g_sel + 1 < g_ntitles) {
                g_sel++;
                if (g_sel >= g_top + PAGE_ROWS) g_top = g_sel - PAGE_ROWS + 1;
                g_dirty = 1;
            }
            if ((down & HidNpadButton_Up) && g_sel > 0) {
                g_sel--;
                if (g_sel < g_top) g_top = g_sel;
                g_dirty = 1;
            }
            if (down & (HidNpadButton_ZL | HidNpadButton_ZR)) {
                if (g_nusers > 1) g_user = (g_user + 1) % g_nusers;
                g_dirty = 1;
            }
            if ((down & HidNpadButton_A) && g_ntitles > 0) {
                game_screen(&g_titles[g_sel]);
                saves_unmount();
                g_dirty = 1;
            }
            break;
        default: scr = SCR_MAIN; break;
        }
        consoleUpdate(NULL);
    }

done:
    os_server_stop(&g_srv);
    saves_exit();
    socketExit();
    free(g_titles);
    consoleExit(NULL);
    return 0;

fail:
    printf("\nPress + to exit.\n");
    while (appletMainLoop()) {
        padUpdate(&g_pad);
        if (padGetButtonsDown(&g_pad) & HidNpadButton_Plus) break;
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
    return 1;
}
