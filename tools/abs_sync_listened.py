#!/usr/bin/env python3
"""Sync listening state between a HiBy R1 SD card and Audiobookshelf.

The R1 writes everything needed onto the SD card itself, so no firmware
involvement:

  <card>/.audiobook_pos/<book_id>.pos          authoritative position/played
  <card>/Audiobooks/.audiobook_library/library.db   book_id -> paths, kind

A .pos file is five lines: track_ordinal, track_pos_ms, book_elapsed_ms,
completed (0/1), unix timestamp. See audiobook_app/posstore.h.

Directions (--direction):
  push   device -> ABS. Read-only with respect to the card.
  pull   ABS -> device. WRITES .pos files on the card.
  both

Intended as a ChronoSync PRE-sync script, so that:
  1. this reconciles state in both directions,
  2. ABS drops played episodes from disk per its retention settings,
  3. the sync then propagates those deletions to the card.
Media deletions therefore flow Mac -> card through the normal sync; this script
only ever writes .pos files, never media.

Exits 0 even on failure unless --strict or --check, because ChronoSync can be
set to abort a sync when a pre-sync script fails and a transient ABS outage
should not stop new episodes reaching the card.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sqlite3
import sys
import tempfile
import time
import unicodedata
import urllib.error
import urllib.request
from pathlib import Path

# Track paths in library.db are absolute device paths; these prefixes are
# stripped to get the card-relative key that also matches an ABS relPath.
DEVICE_ROOTS = {
    "podcast": "/usr/data/mnt/sd_0/Podcasts",
    "book": "/usr/data/mnt/sd_0/Audiobooks",
}
POS_DIRNAME = ".audiobook_pos"
DB_RELPATH = "Audiobooks/.audiobook_library/library.db"

KIND_BOOK, KIND_PODCAST = 0, 1

# A device clock this far ahead of ours means .pos timestamps cannot be trusted
# to order against ABS lastUpdate, so pulling is unsafe.
CLOCK_SKEW_TOLERANCE_S = 3600
# Above this much ahead, warn: still ordered, but a device position can beat a
# genuinely newer ABS change made inside the skew window.
CLOCK_WARN_AHEAD_S = 120
# For a multi-file book, both directions assume the device's track order and
# ABS's concatenation produce the same timeline. Agreeing totals is the
# available proxy for that.
BOOK_DURATION_TOLERANCE_S = 30.0
BOOK_DURATION_TOLERANCE_FRAC = 0.02


def log(msg: str) -> None:
    print(msg, flush=True)


def norm(s: str) -> str:
    """Key for comparing paths across filesystems.

    macOS stores filenames decomposed (NFD) while exFAT keeps whatever it was
    given, so the same item can differ byte-for-byte between the card and what
    ABS reports. Several shows have accented titles, so without NFC
    normalisation the match silently fails. Case is folded too: exFAT is
    case-insensitive and so is a default APFS volume.
    """
    return unicodedata.normalize("NFC", s).casefold()


# ---------------------------------------------------------------- card reading


def find_card(explicit: str | None) -> Path:
    if explicit:
        p = Path(explicit)
        if not (p / POS_DIRNAME).is_dir():
            raise SystemExit(f"no {POS_DIRNAME}/ under {p}")
        return p
    candidates = [
        v for v in Path("/Volumes").iterdir() if (v / POS_DIRNAME).is_dir()
    ] if Path("/Volumes").is_dir() else []
    if not candidates:
        raise SystemExit(
            f"no mounted volume contains {POS_DIRNAME}/ (card not mounted?)")
    if len(candidates) > 1:
        raise SystemExit("multiple candidate cards: "
                         + ", ".join(str(c) for c in candidates)
                         + " (pass --card)")
    return candidates[0]


def read_library(card: Path) -> list[dict]:
    """One row per book/episode, with its tracks in the device's own order."""
    db = card / DB_RELPATH
    if not db.is_file():
        raise SystemExit(f"library.db not found at {db}")
    # Copied before opening: the card is removable and may carry a hot rollback
    # journal, which would make a direct read-only open fail. Under 1 MB, and it
    # guarantees we never write to the DB.
    with tempfile.TemporaryDirectory() as td:
        local = Path(td) / "library.db"
        shutil.copy2(db, local)
        con = sqlite3.connect(f"file:{local}?mode=ro", uri=True)
        try:
            cols = {r[1] for r in con.execute("PRAGMA table_info(books)")}
            if "kind" not in cols:
                raise SystemExit(
                    "library.db has no books.kind column: the card was written "
                    "by a firmware build without podcast support")
            books = con.execute(
                "SELECT book_id, kind, root_path, track_count FROM books"
            ).fetchall()
            db_prog = {
                bid: {"completed": bool(comp),
                      "book_elapsed_ms": elapsed or 0}
                for bid, comp, elapsed in con.execute(
                    "SELECT book_id, completed, total_book_elapsed_ms "
                    "FROM progress")}
            tracks: dict[int, list[dict]] = {}
            for bid, tid, ordinal, path, dur in con.execute(
                    "SELECT book_id, track_id, ordinal, path, duration_ms "
                    "FROM tracks "
                    "ORDER BY book_id, disc_number, track_number, ordinal"):
                tracks.setdefault(bid, []).append(
                    {"track_id": tid, "ordinal": ordinal, "path": path,
                     "duration_ms": dur or 0})
        finally:
            con.close()
    out = []
    for bid, kind, root, tc in books:
        tl = tracks.get(bid, [])
        if not tl:
            continue
        out.append({"book_id": bid, "kind": kind, "root_path": root,
                    "track_count": tc, "tracks": tl,
                    "db_progress": db_prog.get(bid)})
    return out


