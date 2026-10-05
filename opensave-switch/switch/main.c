/* OpenSave for Nintendo Switch.
 *
 * Pair with the OpenSave app on a PC, then take a game's save from the PC onto
 * this Switch, or send this Switch's save to the PC.
 *
 * The interface is drawn straight into the screen's framebuffer by the portable
 * renderer in ../ui (the same code the PC tests render screenshots with); the
 * work is done by the portable core in ../core.
 *
 * Needs custom firmware (Atmosphere): only homebrew with full file system
 * access can open the save data of other games.
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
#include "../ui/ui.h"
#include "saves.h"

#define CONFIG_PATH "sdmc:/config/opensave/state.json"
#define BACKUP_ROOT "sdmc:/switch/OpenSave/backups"
#define STAGING_ROOT "sdmc:/switch/OpenSave/staging"
#define KEEP_BACKUPS 3
#define APP_VERSION "OpenSave for Switch 0.2.0"

/* ------------------------------------------------------------------ state */

static os_state g_st;
static os_server g_srv;
static PadState g_pad;
static Framebuffer g_fb;
static gfx g_gfx;
static int g_frame;

static char g_banner[300];
static ui_kind g_banner_kind = UI_INFO;
static int g_paired_event;
static char g_served[80];

static saves_user g_users[ACC_USER_LIST_SIZE];
static int g_nusers, g_user;
static saves_title *g_titles;
static ui_game_row *g_rows;
static int g_ntitles;

/* --------------------------------------------------------------- drawing */

typedef void (*draw_fn)(gfx *g, void *arg);

/* Draws one frame and shows it; this also paces the loop to the display. */
static void frame(draw_fn fn, void *arg) {
    u32 stride;
    u32 *px = (u32 *)framebufferBegin(&g_fb, &stride);
    gfx_init(&g_gfx, px, UI_W, UI_H, (int)(stride / 4));
    fn(&g_gfx, arg);
    framebufferEnd(&g_fb);
    g_frame++;
}

static os_peer *the_peer(void) {
    int i;
    for (i = 0; i < OS_MAX_PEERS; i++)
        if (g_st.peers[i].in_use) return &g_st.peers[i];
    return NULL;
}

static void set_banner(ui_kind k, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_banner(ui_kind k, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_banner, sizeof g_banner, fmt, ap);
    va_end(ap);
    g_banner_kind = k;
}

static const char *local_ip(char *buf, size_t n) {
    struct in_addr a;
    a.s_addr = (in_addr_t)gethostid();
    if (a.s_addr == 0 || a.s_addr == htonl(INADDR_LOOPBACK)) snprintf(buf, n, "not connected");
    else snprintf(buf, n, "%s", inet_ntoa(a));
    return buf;
}

/* A one-shot busy frame, for work that blocks (the spinner holds still). */
typedef struct {
    const char *title, *sub;
    const ui_hint *hints;
    int nhints;
} busy_t;
static void d_busy(gfx *g, void *a) {
    busy_t *b = (busy_t *)a;
    ui_busy(g, b->title, b->sub, b->hints, b->nhints, g_frame);
}
static void show_busy(const char *title, const char *sub) {
    busy_t b = {title, sub, NULL, 0};
    frame(d_busy, &b);
}

/* ------------------------------------------------------- results, dialogs */

typedef struct {
    ui_kind kind;
    const char *title, *msg;
} result_t;
static void d_result(gfx *g, void *a) {
    result_t *r = (result_t *)a;
    ui_result(g, r->kind, r->title, r->msg, NULL, 0);
}

/* Shows an outcome until A or B is pressed. */
static void show_result(ui_kind kind, const char *title, const char *msg) {
    result_t r = {kind, title, msg};
    while (appletMainLoop()) {
        padUpdate(&g_pad);
        if (padGetButtonsDown(&g_pad) & (HidNpadButton_A | HidNpadButton_B)) return;
        os_server_poll(&g_srv, 0);
        frame(d_result, &r);
    }
}

typedef struct {
    draw_fn under;
    void *under_arg;
    ui_kind kind;
    const char *title, *msg, *code, *yes, *no;
} dialog_t;
static void d_dialog(gfx *g, void *a) {
    dialog_t *d = (dialog_t *)a;
    if (d->under) d->under(g, d->under_arg);
    ui_dialog(g, d->kind, d->title, d->msg, d->code, d->yes, d->no);
}

