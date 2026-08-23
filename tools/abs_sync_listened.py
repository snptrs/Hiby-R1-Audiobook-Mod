#!/usr/bin/env python3
"""Push podcast listening state from a HiBy R1 SD card into Audiobookshelf.

Read-only with respect to the card and the device. The R1 already writes
everything needed onto the SD card itself, so no firmware involvement:

  <card>/.audiobook_pos/<book_id>.pos          authoritative position/played
  <card>/Audiobooks/.audiobook_library/library.db   book_id -> file path

A .pos file is five lines: track_ordinal, track_pos_ms, book_elapsed_ms,
completed (0/1), unix timestamp. See audiobook_app/posstore.h.

Intended as a ChronoSync PRE-sync script, so that:
  1. this runs and marks played episodes finished in ABS,
  2. ABS drops played episodes from disk per its retention settings,
  3. the sync then propagates those deletions to the card.
Deletions therefore flow Mac -> card through the normal sync, and the device
never deletes anything.

Exits 0 even on failure unless --strict, because ChronoSync can be set to abort
a sync when a pre-sync script fails and a transient ABS outage should not stop
new episodes reaching the card.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sqlite3
import sys
import tempfile
import unicodedata
import urllib.error
import urllib.request
from pathlib import Path

# Where the app mounts the card on the device. Track paths in library.db are
# absolute device paths, so this prefix is stripped to get a card-relative key.
DEVICE_PODCAST_ROOT = "/usr/data/mnt/sd_0/Podcasts"
POS_DIRNAME = ".audiobook_pos"
DB_RELPATH = "Audiobooks/.audiobook_library/library.db"

LIB_KIND_PODCAST = 1


def log(msg: str) -> None:
    print(msg, flush=True)


def norm(s: str) -> str:
    """Key for comparing paths across filesystems.

    macOS stores filenames decomposed (NFD) while exFAT keeps whatever it was
    given, so the same episode can differ byte-for-byte between the card and
    what ABS reports. Several of these shows have accented titles, so without
    NFC normalisation the match silently fails. Case is folded too: exFAT is
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
        v for v in Path("/Volumes").iterdir()
        if (v / POS_DIRNAME).is_dir()
    ] if Path("/Volumes").is_dir() else []
    if not candidates:
        raise SystemExit(
            f"no mounted volume contains {POS_DIRNAME}/ (card not mounted?)")
    if len(candidates) > 1:
        raise SystemExit(
            "multiple candidate cards: "
            + ", ".join(str(c) for c in candidates)
            + " (pass --card)")
    return candidates[0]


def read_episode_paths(card: Path) -> dict[int, str]:
    """book_id -> device track path, for podcast episodes only."""
    db = card / DB_RELPATH
    if not db.is_file():
        raise SystemExit(f"library.db not found at {db}")
    # Copied before opening: the card is removable and may carry a hot rollback
    # journal, which would make a direct read-only open fail. The DB is under
    # 1 MB so this is cheap, and it guarantees we never write to the card.
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
            rows = con.execute(
                "SELECT b.book_id, t.path "
                "FROM books b JOIN tracks t ON t.book_id = b.book_id "
                "WHERE b.kind = ?", (LIB_KIND_PODCAST,)).fetchall()
        finally:
            con.close()
    return {int(bid): path for bid, path in rows if path}


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
            # fall back to the file mtime so the conflict rule still works.
            "saved_at": int(parts[4]) if len(parts) > 4
            else int(p.stat().st_mtime),
        }
    except (ValueError, OSError):
        return None


