"""survey.py for Smooth Motion runs: per place, frames shown and game frames a second, and the GX worker's,
interpolation helper's and render worker's CPU per game frame (ms), over retraces warp+450 .. warp+Stop-50.

  python survey_sm.py STOP LOG PLACES
"""
import re
import sys

START = 1200


def main():
    stop = int(sys.argv[1])
    path = sys.argv[2]
    places = sys.argv[3].split(",")
    rows = {p: [] for p in places}
    current = None
    for line in open(path, errors="replace"):
        m = re.match(r"\[perf\] retrace=(\d+) ", line)
        if m:
            r = int(m.group(1))
            i = (r - START) // stop
            off = (r - START) - i * stop
            current = places[i] if 0 <= i < len(places) and 450 <= off <= stop - 50 else None
            continue
        m = re.match(r"\[fps\] shown=([\d.]+) game=([\d.]+) .*busy: gx=(\d+)% interp=(\d+)% render=(\d+)%", line)
        if m and current is not None:
            rows[current].append(tuple(float(g) for g in m.groups()))
    print(f"{'place':14} {'shown':>6} {'game':>6} {'gx ms':>7} {'interp':>7} {'render':>7}  n")
    for p in places:
        v = rows[p]
        if not v:
            print(f"{p:14} (no samples)")
            continue
        n = len(v)
        shown = sum(x[0] for x in v) / n
        game = sum(x[1] for x in v) / n
        ms = [sum(x[k] for x in v) / n * 10 / game for k in (2, 3, 4)]
        print(f"{p:14} {shown:6.1f} {game:6.1f} {ms[0]:7.2f} {ms[1]:7.2f} {ms[2]:7.2f}  {n}")


if __name__ == "__main__":
    main()
