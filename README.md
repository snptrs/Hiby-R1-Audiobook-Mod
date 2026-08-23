# HiBy R1 Audiobook Firmware

A self-contained audiobook app for the normal HiBy R1, based on stock HiBy R1
firmware 1.6. **Not for the R1 MIDI.**

> **v2.0.28 is the current release.** It prevents files under `/Audiobooks`
> from remaining in HiBy's stock Music catalog after Update Database. It also
> includes the v2.0.27 USB-storage fix and all earlier NativeApp stability,
> display-wake, folder, chapter, Now Playing, and resume improvements.

## Current release

- **Version marker:** `2.0.28`
- **About-screen label:** `HiBy R1 2.0.28`
- **Download:** <https://github.com/yetisoldier/Hiby-R1-Audiobook-Mod/releases/tag/v2.0.28>
- **Package:** `r1-audiobooks-2.0.28.upt` (rename to `r1.upt` to install)
- **UPT MD5:** `da13cf3a78823ce9e982bdc1a51f9cd3`
- **UPT SHA256:** `fcce40b32fd1eddef5cd31412cae5565f434c13c2293dd3692648b8b39173431`
- **Boot ADB:** disabled. The public package omits `/etc/init.d/S90adb`, so
  connecting the R1 in Device mode exposes the SD card as normal USB storage.
  ADB can still be enabled manually for development, but ADB and USB storage
  share one controller and cannot be active simultaneously.
- **Restored stock unlocks:** USB DAC mode, Native DSD (analog), and Bluetooth
  SBC XQ - the three general device/music unlocks the v1.5.0-v1.6.3 line carried,
  re-enabled on the 2.0.x build via `-UnlockNativeDsd -EnableBluetoothSbcXq
 -UnlockUsbDacMode`. No audiobook code changed.
- **Base firmware:** stock HiBy R1 1.6 (normal R1).

**v2.0.20 compatibility note:** v2.0.20 was published from an experimental
UTF-8/Cyrillic side branch. That text-rendering experiment is not included in
v2.0.28 because this release follows the separately tested stability line. If
you rely on Cyrillic audiobook names or tags, remain on v2.0.20 for now.

Before flashing, keep a known-good stock 1.6 `r1.upt` for recovery. This is
unofficial firmware tested on one personal normal HiBy R1. Reinstalling stock
firmware should reverse it, but use it at your own risk. Do not install it on
the R1 MIDI or other HiBy players unless you are prepared to recover the device.

## Screenshots

<p>
 <img src="docs/screenshots/01-home.png" alt="Audiobooks home menu" width="200">
 <img src="docs/screenshots/02-titles.png" alt="Titles list with cover thumbnails" width="200">
 <img src="docs/screenshots/03-detail.png" alt="Book detail page" width="200">
 <img src="docs/screenshots/04-now-playing.png" alt="Now Playing with cover and progress handle" width="200">
</p>
<p>
 <img src="docs/screenshots/05-chapters.png" alt="Chapter list" width="200">
 <img src="docs/screenshots/06-bookmarks.png" alt="Bookmarks list" width="200">
 <img src="docs/screenshots/07b-scanning.png" alt="Refresh: scanning library" width="200">
 <img src="docs/screenshots/08-launcher-after-exit.png" alt="HiBy launcher after exiting the app" width="200">
</p>

Captured directly from the test R1. The v2.0.28 catalog maintenance update does
not change these screens. Full set in [`docs/screenshots/`](docs/screenshots/).

## How it works

The audiobook app runs **in-process** inside `hiby_player`:

- A small `LD_PRELOAD` library (`audiobook_app/hook.c`) installs a trampoline on
  the launcher's Audiobooks tile callback. Tapping the tile runs our app instead
  of the stock cave code.
- An `ioctl` hook intercepts `FBIOPAN_DISPLAY` and draws our UI to the target
  framebuffer buffer _before_ each pan. This keeps HiBy's display loop (and the
  touch controller) alive while showing our UI.
- The event loop reads touch and the hardware keys, and the player engine
  decodes MP3 / M4B and writes PCM to ALSA (wired) or BlueALSA (Bluetooth A2DP),
  falling back to wired when no BT sink is connected.
- Exiting restores the launcher frame immediately. A short, bounded handoff
  watcher surfaces HiBy's first hidden double-buffer redraws, avoiding the old
  5-10 second black/frozen-looking return and power-button workaround.
