# Reads a client log of an offline run (RunOffline.ps1 -Client ... -ClientExtra "-logcrossing") and says, for every
# border crossing: what happened and when (the handshake while the old server played on, the world handed over),
# whether a map was loaded or the chunks rebuilt, and the frames drawn within 0.6 s of it: the largest step of the view
# between two frames, the largest gap between two frames, any jump of the yaw or the field of view, and what held the
# view (the server's pawn, the client's own, a placeholder controller).
#
#   python Scripts/AnalyzeCrossings.py                          # Saved/Logs/offline-client-Ann.log
#   python Scripts/AnalyzeCrossings.py Saved/Logs/offline-client-Bob.log
import os
import re
import sys
from collections import Counter

here = os.path.dirname(os.path.abspath(__file__))
log = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, '..', 'Saved', 'Logs', 'offline-client-Ann.log')


def seconds(stamp):
    """[2026.10.01-22.43.58:990] -> seconds of the day."""
    hms = stamp.split('-')[1]
    h, m, rest = hms.split('.')
    s, ms = rest.split(':')
    return int(h) * 3600 + int(m) * 60 + int(s) + int(ms) / 1000


events, frames, loadmaps, rebuilds, errors, walk = [], [], [], [], [], []
EVENT = re.compile(r'crossing to|connecting to|crossed into|entered|seamless travel|leaving the Unreal|could not reach|'
                   r'did not let us in|refused|turned away|crossing:|travelling to')
for line in open(log, encoding='utf-8', errors='replace'):
    m = re.match(r'\[(\d{4}\.\d\d\.\d\d-\d\d\.\d\d\.\d\d:\d{3})\]\[\s*\d+\](\w+): (.*)', line.rstrip('\n'))
    if not m:
        continue
    at, category, text = seconds(m.group(1)), m.group(2), m.group(3)
    f = re.search(r'frame (\d+): view ([-\d.]+) ([-\d.]+) ([-\d.]+) yaw ([-\d.]+) fov ([-\d.]+) target (\S+) pc (\S+)', text)
    if f:
        frames.append((at, int(f.group(1)), float(f.group(2)), float(f.group(3)), float(f.group(4)),
                       float(f.group(5)), float(f.group(6)), f.group(7), f.group(8)))
        continue
    if 'LoadMap(' in text:
        loadmaps.append(at)
    r = re.search(r'rebuilt (\d+) chunks in ([\d.]+) ms', text)
    if r:
        rebuilds.append((at, int(r.group(1)), float(r.group(2))))
    w = re.search(r'walkto: at ([-\d.]+) ([-\d.]+) ([-\d.]+) yaw ([-\d.]+) in (\S+)', text)
    if w:
        walk.append((float(w.group(1)), float(w.group(2)), w.group(5)))
    if category == 'LogCubeWorld' and EVENT.search(text):
        events.append((at, text))
    if re.search(r'Error|Ensure|Fatal|Assertion', text) and not re.search(r'UnifiedErrorTest|FError that', text):
        errors.append((at, category, text))

print('events:')
for at, text in events:
    print('  %9.3f %s' % (at, text[:150]))
print('maps loaded at:', ', '.join('%.3f' % at for at in loadmaps))
print('chunks rebuilt:', ', '.join('%.3f (%d in %.1f ms)' % r for r in rebuilds))
print('errors:', len(errors))
for at, category, text in errors[:10]:
    print('  %9.3f %s %s' % (at, category, text[:150]))

for crossed in [at for at, text in events if text.startswith('crossed into')]:
    window = [f for f in frames if crossed - 0.6 <= f[0] <= crossed + 0.6]
    # Only frames logged one after the other: the log comes in windows, and a step across two windows is no frame's.
    pairs = [(a, b) for a, b in zip(window, window[1:]) if b[1] == a[1] + 1]
    if not pairs:
        print('crossing at %.3f: no frames logged (run the client with -logcrossing)' % crossed)
        continue
    step, a, b = max(((((b[2] - a[2]) ** 2 + (b[3] - a[3]) ** 2 + (b[4] - a[4]) ** 2) ** 0.5, a, b) for a, b in pairs), key=lambda s: s[0])
    gap = max(b[0] - a[0] for a, b in pairs)
    yaw = max(abs(b[5] - a[5]) for a, b in pairs)
    fov = max(abs(b[6] - a[6]) for a, b in pairs)
    held = Counter('%s/%s' % (f[7], f[8]) for f in window)
    print('crossing at %.3f: %d frames, largest view step %.3f blocks (frame %d), largest gap %.0f ms, '
          'largest yaw step %.1f, largest fov step %.1f, view held by %s' % (crossed, len(window), step, b[1], gap * 1000, yaw, fov, dict(held)))

if walk:
    print('walk:', ' '.join('%.1f,%.1f@%s' % (x, y, room.split('-')[0]) for x, y, room in walk[:24]))
