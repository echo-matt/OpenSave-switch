#!/usr/bin/env bash
# Cross-checks the C manifest code against internal/delta: builds a fixture
# folder, has Go describe it, and requires the C side to agree exactly — and
# Go to accept and hash the C side's JSON the same way.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(cd .. && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

FX="$WORK/save"
mkdir -p "$FX/slots/deep/er" "$FX/emptydir" "$FX/.hidden" "$FX/.git"
: > "$FX/empty.dat"
printf 'tiny' > "$FX/small.bin"
head -c 65536 /dev/urandom > "$FX/exact-block.dat"        # exactly one block
head -c 65537 /dev/urandom > "$FX/block-plus-one.dat"     # a one-byte second block
head -c 200000 /dev/urandom > "$FX/slots/slot0.dat"
head -c 3000 /dev/urandom > "$FX/slots/deep/er/slot1.dat"
head -c 21000000 /dev/urandom > "$FX/medium.dat"          # >20 MiB: 512 KiB blocks
printf 'ignore me' > "$FX/.hidden/secret"
printf 'ignore me' > "$FX/.dotfile"
printf 'ignore me' > "$FX/leftover.opensave.tmp"
printf 'x' > "$FX/ünïcode-is-skipped-by-ascii-check.dat" 2>/dev/null || true
rm -f "$FX"/ünï*   # non-ASCII names are refused on the console; keep the fixture to what it accepts

(cd "$ROOT" && go build -o "$WORK/gomanifest" ./opensave-switch/tools/gomanifest)
"$WORK/gomanifest" build "$FX" > "$WORK/go.json"

gcc -std=gnu99 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,undefined \
    -fno-sanitize-recover=undefined -o "$WORK/test_manifest" \
    tests/test_manifest.c core/manifest.c core/json.c core/crypto.c core/fsutil.c
"$WORK/test_manifest" "$FX" "$WORK/go.json" "$WORK/ours.json" | tee "$WORK/out.txt"

CHASH="$(sed -n 's/^manifest hash //p' "$WORK/out.txt")"
GHASH="$("$WORK/gomanifest" hash < "$WORK/ours.json")"
if [ "$CHASH" != "$GHASH" ]; then
    echo "FAIL: Go computes $GHASH from the C side's JSON, C computed $CHASH" >&2
    exit 1
fi
echo "Go accepts the C manifest JSON and hashes it identically ($GHASH)"