def read_pos(card: Path, book_id: int) -> dict | None:
    p = card / POS_DIRNAME / f"{book_id}.pos"
    try:
        parts = p.read_text(errors="replace").split()
    except OSError:
        return None
    if len(parts) < 4:
        return None
    try:
        return {
            "track_ordinal": int(parts[0]),
            "track_pos_ms": int(parts[1]),
            "book_elapsed_ms": int(parts[2]),
            "completed": int(parts[3]) == 1,
            # 5th field is the device's save time. Older builds may omit it;
            # fall back to the file mtime so ordering still works.
            "saved_at": int(parts[4]) if len(parts) > 4
            else int(p.stat().st_mtime),
        }
    except (ValueError, OSError):
        return None


def write_pos(card: Path, book_id: int, ordinal: int, track_pos_ms: int,
              book_elapsed_ms: int, completed: bool, saved_at: int) -> None:
    """Write a .pos the way the device does: temp file then rename.

    saved_at is deliberately the source's timestamp (ABS lastUpdate), not now.
    Stamping it with the current time would make the file look newer than the
    ABS state it came from, so the next run would push it straight back and the
    two would ping-pong.
    """
    d = card / POS_DIRNAME
    d.mkdir(exist_ok=True)
    tmp = d / f"{book_id}.pos.tmp"
    tmp.write_text(f"{ordinal}\n{track_pos_ms}\n{book_elapsed_ms}\n"
                   f"{1 if completed else 0}\n{saved_at}\n")
    os.replace(tmp, d / f"{book_id}.pos")


# Mirrors SQL_SAVE_PROGRESS in library.c. The .pos file is authoritative for
# RESUME, but the list UI reads progress rows via audiobook_get_progress, so a
# pull that writes only .pos resumes correctly and shows no Played badge.
SQL_UPSERT_PROGRESS = (
    "INSERT INTO progress(book_id,track_id,track_ordinal,position_ms,"
    "total_book_elapsed_ms,playback_speed,last_played_at,completed,"
    "completed_at,last_saved_at,protected_until_ms) "
    "VALUES(?,?,?,?,?,1.0,?,?,?,?,0) ON CONFLICT(book_id) DO UPDATE SET "
    "track_id=excluded.track_id,track_ordinal=excluded.track_ordinal,"
    "position_ms=excluded.position_ms,"
    "total_book_elapsed_ms=excluded.total_book_elapsed_ms,"
    "last_played_at=excluded.last_played_at,completed=excluded.completed,"
    "completed_at=excluded.completed_at,last_saved_at=excluded.last_saved_at")


def write_progress_db(card: Path, book_id: int, track_id: int | None,
                      ordinal: int, position_ms: int, book_elapsed_ms: int,
                      completed: bool, when: int) -> None:
    """Upsert the library.db progress row the device's list views read.

    Best-effort by design: the caller keeps the .pos write even if this fails,
    since .pos is what playback resumes from. Safe to write with the device
    powered off, and the schema uses journal_mode=DELETE so there is no WAL to
    leave behind.
    """
    con = sqlite3.connect(card / DB_RELPATH, timeout=10)
    try:
        con.execute(SQL_UPSERT_PROGRESS,
                    (book_id, track_id, ordinal, position_ms, book_elapsed_ms,
                     when, 1 if completed else 0, when if completed else 0,
                     when))
        con.commit()
    finally:
        con.close()


def split_position(tracks: list[dict], book_elapsed_ms: int) -> tuple[dict, int]:
    """Book-relative ms -> (track, ms within that track).

    Mirrors the accumulate-durations walk the player itself does in cmd_seek.
    """
    acc = 0
    for i, t in enumerate(tracks):
        if book_elapsed_ms < acc + t["duration_ms"] or i == len(tracks) - 1:
            return t, max(0, book_elapsed_ms - acc)
        acc += t["duration_ms"]
    return {"ordinal": 1, "track_id": None}, max(0, book_elapsed_ms)


