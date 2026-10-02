"""What the player's own view does at a crossing, from Unreal client logs written with -logcrossing.

For every switch involving an Unreal server (into one: "crossing into <room>", out of one into a C# room: "leaving the
Unreal server"), the gap's first frame against the frame before it:
  - turn: how far the view turned in that frame. A turn against the way the view was turning ("backwards") is the view
    going back to an older one: the snap of 2026-10-02, when the gap started from the 20 Hz game tick's yaw.
  - fov: how much the field of view changed. A locked field of view showed all at once here (the zoom pop).
  - with -fakemouse, the turn the hand meant in that frame and how much of it the view did not show ("lost").
Then, for each switch, the turn the hand meant against the turn the view showed from the gap's start until 300 ms after
the next pawn took the view: mouse movement lost (or doubled) anywhere in the hand-over shows there.

Also counted: the "fov off" stretches (a frame whose drawn field of view is not the pawn's own, logged only while the
pawn is the view), and each crossing's "frame(s) without a view, held from a wrong place" line.

    python CrossingSnap.py <log> [<log> ...]          (Windows paths, E:/...)
    python CrossingSnap.py --pass <log> [...]         exit 1 unless the camera's pass thresholds hold

Thresholds (the crossing test plan of 2026-10-03): no backward turn of 1 degree or more in a gap's first frame, a field
of view change under 0.5 degrees there, no "fov off" stretch, and no frame without a view or held from a wrong place.
"""
import re
import sys
from datetime import datetime

STAMP = r"^\[(\d{4}\.\d\d\.\d\d-\d\d\.\d\d\.\d\d:\d{3})\]\[[ 0-9]*\]LogCubeWorld: "
FRAME = re.compile(STAMP + r"frame (\d+): view ([-\d.]+) ([-\d.]+) ([-\d.]+) yaw ([-\d.]+) fov ([-\d.]+) target (\S+) pc (\S+)(.*)$")
TAIL = re.compile(r" pitch ([-\d.]+) lock ([-\d.]+) pawnfov ([-\d.]+) mouse ([-\d.]+) ([-\d.]+) tap ([-\d.]+) ([-\d.]+) turn ([-\d.]+) ([-\d.]+) gap (\d)(?: fake ([-\d.]+) ([-\d.]+))?")
MARK = re.compile(STAMP + r"(crossing into (\S+): |leaving the Unreal server|crossed into (\S+)( \(C#\))?$|entered (\S+)( \(C#\))?$)")
HELD = re.compile(STAMP + r"crossing: (\d+) frame\(s\) without a view, (\d+) held from a wrong place")
FOVOFF = re.compile(STAMP + r"fov off: drawn ([-\d.]+), the pawn's ([-\d.]+), locked ([-\d.]+), in (\S+)")


def ts(s):
    return datetime.strptime(s, "%Y.%m.%d-%H.%M.%S:%f").timestamp()


def turn(a, b):
    return (b - a + 180) % 360 - 180


class Frame:
    __slots__ = ("t", "yaw", "fov", "target", "pc", "pitch", "lock", "pawnfov", "mouse", "tap", "applied", "gap", "fake")

    def __init__(self, m):
        self.t, self.yaw, self.fov, self.target, self.pc = ts(m[1]), float(m[6]), float(m[7]), m[8], m[9]
        tail = TAIL.search(m[10])
        self.gap = None
        self.fake = None
        if tail:
            self.pitch, self.lock, self.pawnfov = float(tail[1]), float(tail[2]), float(tail[3])
            self.mouse, self.tap, self.applied = (float(tail[4]), float(tail[5])), (float(tail[6]), float(tail[7])), (float(tail[8]), float(tail[9]))
            self.gap = tail[10] == "1"
            if tail[11] is not None:
                self.fake = (float(tail[11]), float(tail[12]))


def read(path):
    frames, marks, held, fovoff = [], [], [], []
    room_kind = None   # "Unreal" or "C#": where the player is before each switch
    for line in open(path, encoding="utf-8", errors="replace"):
        m = FRAME.match(line)
        if m:
            frames.append(Frame(m))
            continue
        m = MARK.match(line)
        if m:
            t = ts(m[1])
            if m[3]:
                marks.append((t, "C# -> Unreal" if room_kind == "C#" else "Unreal -> Unreal", m[3]))
            elif m[2].startswith("leaving"):
                marks.append((t, "Unreal -> C#", "a C# room"))
            elif m[4]:
                room_kind = "C#" if m[5] else "Unreal"
            elif m[6]:
                room_kind = "C#" if m[7] else "Unreal"
            continue
        m = HELD.match(line)
        if m:
            held.append((ts(m[1]), int(m[2]), int(m[3])))
            continue
        m = FOVOFF.match(line)
        if m:
            fovoff.append((ts(m[1]), float(m[2]), float(m[3]), float(m[4]), m[5]))
    return frames, marks, held, fovoff


