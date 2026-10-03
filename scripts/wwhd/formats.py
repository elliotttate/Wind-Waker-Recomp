"""Bounded readers for the two games' texture containers.

The BFRES v3/v4 FTEX layout follows AboodXD's GPL-3.0-or-later BFRES-Tool
(https://github.com/aboood40091/BFRES-Tool, pinned in vendor/SOURCE.json).
Surface addressing uses its unmodified Python addrlib. No keys or game data
are included. Readers never unpack archive members onto the filesystem.
"""
from __future__ import annotations

from dataclasses import dataclass
from io import BytesIO
from pathlib import PurePosixPath
import struct
import sys
from pathlib import Path

_native_yaz0 = None
_native_path = Path(sys.prefix) / "libwwhd_yaz0.so"
if _native_path.is_file():
    import ctypes
    _native_yaz0 = ctypes.CDLL(str(_native_path)).wwhd_yaz0_decode
    _native_yaz0.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
    _native_yaz0.restype = ctypes.c_int

from .vendor import addrlib

MAX_FILE = 256 * 1024 * 1024
BLOCKS = {0: (8, 8, 32), 1: (8, 4, 32), 2: (8, 4, 32),
          3: (4, 4, 32), 4: (4, 4, 32), 5: (4, 4, 32),
          6: (4, 4, 64), 8: (8, 8, 32), 9: (8, 4, 32),
          10: (4, 4, 32), 14: (8, 8, 32)}


class FormatError(ValueError):
    pass


class Reader:
    def __init__(self, data: bytes, endian: str = ">"):
        self.data, self.endian = data, endian

    def take(self, at: int, size: int) -> bytes:
        if at < 0 or size < 0 or at + size > len(self.data):
            raise FormatError(f"truncated data at 0x{at:x} ({size} bytes)")
        return self.data[at:at + size]

    def unpack(self, fmt: str, at: int):
        fmt = self.endian + fmt
        return struct.unpack(fmt, self.take(at, struct.calcsize(fmt)))

    def u16(self, at: int) -> int:
        return self.unpack("H", at)[0]

    def u32(self, at: int) -> int:
        return self.unpack("I", at)[0]

    def relative(self, at: int) -> int:
        value = self.unpack("i", at)[0]
        return at + value if value else 0

    def string(self, at: int) -> str:
        self.take(at, 1)
        end = self.data.find(b"\0", at, min(at + 4096, len(self.data)))
        if end < 0:
            raise FormatError("unterminated string")
        return self.data[at:end].decode("shift_jis")


def safe_name(name: str) -> str:
    p = PurePosixPath(name)
    if p.is_absolute() or ".." in p.parts or "\\" in name or "\0" in name:
        raise FormatError("unsafe archive member name")
    return name


