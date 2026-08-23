# Podcast support for the HiBy R1 audiobook app

## Status

**Code complete; host-verified; not yet built or flashed.**

Done and passing `tools/test_host_library_scan.sh`: the `books.kind` column and
migration, the shared column macro and kind filters, the two new shows queries,
multi-root scanning with per-file episode rows, scoped orphan cleanup plus the
zero-track pass, the scanner fixes, the two `player.c` completion fixes, the
full UI layer, and stock Music catalog isolation for `/Podcasts`.

**Built and verified on macOS.** The build was ported (`tools/*.sh`), and
`r1-audiobooks-2.1.0.upt` passes all 54 checks of
`tools/verify_r1_audiobook_build.py`.

Remaining:

1. Flash and work through the on-device list under Verification below. The UI
   and player changes are **typecheck-only** so far: `ui.c` and `player.c`
   include Linux kernel headers and cannot be linked on a host, so nothing in
   those two files has actually executed yet. The first flash is their first
   test.
2. Fill in `docs/production_release_checklist.md` and promote the CHANGELOG
   heading from "Unreleased" once the on-device pass is done.

### Port validation

Because a bad package can brick the device, the port was checked three ways
beyond its own output verifying:

- A clean worktree at `HEAD` (v2.0.28 source, no podcast changes) rebuilt
  through the ported scripts also passes all 54 verifier checks.
- Diffing that baseline's packaged rootfs against the 2.1.0 one yields exactly
  four changed entries and nothing else: `libaudiobook_hook.so` (+8,044 bytes
  of podcast code) and three version strings (`etc/r1_audiobook_version`,
  `usr/resource/config.json`, `about_dev.ini`). 5,492 entries both sides, all
  modes identical, `hiby_player` byte-identical.
- `build_r1_upt.py` output round-trips through `extract_r1_firmware.sh` with
  matching manifest MD5s and byte-identical images.

The package is **not** bit-identical to the published v2.0.28, and cannot be:
squashfs and the ISO both embed creation timestamps, and the Windows build used
a different mksquashfs version.

## Context

The app currently defines a book as "a folder containing audio files", and every
file in that folder becomes a track of that one book (`find_book_dirs`,
`audiobook_app/scan.c:136`). Dropping podcast episodes into a folder therefore
produces a single library entry with the episodes as chapters, which is not how
anyone listens to podcasts.

The structural reason episodes cannot simply become tracks is
`progress.book_id INTEGER PRIMARY KEY` (`audiobook_app/library.c:83`): one
resume point per book row. Per-episode resume needs one row per episode.

Goal: a sibling `/Podcasts` root on the SD card whose audio files each become
their own library row, surfaced through a dedicated Podcasts section (shows →
episodes) while the audiobook side keeps behaving exactly as it does today.
Episodes are added by copying files to the card, the same as audiobooks. No
on-device feed downloading.

## Decisions already taken

- **Sibling `/Podcasts`, not nested under `/Audiobooks`.** Nesting would make
  the audiobook scanner permanently responsible for excluding one magic
  subfolder name, silently swallowing anyone's real audiobook folder called
  "Podcasts". Cost of the sibling is extending one `LIKE` clause in
  `music_catalog.c`.
- **Episode order:** reverse of the existing natural filename sort. See
  "Ordering" below for what this does and does not guarantee.
- **On finish:** mark played, stop. No auto-advance to the next episode.
- **Navigation:** the Home "Series" row is replaced by "Podcasts", keeping Home
  at 8 rows. Home does not scroll and a 9th row would collide with the refresh
  flash text at y=720 and the footer at y=766, so replacing rather than adding
  is what makes this fit.

## Decisions that need a veto if you disagree

- **Podcast episodes will appear in Continue Listening** alongside books. This
  is a behaviour change to an existing screen that you did not ask for. It is
  included because a half-listened episode belongs in "resume what I was
  doing". Removing it is a one-line `kind=0` filter on `SQL_LIST_CONTINUE`.
- **The Finished list and the "Done" badge will start working for audiobooks
  too.** They are dead today (see below). Nothing appears retroactively; they
  populate for books finished after this ships. This is a latent-bug fix that
  the podcast "Played" badge depends on, not optional scope.

