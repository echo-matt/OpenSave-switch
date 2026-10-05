#include "ui.h"

#include <stdio.h>
#include <string.h>

/* ----------------------------------------------------------------- theme */
#define C_BG_TOP GFX_RGB(0x14, 0x16, 0x1D)
#define C_BG_BOT GFX_RGB(0x0B, 0x0C, 0x10)
#define C_SURFACE GFX_RGB(0x1B, 0x1E, 0x27)
#define C_SURFACE2 GFX_RGB(0x25, 0x29, 0x37)
#define C_BORDER GFX_RGB(0x2C, 0x31, 0x40)
#define C_ACCENT GFX_RGB(0x84, 0x66, 0xFF)
#define C_TEXT GFX_RGB(0xEC, 0xEE, 0xF4)
#define C_MUTED GFX_RGB(0x8E, 0x94, 0xA7)
#define C_FAINT GFX_RGB(0x5A, 0x60, 0x72)
#define C_OK GFX_RGB(0x3D, 0xD6, 0x8C)
#define C_WARN GFX_RGB(0xF5, 0xB8, 0x4B)
#define C_ERR GFX_RGB(0xFF, 0x6B, 0x6B)

#define MARGIN 64
#define TOP_H 104
#define FOOT_H 84

uint32_t ui_color(ui_kind k) {
    switch (k) {
    case UI_OK: return C_OK;
    case UI_WARN: return C_WARN;
    case UI_ERR: return C_ERR;
    default: return C_ACCENT;
    }
}

/* ---------------------------------------------------------------- pieces */

static void background(gfx *g) { gfx_vgradient(g, C_BG_TOP, C_BG_BOT); }

/* Centres s within [x, x+w). */
static void text_center(gfx *g, const gfx_font *f, int x, int w, int y, const char *s, uint32_t c) {
    int tw = gfx_text_width(f, s);
    gfx_text(g, f, x + (w - tw) / 2, y, s, c);
}

static void chip(gfx *g, int x, int y, const char *label, uint32_t fg, uint32_t bg, int *out_w) {
    int w = gfx_text_width(&font_small, label) + 28;
    gfx_rrect(g, x, y, w, 36, 18, bg);
    gfx_text(g, &font_small, x + 14, y + 7, label, fg);
    if (out_w) *out_w = w;
}

/* A status chip with a dot, right-aligned to x_right. */
static void status_pill(gfx *g, int x_right, int y, const char *label, uint32_t dot) {
    int w = gfx_text_width(&font_small, label) + 28 + 22;
    int x = x_right - w;
    gfx_rrect(g, x, y, w, 40, 20, C_SURFACE);
    gfx_rrect_outline(g, x, y, w, 40, 20, 1, C_BORDER);
    gfx_circle(g, x + 22, y + 20, 5, dot);
    gfx_text(g, &font_small, x + 38, y + 9, label, C_MUTED);
}

static void top_bar(gfx *g, const char *title, const char *pill, uint32_t pill_dot) {
    gfx_rrect(g, MARGIN, 30, 44, 44, 12, C_ACCENT);
    text_center(g, &font_strong, MARGIN, 44, 38, "O", GFX_RGB(255, 255, 255));
    gfx_text(g, &font_title, MARGIN + 64, 28, title, C_TEXT);
    if (pill) status_pill(g, UI_W - MARGIN, 32, pill, pill_dot);
}

static int key_is_arrows(const char *key) { return !strcmp(key, "^v") || !strcmp(key, "<>"); }

static int key_width(const char *key) {
    int w = key_is_arrows(key) ? 58 : gfx_text_width(&font_small, key) + 22;
    return w < 34 ? 34 : w;
}

/* Small solid arrows, up and down or left and right. */
static void arrows(gfx *g, float cx, float cy, int horizontal, uint32_t c) {
    if (horizontal) {
        gfx_tri(g, cx - 14, cy, cx - 5, cy - 6, cx - 5, cy + 6, c);
        gfx_tri(g, cx + 14, cy, cx + 5, cy - 6, cx + 5, cy + 6, c);
    } else {
        gfx_tri(g, cx, cy - 14, cx - 6, cy - 5, cx + 6, cy - 5, c);
        gfx_tri(g, cx, cy + 14, cx - 6, cy + 5, cx + 6, cy + 5, c);
    }
}

