# mcdsync — Minecraft Dungeons saves, Windows ⇄ Switch

Keeps character saves in step between the Windows game and the Switch, so
OpenSave can carry them. The two use the same JSON; only the wrapping differs:

| | file | contents |
| --- | --- | --- |
| Windows | `<id>.dat` | `D001`, four zero bytes, then the JSON encrypted with AES |
| Switch | `Character<id>` (no extension) | the JSON as plain text |

The AES key is not public, so encryption and decryption are done by the
**dungeons.tools** service — the one [MCDSaveEdit](https://github.com/CutFlame/MCDSaveEdit)
uses. **Your character JSON (game progress, nothing else) is sent to it over
HTTPS whenever a file changes.** You must pass `-yes` to agree.

## Run

```sh
mcdsync -yes \
  -windows "%LOCALAPPDATA%\Dungeons\Saved\...\<folder holding the .dat files>" \
  -switch  "<the folder OpenSave syncs to the Switch>"
```

`-windows` is the folder that directly holds the `.dat` character files. `-switch`
is a folder laid out the way an emulator keeps a Switch save, which is how
OpenSave recognises it:
`…/nand/user/save/0000000000000000/<32 hex digits>/01006C100EC08000`
(create the folders, and track that last one in OpenSave). On the Switch app, the
game then shows **Receive from the PC**.

It checks every 10 seconds (`-interval 0` runs once). It matches files by id:
`<id>.dat` ⇄ `Character<id>`.

## What it will and will not do

- Copies a change one way only when **exactly one side changed** since the two
  last matched. If both changed it **stops and says CONFLICT**, changing nothing;
  delete the copy you do not want and it is replaced from the other.
- **Never deletes** a file. A character deleted on one side is not recreated from
  the other.
- Before writing a Windows file it encrypts, decrypts the result again and
  requires it to match what it started with, then **backs up the old Windows
  file** to `.mcdsync-backup` (the newest five are kept).
- Leaves alone any file touched in the last 3 seconds, any `.dat` without the
  `D001` header, and any file whose JSON is not a character (no `playerId` /
  `uniqueSaveId`).
- Remembers what matched in `.mcdsync-state.json` inside the Switch folder
  (OpenSave never syncs dot-files).

## Tested, and not

Tested here against a stand-in for the service (same protocol, a key made for the
test): the HTTP exchange, both directions, conflicts, no deletes, service
failures, a service returning bad data, backups, restarts and a damaged state
file. **Not tested against the real service or a real Windows save**: that needs
network access to it, and I could not decrypt a real save here. Run it once with
backups of both folders before trusting it. A game running while a file is
rewritten may overwrite it; close the game first when sending Switch progress to
Windows.