def collect_device_state(card: Path, device_root: str) -> dict[str, dict]:
    """relative key -> device listening state."""
    out: dict[str, dict] = {}
    prefix = device_root.rstrip("/") + "/"
    for book_id, path in read_episode_paths(card).items():
        if not path.startswith(prefix):
            continue
        pos = read_pos(card, book_id)
        if pos is None:
            continue  # never played
        pos["rel"] = path[len(prefix):]
        out[norm(pos["rel"])] = pos
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
        # Parsing unconditionally raised, and because the exception happened
        # after the server had already applied the change, every successful
        # update was reported as a failure.
        if "json" not in ctype:
            return raw.decode("utf-8", "replace").strip()
        return json.loads(raw)

    def get(self, path: str):
        return self._req("GET", path)

    def patch(self, path: str, body: dict):
        """Media progress is PATCH, verified against a live server.

        The published API reference says POST for this endpoint; POST actually
        returns 404 and PATCH returns 200. The episode id is required for
        podcasts: PATCH on the library-item id alone returns 400.
        """
        return self._req("PATCH", path, body)

    def episodes(self, library_id: str) -> dict[str, dict]:
        """relative key -> episode identity, matching the card's layout.

        item.relPath is the show folder relative to the ABS library root and
        audioFile.metadata.relPath is the file within it, which together form
        the same relative path the card uses.
        """
        items = self.get(f"/libraries/{library_id}/items?limit=1000")
        out: dict[str, dict] = {}
        for stub in items.get("results", []):
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
                    "rel": key,
                    "library_item_id": ep.get("libraryItemId") or stub["id"],
                    "episode_id": ep["id"],
                    "duration": af.get("duration"),
                    "title": ep.get("title"),
                }
        return out

    def finished_rule(self, library_id: str) -> tuple[float | None, float | None]:
        """The library's own 'mark as finished' thresholds, if configured.

        Used as the default so ABS stays the single place this is configured.
        The server does NOT apply these to progress pushed over the API: at 30s
        remaining a PATCH leaves isFinished false even with the setting at 10s,
        because the rule runs on ABS's own playback sessions. So we read the
        number from ABS and do the applying ourselves.

        markAsFinishedTimeRemaining is in seconds; markAsFinishedPercentComplete
        is a 0-1 fraction. Either may be null.
        """
        try:
            d = self.get(f"/libraries/{library_id}")
        except (urllib.error.URLError, OSError, ValueError):
            return None, None
        s = ((d.get("library") or d) or {}).get("settings") or {}
        secs = s.get("markAsFinishedTimeRemaining")
        pct = s.get("markAsFinishedPercentComplete")
        return (float(secs) if secs else None), (float(pct) if pct else None)

    def progress(self) -> dict[tuple[str, str], dict]:
        me = self.get("/me")
        out = {}
        for p in me.get("mediaProgress") or []:
            if p.get("episodeId"):
                out[(p["libraryItemId"], p["episodeId"])] = p
        return out


# ----------------------------------------------------------------------- main


def near_end(elapsed_s: float, duration: float | None,
             flat_secs: float | None, frac: float | None,
             pct: float | None = None) -> bool:
    """Treat 'almost at the end' as finished.

    The device only sets completed=1 when the decoder runs off the true end of
    the file: there is no percentage threshold in the firmware (see the three
    EOF sites in player.c). Audiobooks are usually played out, but podcasts
    routinely end with 30-90s of outro or trailing ads, so stopping when the
    content ends leaves the episode permanently unfinished, never synced, and
    never cleaned up.

    The allowance scales with length, min(flat, duration*frac), so a two-minute
    news bulletin is not declared finished a third of the way in while a
    three-hour episode still gets the full flat allowance.
    """
    if not duration or duration <= 0:
        return False
    if pct and (elapsed_s / duration) >= pct:
        return True
    if not flat_secs:
        return False
    # Scale the allowance down for short episodes so a ten-minute news bulletin
    # is not called finished a third of the way in.
    allowance = min(flat_secs, duration * frac) if frac else flat_secs
    return (duration - elapsed_s) <= allowance