static void key_pill(gfx *g, int x, int y, const char *key, int *out_w) {
    int w = key_width(key);
    gfx_rrect(g, x, y, w, 34, 17, C_SURFACE2);
    gfx_rrect_outline(g, x, y, w, 34, 17, 1, C_BORDER);
    if (key_is_arrows(key)) arrows(g, (float)x + w / 2.0f, (float)y + 17, key[0] == '<', C_TEXT);
    else text_center(g, &font_small, x, w, y + 6, key, C_TEXT);
    *out_w = w;
}

/* Footer hints, right-aligned, drawn right to left so the first is leftmost. */
static void hints_bar(gfx *g, const ui_hint *h, int n) {
    int i, x = UI_W - MARGIN, y = UI_H - FOOT_H + 24;
    for (i = n - 1; i >= 0; i--) {
        int lw = gfx_text_width(&font_small, h[i].label), kw;
        x -= lw;
        gfx_text(g, &font_small, x, y + 6, h[i].label, C_MUTED);
        x -= 12;
        x -= key_width(h[i].key);
        key_pill(g, x, y, h[i].key, &kw);
        x -= 32;
    }
}

static void card(gfx *g, int x, int y, int w, int h) {
    gfx_rrect(g, x, y, w, h, 20, C_SURFACE);
    gfx_rrect_outline(g, x, y, w, h, 20, 1, C_BORDER);
}

/* A selectable row; sel draws the highlight. */
static void row_bg(gfx *g, int x, int y, int w, int h, int sel) {
    if (!sel) return;
    gfx_rrect(g, x, y, w, h, 16, C_SURFACE2);
    gfx_rrect_outline(g, x, y, w, h, 16, 2, gfx_alpha(C_ACCENT, 200));
}

static void icon_kind(gfx *g, int cx, int cy, int r, ui_kind k) {
    uint32_t c = ui_color(k);
    float t = 7;
    gfx_circle(g, cx, cy, r, gfx_alpha(c, 40));
    gfx_arc(g, cx, cy, r, 3, 0, 359, gfx_alpha(c, 230));
    if (k == UI_OK) {
        gfx_line(g, cx - r * 0.38f, cy + r * 0.02f, cx - r * 0.10f, cy + r * 0.30f, t, c);
        gfx_line(g, cx - r * 0.10f, cy + r * 0.30f, cx + r * 0.42f, cy - r * 0.30f, t, c);
    } else if (k == UI_ERR) {
        gfx_line(g, cx - r * 0.32f, cy - r * 0.32f, cx + r * 0.32f, cy + r * 0.32f, t, c);
        gfx_line(g, cx - r * 0.32f, cy + r * 0.32f, cx + r * 0.32f, cy - r * 0.32f, t, c);
    } else { /* a stem and a dot */
        gfx_line(g, (float)cx, cy - r * 0.40f, (float)cx, cy + r * 0.10f, t, c);
        gfx_circle(g, cx, (int)(cy + r * 0.40f), 4, c);
    }
}

/* ------------------------------------------------------------------ home */

