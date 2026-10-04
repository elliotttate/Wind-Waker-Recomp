"""Audited choices for art shared by several GameCube resources.

HD debug rooms and figurines sometimes reuse a name with different artwork.
For these visually checked aliases, choose the variant preserving the original
UV composition. Equal-priority disagreements still remain unresolved.
"""

PREFERRED = {
    "dk_kamen1": ("object", "demo_dk"),
    "test_grass_ktga": ("stage", "a_mori", "room0"),
    "txa_nami_ia": ("stage", "sea", "room11"),
    "txa_nami_01": ("stage", "sea", "room11"),
    "door_08": ("stage", "ebesso", "stage"),
    "txa_doom_a": ("stage", "sea", "room44"),
    "txa_hune_a": ("stage", "sea", "room44"),
    "txa_ita3mai_a": ("stage", "sea", "room44"),
    "txa_iwa2_a": ("stage", "linkug", "room0"),
    "bm_head01": ("object", "demo10"),
    # Some HD skies changed to photographic cloud layers. These retained
    # cartoon variants match the silhouette and alpha of the GameCube draw.
    "ws_naka06": ("stage", "cave08", "stage"),
    "ws_mae08": ("stage", "cave08", "stage"),
    "ws_mae09": ("stage", "cave08", "stage"),
    "cloudtx_01": ("stage", "a_nami", "stage"),
    "cloudtx_03": ("stage", "a_nami", "stage"),
    "txo_stone_mori_old": ("stage", "sea", "room42"),
}


def candidate_priority(hd, identity):
    return int(PREFERRED.get(hd.name.lower()) == identity)