/* A confirmation over what was on screen: A says yes, B says no. */
static int confirm(draw_fn under, void *under_arg, ui_kind kind, const char *title, const char *msg, const char *code,
                   const char *yes, const char *no) {
    dialog_t d = {under, under_arg, kind, title, msg, code, yes, no};
    while (appletMainLoop()) {
        u64 down;
        padUpdate(&g_pad);
        down = padGetButtonsDown(&g_pad);
        if (down & HidNpadButton_A) return 1;
        if (down & HidNpadButton_B) return 0;
        os_server_poll(&g_srv, 0);
        frame(d_dialog, &d);
    }
    return 0;
}

/* Serves the PC for a few seconds behind a spinner, so it can call back. */
static void linger(const char *title, const char *sub, int seconds) {
    time_t end = time(NULL) + seconds;
    busy_t b = {title, sub, NULL, 0};
    while (appletMainLoop() && time(NULL) < end) {
        os_server_poll(&g_srv, 0);
        frame(d_busy, &b);
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
}

static void hook_paired(void *ctx, const os_peer *p) {
    (void)ctx;
    (void)p;
    g_paired_event = 1;
}

static void hook_unpaired(void *ctx, const char *id) {
    (void)ctx;
    (void)id;
    set_banner(UI_WARN, "The PC unpaired this Switch.");
}

static void hook_peer_update(void *ctx, const os_peer *p, const char *game, const char *tid) {
    int i;
    (void)ctx;
    (void)p;
    (void)game;
    for (i = 0; i < g_ntitles; i++)
        if (strcasecmp(g_titles[i].tid, tid) == 0) {
            set_banner(UI_INFO, "The PC has newer progress for %s.", g_titles[i].name);
            return;
        }
    set_banner(UI_INFO, "The PC has newer progress for a game (%s).", tid);
}

static void hook_served(void *ctx, const char *what) {
    (void)ctx;
    snprintf(g_served, sizeof g_served, "%s", what);
}

/* ------------------------------------------------------------------- home */

typedef struct {
    ui_home_t v;
    char address[64], peer_addr[80], fp[OS_FINGERPRINT_LEN], hint[300];
    const char *items[4];
} home_t;

static void d_home(gfx *g, void *a) { ui_home(g, &((home_t *)a)->v); }

static void build_home(home_t *h, int sel) {
    char ip[32];
    os_peer *p = the_peer();
    memset(h, 0, sizeof *h);
    h->v.device_name = g_st.device_name;
    snprintf(h->address, sizeof h->address, "%s : %d", local_ip(ip, sizeof ip), os_server_port(&g_srv));
    h->v.address = h->address;
    h->v.version = APP_VERSION;
    if (p) {
        snprintf(h->peer_addr, sizeof h->peer_addr, "%s : %d", p->address, p->port);
        os_fingerprint(h->fp, g_st.pub, p->pubkey);
        h->v.paired = 1;
        h->v.peer_name = p->name;
        h->v.peer_addr = h->peer_addr;
        h->v.fingerprint = h->fp;
        h->items[0] = "Games";
        h->items[1] = "Pair with a different PC";
        h->items[2] = "Exit";
        h->v.nitems = 3;
    } else {
        h->items[0] = "Pair with a PC";
        h->items[1] = "Exit";
        h->v.nitems = 2;
    }
    h->v.items = h->items;
    h->v.sel = sel;
    if (g_banner[0]) {
        h->v.banner = g_banner;
        h->v.banner_kind = g_banner_kind;
    }
    if (os_clock_hint(&g_st, h->hint, sizeof h->hint)[0]) h->v.clock_warning = h->hint;
}

/* Offers a pairing request somebody sent this Switch. */
static void handle_incoming(void) {
    int i;
    for (i = 0; i < OS_MAX_PENDING; i++) {
        os_incoming *in = &g_st.incoming[i];
        char fp[OS_FINGERPRINT_LEN], id[64], err[200], msg[200];
        home_t h;
        if (!in->in_use) continue;
        if (in->has_key) os_fingerprint(fp, g_st.pub, in->pubkey);
        else snprintf(fp, sizeof fp, "(no key)");
        snprintf(id, sizeof id, "%s", in->peer_id);
        snprintf(msg, sizeof msg, "\"%s\" at %s wants to pair. Check that the PC shows the same code.", in->name,
                 in->address);
        build_home(&h, 0);
        if (confirm(d_home, &h, UI_INFO, "Pairing request", msg, fp, "Approve", "Reject")) {
            if (os_server_approve(&g_srv, id, err, sizeof err) != 0) set_banner(UI_ERR, "%s", err);
            else set_banner(UI_OK, "Paired.");
        } else {
            os_server_reject(&g_srv, id);
        }
    }
}

/* ------------------------------------------------------------------- pair */

static int g_ip[4] = {192, 168, 1, 2};
static int g_port = OS_DEFAULT_PORT, g_cursor;

static ui_pair_t g_pairview;
static void d_pair(gfx *g, void *a) {
    (void)a;
    ui_pair(g, &g_pairview);
}

static void pair_input(u64 down) {
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
}

/* Sends the request and waits for the PC to approve it. */
static void do_pair(void) {
    char addr[48], err[300], sub[120];
    os_ping_info info;
    time_t end;
    os_peer *p;
    static const ui_hint cancel[] = {{"B", "Cancel"}};
    busy_t b;

    snprintf(addr, sizeof addr, "%d.%d.%d.%d", g_ip[0], g_ip[1], g_ip[2], g_ip[3]);
    snprintf(sub, sizeof sub, "%s : %d", addr, g_port);
    show_busy("Contacting the PC", sub);
    if (os_peer_ping(&g_st, addr, g_port, &info, err, sizeof err) != 0) {
        show_result(UI_ERR, "Could not reach OpenSave there", err);
        return;
    }
    g_paired_event = 0;
    if (os_peer_handshake(&g_st, addr, g_port, err, sizeof err) != 0) {
        show_result(UI_ERR, "The pairing request failed", err);
        return;
    }
    b.title = "Waiting for approval";
    b.sub = "Approve this Switch in OpenSave on the PC, under Devices.";
    b.hints = cancel;
    b.nhints = 1;
    end = time(NULL) + 120;
    while (appletMainLoop() && !g_paired_event && time(NULL) < end) {
        padUpdate(&g_pad);
        if (padGetButtonsDown(&g_pad) & HidNpadButton_B) break;
        os_server_poll(&g_srv, 0);
        frame(d_busy, &b);
    }
    p = the_peer();
    if (g_paired_event && p) {
        char fp[OS_FINGERPRINT_LEN], msg[300];
        os_fingerprint(fp, g_st.pub, p->pubkey);
        snprintf(msg, sizeof msg,
                 "Fingerprint  %s\nThe PC shows the same code. If the two differ, someone on the network is interfering: unpair on both.",
                 fp);
        show_result(UI_OK, "Paired", msg);
        set_banner(UI_OK, "Paired with %s.", p->name);
    } else {
        show_result(UI_WARN, "No approval arrived", "Open OpenSave on the PC, check Devices, and try again.");
    }
}

static void do_unpair(void) {
    os_peer *p = the_peer();
    char err[200], msg[160];
    home_t h;
    if (!p) return;
    snprintf(msg, sizeof msg, "This Switch will stop syncing with %s. Your saves are not touched.", p->name);
    build_home(&h, 1);
    if (!confirm(d_home, &h, UI_WARN, "Unpair?", msg, NULL, "Unpair", "Cancel")) return;
    os_peer_unpair(&g_st, p, err, sizeof err); /* best effort: the PC may be off */
    os_state_remove_peer(&g_st, p->id);
    os_state_save(&g_st, err, sizeof err);
    set_banner(UI_INFO, "Unpaired.");
}

/* ------------------------------------------------------------------ games */

static ui_games_t g_gamesview;
static char g_status[64];
static void d_games(gfx *g, void *a) {
    (void)a;
    ui_games(g, &g_gamesview);
}

static void rebuild_rows(void) {
    int i;
    free(g_rows);
    g_rows = (ui_game_row *)calloc((size_t)(g_ntitles ? g_ntitles : 1), sizeof *g_rows);
    for (i = 0; g_rows && i < g_ntitles; i++) {
        g_rows[i].name = g_titles[i].name;
        g_rows[i].tid = g_titles[i].tid;
        g_rows[i].kind = g_titles[i].has_user_save ? "Account save" : "Console save";
    }
}

static void scan_progress(void *ctx, int done, int total) {
    (void)ctx;
    snprintf(g_status, sizeof g_status, "Reading games   %d / %d", done, total);
    g_gamesview.status = g_status;
    g_gamesview.n = 0;
    frame(d_games, NULL);
}

static int load_games(int rescan) {
    free(g_titles);
    g_titles = NULL;
    g_ntitles = 0;
    memset(&g_gamesview, 0, sizeof g_gamesview);
    g_gamesview.user = g_nusers ? g_users[g_user].nickname : NULL;
    g_gamesview.nusers = g_nusers;
    snprintf(g_status, sizeof g_status, "Reading games");
    g_gamesview.status = g_status;
    frame(d_games, NULL);
    if (saves_titles(&g_titles, &g_ntitles, rescan, scan_progress, NULL) != 0) {
        show_result(UI_ERR, "Could not list your games", "The system would not list the installed games.");
        return -1;
    }
    rebuild_rows();
    return 0;
}

typedef struct {
    const char *title, *stage;
    int pct;
} prog_t;
static prog_t g_prog;
static int g_last_pct = -2;
static char g_last_stage[64];

static void d_progress(gfx *g, void *a) {
    prog_t *p = (prog_t *)a;
    ui_progress(g, p->title, p->stage, p->pct, g_frame);
}

static void on_progress(void *ctx, const char *stage, int64_t done, int64_t total) {
    int pct = total > 0 ? (int)(done * 100 / total) : -1;
    (void)ctx;
    /* Redraw when something visible changed; every block is not worth a frame. */
    if (pct == g_last_pct && strcmp(g_last_stage, stage) == 0) return;
    g_last_pct = pct;
    snprintf(g_last_stage, sizeof g_last_stage, "%s", stage);
    g_prog.stage = g_last_stage;
    g_prog.pct = pct;
    frame(d_progress, &g_prog);
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
    char backup[400], staging[300], err[400], stamp[32], head[300], msg[400];
    os_pull_result res;
    os_progress pr;
    time_t now = time(NULL);
    struct tm tmv;
    os_peer *p = the_peer();

    if (!p) return;
    gmtime_r(&now, &tmv);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tmv);
    snprintf(backup, sizeof backup, "%s/%s/%s", BACKUP_ROOT, t->tid, stamp);
    snprintf(staging, sizeof staging, "%s/%s", STAGING_ROOT, t->tid);
    snprintf(head, sizeof head, "Receiving %s", name);

    g_prog.title = head;
    g_prog.stage = "Asking the PC what it has";
    g_prog.pct = -1;
    g_last_pct = -2;
    g_last_stage[0] = '\0';
    frame(d_progress, &g_prog);
    pr.ctx = NULL;
    pr.progress = on_progress;
    pr.cancelled = on_cancel;

    if (os_pull(&g_st, p, game_id, name, save_path, SAVES_MOUNT_ROOT, backup, staging, &pr, &res, err, sizeof err) != 0) {
        saves_unmount(); /* nothing was committed: the save is as it was */
        show_result(UI_ERR, "Nothing was changed", err);
        return;
    }
    if (res.already_same) {
        show_result(UI_OK, "Already identical", "This Switch's save already matches the PC's. Nothing to do.");
        return;
    }
    if (saves_commit(err, sizeof err) != 0) {
        saves_unmount();
        show_result(UI_ERR, "The save was not kept", err);
        return;
    }
    prune_backups(t->tid);
    snprintf(msg, sizeof msg, "%d file%s received, %d removed. The previous save is backed up on the SD card.%s",
             res.files_downloaded, res.files_downloaded == 1 ? "" : "s", res.files_deleted,
             res.remote_has_extra_roots ? " The PC keeps extra save folders for this game; only the main one was received." : "");
    if (os_report_in_sync(&g_st, p, game_id, res.manifest_hash, err, sizeof err) == 0)
        linger("Confirming with the PC", NULL, 4); /* the PC calls back to check */
    show_result(UI_OK, "Saved to this Switch", msg);
}

