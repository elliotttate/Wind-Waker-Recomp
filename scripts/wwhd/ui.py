"""Verified HD font and layout adaptations to fixed GameCube sprite UVs."""
from .formats import BLOCKS, FormatError


ACTIONS = {
    "ba_attack": "Attack", "ba_chekku": "Check", "ba_hanasu": "Let Go",
    "ba_harituku": "Slide", "ba_hiraku": "Open", "ba_huru": "Swing",
    "ba_husegu": "Defend", "ba_jump": "Jump", "ba_kaizuhe": "Sea Chart",
    "ba_kettei": "Choose", "ba_modoru": "Return", "ba_motu": "Lift",
    "ba_motu_buki": "Pick Up", "ba_nageru": "Throw", "ba_noru": "Climb",
    "ba_noru_hune": "Get In", "ba_oku": "Drop", "ba_oriru": "Let Go",
    "ba_oriru_hune": "Get Out", "ba_setumei": "Info", "ba_shagamu": "Crouch",
    "ba_shimau": "Put Away", "ba_shiraberu": "Charts", "ba_susumu": "Cruise",
    "ba_syaberu": "Speak", "ba_tomeru": "Stop", "ba_tugihe": "Next",
    "ba_tukamu": "Grab", "ba_yameru": "Cancel", "ba_yomu": "Read",
    "ba_zoom01": "Zoom1", "ba_zoom02": "Zoom2",
}


def font_sprites(font_name):
    """Return exact resource/name/text bindings, never translate by filename."""
    if font_name == "CKingMsg":
        yield from ((("object", "acticon"), name, text) for name, text in ACTIONS.items())
        yield ("object", "menures"), "ba_modoru", "Return"
        for name in ("ba_kettei", "ba_modoru"):
            yield ("stage", "name", "stage"), name, ACTIONS[name]
        for n in range(10):
            yield ("object", "menures"), f"count_num_{n}", str(n)
        yield ("object", "menures"), "rupy_num_cross", "×"
        for n in range(1, 5):
            yield ("object", "dmapres"), f"hierarchy_num_{n}", str(n)
        for n in range(1, 4):
            yield ("object", "dmapres"), f"hierarchy_num_b{n}", f"B{n}"
    elif font_name == "CKingMainL":
        for n in range(10):
            yield ("object", "menures"), f"rupy_num_{n:02d}", str(n)
        for name, text in (("rupy_num_dot", "."), ("rupy_num_slash", "/")):
            yield ("object", "menures"), name, text
        for res, name, text in (("clctres", "title_collect", "QUEST STATUS"),
                                ("clctres", "word_option", "OPTIONS"),
                                ("clctres", "word_save", "SAVE"),
                                ("itemres", "title_item", "ITEMS"),
                                ("itemres", "word_save2", "SAVE"),
                                ("saveres", "title_save", "SAVE")):
            yield ("object", res), name, text
        for n in range(1, 4):
            yield ("stage", "name", "stage"), f"file_data_{n:02d}", f"Quest Log {n}"
    elif font_name == "CKingPic":
        for n in range(4):
            yield ("object", "menures"), f"font_{n:02d}", 0xE000 + n


def alpha_bounds(gc):
    """Read only the original sprite's alpha; no original artwork is copied."""
    from PIL import Image
    fmt = gc.format
    if fmt not in (0, 1, 2, 3, 8, 9):
        raise FormatError("unsupported sprite mask format")
    bw, bh, bs = BLOCKS[fmt]
    mask = bytearray(gc.width * gc.height)
    palette = []
    if fmt in (8, 9):
        for i in range(0, len(gc.palette), 2):
            v = int.from_bytes(gc.palette[i:i + 2], "big")
            if gc.palette_format == 0:
                palette.append(v >> 8)
            elif gc.palette_format == 1:
                palette.append(255)
            elif gc.palette_format == 2:
                palette.append(255 if v & 0x8000 else ((v >> 12) & 7) * 255 // 7)
            else:
                raise FormatError("invalid sprite palette format")
    offset = 0
    for by in range(0, gc.height, bh):
        for bx in range(0, gc.width, bw):
            for p in range(bw * bh):
                x, y = bx + p % bw, by + p // bw
                if x >= gc.width or y >= gc.height:
                    continue
                if fmt in (0, 8):
                    v = (gc.pixels[offset + p // 2] >> (4 if p % 2 == 0 else 0)) & 15
                elif fmt == 3:
                    v = gc.pixels[offset + p * 2]
                else:
                    v = gc.pixels[offset + p]
                if fmt in (8, 9):
                    if v >= len(palette):
                        raise FormatError("sprite mask references missing palette")
                    v = palette[v]
                elif fmt == 0:
                    v *= 17
                elif fmt == 2:
                    v = (v >> 4) * 17
                mask[y * gc.width + x] = v
            offset += bs
    box = Image.frombytes("L", (gc.width, gc.height), bytes(mask)).getbbox()
    if box is None:
        raise FormatError("empty original sprite mask")
    return box


def fit_sprite(image, gc, scale=4):
    from PIL import Image
    box = image.getchannel("A").getbbox()
    if box is None:
        raise FormatError("empty replacement sprite")
    left, top, right, bottom = alpha_bounds(gc)
    sprite = image.crop(box).resize(((right - left) * scale, (bottom - top) * scale),
                                    Image.Resampling.LANCZOS)
    result = Image.new("RGBA", (gc.width * scale, gc.height * scale))
    result.paste(sprite, (left * scale, top * scale))
    return result


# Same key and bracket artwork with different source padding in the HD UI.
LAYOUT_SPRITES = {
    ("Cursor_00", "Crsor_00^t.bflim"): ((("object", "menures"), "cursor_00_02"),),
    ("Cursor_00", "Crsor_01^t.bflim"): ((("object", "menures"), "cursor_00_01"),),
    ("DungeonKey_00", "DungeonKey_00^t.bflim"): ((("object", "menures"), "key"),),
    ("BtnDungeonItemIcon_00", "MapItemIcon_01^l.bflim"): ((("object", "itemicon"), "boss_key"),),
}


# HD adds side margins and stores the fourth story panel as one panorama.
# These crops were registered against the corresponding original panels.
# The panorama halves share their cut edge; independently aligning the second
# plane would skip artwork and leave a visible join while the camera pans.
OPENING_PANELS = {
    "OpeningPic_00^o.bflim": (("demo_1", (100, 0, 1178, 1000)),),
    "OpeningPic_01^o.bflim": (("demo_2", (104, 0, 1175, 500)),),
    "OpeningPic_02^o.bflim": (("demo_3", (102, 0, 1174, 500)),),
    "OpeningPic_03^o.bflim": (("demo_4", (102, 0, 1174, 500)),
                              ("demo_4_2", (1174, 0, 2221, 500))),
    "OpeningPic_04^o.bflim": (("demo_5", (102, 0, 1174, 500)),),
    "OpeningPic_05^o.bflim": (("demo_6", (104, 0, 1175, 500)),),
}


def opening_sprite(image, box, gc):
    from PIL import Image
    left, top, right, bottom = box
    if not (0 <= left < right <= image.width and 0 <= top < bottom <= image.height):
        raise FormatError("opening panel crop outside HD picture")
    return image.crop(box).resize((gc.width * 2, gc.height * 2), Image.Resampling.LANCZOS)
