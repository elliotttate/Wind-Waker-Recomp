"""Side by side: A, B and where they differ (amplified), for one dump frame; prints the differing area.

  python diff_pair.py A.ppm B.ppm OUT.png [scale]
"""
import sys

import numpy as np
from PIL import Image

a = np.asarray(Image.open(sys.argv[1]).convert("RGB")).astype(np.int16)
b = np.asarray(Image.open(sys.argv[2]).convert("RGB")).astype(np.int16)
d = np.abs(a - b).max(-1)
ys, xs = np.nonzero(d > 8)
print(f"{len(ys)} pixels differ by >8 of {d.size}; mean diff {d.mean():.2f}" +
      (f"; box x {xs.min()}-{xs.max()} y {ys.min()}-{ys.max()}" if len(ys) else ""))
heat = np.zeros_like(a)
heat[..., 0] = np.clip(d * 4, 0, 255)
heat[..., 1] = (a.mean(-1) * 0.3).astype(np.int16)
scale = float(sys.argv[4]) if len(sys.argv) > 4 else 0.5
imgs = [Image.fromarray(x.astype(np.uint8)) for x in (a, b, heat)]
w, h = imgs[0].size
sw, sh = int(w * scale), int(h * scale)
out = Image.new("RGB", (sw * 3, sh))
for i, im in enumerate(imgs):
    out.paste(im.resize((sw, sh), Image.LANCZOS), (i * sw, 0))
out.save(sys.argv[3])