static void do_restore(const saves_title *t, draw_fn under, void *under_arg) {
    char backup[400], err[300];
    newest_backup(t->tid, backup, sizeof backup);
    if (!backup[0]) {
        show_result(UI_INFO, "No backup yet", "A backup is made each time a save is received from the PC.");
        return;
    }
    if (!confirm(under, under_arg, UI_WARN, "Restore the previous save?",
                 "This replaces the Switch's current save with the one backed up before the last receive.", NULL, "Restore",
                 "Cancel"))
        return;
    show_busy("Restoring", NULL);
    if (os_restore_backup(SAVES_MOUNT_ROOT, backup, err, sizeof err) != 0) {
        saves_unmount();
        show_result(UI_ERR, "Could not restore", err);
        return;
    }
    if (saves_commit(err, sizeof err) != 0) {
        saves_unmount();
        show_result(UI_ERR, "The save was not kept", err);
        return;
    }
    show_result(UI_OK, "Restored", "The previous save is back.");
}

static void do_push(const char *game_id) {
    char err[300], sub[160];
    os_peer *p = the_peer();
    time_t end;
    int served = 0;
    static const ui_hint stop[] = {{"B", "Stop waiting"}};
    busy_t b;

    if (!p) return;
    show_busy("Asking the PC", NULL);
    if (os_peer_trigger_sync(&g_st, p, game_id, err, sizeof err) != 0) {
        show_result(UI_ERR, "The PC could not be asked to sync", err);
        return;
    }
    snprintf(sub, sizeof sub, "The PC is reading this Switch's save. Keep this screen open.");
    b.title = "Sending to the PC";
    b.sub = sub;
    b.hints = stop;
    b.nhints = 1;
    g_served[0] = '\0';
    end = time(NULL) + 90;
    while (appletMainLoop() && time(NULL) < end) {
        padUpdate(&g_pad);
        if (padGetButtonsDown(&g_pad) & HidNpadButton_B) break;
        if (os_server_poll(&g_srv, 0) == 1 && g_served[0]) {
            served = 1;
            snprintf(sub, sizeof sub, "%s...", g_served);
            end = time(NULL) + 15; /* it is working: allow it time to finish */
        }
        frame(d_busy, &b);
    }
    show_result(served ? UI_OK : UI_INFO, served ? "The PC read this save" : "The PC was asked",
                served ? "If both sides had changed, OpenSave on the PC asks which to keep."
                       : "Nothing was requested from this Switch yet. Check OpenSave on the PC.");
}

