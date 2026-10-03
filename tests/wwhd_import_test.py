"""Generated fixtures only: format decoding, hash identity and safe imports."""
import argparse
import hashlib
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from import_wwhd_textures import build, compatible, install, resource, source_textures
from wwhd import formats
from wwhd.disc import DiscImage, WiiUDisc, read_key
from wwhd.formats import FormatError, GCTexture, HDTexture, bfres
from wwhd.vendor import addrlib
from wwhd.fallback import add_fallback, texture_file
from wwhd.title import title_image
from wwhd.layout import heart_image, inventory_names
from wwhd.font import BitmapFont
from wwhd.ui import alpha_bounds, fit_sprite, opening_sprite, font_sprites, OPENING_PANELS


def fake_font():
    """Two generated array pages with separate glyphs and a negative bearing."""
    from PIL import Image, ImageDraw
    info = addrlib.getSurfaceInfo(0x1A, 32, 32, 1, 1, 4, 0, 0)
    offset, stride = 256, info.surfSize
    cwdh = offset + stride * 2
    cmap = cwdh + 68
    data = bytearray(cmap + 32)
    struct.pack_into(">4sHHIIHH", data, 0, b"FFNT", 0xFEFF, 20,
                     0x03000000, len(data), 4, 0)
    data[20:24] = b"FINF"
    struct.pack_into(">I", data, 24, 32)
    struct.pack_into(">3I", data, 40, 60, cwdh + 8, cmap + 8)
    struct.pack_into(">4sI4BI6HI", data, 52, b"TGLP", cwdh - 52,
                     7, 7, 2, 7, stride, 6, 14, 4, 4, 32, 32, offset)
    for page, color in enumerate(((240, 10, 20, 255), (20, 230, 30, 255))):
        image = Image.new("RGBA", (32, 32))
        ImageDraw.Draw(image).rectangle((1, 1, 7, 7), fill=color)
        raw = image.transpose(Image.Transpose.FLIP_TOP_BOTTOM).tobytes()
        tiled = addrlib.swizzle(32, 32, info.height, 0x1A, info.tileMode,
                               (2 * page) << 8, info.pitch, info.bpp, raw)
        data[offset + page * stride:offset + (page + 1) * stride] = tiled
    struct.pack_into(">4sI2HI", data, cwdh, b"CWDH", 68, 0, 16, 0)
    for i in range(17):
        struct.pack_into(">b2B", data, cwdh + 16 + i * 3, -2, 7, 6)
    struct.pack_into(">4sI4HIH4H", data, cmap, b"CMAP", 32,
                     65, 66, 2, 0, 0, 2, 65, 0, 66, 16)
    return bytes(data), cwdh, cmap


