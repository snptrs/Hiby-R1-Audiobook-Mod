#!/usr/bin/env bash
# macOS/Linux port of extract_r1_firmware.ps1.
#
# A HiBy .upt is an ISO holding the kernel and rootfs split into 512 KiB chunks
# with MD5-chained names. Extracting is: unpack the ISO, then concatenate each
# image's chunks in filename order (the NNNN counter sorts correctly).
#
# Usage: tools/extract_r1_firmware.sh <stock-r1.upt> [outdir]
# Default outdir is work/original, which is what the firmware build expects.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UPT="${1:?usage: extract_r1_firmware.sh <stock-r1.upt> [outdir]}"
OUT="${2:-$REPO/work/original}"

[ -f "$UPT" ] || { echo "package not found: $UPT" >&2; exit 1; }

SEVENZIP="${SEVENZIP:-7zz}"
command -v "$SEVENZIP" >/dev/null || {
  echo "7-Zip not found (brew install sevenzip)" >&2; exit 1; }

EXTRACT="$OUT/upt"
mkdir -p "$OUT"
rm -rf "$EXTRACT"
mkdir -p "$EXTRACT"

"$SEVENZIP" x "$UPT" "-o$EXTRACT" -y >/dev/null

OTA="$EXTRACT/ota_v0"
[ -d "$OTA" ] || { echo "extracted package has no ota_v0 directory" >&2; exit 1; }

join_chunks() { # prefix outfile
  local prefix="$1" outfile="$2"
  # LC_ALL=C so the sort matches the Windows script's ordinal Sort-Object.
  local chunks
  chunks="$(cd "$OTA" && ls -1 | grep -E "^${prefix}\.[0-9]{4}\." | LC_ALL=C sort)"
  [ -n "$chunks" ] || { echo "no $prefix chunks found in $OTA" >&2; exit 1; }
  : >"$outfile"
  while IFS= read -r c; do cat "$OTA/$c" >>"$outfile"; done <<<"$chunks"
  echo "  $outfile ($(wc -c <"$outfile" | tr -d ' ') bytes, $(wc -l <<<"$chunks" | tr -d ' ') chunks)"
}

echo "Extracted images:"
join_chunks 'rootfs\.squashfs' "$OUT/rootfs.squashfs"
join_chunks 'xImage' "$OUT/xImage"

# The manifest records the MD5 of each whole image; verifying it here catches a
# truncated download or a bad chunk before it becomes a bad firmware build.
for pair in "rootfs.squashfs:rootfs" "xImage:xImage"; do
  img="${pair%%:*}"; prefix="${pair##*:}"
  manifest="$(cd "$OTA" && ls -1 2>/dev/null | grep -E "^ota_md5_${prefix}\." | head -1 || true)"
  [ -n "$manifest" ] || continue
  want="${manifest##*.}"
  got="$(md5 -q "$OUT/$img" 2>/dev/null || md5sum "$OUT/$img" | cut -d' ' -f1)"
  if [ "$want" = "$got" ]; then
    echo "  md5 ok: $img ($got)"
  else
    echo "  MD5 MISMATCH for $img: manifest=$want actual=$got" >&2
    exit 1
  fi
done
