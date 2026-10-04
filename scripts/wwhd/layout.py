"""Audited artwork aliases between the two games' inventory layouts.

Names refer to disc resources, not bundled artwork. Only visual equivalents
are mapped: the red Swift Sail and redesigned controller prompts are omitted.
"""
from pathlib import PurePosixPath


ITEM_ICONS = {
    0: ("bottle_00",), 1: ("bottle_05", "bottle_08"),
    # Forest Water's sparkle is drawn separately by the GameCube game.
    3: ("bottle_06",), 4: ("bottle_07",), 5: ("bottle_01",),
    6: ("bottle_02",), 7: ("bottle_03",), 8: ("bottle_04",),
    9: ("bottle_09",), 10: ("telescope",), 11: ("rope",),
    12: ("camera",), 13: ("camera_2",), 15: ("boots_00",),
    17: ("boomerang",), 18: ("bow_01",), 19: ("hookshot",),
    20: ("hammer_01",), 21: ("bomb_00",), 22: ("fan",),
    23: ("coverofbeast",), 24: ("coverofbait",), 25: ("delivery",),
    26: ("bait_02",), 27: ("bait_01",), 28: ("beast_08",),
    29: ("beast_02",), 30: ("beast_04",), 31: ("beast_03",),
    32: ("beast_01",), 33: ("beast_05",), 34: ("beast_06",),
    35: ("beast_07",), 36: ("delivery_17",), 37: ("delivery_13",),
    38: ("delivery_14",), 39: ("delivery_15",), 40: ("delivery_16",),
    41: ("delivery_18",), 42: ("delivery_19",), 43: ("delivery_01",),
    44: ("delivery_02",), 45: ("delivery_03",), 46: ("delivery_06",),
    47: ("delivery_08",), 48: ("delivery_05",), 49: ("delivery_04",),
    50: ("delivery_07",), 51: ("delivery_09",), 52: ("delivery_10",),
    53: ("delivery_11",), 54: ("delivery_12",),
}

COLLECT_ICONS = {
    4: ("shield_00",), 5: ("shield_01",),
    6: ("sail_00", "sail_01", "sail_02"), 8: ("big_purse",),
    9: ("gloves_00", "gloves_01"),
    11: ("sword_00",), 12: ("sword_01",), 13: ("sword_02",),
    14: ("sword_03",), 15: ("baton",),
}


def inventory_names(path: str):
    name = PurePosixPath(path).name
    for archive, prefix, mapping in (
        ("BtnItemIcon_00", "Icon128", ITEM_ICONS),
        ("BtnCollectIcon_00", "CollectIcon118", COLLECT_ICONS),
        ("BtnDungeonItemIcon_00", "MapItemIcon",
         {0: ("dungeon_map",), 2: ("compass",)}),
        ("BtnMapIcon_01", "MapBtn",
         {0: ("cmap_treasure2",), 1: ("cmap_phantomship2",),
          2: ("cmap_tingle2",), 3: ("cmap_hint2",), 4: ("cmap_tri2",)}),
    ):
        if f"/{archive}.szs/timg/" not in path:
            continue
        for number, names in mapping.items():
            if name == f"{prefix}_{number:02d}^l.bflim":
                return names
    return None


SWIM_ICONS = {"SwimTimeLight_00^t.bflim": "light",
              "SwimTimeBase_00^t.bflim": "swimtime_meter",
              "SwimTimeAlpha_00^t.bflim": "swimtime_meter_mask",
              "SwimTimeDeco_00^t.bflim": "swimtime_soul",
              "SwimTimeFrame_00^t.bflim": "swimtime_waku",
              "SwimTimeSpec_00^t.bflim": "tekari"}


HEARTS = {"HeartFull_00^l.bflim": "heart",
          "HeartDamage_00^l.bflim": "heart_01",
          "HeartDamage_01^l.bflim": "heart_02",
          "HeartDamage_02^l.bflim": "heart_03"}


def heart_image(base, fill):
    """Rebuild HD's separate outline/fill in the GameCube heart's UV area."""
    from PIL import Image
    image = Image.alpha_composite(base, fill)
    # Disc-verified alpha bounds: HD (5,7)-(57,45), GC (1,3)-(23,20).
    # Preserve the transparent padding used by the HUD's fixed sprite plane.
    image = image.crop((5, 7, 57, 45)).resize((88, 68), Image.Resampling.LANCZOS)
    result = Image.new("RGBA", (96, 96))
    result.paste(image, (4, 12))
    return result
