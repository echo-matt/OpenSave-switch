/* hostcli: the OpenSave Switch client's core, driven from a terminal.
 *
 * It exists so the protocol code can be exercised against a real OpenSave
 * daemon without a console: the end-to-end tests in tests/e2e run this as a
 * subprocess. A "save" is a plain folder, <saves>/<TITLE ID>, standing in for a
 * mounted save data.
 *
 *   hostcli [options] <command> [args]
 *
 *   options:  --state FILE   settings file (identity and pairings)
 *             --saves DIR    folder holding one sub-folder per title id
 *             --work DIR     where backups and staging go
 *             --port N       port to listen on (default 0 = any)
 *             --auto-approve approve pairing requests received from a device
 *             --name NAME    device name
 *   commands: ping IP PORT | pair IP PORT [SECS] | serve SECS | compare TITLE | offer TITLE |
 *             pull TITLE [LINGER] | push TITLE [SECS] | restore TITLE | unpair | info
 *
 * Output is line-oriented ("PAIRED ...", "PULL OK ...") for the tests to read.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../core/crypto.h"
#include "../core/peer.h"
#include "../core/platform.h"
#include "../core/server.h"
#include "../core/state.h"
#include "../core/sync.h"
#include "../core/title.h"

static const char *saves_dir = "saves";
static const char *work_dir = "work";
static int auto_approve = 0;
static os_server server;
static os_state st;
static int paired_event, update_event;

int os_random(void *buf, size_t len) {
    FILE *f = fopen("/dev/urandom", "rb");
    size_t got;
    if (!f) return -1;
    got = fread(buf, 1, len, f);
    fclose(f);
    return got == len ? 0 : -1;
}

static int open_save(void *ctx, const char *title, char *root, size_t rootlen, char *err, size_t errlen) {
    (void)ctx;
    (void)errlen;
    (void)err;
    snprintf(root, rootlen, "%s/%s", saves_dir, title);
    return 0;
}

static void on_pairing_request(void *ctx, const os_incoming *req) {
    char fp[OS_FINGERPRINT_LEN];
    (void)ctx;
    if (req->has_key) os_fingerprint(fp, st.pub, req->pubkey);
    else strcpy(fp, "(no key)");
    printf("PAIRING-REQUEST id=%s name=\"%s\" from=%s:%d fingerprint=\"%s\"\n", req->peer_id, req->name, req->address,
           req->port, fp);
    fflush(stdout);
}

static void on_paired(void *ctx, const os_peer *p) {
    char fp[OS_FINGERPRINT_LEN];
    (void)ctx;
    os_fingerprint(fp, st.pub, p->pubkey);
    printf("PAIRED id=%s name=\"%s\" address=%s:%d fingerprint=\"%s\"\n", p->id, p->name, p->address, p->port, fp);
    fflush(stdout);
    paired_event = 1;
}

static void on_unpaired(void *ctx, const char *id) {
    (void)ctx;
    printf("UNPAIRED id=%s\n", id);
    fflush(stdout);
}

static void on_peer_update(void *ctx, const os_peer *p, const char *game, const char *title) {
    (void)ctx;
    printf("PEER-UPDATE peer=%s game=%s title=%s\n", p->id, game, title);
    fflush(stdout);
    update_event = 1;
}

static void on_served(void *ctx, const char *what) {
    (void)ctx;
    printf("SERVED %s\n", what);
    fflush(stdout);
}

static void progress(void *ctx, const char *stage, int64_t done, int64_t total) {
    (void)ctx;
    fprintf(stderr, "PROGRESS %s %lld/%lld\n", stage, (long long)done, (long long)total);
}

static void serve_for(int secs) {
    time_t end = time(NULL) + secs;
    while (time(NULL) < end) os_server_poll(&server, 100);
}

static os_peer *first_peer(void) {
    int i;
    for (i = 0; i < OS_MAX_PEERS; i++)
        if (st.peers[i].in_use) return &st.peers[i];
    return NULL;
}

/* The other device's own id and name for a title, falling back to ours. */
static void resolve(const os_peer *p, const char *title, char *game, size_t gl, char *name, size_t nl) {
    os_remote_game g;
    int found = 0;
    char err[200];
    os_game_id_for_title(game, gl, title);
    snprintf(name, nl, "Title %s", title);
    if (os_peer_find_title(&st, p, title, &g, &found, err, sizeof err) != 0) {
        printf("RESOLVE FAIL %s\n", err);
        return;
    }
    if (found == 1) {
        snprintf(game, gl, "%s", g.id);
        snprintf(name, nl, "%s", g.name);
    }
    printf("RESOLVE found=%d game=%s\n", found, game);
}

