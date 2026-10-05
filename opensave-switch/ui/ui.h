/* The app's screens, drawn into a gfx buffer. Each function takes plain data
 * and draws one whole frame (or, for dialogs, a layer over one), so the
 * console and the PC-side screenshot test run exactly the same drawing code.
 *
 * The look: dark, one accent colour, generous spacing, large type. 1280x720. */
#ifndef OPENSAVE_UI_H
#define OPENSAVE_UI_H

#include "gfx.h"

#define UI_W 1280
#define UI_H 720

typedef enum { UI_INFO, UI_OK, UI_WARN, UI_ERR } ui_kind;

typedef struct {
    const char *key;   /* what is on the button: "A", "B", "+", "<>" */
    const char *label; /* what it does */
} ui_hint;

/* Colours, for the few places the caller picks one. */
uint32_t ui_color(ui_kind k);

/* ---------------------------------------------------------------- home */
typedef struct {
    const char *device_name;
    const char *address;     /* "192.168.1.8 : 8383", or "not connected" */
    int paired;
    const char *peer_name;   /* when paired */
    const char *peer_addr;
    const char *fingerprint; /* when paired */
    const char *banner;      /* a line of news, or NULL */
    ui_kind banner_kind;
    const char *clock_warning; /* NULL when the clocks agree */
    const char *const *items;
    int nitems, sel;
    const char *version;
} ui_home_t;
void ui_home(gfx *g, const ui_home_t *v);

/* ---------------------------------------------------------------- pair */
typedef struct {
    int ip[4];
    int port;
    int cursor; /* 0..3 address parts, 4 port */
} ui_pair_t;
void ui_pair(gfx *g, const ui_pair_t *v);

/* ---------------------------------------------------------------- busy */
/* A spinner, a headline and a line under it; frame drives the animation. */
void ui_busy(gfx *g, const char *title, const char *sub, const ui_hint *hints, int nhints, int frame);

/* ------------------------------------------------------------ progress */
void ui_progress(gfx *g, const char *title, const char *stage, int pct /* -1: unknown */, int frame);

/* --------------------------------------------------------------- games */
typedef struct {
    const char *name;
    const char *tid;
    const char *kind; /* "Account save" / "Console save" */
} ui_game_row;
typedef struct {
    const ui_game_row *rows;
    int n, sel, top;
    const char *user; /* NULL when there is no account */
    int nusers;
    const char *status; /* e.g. "Reading games 12 / 80", or NULL */
    const char *title;  /* the page title; NULL for "Games" */
    const char *action; /* what A does ("Open", "Choose"); NULL for "Open" */
    int picker;         /* a one-off choice: no rescan or user hints */
} ui_games_t;
int ui_games_visible(void);
void ui_games(gfx *g, const ui_games_t *v);

/* ---------------------------------------------------------- game detail */
typedef struct {
    const char *title;
    const char *tid;
    const char *peer_name;
    ui_kind status_kind;
    const char *status;  /* "Identical to the PC's save." */
    const char *detail;  /* a second line, or NULL */
    int only_pc, only_here, changed; /* shown as chips when any is set (>=0) */
    const char *const *actions;
    const char *const *action_help;
    const int *action_enabled;
    int nactions, sel;
} ui_game_t;
void ui_game(gfx *g, const ui_game_t *v);

/* -------------------------------------------------------------- result */
void ui_result(gfx *g, ui_kind kind, const char *title, const char *message, const ui_hint *hints, int nhints);

/* -------------------------------------------------------------- dialog */
/* A confirmation over whatever is already drawn. code, if set, is shown large
 * (a pairing fingerprint). */
void ui_dialog(gfx *g, ui_kind kind, const char *title, const char *message, const char *code, const char *yes,
               const char *no);

#endif
