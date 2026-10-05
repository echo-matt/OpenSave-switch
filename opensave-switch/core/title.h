/* Switch title ids, and the game ids other OpenSave devices know them by. */
#ifndef OPENSAVE_TITLE_H
#define OPENSAVE_TITLE_H

#include <stddef.h>
#include <stdint.h>

/* 16 hex digits plus a NUL. */
#define OS_TITLE_LEN 17

/* Whether s is a title id: exactly 16 hex digits. */
int os_title_valid(const char *s);

/* The title id a game id carries, in upper case, or 0 if it carries none.
 * Understands the ids OpenSave tracks Switch games under — "switch-<id>" and
 * the older "<name>-title-id-<id>" — either with the "-2" suffix a second copy
 * on one device is given. Mirrors internal/switchtitle. */
int os_title_from_game_id(const char *game_id, char out[OS_TITLE_LEN]);

/* The title id in a save path laid out the way the yuzu family of emulators
 * keeps it — .../save/<account>/<profile>/<TITLE> — in upper case. Either
 * path separator is accepted, since the path may come from another system.
 * Returns 0 for any other path. Mirrors switchtitle.FromSavePath. */
int os_title_from_save_path(const char *path, char out[OS_TITLE_LEN]);

/* The id this device tracks a title under: "switch-" and the id in lower case. */
void os_game_id_for_title(char *out, size_t outlen, const char *title_id);

/* The save path this device reports for a title, in the layout the emulators
 * keep, which is how a PC recognises a Switch save and finds the matching
 * emulator folder: save/<account>/<profile>/<TITLE>. */
void os_save_path_for_title(char *out, size_t outlen, const char *title_id);

#endif
