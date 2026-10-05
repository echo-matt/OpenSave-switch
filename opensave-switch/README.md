# OpenSave for Nintendo Switch

A homebrew app that makes a Switch a paired OpenSave device. From the console you
can **receive** a game's save from your PC (for example, progress made in an
emulator on Windows) or **send** the console's save to your PC, over your home
network.

> **Status: not yet run on a real console.** The protocol, the safety logic and
> the file handling are tested (below). The part that touches the Switch itself
> — mounting save data, the on-screen interface — is written against libnx but
> has **never been run on hardware**, and was not compiled where it was written.
> The CI job in `.github/workflows/opensave-switch.yml` builds it with the real
> toolchain; treat the first run on your console as a test, and keep your own
> backup of anything you cannot lose.

## What you need

- A Switch running **custom firmware (Atmosphere)**. Only homebrew with full
  file-system access can open the save data of other games; on stock firmware no
  app can, and this one cannot work.
- **OpenSave 2.4.1 or later** on the PC. Earlier versions do not accept the
  signed requests this client sends.
- The PC and the Switch on the **same local network**. The Windows firewall must
  allow OpenSave's port (8383 by default) in, or the Switch cannot reach it.
- The Switch's **date and time set correctly** (System Settings). Requests are
  refused when the two clocks differ by more than five minutes, and the Switch
  has no way to correct itself offline.

## Install

Copy `opensave-switch.nro` to `sdmc:/switch/` on the SD card and start it from the
homebrew menu. (Download it from the CI run's artifacts, or build it: see below.)

## Use

The interface is dark and minimal: a menu, a list of games, and one screen per
game. Every screen shows its buttons along the bottom.

1. **Pair.** On the PC, open OpenSave and keep it running. On the Switch choose
   **Pair with a PC**, set the PC's local address with the D-pad (for example
   `192.168.1.20`; `ipconfig` shows it) and press **A**. In OpenSave on the PC,
   **approve** the request under Devices.
   Both screens now show a **fingerprint**. They must match: it is what rules out
   someone else on the network sitting between the two. If they differ, unpair
   on both.
2. **Pick a game** (**Games**). The list shows every installed game that keeps
   save data. **ZL** switches user account, **R** rescans the games. Open one
   and the Switch compares its save with the PC's.
3. **Choose what to do:**
   - **Receive from the PC** replaces the Switch's save with the PC's. The
     current save is copied to the SD card first and the copy is verified.
   - **Send to the PC** asks the PC to take the Switch's save. The PC then reads
     it from the Switch, so keep the screen open until it finishes. If both sides
     changed since they last agreed, OpenSave on the PC asks which to keep.
   - **Restore the last backup** puts the previous save back.

The first time the games are listed the app asks the system about each one, which
is slow; it keeps the answers in `sdmc:/config/opensave/titles.json`, so later
launches only look up games installed since. **R** on the games list discards the
cache and starts again (use it if a game's name looks wrong).

For the PC to know which of its games a Switch save belongs to, it needs the
game's **title ID**. OpenSave reads it from the folder layout the yuzu family of
emulators uses (`.../save/<account>/<profile>/<title id>`).

Looking at a game never changes anything on the PC. If the PC does not track the
game, the Switch says so and offers **Offer to the PC**: that lists the game on
the PC's Home screen, where you choose its save folder. Do that only for a game
whose Switch save you have in an emulator on the PC; the PC will not guess a
folder for it. An emulator that keeps saves elsewhere needs the game linked by
hand in OpenSave's Manage tab.

A PC game with the same name is not the same save: the Windows version of a game
(for example Minecraft Dungeons) keeps a differently shaped save from the Switch
version, and the two cannot be swapped. The Switch only syncs with the *Switch*
save of that title, as an emulator stores it.

Backups are in `sdmc:/switch/OpenSave/backups/<title id>/<timestamp>/` (the three
newest are kept). Settings and this device's key are in
`sdmc:/config/opensave/state.json`.

## Minecraft Dungeons: using the Windows save on the Switch

The Windows and Switch versions of Minecraft Dungeons keep a character as the same
JSON, but Windows encrypts it into a `.dat` file. The app converts between the two
**on the Switch**, with no helper on the PC:

1. On the PC, track the folder that holds the game's Windows save (the one with
   the `<id>.dat` character files, or a folder above it) in OpenSave.
2. On the Switch, open Minecraft Dungeons and choose **Use a Windows save from the
   PC**, then pick that PC game.
3. **Receive from the PC** converts the Windows characters and writes them as
   `Character<id>` files into the Switch save (backed up first).
   **Send to the PC** encrypts the characters you changed on the Switch and the PC
   takes them into the Windows folder, beside the others. Press **R** to refresh.

How it keeps the PC safe: the Switch keeps a **mirror** on the SD card of the PC's
own files (`sdmc:/switch/OpenSave/convert/<title id>/`), and that — the PC's files
plus your Switch changes — is what the PC pulls from. So the PC never sees a
"deletion" of something the Switch simply did not have. Nothing deletes a
character on either side. A character you create on the Switch lands in the folder
where the PC's other characters are; send only after receiving once, so the Switch
knows where that is.

**Conversion is offline.** The Windows game encrypts characters with AES-256 and a
fixed key that the open-source MCDSaveEdit project publishes (`core/mcd_key.h`
records where it comes from). The Switch does the encryption and decryption
itself, so nothing is sent anywhere and no internet beyond your own network is
needed. Every encryption is decrypted again and must match before it is used.
The key belongs to the game's file format, not to this project; check that using
it is acceptable to you.

