#!/usr/bin/env bash
# macOS/Linux port of build_r1_audiobook_app.ps1.
#
# Builds the small native helper installed at /usr/bin/r1_audiobook_app. It is
# statically linked against musl (unlike the hook, which must match
# hiby_player's glibc + hard-float because it is LD_PRELOADed into it).
#
# Usage: tools/build_r1_audiobook_app.sh [output]

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$REPO/work/native-app/r1_audiobook_smoke}"
ZIG="${ZIG:-zig}"

command -v "$ZIG" >/dev/null || { echo "zig not found (brew install zig)" >&2; exit 1; }

ZIG_LIB="$("$ZIG" env | sed -n 's/^[[:space:]]*\.lib_dir = "\(.*\)",\{0,1\}$/\1/p' | head -1)"
[ -n "$ZIG_LIB" ] || { echo "could not read lib_dir from 'zig env'" >&2; exit 1; }
KERNEL_INCLUDE="$ZIG_LIB/libc/include/any-linux-any"

mkdir -p "$(dirname "$OUT")"

"$ZIG" cc \
  -target mipsel-linux-musleabi \
  -static -Os -s \
  -I "$KERNEL_INCLUDE" \
  "$REPO/audiobook_app/smoke.c" \
  -lm \
  -o "$OUT"

echo "Built: $OUT ($(wc -c <"$OUT" | tr -d ' ') bytes)"