## Two pre-existing bugs this feature depends on

### 1. Nothing ever reads a completed flag that is set

`books.completed` is never set to 1 anywhere in the codebase. Only
`progress.completed` is written, by `save_progress` in `player.c`. But the
"Done" badge (`ui.c:2149`) and the Finished list
(`SQL_LIST_FINISHED ... WHERE b.completed=1`, `library.c:210`) both read
`books.completed`, so neither can ever light up.

Fix it on the read side, not the write side:

- `list_collect_cb` (`ui.c:1559-1561`) already calls `audiobook_get_progress`
  per row. Change `item->completed = b->completed` to OR in `prog.completed`.
- The Folders query (`ui.c:1656-1661`) reads `b.completed` at column 4 and also
  already calls `audiobook_get_progress` right after. Same OR.
- `SQL_LIST_FINISHED` joins `progress` and filters `p.completed=1 ORDER BY
p.completed_at DESC` instead of reading `b.completed`.

Explicitly **not** writing `books.completed` from the player. `books.completed`
stays dead, which is what it already is.

### 2. Exiting the app erases the completed flag

Verified at `player.c:2107`: `CMD_QUIT` calls `save_progress(0, 1)`, guarded
only by `!db || book_id <= 0 || track_count == 0` (`player.c:840`). After a
book finishes, `book_id` and `track_count` are both still set, so exiting the
app writes `completed = 0` over the finish. `cmd_pause` and `cmd_stop` cannot
do this (they bail on `state != PLAYING` and on `(track_open || pcm)`
respectively, all false after a finish). The quit path is the only one, and it
also rewrites the SD `.pos` sidecar the same way.

This is why nothing appears retroactively: no user has a surviving
`progress.completed = 1`. It also means `SQL_LIST_CONTINUE`
(`WHERE p.completed=0 AND p.last_played_at>0`) re-lists every finished item with
a fresh `last_played_at`. For books that is a minor annoyance. For podcasts,
where Continue is the main list and finishing is the steady state, it would keep
Continue permanently full of finished episodes at ~100%.

**Fix: make the quit save preserve the current completed state** instead of
hardcoding 0. Pass `g_pl.db_saved_completed` through, or skip the quit save
entirely when `state == PLAYER_STOPPED && !track_open`. Without this the podcast
"Played" badge works in-session and silently reverts on exit.

### Also in `player.c`: replaying a finished item

`cmd_play` loads `sd_done` from `pos_load_sd` (`player.c:1645`) and never uses
it, then rewinds 5 s from the saved position. Tapping a finished episode plays
the last 5 seconds and finishes again. Tolerable for books; for podcasts,
re-tapping a played episode is a normal gesture and this reads as broken. The
flag is already loaded, so this is a two-line "if completed, start from 0".

So `player.c` **is** touched, contrary to the first draft of this plan: two
small, well-scoped changes, both on the completion path, neither in the decode
or seek hot paths.

## Data model

New `books.kind` column: `0` = audiobook, `1` = podcast episode.

`SCHEMA_VERSION` is written to `schema_meta` and never read back
(`library.c:530`), so nothing wipes on a bump, but `CREATE TABLE IF NOT EXISTS`
will not add a column to an existing DB and there is no `ALTER TABLE` anywhere
in the codebase yet.

**The migration must be its own `sqlite3_exec` with its return code deliberately
ignored, placed after the `SCHEMA_SQL` exec.** It must not go inside
`SCHEMA_SQL`: `audiobook_db_open` runs that whole string as one exec and treats
_any_ error as fatal: it closes the handle and returns -1, which every caller
reads as "no library" (`library.c:520-526`). `ALTER TABLE books ADD COLUMN kind`
succeeds once and then reports `duplicate column name: kind` forever after, so
putting it there would make the app stop opening its library on the second boot.
Add `kind` to the `CREATE TABLE` at `library.c:31-52` as well so fresh DBs match,
and bump `SCHEMA_VERSION` to `"3"` for honesty.

A podcast episode row is:

