"""Per place of a warp tour (tour_run.ps1, Start 1200): game FPS and the game thread's, GX worker's and render
worker's CPU per game frame, over retraces warp+450 .. warp+Stop-50 (Link running and turning).

  python survey.py STOP LOG1 PLACES1 [LOG2 PLACES2 ...]
"""
import re
import sys

START = 1200


def place_rows(path, places, stop):
    rows = {p: {"fps": [], "gx": [], "render": [], "busy": []} for p in places}
    current = None  # place index of the last [perf] retrace seen inside a window
    for line in open(path, errors="replace"):
        m = re.match(r"\[perf\] retrace=(\d+) rate=[\d.]+ worst_ms=[\d.]+ hitches=\d+ busy=(\d+)%", line)
        if m:
            r = int(m.group(1))
            i = (r - START) // stop
            off = (r - START) - i * stop
            current = None
            if 0 <= i < len(places) and 450 <= off <= stop - 50:
                current = places[i]
                rows[current]["busy"].append(int(m.group(2)))
            continue
        m = re.match(r"\[fps\] shown=[\d.]+ game=([\d.]+) .*busy: gx=(\d+)% interp=\d+% render=(\d+)%", line)
        if m and current is not None:
            rows[current]["fps"].append(float(m.group(1)))
            rows[current]["gx"].append(int(m.group(2)))
            rows[current]["render"].append(int(m.group(3)))
    return rows


def mean(v):
    return sum(v) / len(v) if v else float("nan")


def main():
    stop = int(sys.argv[1])
    args = sys.argv[2:]
    out = []
    for k in range(0, len(args), 2):
        places = args[k + 1].split(",")
        rows = place_rows(args[k], places, stop)
        for p in places:
            r = rows[p]
            fps = mean(r["fps"])
            if not r["fps"]:
                out.append((p, None))
                continue
            out.append((p, (fps, mean(r["busy"]) * 10 / fps, mean(r["gx"]) * 10 / fps, mean(r["render"]) * 10 / fps,
                            len(r["fps"]))))
    print(f"{'place':16} {'fps':>6} {'game ms':>8} {'gx ms':>7} {'render':>7}  n")
    for p, v in out:
        if v is None:
            print(f"{p:16} (no samples)")
        else:
            print(f"{p:16} {v[0]:6.1f} {v[1]:8.2f} {v[2]:7.2f} {v[3]:7.2f}  {v[4]}")
    good = [v for _, v in out if v]
    print("worst game thread:", ", ".join(f"{p} {v[1]:.1f}" for p, v in sorted(((p, v) for p, v in out if v),
                                                                           key=lambda x: -x[1][1])[:8]))
    print("worst GX worker:  ", ", ".join(f"{p} {v[2]:.1f}" for p, v in sorted(((p, v) for p, v in out if v),
                                                                           key=lambda x: -x[1][2])[:8]))


main()
