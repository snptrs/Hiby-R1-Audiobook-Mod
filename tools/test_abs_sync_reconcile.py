#!/usr/bin/env python3
"""Unit tests for the abs_sync_listened reconcile step.

reconcile() is the part that decides which side wins and can therefore destroy
real listening state, so it is kept pure and tested here with no card, no
network and no ABS instance. Run: python3 tools/test_abs_sync_reconcile.py
"""

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from abs_sync_listened import (  # noqa: E402
    KIND_BOOK, KIND_PODCAST, device_clock_ok, reconcile, split_position,
    timeline_trustworthy,
)

NOW = int(time.time())
fails = []


def check(label, got, want):
    if got == want:
        print(f"  ok    {label}")
    else:
        print(f"  FAIL  {label}\n          got  {got!r}\n          want {want!r}")
        fails.append(label)


def dev(rel, elapsed_s=None, completed=False, when=NOW, kind=KIND_PODCAST,
        tracks=None, book_id=1):
    tracks = tracks or [(1, f"/usr/data/mnt/sd_0/Podcasts/{rel}", 3600_000)]
    pos = None
    if elapsed_s is not None:
        pos = {"track_ordinal": 1, "track_pos_ms": int(elapsed_s * 1000),
               "book_elapsed_ms": int(elapsed_s * 1000),
               "completed": completed, "saved_at": when}
    return {rel: {"rel": rel, "book_id": book_id, "kind": kind,
                  "tracks": tracks,
                  "device_total_ms": sum(t[2] for t in tracks), "pos": pos}}


def tgt(rel, duration=3600.0, kind=KIND_PODCAST):
    return {rel: {"rel": rel, "kind": kind, "library_item_id": "item1",
                  "episode_id": "ep1" if kind == KIND_PODCAST else None,
                  "duration": duration}}


def prog(cur_s, finished=False, when=NOW, ep="ep1"):
    return {("item1", ep): {"currentTime": cur_s, "isFinished": finished,
                            "lastUpdate": when * 1000, "duration": 3600.0}}


def run(device, targets, progress, **kw):
    opts = dict(direction="both", finished_secs=60.0, finished_frac=0.10,
                finished_pct=None, allow_rewind=False, can_pull=True)
    opts.update(kw)
    return reconcile(device, targets, progress, **opts)


print("direction and precedence")
a, _, _ = run(dev("a.mp3", 100, when=NOW), tgt("a.mp3"), prog(50, when=NOW - 999))
check("newer device pushes", [(x[0], x[3]["body"]) for x in a],
      [("push", {"currentTime": 100.0, "duration": 3600.0,
                 "progress": round(100 / 3600, 6)})])

a, _, _ = run(dev("a.mp3", 50, when=NOW - 999), tgt("a.mp3"), prog(900, when=NOW))
check("newer ABS pulls", [(x[0], x[3]["book_elapsed_ms"]) for x in a],
      [("pull", 900_000)])

a, _, _ = run(dev("a.mp3", 100, when=NOW), tgt("a.mp3"), prog(50, when=NOW - 9),
              direction="pull")
check("direction=pull suppresses a push", a, [])

a, _, _ = run(dev("a.mp3", 50, when=NOW - 9), tgt("a.mp3"), prog(900, when=NOW),
              direction="push")
check("direction=push suppresses a pull", a, [])

print("no-regression guard, both ways")
a, _, s = run(dev("a.mp3", 19, when=NOW), tgt("a.mp3"), prog(1096, when=NOW - 999))
check("device behind ABS does not rewind ABS", a, [])
check("  and says why", "not rewinding ABS" in s[0], True)

a, _, s = run(dev("a.mp3", 1096, when=NOW - 999), tgt("a.mp3"), prog(19, when=NOW))
check("ABS behind device does not rewind device", a, [])
check("  and says why", "not rewinding the device" in s[0], True)

# Above the minimum-position floor, so this exercises the rewind guard alone.
a, _, _ = run(dev("a.mp3", 100, when=NOW), tgt("a.mp3"), prog(1096, when=NOW - 9),
              allow_rewind=True)
check("--allow-rewind overrides", [x[0] for x in a], ["push"])

print("finishing is never a regression")
a, _, _ = run(dev("a.mp3", 3599, when=NOW), tgt("a.mp3"), prog(1000, when=NOW - 9))
check("near-end device beats a larger ABS position",
      [(x[0], x[3]["body"]) for x in a], [("push", {"isFinished": True})])
a, _, _ = run(dev("a.mp3", 1000, when=NOW - 9), tgt("a.mp3"),
              prog(0, finished=True, when=NOW))
check("ABS finished pulls as completed with elapsed 0",
      [(x[0], x[3]["completed"], x[3]["book_elapsed_ms"]) for x in a],
      [("pull", True, 0)])

print("finished threshold")
a, _, _ = run(dev("a.mp3", 3550, when=NOW), tgt("a.mp3"), {})
check("50s remaining is finished by threshold",
      [(x[3]["finished"], x[3]["by_threshold"]) for x in a], [(True, True)])