| Column          | Value                                                                                                                                           |
| --------------- | ----------------------------------------------------------------------------------------------------------------------------------------------- |
| `kind`          | `1`                                                                                                                                             |
| `book_key`      | file path via `audiobook_derive_book_key`, **plus an FNV-1a hash suffix** (see below)                                                           |
| `root_path`     | the **show folder**, not the file. Load-bearing: `audiobook_cleanup_orphans` deletes any book whose `root_path` fails `S_ISDIR` (`scan.c:1024`) |
| `title`         | tag title, else filename with the extension stripped (**not** `clean_book_title`, see below)                                                    |
| `series_id`     | the show's **root-relative path**, via the existing `get_or_create_series`                                                                      |
| `series_number` | episode index from the natural filename sort                                                                                                    |
| `author_id`     | tag artist, else the show name                                                                                                                  |
| `track_count`   | 1                                                                                                                                               |

Three traps here, all verified in the code:

- **`book_key` is lossy, so per-file keys collide.**
  `audiobook_derive_book_key` (`library.c:1026-1048`) maps space, `-`, `.`, `_`,
  brackets and every other non-alphanumeric to a single `_`, so `Ep 1.mp3`,
  `Ep-1.mp3` and `Ep.1.mp3` all produce `Ep_1`. It also truncates at the buffer
  length with no hash fallback, so two long episode titles sharing a prefix
  collide outright. Machine-generated podcast filenames make near-identical
  names normal. The failure is silent and destructive: the second `upsert_book`
  UPDATEs the first episode's row via `ON CONFLICT(book_key)`, then
  `upsert_track` hits `UNIQUE(book_id, ordinal)` with both at ordinal 1, returns
  -1, and the loop `continue`s, so one episode disappears and the survivor carries
  the wrong title and duration. Append `cover_cache_name`'s FNV-1a hash
  (`scan.c:628-635`) of the full file path as a discriminator.
- **`series.display_name` is UNIQUE (`library.c:30`), so show basenames merge.**
  `find_book_dirs` recurses without a depth limit, so `/Podcasts/ShowA/2024/`
  and `/Podcasts/ShowB/2024/` would both become a show named `2024`, merged into
  one row with two interleaved episode-index sequences. Same for `Season 1`,
  `Episodes`, `mp3`. Use the podcast-root-relative path, not
  `strrchr(path, '/') + 1`.
- **`clean_book_title` eats date prefixes.** It strips a leading 4-digit year
  plus separator (`scan.c:533-540`), turning `2026-08-01 Interview.mp3` into
  `08-01 Interview.mp3`, exactly the most useful part of a date-prefixed
  episode name. Do not reuse it for episodes. Also strip the file extension
  explicitly; the existing filename fallback (`scan.c:860`) uses the name
  verbatim, so titles would read `... Ep 12.mp3`.

Reusing `series` for shows and `series_number` for the episode index is
deliberate: it makes `audiobook_list_books_by_series`'s existing
`ORDER BY b.series_number` the episode ordering, so the episode list is a
filtered variant of a query that already exists. It is also why dropping Series
from Home is convenient rather than merely acceptable, since otherwise that view
would mix book series and podcast shows.

### Ordering

`collect_audio_files` already `qsort`s by `audiobook_natural_cmp`
(`scan.c:125`), which handles both `2026-08-01-title.mp3` and `Episode 9` vs
`Episode 10`. `series_number` is that index; the episode list orders by it
descending.

This is **reverse filename order**, which equals newest-first only for
date-prefixed (`2026-08-01-…`) or numbered names. It is wrong for day-first
dates (`05-01-2026`), month names (sorts April, August, December), season
numbering that resets, and any show whose files are named after the episode
title. That last case is common and yields plain reverse alphabetical.

So: do not label the sort "Newest first" anywhere in the UI. Let the order be
implicit and document the filename convention in the README. Reading a real
date would need a new field in `audio_tags_t` (ID3 `TDRC`/`TYER`, MP4 `©day`)
and is deliberately out of scope. `date_modified` is not a usable substitute:
it is only written on INSERT and never updated on re-scan, and a bulk copy to
exFAT stamps every file with the same time.

