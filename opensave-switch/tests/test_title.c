#include "../core/title.h"
#include "testutil.h"

static void yes(const char *id, const char *want) {
    char out[OS_TITLE_LEN];
    int ok = os_title_from_game_id(id, out);
    CHECK(ok);
    if (ok) CHECK_STR(out, want);
    if (!ok) fprintf(stderr, "should have matched: %s\n", id);
}
static void no(const char *id) {
    char out[OS_TITLE_LEN];
    int ok = os_title_from_game_id(id, out);
    if (ok) fprintf(stderr, "should NOT have matched: %s -> %s\n", id, out);
    CHECK(!ok);
}

int main(void) {
    /* The ids OpenSave tracks Switch games under, as internal/switchtitle reads them. */
    yes("switch-0100f2c0115b6000", "0100F2C0115B6000");
    yes("switch-0100F2C0115B6000", "0100F2C0115B6000");
    yes("switch-0100f2c0115b6000-2", "0100F2C0115B6000");
    yes("citron-switch-emulator-title-id-0100f2c0115b6000", "0100F2C0115B6000");
    yes("eden-title-id-0100f2c0115b6000-12", "0100F2C0115B6000");
    no("");
    no("switch-");
    no("switch-0100f2c0115b600");     /* 15 digits */
    no("switch-0100f2c0115b60000");   /* 17 digits */
    no("switch-0100f2c0115b600g");    /* not hex */
    no("zelda-0100f2c0115b6000");     /* wrong prefix */
    no("-title-id-0100f2c0115b6000"); /* empty name */
    no("0100f2c0115b6000");           /* bare id */
    no("switch-0100f2c0115b6000-");   /* dangling dash */
    no("switch-0100f2c0115b6000-abc");
    no("a/b-title-id-0100f2c0115b6000");
    CHECK(os_title_valid("0100F2C0115B6000") && os_title_valid("0100f2c0115b6000"));
    CHECK(!os_title_valid("0100F2C0115B600") && !os_title_valid("0100F2C0115B600Z") && !os_title_valid(NULL));
    {
        char g[64], p[128], t[OS_TITLE_LEN];
        os_game_id_for_title(g, sizeof g, "0100F2C0115B6000");
        CHECK_STR(g, "switch-0100f2c0115b6000");
        CHECK(os_title_from_game_id(g, t));
        os_save_path_for_title(p, sizeof p, "0100F2C0115B6000");
        CHECK_STR(p, "save/0000000000000000/00000000000000000000000000000000/0100F2C0115B6000");
    }
    {   /* save paths, as switchtitle.FromSavePath reads them */
        char t[OS_TITLE_LEN];
        CHECK(os_title_from_save_path("/home/u/.local/share/yuzu/nand/user/save/0000000000000000/ABCDEF0123456789ABCDEF0123456789/0100f2c0115b6000", t));
        CHECK_STR(t, "0100F2C0115B6000");
        CHECK(os_title_from_save_path("C:\\Users\\u\\AppData\\Roaming\\yuzu\\nand\\user\\Save\\0000000000000000\\0123\\0100F2C0115B6000", t));
        CHECK(os_title_from_save_path("save/0000000000000000/00000000000000000000000000000000/0100F2C0115B6000", t));
        CHECK(!os_title_from_save_path("/home/u/saves/0100F2C0115B6000", t));                 /* too short */
        CHECK(!os_title_from_save_path("/x/notsave/0000000000000000/abcd/0100F2C0115B6000", t)); /* not "save" */
        CHECK(!os_title_from_save_path("/x/save/0000000000000000/abcd/0100F2C0115B600", t));  /* bad id */
        CHECK(!os_title_from_save_path("", t) && !os_title_from_save_path(NULL, t));
    }
    return t_finish("title");
}