def collect_device_state(card: Path, kinds: set[int]) -> dict[str, dict]:
    """relative key -> device state, for the requested kinds."""
    out: dict[str, dict] = {}
    for row in read_library(card):
        if row["kind"] not in kinds:
            continue
        root = DEVICE_ROOTS["podcast" if row["kind"] == KIND_PODCAST else "book"]
        prefix = root.rstrip("/") + "/"
        # A podcast episode is keyed on its FILE, a book on its FOLDER, which is
        # what makes each match the corresponding ABS relPath.
        raw = (row["tracks"][0]["path"] if row["kind"] == KIND_PODCAST
               else row["root_path"])
        if not raw.startswith(prefix):
            continue
        rel = raw[len(prefix):]
        pos = read_pos(card, row["book_id"])
        out[norm(rel)] = {
            "rel": rel, "book_id": row["book_id"], "kind": row["kind"],
            "tracks": row["tracks"],
            "device_total_ms": sum(t["duration_ms"] for t in row["tracks"]),
            "pos": pos, "db_progress": row["db_progress"],
        }
    return out


# ----------------------------------------------------------------- ABS client


class Abs:
    def __init__(self, base: str, token: str, timeout: int = 30):
        self.base = base.rstrip("/")
        self.token = token
        self.timeout = timeout

    def _req(self, method: str, path: str, body: dict | None = None):
        url = f"{self.base}/api{path}"
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(url, data=data, method=method)
        req.add_header("Authorization", f"Bearer {self.token}")
        if data:
            req.add_header("Content-Type", "application/json")
        with urllib.request.urlopen(req, timeout=self.timeout) as r:
            raw = r.read()
            ctype = (r.headers.get("Content-Type") or "").lower()
        if not raw:
            return None
        # Progress updates answer 200 with a plain-text "OK" body, not JSON.
        # Parsing unconditionally raised AFTER the server had applied the
        # change, so every successful update was reported as a failure.
        if "json" not in ctype:
            return raw.decode("utf-8", "replace").strip()
        return json.loads(raw)

    def get(self, path: str):
        return self._req("GET", path)

    def patch(self, path: str, body: dict):
        """Media progress is PATCH, verified against a live server.

        The published API reference says POST; POST returns 404 and PATCH
        returns 200. The episode id is required for podcasts: PATCH on the
        library-item id alone returns 400.
        """
        return self._req("PATCH", path, body)

    def episodes(self, library_id: str, rule=(None, None)) -> dict[str, dict]:
        """Podcast episodes, keyed to match the card's layout.

        item.relPath is the show folder relative to the ABS library root and
        audioFile.metadata.relPath is the file within it, which together form
        the same relative path the card uses.
        """
        out: dict[str, dict] = {}
        for stub in self.get(f"/libraries/{library_id}/items?limit=1000"
                             ).get("results", []):
            item = self.get(f"/items/{stub['id']}")
            show = (item.get("relPath") or "").strip("/")
            for ep in (item.get("media") or {}).get("episodes") or []:
                af = ep.get("audioFile") or {}
                md = af.get("metadata") or {}
                rel = (md.get("relPath") or md.get("filename") or "").strip("/")
                if not rel:
                    continue
                key = f"{show}/{rel}" if show else rel
                out[norm(key)] = {
                    "rel": key, "kind": KIND_PODCAST,
                    "library_item_id": ep.get("libraryItemId") or stub["id"],
                    "episode_id": ep["id"], "duration": af.get("duration"),
                    "finished_secs": rule[0], "finished_pct": rule[1],
                }
        return out

    def books(self, library_id: str, rule=(None, None)) -> dict[str, dict]:
        """Audiobooks, keyed on the item folder (its relPath)."""
        out: dict[str, dict] = {}
        for stub in self.get(f"/libraries/{library_id}/items?limit=2000"
                             ).get("results", []):
            rel = (stub.get("relPath") or "").strip("/")
            if not rel:
                continue
            media = stub.get("media") or {}
            dur = media.get("duration")
            if dur is None:
                dur = (media.get("metadata") or {}).get("duration")
            out[norm(rel)] = {
                "rel": rel, "kind": KIND_BOOK,
                "library_item_id": stub["id"], "episode_id": None,
                "duration": dur,
                "finished_secs": rule[0], "finished_pct": rule[1],
            }
        return out

    def finished_rule(self, library_id: str) -> tuple[float | None, float | None]:
        """The library's own 'mark as finished' thresholds, if configured.

        Used as the default so ABS stays the single place this is configured.
        The server does NOT apply these to progress pushed over the API: at 30s
        remaining a PATCH leaves isFinished false even with the setting at 10s,
        because the rule runs on ABS's own playback sessions.
        """
        try:
            d = self.get(f"/libraries/{library_id}")
        except (urllib.error.URLError, OSError, ValueError):
            return None, None
        s = ((d.get("library") or d) or {}).get("settings") or {}
        secs = s.get("markAsFinishedTimeRemaining")
        pct = s.get("markAsFinishedPercentComplete")
        return (float(secs) if secs else None), (float(pct) if pct else None)

    def progress(self) -> dict[tuple[str, str | None], dict]:
        out = {}
        for p in self.get("/me").get("mediaProgress") or []:
            out[(p["libraryItemId"], p.get("episodeId"))] = p
        return out


