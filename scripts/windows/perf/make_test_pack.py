"""A private test HD pack from the GameCube disc: every model/BTI texture decoded, upscaled (4x up to 256,
2x up to 512) and written under Dolphin's name for it (the WWHD importer's GCTexture.filename(), the key the
renderer looks up), with a full mip chain as the WWHD importer writes. Local testing only; never shipped.

  python make_test_pack.py ISO OUT [--no-mips] [--mark]
"""
import sys
from multiprocessing import Pool
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))
from import_wwhd_textures import index_gc  # noqa: E402
from wwhd.formats import BLOCKS  # noqa: E402


def blocks(data, w, h, fmt, bpp_bytes=None):
    """Untile: yields the texel bytes in raster order as a (h, w, n) array for byte formats."""
    bw, bh, bs = BLOCKS[fmt]
    nbx, nby = (w + bw - 1) // bw, (h + bh - 1) // bh
    return bw, bh, bs, nbx, nby


def untile(arr, bw, bh, nbx, nby, w, h):
    # arr: (nby*nbx, bh, bw, ...) -> (h, w, ...)
    a = arr.reshape(nby, nbx, bh, bw, *arr.shape[3:])
    a = a.transpose(0, 2, 1, 3, *range(4, a.ndim)).reshape(nby * bh, nbx * bw, *arr.shape[3:])
    return a[:h, :w]


def c565(v):
    r = ((v >> 11) & 31) * 255 // 31
    g = ((v >> 5) & 63) * 255 // 63
    b = (v & 31) * 255 // 31
    return r, g, b


