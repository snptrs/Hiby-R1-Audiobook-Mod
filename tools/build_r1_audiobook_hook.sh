#!/usr/bin/env bash
# macOS/Linux port of build_r1_audiobook_hook.ps1.
#
# Builds the LD_PRELOAD hook library for the device. Zig cross-compiles from
# any host, so this produces the same mipsel target as the Windows script; the
# only difference is where zig comes from (PATH here, .deps on Windows).
#
# Keep the compiler flags identical to the .ps1. Each one is load-bearing:
#   gnueabihf     hiby_player is glibc + hard-float; ld.so refuses to load a
#                 soft-float .so into a hard-float process.
#   .2.22         pins the glibc ABI. Zig defaults to 2.33+, which fails on the
#                 device's glibc 2.22 with "GLIBC_2.28 not found".
#   -fvisibility=hidden
#                 hides sqlite3_* / audiobook_* so they cannot shadow
#                 hiby_player's own SQLite. Constructors are found via
#                 .init_array, not symbol lookup.
#
# Usage: tools/build_r1_audiobook_hook.sh [output.so]

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$REPO/work/native-app/libaudiobook_hook.so}"
ZIG="${ZIG:-zig}"

command -v "$ZIG" >/dev/null || { echo "zig not found (brew install zig)" >&2; exit 1; }

# Zig ships the Linux kernel headers the sources need; find them next to the
# compiler rather than assuming a Homebrew layout. `zig env` emits ZON (not
# JSON) as of 0.16, so scrape the one field instead of parsing it.
ZIG_LIB="$("$ZIG" env | sed -n 's/^[[:space:]]*\.lib_dir = "\(.*\)",\{0,1\}$/\1/p' | head -1)"
[ -n "$ZIG_LIB" ] || { echo "could not read lib_dir from 'zig env'" >&2; exit 1; }
KERNEL_INCLUDE="$ZIG_LIB/libc/include/any-linux-any"
[ -d "$KERNEL_INCLUDE" ] || { echo "kernel headers not found: $KERNEL_INCLUDE" >&2; exit 1; }

SOURCES=(
  hook.c ui.c render.c font.c library.c scan.c tags.c mp4_audio.c
  cover.c pngdec.c bookmark_sd.c storage_guard.c music_catalog.c
  player.c wsola.c sqlite3.c
)
SRC_PATHS=()
for s in "${SOURCES[@]}"; do
  p="$REPO/audiobook_app/$s"
  [ -f "$p" ] || { echo "source not found: $p" >&2; exit 1; }
  SRC_PATHS+=("$p")
done

mkdir -p "$(dirname "$OUT")"

"$ZIG" cc \
  -target mipsel-linux-gnueabihf.2.22 \
  -shared -fPIC -fvisibility=hidden -fno-common -Os -s \
  -I "$KERNEL_INCLUDE" \
  -I "$REPO/vendor" \
  -I "$REPO/vendor/libjpeg" \
  -DSQLITE_THREADSAFE=2 \
  -DSQLITE_DEFAULT_MEMSTATUS=0 \
  -DSQLITE_OMIT_LOAD_EXTENSION=1 \
  -DSQLITE_ENABLE_FTS5=1 \
  -DSQLITE_OMIT_DEPRECATED=1 \
  -DSQLITE_TEMP_STORE=2 \
  "${SRC_PATHS[@]}" \
  -lpthread -ldl -lm \
  -o "$OUT"

echo "Built: $OUT ($(wc -c <"$OUT" | tr -d ' ') bytes)"
