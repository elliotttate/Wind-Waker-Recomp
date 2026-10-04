"""survey.py plus the draw-done drain: per place, game FPS, game thread and GX worker CPU per game frame (ms), and
the drain per frame (ms), over retraces warp+450 .. warp+Stop-50.

  python survey_drain.py STOP LOG PLACES
"""
import re
import sys

START = 1200
stop, path, places = int(sys.argv[1]), sys.argv[2], sys.argv[3].split(",")
rows = {p: {"busy": [], "fps": []} for p in places}
cur = None
for line in open(path, errors="replace"):
    m = re.match(r"\[perf\] retrace=(\d+) rate=[\d.]+ worst_ms=[\d.]+ hitches=\d+ busy=(\d+)%", line)
    if m:
        r = int(m.group(1)); i = (r - START) // stop; off = (r - START) - i * stop
        cur = places[i] if 0 <= i < len(places) and 450 <= off <= stop - 50 else None
        if cur:
            rows[cur]["busy"].append(int(m.group(2)))
        continue
    m = re.match(r"\[fps\] shown=[\d.]+ game=([\d.]+) drain_ms=(\d+) .*busy: gx=(\d+)%", line)
    if m and cur:
        rows[cur]["fps"].append((float(m.group(1)), int(m.group(2)), int(m.group(3))))
print(f"{'place':12} {'fps':>6} {'game ms':>8} {'gx ms':>7} {'drain':>7}")
for p in places:
    f = rows[p]["fps"]; b = rows[p]["busy"]
    if not f:
        print(f"{p:12} (no samples)"); continue
    fps = sum(x[0] for x in f) / len(f)
    print(f"{p:12} {fps:6.1f} {sum(b)/len(b)*10/fps:8.2f} {sum(x[2] for x in f)/len(f)*10/fps:7.2f} {sum(x[1] for x in f)/len(f)/fps:7.2f}")
