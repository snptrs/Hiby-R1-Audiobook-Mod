#!/usr/bin/env bash
# macOS/Linux port of build_r1_audiobook_firmware.ps1, RELEASE PATH ONLY.
#
# The .ps1 carries ~30 switches, most of them pre-2.0 experiments (resume
# daemon, memscan, DB maintenance, the native-hub variants). This script
# implements exactly the flag set the current release ships with:
#
#   -IncludeAudiobookNativeApp -UnlockNativeDsd -EnableBluetoothSbcXq
#   -UnlockUsbDacMode -CustomVersionId <id> -CustomVersionLabel <label>
#
# and nothing else. If you need any of the other switches, use the .ps1 on
# Windows rather than extending this. Keeping the surface small is the point:
# every branch here is one that the shipped firmware actually takes.
#
# All the rootfs patching is done by the same Python tools the .ps1 calls, so
# the parts that understand the binary formats are shared, not reimplemented.
#
# Usage:
#   tools/build_r1_audiobook_firmware.sh <version-id> <version-label> [outdir]
# Example:
#   tools/build_r1_audiobook_firmware.sh 2.1.0 "HiBy R1 2.1.0"
#
# Inputs expected (see tools/extract_r1_firmware.sh):
#   work/original/rootfs.squashfs
#   work/original/xImage

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

VERSION_ID="${1:?usage: build_r1_audiobook_firmware.sh <version-id> <version-label> [outdir]}"
VERSION_LABEL="${2:?missing version label}"
OUT_DIR="${3:-$REPO/work/audiobook-firmware-$VERSION_ID}"

ROOTFS="${ROOTFS:-$REPO/work/original/rootfs.squashfs}"
XIMAGE="${XIMAGE:-$REPO/work/original/xImage}"
OTA_VERSION="${OTA_VERSION:-0}"
UPT_NAME="${UPT_NAME:-r1-audiobooks-$VERSION_ID.upt}"

PY="${PY:-python3}"
export PYTHONPATH="$REPO/.deps/python${PYTHONPATH:+:$PYTHONPATH}"

need() { command -v "$1" >/dev/null || { echo "$1 not found ($2)" >&2; exit 1; }; }
need mksquashfs "brew install squashfs"
need unsquashfs "brew install squashfs"
need "$PY" "install python3"
for f in "$ROOTFS" "$XIMAGE"; do
  [ -f "$f" ] || { echo "missing input: $f (run tools/extract_r1_firmware.sh)" >&2; exit 1; }
done
"$PY" -c 'import pycdlib' 2>/dev/null || {
  echo "pycdlib missing: $PY -m pip install --target .deps/python pycdlib" >&2; exit 1; }

ROOT_TREE="$OUT_DIR/squashfs-root"
NEW_ROOTFS="$OUT_DIR/rootfs.squashfs"
OTA_TREE="$OUT_DIR/ota-tree"
PSEUDO="$OUT_DIR/rootfs-pseudo.txt"
PATCHED_PLAYER="$OUT_DIR/hiby_player.audiobooks"
OUTPUT_UPT="$OUT_DIR/$UPT_NAME"

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

echo "== 1/9 unpack stock rootfs =="
# Extracting as a normal user cannot restore ownership or every mode bit, and
# that is fine: mksquashfs is invoked with -all-root, and all modes come from
# the pseudo-file generated from the STOCK image further down. Silence the
# resulting noise but keep real errors.
#
# -no-xattrs matters on macOS. The filesystem stamps com.apple.provenance (and
# sometimes com.apple.quarantine) onto files, and without this mksquashfs bakes
# them into the image. The Windows build cannot carry Linux xattrs either, so
# dropping them here makes the output MORE like the tested package, not less.
#
# umask 000 for the extraction only. Linux gives every symlink mode 0777 and
# ignores the bits; BSD/macOS actually stores a symlink's own mode and derives
# it from the umask, so the default 022 would recreate all 482 stock symlinks as
# 0755 and mksquashfs would bake that in. Harmless on the device (Linux ignores
# symlink modes) but it diverges from the stock image and the verifier rightly
# rejects it. Scoped to this call so nothing else changes.
( umask 000; unsquashfs -no-xattrs -d "$ROOT_TREE" "$ROOTFS" >/dev/null )

echo "== 2/9 patch hiby_player (native-app launcher trampoline) =="
"$PY" tools/patch_hiby_player.py "$ROOT_TREE/usr/bin/hiby_player" \
  -o "$PATCHED_PLAYER" --audiobook-native-app-launcher
cp -f "$PATCHED_PLAYER" "$ROOT_TREE/usr/bin/hiby_player"

echo "== 3/9 patch resource text (About screen strings) =="
"$PY" tools/patch_r1_resource_text.py "$ROOT_TREE" \
  --about-model "$VERSION_LABEL" --product-version "$VERSION_ID"

echo "== 4/9 audio feature unlocks =="
"$PY" tools/patch_r1_audio_feature_unlocks.py "$ROOT_TREE" \
  --native-dsd --sbc-xq --usb-dac

echo "== 5/9 version marker =="
# ota_site is inherited from the stock etc/ota_info. OTA_VERSION stays 0 for a
# normal build, which is why etc/ota_info itself is left untouched.
OTA_SITE="/data/autoupdate/autoupdate"
if [ -f "$ROOT_TREE/etc/ota_info" ]; then
  site="$(sed -n 's/^ota_site=\(.*\)$/\1/p' "$ROOT_TREE/etc/ota_info" | head -1 | tr -d '\r')"
  [ -n "$site" ] && OTA_SITE="$site"