typedef struct {
    ui_game_t v;
    char status[80], detail[300];
    const char *actions[4], *help[4];
    int enabled[4];
} game_t;
static void d_game(gfx *g, void *a) { ui_game(g, &((game_t *)a)->v); }

enum { ACT_RECEIVE, ACT_SEND, ACT_RESTORE, ACT_AGAIN };

static void game_screen(const saves_title *t) {
    char err[300], game_id[128], name[128], path[160], backup[400];
    os_cmp_result cmp;
    os_remote_game rg;
    os_peer *p = the_peer();
    game_t gv;
    int found = 0, refresh = 1, sel = 0, i, actions[4], nact;

    if (!p) return;
    memset(&gv, 0, sizeof gv);
    for (;;) {
        u64 d;
        if (refresh) {
            show_busy("Checking with the PC", t->name);
            if (saves_mount(t, &g_users[g_user], err, sizeof err) != 0) {
                show_result(UI_ERR, "Could not open this game's save", err);
                return;
            }
            os_game_id_for_title(game_id, sizeof game_id, t->tid);
            snprintf(name, sizeof name, "%s", t->name);
            os_save_path_for_title(path, sizeof path, t->tid);
            if (os_peer_find_title(&g_st, p, t->tid, &rg, &found, err, sizeof err) != 0) {
                show_result(UI_ERR, "Could not reach the PC", err);
                return;
            }
            if (found == 2) {
                show_result(UI_WARN, "The PC tracks this game twice",
                            "Link the right one in OpenSave on the PC (the game's Manage tab), then try again.");
                return;
            }
            if (found == 1) snprintf(game_id, sizeof game_id, "%s", rg.id); /* the PC's own id */
            os_compare(&g_st, p, game_id, name, path, SAVES_MOUNT_ROOT, &cmp);
            newest_backup(t->tid, backup, sizeof backup);

            memset(&gv, 0, sizeof gv);
            gv.v.title = t->name;
            gv.v.tid = t->tid;
            gv.v.peer_name = p->name;
            gv.v.only_pc = gv.v.only_here = gv.v.changed = -1;
            switch (cmp.state) {
            case OS_CMP_SAME:
                gv.v.status_kind = UI_OK;
                snprintf(gv.status, sizeof gv.status, "Identical to the PC's save");
                break;
            case OS_CMP_DIFFERENT:
                gv.v.status_kind = UI_WARN;
                snprintf(gv.status, sizeof gv.status, "The saves are different");
                gv.v.only_pc = cmp.only_remote;
                gv.v.only_here = cmp.only_local;
                gv.v.changed = cmp.differ;
                break;
            case OS_CMP_PC_LACKS:
                gv.v.status_kind = UI_WARN;
                snprintf(gv.status, sizeof gv.status, "The PC does not have this game yet");
                snprintf(gv.detail, sizeof gv.detail,
                         "It was offered to OpenSave on the PC: choose its save folder there, then check again.");
                break;
            default:
                gv.v.status_kind = UI_ERR;
                snprintf(gv.status, sizeof gv.status, "Could not compare");
                snprintf(gv.detail, sizeof gv.detail, "%s", cmp.message);
            }
            gv.v.status = gv.status;
            if (gv.detail[0]) gv.v.detail = gv.detail;
            else if (cmp.remote_has_extra_roots) {
                snprintf(gv.detail, sizeof gv.detail, "The PC keeps extra save folders for this game; only the main one syncs.");
                gv.v.detail = gv.detail;
            }

            /* Only what can be done is offered. */
            nact = 0;
            if (cmp.state == OS_CMP_DIFFERENT) {
                actions[nact++] = ACT_RECEIVE;
                actions[nact++] = ACT_SEND;
            }
            if (backup[0]) actions[nact++] = ACT_RESTORE;
            actions[nact++] = ACT_AGAIN;
            for (i = 0; i < nact; i++) {
                gv.enabled[i] = 1;
                switch (actions[i]) {
                case ACT_RECEIVE:
                    gv.actions[i] = "Receive from the PC";
                    gv.help[i] = "Replace this Switch's save with the PC's. The current one is backed up first.";
                    break;
                case ACT_SEND:
                    gv.actions[i] = "Send to the PC";
                    gv.help[i] = "Ask the PC to take this Switch's save.";
                    break;
                case ACT_RESTORE:
                    gv.actions[i] = "Restore the last backup";
                    gv.help[i] = "Put back the save from before the last receive.";
                    break;
                default:
                    gv.actions[i] = "Check again";
                    gv.help[i] = "Compare with the PC once more.";
                }
            }
            gv.v.actions = gv.actions;
            gv.v.action_help = gv.help;
            gv.v.action_enabled = gv.enabled;
            gv.v.nactions = nact;
            if (sel >= nact) sel = 0;
            refresh = 0;
        }
        gv.v.sel = sel;
        padUpdate(&g_pad);
        os_server_poll(&g_srv, 0);
        d = padGetButtonsDown(&g_pad);
        if (d & HidNpadButton_B) return;
        if ((d & HidNpadButton_Down) && sel + 1 < gv.v.nactions) sel++;
        if ((d & HidNpadButton_Up) && sel > 0) sel--;
        if (d & HidNpadButton_A) {
            switch (actions[sel]) {
            case ACT_RECEIVE:
                if (confirm(d_game, &gv, UI_WARN, "Replace this Switch's save?",
                            "The current save is backed up on the SD card first, so you can put it back.", NULL, "Replace",
                            "Cancel"))
                    do_pull(t, game_id, name, path);
                break;
            case ACT_SEND: do_push(game_id); break;
            case ACT_RESTORE: do_restore(t, d_game, &gv); break;
            default: break;
            }
            refresh = 1;
        }
        frame(d_game, &gv);
    }
}

