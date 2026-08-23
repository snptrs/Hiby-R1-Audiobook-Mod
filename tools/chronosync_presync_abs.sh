#!/bin/bash
# ChronoSync PRE-sync hook: push HiBy R1 podcast listening state into
# Audiobookshelf before the sync runs.
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
# ChronoSync runs scripts with a minimal environment, so everything below is an
# absolute path and nothing depends on the working directory.
#
# Setup: in the ChronoSync document, Options -> Scripts -> "Run script before
# synchronize", point it at this file.

set -u

REPO="/Users/seanpeters/Code/Repos/Hiby-R1-Audiobook-Mod"
PYTHON="/usr/bin/python3"
SCRIPT="$REPO/tools/abs_sync_listened.py"

ABS_URL="http://seans-imac:13378/audiobookshelf"
LIBRARY_ID="e92e1153-f83a-4a6a-a3f6-235758222e67"
TOKEN_FILE="$HOME/.config/abs/token"

LOG="$HOME/Library/Logs/hiby-abs-sync.log"
MAX_LOG_BYTES=1048576

mkdir -p "$(dirname "$LOG")" 2>/dev/null

# Keep the log from growing without bound; one rotation is plenty for a
# per-sync script.
if [ -f "$LOG" ]; then
  size=$(wc -c <"$LOG" 2>/dev/null | tr -d ' ')
  if [ -n "$size" ] && [ "$size" -gt "$MAX_LOG_BYTES" ]; then
    mv -f "$LOG" "$LOG.1" 2>/dev/null
  fi
fi

{
  echo "=== $(date '+%Y-%m-%d %H:%M:%S') pre-sync ==="
  if [ ! -x "$PYTHON" ]; then
    echo "python3 not found at $PYTHON; skipping"
  elif [ ! -f "$SCRIPT" ]; then
    echo "sync script not found at $SCRIPT; skipping"
  elif [ ! -f "$TOKEN_FILE" ]; then
    echo "no ABS token at $TOKEN_FILE; skipping"
  else
    # HIBY_ABS_DRY_RUN=1 to exercise the whole hook without writing anything.
    # Worth using the first time you wire this into ChronoSync.
    extra=""
    if [ "${HIBY_ABS_DRY_RUN:-0}" = "1" ]; then
      extra="--dry-run"
      echo "(dry run: HIBY_ABS_DRY_RUN=1)"
    fi
    "$PYTHON" "$SCRIPT" \
      --abs-url "$ABS_URL" \
      --library-id "$LIBRARY_ID" \
      --token-file "$TOKEN_FILE" \
      $extra 2>&1
    echo "(script exit: $?)"
  fi
} >>"$LOG" 2>&1

exit 0
