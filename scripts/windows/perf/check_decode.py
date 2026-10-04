"""Decode a sample of each GameCube format with make_test_pack's decoder into a contact sheet, to check it."""
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).parent))
from make_test_pack import decode, index_gc  # noqa: E402

iso = Path(sys.argv[1])
textures = index_gc(iso, [])
by_fmt = {}
for t in textures:
    if 32 <= t.width <= 256 and 32 <= t.height <= 256:
        by_fmt.setdefault(t.format, []).append(t)
tiles = []
for fmt in sorted(by_fmt):
    row = []
    for t in by_fmt[fmt][:: max(1, len(by_fmt[fmt]) // 6)][:6]:
        rgba = np.clip(decode(t), 0, 255).astype(np.uint8)
        img = Image.fromarray(rgba, "RGBA")
        bg = Image.new("RGBA", img.size, (255, 0, 255, 255))
        bg.alpha_composite(img)
        row.append(bg.convert("RGB").resize((128, 128), Image.NEAREST))
    print(fmt, len(by_fmt[fmt]), [t.source.split('/')[-1][:40] for t in by_fmt[fmt][:: max(1, len(by_fmt[fmt]) // 6)][:6]])
    tiles.append(row)
sheet = Image.new("RGB", (128 * 6, 128 * len(tiles)), (40, 40, 40))
for y, row in enumerate(tiles):
    for x, im in enumerate(row):
        sheet.paste(im, (x * 128, y * 128))
sheet.save(sys.argv[2])
