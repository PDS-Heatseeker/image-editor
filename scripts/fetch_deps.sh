#!/usr/bin/env bash
# Downloads the exact library versions Image Shrinker is built with into deps/.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/deps"
fetch() {   # name url commit
  local dir="$ROOT/deps/$1"
  if [ -d "$dir/.git" ] && [ "$(git -C "$dir" rev-parse HEAD)" = "$3" ]; then return; fi
  rm -rf "$dir"; mkdir -p "$dir"
  git -C "$dir" init -q
  git -C "$dir" fetch -q --depth 1 "$2" "$3"
  git -C "$dir" checkout -q FETCH_HEAD
  echo "fetched $1 @ ${3:0:10}"
}
fetch mozjpeg          https://github.com/mozilla/mozjpeg              08265790774cd0714832c9e675522acbe5581437
fetch libwebp          https://github.com/webmproject/libwebp          03816d47cfa96a063a771e1cb52c253f4577bc1d
fetch stb              https://github.com/nothings/stb                 78770d85c4d94787a56c7859017da963bc4f3439
fetch stb_image_resize https://github.com/nothings/stb_image_resize    703100c5f8a0b75b4861e401db567f1d82d545b8
fetch lodepng          https://github.com/lvandeve/lodepng             7b2893e35891c0c85bee5595caafaf3c2044b409