void ui_home(gfx *g, const ui_home_t *v) {
    static const ui_hint hints[] = {{"^v", "Move"}, {"A", "Select"}};
    int i, lx = MARGIN, lw = 540, rx = MARGIN + lw + 48, rw = UI_W - MARGIN - rx, y = TOP_H + 8;
    char buf[160];

    background(g);
    top_bar(g, "OpenSave", v->paired ? v->peer_name : "Not paired", v->paired ? C_OK : C_FAINT);

    /* Left: where this Switch is, and who it is paired with. */
    card(g, lx, y, lw, 150);
    gfx_text(g, &font_small, lx + 28, y + 22, "THIS SWITCH", C_FAINT);
    gfx_text_fit(g, &font_strong, lx + 28, y + 50, lw - 56, v->device_name, C_TEXT);
    gfx_text_fit(g, &font_body, lx + 28, y + 92, lw - 56, v->address, C_MUTED);

    card(g, lx, y + 170, lw, 190);
    gfx_text(g, &font_small, lx + 28, y + 192, "PAIRED PC", C_FAINT);
    if (v->paired) {
        gfx_text_fit(g, &font_strong, lx + 28, y + 220, lw - 56, v->peer_name, C_TEXT);
        gfx_text_fit(g, &font_body, lx + 28, y + 260, lw - 56, v->peer_addr, C_MUTED);
        snprintf(buf, sizeof buf, "Fingerprint  %s", v->fingerprint ? v->fingerprint : "");
        gfx_text_fit(g, &font_small, lx + 28, y + 304, lw - 56, buf, C_FAINT);
    } else {
        gfx_text(g, &font_strong, lx + 28, y + 220, "Nobody yet", C_MUTED);
        gfx_text_wrap(g, &font_small, lx + 28, y + 262, lw - 56, 4, "Open OpenSave on your PC, then choose Pair with a PC.", C_FAINT, 2);
    }

    /* Right: what to do. */
    {
        int ry = y;
        if (v->banner && v->banner[0]) {
            int h = 76;
            uint32_t c = ui_color(v->banner_kind);
            gfx_rrect(g, rx, ry, rw, h, 16, gfx_alpha(c, 34));
            gfx_rrect_outline(g, rx, ry, rw, h, 16, 1, gfx_alpha(c, 140));
            gfx_circle(g, rx + 30, ry + h / 2, 6, c);
            gfx_text_wrap(g, &font_small, rx + 52, ry + 12, rw - 76, 2, v->banner, C_TEXT, 2);
            ry += h + 16;
        }
        if (v->clock_warning && v->clock_warning[0]) {
            gfx_rrect(g, rx, ry, rw, 118, 16, gfx_alpha(C_WARN, 30));
            gfx_rrect_outline(g, rx, ry, rw, 118, 16, 1, gfx_alpha(C_WARN, 140));
            gfx_text_wrap(g, &font_small, rx + 24, ry + 16, rw - 48, 4, v->clock_warning, C_TEXT, 4);
            ry += 118 + 16;
        }
        for (i = 0; i < v->nitems; i++) {
            int rh = 80;
            row_bg(g, rx, ry, rw, rh, i == v->sel);
            gfx_text(g, &font_strong, rx + 28, ry + 22, v->items[i], i == v->sel ? C_TEXT : C_MUTED);
            if (i == v->sel) gfx_tri(g, (float)(rx + rw - 40), (float)(ry + rh / 2 - 9), (float)(rx + rw - 40), (float)(ry + rh / 2 + 9), (float)(rx + rw - 26), (float)(ry + rh / 2), C_ACCENT);
            ry += rh + 8;
        }
    }
    if (v->version) gfx_text(g, &font_small, MARGIN, UI_H - FOOT_H + 30, v->version, C_FAINT);
    hints_bar(g, hints, 2);
}

/* ------------------------------------------------------------------ pair */

void ui_pair(gfx *g, const ui_pair_t *v) {
    static const ui_hint hints[] = {{"<>", "Field"}, {"^v", "Change"}, {"L R", "Faster"}, {"A", "Send request"}, {"B", "Back"}};
    int i, bw = 120, gap = 36, total = 4 * bw + 3 * gap + 40 + 150, x0 = (UI_W - total) / 2, y = 330;
    char b[16];

    background(g);
    top_bar(g, "Pair with a PC", NULL, 0);
    text_center(g, &font_body, 0, UI_W, 176, "Enter the PC's address", C_TEXT);
    text_center(g, &font_small, 0, UI_W, 218, "Shown in OpenSave on the PC, under Devices. Both must be on the same network.", C_MUTED);

    for (i = 0; i < 5; i++) {
        int x, w = i < 4 ? bw : 150, sel = v->cursor == i;
        x = i < 4 ? x0 + i * (bw + gap) : x0 + 4 * bw + 3 * gap + 40;
        gfx_rrect(g, x, y, w, 120, 20, sel ? C_SURFACE2 : C_SURFACE);
        gfx_rrect_outline(g, x, y, w, 120, 20, sel ? 3 : 1, sel ? C_ACCENT : C_BORDER);
        snprintf(b, sizeof b, "%d", i < 4 ? v->ip[i] : v->port);
        text_center(g, &font_title, x, w, y + 36, b, sel ? C_TEXT : C_MUTED);
        if (sel) {
            gfx_tri(g, (float)(x + w / 2), (float)(y - 26), (float)(x + w / 2 - 10), (float)(y - 12), (float)(x + w / 2 + 10), (float)(y - 12), C_ACCENT);
            gfx_tri(g, (float)(x + w / 2), (float)(y + 146), (float)(x + w / 2 - 10), (float)(y + 132), (float)(x + w / 2 + 10), (float)(y + 132), C_ACCENT);
        }
        if (i < 3) gfx_circle(g, x + w + gap / 2, y + 100, 4, C_FAINT);
        if (i == 3) gfx_text(g, &font_title, x + w + 12, y + 28, ":", C_FAINT);
    }
    text_center(g, &font_small, 0, UI_W, y + 190, "Then approve this Switch in OpenSave on the PC.", C_FAINT);
    hints_bar(g, hints, 5);
}

