#!/bin/sh
# Vendor SameBoy boot ROM binaries into cli/bootroms/.
# Strategy 1: build with RGBDS if available (make -C SameBoy bootroms).
# Strategy 2: extract from the official SameBoy release assets.
set -e
cd "$(dirname "$0")/.."
DEST=cli/bootroms
mkdir -p "$DEST"

need="dmg_boot.bin cgb_boot.bin agb_boot.bin sgb_boot.bin sgb2_boot.bin mgb_boot.bin"
missing=""
for f in $need; do [ -f "$DEST/$f" ] || missing="$missing $f"; done
[ -z "$missing" ] && { echo "boot ROMs already present:"; ls "$DEST"; exit 0; }

if command -v rgbasm >/dev/null 2>&1; then
    echo "Building boot ROMs with RGBDS..."
    make -C SameBoy bootroms
    for f in $missing; do
        for src in "SameBoy/build/bin/BootROMs/$f" "SameBoy/build/BootROMs/$f"; do
            [ -f "$src" ] && cp "$src" "$DEST/" && echo "  $f" && break
        done
    done
fi

missing=""
for f in $need; do [ -f "$DEST/$f" ] || missing="$missing $f"; done
[ -z "$missing" ] && exit 0

echo "Fetching compiled boot ROMs from official SameBoy release assets..."
VER=$(sed -n 's/^VERSION *:= *//p' SameBoy/version.mk)
TMPD=$(mktemp -d)
trap 'rm -rf "$TMPD"' EXIT
curl -fSL -o "$TMPD/sameboy.zip" "https://github.com/LIJI32/SameBoy/releases/download/v${VER}/SameBoy-full.zip" \
  || curl -fSL -o "$TMPD/sameboy.zip" "https://github.com/LIJI32/SameBoy/releases/download/v${VER}/SameBoy.zip"
cd "$TMPD" && unzip -qo sameboy.zip
found=$(find . -name '*_boot.bin' | head -20)
cd - >/dev/null
for f in $found; do
    base=$(basename "$f")
    cp "$TMPD/$f" "$DEST/$base" 2>/dev/null || cp "$f" "$DEST/" && echo "  $base"
done
missing=""
for f in $need; do [ -f "$DEST/$f" ] || missing="$missing $f"; done
if [ -n "$missing" ]; then
    echo "WARNING: still missing:$missing"
    echo "gbemu can run DMG roms with boot:\"builtin\" regardless."
else
    echo "All boot ROMs present in $DEST"
fi