static int need_peer(os_peer **p) {
    *p = first_peer();
    if (!*p) {
        printf("ERROR not paired with any device\n");
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *state_path = "state.json", *name = NULL;
    int port = 0, i, rc = 0;
    char err[400];
    os_server_hooks hooks;
    char **cmd;
    os_peer *peer;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--state") && i + 1 < argc) state_path = argv[++i];
        else if (!strcmp(argv[i], "--saves") && i + 1 < argc) saves_dir = argv[++i];
        else if (!strcmp(argv[i], "--work") && i + 1 < argc) work_dir = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--auto-approve")) auto_approve = 1;
        else break;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: hostcli [options] <command>\n");
        return 2;
    }
    cmd = argv + i;

    if (os_state_load(&st, state_path, err, sizeof err) != 0) {
        printf("ERROR %s\n", err);
        return 1;
    }
    if (name) snprintf(st.device_name, sizeof st.device_name, "%s", name);
    memset(&hooks, 0, sizeof hooks);
    hooks.open_save = open_save;
    hooks.on_pairing_request = on_pairing_request;
    hooks.on_paired = on_paired;
    hooks.on_unpaired = on_unpaired;
    hooks.on_peer_update = on_peer_update;
    hooks.on_served = on_served;
    if (os_server_start(&server, &st, &hooks, port ? port : 0, err, sizeof err) != 0) {
        printf("ERROR %s\n", err);
        return 1;
    }
    st.port = os_server_port(&server); /* the port actually bound is what peers must be told */
    printf("LISTENING port=%d node=%s\n", st.port, st.node_id);
    fflush(stdout);

    if (!strcmp(cmd[0], "info")) {
        char pub[64];
        os_b64_encode(pub, st.pub, 32);
        printf("INFO node=%s name=\"%s\" publicKey=%s\n", st.node_id, st.device_name, pub);
    } else if (!strcmp(cmd[0], "ping") && argc - i >= 3) {
        os_ping_info info;
        if (os_peer_ping(&st, cmd[1], atoi(cmd[2]), &info, err, sizeof err) != 0) {
            printf("PING FAIL %s\n", err);
            rc = 1;
        } else {
            printf("PING OK name=\"%s\" version=%s paired=%d\n", info.device_name, info.version, info.paired);
        }
    } else if (!strcmp(cmd[0], "pair") && argc - i >= 3) {
        int wait = argc - i >= 4 ? atoi(cmd[3]) : 60;
        time_t end;
        if (os_peer_handshake(&st, cmd[1], atoi(cmd[2]), err, sizeof err) != 0) {
            printf("PAIR FAIL %s\n", err);
            rc = 1;
        } else {
            printf("HANDSHAKE-SENT\n");
            fflush(stdout);
            end = time(NULL) + wait;
            while (!paired_event && time(NULL) < end) os_server_poll(&server, 100);
            if (!paired_event) {
                printf("PAIR TIMEOUT\n");
                rc = 1;
            }
        }
    } else if (!strcmp(cmd[0], "serve") && argc - i >= 2) {
        time_t end = time(NULL) + atoi(cmd[1]);
        while (time(NULL) < end) {
            os_server_poll(&server, 100);
            if (auto_approve) {
                int k;
                for (k = 0; k < OS_MAX_PENDING; k++)
                    if (st.incoming[k].in_use) {
                        char id[64];
                        snprintf(id, sizeof id, "%s", st.incoming[k].peer_id);
                        if (os_server_approve(&server, id, err, sizeof err) != 0) printf("APPROVE FAIL %s\n", err);
                    }
            }
        }
    } else if ((!strcmp(cmd[0], "compare") || !strcmp(cmd[0], "offer")) && argc - i >= 2) {
        int offer = !strcmp(cmd[0], "offer");
        char game[128], gname[128], path[128], root[300];
        os_cmp_result r;
        if (need_peer(&peer) != 0) return 1;
        resolve(peer, cmd[1], game, sizeof game, gname, sizeof gname);
        os_save_path_for_title(path, sizeof path, cmd[1]);
        snprintf(root, sizeof root, "%s/%s", saves_dir, cmd[1]);
        os_compare(&st, peer, game, gname, path, root, offer, &r);
        printf("COMPARE state=%d only_remote=%d only_local=%d differ=%d message=\"%s\"\n", (int)r.state, r.only_remote,
               r.only_local, r.differ, r.message);
    } else if (!strcmp(cmd[0], "pull") && argc - i >= 2) {
        char game[128], gname[128], path[128], root[300], backup[400], staging[400];
        os_pull_result r;
        os_progress pr;
        int linger = argc - i >= 3 ? atoi(cmd[2]) : 4;
        if (need_peer(&peer) != 0) return 1;
        resolve(peer, cmd[1], game, sizeof game, gname, sizeof gname);
        os_save_path_for_title(path, sizeof path, cmd[1]);
        snprintf(root, sizeof root, "%s/%s", saves_dir, cmd[1]);
        snprintf(backup, sizeof backup, "%s/backup/%s", work_dir, cmd[1]);
        snprintf(staging, sizeof staging, "%s/staging/%s", work_dir, cmd[1]);
        memset(&pr, 0, sizeof pr);
        pr.progress = progress;
        if (os_pull(&st, peer, game, root, backup, staging, &pr, &r, err, sizeof err) != 0) {
            printf("PULL FAIL %s\n", err);
            rc = 1;
        } else {
            printf("PULL OK same=%d downloaded=%d deleted=%d bytes=%lld backup=\"%s\" hash=%s extra=%d\n", r.already_same,
                   r.files_downloaded, r.files_deleted, (long long)r.bytes_downloaded, r.backup_path, r.manifest_hash,
                   r.remote_has_extra_roots);
            fflush(stdout);
            if (os_report_in_sync(&st, peer, game, r.manifest_hash, err, sizeof err) != 0)
                printf("IN-SYNC FAIL %s\n", err);
            /* The PC calls back for our file list to confirm; stay up for it. */
            serve_for(linger);
        }
    } else if (!strcmp(cmd[0], "push") && argc - i >= 2) {
        char game[128], gname[128];
        int secs = argc - i >= 3 ? atoi(cmd[2]) : 8;
        if (need_peer(&peer) != 0) return 1;
        resolve(peer, cmd[1], game, sizeof game, gname, sizeof gname);
        if (os_peer_trigger_sync(&st, peer, game, err, sizeof err) != 0) {
            printf("PUSH FAIL %s\n", err);
            rc = 1;
        } else {
            printf("PUSH TRIGGERED\n");
            fflush(stdout);
            serve_for(secs);
        }
    } else if (!strcmp(cmd[0], "restore") && argc - i >= 2) {
        char root[300], backup[400];
        snprintf(root, sizeof root, "%s/%s", saves_dir, cmd[1]);
        snprintf(backup, sizeof backup, "%s/backup/%s", work_dir, cmd[1]);
        if (os_restore_backup(root, backup, err, sizeof err) != 0) {
            printf("RESTORE FAIL %s\n", err);
            rc = 1;
        } else {
            printf("RESTORE OK\n");
        }
    } else if (!strcmp(cmd[0], "unpair")) {
        if (need_peer(&peer) != 0) return 1;
        if (os_peer_unpair(&st, peer, err, sizeof err) != 0) printf("UNPAIR NOTIFY FAIL %s\n", err);
        os_state_remove_peer(&st, peer->id);
        os_state_save(&st, err, sizeof err);
        printf("UNPAIRED-LOCALLY\n");
    } else {
        printf("ERROR unknown command %s\n", cmd[0]);
        rc = 2;
    }
    os_server_stop(&server);
    return rc;
}