- Opening Audiobooks starts a short-lived background cleanup that removes root
  `/Audiobooks` paths from HiBy's separate Music database copies. It has bounded
  lock retries and exits when finished; there is no resident database daemon.

The app and its UI, player, library scanner, and chapter/bookmark storage live
in `audiobook_app/` and a tiny native helper. The stock Music player, file
browser, Bluetooth, USB, and system UI are otherwise untouched.

## Features

**Podcasts**

- A sibling `/Podcasts` folder on the SD card, added the same way as
  audiobooks: copy files across, then Refresh Library. Nothing is downloaded
  on the device.
- **Each audio file is its own entry**, not a chapter of one big book. That
  means every episode keeps its own resume position, and finishing one marks
  only that episode played.
- Home -> **Podcasts** lists your shows; tapping a show lists its episodes.
  Episodes stay out of Titles, Authors, Folders and Finished, so the audiobook
  views are unchanged.
- One folder per show is the expected layout (`/Podcasts/Some Show/*.mp3`).
  Nested folders work too, and a show is named by its path under `/Podcasts`,
  so two shows can both have a `2024` subfolder without merging. Files dropped
  loose in `/Podcasts` with no show folder are grouped under **Unsorted**.
- **Episode order is reverse filename order.** That gives newest-first for the
  usual naming schemes (`2026-08-01 Title.mp3`, `Episode 12.mp3`), so prefer a
  sortable date or number in the filename. Files named only after the episode
  title will list in reverse alphabetical order, which is not meaningful. Tag
  dates are not read.
- An episode that runs to the end is marked **Played** and playback stops; it
  does not roll into the next episode. Tapping a played episode restarts it
  from the beginning rather than replaying the last few seconds.
- Podcast files are hidden from HiBy's stock Music catalog exactly as
  audiobooks are.
- Episodes appear in **Continue** alongside books, so a part-heard episode is
  one tap from the Home screen.

### Syncing listening state with Audiobookshelf

`tools/abs_sync_listened.py` reconciles listening state between the SD card and
Audiobookshelf. It needs no firmware involvement: the device already writes
everything to the card (`.audiobook_pos/<book_id>.pos` for position and played
state, `Audiobooks/.audiobook_library/library.db` to map ids to paths).

`tools/chronosync_presync_abs.sh` wraps it as a ChronoSync **pre-sync** hook.
Pre-sync, so that played items are marked finished in ABS first, ABS then drops
them from disk per its retention settings, and the sync propagates those
deletions to the card. Media deletions flow Mac to card through the normal
sync; this script only ever writes `.pos` files, never media.

`--direction` selects what it does:

|        |                                                    |
| ------ | -------------------------------------------------- |
| `push` | device to ABS. Read-only with respect to the card. |
| `pull` | ABS to device. **Writes `.pos` files.**            |
| `both` | two-way. What the wrapper uses.                    |

Podcasts are always included. Audiobooks are included when
`--book-library-id` is given; the wrapper sets it.

Behaviours worth knowing:

- **The device keeps listening state in two places, and both are written.**
  `.pos` files are authoritative for resume, but the list views read the
  `progress` table in `library.db`, so a pull writes both. Writing only `.pos`
  resumes correctly and shows no Played badge. A repair pass also brings any
  `progress` row that disagrees with its `.pos` back into line, which covers the
  device's own once-a-minute mirror lag.
- **The more recent save wins**, comparing the device's `.pos` timestamp against
  the ABS `lastUpdate`. A pull writes ABS's timestamp into the `.pos` rather
  than the current time, so the two sides cannot ping-pong.
- **Neither side is ever rewound.** If one is behind the other, the update is
  skipped and logged. A brief tap on the device is "newer" than a real position
  set in the web player, and pushing it would destroy listening state.
  `--allow-rewind` overrides.