One consequence to accept: the index is positional, so adding an episode that
sorts into the middle renumbers everything after it. Harmless for ordering, but
if an `upsert_book` fails and the loop `continue`s, that episode keeps a stale
number while its neighbours shift.

## Scanner changes (`audiobook_app/scan.c`)

`audiobook_scan_library` gains a `kind` parameter. A new `audiobook_scan_all`
iterates a static root table (`{path, kind, label}`) and **silently skips roots
that do not exist** so a user with no `/Podcasts` folder sees no error. Update
the two callers: `ui.c:172` and `library_test.c`.

### Transactions and orphan cleanup: the data-loss guard

This is the highest-risk part of the change and the first draft of this plan had
it wrong. Do **not** span both roots in one transaction: `g_db_write_lock` is a
plain `PTHREAD_MUTEX_INITIALIZER` (`library.c:421`), so it is non-recursive, and
holding it across two full scans also starves the player thread's progress
saves for the whole duration.

Instead:

- **Keep the per-root `lock` / `BEGIN` / `COMMIT` / `unlock` cycle** exactly as
  it works today, once per root.
- **Move `audiobook_cleanup_orphans` out of the per-root scan** into its own
  final lock/transaction, run **once** after all roots. It has to run after
  every root's tracks are upserted, or the new zero-tracks pass deletes the
  other root's rows mid-scan.
- **Pass the set of roots that actually scanned successfully into cleanup, and
  skip any book whose `root_path` is not under one of them.**

That last point is not a nicety. Today the only thing preventing mass deletion
is an accident of ordering: `audiobook_scan_library` returns -1 when
`stat(root_path)` fails (`scan.c:644`) **before** reaching cleanup at
`scan.c:966`, so an unmounted card does no damage. A root table that "silently
skips missing roots" removes that protection. If `/Podcasts` exists and
`/Audiobooks` does not (folder renamed, deleted, or a card with only
podcasts on it), the podcast scan would reach the global cleanup and every audiobook row
would fail the `S_ISDIR` test at `scan.c:1024`. The cascade takes tracks,
chapters, progress and bookmarks, and then `scan.c:1055-1057` calls
`pos_remove_sd` and `bookmark_remove_book_sd` on each. Those SD sidecars are the
_authoritative_ copies of resume positions and bookmarks, so that is
unrecoverable loss triggered by a folder rename.

Scoping cleanup to successfully-scanned roots kills the whole class of bug: a
missing `/Audiobooks` leaves every audiobook row untouched.

Related guards worth keeping in the same spirit:

- The zero-tracks pass must run **after** the dead-track pass, must do all four
  side effects the existing dead-book loop does (`books`, `book_search`,
  `pos_remove_sd`, `bookmark_remove_book_sd`, `scan.c:1040-1057`), and should
  only fire for books whose `root_path` still stats, so a transient SD hiccup
  cannot destroy sidecars. `book_id` is a rowid and gets reused after deletion,
  so a stale `POS_DIR/<id>.pos` can otherwise attach to an unrelated episode
  later.
- Hoist the `VACUUM` (`scan.c:991`) out of the per-root function so it runs once
  per Refresh rather than once per root. Two full DB rebuilds per refresh doubles
  the stall for no benefit.

Per-root work, in the podcast branch of the per-book loop: after
`collect_audio_files` returns the show folder's files, **emit one book row per
file directly to the DB** rather than growing the `book_dir_t` array. That
keeps `find_book_dirs` untouched and removes any memory question about
`MAX_BOOKS` (which is defined at `scan.c:30` and never used anyway).

Other required scanner fixes:

- **`derive_path_metadata` (`scan.c:452`)** must take the active root as a
  parameter instead of `strncmp`ing against `AUDIOBOOK_LIBRARY_ROOT` at line 461. Today a path outside that root keeps its full absolute path, so
  `comps[0]` becomes `""` and the function derives `author=""` and
  `series="usr"`, creating a junk `series` row that every episode files under.
  Parameterize the root; do not rewrite the component split.
