/* The AES-256 key Minecraft Dungeons uses for its Windows save files.
 *
 * Provenance: the value is published in the open-source Minecraft Dungeons save
 * editor MCDSaveEdit (by CutFlame, MIT) through its DungeonTools library
 * (AGPL-3.0), file src/DungeonTools.Save.File/AesEncryptionProvider.cs at commit
 * 7413728 (the version MCDSaveEdit pins). Only this 32-byte constant is used;
 * the cipher code in aes.c is this project's own.
 *
 * Be aware: an earlier version of that library kept the key out of its source,
 * saying it is Mojang's property and that publishing it could put the project's
 * legality at risk. It is used here only to read and write the player's own save
 * files, as that editor does. */
#ifndef OPENSAVE_MCD_KEY_H
#define OPENSAVE_MCD_KEY_H

#include <stdint.h>

static const uint8_t OS_MCD_KEY[32] = {
    0x5C, 0xEB, 0x9D, 0x0A, 0xEB, 0xB9, 0x5A, 0xC0, 0x27, 0x0B, 0x0A, 0xF6, 0x75, 0x3D, 0xFC, 0x0E,
    0xE3, 0xE6, 0x8B, 0xB6, 0x94, 0x79, 0x02, 0x0F, 0x24, 0x30, 0xE2, 0xEA, 0x00, 0x2B, 0xD4, 0xC9,
};

#endif