def analyse(paths):
    rows, helds, fovoffs, hands = [], [], [], []
    for path in paths:
        frames, marks, held, fovoff = read(path)
        helds += held
        fovoffs += fovoff
        for t0, kind, room in marks:
            # The gap's first frame: the first one the gap holds after the switch (an older log has no gap field: the
            # first frame drawn after it).
            i = next((k for k, f in enumerate(frames) if f.t >= t0 - 0.0005 and (f.gap is None or f.gap)), None)
            if i is None or i < 12 or frames[i].t - t0 > 0.1:
                continue
            p, q = frames[i - 1], frames[i]
            before = frames[i - 11:i]
            signed = sum(turn(a.yaw, b.yaw) for a, b in zip(before, before[1:]))
            span = before[-1].t - before[0].t
            rate = abs(signed) / span if span > 0 else 0
            step = turn(p.yaw, q.yaw)
            backwards = rate >= 20 and step * signed < 0
            meant = q.fake[0] if q.fake else None
            rows.append(dict(t=t0, kind=kind, room=room, rate=rate, step=step, backwards=backwards, meant=meant,
                             pitch=(q.pitch - p.pitch) if q.gap is not None else None, dfov=q.fov - p.fov,
                             frame_ms=(q.t - p.t) * 1000))
            # The hand-over as a whole: from the frame before the gap until 300 ms after the pawn took the view back.
            if q.fake is None:
                continue
            end = next((k for k in range(i, len(frames)) if not frames[k].gap), None)
            if end is None:
                continue
            stop = next((k for k in range(end, len(frames)) if frames[k].t > frames[end].t + 0.3), len(frames) - 1)
            span_frames = frames[i:stop + 1]
            shown = sum(turn(a.yaw, b.yaw) for a, b in zip(frames[i - 1:stop], frames[i:stop + 1]))
            wanted = sum(f.fake[0] for f in span_frames if f.fake)
            gap_ms = (frames[end].t - q.t) * 1000
            hands.append(dict(t=t0, kind=kind, wanted=wanted, shown=shown, gap_ms=gap_ms,
                              first=meant - step if meant is not None else None))
    return rows, helds, fovoffs, hands


def main(argv):
    check = "--pass" in argv
    paths = [a for a in argv if a != "--pass"]
    rows, helds, fovoffs, hands = analyse(paths)
    rows.sort(key=lambda r: r["t"])
    print(f"{len(rows)} switches with an Unreal server")
    for kind in sorted({r["kind"] for r in rows}):
        k = [r for r in rows if r["kind"] == kind]
        turning = [r for r in k if r["rate"] >= 20]
        back = [r for r in k if r["backwards"] and abs(r["step"]) >= 1]
        print(f"  {kind:<16} {len(k):3d}: turning at {len(turning)}; a backward turn of 1 degree or more at {len(back)}, "
              f"largest {max((abs(r['step']) for r in back), default=0):.1f}; the largest fov change {max((abs(r['dfov']) for r in k), default=0):.2f}")
    for r in rows:
        flag = "  <<" if (r["backwards"] and abs(r["step"]) >= 1) or abs(r["dfov"]) >= 0.5 else ""
        meant = f", the hand meant {r['meant']:+6.2f}" if r["meant"] is not None else ""
        pitch = f", pitch {r['pitch']:+5.2f}" if r["pitch"] is not None else ""
        print(f"{datetime.fromtimestamp(r['t']).strftime('%H:%M:%S.%f')[:-3]} {r['kind']:<16} into {r['room']:<13} turning {r['rate']:5.0f} deg/s;"
              f" first gap frame ({r['frame_ms']:4.1f} ms): turn {r['step']:+7.2f}{meant}{pitch}, fov {r['dfov']:+5.2f}{flag}")
    if hands:
        print(f"\nthe hand's turn through each hand-over (gap start to 300 ms after the pawn took the view), {len(hands)} switches:")
        for h in hands:
            lost = h["wanted"] - h["shown"]
            flag = "  <<" if abs(lost) >= 1 else ""
            first = f", first frame {h['first']:+5.2f}" if h["first"] is not None else ""
            print(f"  {datetime.fromtimestamp(h['t']).strftime('%H:%M:%S.%f')[:-3]} {h['kind']:<16} gap {h['gap_ms']:5.0f} ms: meant {h['wanted']:+7.2f}, shown {h['shown']:+7.2f}, lost {lost:+6.2f}{first}{flag}")
        lost_all = [h["wanted"] - h["shown"] for h in hands]
        print(f"  lost: largest {max(abs(x) for x in lost_all):.2f} degrees, at {sum(1 for x in lost_all if abs(x) >= 1)} of {len(hands)} switches 1 degree or more")
    bad_held = [h for h in helds if h[1] or h[2]]
    print(f"\n{len(helds)} crossing views held: {len(bad_held)} with frames without a view or held from a wrong place")
    print(f"{len(fovoffs)} 'fov off' stretches" + "".join(f"\n  {datetime.fromtimestamp(f[0]).strftime('%H:%M:%S.%f')[:-3]} drawn {f[1]:.1f}, the pawn's {f[2]:.1f}, locked {f[3]:.1f}, in {f[4]}" for f in fovoffs[:20]))
    if not check:
        return 0
    fails = []
    if any(r["backwards"] and abs(r["step"]) >= 1 for r in rows):
        fails.append("a backward turn of 1 degree or more in a gap's first frame")
    if any(abs(r["dfov"]) >= 0.5 for r in rows):
        fails.append("a field of view change of 0.5 degrees or more in a gap's first frame")
    if fovoffs:
        fails.append("a drawn field of view that was not the pawn's")
    if bad_held:
        fails.append("a frame without a view or held from a wrong place")
    print("\nPASS" if not fails else "\nFAIL: " + "; ".join(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