- **`.covercache`** is a string-literal concatenation of
  `AUDIOBOOK_LIBRARY_ROOT` at `scan.c:682` and `scan.c:754-755`. Make it
  root-relative so podcast art lands next to podcasts. Note that the
  free-space guard at `scan.c:654` `statvfs`es `AUDIOBOOK_DB_DIR`, which lives
  under `/Audiobooks`, so a podcast scan still depends on `/Audiobooks`
  existing regardless.
- **Cover art per show:** extract embedded art once from `files[0]` and point
  every episode row in that folder at the same `cover_path`, so the on-SD
  `.r565` cache is shared instead of duplicated per episode.
- **No chapter synthesis for podcasts.** An episode would otherwise take the
  single-file branch (`scan.c:915-933`) and get one useless "Chapter 1". Real
  embedded chapters are still parsed when present.
- **New orphan pass: delete books with zero tracks.** Deleting one episode file
  removes its track but leaves its book row, because the show folder still
  exists and passes the `S_ISDIR` test. Follow the existing dead-books loop so
  `book_search`, `pos_remove_sd` and `bookmark_remove_book_sd` are cleaned too
  (`scan.c:1038-1059`).
- **Fix the `bd->files` leak** on the `continue` paths at `scan.c:726`, `816`
  and `820`, which each skip the `free` at line 945 and leak ~786 KB on a 56 MB
  device. These are the exact lines being restructured.
- The `library_roots` label is hardcoded `"Audiobooks"` at `scan.c:691`; take it
  from the root table.

## Library layer changes (`audiobook_app/library.c` / `.h`)

`audiobook_book_t` gains `int kind`. **Seven** SQL constants repeat the same
20-column book list, all feeding `fill_book_from_stmt`:
`SQL_GET_BOOK_BY_KEY` (`library.c:147`), `SQL_GET_BOOK_BY_ID` (`:159`),
`SQL_LIST_BOOKS` (`:171`), `SQL_LIST_CONTINUE` (`:184`), `SQL_LIST_FINISHED`
(`:199`), `SQL_LIST_BOOKS_BY_AUTHOR` (`:218`), `SQL_LIST_BOOKS_BY_SERIES`
(`:232`), plus the two new shows queries.

**Extract that column list into one shared `SQL_BOOK_COLUMNS` macro** and append
`b.kind` as the last column, so `fill_book_from_stmt`'s positional indices do
not shift and the copies cannot drift. That function reads columns 0-19
(`library.c:331-359`, completed at 17, `playback_speed` at 19), so `kind`
becomes column 20.

The extraction is not cosmetic: this rollout is all-or-nothing and fails
**silently**, not loudly. `sqlite3_column_int(stmt, 20)` on a SELECT that forgot
`kind` returns 0 rather than erroring, so a half-updated set of constants means
every episode reads as an audiobook. One macro makes that impossible.

`upsert_book` (`scan.c:186`) gains a `kind` parameter; its
`ON CONFLICT(book_key) DO UPDATE SET` list (`scan.c:198-205`) needs `kind` added
so a row's kind is corrected if a folder moves between roots.

Kind filters:

| Query                      | Change                                           |
| -------------------------- | ------------------------------------------------ |
| `SQL_LIST_BOOKS`           | `WHERE b.kind=0`                                 |
| `SQL_LIST_AUTHORS`         | `AND b.kind=0`                                   |
| `SQL_LIST_SERIES`          | `AND b.kind=0`                                   |
| `SQL_LIST_FOLDERS`         | `AND kind=0`                                     |
| `SQL_LIST_BOOKS_BY_AUTHOR` | `AND b.kind=0`                                   |
| `SQL_LIST_BOOKS_BY_SERIES` | `AND b.kind=0`                                   |
| `SQL_LIST_FINISHED`        | join `progress`, `p.completed=1`, `AND b.kind=0` |
| `SQL_LIST_CONTINUE`        | **no** kind filter (the vetoable decision above) |

**Deliberately not filtered: the ad-hoc Folders query at `ui.c:1620-1626`.**
Its predicate is `WHERE b.root_path=?1 OR (b.root_path>=?2 AND b.root_path<?3)`
with the range bounds built from the Audiobooks root (`ui.c:1601-1604`), and
under BINARY collation `/usr/data/mnt/sd_0/Podcasts/…` cannot fall inside
`[".../Audiobooks/", ".../Audiobooks0")` because `P` > `A`. Podcasts are already
excluded at every drill level. Adding `AND b.kind=0` there would buy nothing and
is exactly where `AND` binding tighter than `OR` would silently drop the
books-at-this-level rows.