# --------------------------------------------------------------- reconcile


def near_end(elapsed_s: float, duration: float | None,
             flat_secs: float | None, frac: float | None,
             pct: float | None = None) -> bool:
    """Treat 'almost at the end' as finished.

    The device only sets completed=1 when the decoder runs off the true end of
    the file: there is no percentage threshold in the firmware. Audiobooks are
    usually played out, but podcasts routinely end with 30-90s of outro, so
    stopping when the content ends would leave an episode permanently
    unfinished, never synced and never cleaned up.
    """
    if not duration or duration <= 0:
        return False
    if pct and (elapsed_s / duration) >= pct:
        return True
    if not flat_secs:
        return False
    allowance = min(flat_secs, duration * frac) if frac else flat_secs
    return (duration - elapsed_s) <= allowance


# Positions within this much of each other are the same place. The device
# stores integer ms and ABS a float of seconds, so exact equality never holds.
SAME_POSITION_S = 1.0

# A few seconds of playback is not a listening position, it is a stray tap.
# Syncing those just fills Continue Listening on both sides with noise, so an
# unfinished position below this is ignored. Finishing is always synced.
DEFAULT_MIN_POSITION_S = 30.0


def materially_same(dev_secs: float | None, dev_fin: bool,
                    abs_secs: float | None, abs_fin: bool) -> bool:
    """Would syncing either way be a no-op?

    Without this, an item whose two sides already agree still gets rewritten
    whenever one timestamp happens to be newer, so every run would rewrite .pos
    files on the card and fill the log with churn.
    """
    if dev_secs is None or abs_secs is None:
        return False
    if dev_fin != abs_fin:
        return False
    return abs(dev_secs - abs_secs) <= SAME_POSITION_S


def timeline_trustworthy(dev: dict, tgt: dict) -> bool:
    """Do the device and ABS agree on this item's timeline?

    Only in question for a multi-file book, where the device sums its own track
    durations in its own order and ABS concatenates in its. A single-file item
    has nothing to disagree about. Agreeing totals is the available proxy; if
    they differ, positions would land somewhere else entirely, so skip.
    """
    if dev["kind"] == KIND_PODCAST or len(dev["tracks"]) <= 1:
        return True
    abs_total = tgt.get("duration")
    if not abs_total:
        return False
    dev_total = dev["device_total_ms"] / 1000.0
    tol = max(BOOK_DURATION_TOLERANCE_S,
              abs_total * BOOK_DURATION_TOLERANCE_FRAC)
    return abs(dev_total - abs_total) <= tol


