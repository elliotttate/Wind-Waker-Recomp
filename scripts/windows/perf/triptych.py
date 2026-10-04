"""Rows of dump frames side by side: for each frame, each run's image (optionally a crop, zoomed).

  python triptych.py OUT.png FRAME[,FRAME..] RUN_DIR [RUN_DIR..] [--crop x0,y0,x1,y1] [--zoom Z] [--kind real|between]
"""
import sys

from PIL import Image

args = sys.argv[1:]
crop, zoom, kind = None, 0.5, "real"
if "--crop" in args:
    i = args.index("--crop"); crop = tuple(int(v) for v in args[i + 1].split(",")); del args[i:i + 2]
if "--zoom" in args:
    i = args.index("--zoom"); zoom = float(args[i + 1]); del args[i:i + 2]
if "--kind" in args:
    i = args.index("--kind"); kind = args[i + 1]; del args[i:i + 2]
out, frames, runs = args[0], [int(f) for f in args[1].split(",")], args[2:]
tiles = []
for f in frames:
    row = []
    for r in runs:
        im = Image.open(f"{r}/frame{f:06d}-{kind}.ppm").convert("RGB")
        if crop:
            im = im.crop(crop)
        im = im.resize((int(im.width * zoom), int(im.height * zoom)), Image.NEAREST if zoom >= 1 else Image.LANCZOS)
        row.append(im)
    tiles.append(row)
w, h = tiles[0][0].size
sheet = Image.new("RGB", (w * len(runs) + 4 * (len(runs) - 1), h * len(frames) + 4 * (len(frames) - 1)), (255, 255, 255))
for y, row in enumerate(tiles):
    for x, im in enumerate(row):
        sheet.paste(im, (x * (w + 4), y * (h + 4)))
sheet.save(out)
print(sheet.size)