Two new queries and their wrappers, mirroring the `series` pair:

- `audiobook_list_shows`: `SELECT DISTINCT s.display_name ... WHERE b.kind=1
ORDER BY s.sort_name`
- `audiobook_list_episodes_by_show`: the shared column list,
  `WHERE b.kind=1 AND s.display_name=? ORDER BY b.series_number DESC`

## UI changes (`audiobook_app/ui.c` / `ui.h`)

Two new `list_mode_t` values on the existing `SCREEN_LIST`; no new screen:

- `LIST_SHOWS`: strlist of show names
- `LIST_SHOW_EPISODES`: book rows filtered by `ui->list_filter`

`list_item_t` gains `int kind` so an episode row can render a **"Played"** badge
where a book renders "Done". Two care points:

- The field must be set explicitly at all **three** row-construction sites:
  `list_collect_cb` (`ui.c:1550-1562`), the book-at-this-level row
  (`ui.c:1649-1660`) and the folder rows built after the memmove
  (`ui.c:1722-1733`). That buffer comes from `realloc` (`ui.c:1713`), not
  `calloc`, so an unset field is garbage rather than zero.
- The badge width is hardcoded twice: `ui.c:2094` reserves
  `render_text_width("Done", …) + 16` for the title truncation and `ui.c:2149`
  draws the label. "Played" is wider, so both must read one variable or the
  title will overlap the badge.

Home: `HOME_SERIES` becomes `HOME_PODCASTS`, its `home_labels` entry
(`ui.c:84-93`) becomes "Podcasts", and its `handle_home_touch` case navigates to
`LIST_SHOWS`. The `LIST_SERIES` / `LIST_SERIES_BOOKS` code stays in place,
simply unreachable from Home, so the change is trivially reversible.

Every site a new `list_mode_t` must be registered, or it fails silently:

- the enum, `ui.h:33-42`
- the `is_str` predicate in `rebuild_list`, `ui.c:1793`, add `LIST_SHOWS`
- the `collect_list_books` switch, `ui.c:1568`, add explicit cases; its
  `default:` silently lists the whole library
- the title switch in `draw_list`, `ui.c:1976-1985`
- the footer wording check, `ui.c:2162` ("%d episodes")
- **the strlist branch in `handle_list_touch`, `ui.c:2188`**, which tests
  `list_mode == LIST_AUTHORS || LIST_SERIES` rather than `list_is_strlist`. A
  strlist mode missing here renders correctly and then silently no-ops on tap.

Follow the documented render-cache contract for the new modes (`ui.c:1775-1779`,
`ui.h:232-238`): all DB I/O outside `g_cache_lock`, swap the pointer under the
lock, free the old buffer outside it, and never hold the lock across a pan
ioctl. `rebuild_list`'s existing two branches already model both shapes.

Note that none of these switches will get compiler help: the build is
`zig cc … -Os` with no `-Wall` or `-Werror`
(`tools/build_r1_audiobook_hook.ps1:82-95`), and `rebuild_screen` (`ui.c:1945`)
has no `default:`. Missed cases fail silently at runtime, so each site has to be
checked by hand.

Hiding the Chapters button when an episode has no chapters means **two** places,
not one:

- Detail, `draw_detail` (`ui.c:2340-2346`) and `handle_detail_touch`
  (`ui.c:2376-2380`). Both must consult the flag or the invisible button stays
  tappable. Keep using the shared `DETAIL_BTN_*` constants (`ui.c:54-61`).
- **Now Playing has its own "Chaps" button**, drawn in `draw_now_playing` and
  hit-tested at `ui.c:2724-2728` with coordinates duplicated inline rather than
  shared. Hiding it only on Detail leaves it live here.

The flag itself needs caching: `ui->ch_rows` / `ch_count` are only built on
navigating to `SCREEN_CHAPTERS` (`rebuild_chapters`, `ui.c:1910`), so
`rebuild_current_book` (`ui.c:1846`) has to compute and cache a
`cur_has_chapters`.

