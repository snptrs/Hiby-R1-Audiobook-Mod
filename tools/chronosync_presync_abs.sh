#!/bin/bash
# ChronoSync PRE-sync hook: reconcile HiBy R1 listening state with
# Audiobookshelf before the sync runs. Two-way by default, covering both
# podcasts and audiobooks.
#
# Pre-sync, not post-sync, so the ordering works out:
#   1. this marks played episodes finished in ABS
#   2. ABS drops played episodes from disk per its retention settings
#   3. the sync then propagates those deletions to the card
# Deletions flow Mac -> card through the normal sync, and the device never
# deletes anything itself.
#
# ALWAYS exits 0. ChronoSync can be configured to abort a sync when a pre-sync
# script fails, and a transient ABS outage must not stop new episodes reaching
# the card. Check the log if listened state stops arriving.
#
# Needs nothing installed: abs_sync_listened.py is standard library only, so the
# system python3 is enough. No Homebrew, no pip, no firmware build toolchain.
#
# Nothing in here is machine-specific, so the same file works on every Mac that
# syncs to the device. Per-machine values go in the optional config file below.
#
# Setup:
#   1. write the ABS API token to ~/.config/abs/token (chmod 600)
#   2. optionally override the defaults in ~/.config/abs/config, e.g.
#        HIBY_ABS_URL=http://seans-imac.local:13378/audiobookshelf
#        HIBY_ABS_LIBRARY_ID=e92e1153-f83a-4a6a-a3f6-235758222e67
#   3. in the ChronoSync document: Options -> Scripts -> "Run script before
#      synchronize", pointing at this file
#
# Test it without writing anything:
#   HIBY_ABS_DRY_RUN=1 bash tools/chronosync_presync_abs.sh
#   tail -20 ~/Library/Logs/hiby-abs-sync.log

set -u

# Resolve this script's own directory so the repo can live anywhere. ChronoSync
# runs scripts with a minimal environment and an arbitrary working directory,
# so nothing here may depend on either.
SELF="${BASH_SOURCE[0]}"
while [ -L "$SELF" ]; do SELF="$(readlink "$SELF")"; done
TOOLS="$(cd "$(dirname "$SELF")" && pwd)"
SCRIPT="$TOOLS/abs_sync_listened.py"

CONFIG="$HOME/.config/abs/config"
# shellcheck source=/dev/null
[ -f "$CONFIG" ] && . "$CONFIG"

ABS_URL="${HIBY_ABS_URL:-http://seans-imac:13378/audiobookshelf}"
LIBRARY_ID="${HIBY_ABS_LIBRARY_ID:-e92e1153-f83a-4a6a-a3f6-235758222e67}"
BOOK_LIBRARY_ID="${HIBY_ABS_BOOK_LIBRARY_ID:-a7a8932b-049d-46ce-84eb-61effc98cfee}"
TOKEN_FILE="${HIBY_ABS_TOKEN_FILE:-$HOME/.config/abs/token}"
# both = two-way. This is the only setting that lets the script WRITE to the
# card (.pos files only, never media). Set to push for read-only behaviour.
DIRECTION="${HIBY_ABS_DIRECTION:-both}"

# System python3 first: it is the one guaranteed to exist, and a Homebrew
# upgrade cannot break the hook.
PYTHON="/usr/bin/python3"
[ -x "$PYTHON" ] || PYTHON="$(command -v python3 2>/dev/null)"

LOG="${HIBY_ABS_LOG:-$HOME/Library/Logs/hiby-abs-sync.log}"
MAX_LOG_BYTES=1048576

mkdir -p "$(dirname "$LOG")" 2>/dev/null

if [ -f "$LOG" ]; then
  size=$(wc -c <"$LOG" 2>/dev/null | tr -d ' ')
  if [ -n "$size" ] && [ "$size" -gt "$MAX_LOG_BYTES" ]; then
    mv -f "$LOG" "$LOG.1" 2>/dev/null
  fi
fi

{
  # Record which commit produced this run. After a git pull, the log is then
  # the answer to "is ChronoSync actually running the new code?" without having
  # to infer it from which messages appear. Best-effort: a copy taken outside a
  # git checkout just shows "unknown".
  rev="$(git -C "$TOOLS" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  dirty=""
  git -C "$TOOLS" diff --quiet -- "$TOOLS" 2>/dev/null || dirty="+local-changes"
  echo "=== $(date '+%Y-%m-%d %H:%M:%S') pre-sync on $(hostname -s)" \
       "[$rev$dirty] ==="
  if [ -z "${PYTHON:-}" ] || [ ! -x "$PYTHON" ]; then
    echo "no python3 found; skipping"
  elif [ ! -f "$SCRIPT" ]; then
    echo "sync script not found at $SCRIPT; skipping"
  elif [ ! -f "$TOKEN_FILE" ]; then
    echo "no ABS token at $TOKEN_FILE; skipping"
  else
    extra=""
    if [ "${HIBY_ABS_DRY_RUN:-0}" = "1" ]; then
      extra="--dry-run"
      echo "(dry run: HIBY_ABS_DRY_RUN=1)"
    fi
    book_arg=""
    [ -n "${BOOK_LIBRARY_ID:-}" ] && book_arg="--book-library-id $BOOK_LIBRARY_ID"
    "$PYTHON" "$SCRIPT" \
      --abs-url "$ABS_URL" \
      --library-id "$LIBRARY_ID" \
      $book_arg \
      --direction "$DIRECTION" \
      --token-file "$TOKEN_FILE" \
      $extra 2>&1
    echo "(script exit: $?)"
  fi
} >>"$LOG" 2>&1

exit 0