/* ------------------------------------------------------------------ busy */

static void spinner(gfx *g, int cx, int cy, int frame) {
    gfx_arc(g, cx, cy, 44, 7, 0, 359, gfx_alpha(C_ACCENT, 40));
    gfx_arc(g, cx, cy, 44, 7, (frame * 7) % 360, (frame * 7 + 100) % 360, C_ACCENT);
}

void ui_busy(gfx *g, const char *title, const char *sub, const ui_hint *hints, int nhints, int frame) {
    background(g);
    top_bar(g, "OpenSave", NULL, 0);
    spinner(g, UI_W / 2, 290, frame);
    text_center(g, &font_title, 0, UI_W, 380, title, C_TEXT);
    if (sub) {
        gfx_text_wrap_ex(g, &font_body, (UI_W - 760) / 2, 448, 760, 6, sub, C_MUTED, 4, 1);
    }
    if (nhints) hints_bar(g, hints, nhints);
}

/* -------------------------------------------------------------- progress */

void ui_progress(gfx *g, const char *title, const char *stage, int pct, int frame) {
    static const ui_hint hints[] = {{"B", "Hold to cancel"}};
    int x = (UI_W - 820) / 2, y = 300, w = 820;
    char b[16];
    background(g);
    top_bar(g, "OpenSave", NULL, 0);
    card(g, x, y - 70, w, 220);
    gfx_text_fit(g, &font_strong, x + 36, y - 40, w - 72, title, C_TEXT);
    gfx_text_fit(g, &font_body, x + 36, y + 10, w - 72 - 110, stage, C_MUTED);
    gfx_rrect(g, x + 36, y + 90, w - 72, 14, 7, C_SURFACE2);
    if (pct >= 0) {
        int fw = (w - 72) * (pct > 100 ? 100 : pct) / 100;
        if (fw < 14) fw = 14;
        gfx_rrect(g, x + 36, y + 90, fw, 14, 7, C_ACCENT);
        snprintf(b, sizeof b, "%d%%", pct);
        gfx_text(g, &font_strong, x + w - 36 - gfx_text_width(&font_strong, b), y + 6, b, C_TEXT);
    } else { /* unknown length: a sliding segment */
        int seg = 180, travel = w - 72 - seg, p = (frame * 6) % (2 * travel), pos = p < travel ? p : 2 * travel - p;
        gfx_rrect(g, x + 36 + pos, y + 90, seg, 14, 7, C_ACCENT);
    }
    hints_bar(g, hints, 1);
}

/* ----------------------------------------------------------------- games */

#define GAME_ROW_H 68
int ui_games_visible(void) { return (UI_H - TOP_H - FOOT_H - 40) / GAME_ROW_H; }