/* ------------------------------------------------------------------- main */

typedef enum { SCR_HOME, SCR_PAIR, SCR_GAMES } screen_t;

int main(void) {
    screen_t scr = SCR_HOME;
    char err[300];
    os_server_hooks hooks;
    home_t home;
    int home_sel = 0;
    NWindow *win;

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    win = nwindowGetDefault();
    framebufferCreate(&g_fb, win, UI_W, UI_H, PIXEL_FORMAT_RGBA_8888, 2);
    framebufferMakeLinear(&g_fb);

    if (R_FAILED(socketInitializeDefault())) {
        show_result(UI_ERR, "Could not start networking", "Check the Wi-Fi settings and start OpenSave again.");
        goto done;
    }
    if (saves_init() != 0) {
        show_result(UI_ERR, "Could not start the game services", "OpenSave needs to run with full access (Atmosphere).");
        goto done;
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
        show_result(UI_ERR, "Settings problem", err);
        goto done;
    }
    memset(&hooks, 0, sizeof hooks);
    hooks.open_save = hook_open_save;
    hooks.on_pairing_request = hook_pairing_request;
    hooks.on_paired = hook_paired;
    hooks.on_unpaired = hook_unpaired;
    hooks.on_peer_update = hook_peer_update;
    hooks.on_served = hook_served;
    if (os_server_start(&g_srv, &g_st, &hooks, g_st.port, err, sizeof err) != 0) {
        char msg[320];
        snprintf(msg, sizeof msg, "Port %d: %s", g_st.port, err);
        show_result(UI_ERR, "Could not start listening", msg);
        goto done;
    }
    load_games(0);

    while (appletMainLoop()) {
        u64 down;
        padUpdate(&g_pad);
        down = padGetButtonsDown(&g_pad);
        os_server_poll(&g_srv, 0);
        handle_incoming();

        switch (scr) {
        case SCR_HOME: {
            int paired = the_peer() != NULL, n = paired ? 3 : 2;
            if (home_sel >= n) home_sel = 0;
            if ((down & HidNpadButton_Down) && home_sel + 1 < n) home_sel++;
            if ((down & HidNpadButton_Up) && home_sel > 0) home_sel--;
            if (down & HidNpadButton_Plus) goto done;
            if (down & HidNpadButton_A) {
                if (paired && home_sel == 0) {
                    scr = SCR_GAMES;
                    memset(&g_gamesview, 0, sizeof g_gamesview);
                } else if (paired && home_sel == 1) {
                    do_unpair();
                    if (!the_peer()) home_sel = 0;
                } else if (!paired && home_sel == 0) {
                    scr = SCR_PAIR;
                } else {
                    goto done; /* Exit */
                }
            }
            build_home(&home, home_sel);
            frame(d_home, &home);
            break;
        }
        case SCR_PAIR:
            if (down & HidNpadButton_B) scr = SCR_HOME;
            else if (down & HidNpadButton_A) {
                do_pair();
                scr = SCR_HOME;
            } else {
                pair_input(down);
            }
            g_pairview.cursor = g_cursor;
            memcpy(g_pairview.ip, g_ip, sizeof g_ip);
            g_pairview.port = g_port;
            frame(d_pair, NULL);
            break;
        case SCR_GAMES: {
            static int sel, top;
            int vis = ui_games_visible();
            if (sel >= g_ntitles) sel = g_ntitles ? g_ntitles - 1 : 0;
            if (top > sel) top = sel;
            if (down & HidNpadButton_B) {
                saves_unmount();
                scr = SCR_HOME;
                break;
            }
            if ((down & HidNpadButton_Down) && sel + 1 < g_ntitles) sel++;
            if ((down & HidNpadButton_Up) && sel > 0) sel--;
            if ((down & HidNpadButton_Right) && sel + vis < g_ntitles) sel += vis; /* page down */
            if ((down & HidNpadButton_Left) && sel > 0) sel = sel - vis < 0 ? 0 : sel - vis;
            if (sel < top) top = sel;
            if (sel >= top + vis) top = sel - vis + 1;
            if (down & (HidNpadButton_ZL | HidNpadButton_ZR)) {
                if (g_nusers > 1) g_user = (g_user + 1) % g_nusers;
                sel = top = 0;
                load_games(0);
            }
            if (down & HidNpadButton_R) {
                sel = top = 0;
                load_games(1);
            }
            memset(&g_gamesview, 0, sizeof g_gamesview);
            g_gamesview.rows = g_rows;
            g_gamesview.n = g_ntitles;
            g_gamesview.sel = sel;
            g_gamesview.top = top;
            g_gamesview.user = g_nusers ? g_users[g_user].nickname : NULL;
            g_gamesview.nusers = g_nusers;
            if ((down & HidNpadButton_A) && g_ntitles > 0) {
                game_screen(&g_titles[sel]);
                saves_unmount();
            }
            frame(d_games, NULL);
            break;
        }
        }
    }

done:
    os_server_stop(&g_srv);
    saves_exit();
    socketExit();
    free(g_titles);
    free(g_rows);
    framebufferClose(&g_fb);
    return 0;
}
