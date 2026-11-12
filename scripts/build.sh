#!/usr/bin/env bash
# Cross-compiles out/ImageShrinker.exe (Windows x64) with zig cc.
# Usage: scripts/build.sh 1.2.0        (run scripts/fetch_deps.sh first)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${1:-${VERSION:-0.0.0}}"
if ! [[ "$VERSION" =~ ^[0-9]{1,4}\.[0-9]{1,4}\.[0-9]{1,4}$ ]]; then
  echo "Version must look like 1.2.0 (got '$VERSION')" >&2; exit 1
fi
D="$ROOT/deps"; B="$ROOT/build"; O="$B/obj"; S="$ROOT/src"
mkdir -p "$O" "$ROOT/out"
ZIG="${ZIG:-python3 -m ziglang}"
CC="$ZIG cc -target x86_64-windows-gnu"
CF="-O2 -w -ffunction-sections -fdata-sections"
JCFG="$ROOT/third_party/mozjpeg-config"

# 1) codec libraries -> build/libcodecs.a (only rebuilt when missing)
if [ ! -f "$B/libcodecs.a" ]; then
  J="$D/mozjpeg"; W="$D/libwebp"
  J16="jcapistd jccolor jcdiffct jclossls jcmainct jcprepct jcsample jdapistd jdcolor jddiffct jdlossls jdmainct jdpostct jdsample jutils"
  J12="$J16 jccoefct jcdctmgr jdcoefct jddctmgr jdmerge jfdctfst jfdctint jidctflt jidctfst jidctint jidctred jquant1 jquant2"
  J8="$J12 jcapimin jchuff jcicc jcinit jcext jclhuff jcmarker jcmaster jcomapi jcparam jcphuff jctrans jdapimin jdatadst jdatasrc jdhuff jdicc jdinput jdlhuff jdmarker jdmaster jdphuff jdtrans jerror jfdctflt jmemmgr jmemnobs jpeg_nbits jaricom jcarith jdarith"
  {
    for s in $J8;  do echo "$CC $CF -I$JCFG -I$J -c $J/$s.c -o $O/j8_$s.o"; done
    for s in $J12; do echo "$CC $CF -DBITS_IN_JSAMPLE=12 -I$JCFG -I$J -c $J/$s.c -o $O/j12_$s.o"; done
    for s in $J16; do echo "$CC $CF -DBITS_IN_JSAMPLE=16 -I$JCFG -I$J -c $J/$s.c -o $O/j16_$s.o"; done
    for f in $W/src/dec/*.c $W/src/enc/*.c $W/src/dsp/*.c $W/src/utils/*.c $W/src/mux/*.c $W/sharpyuv/*.c; do
      n=$(echo "${f#$W/}" | tr '/' '_'); echo "$CC $CF -I$W -I$W/src -c $f -o $O/w_${n%.c}.o"; done
    echo "$CC $CF -x c -c $D/lodepng/lodepng.cpp -o $O/lodepng.o"
  } > "$B/cmds.txt"
  xargs -P "$(nproc)" -I{} sh -c '{}' < "$B/cmds.txt"
  rm -f "$B/libcodecs.a"
  $ZIG ar rcs "$B/libcodecs.a" "$O"/*.o
fi

# 2) resources with the version number
IFS=. read -r V1 V2 V3 <<< "$VERSION"
sed -e "s|@VERSION@|$VERSION|g; s|@VERSION_COMMAS@|$V1,$V2,$V3,0|g; s|@SRC@|$S|g" "$S/app.rc.in" > "$B/app.rc"
$ZIG rc /nologo /fo "$B/app.res" -- "$B/app.rc"

# 3) the exe
rm -f "$ROOT/out/ImageShrinker.exe"
INC="-I$JCFG -I$D/mozjpeg -I$D/libwebp/src -I$D/stb -I$D/stb_image_resize -I$D/lodepng"
$CC -O2 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -ffunction-sections -fdata-sections $INC \
  -DAPP_VERSION="\"$VERSION\"" -municode \
  "$S/gui.c" "$S/core.c" "$S/zipw.c" "$S/update.c" "$S/preview.c" "$S/shellcopy.c" "$B/app.res" "$B/libcodecs.a" \
  -lcomctl32 -luuid -lwinhttp -lcomdlg32 -lshell32 -lole32 -luser32 -lgdi32 \
  -Wl,--subsystem,windows -Wl,--gc-sections -s -o "$ROOT/out/ImageShrinker.exe" 2>&1 | { grep -v "deps/" || true; }
test -s "$ROOT/out/ImageShrinker.exe"
echo "Built out/ImageShrinker.exe  version $VERSION  ($(stat -c %s "$ROOT/out/ImageShrinker.exe") bytes)"