def reconcile(device: dict[str, dict], targets: dict[str, dict],
              progress: dict[tuple[str, str | None], dict], *,
              direction: str, finished_secs: float | None,
              finished_frac: float | None, finished_pct: float | None,
              allow_rewind: bool, can_pull: bool,
              min_position_s: float = DEFAULT_MIN_POSITION_S,
              rewind_grace_s: float = 60.0) -> tuple[list, list, list]:
    """Pure decision step. Returns (actions, unmatched, skipped).

    An action is (kind_of_action, dev, tgt, payload) where kind_of_action is
    "push" or "pull".
    """
    actions, unmatched, skipped = [], [], []

    for key in sorted(device, key=lambda k: device[k]["rel"]):
        dev = device[key]
        tgt = targets.get(key)
        if tgt is None:
            unmatched.append(dev["rel"])
            continue
        if not timeline_trustworthy(dev, tgt):
            skipped.append(f"{dev['rel']}: device and ABS disagree on total "
                           f"duration, refusing to map positions")
            continue

        cur = progress.get((tgt["library_item_id"], tgt["episode_id"]))
        pos = dev["pos"]
        dev_secs = (pos["book_elapsed_ms"] / 1000.0) if pos else None
        dev_when = pos["saved_at"] if pos else None
        abs_secs = (cur.get("currentTime") or 0.0) if cur else None
        abs_when = ((cur.get("lastUpdate") or 0) / 1000.0) if cur else None
        dur = tgt.get("duration")

        # Each library configures its own markAsFinishedTimeRemaining (yours
        # are 10s for podcasts, 30s for audiobooks), so use the one belonging
        # to this item unless the CLI overrode it.
        item_secs = (finished_secs if finished_secs is not None
                     else tgt.get("finished_secs"))
        item_pct = (finished_pct if finished_pct is not None
                    else tgt.get("finished_pct"))
        dev_fin = bool(pos and (pos["completed"] or near_end(
            dev_secs, dur, item_secs, finished_frac, item_pct)))
        abs_fin = bool(cur and cur.get("isFinished"))

        if pos is None and cur is None:
            continue
        if materially_same(dev_secs, dev_fin, abs_secs, abs_fin):
            continue

        # Discount a sub-floor unfinished position as a stray tap rather than a
        # listening position, and do it BEFORE choosing a winner. Applying the
        # floor later (inside the push branch, after the rewind guard) meant a
        # 2-second tap counted as real device state, lost the rewind check
        # against a genuine ABS position, and deadlocked there permanently.
        # Discounting it makes the other side the source, so the stray value
        # gets overwritten instead of blocking forever.
        dev_meaningful = bool(pos) and (dev_fin or dev_secs >= min_position_s)
        abs_meaningful = bool(cur) and (abs_fin or abs_secs >= min_position_s)

        # Decide which side is authoritative: the more recent save wins, but
        # only among sides that hold something worth syncing.
        if not dev_meaningful and not abs_meaningful:
            continue
        if not dev_meaningful:
            newer = "abs"
        elif not abs_meaningful:
            newer = "device"
        else:
            newer = "device" if dev_when > abs_when else "abs"

        if newer == "device" and direction in ("push", "both"):
            if abs_fin and dev_fin:
                continue
            # Never move a position backwards. A brief tap on the device is
            # "newer" than a genuine position set in the web player, and
            # pushing it would silently destroy real listening state. Finishing
            # is exempt: it is not a regression.
            if cur and not dev_fin and not allow_rewind \
                    and dev_secs + rewind_grace_s < abs_secs:
                skipped.append(
                    f"{dev['rel']}: device {dev_secs:.0f}s is behind ABS "
                    f"{abs_secs:.0f}s, not rewinding ABS")
                continue
            body: dict[str, object] = {}
            if dev_fin:
                if abs_fin:
                    continue
                body["isFinished"] = True
            else:
                body["currentTime"] = round(dev_secs, 3)
                if dur:
                    body["duration"] = dur
                    body["progress"] = round(min(dev_secs / dur, 1.0), 6)
            actions.append(("push", dev, tgt,
                            {"body": body, "finished": dev_fin,
                             "by_threshold": dev_fin and not pos["completed"]}))

        elif newer == "abs" and direction in ("pull", "both"):
            if not can_pull:
                continue
            if dev_fin and abs_fin:
                continue
            # Symmetric guard: do not rewind the device either.
            if pos and not abs_fin and not allow_rewind \
                    and abs_secs + rewind_grace_s < dev_secs:
                skipped.append(
                    f"{dev['rel']}: ABS {abs_secs:.0f}s is behind device "
                    f"{dev_secs:.0f}s, not rewinding the device")
                continue
            # A finished ABS item reports currentTime 0, so write elapsed 0 and
            # let the completed flag stand: the player restarts a finished item
            # from the beginning anyway.
            elapsed_ms = 0 if abs_fin else int(round(abs_secs * 1000))
            track, within = split_position(dev["tracks"], elapsed_ms)
            actions.append(("pull", dev, tgt, {
                "ordinal": track["ordinal"], "track_id": track.get("track_id"),
                "track_pos_ms": within,
                "book_elapsed_ms": elapsed_ms, "completed": abs_fin,
                # Carry ABS's own timestamp so the file does not look newer
                # than the state it came from.
                "saved_at": int(abs_when or time.time()),
                "abs_secs": abs_secs,
            }))

    return actions, unmatched, skipped


# ----------------------------------------------------------------------- main


