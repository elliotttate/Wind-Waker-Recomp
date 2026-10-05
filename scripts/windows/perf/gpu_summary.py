"""Per place, the [gpu-prof] lines of a run (DOL_AURORA_GPU_PROF=1): GPU busy time,
the staging copies' and the render passes' time per frame (ms), and the KB the
staging copies move per frame, averaged over the lines after each warp but the
first (the load).

  python gpu_summary.py LOG [LOG ...]
"""
import re
import sys

WARP = re.compile(r"\[load\] test warp retrace=\d+ to (\S+) room (\d+) point (\d+)")
LINE = re.compile(r"\[gpu-prof\] frames=\d+ frame_ms=[\d.]+ max=[\d.]+ busy_ms=([\d.]+)"
                  r"(?: copy_kb verts=(\d+) uniforms=(\d+) indices=(\d+) storage=\d+ textures=(\d+))?(.*)")


def kind_ms(rest, name):
    m = re.search(rf"\| {re.escape(name)} ([\d.]+) x", rest)
    return float(m.group(1)) if m else 0.0


for path in sys.argv[1:]:
    place, seen, rows = None, 0, {}
    for text in open(path, errors="replace"):
        w = WARP.search(text)
        if w:
            place, seen = f"{w.group(1)}:{w.group(2)}", 0
            continue
        m = LINE.search(text)
        if not m or place is None:
            continue
        seen += 1
        if seen == 1:
            continue
        r = rows.setdefault(place, [])
        r.append((float(m.group(1)), kind_ms(m.group(6), "Staging copies"), kind_ms(m.group(6), "EFB"),
                  int(m.group(2) or 0), int(m.group(5) or 0)))
    print(path)
    print(f"  {'place':<12} {'busy':>7} {'staging':>8} {'EFB':>7} {'verts KB':>9} {'tex KB':>7}")
    for p, r in rows.items():
        n = len(r)
        avg = [sum(x[i] for x in r) / n for i in range(5)]
        print(f"  {p:<12} {avg[0]:7.3f} {avg[1]:8.3f} {avg[2]:7.3f} {avg[3]:9.0f} {avg[4]:7.0f}")