**Not verified:** the conversion has not been run on a real console. It is
tested against Go's AES, a real Windows character file, and the real PC daemon.
The first receive on your console is the real test: keep the backup it makes.

## Safety, and its limits

A pull is built so a failure cannot leave a half-written save:

1. The current save is copied to the SD card, and the copy is **verified by hash**
   before anything else happens.
2. Every file that has to change is **downloaded to a staging folder first**; each
   block and each whole file is checked against the hashes the PC sent. A dropped
   connection or a corrupt or tampered block fails here, with the save untouched.
3. Only then is the save written, and the result is hashed and compared with the
   PC's. If it differs, the backup is put back automatically.
4. Changes are committed to the console's save data only after all of that passed.

The PC refuses to hand over an emptied save and so does the Switch (it will not
replace a save with nothing). The Switch never lets the PC delete its files.
Sending is the PC's own sync, with all of its safeguards (snapshots, conflict
detection): files the Switch has deleted since the two last agreed are removed on
the PC too, as between any two OpenSave devices, and the PC keeps its history.

Things to know:

- **LAN traffic is not encrypted**, the same as OpenSave's own LAN sync: requests
  are authenticated (signed with a key from pairing, with replay protection), but
  saves cross the network in clear and replies are not authenticated. Use it on a
  network you trust. Hash checks catch corruption and most tampering with data in
  flight; they do not hide it.
- The device key is stored unencrypted on the SD card, as on any Switch homebrew.
- The relay (internet sync) is not supported, only the local network. (The one internet connection the app makes is the optional Minecraft Dungeons conversion above.)
- Some games bind a save to the console or account that made it. A save moved
  between an emulator and a console may not load for those games; that is what the
  backup is for.
- Only a game's main save location syncs. If the PC tracks extra folders for the
  game, the app says so.
- Game titles are drawn in ASCII: a title in another script shows as `?`.
- File names must be plain ASCII; a save with other names is refused rather than
  risk writing a different name than the PC meant.

## What is tested

| Layer | How |
| --- | --- |
| Crypto (SHA-256, HMAC, HKDF, X25519, base64, pairing fingerprint, request MACs) | Known-answer tests whose expected values come from the Go implementation in `internal/e2ee`, plus RFC vectors |
| Manifests and block hashes | Built by the C code and by `internal/delta` for the same folders (empty files, one byte past a block, a 21 MB file with larger blocks, nested and empty folders, hidden files) and required to agree; Go must accept and hash the C side's JSON identically |
| JSON, HTTP | Unit tests, including hostile input (escapes that would truncate a path, oversize and truncated bodies, silent clients) |
| Everything on the wire | `e2e/`: the C client runs against a **real in-process OpenSave daemon**: pairing in both directions (with the fingerprints compared), compare, pull with backup, push, restore, unpairing, and attacks on the Switch's server (unsigned, forged, replayed, re-aimed and stale requests; path traversal; remote deletion) |
| Failure | A proxy truncates a reply and flips a byte in a block; the pull fails and the save is byte-for-byte unchanged |
| Memory safety | All C tests run under AddressSanitizer and UBSan |
| Interface | Every screen is rendered to an image by a PC test (under AddressSanitizer, so any drawing outside the screen fails it) and was looked at; frame cost is measured |
| Title cache | Unit tests: round trip, replace, drop uninstalled games, damaged and wrong-version files |
| Windows-save conversion | `e2e/mcd_test.go`: receive, send, new characters, untouched files staying byte-identical, damaged characters, non-character files, send-before-receive — all against the real daemon, with Go's AES as the independent check. Real console behaviour is **not** tested |
| Console layer | Type-checked against stand-in libnx headers (`make -f Makefile.host check-switch`) — which only checks this project's own code — and built with the real toolchain in CI. **Not run on hardware.** |

## Build and test

Console build (needs [devkitPro](https://devkitpro.org/wiki/Getting_Started) with
devkitA64 and libnx):

```sh
cd opensave-switch
make                      # opensave-switch.nro
```

On a PC (needs gcc, make and Go):

```sh
cd opensave-switch
make -f Makefile.host test            # unit tests, cross-checked against Go
cd .. && go test ./opensave-switch/e2e/ -v
```

`make -f Makefile.host hostcli` builds a terminal driver for the same core
(`tools/hostcli.c`), handy for trying the protocol against a daemon without a
console.

## Layout

```
core/      portable C: crypto, JSON, HTTP, manifests, pairing state, client, server, sync, title cache
ui/        portable renderer and screens (drawn into a pixel buffer; ASCII font generated from
           Liberation Sans, licence in FONT-LICENSE.txt)
switch/    the console layer: save mounting and the game list (libnx), the input loop
tools/     hostcli (terminal driver); Go helpers that produce the expected values for tests
tests/     unit tests and the vectors they check against
e2e/       Go end-to-end tests against a real daemon
```

## How it talks to the PC

It speaks OpenSave's LAN protocol (`/api/p2p/*`): `ping`, `handshake` and
`approve-confirm` to pair; then signed `manifest`, `blocks`, `games`, `sync-event`
and `sync/trigger` calls. A request carries `X-Opensave-Peer`, `-Nonce`,
`-Auth-Ms` and `-Auth` headers, the last an HMAC-SHA-256 over sender, receiver,
request target, method, body, nonce and time, keyed with a secret derived from an
X25519 key agreement made at pairing (`internal/e2ee/auth.go`). Sending a save to
the PC works the way it does between two PCs: the Switch tells the PC it has newer
content, and the PC pulls it from the Switch's own small server. Each request about
a game uses the PC's *own* id for it, found from the PC's game list by title ID.
