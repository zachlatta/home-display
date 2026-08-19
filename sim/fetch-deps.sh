#!/bin/sh
# Download the exact upstream libraries the firmware builds against, so the
# simulator renders with the same glyph rasteriser, the same fonts, and the same
# JSON parser as the device. Vendored copies are gitignored.
set -eu

GFX_VERSION=1.12.6
ARDUINOJSON_VERSION=7.4.3

SIM_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
VENDOR="$SIM_DIR/vendor"
STAMP="$VENDOR/.stamp-$GFX_VERSION-$ARDUINOJSON_VERSION"

if [ -f "$STAMP" ]; then
  exit 0
fi

echo "sim: fetching Adafruit_GFX $GFX_VERSION and ArduinoJson $ARDUINOJSON_VERSION"
rm -rf "$VENDOR"
mkdir -p "$VENDOR"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

curl -fsSL -o "$tmp/gfx.tar.gz" \
  "https://github.com/adafruit/Adafruit-GFX-Library/archive/refs/tags/$GFX_VERSION.tar.gz"
tar -xzf "$tmp/gfx.tar.gz" -C "$tmp"
mkdir -p "$VENDOR/Adafruit_GFX"
cp "$tmp/Adafruit-GFX-Library-$GFX_VERSION/Adafruit_GFX.cpp" \
   "$tmp/Adafruit-GFX-Library-$GFX_VERSION/Adafruit_GFX.h" \
   "$tmp/Adafruit-GFX-Library-$GFX_VERSION/gfxfont.h" \
   "$tmp/Adafruit-GFX-Library-$GFX_VERSION/glcdfont.c" \
   "$VENDOR/Adafruit_GFX/"
cp -R "$tmp/Adafruit-GFX-Library-$GFX_VERSION/Fonts" "$VENDOR/Adafruit_GFX/Fonts"
cp "$tmp/Adafruit-GFX-Library-$GFX_VERSION/license.txt" "$VENDOR/Adafruit_GFX/" 2>/dev/null || true

curl -fsSL -o "$VENDOR/ArduinoJson.h" \
  "https://github.com/bblanchon/ArduinoJson/releases/download/v$ARDUINOJSON_VERSION/ArduinoJson-v$ARDUINOJSON_VERSION.h"

touch "$STAMP"
echo "sim: vendored dependencies ready in $VENDOR"