a, _, _ = run(dev("a.mp3", 3400, when=NOW), tgt("a.mp3"), {})
check("200s remaining is not", [x[3]["finished"] for x in a], [False])
a, _, _ = run(dev("a.mp3", 100, when=NOW), tgt("a.mp3", duration=120.0), {})
check("short item uses the fractional cap, not the flat 60s",
      [x[3]["finished"] for x in a], [False])
a, _, _ = run(dev("a.mp3", 3550, when=NOW), tgt("a.mp3"), {}, finished_secs=0)
check("threshold 0 requires true EOF", [x[3]["finished"] for x in a], [False])

print("idempotency and no-ops")
a, _, _ = run(dev("a.mp3", 100, when=NOW - 9), tgt("a.mp3"), prog(100, when=NOW))
check("equal state does nothing", a, [])
a, _, _ = run(dev("a.mp3", 3599, when=NOW), tgt("a.mp3"),
              prog(0, finished=True, when=NOW - 9))
check("both finished does nothing", a, [])
acts, unmatched, _ = run(dev("a.mp3", 100), {}, {})
check("no ABS counterpart is reported, not synced", (acts, unmatched),
      ([], ["a.mp3"]))
acts, _, _ = run(dev("a.mp3"), tgt("a.mp3"), {})
check("never played on either side does nothing", acts, [])

print("minimum position floor")
a, _, _ = run(dev("a.mp3", 19, when=NOW), tgt("a.mp3"), {})
check("a stray 19s push is ignored", a, [])
a, _, _ = run(dev("a.mp3", 19, when=NOW), tgt("a.mp3"), {}, min_position_s=0)
check("  unless the floor is lowered", [x[0] for x in a], ["push"])
a, _, _ = run(dev("a.mp3", 200, when=NOW), tgt("a.mp3"), {})
check("a real 200s push still goes", [x[0] for x in a], ["push"])
a, _, _ = run(dev("a.mp3", 5, when=NOW - 999), tgt("a.mp3"), prog(3, when=NOW))
check("a stray 3s pull is ignored", a, [])
a, _, _ = run(dev("a.mp3", 3599, completed=True, when=NOW), tgt("a.mp3"), {})
check("finishing is never floored out", [x[3]["finished"] for x in a], [True])

print("clock skew reporting")
def clockdev(offset):
    return {"k": {"pos": {"saved_at": NOW + offset}}}
ok, why = clockdev, None
ok, why = device_clock_ok(clockdev(0))
check("in sync -> ok", (ok, "within" in why), (True, True))
ok, why = device_clock_ok(clockdev(1800))
check("30 min ahead -> allowed but warns", (ok, "WARNING" in why and "AHEAD" in why),
      (True, True))
ok, why = device_clock_ok(clockdev(7200))
check("2h ahead -> refuses to pull", (ok, "refusing to pull" in why), (False, True))
ok, why = device_clock_ok(clockdev(-7200))
check("2h behind -> allowed, names the direction",
      (ok, "behind" in why.lower()), (True, True))
# The real-world case that was previously reported as a bland "within 35 min",
# which read like a pass and hid the direction entirely.
ok, why = device_clock_ok(clockdev(-2100))
check("35 min behind -> warns and names the direction",
      (ok, "WARNING" in why and "behind" in why.lower()), (True, True))
ok, why = device_clock_ok(clockdev(-60))
check("1 min out -> quiet pass", (ok, "WARNING" in why), (True, False))

print("pull safety")
a, _, _ = run(dev("a.mp3", 50, when=NOW - 999), tgt("a.mp3"), prog(900, when=NOW),
              can_pull=False)
check("can_pull=False (bad device clock) blocks the write", a, [])

print("multi-file books")
tracks = [(1, "/usr/data/mnt/sd_0/Audiobooks/A/B/1.mp3", 1000_000),
          (2, "/usr/data/mnt/sd_0/Audiobooks/A/B/2.mp3", 1000_000),
          (3, "/usr/data/mnt/sd_0/Audiobooks/A/B/3.mp3", 1000_000)]
d = dev("A/B", 1500, kind=KIND_BOOK, tracks=tracks, when=NOW - 999)
t = tgt("A/B", duration=3000.0, kind=KIND_BOOK)
a, _, _ = run(d, t, prog(2500, when=NOW, ep=None))
check("pull splits a book position into track + offset",
      [(x[3]["ordinal"], x[3]["track_pos_ms"]) for x in a], [(3, 500_000)])

t_bad = tgt("A/B", duration=9999.0, kind=KIND_BOOK)
a, _, s = run(d, t_bad, prog(2500, when=NOW, ep=None))
check("mismatched totals refuse to map positions", a, [])
check("  and say why", "disagree on total duration" in s[0], True)

check("single-file book needs no duration agreement",
      timeline_trustworthy(
          {"kind": KIND_BOOK, "tracks": [(1, "x", 1000)],
           "device_total_ms": 1000}, {"duration": 99999.0}), True)

print("split_position")
check("start", split_position(tracks, 0), (1, 0))
check("inside track 2", split_position(tracks, 1_500_000), (2, 500_000))
check("past the end clamps into the last track",
      split_position(tracks, 9_000_000), (3, 7_000_000))

print()
print(f"RESULT: {'FAIL (' + str(len(fails)) + ')' if fails else 'PASS'}")
sys.exit(1 if fails else 0)