def repair_db_mirror(card: Path, device: dict[str, dict],
                     dry_run: bool) -> list[str]:
    """Make each library.db progress row agree with its .pos file.

    .pos is authoritative, but the list UI reads progress rows, so the two must
    agree for played state and progress bars to show. They can diverge two ways:
    the device mirrors .pos into the DB only once a minute, and an earlier
    version of this script pulled by writing .pos alone, leaving items that
    resume correctly while showing nothing in the UI.

    Writing the DB to match .pos is always safe: .pos is the newer, canonical
    copy by construction.
    """
    fixed = []
    for dev in sorted(device.values(), key=lambda d: d["rel"]):
        pos = dev["pos"]
        if not pos:
            continue
        cur = dev.get("db_progress")
        same = (cur is not None
                and cur["completed"] == pos["completed"]
                and abs(cur["book_elapsed_ms"] - pos["book_elapsed_ms"]) <= 1000)
        if same:
            continue
        was = ("no row" if cur is None
               else f"{cur['book_elapsed_ms'] // 1000}s"
                    f"{' played' if cur['completed'] else ''}")
        now = (f"{pos['book_elapsed_ms'] // 1000}s"
               f"{' played' if pos['completed'] else ''}")
        fixed.append(f"{dev['rel']}: db progress {was} -> {now}")
        if dry_run:
            continue
        track, within = split_position(dev["tracks"], pos["book_elapsed_ms"])
        try:
            write_progress_db(card, dev["book_id"], track.get("track_id"),
                              pos["track_ordinal"] or track["ordinal"],
                              pos["track_pos_ms"], pos["book_elapsed_ms"],
                              pos["completed"], pos["saved_at"])
        except (sqlite3.Error, OSError) as exc:
            fixed[-1] += f" (FAILED: {exc})"
    return fixed


def plan(args, device: dict[str, dict], targets: dict[str, dict],
         progress: dict, can_pull: bool) -> tuple[list, list, list]:
    """Translate parsed options into reconcile() arguments and apply
    --only-finished.

    Extracted from main so the option wiring is reachable from a test. It was
    not, and a rename left a stale local referenced here: the run died with a
    NameError, which the exit-0 guard then turned into a silent no-op.

    finished_remaining_secs is passed straight through and may be None, which
    reconcile reads as "use each target's own library setting".
    """
    actions, unmatched, skipped = reconcile(
        device, targets, progress,
        direction=args.direction,
        finished_secs=args.finished_remaining_secs,
        finished_frac=args.finished_remaining_frac,
        finished_pct=None,
        allow_rewind=args.allow_rewind,
        can_pull=can_pull,
        min_position_s=args.min_position_secs)
    if args.only_finished:
        actions = [a for a in actions
                   if (a[0] == "push" and a[3]["finished"])
                   or (a[0] == "pull" and a[3]["completed"])]
    return actions, unmatched, skipped


def device_clock_ok(device: dict[str, dict]) -> tuple[bool, str]:
    """Detect a device clock running AHEAD, the only direction a .pos can show.

    Ordering compares .pos timestamps against ABS lastUpdate, so skew matters.
    But the evidence is one-sided:

      AHEAD  is detectable. A save timestamped in the future is impossible with
             a correct clock, so it proves skew. It is also the dangerous
             direction: a .pos written at real time T looks like T+skew and
             beats any ABS change made inside that window.

      BEHIND is NOT detectable here. The newest .pos is simply when the user
             last listened, so a timestamp in the past is normal and says
             nothing about the clock. An earlier version subtracted now from it
             and reported ordinary usage as "39 min BEHIND", which was just
             wrong: a device untouched for a week would have read as a week
             behind.

    So a clock running behind can slip through, biasing ordering toward ABS.
    The symmetric no-rewind guard is what limits the damage there.
    """
    stamps = [d["pos"]["saved_at"] for d in device.values() if d["pos"]]
    if not stamps:
        return True, "no saved positions to check"
    ahead = max(stamps) - int(time.time())
    if ahead > CLOCK_SKEW_TOLERANCE_S:
        return False, (f"a saved position is dated {ahead / 3600:.1f}h in the "
                       f"future, so the device clock is ahead; refusing to "
                       f"pull, positions cannot be ordered")
    if ahead > CLOCK_WARN_AHEAD_S:
        return True, (f"WARNING a saved position is dated ~{ahead / 60:.0f} min "
                      f"in the future, so the device clock is ahead and a "
                      f"device position can wrongly beat a newer ABS change. "
                      f"Set the R1's clock.")
    age = -ahead
    return True, (f"no clock skew detected (newest saved position is "
                  f"{age / 60:.0f} min old)")


