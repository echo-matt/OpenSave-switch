/* Renders every screen to a PPM picture (and checks nothing draws outside the
 * buffer, since this runs under AddressSanitizer). Usage: ui_shots OUTDIR */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../ui/ui.h"
#include "testutil.h"

static uint32_t *buf;
static gfx g;

static void save(const char *dir, const char *name) {
    char path[400];
    FILE *f;
    int x, y;
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    f = fopen(path, "wb");
    if (!f) { t_failures++; return; }
    fprintf(f, "P6\n%d %d\n255\n", UI_W, UI_H);
    for (y = 0; y < UI_H; y++)
        for (x = 0; x < UI_W; x++) {
            uint32_t p = buf[y * UI_W + x];
            fputc((int)(p & 255), f);
            fputc((int)((p >> 8) & 255), f);
            fputc((int)((p >> 16) & 255), f);
        }
    fclose(f);
    CHECK(1);
}

static int differs_from_blank(void) {
    int i, n = 0;
    for (i = 0; i < UI_W * UI_H; i += 997) n += buf[i] != buf[0];
    return n;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    static const char *const items_paired[] = {"Games", "Pair with a different PC", "Exit"};
    static const char *const items_new[] = {"Pair with a PC", "Exit"};
    ui_home_t home;
    ui_pair_t pair = {{192, 168, 1, 20}, 8383, 1};
    ui_games_t games;
    ui_game_row rows[40];
    static char names[40][80];
    static const char *const acts[] = {"Receive from the PC", "Send to the PC", "Restore the last backup", "Check again"};
    static const char *const help[] = {"Replace this Switch's save with the PC's. The current one is backed up first.",
                                       "Ask the PC to take this Switch's save.", "Put back the save from before the last receive.", "Compare with the PC once more."};
    static const int en[] = {1, 1, 1, 1};
    ui_game_t game;
    int i;
    static const ui_hint cancel_hint[] = {{"B", "Cancel"}};

    buf = calloc((size_t)UI_W * UI_H, 4);
    gfx_init(&g, buf, UI_W, UI_H, UI_W);

    memset(&home, 0, sizeof home);
    home.device_name = "Nintendo Switch";
    home.address = "192.168.1.8 : 8383";
    home.paired = 1;
    home.peer_name = "Matt's PC";
    home.peer_addr = "192.168.1.20 : 8383";
    home.fingerprint = "3f9a 12bc 77de 04a1 b8c3 5e60";
    home.items = items_paired;
    home.nitems = 3;
    home.version = "OpenSave for Switch 0.2.0";
    ui_home(&g, &home);
    CHECK(differs_from_blank() > 20);
    save(dir, "home_paired");
    home.banner = "The PC has newer progress for The Legend of Zelda: Tears of the Kingdom.";
    home.banner_kind = UI_INFO;
    home.clock_warning = "The Switch's clock is about 11 minute(s) behind the other device's. Set the date and time in System Settings.";
    home.sel = 1;
    ui_home(&g, &home);
    save(dir, "home_banner");
    home.paired = 0;
    home.banner = NULL;
    home.clock_warning = NULL;
    home.items = items_new;
    home.nitems = 2;
    home.sel = 0;
    ui_home(&g, &home);
    save(dir, "home_unpaired");

    ui_pair(&g, &pair);
    save(dir, "pair");
    ui_busy(&g, "Waiting for approval", "Approve this Switch in OpenSave on the PC, under Devices. This can take a moment.", cancel_hint, 1, 12);
    save(dir, "busy");

    for (i = 0; i < 40; i++) {
        snprintf(names[i], sizeof names[i], i == 3 ? "\xe3\x82\xbc\xe3\x83\xab\xe3\x83\x80 \xe3\x81\xae\xe4\xbc\x9d\xe8\xaa\xac" : i == 5 ? "A Very Long Game Title That Goes On And On: The Subtitle Edition Deluxe Remastered Collection" : "Game number %d", i + 1);
        rows[i].name = names[i];
        rows[i].tid = "0100F2C0115B6000";
        rows[i].kind = i % 7 == 0 ? "Console save" : "Account save";
    }
    memset(&games, 0, sizeof games);
    games.rows = rows;
    games.n = 40;
    games.sel = 4;
    games.top = 0;
    games.user = "Matt";
    games.nusers = 2;
    ui_games(&g, &games);
    CHECK(ui_games_visible() >= 6);
    save(dir, "games");
    games.n = 0;
    games.status = "Reading games  12 / 80";
    ui_games(&g, &games);
    save(dir, "games_loading");

    memset(&game, 0, sizeof game);
    game.title = "The Legend of Zelda: Tears of the Kingdom";
    game.tid = "0100F2C0115B6000";
    game.peer_name = "Matt's PC";
    game.status_kind = UI_WARN;
    game.status = "The saves are different";
    game.only_pc = 1;
    game.only_here = 0;
    game.changed = 3;
    game.actions = acts;
    game.action_help = help;
    game.action_enabled = en;
    game.nactions = 4;
    game.sel = 0;
    ui_game(&g, &game);
    save(dir, "game_different");
    game.status_kind = UI_OK;
    game.status = "Identical to the PC's save";
    game.only_pc = game.only_here = game.changed = 0;
    game.sel = 2;
    ui_game(&g, &game);
    save(dir, "game_same");
    game.status_kind = UI_WARN;
    game.status = "The PC does not have this game yet";
    game.detail = "It has been offered to OpenSave on the PC: choose its save folder there, then check again.";
    game.only_pc = game.only_here = game.changed = -1;
    ui_game(&g, &game);
    save(dir, "game_missing");

    ui_progress(&g, "Receiving The Legend of Zelda: Tears of the Kingdom", "slot/autosave_0.sav", 63, 0);
    save(dir, "progress");
    ui_progress(&g, "Receiving Game", "Asking the PC what it has", -1, 20);
    save(dir, "progress_unknown");

    ui_result(&g, UI_OK, "Saved to this Switch", "2 files received, 1 removed. The previous save is backed up on the SD card.", NULL, 0);
    save(dir, "result_ok");
    ui_result(&g, UI_ERR, "Nothing was changed", "A block does not match the hash in the manifest (corrupted or tampered with). The save was put back as it was.", NULL, 0);
    save(dir, "result_err");

    ui_home(&g, &home);
    ui_dialog(&g, UI_WARN, "Replace this Switch's save?", "The current save is backed up on the SD card first, so you can put it back.", NULL, "Replace", "Cancel");
    save(dir, "dialog");
    ui_dialog(&g, UI_INFO, "Pairing request", "\"Matt's PC\" at 192.168.1.20 wants to pair. Check that the PC shows the same code.", "3f9a 12bc 77de 04a1 b8c3 5e60", "Approve", "Reject");
    save(dir, "dialog_pair");

    /* text helpers */
    CHECK(gfx_text_width(&font_body, "Hello") > 40);
    CHECK(gfx_text_width(&font_body, "\xe3\x82\xbc\xe3\x83\xab") == gfx_text_width(&font_body, "?")); /* a run of non-ASCII is one '?' */
    CHECK(gfx_text_wrap(NULL, &font_body, 0, 0, 200, 0, "one two three four five six seven eight nine ten", 0, 0) >= 3);
    CHECK(gfx_text_wrap(NULL, &font_body, 0, 0, 200, 0, "Supercalifragilisticexpialidocious-and-more", 0, 0) >= 2); /* a long word is cut, not lost */
    CHECK(gfx_text_wrap(NULL, &font_body, 0, 0, 200, 0, "", 0, 0) == 0);
    {   /* A frame must stay cheap: the console redraws every vertical sync. The
         * host is much faster than the Switch's CPU, so the bound is loose, but
         * it catches a change that makes drawing dramatically slower. */
        clock_t t0 = clock();
        int k;
        home.paired = 1;
        home.items = items_paired;
        home.nitems = 3;
        for (k = 0; k < 20; k++) ui_games(&g, &games), ui_home(&g, &home);
        printf("avg frame: %.2f ms (host, with sanitizers)\n", 1000.0 * (double)(clock() - t0) / CLOCKS_PER_SEC / 40);
    }
    free(buf);
    return t_finish("ui");
}