def build_updates(device: dict[str, dict], eps: dict[str, dict],
                  prog: dict[tuple[str, str], dict],
                  only_finished: bool,
                  finished_secs: float | None, finished_frac: float | None,
                  finished_pct: float | None, allow_rewind: bool,
                  rewind_grace_s: float = 60.0) -> tuple[list, list, list]:
    updates, unmatched, skipped = [], [], []
    for key, st in sorted(device.items(), key=lambda kv: kv[1]["rel"]):
        ep = eps.get(key)
        if ep is None:
            unmatched.append(st["rel"])
            continue

        secs = st["book_elapsed_ms"] / 1000.0
        dur = ep.get("duration")
        finished = st["completed"] or near_end(
            secs, dur, finished_secs, finished_frac, finished_pct)

        if only_finished and not finished:
            continue

        cur = prog.get((ep["library_item_id"], ep["episode_id"]))

        # Never move a position backwards. The device's timestamp being newer
        # is not enough on its own: a brief tap on the device (or a stray
        # position from a playback bug) is "newer" than a genuine 18-minute
        # position set in the web player, and pushing it would silently destroy
        # real listening state. Verified the hard way. Finishing is always
        # allowed through, since that is not a regression.
        if cur and not finished and not allow_rewind:
            prev = cur.get("currentTime") or 0
            if secs + rewind_grace_s < prev:
                skipped.append(
                    f"{st['rel']}: device {secs:.0f}s is behind ABS "
                    f"{prev:.0f}s, not rewinding")
                continue

        # Among non-regressive updates, the newer save wins, so repeated syncs
        # do not fight the server.
        if cur and (cur.get("lastUpdate") or 0) / 1000.0 >= st["saved_at"]:
            continue

        body: dict[str, object] = {}
        if finished:
            if cur and cur.get("isFinished"):
                continue
            body["isFinished"] = True
        else:
            body["currentTime"] = round(secs, 3)
            if dur:
                body["duration"] = dur
                body["progress"] = round(min(secs / dur, 1.0), 6)
        st = dict(st, finished=finished,
                  by_threshold=finished and not st["completed"])
        updates.append((ep, st, body))
    return updates, unmatched, skipped


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Push HiBy R1 podcast listening state into Audiobookshelf")
    ap.add_argument("--abs-url", required=True,
                    help="ABS base URL, including any reverse-proxy subpath "
                         "(e.g. http://host:13378 or http://host/audiobookshelf)")
    ap.add_argument("--library-id", required=True, help="ABS podcast library id")
    ap.add_argument("--token-file", default="~/.config/abs/token")
    ap.add_argument("--card", help="card mount point (default: auto-detect)")
    ap.add_argument("--device-root", default=DEVICE_PODCAST_ROOT)
    ap.add_argument("--dry-run", action="store_true",
                    help="report what would change, send nothing")
    ap.add_argument("--only-finished", action="store_true",
                    help="skip partial positions, push played episodes only")
    ap.add_argument("--finished-remaining-secs", type=float, default=None,
                    help="treat an episode as finished when this many seconds "
                         "remain. Default: the library's own "
                         "markAsFinishedTimeRemaining, so ABS stays the one "
                         "place this is configured. 0 requires true EOF.")
    ap.add_argument("--finished-remaining-frac", type=float, default=0.10,
                    # Not an ABS concept; ours, to protect short episodes.
                    help="cap the above at this fraction of the episode, so "
                         "short episodes need a tighter margin (default 0.10)")
    ap.add_argument("--allow-rewind", action="store_true",
                    help="permit pushing a position EARLIER than the one ABS "
                         "already has (off by default: it destroys real "
                         "listening state)")
    ap.add_argument("--strict", action="store_true",
                    help="exit non-zero on error (default exits 0 so a "
                         "ChronoSync pre-sync failure cannot block the sync)")
    args = ap.parse_args()

    token = Path(os.path.expanduser(args.token_file)).read_text().strip()
    if not token:
        raise SystemExit(f"empty token file: {args.token_file}")

    card = find_card(args.card)
    log(f"card: {card}")
    device = collect_device_state(card, args.device_root)
    log(f"episodes with saved state on card: {len(device)}"
        f" ({sum(1 for s in device.values() if s['completed'])} played)")

    abs_ = Abs(args.abs_url, token)

    # Default the finished rule to the library's own setting, so ABS remains
    # the single place it is configured. ABS will not apply it to progress we
    # PATCH in (verified: 30s remaining stays unfinished even with the setting
    # at 10s, because the rule runs on ABS's own playback sessions), so we read
    # the number and apply it ourselves.
    abs_secs, abs_pct = abs_.finished_rule(args.library_id)
    finished_secs = (args.finished_remaining_secs
                     if args.finished_remaining_secs is not None else abs_secs)
    source = ("--finished-remaining-secs"
              if args.finished_remaining_secs is not None
              else "ABS markAsFinishedTimeRemaining")
    if finished_secs:
        log(f"finished when <= {finished_secs:g}s remain"
            f" (capped at {args.finished_remaining_frac:g} of duration)"
            f" [from {source}]")
    elif abs_pct:
        log(f"finished at >= {abs_pct:g} complete"
            f" [from ABS markAsFinishedPercentComplete]")
    else:
        log("no finished threshold configured: requires true end-of-file")

    eps = abs_.episodes(args.library_id)
    log(f"episodes known to ABS: {len(eps)}")
    prog = abs_.progress()

    updates, unmatched, skipped = build_updates(
        device, eps, prog, args.only_finished,
        finished_secs, args.finished_remaining_frac, abs_pct,
        args.allow_rewind)

    for rel in unmatched:
        log(f"  no ABS match: {rel}")
    for msg in skipped:
        log(f"  skip: {msg}")
    if not updates:
        log("nothing to update")
        return 0

    sent = failed = 0
    for ep, st, body in updates:
        if st["finished"]:
            what = "played*" if st["by_threshold"] else "played"
        else:
            what = f"{body.get('currentTime', 0):.0f}s"
        if args.dry_run:
            log(f"  DRY  {what:>9}  {ep['rel']}")
            continue
        try:
            abs_.patch(
                f"/me/progress/{ep['library_item_id']}/{ep['episode_id']}", body)
            log(f"  sent {what:>9}  {ep['rel']}")
            sent += 1
        except (urllib.error.URLError, OSError, ValueError) as exc:
            log(f"  FAIL {what:>9}  {ep['rel']}: {exc}")
            failed += 1

    if args.dry_run:
        log(f"dry run: {len(updates)} would be updated")
    else:
        log(f"updated {sent}, failed {failed}")
    return 1 if (failed and args.strict) else 0


if __name__ == "__main__":
    try:
        rc = main()
    except SystemExit as exc:
        # argparse and our own bail-outs. Honour --strict only for real errors.
        code = exc.code if isinstance(exc.code, int) else 1
        if code and "--strict" not in sys.argv:
            log(f"error (suppressed, no --strict): {exc}")
            code = 0
        rc = code
    except Exception as exc:  # never take a ChronoSync sync down with us
        log(f"unexpected error: {exc!r}")
        rc = 1 if "--strict" in sys.argv else 0
    sys.exit(rc)