void ui_games(gfx *g, const ui_games_t *v) {
    static const ui_hint hints[] = {{"^v", "Move"}, {"A", "Open"}, {"R", "Rescan"}, {"B", "Back"}};
    static const ui_hint hints_users[] = {{"^v", "Move"}, {"A", "Open"}, {"ZL", "User"}, {"R", "Rescan"}, {"B", "Back"}};
    int i, vis = ui_games_visible(), y0 = TOP_H + 20, x = MARGIN, w = UI_W - 2 * MARGIN;
    char buf[64];

    background(g);
    top_bar(g, "Games", v->user ? v->user : "No account", v->user ? C_ACCENT : C_FAINT);
    if (v->status) {
        text_center(g, &font_small, 0, UI_W, y0 + 8, v->status, C_MUTED);
    }
    if (v->n == 0) {
        text_center(g, &font_strong, 0, UI_W, 300, v->status ? "" : "No games with save data", C_MUTED);
        if (!v->status) text_center(g, &font_small, 0, UI_W, 346, "Games you have not played yet have no save to move.", C_FAINT);
    }
    for (i = 0; i < vis && v->top + i < v->n; i++) {
        const ui_game_row *r = &v->rows[v->top + i];
        int y = y0 + 28 + i * GAME_ROW_H, sel = v->top + i == v->sel;
        row_bg(g, x, y, w - 20, GAME_ROW_H - 6, sel);
        gfx_text_fit(g, &font_body, x + 26, y + 15, w - 20 - 52 - 270, r->name, sel ? C_TEXT : GFX_RGB(0xC4, 0xC8, 0xD4));
        {
            int kw = gfx_text_width(&font_small, r->kind);
            gfx_text(g, &font_small, x + w - 20 - 26 - kw, y + 20, r->kind, sel ? C_MUTED : C_FAINT);
        }
    }
    if (v->n > vis) { /* a thin scroll bar */
        int th = (UI_H - TOP_H - FOOT_H - 40), bh = th * vis / v->n, by = y0 + 28 + (th - bh) * v->top / (v->n - vis > 0 ? v->n - vis : 1);
        if (bh < 36) bh = 36;
        gfx_rrect(g, UI_W - MARGIN - 8, y0 + 28, 6, th, 3, gfx_alpha(C_BORDER, 120));
        gfx_rrect(g, UI_W - MARGIN - 8, by, 6, bh, 3, C_FAINT);
    }
    snprintf(buf, sizeof buf, "%d game%s", v->n, v->n == 1 ? "" : "s");
    gfx_text(g, &font_small, MARGIN, UI_H - FOOT_H + 30, buf, C_FAINT);
    if (v->nusers > 1) hints_bar(g, hints_users, 5);
    else hints_bar(g, hints, 4);
}

/* ----------------------------------------------------------- game detail */

void ui_game(gfx *g, const ui_game_t *v) {
    static const ui_hint hints[] = {{"^v", "Move"}, {"A", "Choose"}, {"B", "Back"}};
    int i, y = TOP_H + 4, w = UI_W - 2 * MARGIN;
    char buf[64];

    background(g);
    top_bar(g, "Game", v->peer_name, C_ACCENT);

    gfx_text_fit(g, &font_title, MARGIN, y, w, v->title, C_TEXT);
    gfx_text(g, &font_small, MARGIN, y + 56, v->tid, C_FAINT);

    /* Status card: the verdict on the left, the file counts on the right. */
    {
        int cy = y + 84, ch = v->detail ? 124 : 84, x = MARGIN, cx = x + w - 28;
        uint32_t k = ui_color(v->status_kind);
        int have_counts = v->only_pc >= 0 && v->only_here >= 0 && v->changed >= 0 && (v->only_pc + v->only_here + v->changed) > 0;
        card(g, x, cy, w, ch);
        gfx_circle(g, x + 40, cy + 42, 9, k);
        if (have_counts) { /* right to left */
            int cw;
            snprintf(buf, sizeof buf, "%d changed", v->changed);
            cx -= gfx_text_width(&font_small, buf) + 28;
            chip(g, cx, cy + 24, buf, C_TEXT, C_SURFACE2, &cw);
            snprintf(buf, sizeof buf, "%d only here", v->only_here);
            cx -= gfx_text_width(&font_small, buf) + 28 + 10;
            chip(g, cx, cy + 24, buf, C_TEXT, C_SURFACE2, &cw);
            snprintf(buf, sizeof buf, "%d only on PC", v->only_pc);
            cx -= gfx_text_width(&font_small, buf) + 28 + 10;
            chip(g, cx, cy + 24, buf, C_TEXT, C_SURFACE2, &cw);
        }
        gfx_text_fit(g, &font_strong, x + 68, cy + 26, (have_counts ? cx - 12 : x + w - 28) - (x + 68), v->status, C_TEXT);
        if (v->detail) gfx_text_wrap(g, &font_small, x + 68, cy + 70, w - 100, 4, v->detail, C_MUTED, 2);
        y = cy + ch + 16;
    }

    /* Actions. */
    for (i = 0; i < v->nactions; i++) {
        int rh = 82, en = v->action_enabled ? v->action_enabled[i] : 1;
        uint32_t t = en ? (i == v->sel ? C_TEXT : GFX_RGB(0xC4, 0xC8, 0xD4)) : C_FAINT;
        row_bg(g, MARGIN, y, w, rh - 8, i == v->sel);
        gfx_text(g, &font_strong, MARGIN + 28, y + 10, v->actions[i], t);
        if (v->action_help && v->action_help[i])
            gfx_text_fit(g, &font_small, MARGIN + 28, y + 44, w - 80, v->action_help[i], en ? C_MUTED : C_FAINT);
        y += rh;
    }
    hints_bar(g, hints, 3);
}