def yaz0(data: bytes) -> bytes:
    if data[:4] != b"Yaz0":
        return data
    r = Reader(data)
    size = r.u32(4)
    if size > MAX_FILE:
        raise FormatError("Yaz0 decompression limit exceeded")
    if _native_yaz0 is not None:
        output = ctypes.create_string_buffer(size)
        if _native_yaz0(data, len(data), output, size):
            raise FormatError("invalid or truncated Yaz0 stream")
        return output.raw
    out = bytearray()
    src, bits, code = 16, 0, 0
    while len(out) < size:
        if not bits:
            code = r.take(src, 1)[0]
            src += 1
            bits = 8
        if code & 0x80:
            out.extend(r.take(src, 1))
            src += 1
        else:
            a, b = r.take(src, 2)
            src += 2
            distance, count = ((a & 15) << 8) + b + 1, a >> 4
            if not count:
                count = r.take(src, 1)[0] + 18
                src += 1
            else:
                count += 2
            if distance > len(out) or len(out) + count > size:
                raise FormatError("invalid Yaz0 back reference")
            # Repeated back references overlap. Repeat the available seed
            # instead of copying each byte through Python.
            seed = out[len(out) - distance:len(out) - distance + min(distance, count)]
            out.extend((seed * ((count + len(seed) - 1) // len(seed)))[:count])
        bits -= 1
        code = (code << 1) & 255
    return bytes(out)


def sarc(data: bytes):
    r = Reader(data)
    if r.take(6, 2) == b"\xff\xfe":
        r.endian = "<"
    elif r.take(6, 2) != b"\xfe\xff":
        raise FormatError("invalid SARC byte order")
    header = r.u16(4)
    total, payload = r.u32(8), r.u32(12)
    if total != len(data) or payload > total:
        raise FormatError("invalid SARC size")
    if r.take(header, 4) != b"SFAT":
        raise FormatError("missing SFAT")
    count = r.u16(header + 6)
    nodes = header + r.u16(header + 4)
    sfnt = nodes + 16 * count
    if r.take(sfnt, 4) != b"SFNT":
        raise FormatError("missing SFNT")
    strings = sfnt + r.u16(sfnt + 4)
    for i in range(count):
        _, attr, start, end = r.unpack("4I", nodes + i * 16)
        if not attr >> 24:
            continue
        name = safe_name(r.string(strings + (attr & 0xFFFFFF) * 4))
        yield name, r.take(payload + start, end - start)


def rarc(data: bytes):
    r = Reader(data)
    base = r.u32(8)
    if base != 32 or r.u32(4) != len(data):
        raise FormatError("invalid RARC header")
    payload = base + r.u32(12)
    count, nodes = r.u32(base), base + r.u32(base + 4)
    entries = base + r.u32(base + 12)
    strings = base + r.u32(base + 20)

    def walk(index, prefix, ancestors):
        if index >= count or index in ancestors or len(ancestors) > 32:
            raise FormatError("invalid RARC directory graph")
        node = nodes + index * 16
        n, first = r.u16(node + 10), r.u32(node + 12)
        for i in range(first, first + n):
            e = entries + i * 20
            attr_name, offset, size = r.unpack("3I", e + 4)
            name = r.string(strings + (attr_name & 0xFFFFFF))
            if name in (".", ".."):
                continue
            name = safe_name(prefix + name)
            if attr_name >> 24 & 2:
                yield from walk(offset, name + "/", ancestors | {index})
            elif attr_name >> 24 & 1:
                yield name, r.take(payload + offset, size)
    yield from walk(0, "", set())


def leaves(data: bytes, path: str, depth: int = 0):
    if depth > 8 or len(data) > MAX_FILE:
        raise FormatError("archive nesting or size limit exceeded")
    data = yaz0(data)
    if data[:4] in (b"SARC", b"RARC"):
        read = sarc if data[:4] == b"SARC" else rarc
        for name, member in read(data):
            yield from leaves(member, path + "/" + name, depth + 1)
    else:
        yield path, data


@dataclass
class GCTexture:
    source: str
    name: str
    width: int
    height: int
    format: int
    pixels: bytes
    palette: bytes
    palette_format: int
    mipmaps: int

    def filename(self) -> str:
        import xxhash
        parts = [f"tex1_{self.width}x{self.height}", xxhash.xxh64_hexdigest(self.pixels)]
        if self.format in (8, 9, 10):
            if self.format == 8:
                indices = [v for b in self.pixels for v in (b >> 4, b & 15)]
            elif self.format == 9:
                indices = self.pixels
            else:
                indices = [v & 0x3FFF for (v,) in struct.iter_unpack(">H", self.pixels)]
            lo, hi = min(indices), max(indices)
            if 2 * (hi + 1) > len(self.palette):
                raise FormatError("texture references missing palette entries")
            parts.append(xxhash.xxh64_hexdigest(self.palette[lo * 2:(hi + 1) * 2]))
        parts.append(str(self.format))
        return "_".join(parts) + ".png"


def bti(data: bytes, at: int, source: str, name: str) -> GCTexture:
    r = Reader(data)
    h = r.take(at, 32)
    fmt, palette_fmt = h[0], h[9]
    if fmt not in BLOCKS:
        raise FormatError(f"unsupported GameCube texture format {fmt}")
    w, height = r.u16(at + 2), r.u16(at + 4)
    if not 0 < w <= 4096 or not 0 < height <= 4096:
        raise FormatError("invalid BTI dimensions")
    bw, bh, bs = BLOCKS[fmt]
    size = ((w + bw - 1) // bw) * ((height + bh - 1) // bh) * bs
    pixels = r.take(at + r.u32(at + 28), size)
    palette = (r.take(at + r.u32(at + 12), r.u16(at + 10) * 2)
               if fmt in (8, 9, 10) else b"")
    return GCTexture(source, name, w, height, fmt, pixels, palette, palette_fmt, max(h[24], 1))


def gc_textures(data: bytes, path: str):
    if path.lower().endswith(".bti"):
        yield bti(data, 0, path, PurePosixPath(path).stem)
    elif data[:4] in (b"J3D1", b"J3D2") and data[4:7] in (b"bmd", b"bdl", b"bmt"):
        r = Reader(data)
        at, count = 32, r.u32(12)
        for _ in range(count):
            magic, size = r.take(at, 4), r.u32(at + 4)
            if size < 8:
                raise FormatError("invalid J3D chunk size")
            chunk = r.take(at, size)
            if magic == b"TEX1":
                c = Reader(chunk)
                n, headers, strings = c.u16(8), c.u32(12), c.u32(16)
                if c.u16(strings) != n:
                    raise FormatError("J3D texture name count differs")
                for i in range(n):
                    name = c.string(strings + c.u16(strings + 6 + i * 4))
                    yield bti(chunk, headers + i * 32, path + "#" + name, name)
            at += size


@dataclass
class HDTexture:
    source: str
    name: str
    width: int
    height: int
    format: int
    components: tuple[int, ...]
    levels: list[bytes]

    def images(self):
        from PIL import Image
        base_format = self.format & 0xFF
        for level, raw in enumerate(self.levels):
            w, h = max(1, self.width >> level), max(1, self.height >> level)
            if base_format in (0x31, 0x32, 0x33, 0x34, 0x35):
                if self.format & 0x200:
                    raise FormatError("signed BC surfaces are not replacement art")
                fourcc = {0x31: b"DXT1", 0x32: b"DXT3", 0x33: b"DXT5", 0x34: b"BC4U", 0x35: b"BC5U"}[base_format]
                # DDS_HEADER + DDS_PIXELFORMAT, with a single mip per decode.
                header = struct.pack("<7I", 124, 0x81007, h, w, len(raw), 0, 1)
                header += bytes(44) + struct.pack("<II4s5I", 32, 4, fourcc, 0, 0, 0, 0, 0)
                header += struct.pack("<5I", 0x1000, 0, 0, 0, 0)
                image = Image.open(BytesIO(b"DDS " + header + raw)).convert("RGBA")
            elif base_format == 0x1A:
                image = Image.frombytes("RGBA", (w, h), raw)
            elif base_format in (1, 7):
                mode = "L" if base_format == 1 else "LA"
                channels = Image.frombytes(mode, (w, h), raw).split()
                zero, one = Image.new("L", (w, h), 0), Image.new("L", (w, h), 255)
                # GX2 R8/RG8 stores R and G, not luminance/alpha. compSel does
                # the replication, alpha selection and constant channels.
                image = Image.merge("RGBA", (channels[0], channels[1] if len(channels) > 1 else zero, zero, one))
            elif base_format in (2, 8, 10, 11, 25):
                masks = {2: (4, 4, 0, 0), 8: (5, 6, 5, 0),
                         10: (5, 5, 5, 1), 11: (4, 4, 4, 4), 25: (10, 10, 10, 2)}[base_format]
                stride = sum(masks) // 8
                rgba = bytearray()
                for at in range(0, len(raw), stride):
                    value = int.from_bytes(raw[at:at + stride], "little")
                    for ch, bits in enumerate(masks):
                        rgba.append((value & ((1 << bits) - 1)) * 255 // ((1 << bits) - 1) if bits else (255 if ch == 3 else 0))
                        value >>= bits
                image = Image.frombytes("RGBA", (w, h), bytes(rgba))
            else:
                raise FormatError(f"unsupported Wii U pixel format 0x{self.format:x}")
            channels = list(image.split()) + [Image.new("L", (w, h), 0), Image.new("L", (w, h), 255)]
            if any(c > 5 for c in self.components):
                raise FormatError("unsupported GX2 component selection")
            yield Image.merge("RGBA", tuple(channels[c] for c in self.components))


def for_gc_format(images, fmt: int, layout_mask: bool = False):
    # GameCube I4/I8 sampling returns intensity in all four channels,
    # including alpha. Wii U materials may instead select constant opaque
    # alpha for that same artwork. Preserve the GameCube sampling contract
    # so shared shadow/HUD masks do not become opaque rectangles.
    if fmt in (0, 1):
        from PIL import Image
        from PIL import ImageChops
        intensity = [ImageChops.multiply(im.getchannel("R"), im.getchannel("A"))
                     if layout_mask else im.getchannel("R") for im in images]
        return [Image.merge("RGBA", (channel,) * 4) for channel in intensity]
    return images


def bflim(data: bytes, path: str) -> HDTexture:
    """Read Wii U layout artwork using BFLIM-Tool's footer/format layout."""
    r = Reader(data)
    footer = len(data) - 40
    if r.take(footer, 4) != b"FLIM" or r.take(footer + 4, 2) != b"\xfe\xff":
        raise FormatError("invalid Wii U BFLIM footer")
    if r.u16(footer + 6) != 20 or r.u32(footer + 12) != len(data):
        raise FormatError("invalid BFLIM file size")
    magic, size, w, h, alignment, fmt, packed, image_size = r.unpack("4sI3H2BI", footer + 20)
    formats = {0: (1, (0, 0, 0, 5)), 1: (1, (5, 5, 5, 0)),
               3: (7, (0, 0, 0, 1)), 5: (8, (0, 1, 2, 5)),
               9: (0x1A, (0, 1, 2, 3)), 20: (0x1A, (0, 1, 2, 3)),
               12: (0x31, (0, 1, 2, 3)), 21: (0x431, (0, 1, 2, 3)),
               13: (0x32, (0, 1, 2, 3)), 22: (0x432, (0, 1, 2, 3)),
               14: (0x33, (0, 1, 2, 3)), 23: (0x433, (0, 1, 2, 3)),
               15: (0x34, (0, 0, 0, 5)), 16: (0x34, (5, 5, 5, 0)),
               17: (0x35, (0, 0, 0, 1))}
    if magic != b"imag" or size != 16 or fmt not in formats:
        raise FormatError(f"unsupported BFLIM format {fmt}")
    if not 0 < w <= 8192 or not 0 < h <= 8192 or image_size > footer:
        raise FormatError("invalid BFLIM surface bounds")
    tile, swizzle = packed & 31, (packed >> 5) << 8
    surface, components = formats[fmt]
    info = addrlib.getSurfaceInfo(surface, w, h, 1, 1, tile, 0, 0)
    tiled = r.take(0, image_size)
    if image_size < info.surfSize:
        raise FormatError("truncated BFLIM surface")
    raw = addrlib.deswizzle(w, h, info.height, surface, info.tileMode,
                           swizzle, info.pitch, info.bpp, tiled)
    block = 4 if (surface & 0xFF) in (0x31, 0x32, 0x33, 0x34, 0x35) else 1
    size = ((w + block - 1) // block) * ((h + block - 1) // block) * info.bpp // 8
    return HDTexture(path, PurePosixPath(path).stem, w, h, surface,
                     components, [Reader(raw).take(0, size)])


def bfres(data: bytes, path: str, names: set[str] | None = None):
    r = Reader(data)
    if r.take(4, 1)[0] not in (3, 4) or r.take(8, 2) != b"\xfe\xff":
        raise FormatError("only Wii U BFRES v3/v4 is supported")
    group = r.relative(0x24)
    if not group:
        return
    count, entries = r.u32(group + 4), group + 24
    # ResDict starts at relative_pointer + 4. The dictionary header is eight
    # bytes followed by a 16-byte root node (BFRES-Tool read(), group.pos).
    for i in range(count):
        e = entries + i * 16
        name, tex = r.string(r.relative(e + 8)), r.relative(e + 12)
        if names is not None and name.lower() not in names:
            continue
        if r.take(tex, 4) != b"FTEX":
            raise FormatError("missing FTEX")
        (dim, w, h, depth, mips, fmt, aa, use, image_size, _, mip_size,
         _, tile, swizzle, alignment, pitch) = r.unpack("16I", tex + 4)
        if dim != 1 or depth != 1 or aa or not 0 < w <= 8192 or not 0 < h <= 8192:
            raise FormatError("only 2D, single-sample FTEX surfaces are supported")
        if not 1 <= mips <= 14:
            raise FormatError("invalid FTEX mip count")
        # BFRES v4 stores one level in the FTEX payload (upstream behavior).
        if data[4] == 4:
            mips = 1
        components = tuple(r.take(tex + 0x88, 4))
        image = r.take(r.relative(tex + 0xB0), image_size)
        mip_data = r.take(r.relative(tex + 0xB4), mip_size) if mip_size else b""
        offsets = r.unpack("13I", tex + 0x44)
        levels = []
        base_info = addrlib.getSurfaceInfo(fmt, w, h, depth, dim, tile, aa, 0)
        bw = 4 if (fmt & 0xFF) in (0x31, 0x32, 0x33, 0x34, 0x35) else 1
        bpp = addrlib.surfaceGetBitsPerPixel(fmt)
        if not bpp:
            raise FormatError("unknown GX2 surface format")
        for level in range(mips):
            info = addrlib.getSurfaceInfo(fmt, w, h, depth, dim, tile, aa, level)
            if level:
                start = offsets[level - 1] - (base_info.surfSize if level == 1 else 0)
                tiled = Reader(mip_data).take(start, info.surfSize)
            else:
                tiled = Reader(image).take(0, info.surfSize)
            lw, lh = max(1, w >> level), max(1, h >> level)
            size = ((lw + bw - 1) // bw) * ((lh + bw - 1) // bw) * bpp // 8
            raw = addrlib.deswizzle(lw, lh, info.height, fmt, info.tileMode,
                                   swizzle, info.pitch, info.bpp, tiled)
            levels.append(Reader(raw).take(0, size))
        yield HDTexture(path + "#" + name, name, w, h, fmt, components, levels)
