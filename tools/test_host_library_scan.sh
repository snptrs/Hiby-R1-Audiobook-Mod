#!/usr/bin/env bash
# Host test driver for the library/scanner layer: builds the C test binaries
# with clang and runs them against a synthetic Audiobooks + Podcasts tree.
#
# Runs on macOS or Linux. The .ps1 build scripts are Windows/Zig only, and
# ui.c / player.c cannot be built here at all (they include <linux/input.h>),
# so this covers library.c, scan.c and tags.c only. Everything in the UI and
# player layers needs the device build.
#
# Requires: clang, ffmpeg (to synthesize the audio files).
# Usage: tools/test_host_library_scan.sh [workdir]

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${1:-$(mktemp -d -t r1-host-scan)}"
mkdir -p "$WORK"
cd "$REPO"

SQLITE_DEFS=(
  -DSQLITE_THREADSAFE=2
  -DSQLITE_DEFAULT_MEMSTATUS=0
  -DSQLITE_OMIT_LOAD_EXTENSION=1
  -DSQLITE_ENABLE_FTS5=1
  -DSQLITE_OMIT_DEPRECATED=1
  -DSQLITE_TEMP_STORE=2
)
# Warnings are ON here deliberately: the shipped build uses no -Wall, so this
# driver is the only place a missed switch case or bad format string surfaces.
CFLAGS=(-O1 -Wall -Wextra -Wno-unused-parameter -Iaudiobook_app -Ivendor)

SCAN_SRCS=(
  audiobook_app/library.c
  audiobook_app/scan.c
  audiobook_app/tags.c
  audiobook_app/bookmark_sd.c
  audiobook_app/library_test_stubs.c
  audiobook_app/sqlite3.c
)

# ui.c / hook.c / render.c cannot be linked on a host, but they CAN be
# typechecked against tools/hostshim. Worth doing: the shipped build passes no
# -Wall, and ui.c has several switches whose missing cases fail silently at
# runtime rather than loudly at build time.
echo "=== typecheck (incl. files the device build compiles without warnings) ==="
tc_fail=0
for f in ui.c hook.c render.c player.c cover.c library.c scan.c tags.c \
         music_catalog.c bookmark_sd.c; do
  if ! clang -fsyntax-only -Wall -Wextra -Wno-unused-parameter \
       -Wno-deprecated-declarations -DSO_DOMAIN=39 -DSO_PEERCRED=17 \
       -Itools/hostshim -Iaudiobook_app -Ivendor -Ivendor/libjpeg \
       "audiobook_app/$f" 2>"$WORK/tc.$f.log"; then
    echo "  FAIL $f"; sed -n '1,6p' "$WORK/tc.$f.log"; tc_fail=1
  else
    echo "  ok   $f"
  fi
done
# Catches a list_mode_t / ui_screen_t case added to the enum but not to a
# switch, which -Wall misses whenever the switch has a default.
clang -fsyntax-only -Wno-everything -Wswitch-enum -Itools/hostshim \
  -Iaudiobook_app -Ivendor audiobook_app/ui.c 2>&1 \
  | grep -E 'LIST_|SCREEN_' | sed 's/^/  switch-enum: /' || true
[ "$tc_fail" = "0" ] || { echo "TYPECHECK FAILED"; exit 1; }
echo

echo "=== building ==="
clang "${CFLAGS[@]}" "${SQLITE_DEFS[@]}" -o "$WORK/library_kind_test" \
  tools/library_kind_test_main.c audiobook_app/library.c \
  audiobook_app/bookmark_sd.c audiobook_app/sqlite3.c -lpthread
clang "${CFLAGS[@]}" "${SQLITE_DEFS[@]}" -o "$WORK/scan_orphan_test" \
  tools/scan_orphan_test_main.c "${SCAN_SRCS[@]}" -lpthread
clang "${CFLAGS[@]}" "${SQLITE_DEFS[@]}" -o "$WORK/library_test" \
  audiobook_app/library_test.c "${SCAN_SRCS[@]}" -lpthread
clang "${CFLAGS[@]}" "${SQLITE_DEFS[@]}" -o "$WORK/music_catalog_test" \
  tools/music_catalog_cleanup_test_main.c audiobook_app/music_catalog.c \
  audiobook_app/sqlite3.c -lpthread
echo "ok"
echo

mktone() { # path freq seconds [artist] [title]
  local out="$1" freq="$2" secs="$3" artist="${4:-}" title="${5:-}"
  local args=()
  [ -n "$artist" ] && args+=(-metadata "artist=$artist")
  [ -n "$title" ] && args+=(-metadata "title=$title")
  mkdir -p "$(dirname "$out")"
  # ${args[@]+...} guards the empty-array case: macOS ships bash 3.2, where a
  # bare "${args[@]}" trips set -u when the array has no elements.
  ffmpeg -loglevel quiet -f lavfi -i "sine=frequency=$freq:duration=$secs" \
    ${args[@]+"${args[@]}"} "$out" -y
}