Minor, worth fixing while in there: `draw_chapters` returns early on the empty
case (`ui.c:2909-2915`) **before** setting `ui->scroll_max`, so the stale value
from the previous screen lets an empty list drag-scroll.

The scan trigger at `ui.c:172` calls `audiobook_scan_all`. A missing
`/Podcasts` root must not raise the red flash. Note that flash text is
hardcoded `"Scan failed: storage full"` (`ui.c:1474`), which is already wrong
for several causes and should be generalized since a second root adds more.

## Music catalog isolation (`audiobook_app/music_catalog.c`): done

Without this, podcast files leak into HiBy's stock Music catalog after Update
Database, which is the exact regression v2.0.28 shipped to fix.

Completed and verified:

- `AUDIOBOOK_PATH_SQL` renamed to `APP_PATH_SQL` and extended with the three
  `Podcasts` spellings (`a:\Podcasts\%`, `/mnt/sd_0/Podcasts/%`,
  `/usr/data/mnt/sd_0/Podcasts/%`).
- `delete_path_rows`'s `char sql[512]` raised to 768. The macro grew from ~160
  to ~310 bytes, which still fits, but the margin was thin enough that a future
  root would have silently truncated into malformed SQL.
- `music_catalog_remove_audiobooks{,_default}` renamed to
  `music_catalog_remove_app_paths{,_default}` and the result field
  `audiobook_rows_removed` to `app_rows_removed`, since the function no longer
  only removes audiobooks. Callers updated: `ui.c:133`,
  `tools/music_catalog_cleanup_test_main.c`. The printf label stays `removed=`
  so the Python harness's parsing is unaffected.
- `tools/test_music_catalog_cleanup.py` generalized from a single
  `AUDIOBOOK_LIKE` to an `APP_LIKES` tuple with a generated `APP_MATCH`
  predicate, and the synthetic fixture gained two podcast episodes, one sharing
  artist and album-artist with a Music row so catalog reconciliation is exercised.

Verified: the pre-change test passed (removed 2, preserved 2), and the extended
fixture is the gate for the podcast rows.

## Files to change

| File                                          | Work                                                                                                                                                                                                                                    |
| --------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `audiobook_app/library.h`                     | `AUDIOBOOK_PODCAST_ROOT`, kind enum, `audiobook_book_t.kind`, new API decls, `SCHEMA_VERSION`                                                                                                                                           |
| `audiobook_app/library.c`                     | `kind` column + migration helper, `SQL_BOOK_COLUMNS` extraction, kind filters, Finished progress join, two new queries                                                                                                                  |
| `audiobook_app/scan.c`                        | `kind` param, `audiobook_scan_all` + root table, per-file episode emission, `derive_path_metadata` root param, root-relative covercache, shared show cover, no podcast chapter synthesis, zero-tracks orphan pass, `bd->files` leak fix |
| `audiobook_app/scan.h`                        | signature changes                                                                                                                                                                                                                       |
| `audiobook_app/ui.c`                          | Home row swap, two list modes and all six registration sites, Played badge, completed-read fix in two collectors, conditional Chapters button on Detail **and** Now Playing, `audiobook_scan_all` call                                  |
| `audiobook_app/ui.h`                          | list mode enum, `list_item_t.kind`, cache fields                                                                                                                                                                                        |
| `audiobook_app/player.c`                      | quit-save must preserve `completed` (`:2107`); replaying a finished item starts from 0 using the already-loaded `sd_done` (`:1645`)                                                                                                     |
| ~~`audiobook_app/music_catalog.c`~~           | **done**: podcast path spellings, buffer bump, honest function names                                                                                                                                                                    |
| `audiobook_app/library_test.c`                | new scan signature; exercise a podcast root                                                                                                                                                                                             |
| ~~`tools/test_music_catalog_cleanup.py`~~     | **done**: generalized predicate + podcast fixture rows                                                                                                                                                                                  |
| ~~`tools/music_catalog_cleanup_test_main.c`~~ | **done**: renamed API                                                                                                                                                                                                                   |
| `tools/build_r1_audiobook_hook.ps1`           | only if a new `.c` file is added (current plan adds none)                                                                                                                                                                               |