/* ---------------------------------------------------------------- result */

void ui_result(gfx *g, ui_kind kind, const char *title, const char *message, const ui_hint *hints, int nhints) {
    static const ui_hint def[] = {{"A", "Continue"}};
    background(g);
    top_bar(g, "OpenSave", NULL, 0);
    icon_kind(g, UI_W / 2, 250, 56, kind);
    text_center(g, &font_title, 0, UI_W, 340, title, C_TEXT);
    if (message && message[0]) gfx_text_wrap_ex(g, &font_body, (UI_W - 820) / 2, 412, 820, 8, message, C_MUTED, 6, 1);
    hints_bar(g, nhints ? hints : def, nhints ? nhints : 1);
}

/* ---------------------------------------------------------------- dialog */

void ui_dialog(gfx *g, ui_kind kind, const char *title, const char *message, const char *code, const char *yes,
               const char *no) {
    int w = 760, h = code ? 430 : 340, x = (UI_W - w) / 2, y = (UI_H - h) / 2, bx, bw = 300;
    uint32_t k = ui_color(kind);
    gfx_rect(g, 0, 0, UI_W, UI_H, GFX_RGBA(0, 0, 0, 170));
    gfx_rrect(g, x, y, w, h, 26, GFX_RGB(0x20, 0x23, 0x2E));
    gfx_rrect_outline(g, x, y, w, h, 26, 1, C_BORDER);
    gfx_rrect(g, x + 36, y + 36, 8, 36, 4, k);
    gfx_text_fit(g, &font_title, x + 62, y + 30, w - 100, title, C_TEXT);
    gfx_text_wrap(g, &font_body, x + 40, y + 98, w - 80, 6, message, C_MUTED, code ? 3 : 5);
    if (code) {
        gfx_rrect(g, x + 40, y + 218, w - 80, 76, 16, C_SURFACE);
        text_center(g, &font_strong, x + 40, w - 80, y + 238, code, C_OK);
    }
    {
        int by = y + h - 96;
        bx = x + w - 40 - bw;
        gfx_rrect(g, bx, by, bw, 64, 32, k);
        {
            char lbl[80];
            snprintf(lbl, sizeof lbl, "A   %s", yes);
            text_center(g, &font_strong, bx, bw, by + 15, lbl, GFX_RGB(255, 255, 255));
        }
        bx -= bw + 20;
        gfx_rrect(g, bx, by, bw, 64, 32, C_SURFACE2);
        gfx_rrect_outline(g, bx, by, bw, 64, 32, 1, C_BORDER);
        {
            char lbl[80];
            snprintf(lbl, sizeof lbl, "B   %s", no);
            text_center(g, &font_strong, bx, bw, by + 15, lbl, C_TEXT);
        }
    }
}