build_tree() {
  local tree="$1"
  rm -rf "$tree"
  # /Audiobooks/<Author>/<Series>/<leaf>/ is the documented convention.
  local ab="$tree/Audiobooks"
  for i in 01 02 03; do
    mktone "$ab/David Sedaris/Essays/2008 - Calypso/$i - part.mp3" 440 2 \
      "David Sedaris" "Part $i"
  done
  mktone "$ab/Jane Author/Single Book/book.mp3" 300 3 "Jane Author" "Whole Book"

  local pc="$tree/Podcasts"
  # Date-prefixed names: the case reverse-index ordering actually gets right.
  for n in "2026-08-01 First Episode" "2026-08-08 Second Episode" \
           "2026-08-15 Third Episode"; do
    mktone "$pc/Tech Show/$n.mp3" 500 2 "Tech Show"
  done
  # book_key collision case: all three sanitize to the same string, so without
  # the hash suffix two of them would silently vanish.
  for n in "Ep 1" "Ep-1" "Ep.1"; do
    mktone "$pc/History Show/2024/$n.mp3" 600 1
  done
  # Same folder basename as above under a different show: must not merge, since
  # series.display_name is UNIQUE.
  mktone "$pc/Other Show/2024/Only One.mp3" 700 1
  # Flat show-less file: the degenerate layout.
  mktone "$pc/loose episode.mp3" 800 1
}

fail=0
run() { # label cmd...
  local label="$1"; shift
  echo "=== $label ==="
  if "$@"; then echo "PASS: $label"; else echo "FAIL: $label"; fail=1; fi
  echo
}

echo "=== synthesizing tree ==="
build_tree "$WORK/tree"
find "$WORK/tree" -name '*.mp3' | wc -l | xargs echo "  audio files:"
echo

run "books.kind schema + migration + queries" \
  "$WORK/library_kind_test" "$WORK/kind.db"

run "stock Music catalog isolation" \
  python3 tools/test_music_catalog_cleanup.py --helper "$WORK/music_catalog_test"

# Rebuilt because the orphan test deletes an episode file and moves the
# audiobook tree around.
build_tree "$WORK/tree"
rm -f "$WORK/orphan.db"
run "orphan cleanup scoping + zero-track rows" \
  "$WORK/scan_orphan_test" "$WORK/tree" "$WORK/orphan.db"

build_tree "$WORK/tree"
echo "=== audiobook scan (regression: unchanged grouping) ==="
rm -f "$WORK/ab.db"
"$WORK/library_test" "$WORK/tree/Audiobooks" "$WORK/ab.db" > "$WORK/ab.out"
ab_books=$(sed -n 's/^Total books: //p' "$WORK/ab.out")
echo "  books: $ab_books (expected 2, i.e. one per folder not per file)"
[ "$ab_books" = "2" ] || { echo "FAIL: audiobook grouping changed"; fail=1; }
# "2008 - Calypso" must still lose its year prefix, and the depth-2 ancestor
# must still become the series. Both go through the now-parameterized root.
grep -q '\[1\] Calypso' "$WORK/ab.out" || { echo "FAIL: title not cleaned"; fail=1; }
grep -q 'series: Essays' "$WORK/ab.out" || { echo "FAIL: series not derived"; fail=1; }
grep -q 'author: David Sedaris' "$WORK/ab.out" || { echo "FAIL: author not derived"; fail=1; }
# One chapter per track for a multi-file book with no embedded chapters.
grep -q 'ch 3: Part 03' "$WORK/ab.out" || { echo "FAIL: chapter synthesis changed"; fail=1; }
# FTS still indexes and matches.
grep -q "Search Test" "$WORK/ab.out" || { echo "FAIL: no search section"; fail=1; }
[ "$fail" = "0" ] && echo "PASS: audiobook scan"
echo

echo "=== podcast scan ==="
rm -f "$WORK/pc.db"
"$WORK/library_test" --podcast "$WORK/tree/Podcasts" "$WORK/pc.db" > "$WORK/pc.out"
eps=$(grep -c 'key=' "$WORK/pc.out" || true)
echo "  episode rows: $eps (expected 8)"
[ "$eps" = "8" ] || { echo "FAIL: expected one row per file"; fail=1; }
# The three colliding names must all survive as distinct rows.
colliding=$(grep -cE '^ +[0-9.]+ +Ep[ .-]?1 ' "$WORK/pc.out" || true)
echo "  colliding-name episodes kept: $colliding (expected 3)"
[ "$colliding" = "3" ] || { echo "FAIL: book_key collision lost episodes"; fail=1; }
# Shows must not merge on basename.
grep -q 'Show: History Show/2024' "$WORK/pc.out" || { echo "FAIL: show not root-relative"; fail=1; }
grep -q 'Show: Other Show/2024' "$WORK/pc.out" || { echo "FAIL: shows merged on basename"; fail=1; }
# Date prefixes must survive (clean_book_title would eat the year).
grep -q '2026-08-15 Third Episode' "$WORK/pc.out" \
  || { echo "FAIL: date prefix stripped from episode title"; fail=1; }
# No leakage into the audiobook views.
grep -q 'Titles:  0' "$WORK/pc.out" || { echo "FAIL: episodes leak into Titles"; fail=1; }
grep -q 'Series:  0' "$WORK/pc.out" || { echo "FAIL: shows leak into Series"; fail=1; }
grep -q 'Folders: 0' "$WORK/pc.out" || { echo "FAIL: episodes leak into Folders"; fail=1; }
echo

if [ "$fail" = "0" ]; then
  echo "ALL HOST TESTS PASSED  (workdir: $WORK)"
else
  echo "SOME HOST TESTS FAILED  (workdir: $WORK)"
fi
exit "$fail"
