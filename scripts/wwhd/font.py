"""Bounded Wii U FFNT v3 bitmap font reader for audited English UI glyphs.

The FINF/TGLP/CWDH/CMAP field layouts are also documented by 3dstools:
https://github.com/ObsidianX/3dstools/blob/master/bffnt.py
Only the two surface formats used by the supplied HD fonts are supported.
No fonts, character images or external font-rendering dependencies are shipped.
"""
from .formats import FormatError, HDTexture, Reader, MAX_FILE
from .vendor import addrlib


class BitmapFont:
    def __init__(self, data: bytes):
        self.reader = r = Reader(data)
        if (r.take(0, 4) != b"FFNT" or r.u16(4) != 0xFEFF or
                r.u16(6) != 20 or r.u32(8) != 0x03000000 or
                r.u32(12) != len(data) or len(data) > MAX_FILE):
            raise FormatError("unsupported Wii U bitmap font")
        if r.take(20, 4) != b"FINF" or r.u32(24) != 32:
            raise FormatError("invalid font info section")
        glyphs, widths, mapping = r.unpack("3I", 40)
        at = glyphs - 8
        (magic, size, self.cell_w, self.cell_h, self.sheet_count, _,
         self.sheet_size, _, fmt, self.cols, self.rows, self.width,
         self.height, self.offset) = r.unpack("4sI4BI6HI", at)
        if (magic != b"TGLP" or not self.cell_w or not self.cell_h or
                not 0 < self.sheet_count <= 16 or not self.cols or not self.rows or
                not 0 < self.width <= 4096 or not 0 < self.height <= 4096 or
                self.cols * (self.cell_w + 1) > self.width or
                self.rows * (self.cell_h + 1) > self.height or fmt not in (12, 14)):
            raise FormatError("invalid font glyph surface")
        r.take(at, size)
        r.take(self.offset, self.sheet_count * self.sheet_size)
        self.surface = 0x34 if fmt == 12 else 0x1A  # BC4 alpha / RGBA8
        self.components = (5, 5, 5, 0) if fmt == 12 else (0, 1, 2, 3)
        self.info = addrlib.getSurfaceInfo(self.surface, self.width, self.height,
                                           1, 1, 4, 0, 0)
        if self.sheet_size != self.info.surfSize:
            raise FormatError("unexpected font sheet stride")
        self.widths, self.mapping, self.sheets = {}, {}, {}
        for at in self._chain(widths, b"CWDH", 12):
            first, last = r.unpack("2H", at + 8)
            if last < first or 16 + (last - first + 1) * 3 > r.u32(at + 4):
                raise FormatError("invalid font width range")
            for i in range(first, last + 1):
                self.widths[i] = r.unpack("b2B", at + 16 + (i - first) * 3)
        for at in self._chain(mapping, b"CMAP", 16):
            first, last, method, _ = r.unpack("4H", at + 8)
            if last < first:
                raise FormatError("invalid font character range")
            if method == 0:
                if r.u32(at + 4) < 22:
                    raise FormatError("truncated direct font mapping")
                start = r.u16(at + 20)
                self.mapping.update({c: start + c - first for c in range(first, last + 1)})
            elif method == 1:
                if 20 + (last - first + 1) * 2 > r.u32(at + 4):
                    raise FormatError("truncated table font mapping")
                for c in range(first, last + 1):
                    i = r.u16(at + 20 + (c - first) * 2)
                    if i != 65535:
                        self.mapping[c] = i
            elif method == 2:
                count = r.u16(at + 20)
                if 22 + count * 4 > r.u32(at + 4):
                    raise FormatError("truncated sparse font mapping")
                for i in range(count):
                    c, glyph = r.unpack("2H", at + 22 + i * 4)
                    self.mapping[c] = glyph
            else:
                raise FormatError("unsupported font mapping method")

    def _chain(self, pointer, magic, link_offset):
        seen = set()
        while pointer:
            at = pointer - 8
            if at in seen or len(seen) >= 256:
                raise FormatError("cyclic font section chain")
            seen.add(at)
            if self.reader.take(at, 4) != magic:
                raise FormatError("invalid font section pointer")
            size = self.reader.u32(at + 4)
            if size < link_offset + 4:
                raise FormatError("invalid font section size")
            self.reader.take(at, size)
            yield at
            pointer = self.reader.u32(at + link_offset)

    def glyph(self, index):
        from PIL import Image
        page, cell = divmod(index, self.cols * self.rows)
        if index < 0 or page >= self.sheet_count:
            raise FormatError("font glyph outside atlas")
        if page not in self.sheets:
            info = self.info
            tiled = self.reader.take(self.offset + page * self.sheet_size, self.sheet_size)
            # GX2 thin 2D array slices rotate the bank selection by two per
            # slice. Ignoring this scrambles later pages of the larger font.
            raw = addrlib.deswizzle(self.width, self.height, info.height,
                                   self.surface, info.tileMode, (2 * page) << 8,
                                   info.pitch, info.bpp, tiled)
            texture = HDTexture("font", "font", self.width, self.height,
                                self.surface, self.components, [raw])
            self.sheets[page] = next(texture.images()).transpose(Image.Transpose.FLIP_TOP_BOTTOM)
        row, col = divmod(cell, self.cols)
        x, y = col * (self.cell_w + 1) + 1, row * (self.cell_h + 1) + 1
        return self.sheets[page].crop((x, y, x + self.cell_w, y + self.cell_h))

    def render(self, text):
        from PIL import Image
        if not text or len(text) > 256:
            raise FormatError("invalid sprite text length")
        try:
            indices = [self.mapping[ord(c)] for c in text]
            metrics = [self.widths[i] for i in indices]
        except KeyError as ex:
            raise FormatError("font lacks requested English glyph") from ex
        width = sum(advance for _, _, advance in metrics) + self.cell_w * 2
        if not 0 < width <= 16384:
            raise FormatError("font sprite size limit exceeded")
        out, x = Image.new("RGBA", (width, self.cell_h)), self.cell_w
        for index, (left, glyph_width, advance) in zip(indices, metrics):
            if glyph_width > self.cell_w:
                raise FormatError("font glyph width exceeds cell")
            glyph = self.glyph(index).crop((0, 0, glyph_width, self.cell_h))
            out.alpha_composite(glyph, (x + left, 0))
            x += advance
        box = out.getchannel("A").getbbox()
        if box is None:
            raise FormatError("empty font sprite")
        return out.crop(box)