- **Near-end counts as finished, using the ABS library's own setting.** The
  firmware only sets `completed` when the decoder runs off the true end of a
  file, so stopping during a podcast outro would leave an item unfinished
  forever. Each library's own `markAsFinishedTimeRemaining` is used for its own
  items, so ABS stays the one place it is configured and podcasts and
  audiobooks can differ. ABS does not apply that setting to progress pushed
  over its API (verified: at 30s remaining a write stays unfinished even with
  the setting at 10s, because the rule runs on ABS's own playback sessions), so
  the script reads the number and applies it. It caps the allowance at 10% of
  duration so short episodes
  are not called finished too early.
- **A few seconds of playback is not a position.** An unfinished position below
  30s is discounted on both sides, so a stray tap neither becomes a Continue
  Listening entry nor blocks the other side from winning: the real position
  overwrites it. Tune with `--min-position-secs`; finishing is always synced.
- **Pulling refuses to run if a saved position is dated in the future.**
  Ordering depends on the `.pos` timestamps, and a device clock running ahead
  would let stale card state beat a newer ABS change. Only that direction is
  detectable: a timestamp in the past is simply when you last listened, so it
  says nothing about the clock. A clock running behind can therefore slip
  through and bias ordering toward ABS, which is what the no-rewind guard is
  there to limit.
- **A multi-file audiobook is skipped unless the device and ABS agree on its
  total duration.** The device sums its own track durations in its own order
  and ABS concatenates in its; if the totals disagree the timelines differ and
  a mapped position would land somewhere else entirely. Single-file items have
  nothing to disagree about.

Always try `--dry-run` first, or `HIBY_ABS_DRY_RUN=1` for the wrapper.
`tools/test_abs_sync_reconcile.py` unit-tests the decision logic with no card,
network or ABS instance.

**Setting it up on another Mac.** The sync script is standard library only, so
system `python3` is enough: no Homebrew, no pip, and none of the firmware build
toolchain. The wrapper locates the script relative to itself, so the repo can
live anywhere and needs no editing. Clone the repo, write the ABS API token to
`~/.config/abs/token` (`chmod 600`), then verify with the card mounted:

```bash
python3 tools/abs_sync_listened.py \
  --abs-url http://YOUR-ABS-HOST:13378 \
  --library-id YOUR-PODCAST-LIBRARY-ID \
  --book-library-id YOUR-AUDIOBOOK-LIBRARY-ID --check
```

`--check` validates the token, ABS reachability and auth, that each library is
the expected media type, that the card is mounted with a `books.kind` column,
and that the device clock is sane. It changes nothing and exits non-zero on
failure. Then point ChronoSync at `tools/chronosync_presync_abs.sh`.

Override the defaults baked into the wrapper in `~/.config/abs/config`:

```
HIBY_ABS_URL=http://other-host:13378/audiobookshelf
HIBY_ABS_LIBRARY_ID=...
HIBY_ABS_BOOK_LIBRARY_ID=...
HIBY_ABS_DIRECTION=push
```

### macOS / Linux

The `.sh` scripts build the current release configuration
(`-IncludeAudiobookNativeApp -UnlockNativeDsd -EnableBluetoothSbcXq
-UnlockUsbDacMode`) and nothing else. For any other switch combination use the
PowerShell scripts below on Windows.

```bash
brew install zig squashfs sevenzip          # zig must be 0.16.0
python3 -m pip install --target .deps/python pycdlib pyelftools

# Stock 1.6 r1.upt -> work/original/{rootfs.squashfs,xImage}
tools/extract_r1_firmware.sh firmware/r1.upt

tools/build_r1_audiobook_firmware.sh 2.1.0 "HiBy R1 2.1.0"

# Always verify before flashing. It prints the exact command on completion.
PYTHONPATH=.deps/python python3 tools/verify_r1_audiobook_build.py \
  --out-dir work/audiobook-firmware-2.1.0 \
  --upt-name r1-audiobooks-2.1.0.upt \
  --expected-version 2.1.0 --expected-label "HiBy R1 2.1.0" \
  --expect-native-app --expect-native-dsd --expect-sbc-xq \
  --expect-usb-dac-mode --unsquashfs "$(command -v unsquashfs)"
```

Host-side tests for the library and scanner layers:
`tools/test_host_library_scan.sh`. It also typechecks `ui.c`, `player.c`,
`hook.c` and `render.c` against `tools/hostshim/`, which is the only way to get
warnings out of those files (they need Linux kernel headers, and the device
build passes no `-Wall`).

Two host quirks the scripts handle, both of which the verifier catches if they
regress: macOS stores a symlink's own mode and derives it from the umask, so
extraction runs under `umask 000` or all 482 stock symlinks come out `0755`
instead of `0777`; and macOS stamps `com.apple.provenance` xattrs on files,
so extraction and packing both pass `-no-xattrs`.

### Windows

Optionally build the hook/app shared library by itself:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build_r1_audiobook_hook.ps1
```

The production firmware command below recompiles the hook from current source
before packaging it, so this separate command is useful for quick development
checks but is not required for a release build.

Build the public-release firmware (version 2.0.28):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build_r1_audiobook_firmware.ps1 `
 -OutDir work\audiobook-firmware-2.0.28 `
 -OutputUpt work\audiobook-firmware-2.0.28\r1-audiobooks-2.0.28.upt `
 -IncludeAudiobookNativeApp `
 -UnlockNativeDsd `
 -EnableBluetoothSbcXq `
 -UnlockUsbDacMode `
 -CustomVersionId 2.0.28 `
 -CustomVersionLabel "HiBy R1 2.0.28"
```

The public build intentionally omits `-EnableBootAdb`. For a development-only
image, that flag installs `/etc/init.d/S90adb`; persistent ADB still remains off
until `/usr/data/enable_boot_adb` is explicitly created with
`tools\adb_manage_boot_adb.ps1 -Action enable`. `-UnlockNativeDsd`,
`-EnableBluetoothSbcXq`, and `-UnlockUsbDacMode` restore the three general
device/music unlocks the pre-2.0 line carried (Native DSD on the analog path,
BlueALSA SBC XQ, and the USB DAC working mode). They are independent of the
NativeApp pivot and combine cleanly with it.

The NativeApp build is mutually exclusive with the legacy resume-daemon
switches (`-IncludeAudiobookLauncherGenre`, `-IncludeAudiobookResumeRuntime`,
`-IncludeAudiobookDbMaintenance`, etc.) - use `-IncludeAudiobookNativeApp`
alone.

## Architecture and docs

- `docs/modding/` - **modder knowledge base**: the reverse-engineering
  reference for the hook architecture, flash/recovery flow, audio decode/ALSA,
  Bluetooth A2DP/AVRCP, input keys, cover art, WSOLA/seek, library scan/storage,
  SD runtime-power stability, ADB automation, and the brick-lessons/build-risk
  guide. Start at
  `docs/modding/README.md`.
- `docs/audiobook_firmware_architecture.md` - high-level firmware architecture
  (NativeApp pivot).
- `docs/build_flash_verify_runbook.md` - build, flash, and verify runbook.
- `docs/adb_control_tools.md` - live ADB control (screenshots, taps, presets).
- `docs/github_release_process.md` - GitHub Release publishing runbook.
- `docs/production_release_checklist.md` - release and verification checklist.
- `docs/modder_start_here.md` - orientation for modders (points into
  `docs/modding/`).
- `docs/audiobook_app_feature_reference.md` - historical feature map (carries a
  superseded banner; top section reflects the NativeApp).
- `docs/screenshots/` - README screenshots.
- `firmware/releases/v2.0.16/` - v2.0.16 release notes and checksums.
- `firmware/releases/v2.0.17/` - v2.0.17 release notes and checksums.
- `firmware/releases/v2.0.28/` - current release notes and checksums.
- `CHANGELOG.md` - release history.

## Attribution and sources

This project is unofficial and not affiliated with or endorsed by HiBy. HiBy,
HiBy R1, and the stock firmware remain HiBy's work.

Information and techniques used while building this mod came from:

- [HiBy R1 User Manual](https://guide.hiby.com/en/docs/products/audio_player/hiby_r1/guide)
- [HiBy R1 firmware 1.6 update page](https://store.hiby.com/apps/help-center#hc-r1-firmware-v16-update)
- [Rockbox HiBy Port wiki](https://www.rockbox.org/wiki/HibyPort)
- [bidhata/Hiby-R1-Mod](https://github.com/bidhata/Hiby-R1-Mod)
- [SuperTaiyaki/hiby-firmware-tools](https://github.com/SuperTaiyaki/hiby-firmware-tools)
- [hiby-modding/hiby-mods](https://github.com/hiby-modding/hiby-mods)
- [hiby-modding/hiby_os_crack](https://github.com/hiby-modding/hiby_os_crack)
- [baijz/ingenic-toolchain](https://gitee.com/baijz/ingenic-toolchain)
- [nanowave-player/nanowave](https://github.com/nanowave-player/nanowave)
- [seanap/Plex-Audiobook-Guide](https://github.com/seanap/Plex-Audiobook-Guide)

The audiobook-specific behavior was developed and tested on a personal normal
HiBy R1 through local reverse engineering, live ADB testing, and repeated
stock-firmware recovery tests.