`hook.c`, `storage_guard.c` and `cover.c` are untouched. The storage guard pins
SD runtime PM for the whole card, so it is already path-agnostic.

## Verification

**Host, no device needed.** `library_test.c` is a standalone harness that runs
the scanner against a directory and prints books, tracks and chapters. Its
sources (`library_test.c`, `library.c`, `scan.c`, `tags.c`, `sqlite3.c`) are
pure POSIX, so it compiles with clang on macOS even though
`tools/build_r1_audiobook_library.ps1` is Windows/Zig only. Build it directly
with clang and check:

1. An audiobook tree produces byte-identical output to a pre-change run
   (capture a baseline first). This is the main regression gate.
2. A podcast tree of one show folder with several files produces one row per
   file, `track_count` 1, `series` = show name, `series_number` ascending in
   natural filename order, and one shared `cover_path`.
3. Episodes have zero chapters unless the file carries embedded chapters.
4. Deleting one episode file and re-scanning removes exactly that row (the
   zero-tracks orphan pass), and the other episodes keep their progress.
5. A flat `/Podcasts` with loose files and no show subfolder still produces one
   row per file rather than one row for everything.
6. Scanning with no `/Podcasts` directory at all returns success, not an error.
7. **The data-loss guard:** a tree with `/Podcasts` present and `/Audiobooks`
   renamed away must leave every audiobook row, `.pos` file and bookmark intact.
   This is the single most important test in the list, so get it in place before
   touching `audiobook_cleanup_orphans`.

**What host builds cannot cover.** `ui.c` and `player.c` include
`<linux/input.h>` and other kernel headers, so they do not compile on macOS
(confirmed). Everything in those two files is only validated by the Windows/Zig
build and on-device testing. Since the build uses no `-Wall`/`-Werror`, treat
the UI registration-site checklist above as a manual review step, not something
the toolchain will catch.

**Catalog isolation.** Run `tools/test_music_catalog_cleanup.py` with podcast
rows added to the synthetic fixture. It already checks zero leakage, that every
legitimate Music row survives, catalog counts, `PRAGMA integrity_check`, and
that a second cleanup is a no-op.

**On device**, per `docs/build_flash_verify_runbook.md`. Everything in `ui.c`
and `player.c` is typecheck-only until this runs.

- Home shows Podcasts where Series was; Podcasts lists shows; a show lists
  episodes in reverse filename order.
- Playing an episode resumes at its own position, independently of every other
  episode. Letting one run to the end marks it Played and stops without
  rolling into the next. Tapping a Played episode restarts from the beginning.
- **Finish an episode, then play a few seconds of a different one, then exit
  and reopen.** Only the first should be Played. This is the cross-book case
  the quit-save fix is scoped for.
- Titles / Authors / Folders show no episodes; a half-played episode appears in
  Continue; a finished one does not reappear there after exiting the app.
- Detail and Now Playing show no Chapters button for an episode without
  embedded chapters, and the space where it was is not tappable.
- **`audiobook_scan_all` itself is untested.** The host suite calls
  `audiobook_scan_root` and `audiobook_cleanup_orphans_scoped` directly, but
  `SCAN_ROOTS` hardcodes the device paths, so on a host `scan_all` takes its
  `scanned_n == 0` early return and the real wiring (skip-missing,
  collect-scanned, cleanup-once, VACUUM-once, the `failures ? -1 : 0` result)
  never runs. Refresh with `/Podcasts` absent, then present, then absent again:
  no red error flash in any case, and no audiobook rows or resume positions
  lost. Then rename `/Audiobooks` away with `/Podcasts` present and refresh:
  audiobooks must survive.
- After Music → Update Database, opening Audiobooks leaves no podcast rows in
  the stock Music catalog.

**Release paperwork** this repo expects: `CHANGELOG.md`, `README.md`,
`docs/production_release_checklist.md`, and
`docs/modding/library_scan_storage.md`. This is a feature, so it wants a minor
bump to v2.1.0 rather than a patch.