def run_check(args) -> int:
    ok = True

    def step(label, fn):
        nonlocal ok
        try:
            log(f"  ok    {label}: {fn()}")
        # SystemExit too: find_card and read_library bail that way, and
        # SystemExit is not an Exception, so it would otherwise escape and be
        # swallowed by the exit-0 guard in __main__.
        except (Exception, SystemExit) as exc:
            log(f"  FAIL  {label}: {exc}")
            ok = False
            return False
        return True

    log(f"host: {os.uname().nodename}")
    log(f"direction: {args.direction}")

    tf = Path(os.path.expanduser(args.token_file))
    token = ""
    if step("token file", lambda: f"{tf} ({tf.stat().st_size} bytes, mode "
                                 f"{oct(tf.stat().st_mode & 0o777)})"):
        token = tf.read_text().strip()
        if not token:
            log("  FAIL  token file: empty")
            ok = False

    abs_ = Abs(args.abs_url, token)
    if token:
        step("ABS reachable + token valid",
             lambda: f"{args.abs_url} as user "
                     f"{abs_.get('/me').get('username')!r}")

        def check_lib(lib_id, want):
            d = abs_.get(f"/libraries/{lib_id}")
            lib = d.get("library") or d
            if lib.get("mediaType") != want:
                raise RuntimeError(f"mediaType {lib.get('mediaType')!r}, "
                                   f"expected {want!r}")
            secs, pct = abs_.finished_rule(lib_id)
            rule = (f"finished at <= {secs:g}s remaining" if secs
                    else f"finished at >= {pct:g} complete" if pct
                    else "no finished threshold set (requires true EOF)")
            return f"{lib.get('name')!r}, {rule}"
        step("podcast library", lambda: check_lib(args.library_id, "podcast"))
        if args.book_library_id:
            step("audiobook library",
                 lambda: check_lib(args.book_library_id, "book"))
        else:
            log("  --    audiobook library: not configured "
                "(pass --book-library-id to include audiobooks)")

    def check_card():
        card = find_card(args.card)
        kinds = {KIND_PODCAST} | ({KIND_BOOK} if args.book_library_id else set())
        state = collect_device_state(card, kinds)
        with_pos = [d for d in state.values() if d["pos"]]
        played = sum(1 for d in with_pos if d["pos"]["completed"])
        clock_ok, why = device_clock_ok(state)
        if not clock_ok:
            raise RuntimeError(f"{why}; pulling would be unsafe")
        return (f"{card}: {len(state)} items, {len(with_pos)} with saved "
                f"state, {played} played; clock {why}")
    step("card", check_card)

    log("check passed" if ok else "check FAILED")
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Sync listening state between a HiBy R1 card and "
                    "Audiobookshelf")
    ap.add_argument("--abs-url", required=True,
                    help="ABS base URL, including any reverse-proxy subpath "
                         "(e.g. http://host:13378 or http://host/audiobookshelf)")
    ap.add_argument("--library-id", required=True, help="ABS podcast library id")
    ap.add_argument("--book-library-id",
                    help="ABS audiobook library id. Omit to sync podcasts only.")
    ap.add_argument("--token-file", default="~/.config/abs/token")
    ap.add_argument("--card", help="card mount point (default: auto-detect)")
    ap.add_argument("--direction", choices=("push", "pull", "both"),
                    default="push",
                    help="push: device -> ABS (read-only on the card). "
                         "pull: ABS -> device (WRITES .pos files). "
                         "Default push.")
    ap.add_argument("--dry-run", action="store_true",
                    help="report what would change, change nothing")
    ap.add_argument("--only-finished", action="store_true",
                    help="skip partial positions, sync played state only")
    ap.add_argument("--finished-remaining-secs", type=float, default=None,
                    help="treat an item as finished when this many seconds "
                         "remain. Default: the library's own "
                         "markAsFinishedTimeRemaining, so ABS stays the one "
                         "place this is configured. 0 requires true EOF.")
    ap.add_argument("--finished-remaining-frac", type=float, default=0.10,
                    # Not an ABS concept; ours, to protect short episodes.
                    help="cap the above at this fraction of the item, so short "
                         "episodes need a tighter margin (default 0.10)")
    ap.add_argument("--min-position-secs", type=float,
                    default=DEFAULT_MIN_POSITION_S,
                    help="ignore unfinished positions shorter than this, on "
                         "both sides, so a stray tap does not become a "
                         "Continue Listening entry (default 30). Finishing is "
                         "always synced.")
    ap.add_argument("--allow-rewind", action="store_true",
                    help="permit moving a position EARLIER on either side "
                         "(off by default: it destroys real listening state)")
    ap.add_argument("--strict", action="store_true",
                    help="exit non-zero on error (default exits 0 so a "
                         "ChronoSync pre-sync failure cannot block the sync)")
    ap.add_argument("--check", action="store_true",
                    help="verify token, ABS, libraries and card, then stop")
    args = ap.parse_args()

    if args.check:
        return run_check(args)

    token = Path(os.path.expanduser(args.token_file)).read_text().strip()
    if not token:
        raise SystemExit(f"empty token file: {args.token_file}")

    card = find_card(args.card)
    kinds = {KIND_PODCAST} | ({KIND_BOOK} if args.book_library_id else set())
    device = collect_device_state(card, kinds)
    log(f"card: {card} ({len(device)} items, "
        f"{sum(1 for d in device.values() if d['pos'])} with saved state)")

    # Pulling orders the device's .pos timestamps against ABS lastUpdate, so a
    # device clock in the future would let stale state win. Push has the
    # no-regression guard as a backstop; pull writes to the card, so refuse.
    can_pull = True
    if args.direction in ("pull", "both"):
        can_pull, why = device_clock_ok(device)
        if not can_pull:
            log(f"NOT pulling: {why}")
        else:
            log(f"clock: {why}")

    abs_ = Abs(args.abs_url, token)
    pod_rule = abs_.finished_rule(args.library_id)
    book_rule = (abs_.finished_rule(args.book_library_id)
                 if args.book_library_id else (None, None))
    if args.finished_remaining_secs is not None:
        log(f"finished when <= {args.finished_remaining_secs:g}s remain, all "
            f"libraries [from --finished-remaining-secs]")
    else:
        def describe(name, rule):
            secs, pct = rule
            if secs:
                return f"{name}: <= {secs:g}s remaining"
            if pct:
                return f"{name}: >= {pct:g} complete"
            return f"{name}: true end-of-file only"
        parts = [describe("podcasts", pod_rule)]
        if args.book_library_id:
            parts.append(describe("audiobooks", book_rule))
        log("finished thresholds [each library's own setting] "
            + "; ".join(parts)
            + f" (capped at {args.finished_remaining_frac:g} of duration)")

    targets = abs_.episodes(args.library_id, pod_rule)
    if args.book_library_id:
        targets.update(abs_.books(args.book_library_id, book_rule))
    log(f"items known to ABS: {len(targets)}")
    prog = abs_.progress()

    actions, unmatched, skipped = plan(args, device, targets, prog, can_pull)

    for rel in unmatched:
        log(f"  no ABS match: {rel}")
    for msg in skipped:
        log(f"  skip: {msg}")
    if not actions:
        log("nothing to update")
        run_mirror_repair(args, card, device)
        return 0

    run_mirror_repair(args, card, device)

    sent = failed = 0
    for kind, dev, tgt, payload in actions:
        if kind == "push":
            what = ("played*" if payload["by_threshold"]
                    else "played" if payload["finished"]
                    else f"{payload['body'].get('currentTime', 0):.0f}s")
            arrow = "->ABS"
        else:
            what = ("played" if payload["completed"]
                    else f"{payload['abs_secs']:.0f}s")
            arrow = "->R1 "
        if args.dry_run:
            log(f"  DRY  {arrow} {what:>9}  {dev['rel']}")
            continue
        try:
            if kind == "push":
                ep = f"/{tgt['episode_id']}" if tgt["episode_id"] else ""
                abs_.patch(f"/me/progress/{tgt['library_item_id']}{ep}",
                           payload["body"])
            else:
                write_pos(card, dev["book_id"], payload["ordinal"],
                          payload["track_pos_ms"], payload["book_elapsed_ms"],
                          payload["completed"], payload["saved_at"])
                # Also the DB row, or the device resumes correctly but shows
                # no Played badge and no progress bar.
                try:
                    write_progress_db(
                        card, dev["book_id"], payload["track_id"],
                        payload["ordinal"], payload["track_pos_ms"],
                        payload["book_elapsed_ms"], payload["completed"],
                        payload["saved_at"])
                except (sqlite3.Error, OSError) as exc:
                    log(f"       (.pos written; library.db progress row "
                        f"failed: {exc})")
            log(f"  sent {arrow} {what:>9}  {dev['rel']}")
            sent += 1
        except (urllib.error.URLError, OSError, ValueError) as exc:
            log(f"  FAIL {arrow} {what:>9}  {dev['rel']}: {exc}")
            failed += 1

    if args.dry_run:
        log(f"dry run: {len(actions)} would be updated")
    else:
        log(f"updated {sent}, failed {failed}")
    return 1 if (failed and args.strict) else 0


def run_mirror_repair(args, card, device) -> None:
    if args.direction == "push":
        return  # read-only on the card in this mode
    fixed = repair_db_mirror(card, device, args.dry_run)
    for msg in fixed:
        log(f"  {'DRY  ' if args.dry_run else 'sent '} db-mirror  {msg}")


if __name__ == "__main__":
    # The exit-0 guard exists so an unattended ChronoSync pre-sync failure
    # cannot block a sync. It must NOT apply to --check, an interactive
    # diagnostic whose whole job is to report failure.
    _report = ("--strict" in sys.argv) or ("--check" in sys.argv)
    try:
        rc = main()
    except SystemExit as exc:
        code = exc.code if isinstance(exc.code, int) else 1
        if code and not _report:
            log(f"error (suppressed, no --strict): {exc}")
            code = 0
        rc = code
    except Exception as exc:  # never take a ChronoSync sync down with us
        log(f"unexpected error: {exc!r}")
        rc = 1 if _report else 0
    sys.exit(rc)