fi
# Field order and values match the .ps1 exactly; verify_r1_audiobook_build.py
# parses this file and the on-device About screen reads from it.
cat >"$ROOT_TREE/etc/r1_audiobook_version" <<EOF
version=$VERSION_ID
label=$VERSION_LABEL
base_firmware=1.6
ota_version=$OTA_VERSION
ota_site=$OTA_SITE
audiobook_entry=stock-book
boot_adb=disabled
usb_gadget_scripts=hardened
batd_logger=enabled
launcher_icon=stock-book
native_dsd=enabled
bluetooth_sbc_xq=enabled
usb_dac_mode=enabled
bidhata_tweaks=stock
bidhata_brightness=stock
bidhata_file_limit=stock
native_hub_launcher=disabled
native_hub_folder_rows=disabled
native_hub_view_rows=disabled
resume_runtime_profile=direct
EOF

echo "== 6/9 native app + hook library =="
# Shell scripts go in as LF/ASCII: the device's /bin/sh will not run a script
# with CRLF line endings.
install_lf() { # src dest
  tr -d '\r' <"$1" >"$2"
}
install_lf audiobook_app/r1_player_supervisor.sh "$ROOT_TREE/usr/bin/hiby_player.sh"

NATIVE_APP="${NATIVE_APP:-$REPO/work/native-app/r1_audiobook_smoke}"
if [ ! -f "$NATIVE_APP" ]; then
  bash tools/build_r1_audiobook_app.sh "$NATIVE_APP"
fi
cp -f "$NATIVE_APP" "$ROOT_TREE/usr/bin/r1_audiobook_app"

# Always rebuilt from source. Packaging a stale hook produces a valid update
# file that silently omits the latest code, which is very hard to notice.
HOOK="$REPO/work/native-app/libaudiobook_hook.so"
bash tools/build_r1_audiobook_hook.sh "$HOOK"
mkdir -p "$ROOT_TREE/usr/lib"
cp -f "$HOOK" "$ROOT_TREE/usr/lib/libaudiobook_hook.so"

echo "== 7/9 hardened USB gadget scripts =="
install_lf firmware/scripts/r1_usb_gadget_common.sh "$ROOT_TREE/usr/bin/r1_usb_gadget_common.sh"
install_lf firmware/scripts/adbon  "$ROOT_TREE/usr/bin/adbon"
install_lf firmware/scripts/adboff "$ROOT_TREE/usr/bin/adboff"
# Release builds deliberately omit etc/init.d/S90adb, so boot ADB stays off.

echo "== 8/9 squashfs pseudo modes =="
# Modes and ownership for every stock path are read back out of the STOCK
# image, so a host that cannot preserve them during extraction does not matter.
"$PY" tools/write_squashfs_pseudo_modes.py \
  --rootfs "$ROOTFS" --unsquashfs "$(command -v unsquashfs)" --output "$PSEUDO"

# Modes for the files this build adds or replaces, appended only when present.
add_mode() { # relpath mode
  [ -e "$ROOT_TREE/$1" ] && printf '%s m %s 0 0\n' "$1" "$2" >>"$PSEUDO"
  return 0
}
add_mode usr/bin/r1_usb_gadget_common.sh 0755
add_mode usr/bin/adbon                   0755
add_mode usr/bin/adboff                  0755
add_mode usr/bin/hiby_player.sh          0755
add_mode usr/bin/r1_audiobook_app        0755
add_mode usr/lib/libaudiobook_hook.so    0644
add_mode etc/r1_audiobook_version        0644

echo "== 9/9 mksquashfs + .upt =="
# Belt and braces on the xattr front: strip anything macOS attached to the tree
# while we were writing to it, then tell mksquashfs to ignore xattrs anyway.
if command -v xattr >/dev/null; then
  xattr -cr "$ROOT_TREE" 2>/dev/null || true
fi
mksquashfs "$ROOT_TREE" "$NEW_ROOTFS" \
  -comp lzo -b 131072 -no-progress -all-root -no-xattrs -pf "$PSEUDO" >/dev/null

"$PY" tools/build_r1_upt.py --ximage "$XIMAGE" --rootfs "$NEW_ROOTFS" \
  --output "$OUTPUT_UPT" --keep-tree "$OTA_TREE" --ota-version "$OTA_VERSION"

echo
echo "rootfs : $NEW_ROOTFS ($(wc -c <"$NEW_ROOTFS" | tr -d ' ') bytes)"
echo "package: $OUTPUT_UPT ($(wc -c <"$OUTPUT_UPT" | tr -d ' ') bytes)"
md5="$(md5 -q "$OUTPUT_UPT" 2>/dev/null || md5sum "$OUTPUT_UPT" | cut -d' ' -f1)"
sha="$(shasum -a 256 "$OUTPUT_UPT" | cut -d' ' -f1)"
echo "MD5    : $md5"
echo "SHA256 : $sha"
echo
echo "Next: verify before flashing."
echo "  PYTHONPATH=.deps/python $PY tools/verify_r1_audiobook_build.py \\"
echo "    --out-dir $OUT_DIR --upt-name $UPT_NAME \\"
echo "    --expected-version $VERSION_ID --expected-label \"$VERSION_LABEL\" \\"
echo "    --expect-native-app --expect-native-dsd --expect-sbc-xq \\"
echo "    --expect-usb-dac-mode --unsquashfs \"$(command -v unsquashfs)\""
