/* Minecraft Dungeons: the Windows game's saves on a PC, used on a Switch.
 *
 * Both versions keep a character as the same JSON. The Switch stores it as a
 * file named "Character<id>"; Windows stores "<id>.dat": the JSON, encrypted
 * with AES-256 behind a "D001" header. Converting is done on the console itself
 * (core/mcd_key.h says where the key comes from); nothing leaves the Switch.
 *
 * The PC game is *linked* to the Switch title. Receiving brings the PC's files to
 * a mirror on the SD card, decrypts the characters in them and writes the Switch
 * files. Sending encrypts the characters that changed on the Switch into the
 * mirror, which is what the PC then pulls from — so the PC always sees exactly
 * its own files plus the Switch's changes, never a "deletion" of something the
 * Switch simply never had.
 *
 * Nothing here deletes a Switch character, and the Switch save is backed up and
 * checked before anything in it is written.
 */
#ifndef OPENSAVE_MCD_H
#define OPENSAVE_MCD_H

#include <stddef.h>
#include <stdint.h>

#include "peer.h"
#include "sync.h"

#define OS_MCD_TITLE "01006C100EC08000"

/* Whether a Switch title can have a Windows save linked to it. */
int os_mcd_supported(const char *title_id);

/* Whether bytes are a Windows save: the D001 header and something after it. */
int os_mcd_is_dat(const uint8_t *b, size_t n);
/* Whether JSON is a character (an object with a playerId or uniqueSaveId). */
int os_mcd_is_character(const uint8_t *json, size_t n);

/* A whole Windows file to JSON: header checked, decrypted. Returns 0, -1 if it is
 * not a valid encrypted save, or 1 if it decrypted but is not a character (a
 * profile, say, or garbage). *json is malloc'd on success. */
int os_mcd_decrypt_dat(const uint8_t *dat, size_t n, uint8_t **json, size_t *jn, char *err, size_t errlen);
/* JSON to a whole Windows file. The result is decrypted again and must match the
 * input before it is returned, since a file the game cannot read is worse than none. */
int os_mcd_encrypt_json(const uint8_t *json, size_t n, uint8_t **dat, size_t *dn, char *err, size_t errlen);

typedef struct {
    int converted;      /* characters written to the Switch */
    int skipped;        /* encrypted files that are not characters */
    int files;          /* files downloaded to the mirror */
    int64_t bytes;
    int already_same;
    char backup_path[256];
    char manifest_hash[65]; /* of the mirror, which equals the PC's after a receive */
} os_mcd_result;

/* Receive: PC to Switch. save_root is the mounted save; convert_dir holds the
 * mirror, staging and bookkeeping for this title; backup_dir is where the
 * Switch's current save is copied first. On failure the save is as it was. */
int os_mcd_pull(os_state *s, const os_peer *p, const os_link *l, const char *save_root, const char *convert_dir,
                const char *backup_dir, const os_progress *pr, os_mcd_result *res, char *err, size_t errlen);

/* Send, first half: encrypt the Switch's changed characters into the mirror, so
 * the PC can pull them. *prepared counts the characters. The caller then asks the
 * PC to sync, and serves the mirror (convert_dir/mirror) to it. */
int os_mcd_prepare_send(const char *save_root, const char *convert_dir, int *prepared, char *err, size_t errlen);

/* Where the mirror lives under convert_dir. */
void os_mcd_mirror_path(const char *convert_dir, char *out, size_t outlen);

/* Compare: no data leaves the console. */
void os_mcd_compare(os_state *s, const os_peer *p, const os_link *l, const char *save_root, const char *convert_dir,
                    os_cmp_result *out);

#endif