def rgb5a3(v):
    opaque = (v & 0x8000) != 0
    r = np.where(opaque, ((v >> 10) & 31) * 255 // 31, ((v >> 8) & 15) * 17)
    g = np.where(opaque, ((v >> 5) & 31) * 255 // 31, ((v >> 4) & 15) * 17)
    b = np.where(opaque, (v & 31) * 255 // 31, (v & 15) * 17)
    a = np.where(opaque, 255, ((v >> 12) & 7) * 255 // 7)
    return r, g, b, a


def palette_rgba(pal, pfmt):
    v = np.frombuffer(pal, ">u2").astype(np.int32)
    if pfmt == 0:  # IA8: alpha high, intensity low
        a, i = v >> 8, v & 255
        return np.stack([i, i, i, a], -1)
    if pfmt == 1:
        r, g, b = c565(v)
        return np.stack([r, g, b, np.full_like(r, 255)], -1)
    return np.stack(rgb5a3(v), -1)


def decode(t):
    w, h, fmt, data = t.width, t.height, t.format, t.pixels
    bw, bh, bs, nbx, nby = blocks(data, w, h, fmt)
    n = nbx * nby
    raw = np.frombuffer(data[:n * bs], np.uint8)
    if fmt in (0, 8):  # 4 bpp
        nib = np.stack([raw >> 4, raw & 15], -1).reshape(n, bh, bw)
        if fmt == 0:
            i = untile(nib, bw, bh, nbx, nby, w, h).astype(np.int32) * 17
            return np.stack([i, i, i, i], -1)
        return palette_rgba(t.palette, t.palette_format)[untile(nib, bw, bh, nbx, nby, w, h)]
    if fmt in (1, 9, 2):  # 8 bpp
        b = raw.reshape(n, bh, bw)
        px = untile(b, bw, bh, nbx, nby, w, h).astype(np.int32)
        if fmt == 1:
            return np.stack([px, px, px, px], -1)
        if fmt == 2:
            a, i = (px >> 4) * 17, (px & 15) * 17
            return np.stack([i, i, i, a], -1)
        return palette_rgba(t.palette, t.palette_format)[px]
    if fmt in (3, 4, 5, 10):  # 16 bpp
        v = np.frombuffer(data[:n * bs], ">u2").astype(np.int32).reshape(n, bh, bw)
        px = untile(v, bw, bh, nbx, nby, w, h)
        if fmt == 3:
            a, i = px >> 8, px & 255
            return np.stack([i, i, i, a], -1)
        if fmt == 4:
            r, g, b = c565(px)
            return np.stack([r, g, b, np.full_like(r, 255)], -1)
        if fmt == 5:
            return np.stack(rgb5a3(px), -1)
        return palette_rgba(t.palette, t.palette_format)[px & 0x3FFF]
    if fmt == 6:  # RGBA8: 64-byte blocks, AR then GB
        b = raw.reshape(n, 2, 16, 2)
        a, r = b[:, 0, :, 0], b[:, 0, :, 1]
        g, bl = b[:, 1, :, 0], b[:, 1, :, 1]
        px = np.stack([r, g, bl, a], -1).reshape(n, 4, 4, 4)
        return untile(px, 4, 4, nbx, nby, w, h).astype(np.int32)
    if fmt == 14:  # CMPR: 8x8 of four 4x4 DXT1 blocks, big-endian colours
        sub = raw.reshape(n, 4, 8)
        c0 = (sub[:, :, 0].astype(np.int32) << 8) | sub[:, :, 1]
        c1 = (sub[:, :, 2].astype(np.int32) << 8) | sub[:, :, 3]
        p0, p1 = np.stack(c565(c0), -1), np.stack(c565(c1), -1)
        four = c0 > c1
        p2 = np.where(four[..., None], (2 * p0 + p1) // 3, (p0 + p1) // 2)
        p3 = np.where(four[..., None], (p0 + 2 * p1) // 3, 0)
        opaque = np.full(c0.shape + (1,), 255)
        pal = np.stack([np.concatenate([p0, opaque], -1), np.concatenate([p1, opaque], -1),
                        np.concatenate([p2, opaque], -1),
                        np.concatenate([p3, np.where(four, 255, 0)[..., None]], -1)], 2)  # (n, sub, entry, rgba)
        bits = sub[:, :, 4:8].astype(np.int32)
        idx = np.stack([(bits >> s) & 3 for s in (6, 4, 2, 0)], -1)  # (n, sub, row, col)
        texel = pal[np.arange(n)[:, None, None, None], np.arange(4)[None, :, None, None], idx]  # (n,sub,row,col,4)
        tile = texel.reshape(n, 2, 2, 4, 4, 4).transpose(0, 1, 3, 2, 4, 5).reshape(n, 8, 8, 4)
        return untile(tile, 8, 8, nbx, nby, w, h).astype(np.int32)
    raise ValueError(f"format {fmt}")


def write(job):
    out, name, rgba, mips, mark = job
    w, h = rgba.shape[1], rgba.shape[0]
    scale = 4 if max(w, h) <= 256 else 2 if max(w, h) <= 512 else 1
    img = Image.fromarray(np.clip(rgba, 0, 255).astype(np.uint8), "RGBA")
    if scale > 1:
        img = img.resize((w * scale, h * scale), Image.LANCZOS)
    if mark:
        # A fine grid at the HD resolution: shows a replacement, and is detail that aliases without mips.
        a = np.array(img)
        a[::8, :, :3] = a[::8, :, :3] // 2
        a[:, ::8, :3] = a[:, ::8, :3] // 2
        img = Image.fromarray(a, "RGBA")
    img.save(out / name, compress_level=1)
    if mips:
        level, cur = 1, img
        while cur.width > 1 or cur.height > 1:
            cur = cur.resize((max(cur.width // 2, 1), max(cur.height // 2, 1)), Image.BOX)
            cur.save(out / name.replace(".png", f"_mip{level}.png"), compress_level=1)
            level += 1
    return name


def main():
    iso, out = Path(sys.argv[1]), Path(sys.argv[2])
    mips, mark = "--no-mips" not in sys.argv, "--mark" in sys.argv
    out.mkdir(parents=True, exist_ok=True)
    errors = []
    textures = index_gc(iso, errors)
    seen, jobs, failed = set(), [], 0
    for t in textures:
        try:
            name = t.filename()
        except Exception:
            failed += 1
            continue
        if name in seen:
            continue
        seen.add(name)
        try:
            jobs.append((out, name, decode(t), mips, mark))
        except Exception as ex:
            failed += 1
            if failed < 5:
                print("decode failed", t.source, t.format, ex)
    print(f"{len(textures)} textures, {len(jobs)} unique, {failed} failed, {len(errors)} index errors")
    with Pool(int(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[3].isdigit() else 4) as pool:
        for i, _ in enumerate(pool.imap_unordered(write, jobs, chunksize=8)):
            if i % 1000 == 0:
                print("written", i, flush=True)
    print("done", out)


if __name__ == "__main__":
    main()
