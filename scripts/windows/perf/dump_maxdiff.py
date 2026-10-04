"""Two dump folders: real frames identical or not, and each in-between frame's differing pixels and largest
difference.  python dump_maxdiff.py A B"""
import os
import sys

import numpy as np
from PIL import Image

a, b = sys.argv[1], sys.argv[2]
names = sorted(set(os.listdir(a)) & set(os.listdir(b)))
only = sorted(set(os.listdir(a)) ^ set(os.listdir(b)))
real_same = real = 0
worst = []
for n in names:
    x = np.asarray(Image.open(os.path.join(a, n)).convert("RGB")).astype(np.int16)
    y = np.asarray(Image.open(os.path.join(b, n)).convert("RGB")).astype(np.int16)
    if x.shape != y.shape:
        print("size differs", n)
        continue
    d = np.abs(x - y).max(-1)
    if "real" in n:
        real += 1
        real_same += int(d.max() == 0)
        if d.max():
            print(f"  REAL {n}: {int((d > 0).sum())} pixels differ, max {int(d.max())}")
    else:
        worst.append((int((d > 8).sum()), int(d.max()), n))
print(f"  real frames identical {real_same} of {real}; files only in one: {len(only)}")
if worst:
    worst.sort(reverse=True)
    print("  in-between: most pixels >8 " + ", ".join(f"{n.split('-')[0][5:]}:{c}px max{m}" for c, m, n in worst[:4]))