def fake_bfres(name="A", color=(255, 0, 0, 255)):
    data = bytearray(0x2000)
    data[:10] = b"FRES\x03\0\0\0\xfe\xff"
    struct.pack_into(">i", data, 0x24, 0x100 - 0x24)
    struct.pack_into(">II", data, 0x100, 40, 1)
    struct.pack_into(">ii", data, 0x120, 0x200 - 0x120, 0x240 - 0x124)
    data[0x200:0x201 + len(name)] = name.encode() + b"\0"
    data[0x240:0x244] = b"FTEX"
    info = addrlib.getSurfaceInfo(0x1A, 32, 32, 1, 1, 1, 0, 0)
    struct.pack_into(">16I", data, 0x244, 1, 32, 32, 1, 1, 0x1A, 0, 1,
                     info.surfSize, 0, 0, 0, 1, 0, info.baseAlign, info.pitch)
    data[0x2C8:0x2CC] = bytes((0, 1, 2, 3))
    struct.pack_into(">i", data, 0x2F0, 0x800 - 0x2F0)
    raw = bytes(color) * (info.surfSize // 4)
    data[0x800:0x800 + len(raw)] = raw
    return bytes(data)


def fake_iso(path, names):
    data = bytearray(8192)
    data[:8] = b"GZLE01\0\0"
    struct.pack_into(">I", data, 28, 0xC2339F3D)
    strings = b"".join(n.encode() + b".bti\0" for n in names)
    fst = bytearray((len(names) + 1) * 12) + strings
    struct.pack_into(">III", fst, 0, 0x01000000, 0, len(names) + 1)
    name_at = 0
    for i, name in enumerate(names):
        offset = 0x800 + i * 512
        struct.pack_into(">III", fst, (i + 1) * 12, name_at, offset, 288)
        name_at += len(name) + 5
        data[offset] = 1  # I8, 16x16
        struct.pack_into(">HH", data, offset + 2, 16, 16)
        data[offset + 24] = 1
        struct.pack_into(">I", data, offset + 28, 32)
        data[offset + 32:offset + 288] = bytes([42]) * 256
    struct.pack_into(">II", data, 0x424, 0x500, len(fst))
    data[0x500:0x500 + len(fst)] = fst
    path.write_bytes(data)


class TextureTests(unittest.TestCase):
    def test_font_array_pages_and_negative_bearings(self):
        data, _, _ = fake_font()
        font = BitmapFont(data)
        self.assertEqual(font.glyph(font.mapping[65]).getpixel((3, 3)), (240, 10, 20, 255))
        self.assertEqual(font.glyph(font.mapping[66]).getpixel((3, 3)), (20, 230, 30, 255))
        image = font.render("AB")
        self.assertEqual(image.size, (13, 7))
        self.assertEqual(image.getpixel((0, 3)), (240, 10, 20, 255))
        self.assertEqual(image.getpixel((12, 3)), (20, 230, 30, 255))
        with self.assertRaises(FormatError):
            font.render("C")
        with self.assertRaises(FormatError):
            font.glyph(32)

    def test_font_rejects_cycles_truncated_tables_and_surfaces(self):
        data, cwdh, cmap = fake_font()
        with self.assertRaises(FormatError):
            BitmapFont(data[:-1])
        corrupt = bytearray(data)
        struct.pack_into(">I", corrupt, cwdh + 12, cwdh + 8)
        with self.assertRaises(FormatError):
            BitmapFont(bytes(corrupt))
        corrupt = bytearray(data)
        struct.pack_into(">H", corrupt, cmap + 20, 8)
        with self.assertRaises(FormatError):
            BitmapFont(bytes(corrupt))

    def test_sprite_adaptation_preserves_palette_mask_padding(self):
        from PIL import Image
        pixels = bytearray(128)
        for y in range(2, 12):
            for x in range(5, 10):
                block = (y // 8 * 2 + x // 8) * 32
                pos = y % 8 * 8 + x % 8
                pixels[block + pos // 2] |= 1 << (4 if pos % 2 == 0 else 0)
        gc = GCTexture("x", "x", 16, 16, 8, bytes(pixels), b"\0\0\xff\xff", 0, 1)
        self.assertEqual(alpha_bounds(gc), (5, 2, 10, 12))
        image = Image.new("RGBA", (20, 20))
        for y in range(4, 16):
            for x in range(2, 18):
                image.putpixel((x, y), (210, 80, 0, 255))
        fitted = fit_sprite(image, gc)
        self.assertEqual(fitted.size, (64, 64))
        self.assertEqual(fitted.getchannel("A").getbbox(), (20, 8, 40, 48))
        self.assertEqual(fitted.getpixel((30, 30)), (210, 80, 0, 255))

    def test_font_bindings_preserve_words_and_controller_semantics(self):
        msg = {(res, name): value for res, name, value in font_sprites("CKingMsg")}
        self.assertEqual(msg[(("object", "acticon"), "ba_hanasu")], "Let Go")
        self.assertEqual(msg[(("object", "acticon"), "ba_syaberu")], "Speak")
        self.assertEqual(msg[(("object", "menures"), "count_num_9")], "9")
        pic = {name: code for _, name, code in font_sprites("CKingPic")}
        self.assertEqual(pic, {"font_00": 0xE000, "font_01": 0xE001,
                               "font_02": 0xE002, "font_03": 0xE003})
        self.assertEqual(list(font_sprites("Unknown")), [])
        data, _, _ = fake_font()
        self.assertEqual(list(source_textures(data, "pack/CKingMsg.bffnt", {})), [])
        self.assertEqual(list(source_textures(data, "permanent_2d_UsFrench.pack/CKingMsg.bffnt", {})), [])

    def test_opening_panorama_crops_do_not_repeat_first_panel(self):
        from PIL import Image
        image = Image.new("RGBA", (2300, 500), (200, 40, 10, 255))
        for y in range(500):
            for x in range(1195, 2221):
                image.putpixel((x, y), (10, 40, 220, 255))
        gc = GCTexture("x", "x", 492, 308, 5, b"", b"", 0, 1)
        panels = OPENING_PANELS["OpeningPic_03^o.bflim"]
        self.assertEqual(panels[0][1][2], panels[1][1][0])
        result = opening_sprite(image, panels[1][1], gc)
        self.assertEqual(result.size, (984, 616))
        self.assertEqual(result.getpixel((492, 308)), (10, 40, 220, 255))
        with self.assertRaises(FormatError):
            opening_sprite(image, (1195, 0, 2400, 500), gc)

    def test_title_layout_keeps_alpha_and_hd_badge(self):
        from PIL import Image
        word = Image.new("RGBA", (34, 6), (220, 170, 0, 128))
        badge = Image.new("RGBA", (17, 5), (0, 30, 240, 200))
        image = title_image((256, 64), word, badge)
        self.assertEqual(image.size, (256, 64))
        self.assertEqual(image.getpixel((0, 0)), (0, 0, 0, 0))
        self.assertEqual(image.getpixel((128, 10))[3], 128)
        self.assertEqual(image.getpixel((128, 55))[3], 200)
        self.assertGreater(image.getpixel((128, 55))[2], 230)

    def test_heart_layers_keep_original_sprite_padding(self):
        from PIL import Image
        base = Image.new("RGBA", (60, 50), (70, 70, 70, 255))
        fill = Image.new("RGBA", (60, 50), (250, 10, 10, 128))
        result = heart_image(base, fill)
        self.assertEqual(result.size, (96, 96))
        self.assertEqual(result.getchannel("A").getbbox(), (4, 12, 92, 80))
        self.assertEqual(result.getpixel((0, 0)), (0, 0, 0, 0))
        self.assertGreater(result.getpixel((48, 40))[0], result.getpixel((48, 40))[1])
        self.assertEqual(inventory_names("pack/BtnMapIcon_01.szs/timg/MapBtn_02^l.bflim"), ("cmap_tingle2",))
        self.assertIsNone(inventory_names("pack/Other.szs/timg/MapBtn_02^l.bflim"))
        self.assertIsNone(inventory_names("pack/BtnCollectIcon_00.szs/timg/CollectIcon118_16^l.bflim"))

    def test_bflim_rgba_and_camera_scope(self):
        # Generated linear RGBA8 with a Wii U footer, including transparent
        # pixels. The same numbered icon in another layout must not match.
        info = addrlib.getSurfaceInfo(0x1A, 32, 32, 1, 1, 1, 0, 0)
        pixels = bytes((210, 100, 40, 0)) * (info.surfSize // 4)
        footer = struct.pack(">4sHHIIHBB", b"FLIM", 0xFEFF, 20, 0x02020000,
                             len(pixels) + 40, 1, 1, 0)
        footer += struct.pack(">4sI3H2BI", b"imag", 16, 32, 32,
                              info.baseAlign, 20, 1, len(pixels))
        data = pixels + footer
        path = "content/Common/Pack/permanent_2d_UsEnglish.pack/BtnItemIcon_00.szs/timg/Icon128_12^l.bflim"
        hd = formats.bflim(data, path)
        self.assertEqual(next(hd.images()).getpixel((0, 0)), (210, 100, 40, 0))
        target = object()
        matches = list(source_textures(data, path, {(("object", "itemicon"), "camera"): [target]}))
        self.assertEqual(matches[0][0].name, "camera")
        self.assertEqual(matches[0][1], [target])
        water_path = path.replace("Icon128_12", "Icon128_01")
        water_targets = {(("object", "itemicon"), "bottle_05"): [target],
                         (("object", "itemicon"), "bottle_08"): [target]}
        water = list(source_textures(data, water_path, water_targets))
        self.assertEqual(water[0][1], [target, target])
        self.assertEqual(water[0][0].name, "bottle_05")
        self.assertEqual(list(source_textures(data, path.replace("BtnItemIcon_00", "Other"), {})), [])
        with self.assertRaises(FormatError):
            formats.bflim(data[:-1], path)

    def test_yaz0_overlap_and_truncation(self):
        encoded = b"Yaz0" + struct.pack(">I", 19) + bytes(8) + b"\x80A\0\0\0"
        self.assertEqual(formats.yaz0(encoded), b"A" * 19)
        with patch.object(formats, "_native_yaz0", None):
            self.assertEqual(formats.yaz0(encoded), b"A" * 19)
            with self.assertRaises(FormatError):
                formats.yaz0(encoded[:-1])
        with self.assertRaises(FormatError):
            formats.yaz0(encoded[:-1])

    def test_bflim_luminance_alpha_masks(self):
        # BC5 encodes luminance in R and alpha in G for Wii U layout images.
        # A constant block makes both selections independently observable.
        info = addrlib.getSurfaceInfo(0x35, 4, 4, 1, 1, 1, 0, 0)
        block = bytes((90, 90)) + bytes(6) + bytes((170, 170)) + bytes(6)
        pixels = block * (info.surfSize // 16)
        footer = struct.pack(">4sHHIIHBB", b"FLIM", 0xFEFF, 20, 0x02020000,
                             len(pixels) + 40, 1, 1, 0)
        footer += struct.pack(">4sI3H2BI", b"imag", 16, 4, 4,
                              info.baseAlign, 17, 1, len(pixels))
        image = next(formats.bflim(pixels + footer, "mask.bflim").images())
        self.assertEqual(image.getpixel((0, 0)), (90, 90, 90, 170))

    def test_audited_variant_can_resolve_prior_conflict(self):
        from PIL import Image
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            gc, hd = base / "game.iso", base / "content"
            fake_iso(gc, ["A", "B", "C"])
            hd.mkdir()
            for name, color in (("A", (255, 0, 0, 255)),
                                ("B", (0, 255, 0, 255)),
                                ("C", (0, 0, 255, 255))):
                (hd / (name + ".bfres")).write_bytes(fake_bfres(name, color))
            args = argparse.Namespace(gc_disc=gc, hd_source=hd, output=base / "pack", only=None, install=False)
            with patch("import_wwhd_textures.candidate_priority",
                       side_effect=lambda texture, identity: int(texture.name == "C")):
                report = build(args)
            self.assertEqual(report["replacement_count"], 1)
            self.assertEqual(report["conflicts_skipped"], [])
            # The fixture's GameCube I8 sampling uses the selected blue art's
            # zero R channel for intensity and alpha.
            self.assertEqual(Image.open(args.output / report["textures"][0]["file"]).getpixel((0, 0)), (0, 0, 0, 0))
            self.assertTrue(report["textures"][0]["hd"].endswith("C.bfres#C"))
            self.assertEqual(len(report["textures"][0]["alternatives"]), 2)

    def test_palette_hash_only_referenced_range(self):
        import xxhash
        data = b"\x55" * 32  # C4 references only index 5.
        palette = bytes(range(32))
        t = GCTexture("x", "x", 8, 8, 8, data, palette, 0, 1)
        self.assertEqual(t.filename(), f"tex1_8x8_{xxhash.xxh64_hexdigest(data)}_{xxhash.xxh64_hexdigest(palette[10:12])}_8.png")
        t.palette = bytes(10) + palette[10:12] + bytes(20)
        self.assertIn(xxhash.xxh64_hexdigest(palette[10:12]), t.filename())
        t.palette = bytes(10)
        with self.assertRaises(FormatError):
            t.filename()

    def test_bc1_color_and_alpha(self):
        # A BC1 block selecting RGB565 red for every pixel.
        t = HDTexture("x", "x", 4, 4, 0x431, (0, 1, 2, 3), [b"\x00\xf8\0\0\0\0\0\0"])
        self.assertEqual(next(t.images()).getpixel((3, 3)), (255, 0, 0, 255))

    def test_rg8_component_select(self):
        t = HDTexture("x", "x", 1, 1, 7, (0, 0, 0, 1), [b"\x64\x2a"])
        self.assertEqual(next(t.images()).getpixel((0, 0)), (100, 100, 100, 42))
        t.components = (4, 5, 4, 5)
        self.assertEqual(next(t.images()).getpixel((0, 0)), (0, 255, 0, 255))

    def test_gc_intensity_replacement_keeps_alpha_mask(self):
        from PIL import Image
        image = Image.new("RGBA", (2, 1), (64, 64, 64, 255))
        image.putpixel((0, 0), (0, 0, 0, 255))
        for fmt in (0, 1):
            fixed = formats.for_gc_format([image], fmt)[0]
            self.assertEqual(fixed.getpixel((0, 0)), (0, 0, 0, 0))
            self.assertEqual(fixed.getpixel((1, 0)), (64, 64, 64, 64))
        self.assertEqual(formats.for_gc_format([image], 3)[0].getpixel((1, 0)), (64, 64, 64, 255))
        image.putpixel((1, 0), (255, 255, 255, 0))
        self.assertEqual(formats.for_gc_format([image], 0, layout_mask=True)[0].getpixel((1, 0)), (0, 0, 0, 0))

    def test_ftex_real_offsets(self):
        ts = list(bfres(fake_bfres(), "A.bfres"))
        self.assertEqual(len(ts), 1)
        self.assertEqual(ts[0].name, "A")
        self.assertEqual(next(ts[0].images()).getpixel((31, 31)), (255, 0, 0, 255))
        with self.assertRaises(FormatError):
            list(bfres(fake_bfres()[:0x1000], "A.bfres"))

    def test_stage_resource_scope(self):
        self.assertEqual(resource("res/Stage/sea/Room0.arc/bdl/model.bdl#grass"),
                         resource("content/Common/Stage/sea_Room0.szs/Room0.bfres#grass"))
        self.assertNotEqual(resource("res/Stage/sea/Room0.arc/bdl/model.bdl"),
                            resource("content/Common/Stage/sea_Room1.szs/Room1.bfres"))
        self.assertEqual(resource("res/Object/Link.arc/bdl/cl.bdl"),
                         resource("content/Common/Pack/permanent_3d.pack/Link.szs/Link.bfres"))
        self.assertEqual(resource("res/Stage/sea/Room11.arc/bdl/model.bdl"),
                         resource("content/Common/Pack/szs_permanent1.pack/sea_Room11.szs/Room11.bfres"))
        self.assertNotEqual(resource("content/Common/Pack/all.pack/sea_Room11.szs/Room11.bfres"),
                            resource("content/Common/Pack/all.pack/Other_Room11.szs/Room11.bfres"))

    def test_changed_uv_aspect_and_placeholder_skipped(self):
        gc = GCTexture("x", "x", 32, 16, 1, bytes(512), b"", 0, 1)
        hd = HDTexture("x", "x", 128, 64, 1, (0, 0, 0, 5), [])
        self.assertTrue(compatible(gc, hd))
        hd.height = 128
        self.assertFalse(compatible(gc, hd))
        gc.width = gc.height = 8
        self.assertFalse(compatible(gc, hd))
        gc.width = hd.width = 128
        gc.height = hd.height = 32
        gc.source, gc.name = "res/Object/Always.arc/tex/camera_self.bti", "camera_self"
        self.assertFalse(compatible(gc, hd))
        gc.source, gc.name = "res/Object/TlogoE.arc/timg/logo_zelda_main.bti", "logo_zelda_main"
        hd.source = "content/Common/Object/Tlogo.szs/Tlogo.bfres#logo_zelda_main"
        self.assertFalse(compatible(gc, hd))
        hd.source = "content/Common/Layout/Title_00.szs/timg/TitleLogoZelda_00^l.bflim#GameCube-layout"
        self.assertTrue(compatible(gc, hd))

    def test_wux_random_access(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "synthetic.wux"
            header = struct.pack("<4sII I Q Q", b"WUX0", 0x1099D02E, 0x8000, 0, 0x10000, 0)
            path.write_bytes(header + struct.pack("<2I", 1, 0) + bytes(0x8000 - 40) + b"B" * 0x8000 + b"A" * 0x8000)
            image = DiscImage(path)
            try:
                self.assertEqual(image.read(0x7FFD, 6), b"AAABBB")
                with self.assertRaises(FormatError):
                    image.read(0x10000, 1)
            finally:
                image.close()

    def test_hashed_content_verifies_decryption(self):
        from Crypto.Cipher import AES
        key = bytes(range(16))
        plain = bytes([73]) * 0xFC00
        digest = hashlib.sha1(plain).digest()
        hashes = digest + bytes(0x400 - 20)
        raw = AES.new(key, AES.MODE_CBC, bytes(16)).encrypt(hashes)
        raw += AES.new(key, AES.MODE_CBC, digest[:16]).encrypt(plain)
        disc = object.__new__(WiiUDisc)
        disc.start, disc.clusters, disc.title_key = 0, [(1, 2)], key
        disc.image = type("Image", (), {"read": lambda _, at, size: raw})()
        self.assertEqual(disc.hashed_block(0, 0), plain)
        disc.hashed_block.cache_clear()
        raw = raw[:-1] + bytes([raw[-1] ^ 1])
        with self.assertRaises(FormatError):
            disc.hashed_block(0, 0)
        disc.hashed_block.cache_clear()

    def test_import_and_conflicting_hashes(self):
        from PIL import Image
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            gc, hd = base / "game.iso", base / "content"
            fake_iso(gc, ["A"])
            hd.mkdir()
            (hd / "A.bfres").write_bytes(fake_bfres())
            args = argparse.Namespace(gc_disc=gc, hd_source=hd, output=base / "pack", only=None, install=False)
            report = build(args)
            self.assertEqual(report["replacement_count"], 1)
            self.assertEqual(Image.open(args.output / report["textures"][0]["file"]).getpixel((0, 0)), (255, 255, 255, 255))
            self.assertEqual(report["textures"][0]["mipmaps"], 6)
            fake_iso(gc, ["A", "B"])
            (hd / "B.bfres").write_bytes(fake_bfres("B", (0, 255, 0, 255)))
            args.output = base / "conflict"
            with self.assertRaisesRegex(FormatError, "no compatible textures"):
                build(args)
            self.assertFalse(args.output.exists())

    def test_install_preserves_settings_and_backup(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            settings = base / "settings.ini"
            original = "# personal settings\nDOL_AURORA_TEXTURE_PACK=old\nBLUEWAKE_HAPTICS=classic\nOTHER=value\n"
            settings.write_text(original)
            install(base / "pack", settings, windows=False)
            self.assertIn("BLUEWAKE_HAPTICS=classic\nOTHER=value\n", settings.read_text())
            self.assertIn("DOL_AURORA_TEXTURE_PACK=" + str((base / "pack").resolve()) + "\n", settings.read_text())
            self.assertEqual(next(base.glob("settings.ini.before-wwhd-*")).read_text(), original)
            # BlueWake for Windows: its own keys, the pack turned on and named.
            windows = base / "windows" / "settings.ini"
            windows.parent.mkdir()
            windows.write_text("# BlueWake settings\nhd_textures=0\nclimb=1\ntexture_pack=old\nhaptics=classic\n")
            install(base / "pack", windows, windows=True)
            self.assertEqual(windows.read_text(), "# BlueWake settings\nclimb=1\nhaptics=classic\nhd_textures=1\n"
                             "texture_pack=" + str((base / "pack").resolve()) + "\n")
            invalid = base / "bad.key"
            invalid.write_bytes(b"x")
            with self.assertRaises(FormatError):
                read_key(invalid)

    def test_fallback_preserves_primary_across_formats_and_mips(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            pack, fallback = root / "pack", root / "old-pack"
            pack.mkdir()
            fallback.mkdir()
            key = "tex1_32x32_0123456789abcdef_14"
            other = "tex1_16x16_1111111111111111_0"
            excluded = "tex1_16x16_2222222222222222_0"
            (pack / (key + ".png")).write_bytes(b"primary")
            (fallback / (key + ".dds")).write_bytes(b"old base")
            (fallback / (key + "_mip1.dds")).write_bytes(b"old mip")
            (fallback / (other + ".png")).write_bytes(b"fallback base")
            (fallback / (other + "_mip1.png")).write_bytes(b"fallback mip")
            (fallback / (excluded + ".png")).write_bytes(b"excluded base")
            (fallback / (excluded + "_mip1.png")).write_bytes(b"excluded mip")
            (fallback / "Dolphin.exe").write_bytes(b"not texture content")
            report = add_fallback(pack, [key + ".png"], fallback, [excluded])
            self.assertEqual(report["texture_count"], 1)
            self.assertEqual(report["file_count"], 2)
            self.assertEqual(report["primary_overlap_count"], 1)
            self.assertEqual(report["excluded_keys"], [excluded])
            self.assertFalse((pack / "fallback" / (excluded + ".png")).exists())
            self.assertFalse((pack / "fallback" / (excluded + "_mip1.png")).exists())
            self.assertEqual((pack / (key + ".png")).read_bytes(), b"primary")
            self.assertFalse((pack / "fallback" / (key + ".dds")).exists())
            self.assertEqual((pack / "fallback" / (other + "_mip1.png")).read_bytes(), b"fallback mip")
            self.assertFalse((pack / "fallback" / "Dolphin.exe").exists())

    def test_fallback_normalizes_renderer_aliases_and_ignores_orphans(self):
        key = "tex1_256x256_35b39510e63e0edd_0"
        self.assertEqual(texture_file("tex1_256x256_m_35b39510e63e0edd_0_arb.dds"), (key, 0, "dds"))
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            pack, fallback = root / "pack", root / "old"
            pack.mkdir()
            fallback.mkdir()
            alias = "tex1_256x256_m_35b39510e63e0edd_0_arb"
            (fallback / (alias + ".dds")).write_bytes(b"base")
            (fallback / (alias + "_mip1.dds")).write_bytes(b"mip")
            (fallback / "tex1_32x32_0000000000000000_14_mip1.png").write_bytes(b"orphan")
            report = add_fallback(pack, [], fallback)
            self.assertEqual(report["texture_count"], 1)
            self.assertEqual(report["file_count"], 2)
            self.assertTrue((pack / "fallback" / (key + "_mip1.dds")).exists())


if __name__ == "__main__":
    unittest.main()
