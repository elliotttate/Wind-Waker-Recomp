#!/usr/bin/env python3
"""Certified native entries, the second set: hooks at the entries of
translated GZLE01 functions with a native form in cmake/composite
(native_fifo.c, native_bg.c, native_vec.c's PSMTXMultVecSR,
native_mtxcalc.c).
The third set (native_search.c: strcmp and dStage_searchName, the actor
search by name) is hooked the same way, by the entries added below the
second set's.

  native_entries.py COMPOSITE_SRC
  native_entries.py --hashes COMPOSITE_SRC    (print each entry's body hash)

Each hook goes right after the function's entry label, so every way into
the function meets it - a call inside the chunk (a goto), a direct call from
another chunk, a dispatch - and tries the native first:

    if (bluewake_native_fifo_enabled && bluewake_native_fifo(ctx, 0x802D8BD8u))
        goto return_dispatch_802D56E0;

The native either ran the whole function to its blr, leaving pc at the
return address, and the chunk's return dispatch carries on from there as the
blr's would; or it changed nothing and the translation runs as before.

Only where the translation is the one the native's comparison test
(tests/native_*_test.c) compared it against: every fragment of the function
(the blocks from its entry label, in each chunk it spans) and every prepaid
copy those blocks jump to (scripts/windows/fast_blocks.py and lean_memory.py
made them, and they are what runs), whitespace aside and this script's hooks
removed, hashes to the certified value - in the base chunk and in every mod's
variant of it. An entry whose fragments differ anywhere, or one of whose
addresses the host watches (direct_calls.watched_addresses), is not hooked,
and the step says so; a rerun after a change removes a hook it no longer
certifies. The change is repeatable and keeps LF line ends.

Run it after lean_memory.py and prepare_native_j3d.py, and before
scripts/mods/prepare_simulation_60hz.py and prepare_native_math.py, whose
manifests hash the chunks as they end up.
"""
import hashlib
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from direct_calls import watched_addresses  # noqa: E402

MARK = "/* bluewake: certified native entries (scripts/windows/native_entries.py) */\n"
INCLUDE = '#include "../generated.h"\n'

# group -> (header, enable flag, native call with {entry})
GROUPS = {
    "fifo": ("native_fifo.h", "bluewake_native_fifo_enabled", "bluewake_native_fifo(ctx, 0x{entry:08X}u)"),
    "bg": ("native_bg.h", "bluewake_native_bg_enabled", "bluewake_native_bg(ctx, 0x{entry:08X}u)"),
    "vec_sr": ("native_vec.h", "bluewake_native_vec_sr_enabled", "bluewake_native_vec_sr(ctx)"),
    "mtxcalc": ("native_mtxcalc.h", "bluewake_native_mtxcalc_enabled", "bluewake_native_mtxcalc(ctx, 0x{entry:08X}u)"),
}
# --- The third set: the actor search by name (cmake/composite/native_search.c) ---
GROUPS["search"] = ("native_search.h", "bluewake_native_search_enabled",
                    "bluewake_native_search(ctx, 0x{entry:08X}u)")
# --- end of the third set's groups ---

# entry -> (name, group, fragments [(chunk start, first address, end address)],
#           SHA-256 of the canonical fragments and prepaid copies)
ENTRIES = {
    0x802D8BD8: ("J3DFifoLoadPosMtxImm", "fifo", [(0x802D56E0, 0x802D8BD8, 0x802D8C58)],
                 "c1f2664aa12f9dd8c195fcf4a862e77793b889902ca390018f1b1b5090c4dbdf"),
    0x802D8C58: ("J3DFifoLoadNrmMtxImm", "fifo", [(0x802D56E0, 0x802D8C58, 0x802D8CC4)],
                 "6cee7486d365cb81b8d1b9e5b63dd0720e271af0d4bb323be726d4333e3ec9ad"),
    0x802D8CC4: ("J3DFifoLoadNrmMtxImm3x3", "fifo", [(0x802D56E0, 0x802D8CC4, 0x802D8D30)],
                 "2affe02fcb1211515e55f97b2eab1c7f685fe661c30259af77361385fec49ee9"),
    0x8024734C: ("cBgS_Chk::ChkSameActorPid", "bg", [(0x802456E0, 0x8024734C, 0x8024738C)],
                 "a3cf44b28b18dfc3b21022150567e7e5223ea3ff3ffdc777e374f7fdd806d1ee"),
    0x800A9684: ("dBgW::ChkGrpThrough", "bg", [(0x800A56E0, 0x800A9684, 0x800A96E0),
                                               (0x800A96E0, 0x800A96E0, 0x800A974C)],
                 "d491b8f1f60e6ad9ae20524d879bb8b1d74d9608e919e6c07601e8dcfa69227f"),
    0x8030DB24: ("PSMTXMultVecSR", "vec_sr", [(0x8030D6E0, 0x8030DB24, 0x8030DB78)],
                 "600139a8e4e0caabf4a3006b3eb737577c0934568e1741186eeec1b9565b7db9"),
    # The joint matrix calculations, and the callees they stand in for too:
    # J3DGetTranslateRotateMtx (802DA64C or 802DA724), PSMTXConcat (8030D0FC),
    # PSMTXCopy (8030D0C8).
    0x802F5090: ("J3DMtxCalcBasic::calcTransform", "mtxcalc",
                 [(0x802F16E0, 0x802F5090, 0x802F525C), (0x802D96E0, 0x802DA64C, 0x802DA724),
                  (0x803096E0, 0x8030D0FC, 0x8030D1C8), (0x803096E0, 0x8030D0C8, 0x8030D0FC)],
                 "3e442f9bac67c268093ed4abdbcb9c4659fad59339a700282a7eb9e023b11d67"),
    0x802F52BC: ("J3DMtxCalcSoftimage::calcTransform", "mtxcalc",
                 [(0x802F16E0, 0x802F52BC, 0x802F5508), (0x802D96E0, 0x802DA724, 0x802DA7E4),
                  (0x803096E0, 0x8030D0FC, 0x8030D1C8), (0x803096E0, 0x8030D0C8, 0x8030D0FC)],
                 "7ad20637ab7fb125cfee5c1aa81bd982abfc8bbe5f26747bae359fa320fd56d2"),
    0x802F5508: ("J3DMtxCalcMaya::calcTransform", "mtxcalc",
                 [(0x802F16E0, 0x802F5508, 0x802F56E0), (0x802F56E0, 0x802F56E0, 0x802F5724),
                  (0x802D96E0, 0x802DA64C, 0x802DA724), (0x803096E0, 0x8030D0FC, 0x8030D1C8),
                  (0x803096E0, 0x8030D0C8, 0x8030D0FC)],
                 "8df3c54d0620dc3152b7b640038b7637d9514d3e51cd6133f4dfb4b836f9c324"),
}
# --- The third set (tests/native_search_test.c): strcmp, and dStage_searchName
# with the strcmps it calls in strcmp's chunk, which its native stands in for
# too. ---
ENTRIES[0x8032DB44] = ("strcmp", "search", [(0x8032D6E0, 0x8032DB44, 0x8032DC6C)],
                       "0a578813114c0553f81a447d7d0c6ef7d36b22afc5e5f6465ae01517bb5a2375")
ENTRIES[0x80041544] = ("dStage_searchName", "search",
                       [(0x8003D6E0, 0x80041544, 0x800415B4), (0x8032D6E0, 0x8032DB44, 0x8032DC6C)],
                       "bdf91cee67046b81e7b60bb4669129749a912db84cd9fd96028b74a4cac01dd7")
# cTgIt_JudgeFilter with fopAcM_findObjectCB as its judge (one call), with the
# callees its native stands in for: fopAcM_findObjectCB, dStage_searchName and
# strcmp; and cNdIt_Judge's loop, which the walk native the host may call
# (bluewake_native_search_judge) runs too - the hook arms it.
ENTRIES[0x80245640] = ("cTgIt_JudgeFilter", "search",
                       [(0x802416E0, 0x80245640, 0x80245674), (0x802416E0, 0x80244F78, 0x80244FB4),
                        (0x800256E0, 0x8002833C, 0x80028410), (0x8003D6E0, 0x80041544, 0x800415B4),
                        (0x8032D6E0, 0x8032DB44, 0x8032DC6C)],
                       "795c6cb80232a10a8e2ceca570ee310e52f37a4ca4931a70920261c6f75385c2")
# --- end of the third set's entries ---
# --- The third set, resumable (tests/native_search_test.c): dStage_searchName's
# loop at its call block, a strcmp's return and its step, where a search the
# native stopped for the host's window, or the translation began, goes on
# natively. The same translation as the entry's (the whole function and
# strcmp), so the same hash. ---
for _resume in (0x8004156C, 0x80041578, 0x80041588):
    ENTRIES[_resume] = ("dStage_searchName at %08X" % _resume, "search",
                        [(0x8003D6E0, 0x80041544, 0x800415B4), (0x8032D6E0, 0x8032DB44, 0x8032DC6C)],
                        "bdf91cee67046b81e7b60bb4669129749a912db84cd9fd96028b74a4cac01dd7")
# --- end of the resumable entries ---
# --- The fourth set (tests/native_kankyo_test.c): the environment's colour
# blends (cmake/composite/native_kankyo.c). kankyo_color_ratio_set stands in
# for the three s16_data_ratio_set calls it makes inside its chunk, so its
# hash covers that function too. ---
GROUPS["kankyo"] = ("native_kankyo.h", "bluewake_native_kankyo_enabled",
                    "bluewake_native_kankyo(ctx, 0x{entry:08X}u)")
ENTRIES[0x8018F894] = ("s16_data_ratio_set (d_kankyo)", "kankyo", [(0x8018D6E0, 0x8018F894, 0x8018F8E4)],
                       "a97d798df755a9dfa8d8c76e9387bd63fcb1a3a01ac77dea64257a70f3c02ed2")
ENTRIES[0x8018F8E4] = ("kankyo_color_ratio_set", "kankyo",
                       [(0x8018D6E0, 0x8018F8E4, 0x8018F9E8), (0x8018D6E0, 0x8018F894, 0x8018F8E4)],
                       "97603052f10971cf557a41245d089cb826682dbbbd57ffdc4fc68d9c38f2106b")
ENTRIES[0x8019803C] = ("s16_data_ratio_set (d_kyeff)", "kankyo", [(0x801956E0, 0x8019803C, 0x8019808C)],
                       "5d00d5f30faa943ea2066d221c0027d91cf9cdf449c1dd493964417803fc6c82")
# --- end of the fourth set's colour blends ---
# --- The fourth set (tests/native_anim_test.c): J3D's key-frame animation
# (cmake/composite/native_anim.c). Each entry's hash covers the callees its
# native stands in for: JMAHermiteInterpolation (in its own chunk) under
# J3DGetKeyFrameInterpolation<f32>, J3DHermiteInterpolationS under
# J3DGetKeyFrameInterpolationS, and all four under calcTransform. ---
GROUPS["anim"] = ("native_anim.h", "bluewake_native_anim_enabled", "bluewake_native_anim(ctx, 0x{entry:08X}u)")
_ANIM_HERMITE = (0x802FD6E0, 0x803012D8, 0x80301350)
_ANIM_KEY_F = (0x802F16E0, 0x802F2DAC, 0x802F2EF8)
_ANIM_HERMITE_S = (0x802ED6E0, 0x802F06D8, 0x802F072C)
_ANIM_KEY_S = (0x802ED6E0, 0x802F072C, 0x802F0954)
ENTRIES[0x803012D8] = ("JMAHermiteInterpolation", "anim", [_ANIM_HERMITE], "3db6672c3359fdffca724dccbc68927255deb4eb8e325ab50b41a5024002283e")
ENTRIES[0x802F2DAC] = ("J3DGetKeyFrameInterpolation<f32>", "anim", [_ANIM_KEY_F, _ANIM_HERMITE], "392cbc991dd96ab0eb97fb421508640e977421cdeb4f38dbc174605658c9d53a")
ENTRIES[0x802F072C] = ("J3DGetKeyFrameInterpolationS", "anim", [_ANIM_KEY_S, _ANIM_HERMITE_S], "1eea0238808243d866100a41ad995a9f50c1fd4df493d3f22335ec9bfa2f5813")
ENTRIES[0x802F0954] = ("J3DAnmTransformKey::calcTransform", "anim",
                       [(0x802ED6E0, 0x802F0954, 0x802F0E20), _ANIM_KEY_S, _ANIM_HERMITE_S, _ANIM_KEY_F,
                        _ANIM_HERMITE], "0e3e908a655916320aaef9b8bf7bcd304406d7f9ad3983ea056905c670cbd549")
# J3DPSCalcInverseTranspose, the normal matrices' paired-single leaf.
ENTRIES[0x802DA584] = ("J3DPSCalcInverseTranspose", "anim", [(0x802D96E0, 0x802DA584, 0x802DA64C)],
                       "15f0f639f4c2003e1d1bfa800ce1c8e75ffeb6268d5ed96240d6ddbb76c7b861")
# --- end of the fourth set's animation ---
# --- The fourth set (tests/native_cc_test.c): the collision checker's area
# division (cmake/composite/native_cc.c). ---
GROUPS["cc"] = ("native_cc.h", "bluewake_native_cc_enabled", "bluewake_native_cc(ctx, 0x{entry:08X}u)")
ENTRIES[0x80251D88] = ("cM3dGCyl::SetC", "cc", [(0x802516E0, 0x80251D88, 0x80252020)],
                       "415fe9d6692cbd65923cc297820c365807abbf82868b47e33d9ac7a25db8846b")
ENTRIES[0x8024170C] = ("cCcD_DivideArea::CalcDivideInfoOverArea", "cc", [(0x802416E0, 0x8024170C, 0x80241924)],
                       "8a3217398db768a43715377f0b10f65fe4d31ae19d1f7e8725d236a2142281c9")
# --- end of the fourth set's area division ---
# --- The fifth set (tests/native_gx_test.c): the GX SDK's FIFO writers
# (cmake/composite/native_gx.c, each its translation replayed: native_gx_gen.inc).
# Each entry's hash covers the callees its native stands in for:
# GXLoadTexObj's region callback, GXGetTexObjFmt and GXLoadTexObjPreLoaded,
# PreLoaded's TLUT callback, __GXSetSUTexRegs's __SetSURegs, __GXSetVCD's
# __GXXfVtxSpecs, and GXSetTexCoordGen2's and GXSetCurrentMtx's
# __GXSetMatrixIndex. ---
GROUPS["gx"] = ("native_gx.h", "bluewake_native_gx_enabled", "bluewake_native_gx_{entry:08X}(ctx)")
_GX_0199, _GX_0200, _GX_0201 = 0x8031D6E0, 0x803216E0, 0x803256E0
_GX_SU_TEX_REGS = [(_GX_0200, 0x803253B8, 0x80325534), (_GX_0200, 0x80325300, 0x803253B8)]  # with __SetSURegs
_GX_SET_VAT = (_GX_0200, 0x803221D8, 0x80322274)
_GX_SET_MATRIX_INDEX = (_GX_0201, 0x80327364, 0x803273E8)
_GX_PRELOADED = [(_GX_0200, 0x80324D50, 0x80324EE8), (_GX_0199, 0x8031FAC4, 0x8031FAE8)]  # with the TLUT callback
ENTRIES[0x80326F38] = ("GXLoadPosMtxImm", "gx", [(_GX_0201, 0x80326F38, 0x80326F88)], "8de21439c3a30b9b500d7f19a87230030c43dee872279bc9620f66de96383f03")
ENTRIES[0x80326F88] = ("GXLoadNrmMtxImm", "gx", [(_GX_0201, 0x80326F88, 0x80326FD8)], "afacaa53e925d22f7b988e3854cffc8430c04b59a45f4f02069d34585a3e174f")
ENTRIES[0x80325FA8] = ("GXSetTevColor", "gx", [(_GX_0201, 0x80325FA8, 0x8032601C)], "2182408aaeaa731470b4e6a79c92e978061fe4cee0c20a57e195e55d13694e28")
ENTRIES[0x8032601C] = ("GXSetTevColorS10", "gx", [(_GX_0201, 0x8032601C, 0x80326090)], "3cad71ba008ab04273afa4cd47dd28fe9cf65c42df33664110fa7e039b919eec")
ENTRIES[0x80326090] = ("GXSetTevKColor", "gx", [(_GX_0201, 0x80326090, 0x80326104)], "d03989eadea5e3658468928b8e654bb4f2ce6fba0e0ea999304c3e37110f5830")
ENTRIES[0x80322568] = ("GXSetArray", "gx", [(_GX_0200, 0x80322568, 0x803225F4)], "994bdd8537fa1d6a105929c11f72831f0049fad254a14182141cdcc376d83b0c")
ENTRIES[0x803263A0] = ("GXSetTevOrder", "gx", [(_GX_0201, 0x803263A0, 0x80326578)], "90dd983a02547d3ba4d679dd9f8c6b3e037aed7aee6e34acb429be9c8a5ec7cc")
ENTRIES[0x80326B80] = ("GXCallDisplayList", "gx", [(_GX_0201, 0x80326B80, 0x80326BF0)], "5ccd94da70ba6178a57e70e518f79ac6d966529cd93a52774dc6b952bbaefe16")
# GXBegin alone: where its dirty state needs a call, it declines, and the
# translation makes the call, each callee native at its own hook.
ENTRIES[0x803230C4] = ("GXBegin", "gx", [(_GX_0200, 0x803230C4, 0x803231B4)], "aa7721852ed559a5a69aaae0cf62658dfe6f292a902da2dbad164c49b0fb276d")
ENTRIES[0x80324D50] = ("GXLoadTexObjPreLoaded", "gx", _GX_PRELOADED, "69e4d6b093571b644c0651d0c9e22177f3867b77045b05f7937500627e1f9a53")
ENTRIES[0x80324EE8] = ("GXLoadTexObj", "gx",
                       [(_GX_0200, 0x80324EE8, 0x80324F3C), (_GX_0199, 0x8031FA48, 0x8031FAC4),
                        (_GX_0200, 0x80324D28, 0x80324D30)] + _GX_PRELOADED, "f1303133a4a2a5d4ac80bfa425cb2b4981b8ae5379013a50b931999c34d1bfc9")
ENTRIES[0x803253B8] = ("__GXSetSUTexRegs", "gx", _GX_SU_TEX_REGS, "32404484f84b9606f83476964e5386ca2963a0d2edbc46ac6432a01e0ed6da3f")
ENTRIES[0x803221D8] = ("__GXSetVAT", "gx", [_GX_SET_VAT], "09f176ea1909c6acae1f294ea142f205bf150065d26c072fd23f4d47e7bba2bc")
ENTRIES[0x80327364] = ("__GXSetMatrixIndex", "gx", [(_GX_0201, 0x80327364, 0x803273E8)], "b7ef7243b32b9accaf763c748629310cacf5a75fa1d6902a5d54453571e6f5bd")
ENTRIES[0x80325CD4] = ("__GXUpdateBPMask", "gx", [(_GX_0201, 0x80325CD4, 0x80325DA0)], "2868c3aff143fc5f0af338ced04c12122db31ab8fc9b91f02b1478eff4f46a92")
ENTRIES[0x803233B0] = ("__GXSetGenMode", "gx", [(_GX_0200, 0x803233B0, 0x803233D4)], "9601c3051ea5c89cd989f1939be34ab6945c9f23db060172e7300111137631c1")
ENTRIES[0x80321958] = ("__GXSetVCD", "gx", [(_GX_0200, 0x80321958, 0x803219AC), (_GX_0199, 0x803214B0, 0x80321608)], "a780fca6787628e28af7b5a8fa99fc241b4da5a25b1fde9b1cf6429be4809ab0")
ENTRIES[0x803214B0] = ("__GXXfVtxSpecs", "gx", [(_GX_0199, 0x803214B0, 0x80321608)], "0bc9c3bff2f0a7622646f3cce4317e874b4910970ea3a7772e936373ea196b70")
ENTRIES[0x803219AC] = ("__GXCalculateVLim", "gx", [(_GX_0200, 0x803219AC, 0x80321AD0)], "c143b4c616a0bf8b2f8e69f77753a7d79cc37040391d70ede2a66063110c69f3")
# Texture coordinates, lighting channels, the current matrix.
ENTRIES[0x80322604] = ("GXSetTexCoordGen2", "gx", [(_GX_0200, 0x80322604, 0x803228D4), _GX_SET_MATRIX_INDEX], "1439617621dd5b80bc69edbb6ce727042f67ef8517b08799c3bc308571b951dd")
ENTRIES[0x803228D4] = ("GXSetNumTexGens", "gx", [(_GX_0200, 0x803228D4, 0x80322914)], "5efc2b471817a42487c38ca3311e4ac35b5e53ee8dfdb9ef21543a9b9c69f5d8")
ENTRIES[0x80324390] = ("GXSetChanAmbColor", "gx", [(_GX_0200, 0x80324390, 0x80324484)], "1f515c934c1836f9bf37ce699f8f8ac35627f2d8b471ece8e61e8c67b38ad02a")
ENTRIES[0x80324484] = ("GXSetChanMatColor", "gx", [(_GX_0200, 0x80324484, 0x80324578)], "fcda2494a487596bdc6dce874aabde41651f3ad68de04146171a9c23b29ac2dc")
ENTRIES[0x80324578] = ("GXSetNumChans", "gx", [(_GX_0200, 0x80324578, 0x803245BC)], "8f4e5b4524b2f7255bb5ea17bb3374a04df56c4a9bd626be57df23d449ef4a3f")
ENTRIES[0x803245BC] = ("GXSetChanCtrl", "gx", [(_GX_0200, 0x803245BC, 0x80324688)], "d516a0552692157aa3ada31ea64b14b381f06698739b77a72c967c33df49b539")
ENTRIES[0x80326FD8] = ("GXSetCurrentMtx", "gx", [(_GX_0201, 0x80326FD8, 0x80327010), _GX_SET_MATRIX_INDEX], "6ab6ebcd301b1d79e5ef5878cdb8146ca72bde211818b54a5f0ed3f16ebd0f21")
# With floating point: the fog (with __cvt_fp2unsigned), texture LOD and
# indirect matrices; J3D's display-list writers (J3DGD: through the current
# GD list's write pointer) and its direct FIFO forms (GF).
_GX_0171, _GX_0181 = 0x802AD6E0, 0x802D56E0
_GX_CVT_FP2UNSIGNED = (_GX_0201, 0x80328E10, 0x80328E6C)
ENTRIES[0x803265A8] = ("GXSetFog", "gx", [(_GX_0201, 0x803265A8, 0x80326758), _GX_CVT_FP2UNSIGNED], "1c9cdbea0b3ea321249415d1062eb472b8a0bd43d1d342951b43242751045d74")
ENTRIES[0x80326758] = ("GXSetFogRangeAdj", "gx", [(_GX_0201, 0x80326758, 0x80326858)], "b990f24405435ab251623d6cdb444a13b929e27d08c4dbe18175c48c05bd48de")
ENTRIES[0x80324B68] = ("GXInitTexObjLOD", "gx", [(_GX_0200, 0x80324B68, 0x80324CFC)], "f02fad81d28494fcb02a16b05c5ce310dabe39e8db86462d8ae214beb250a2de")
ENTRIES[0x80325810] = ("GXSetIndTexMtx", "gx", [(_GX_0201, 0x80325810, 0x80325970)], "ada8314d444547909cdf40932c17032bbd50370030ef3e0f984f11775188af79")
ENTRIES[0x802D85F8] = ("J3DGDSetFog", "gx", [(_GX_0181, 0x802D85F8, 0x802D895C), _GX_CVT_FP2UNSIGNED], "ee593f386fe20a3d20327fce7d2d6e480ffdd71496cfdc6a077706b931f72c41")
ENTRIES[0x802D80D0] = ("J3DGDSetTevOrder", "gx", [(_GX_0181, 0x802D80D0, 0x802D825C)], "3792f349405630690fdae3c9ae75619710dc6f9001fbb1eb7e4c247ffedb6283")
ENTRIES[0x802AFDDC] = ("GFSetTevColor", "gx", [(_GX_0171, 0x802AFDDC, 0x802AFE38)], "42e830e45ac4512d6236f9f18edb3dabfb1e26fac22aa632b9d5c6369b4ae8a7")
ENTRIES[0x802AFE38] = ("GFSetTevColorS10", "gx", [(_GX_0171, 0x802AFE38, 0x802AFEA0)], "1644d8fa780ee26d666077668b009e40284e94d6d9d21abf8ac525ed8ea2a8df")
ENTRIES[0x802AFBD4] = ("GFSetFog", "gx", [(_GX_0171, 0x802AFBD4, 0x802AFD3C), _GX_CVT_FP2UNSIGNED], "0f47ab2acb94e0b98afcd8e28a845f02ae57b8fb6acadae3d9c19eb83218660d")
# --- end of the fifth set's entries ---
# prepare_native_j3d.py's hooks, in the J3DGetTranslateRotateMtx fragments
# (it runs first): not part of the translation certified here.
J3D_HOOK = re.compile(r"    /\* bluewake: recovered J3D matrix [0-9A-F]{8} \*/\n"
                      r"    if \(bluewake_native_j3d_enabled && bluewake_native_j3d_transform\(ctx, 0x[0-9A-F]{8}u\)\)\n"
                      r"        goto return_dispatch_[0-9A-F]{8};\n")

LABEL = re.compile(r"\n(?:label_([0-9A-F]{8})|return_dispatch_[0-9A-F]{8}):")
FAST_JUMP = re.compile(r"\bgoto (bwfast_\d+);")


def hook_text(entry, chunk):
    _, group, _, _ = ENTRIES[entry]
    _, flag, call = GROUPS[group]
    return (f"    if ({flag} && {call.format(entry=entry)})\n"
            f"        goto return_dispatch_{chunk:08X};\n")


def strip_hooks(text):
    for entry, (_, _, fragments, _) in ENTRIES.items():
        text = text.replace(hook_text(entry, fragments[0][0]), "")
    return J3D_HOOK.sub("", text)


def main_function(text, chunk):
    """The chunk's own function (the loops the translator extracted come
    first, with prepaid copies of their own under the same names)."""
    begin = text.find(f"\nvoid func_{chunk:08X}(CPUState* ctx_param) {{\n")
    if begin < 0:
        return None
    end = text.find("\n}\n", begin)
    return text[begin:end + 3] if end >= 0 else None


def fragment(function, start, end):
    """The blocks from label_START up to the first label at END or beyond (or
    the chunk's return dispatch), and the prepaid copies they jump to."""
    begin = function.find(f"\nlabel_{start:08X}:\n")
    if begin < 0:
        return None
    finish = None
    for m in LABEL.finditer(function, begin + 1):
        if m.group(1) is None or int(m.group(1), 16) >= end:
            finish = m.start()
            break
    if finish is None:
        return None
    body = function[begin:finish]
    copies = []
    dispatch = function.find("\nreturn_dispatch_")
    for name in FAST_JUMP.findall(body):
        at = function.find(f"\n{name}:\n", dispatch)
        if at < 0:
            return None
        stop = function.find("\nbwfast_", at + 1)
        if stop < 0:
            stop = function.rfind("\n}\n")
        copies.append(function[at:stop])
    return body, copies


def canonical(text):
    return " ".join(strip_hooks(text).split())


def addresses(text):
    found = set()
    for m in re.finditer(r"label_([8C][0-9A-F]{7})|// ([8C][0-9A-F]{7}):|ctx->(?:pc|lr) = 0x([8C][0-9A-F]{7})u",
                         text):
        found.add(int(next(v for v in m.groups() if v), 16))
    return found


# --- The third set: addresses on the watch list its hooks do not run past ---
# The host names cTgIt_JudgeFilter's entry (0x80245640), the bctrl in
# cNdIt_Judge that calls it (0x80244F84) and that call's return (0x80244F88)
# for its own actor-search native (runtime/host/src/main.c,
# BW_SEARCH_JUDGE_FILTER, BW_SEARCH_NDIT_RETURN), so all three are on the
# watch list. None is a boundary the third set's hooks run past: the
# JudgeFilter hook sits at the entry, which is reached only by a dispatch
# after the host's edge service (cNdIt_Judge's bctrl goes round the loop: its
# return is watched) or by a goto inside the chunk, as the translation reaches
# it; its call ends at its blr, whose return to cNdIt_Judge goes through the
# chunk's own return dispatch as before; 0x80244F84 is the middle of a block.
# The cNdIt_Judge fragment is hashed for the walk native, which only the host
# calls, at that boundary. And fopAcM_findObjectCB calls OSPanic (0x80006C4C,
# which the host reports) only on a NULL search parameter, where the native
# declines: the translation it stands in for never reaches it. So in those
# fragments - and only there - those addresses are left out of the check.
SEARCH_HOST_OWN = {
    "\nlabel_80245640:\n": {0x80245640, 0x80244F84, 0x80244F88},  # cTgIt_JudgeFilter
    "\nlabel_80244F78:\n": {0x80245640, 0x80244F84, 0x80244F88},  # cNdIt_Judge's loop
    "\nlabel_8002833C:\n": {0x80006C4C},                          # fopAcM_findObjectCB
}
_addresses_of = addresses


def addresses(text):  # noqa: F811 (the check above, less what SEARCH_HOST_OWN leaves out)
    found = _addresses_of(text)
    for start, own in SEARCH_HOST_OWN.items():
        if text.startswith(start):
            found -= own
    return found
# --- end of the third set's exemption ---


# --- The fourth set: mods' variants of a chunk ---
# A mod's variant of a chunk (scripts/mods/build_mod_variants.py) names its
# function func_XXXXXXXX__mod_NAME. The second and third sets' chunks have no
# variants; the fourth set's kankyo chunk (8018D6E0) has two (the widescreen
# mods), whose fragments must hash the same as the base's to be hooked.
_main_function_of = main_function


def main_function(text, chunk):  # noqa: F811 (the base's function, or a mod variant's)
    found = _main_function_of(text, chunk)
    if found is not None:
        return found
    m = re.search(rf"\nvoid func_{chunk:08X}__mod_\w+\(CPUState\* ctx_param\) {{\n", text)
    if m is None:
        return None
    end = text.find("\n}\n", m.start())
    return text[m.start():end + 3] if end >= 0 else None
# --- end of the fourth set's mod variants ---


# --- The fourth set: OSPanic on cM3dGCyl::SetC's assert paths ---
# SetC's asserts (a NaN component, or one outside +-1e32) call JUTAssertion
# and OSPanic (0x80006C4C, which the host reports, so it is on the watch
# list). The native declines on both, so the translation it stands in for
# never reaches OSPanic: in that fragment - and only there - it is left out
# of the check, as the third set leaves it out of fopAcM_findObjectCB's.
FOURTH_HOST_OWN = {
    "\nlabel_80251D88:\n": {0x80006C4C},  # cM3dGCyl::SetC
}
_addresses_third = addresses


def addresses(text):  # noqa: F811 (the checks above, less what FOURTH_HOST_OWN leaves out)
    found = _addresses_third(text)
    for start, own in FOURTH_HOST_OWN.items():
        if text.startswith(start):
            found -= own
    return found
# --- end of the fourth set's exemption ---


# --- The fifth set: watched GX entries ---
# The host names GXBegin's, GXLoadTexObj's and GXCallDisplayList's entries
# (and their ends, and __GXSetDirtyState's) for a diagnostic trace of the pcs
# its loop dispatches (runtime/host/src/main.c, g_gx_entry_trace), so all of
# them are on the watch list, and every call into them from another chunk goes
# round the chassis loop and asks the host's edge service (direct_calls.py
# never rewrites such a call, and bw_call_translated declines it). None is a
# boundary these hooks run past: each hook sits at its function's entry label,
# which is reached only by a dispatch after that service has had its say or by
# a goto inside the chunk, as the translation reaches it, and its call ends at
# the function's blr, whose return goes through the chunk's own return
# dispatch as before. Inside, GXCallDisplayList calls __GXSetDirtyState
# (0x80323024) and __GXSendFlushPrim (0x803231B4), and GXBegin calls
# __GXSendFlushPrim with a goto inside its chunk; on each of those paths the
# native declines before the call, so the translation it stands in for never
# reaches them. So in those fragments - and only there - those addresses are
# left out of the check. (Since the trace's ranges went between
# bluewake-unwatched markers, none of these addresses is watched any more and
# this exemption changes nothing; calls into them are direct calls now, which
# changed GXCallDisplayList's translation - its call to __GXSetDirtyState - and
# so its hash, not its native: native_gx_gen.py writes the same code from the
# new text. The declines stay: __GXSendFlushPrim's loop is extracted, which the
# generator does not replay, and GXCallDisplayList's dirty path is left to
# the translation, as GXBegin's is.)
FIFTH_HOST_OWN = {
    "\nlabel_803230C4:\n": {0x803230C4, 0x803231B4},              # GXBegin
    "\nlabel_80326B80:\n": {0x80326B80, 0x80323024, 0x803231B4},  # GXCallDisplayList
    "\nlabel_80324EE8:\n": {0x80324EE8},                          # GXLoadTexObj
}
_addresses_fourth = addresses


def addresses(text):  # noqa: F811 (the checks above, less what FIFTH_HOST_OWN leaves out)
    found = _addresses_fourth(text)
    for start, own in FIFTH_HOST_OWN.items():
        if text.startswith(start):
            found -= own
    return found
# --- end of the fifth set's exemption ---


def chunk_files(root, chunk):
    return sorted(p for p in root.glob("chunks_*/*.c") if p.name.endswith(f"_{chunk:08X}.c"))


def entry_hash(texts, entry):
    """The entry's hash over one translation (texts: chunk start -> text), and
    the guest addresses its fragments name."""
    _, _, fragments, _ = ENTRIES[entry]
    pieces, named = [], set()
    for chunk, start, end in fragments:
        function = main_function(texts[chunk], chunk)
        piece = fragment(function, start, end) if function else None
        if piece is None:
            return None, named
        body, copies = piece
        pieces.append(canonical(body))
        pieces += [canonical(copy) for copy in copies]
        # The inline register save and restore's fallback (the routine itself,
        # 80328Fxx, through the chassis loop) runs only when the turn's budget
        # is too short for the inline form, which the natives decline: the
        # translation they replace never reaches it.
        named |= addresses(re.sub(r"ctx->pc = 0x80328F[0-9A-F]{2}u;", "", strip_hooks(body)))
    return hashlib.sha256("\n".join(pieces).encode()).hexdigest(), named


def translations(root, entry):
    """Every translation of the entry: the base chunks, and each mod variant of
    any of them with the base's other chunks."""
    _, _, fragments, _ = ENTRIES[entry]
    chunks = sorted({chunk for chunk, _, _ in fragments})
    files = {chunk: chunk_files(root, chunk) for chunk in chunks}
    if any(not paths for paths in files.values()):
        return None
    base = {chunk: next((p for p in paths if p.parent.name == "chunks_dol"), paths[0])
            for chunk, paths in files.items()}
    sets = [dict(base)]
    for chunk, paths in files.items():
        for path in paths:
            if path != base[chunk]:
                variant = dict(base)
                variant[chunk] = path
                sets.append(variant)
    return sets


# --- The sixth set (tests/native_draw_test.c): the particle draw code and the
# sea's waves (cmake/composite/native_draw.c, each its translation
# replayed with stops: native_draw_gen.inc). An entry's hash covers every
# function its native replays. Besides each function's entry, its resumes
# (the return addresses of the calls it stops at) are entries too, with the
# same fragments and hash. ---
GROUPS["draw"] = ("native_draw.h", "bluewake_native_draw_enabled", "bluewake_native_draw_{entry:08X}(ctx)")
# A draw native returns 1 where it ran to its function's blr (on through the
# return dispatch, as the other sets' hooks go), 2 where it stopped just
# before an instruction it leaves to the translation (pc): the block is
# prepaid there (a stop at a block's leader is before the block is entered,
# and the leader sets the flag itself), and the chunk's pc table goes on at
# that instruction.
DRAW_HOOK = ("    if (bluewake_native_draw_enabled) {{\n"
             "        const int bw_native_run = bluewake_native_draw_{entry:08X}(ctx);\n"
             "        if (bw_native_run == 1)\n"
             "            goto return_dispatch_{chunk:08X};\n"
             "        if (bw_native_run == 2) {{\n"
             "            cycle_block_prepaid = true;\n"
             "            goto *pc_table_{chunk:08X}[(ctx->pc - 0x{chunk:08X}u) >> 2];\n"
             "        }}\n"
             "    }}\n")
_hook_text_fifth = hook_text


def hook_text(entry, chunk):  # noqa: F811 (the draw group's form, the others' as before)
    if ENTRIES[entry][1] == "draw":
        return DRAW_HOOK.format(entry=entry, chunk=chunk)
    return _hook_text_fifth(entry, chunk)


# The watched-address check guards boundaries a native might run past without
# the host. A draw native asks at every boundary it crosses between chunks,
# at run time, the edge filter's own question (native_gx_run.h's gx_silent:
# the host quiet and the address not watched, from the same watch list), and
# stops or declines where the host would be asked; a transfer inside a chunk
# asks the host in neither the translation nor the native; and its hooks sit
# at labels the translation reaches only by its own transfers or after the
# host's edge service (a watched entry such as a tagged particle draw's is
# reached through the chassis, its tags written first). So the static check
# has nothing to add for this group.
_entry_hash_fifth = entry_hash


def entry_hash(texts, entry):  # noqa: F811
    digest, named = _entry_hash_fifth(texts, entry)
    if ENTRIES[entry][1] == "draw":
        named = set()
    return digest, named


# --- The sixth set's entries (scripts/windows/native_draw_gen.py) ---
_DRAW_80260D24 = [(0x8025D6E0, 0x80260D24, 0x80260F2C), (0x8030D6E0, 0x8030DA44, 0x8030DA98), (0x8030D6E0, 0x8030DA98, 0x8030DB24), (0x8030D6E0, 0x8030DB24, 0x8030DB78), (0x803096E0, 0x8030D0FC, 0x8030D1C8), (0x803216E0, 0x803230C4, 0x803231B4), (0x8025D6E0, 0x802614A8, 0x8026168C), (0x8025D6E0, 0x8026134C, 0x802614A8), (0x8025D6E0, 0x8025DCDC, 0x8025DD5C), (0x802556E0, 0x80257550, 0x80257988), (0x802556E0, 0x80257B4C, 0x80257D4C), (0x803256E0, 0x80325FA8, 0x8032601C)]
ENTRIES[0x80260D24] = ("JPADrawExecRotBillBoard::exec", "draw", _DRAW_80260D24, "7d767a27fcc5f5b117d6da9eb8f0fbe1da15a63d1f31223a8549f59b92d6eb2b")
ENTRIES[0x80260DF8] = ("JPADrawExecRotBillBoard::exec@80260DF8", "draw", _DRAW_80260D24, "7d767a27fcc5f5b117d6da9eb8f0fbe1da15a63d1f31223a8549f59b92d6eb2b")
ENTRIES[0x80260E08] = ("JPADrawExecRotBillBoard::exec@80260E08", "draw", _DRAW_80260D24, "7d767a27fcc5f5b117d6da9eb8f0fbe1da15a63d1f31223a8549f59b92d6eb2b")
_DRAW_80260BAC = [(0x8025D6E0, 0x80260BAC, 0x80260D24), (0x8030D6E0, 0x8030DA44, 0x8030DA98), (0x8030D6E0, 0x8030DA98, 0x8030DB24), (0x8030D6E0, 0x8030DB24, 0x8030DB78), (0x803096E0, 0x8030D0FC, 0x8030D1C8), (0x803216E0, 0x803230C4, 0x803231B4), (0x8025D6E0, 0x802614A8, 0x8026168C), (0x8025D6E0, 0x8026134C, 0x802614A8), (0x8025D6E0, 0x8025DCDC, 0x8025DD5C), (0x802556E0, 0x80257550, 0x80257988), (0x802556E0, 0x80257B4C, 0x80257D4C), (0x803256E0, 0x80325FA8, 0x8032601C)]
ENTRIES[0x80260BAC] = ("JPADrawExecBillBoard::exec", "draw", _DRAW_80260BAC, "44d5298ce2384b558e716b6e87cc5a4e94c37f90bb2e55dd53621fe965fd110e")
ENTRIES[0x80260C48] = ("JPADrawExecBillBoard::exec@80260C48", "draw", _DRAW_80260BAC, "44d5298ce2384b558e716b6e87cc5a4e94c37f90bb2e55dd53621fe965fd110e")
ENTRIES[0x80260C58] = ("JPADrawExecBillBoard::exec@80260C58", "draw", _DRAW_80260BAC, "44d5298ce2384b558e716b6e87cc5a4e94c37f90bb2e55dd53621fe965fd110e")
_DRAW_80260F2C = [(0x8025D6E0, 0x80260F2C, 0x8026110C), (0x8030D6E0, 0x8030DA44, 0x8030DA98), (0x8030D6E0, 0x8030DA98, 0x8030DB24), (0x8030D6E0, 0x8030DB24, 0x8030DB78), (0x803096E0, 0x8030D0FC, 0x8030D1C8), (0x803216E0, 0x803230C4, 0x803231B4), (0x8025D6E0, 0x802614A8, 0x8026168C), (0x8025D6E0, 0x8026134C, 0x802614A8), (0x8025D6E0, 0x8025DCDC, 0x8025DD5C), (0x802556E0, 0x80257550, 0x80257988), (0x802556E0, 0x80257B4C, 0x80257D4C), (0x803256E0, 0x80325FA8, 0x8032601C)]
ENTRIES[0x80260F2C] = ("JPADrawExecYBillBoard::exec", "draw", _DRAW_80260F2C, "e364377abf81beef0fd4c76153984d490fb18e72b4ce004bb6cb298f7eb54040")
ENTRIES[0x80260FD8] = ("JPADrawExecYBillBoard::exec@80260FD8", "draw", _DRAW_80260F2C, "e364377abf81beef0fd4c76153984d490fb18e72b4ce004bb6cb298f7eb54040")
ENTRIES[0x80261004] = ("JPADrawExecYBillBoard::exec@80261004", "draw", _DRAW_80260F2C, "e364377abf81beef0fd4c76153984d490fb18e72b4ce004bb6cb298f7eb54040")
ENTRIES[0x80261014] = ("JPADrawExecYBillBoard::exec@80261014", "draw", _DRAW_80260F2C, "e364377abf81beef0fd4c76153984d490fb18e72b4ce004bb6cb298f7eb54040")
_DRAW_8026110C = [(0x8025D6E0, 0x8026110C, 0x8026134C), (0x8030D6E0, 0x8030DA44, 0x8030DA98), (0x8030D6E0, 0x8030DA98, 0x8030DB24), (0x8030D6E0, 0x8030DB24, 0x8030DB78), (0x803096E0, 0x8030D0FC, 0x8030D1C8), (0x803216E0, 0x803230C4, 0x803231B4), (0x8025D6E0, 0x802614A8, 0x8026168C), (0x8025D6E0, 0x8026134C, 0x802614A8), (0x8025D6E0, 0x8025DCDC, 0x8025DD5C), (0x802556E0, 0x80257550, 0x80257988), (0x802556E0, 0x80257B4C, 0x80257D4C), (0x803256E0, 0x80325FA8, 0x8032601C)]
ENTRIES[0x8026110C] = ("JPADrawExecRotYBillBoard::exec", "draw", _DRAW_8026110C, "cc82f356507bd6ae97f982c38bdfb52c798c98e43ee9c258bf90f69011bb0c84")
ENTRIES[0x80261218] = ("JPADrawExecRotYBillBoard::exec@80261218", "draw", _DRAW_8026110C, "cc82f356507bd6ae97f982c38bdfb52c798c98e43ee9c258bf90f69011bb0c84")
ENTRIES[0x80261244] = ("JPADrawExecRotYBillBoard::exec@80261244", "draw", _DRAW_8026110C, "cc82f356507bd6ae97f982c38bdfb52c798c98e43ee9c258bf90f69011bb0c84")
ENTRIES[0x80261254] = ("JPADrawExecRotYBillBoard::exec@80261254", "draw", _DRAW_8026110C, "cc82f356507bd6ae97f982c38bdfb52c798c98e43ee9c258bf90f69011bb0c84")
_DRAW_80261AD0 = [(0x802616E0, 0x80261AD0, 0x80261F60), (0x8030D6E0, 0x8030DA44, 0x8030DA98), (0x8030D6E0, 0x8030DA98, 0x8030DB24), (0x8030D6E0, 0x8030DB24, 0x8030DB78), (0x803096E0, 0x8030D0FC, 0x8030D1C8), (0x803216E0, 0x803230C4, 0x803231B4), (0x8025D6E0, 0x802614A8, 0x8026168C), (0x8025D6E0, 0x8026134C, 0x802614A8), (0x8025D6E0, 0x8025DCDC, 0x8025DD5C), (0x802556E0, 0x80257550, 0x80257988), (0x802556E0, 0x80257B4C, 0x80257D4C), (0x803256E0, 0x80325FA8, 0x8032601C)]
ENTRIES[0x80261AD0] = ("JPADrawExecRotDirectional::exec", "draw", _DRAW_80261AD0, "14b3ade8dc17a23db23612ddaf908c267df115e96454bcd2f205b1393505c35b")
ENTRIES[0x80261B84] = ("JPADrawExecRotDirectional::exec@80261B84", "draw", _DRAW_80261AD0, "14b3ade8dc17a23db23612ddaf908c267df115e96454bcd2f205b1393505c35b")
ENTRIES[0x80261BA0] = ("JPADrawExecRotDirectional::exec@80261BA0", "draw", _DRAW_80261AD0, "14b3ade8dc17a23db23612ddaf908c267df115e96454bcd2f205b1393505c35b")
ENTRIES[0x80261BBC] = ("JPADrawExecRotDirectional::exec@80261BBC", "draw", _DRAW_80261AD0, "14b3ade8dc17a23db23612ddaf908c267df115e96454bcd2f205b1393505c35b")
ENTRIES[0x80261E28] = ("JPADrawExecRotDirectional::exec@80261E28", "draw", _DRAW_80261AD0, "14b3ade8dc17a23db23612ddaf908c267df115e96454bcd2f205b1393505c35b")
ENTRIES[0x80261E3C] = ("JPADrawExecRotDirectional::exec@80261E3C", "draw", _DRAW_80261AD0, "14b3ade8dc17a23db23612ddaf908c267df115e96454bcd2f205b1393505c35b")
ENTRIES[0x80261E58] = ("JPADrawExecRotDirectional::exec@80261E58", "draw", _DRAW_80261AD0, "14b3ade8dc17a23db23612ddaf908c267df115e96454bcd2f205b1393505c35b")
_DRAW_80261F60 = [(0x802616E0, 0x80261F60, 0x802624D4), (0x8030D6E0, 0x8030DA44, 0x8030DA98), (0x8030D6E0, 0x8030DA98, 0x8030DB24), (0x8030D6E0, 0x8030DB24, 0x8030DB78), (0x803096E0, 0x8030D0FC, 0x8030D1C8), (0x803216E0, 0x803230C4, 0x803231B4), (0x8025D6E0, 0x802614A8, 0x8026168C), (0x8025D6E0, 0x8026134C, 0x802614A8), (0x8025D6E0, 0x8025DCDC, 0x8025DD5C), (0x802556E0, 0x80257550, 0x80257988), (0x802556E0, 0x80257B4C, 0x80257D4C), (0x803256E0, 0x80325FA8, 0x8032601C)]
ENTRIES[0x80261F60] = ("JPADrawExecDirectionalCross::exec", "draw", _DRAW_80261F60, "4db117f9a4955c7f69cdf790964c44d7f991cfd0c0957dbab948abd7e6d49208")
ENTRIES[0x80262074] = ("JPADrawExecDirectionalCross::exec@80262074", "draw", _DRAW_80261F60, "4db117f9a4955c7f69cdf790964c44d7f991cfd0c0957dbab948abd7e6d49208")
ENTRIES[0x802622E4] = ("JPADrawExecDirectionalCross::exec@802622E4", "draw", _DRAW_80261F60, "4db117f9a4955c7f69cdf790964c44d7f991cfd0c0957dbab948abd7e6d49208")
ENTRIES[0x80262300] = ("JPADrawExecDirectionalCross::exec@80262300", "draw", _DRAW_80261F60, "4db117f9a4955c7f69cdf790964c44d7f991cfd0c0957dbab948abd7e6d49208")
_DRAW_802624D4 = [(0x802616E0, 0x802624D4, 0x80262A98), (0x8030D6E0, 0x8030DA44, 0x8030DA98), (0x8030D6E0, 0x8030DA98, 0x8030DB24), (0x8030D6E0, 0x8030DB24, 0x8030DB78), (0x803096E0, 0x8030D0FC, 0x8030D1C8), (0x803216E0, 0x803230C4, 0x803231B4), (0x8025D6E0, 0x802614A8, 0x8026168C), (0x8025D6E0, 0x8026134C, 0x802614A8), (0x8025D6E0, 0x8025DCDC, 0x8025DD5C), (0x802556E0, 0x80257550, 0x80257988), (0x802556E0, 0x80257B4C, 0x80257D4C), (0x803256E0, 0x80325FA8, 0x8032601C)]
ENTRIES[0x802624D4] = ("JPADrawExecRotDirectionalCross::exec", "draw", _DRAW_802624D4, "10303d3d5bf1d9b471bc78ab4086721278ff0703c394ca16915399dd1d02ab72")
ENTRIES[0x80262608] = ("JPADrawExecRotDirectionalCross::exec@80262608", "draw", _DRAW_802624D4, "10303d3d5bf1d9b471bc78ab4086721278ff0703c394ca16915399dd1d02ab72")
ENTRIES[0x80262624] = ("JPADrawExecRotDirectionalCross::exec@80262624", "draw", _DRAW_802624D4, "10303d3d5bf1d9b471bc78ab4086721278ff0703c394ca16915399dd1d02ab72")
ENTRIES[0x80262890] = ("JPADrawExecRotDirectionalCross::exec@80262890", "draw", _DRAW_802624D4, "10303d3d5bf1d9b471bc78ab4086721278ff0703c394ca16915399dd1d02ab72")
ENTRIES[0x802628A4] = ("JPADrawExecRotDirectionalCross::exec@802628A4", "draw", _DRAW_802624D4, "10303d3d5bf1d9b471bc78ab4086721278ff0703c394ca16915399dd1d02ab72")
ENTRIES[0x802628C0] = ("JPADrawExecRotDirectionalCross::exec@802628C0", "draw", _DRAW_802624D4, "10303d3d5bf1d9b471bc78ab4086721278ff0703c394ca16915399dd1d02ab72")
_DRAW_80262A98 = [(0x802616E0, 0x80262A98, 0x80262DC0), (0x8030D6E0, 0x8030DA44, 0x8030DA98), (0x8030D6E0, 0x8030DA98, 0x8030DB24), (0x8030D6E0, 0x8030DB24, 0x8030DB78), (0x803096E0, 0x8030D0FC, 0x8030D1C8), (0x803216E0, 0x803230C4, 0x803231B4), (0x8025D6E0, 0x802614A8, 0x8026168C), (0x8025D6E0, 0x8026134C, 0x802614A8), (0x8025D6E0, 0x8025DCDC, 0x8025DD5C), (0x802556E0, 0x80257550, 0x80257988), (0x802556E0, 0x80257B4C, 0x80257D4C), (0x803256E0, 0x80325FA8, 0x8032601C)]
ENTRIES[0x80262A98] = ("JPADrawExecDirBillBoard::exec", "draw", _DRAW_80262A98, "30ce9338addf15bed675940b3e5955c65bb25db3c5b1a06025888c07fea71bdd")
ENTRIES[0x80262B14] = ("JPADrawExecDirBillBoard::exec@80262B14", "draw", _DRAW_80262A98, "30ce9338addf15bed675940b3e5955c65bb25db3c5b1a06025888c07fea71bdd")
ENTRIES[0x80262C04] = ("JPADrawExecDirBillBoard::exec@80262C04", "draw", _DRAW_80262A98, "30ce9338addf15bed675940b3e5955c65bb25db3c5b1a06025888c07fea71bdd")
ENTRIES[0x80262CB8] = ("JPADrawExecDirBillBoard::exec@80262CB8", "draw", _DRAW_80262A98, "30ce9338addf15bed675940b3e5955c65bb25db3c5b1a06025888c07fea71bdd")
ENTRIES[0x80262CC8] = ("JPADrawExecDirBillBoard::exec@80262CC8", "draw", _DRAW_80262A98, "30ce9338addf15bed675940b3e5955c65bb25db3c5b1a06025888c07fea71bdd")
_DRAW_80262DC0 = [(0x802616E0, 0x80262DC0, 0x80262FBC), (0x8030D6E0, 0x8030DA44, 0x8030DA98), (0x8030D6E0, 0x8030DA98, 0x8030DB24), (0x8030D6E0, 0x8030DB24, 0x8030DB78), (0x803096E0, 0x8030D0FC, 0x8030D1C8), (0x803216E0, 0x803230C4, 0x803231B4), (0x8025D6E0, 0x802614A8, 0x8026168C), (0x8025D6E0, 0x8026134C, 0x802614A8), (0x8025D6E0, 0x8025DCDC, 0x8025DD5C), (0x802556E0, 0x80257550, 0x80257988), (0x802556E0, 0x80257B4C, 0x80257D4C), (0x803256E0, 0x80325FA8, 0x8032601C)]
ENTRIES[0x80262DC0] = ("JPADrawExecRotation::exec", "draw", _DRAW_80262DC0, "cd43c1cec7889d3fc7de9d57478b3bfb7957aa91739b059a2401c14288cda4c4")
ENTRIES[0x80262E6C] = ("JPADrawExecRotation::exec@80262E6C", "draw", _DRAW_80262DC0, "cd43c1cec7889d3fc7de9d57478b3bfb7957aa91739b059a2401c14288cda4c4")
ENTRIES[0x80262E88] = ("JPADrawExecRotation::exec@80262E88", "draw", _DRAW_80262DC0, "cd43c1cec7889d3fc7de9d57478b3bfb7957aa91739b059a2401c14288cda4c4")
ENTRIES[0x80262E9C] = ("JPADrawExecRotation::exec@80262E9C", "draw", _DRAW_80262DC0, "cd43c1cec7889d3fc7de9d57478b3bfb7957aa91739b059a2401c14288cda4c4")
ENTRIES[0x80262EB8] = ("JPADrawExecRotation::exec@80262EB8", "draw", _DRAW_80262DC0, "cd43c1cec7889d3fc7de9d57478b3bfb7957aa91739b059a2401c14288cda4c4")
_DRAW_80262FBC = [(0x802616E0, 0x80262FBC, 0x802632EC), (0x8030D6E0, 0x8030DA44, 0x8030DA98), (0x8030D6E0, 0x8030DA98, 0x8030DB24), (0x8030D6E0, 0x8030DB24, 0x8030DB78), (0x803096E0, 0x8030D0FC, 0x8030D1C8), (0x803216E0, 0x803230C4, 0x803231B4), (0x8025D6E0, 0x802614A8, 0x8026168C), (0x8025D6E0, 0x8026134C, 0x802614A8), (0x8025D6E0, 0x8025DCDC, 0x8025DD5C), (0x802556E0, 0x80257550, 0x80257988), (0x802556E0, 0x80257B4C, 0x80257D4C), (0x803256E0, 0x80325FA8, 0x8032601C)]
ENTRIES[0x80262FBC] = ("JPADrawExecRotationCross::exec", "draw", _DRAW_80262FBC, "7958e127058053a98fc0e252baa637c10e5866d545db74cc116b702a1110d87b")
ENTRIES[0x802630E8] = ("JPADrawExecRotationCross::exec@802630E8", "draw", _DRAW_80262FBC, "7958e127058053a98fc0e252baa637c10e5866d545db74cc116b702a1110d87b")
ENTRIES[0x802630FC] = ("JPADrawExecRotationCross::exec@802630FC", "draw", _DRAW_80262FBC, "7958e127058053a98fc0e252baa637c10e5866d545db74cc116b702a1110d87b")
ENTRIES[0x80263118] = ("JPADrawExecRotationCross::exec@80263118", "draw", _DRAW_80262FBC, "7958e127058053a98fc0e252baa637c10e5866d545db74cc116b702a1110d87b")
_DRAW_802632EC = [(0x802616E0, 0x802632EC, 0x80263380), (0x8030D6E0, 0x8030DA44, 0x8030DA98), (0x8030D6E0, 0x8030DA98, 0x8030DB24), (0x8030D6E0, 0x8030DB24, 0x8030DB78), (0x803096E0, 0x8030D0FC, 0x8030D1C8), (0x803216E0, 0x803230C4, 0x803231B4), (0x8025D6E0, 0x802614A8, 0x8026168C), (0x8025D6E0, 0x8026134C, 0x802614A8), (0x8025D6E0, 0x8025DCDC, 0x8025DD5C), (0x802556E0, 0x80257550, 0x80257988), (0x802556E0, 0x80257B4C, 0x80257D4C), (0x803256E0, 0x80325FA8, 0x8032601C)]
ENTRIES[0x802632EC] = ("JPADrawExecPoint::exec", "draw", _DRAW_802632EC, "f425a36754e00dbc913d2443bd2a0afca5b628080fa18c1dccfb4de92fce0da6")
ENTRIES[0x80263338] = ("JPADrawExecPoint::exec@80263338", "draw", _DRAW_802632EC, "f425a36754e00dbc913d2443bd2a0afca5b628080fa18c1dccfb4de92fce0da6")
_DRAW_80263380 = [(0x802616E0, 0x80263380, 0x80263508), (0x8030D6E0, 0x8030DA44, 0x8030DA98), (0x8030D6E0, 0x8030DA98, 0x8030DB24), (0x8030D6E0, 0x8030DB24, 0x8030DB78), (0x803096E0, 0x8030D0FC, 0x8030D1C8), (0x803216E0, 0x803230C4, 0x803231B4), (0x8025D6E0, 0x802614A8, 0x8026168C), (0x8025D6E0, 0x8026134C, 0x802614A8), (0x8025D6E0, 0x8025DCDC, 0x8025DD5C), (0x802556E0, 0x80257550, 0x80257988), (0x802556E0, 0x80257B4C, 0x80257D4C), (0x803256E0, 0x80325FA8, 0x8032601C)]
ENTRIES[0x80263380] = ("JPADrawExecLine::exec", "draw", _DRAW_80263380, "c7b37d8a2c743630dad68024ab5403c682f7110944660434b5709f8665fc3fa7")
ENTRIES[0x8026348C] = ("JPADrawExecLine::exec@8026348C", "draw", _DRAW_80263380, "c7b37d8a2c743630dad68024ab5403c682f7110944660434b5709f8665fc3fa7")
_DRAW_80263518 = [(0x802616E0, 0x80263518, 0x80263A68), (0x802616E0, 0x80263508, 0x80263518), (0x8025D6E0, 0x802614A8, 0x8026168C), (0x8025D6E0, 0x8026134C, 0x802614A8), (0x8025D6E0, 0x8025DCDC, 0x8025DD5C), (0x803216E0, 0x803230C4, 0x803231B4), (0x802556E0, 0x80257550, 0x80257988), (0x802556E0, 0x80257B4C, 0x80257D4C)]
ENTRIES[0x80263518] = ("JPADrawExecStripe::exec", "draw", _DRAW_80263518, "1ab21447e4f0a50ecc3b7ac7baee45f2a26d36f30fe65a87e68db578a9d2abac")
ENTRIES[0x80263584] = ("JPADrawExecStripe::exec@80263584", "draw", _DRAW_80263518, "1ab21447e4f0a50ecc3b7ac7baee45f2a26d36f30fe65a87e68db578a9d2abac")
ENTRIES[0x802635D4] = ("JPADrawExecStripe::exec@802635D4", "draw", _DRAW_80263518, "1ab21447e4f0a50ecc3b7ac7baee45f2a26d36f30fe65a87e68db578a9d2abac")
ENTRIES[0x80263620] = ("JPADrawExecStripe::exec@80263620", "draw", _DRAW_80263518, "1ab21447e4f0a50ecc3b7ac7baee45f2a26d36f30fe65a87e68db578a9d2abac")
ENTRIES[0x802636B0] = ("JPADrawExecStripe::exec@802636B0", "draw", _DRAW_80263518, "1ab21447e4f0a50ecc3b7ac7baee45f2a26d36f30fe65a87e68db578a9d2abac")
ENTRIES[0x802639E8] = ("JPADrawExecStripe::exec@802639E8", "draw", _DRAW_80263518, "1ab21447e4f0a50ecc3b7ac7baee45f2a26d36f30fe65a87e68db578a9d2abac")
ENTRIES[0x80263A58] = ("JPADrawExecStripe::exec@80263A58", "draw", _DRAW_80263518, "1ab21447e4f0a50ecc3b7ac7baee45f2a26d36f30fe65a87e68db578a9d2abac")
_DRAW_80263A68 = [(0x802616E0, 0x80263A68, 0x802643B0), (0x802616E0, 0x80263508, 0x80263518), (0x8025D6E0, 0x802614A8, 0x8026168C), (0x8025D6E0, 0x8026134C, 0x802614A8), (0x8025D6E0, 0x8025DCDC, 0x8025DD5C), (0x803216E0, 0x803230C4, 0x803231B4), (0x802556E0, 0x80257550, 0x80257988), (0x802556E0, 0x80257B4C, 0x80257D4C)]
ENTRIES[0x80263A68] = ("JPADrawExecStripeCross::exec", "draw", _DRAW_80263A68, "3787968c65088a0a733ff6c52c101828b9cdc47d76412e7c363af448419be196")
ENTRIES[0x80263ADC] = ("JPADrawExecStripeCross::exec@80263ADC", "draw", _DRAW_80263A68, "3787968c65088a0a733ff6c52c101828b9cdc47d76412e7c363af448419be196")
ENTRIES[0x80263B28] = ("JPADrawExecStripeCross::exec@80263B28", "draw", _DRAW_80263A68, "3787968c65088a0a733ff6c52c101828b9cdc47d76412e7c363af448419be196")
ENTRIES[0x80263B84] = ("JPADrawExecStripeCross::exec@80263B84", "draw", _DRAW_80263A68, "3787968c65088a0a733ff6c52c101828b9cdc47d76412e7c363af448419be196")
ENTRIES[0x80263C14] = ("JPADrawExecStripeCross::exec@80263C14", "draw", _DRAW_80263A68, "3787968c65088a0a733ff6c52c101828b9cdc47d76412e7c363af448419be196")
ENTRIES[0x80263F4C] = ("JPADrawExecStripeCross::exec@80263F4C", "draw", _DRAW_80263A68, "3787968c65088a0a733ff6c52c101828b9cdc47d76412e7c363af448419be196")
ENTRIES[0x80263F70] = ("JPADrawExecStripeCross::exec@80263F70", "draw", _DRAW_80263A68, "3787968c65088a0a733ff6c52c101828b9cdc47d76412e7c363af448419be196")
ENTRIES[0x8026432C] = ("JPADrawExecStripeCross::exec@8026432C", "draw", _DRAW_80263A68, "3787968c65088a0a733ff6c52c101828b9cdc47d76412e7c363af448419be196")
ENTRIES[0x802643A0] = ("JPADrawExecStripeCross::exec@802643A0", "draw", _DRAW_80263A68, "3787968c65088a0a733ff6c52c101828b9cdc47d76412e7c363af448419be196")
_DRAW_802605FC = [(0x8025D6E0, 0x802605FC, 0x80260728), (0x8030D6E0, 0x8030DA44, 0x8030DA98), (0x8030D6E0, 0x8030DA98, 0x8030DB24), (0x8030D6E0, 0x8030DB24, 0x8030DB78), (0x803096E0, 0x8030D0FC, 0x8030D1C8), (0x803216E0, 0x803230C4, 0x803231B4), (0x8025D6E0, 0x802614A8, 0x8026168C), (0x8025D6E0, 0x8026134C, 0x802614A8), (0x8025D6E0, 0x8025DCDC, 0x8025DD5C), (0x802556E0, 0x80257550, 0x80257988), (0x802556E0, 0x80257B4C, 0x80257D4C), (0x803256E0, 0x80325FA8, 0x8032601C)]
ENTRIES[0x802605FC] = ("JPADrawExecRegisterPrmCEnv::exec", "draw", _DRAW_802605FC, "59e2c23aba5d9b31ecbd5eb98019cd9f77e8a9e0a90aa0f3ab439a5979fdf15f")
ENTRIES[0x80260704] = ("JPADrawExecRegisterPrmCEnv::exec@80260704", "draw", _DRAW_802605FC, "59e2c23aba5d9b31ecbd5eb98019cd9f77e8a9e0a90aa0f3ab439a5979fdf15f")
ENTRIES[0x80260718] = ("JPADrawExecRegisterPrmCEnv::exec@80260718", "draw", _DRAW_802605FC, "59e2c23aba5d9b31ecbd5eb98019cd9f77e8a9e0a90aa0f3ab439a5979fdf15f")
_DRAW_80260728 = [(0x8025D6E0, 0x80260728, 0x80260858), (0x8030D6E0, 0x8030DA44, 0x8030DA98), (0x8030D6E0, 0x8030DA98, 0x8030DB24), (0x8030D6E0, 0x8030DB24, 0x8030DB78), (0x803096E0, 0x8030D0FC, 0x8030D1C8), (0x803216E0, 0x803230C4, 0x803231B4), (0x8025D6E0, 0x802614A8, 0x8026168C), (0x8025D6E0, 0x8026134C, 0x802614A8), (0x8025D6E0, 0x8025DCDC, 0x8025DD5C), (0x802556E0, 0x80257550, 0x80257988), (0x802556E0, 0x80257B4C, 0x80257D4C), (0x803256E0, 0x80325FA8, 0x8032601C)]
ENTRIES[0x80260728] = ("JPADrawExecRegisterPrmAEnv::exec", "draw", _DRAW_80260728, "e9158223cc0e3bc43e41c0c235ddfb081f8aea2282a76f150c4ba12837e714ad")
ENTRIES[0x80260834] = ("JPADrawExecRegisterPrmAEnv::exec@80260834", "draw", _DRAW_80260728, "e9158223cc0e3bc43e41c0c235ddfb081f8aea2282a76f150c4ba12837e714ad")
ENTRIES[0x80260848] = ("JPADrawExecRegisterPrmAEnv::exec@80260848", "draw", _DRAW_80260728, "e9158223cc0e3bc43e41c0c235ddfb081f8aea2282a76f150c4ba12837e714ad")
_DRAW_802643B0 = [(0x802616E0, 0x802643B0, 0x802644B4), (0x8030D6E0, 0x8030DA44, 0x8030DA98), (0x8030D6E0, 0x8030DA98, 0x8030DB24), (0x8030D6E0, 0x8030DB24, 0x8030DB78), (0x803096E0, 0x8030D0FC, 0x8030D1C8), (0x803216E0, 0x803230C4, 0x803231B4), (0x8025D6E0, 0x802614A8, 0x8026168C), (0x8025D6E0, 0x8026134C, 0x802614A8), (0x8025D6E0, 0x8025DCDC, 0x8025DD5C), (0x802556E0, 0x80257550, 0x80257988), (0x802556E0, 0x80257B4C, 0x80257D4C), (0x803256E0, 0x80325FA8, 0x8032601C)]
ENTRIES[0x802643B0] = ("JPADrawExecRegisterColorEmitterPE::exec", "draw", _DRAW_802643B0, "f0197febe86b9bc9076fe26d6e8a3b4d7dcd6813981e243f3099335677d847e1")
ENTRIES[0x80264490] = ("JPADrawExecRegisterColorEmitterPE::exec@80264490", "draw", _DRAW_802643B0, "f0197febe86b9bc9076fe26d6e8a3b4d7dcd6813981e243f3099335677d847e1")
ENTRIES[0x802644A4] = ("JPADrawExecRegisterColorEmitterPE::exec@802644A4", "draw", _DRAW_802643B0, "f0197febe86b9bc9076fe26d6e8a3b4d7dcd6813981e243f3099335677d847e1")
_DRAW_80264C88 = [(0x802616E0, 0x80264C88, 0x80264DB8), (0x802556E0, 0x80257550, 0x80257988), (0x802556E0, 0x80257B4C, 0x80257D4C)]
ENTRIES[0x80264C88] = ("JPADrawCalcScaleX::calc", "draw", _DRAW_80264C88, "5e71349c93001e18fc8cd5e1ea4de27b733deb7030d8e9480acfcc4e9bfe63d9")
ENTRIES[0x80264CC0] = ("JPADrawCalcScaleX::calc@80264CC0", "draw", _DRAW_80264C88, "5e71349c93001e18fc8cd5e1ea4de27b733deb7030d8e9480acfcc4e9bfe63d9")
ENTRIES[0x80264CE4] = ("JPADrawCalcScaleX::calc@80264CE4", "draw", _DRAW_80264C88, "5e71349c93001e18fc8cd5e1ea4de27b733deb7030d8e9480acfcc4e9bfe63d9")
ENTRIES[0x80264CFC] = ("JPADrawCalcScaleX::calc@80264CFC", "draw", _DRAW_80264C88, "5e71349c93001e18fc8cd5e1ea4de27b733deb7030d8e9480acfcc4e9bfe63d9")
ENTRIES[0x80264D30] = ("JPADrawCalcScaleX::calc@80264D30", "draw", _DRAW_80264C88, "5e71349c93001e18fc8cd5e1ea4de27b733deb7030d8e9480acfcc4e9bfe63d9")
ENTRIES[0x80264D54] = ("JPADrawCalcScaleX::calc@80264D54", "draw", _DRAW_80264C88, "5e71349c93001e18fc8cd5e1ea4de27b733deb7030d8e9480acfcc4e9bfe63d9")
ENTRIES[0x80264D74] = ("JPADrawCalcScaleX::calc@80264D74", "draw", _DRAW_80264C88, "5e71349c93001e18fc8cd5e1ea4de27b733deb7030d8e9480acfcc4e9bfe63d9")
_DRAW_80264DB8 = [(0x802616E0, 0x80264DB8, 0x80264EE8), (0x802556E0, 0x80257550, 0x80257988), (0x802556E0, 0x80257B4C, 0x80257D4C)]
ENTRIES[0x80264DB8] = ("JPADrawCalcScaleY::calc", "draw", _DRAW_80264DB8, "282a0170096fbf0f1894406e753708c5d2d458da2488657031f341990a8635a7")
ENTRIES[0x80264DF0] = ("JPADrawCalcScaleY::calc@80264DF0", "draw", _DRAW_80264DB8, "282a0170096fbf0f1894406e753708c5d2d458da2488657031f341990a8635a7")
ENTRIES[0x80264E14] = ("JPADrawCalcScaleY::calc@80264E14", "draw", _DRAW_80264DB8, "282a0170096fbf0f1894406e753708c5d2d458da2488657031f341990a8635a7")
ENTRIES[0x80264E2C] = ("JPADrawCalcScaleY::calc@80264E2C", "draw", _DRAW_80264DB8, "282a0170096fbf0f1894406e753708c5d2d458da2488657031f341990a8635a7")
ENTRIES[0x80264E60] = ("JPADrawCalcScaleY::calc@80264E60", "draw", _DRAW_80264DB8, "282a0170096fbf0f1894406e753708c5d2d458da2488657031f341990a8635a7")
ENTRIES[0x80264E84] = ("JPADrawCalcScaleY::calc@80264E84", "draw", _DRAW_80264DB8, "282a0170096fbf0f1894406e753708c5d2d458da2488657031f341990a8635a7")
ENTRIES[0x80264EA4] = ("JPADrawCalcScaleY::calc@80264EA4", "draw", _DRAW_80264DB8, "282a0170096fbf0f1894406e753708c5d2d458da2488657031f341990a8635a7")
_DRAW_80264EE8 = [(0x802616E0, 0x80264EE8, 0x802650B8), (0x802556E0, 0x80257550, 0x80257988), (0x802556E0, 0x80257B4C, 0x80257D4C)]
ENTRIES[0x80264EE8] = ("JPADrawCalcScaleXBySpeed::calc", "draw", _DRAW_80264EE8, "2d484cbe736f518e0a1b6051db28b0750401e275dbd28b29f66f62b0c5ab8dfa")
ENTRIES[0x80264F44] = ("JPADrawCalcScaleXBySpeed::calc@80264F44", "draw", _DRAW_80264EE8, "2d484cbe736f518e0a1b6051db28b0750401e275dbd28b29f66f62b0c5ab8dfa")
ENTRIES[0x80264F68] = ("JPADrawCalcScaleXBySpeed::calc@80264F68", "draw", _DRAW_80264EE8, "2d484cbe736f518e0a1b6051db28b0750401e275dbd28b29f66f62b0c5ab8dfa")
ENTRIES[0x80264F80] = ("JPADrawCalcScaleXBySpeed::calc@80264F80", "draw", _DRAW_80264EE8, "2d484cbe736f518e0a1b6051db28b0750401e275dbd28b29f66f62b0c5ab8dfa")
ENTRIES[0x80264FB4] = ("JPADrawCalcScaleXBySpeed::calc@80264FB4", "draw", _DRAW_80264EE8, "2d484cbe736f518e0a1b6051db28b0750401e275dbd28b29f66f62b0c5ab8dfa")
ENTRIES[0x80264FD8] = ("JPADrawCalcScaleXBySpeed::calc@80264FD8", "draw", _DRAW_80264EE8, "2d484cbe736f518e0a1b6051db28b0750401e275dbd28b29f66f62b0c5ab8dfa")
ENTRIES[0x80264FF8] = ("JPADrawCalcScaleXBySpeed::calc@80264FF8", "draw", _DRAW_80264EE8, "2d484cbe736f518e0a1b6051db28b0750401e275dbd28b29f66f62b0c5ab8dfa")
_DRAW_80265B14 = [(0x802656E0, 0x80265B14, 0x80265C40), (0x802556E0, 0x80257550, 0x80257988), (0x802556E0, 0x80257B4C, 0x80257D4C)]
ENTRIES[0x80265B14] = ("JPADrawCalcAlpha::calc", "draw", _DRAW_80265B14, "f6e3b77c00e4f1b77e9f12e9a832630c3885ef6f8c50bde011c484747ded1877")
ENTRIES[0x80265B58] = ("JPADrawCalcAlpha::calc@80265B58", "draw", _DRAW_80265B14, "f6e3b77c00e4f1b77e9f12e9a832630c3885ef6f8c50bde011c484747ded1877")
ENTRIES[0x80265B74] = ("JPADrawCalcAlpha::calc@80265B74", "draw", _DRAW_80265B14, "f6e3b77c00e4f1b77e9f12e9a832630c3885ef6f8c50bde011c484747ded1877")
ENTRIES[0x80265B8C] = ("JPADrawCalcAlpha::calc@80265B8C", "draw", _DRAW_80265B14, "f6e3b77c00e4f1b77e9f12e9a832630c3885ef6f8c50bde011c484747ded1877")
ENTRIES[0x80265BAC] = ("JPADrawCalcAlpha::calc@80265BAC", "draw", _DRAW_80265B14, "f6e3b77c00e4f1b77e9f12e9a832630c3885ef6f8c50bde011c484747ded1877")
ENTRIES[0x80265BC8] = ("JPADrawCalcAlpha::calc@80265BC8", "draw", _DRAW_80265B14, "f6e3b77c00e4f1b77e9f12e9a832630c3885ef6f8c50bde011c484747ded1877")
ENTRIES[0x80265BE0] = ("JPADrawCalcAlpha::calc@80265BE0", "draw", _DRAW_80265B14, "f6e3b77c00e4f1b77e9f12e9a832630c3885ef6f8c50bde011c484747ded1877")
ENTRIES[0x80265BF8] = ("JPADrawCalcAlpha::calc@80265BF8", "draw", _DRAW_80265B14, "f6e3b77c00e4f1b77e9f12e9a832630c3885ef6f8c50bde011c484747ded1877")
ENTRIES[0x80265C14] = ("JPADrawCalcAlpha::calc@80265C14", "draw", _DRAW_80265B14, "f6e3b77c00e4f1b77e9f12e9a832630c3885ef6f8c50bde011c484747ded1877")
_DRAW_80268940 = [(0x802656E0, 0x80268940, 0x802689C4), (0x802616E0, 0x80264C88, 0x802656E0), (0x802656E0, 0x802656E0, 0x80266450), (0x802556E0, 0x80257550, 0x80257988), (0x802556E0, 0x80257B4C, 0x80257D4C)]
ENTRIES[0x80268940] = ("JPADraw::calcParticle", "draw", _DRAW_80268940, "998280e369e7e8007eb9a130cfe4f10e2d0b6ce060d0ffd0032b671cc6364b88")
ENTRIES[0x80268954] = ("JPADraw::calcParticle@80268954", "draw", _DRAW_80268940, "998280e369e7e8007eb9a130cfe4f10e2d0b6ce060d0ffd0032b671cc6364b88")
ENTRIES[0x80268998] = ("JPADraw::calcParticle@80268998", "draw", _DRAW_80268940, "998280e369e7e8007eb9a130cfe4f10e2d0b6ce060d0ffd0032b671cc6364b88")
ENTRIES[0x802689B4] = ("JPADraw::calcParticle@802689B4", "draw", _DRAW_80268940, "998280e369e7e8007eb9a130cfe4f10e2d0b6ce060d0ffd0032b671cc6364b88")
_DRAW_8009A094 = [(0x800996E0, 0x8009A094, 0x8009A5D4), (0x8032D6E0, 0x80330C84, 0x80330D5C), (0x8032D6E0, 0x80330240, 0x803302E0), (0x8032D6E0, 0x8032EF58, 0x8032F2F8), (0x8032D6E0, 0x8032F2F8, 0x8032F3EC), (0x803216E0, 0x80324EE8, 0x80324F3C), (0x8031D6E0, 0x8031FA48, 0x8031FAC4), (0x803216E0, 0x80324D28, 0x80324D30), (0x803216E0, 0x80324D50, 0x80324EE8), (0x8031D6E0, 0x8031FAC4, 0x8031FAE8), (0x803256E0, 0x80326090, 0x80326104), (0x8030D6E0, 0x8030DA44, 0x8030DA98), (0x803256E0, 0x80328990, 0x803289A8)]
ENTRIES[0x8009A094] = ("drawWave", "draw", _DRAW_8009A094, "a8dbdca998ff7631943dc18f2ca4c49db3918f385bfbc04eb21dfd8acc5985c7")
ENTRIES[0x8009A0D0] = ("drawWave@8009A0D0", "draw", _DRAW_8009A094, "a8dbdca998ff7631943dc18f2ca4c49db3918f385bfbc04eb21dfd8acc5985c7")
ENTRIES[0x8009A148] = ("drawWave@8009A148", "draw", _DRAW_8009A094, "a8dbdca998ff7631943dc18f2ca4c49db3918f385bfbc04eb21dfd8acc5985c7")
ENTRIES[0x8009A178] = ("drawWave@8009A178", "draw", _DRAW_8009A094, "a8dbdca998ff7631943dc18f2ca4c49db3918f385bfbc04eb21dfd8acc5985c7")
ENTRIES[0x8009A1F4] = ("drawWave@8009A1F4", "draw", _DRAW_8009A094, "a8dbdca998ff7631943dc18f2ca4c49db3918f385bfbc04eb21dfd8acc5985c7")
ENTRIES[0x8009A298] = ("drawWave@8009A298", "draw", _DRAW_8009A094, "a8dbdca998ff7631943dc18f2ca4c49db3918f385bfbc04eb21dfd8acc5985c7")
ENTRIES[0x8009A2E8] = ("drawWave@8009A2E8", "draw", _DRAW_8009A094, "a8dbdca998ff7631943dc18f2ca4c49db3918f385bfbc04eb21dfd8acc5985c7")
ENTRIES[0x8009A33C] = ("drawWave@8009A33C", "draw", _DRAW_8009A094, "a8dbdca998ff7631943dc18f2ca4c49db3918f385bfbc04eb21dfd8acc5985c7")
ENTRIES[0x8009A3DC] = ("drawWave@8009A3DC", "draw", _DRAW_8009A094, "a8dbdca998ff7631943dc18f2ca4c49db3918f385bfbc04eb21dfd8acc5985c7")
ENTRIES[0x8009A414] = ("drawWave@8009A414", "draw", _DRAW_8009A094, "a8dbdca998ff7631943dc18f2ca4c49db3918f385bfbc04eb21dfd8acc5985c7")
ENTRIES[0x8009A44C] = ("drawWave@8009A44C", "draw", _DRAW_8009A094, "a8dbdca998ff7631943dc18f2ca4c49db3918f385bfbc04eb21dfd8acc5985c7")
ENTRIES[0x8009A484] = ("drawWave@8009A484", "draw", _DRAW_8009A094, "a8dbdca998ff7631943dc18f2ca4c49db3918f385bfbc04eb21dfd8acc5985c7")
ENTRIES[0x8009A49C] = ("drawWave@8009A49C", "draw", _DRAW_8009A094, "a8dbdca998ff7631943dc18f2ca4c49db3918f385bfbc04eb21dfd8acc5985c7")
ENTRIES[0x8009A5C4] = ("drawWave@8009A5C4", "draw", _DRAW_8009A094, "a8dbdca998ff7631943dc18f2ca4c49db3918f385bfbc04eb21dfd8acc5985c7")
# --- end of the sixth set's entries ---
# --- The seventh set (tests/native_leaf_test.c): leaf compute code, each
# function's translation replayed on local registers (native_leaf_run.h,
# scripts/windows/native_leaf_gen.py): libm's fmod and the random numbers and
# angle built on it, the collision blocks' bounds, the Euler quaternions, the
# planes, polar coordinates and point winds. A group per switch
# (BLUEWAKE_NATIVE_<GROUP>=0). An entry's hash covers every function its
# native replays, callees in other chunks included, and every loop the
# translator extracted from them (below). ---
SEVENTH_GROUPS = ("libm", "bgblk", "rot", "calc", "geom", "jas")
for _group in SEVENTH_GROUPS:
    GROUPS[_group] = (f"native_{_group}.h", f"bluewake_native_{_group}_enabled",
                      f"bluewake_native_{_group}_{{entry:08X}}(ctx)")
_N7_FMOD = [(0x8032D6E0, 0x8032EC1C, 0x8032EF58)]                                  # __ieee754_fmod
_N7_FMOD_CALL = [(0x8032D6E0, 0x80330E34, 0x80330E54)] + _N7_FMOD                  # fmod
_N7_RND = [(0x802456E0, 0x802462C8, 0x802463B0)] + _N7_FMOD_CALL                   # cM_rnd
_N7_PSVEC_ADD = (0x8030D6E0, 0x8030DCE0, 0x8030DD04)
_N7_BLCK_MIN_MAX = (0x802456E0, 0x80247C4C, 0x80247CD4)
ENTRIES[0x8032EC1C] = ("__ieee754_fmod", "libm", _N7_FMOD, "9fa82fa48c28ac1ae46d1f01fc7c2cea2244f101825036a6d4e6b7e2a871bf63")
ENTRIES[0x80330E34] = ("fmod", "libm", _N7_FMOD_CALL, "88aac1cf75d0b8f23cc05b650b1738be37ff04427240c4f30f3d58734a135d97")
ENTRIES[0x80246044] = ("cM_rad2s", "libm", [(0x802456E0, 0x80246044, 0x8024609C)] + _N7_FMOD_CALL, "a71baceea7b9de408ffe2982473fd0b584d4a106e1726ec511f17c33cbe45bae")
ENTRIES[0x802462C8] = ("cM_rnd", "libm", _N7_RND, "1ff714bc3355e7e84057cc8e6b14c32d5ab8e1ae892adf964a77495dd046e1da")
ENTRIES[0x802463B0] = ("cM_rndF", "libm", [(0x802456E0, 0x802463B0, 0x802463E8)] + _N7_RND, "1049f634ddc70f51b818f8ee76ac644b8293dc251869c87cf29cb60236fd6af3")
ENTRIES[0x802463E8] = ("cM_rndFX", "libm", [(0x802456E0, 0x802463E8, 0x80246430)] + _N7_RND, "12457a9d276c6375dc52b646061e3ad555909e0a3b0dde90a494ea0236bc843e")
_N7_REM_PIO2 = (0x8032D6E0, 0x8032EF58, 0x8032F2F8)
_N7_KERNEL_SIN = (0x8032D6E0, 0x80330240, 0x803302E0)
_N7_KERNEL_COS = (0x8032D6E0, 0x8032F2F8, 0x8032F3EC)
ENTRIES[0x80330C84] = ("sin", "libm", [(0x8032D6E0, 0x80330C84, 0x80330D5C), _N7_REM_PIO2, _N7_KERNEL_SIN,
                                       _N7_KERNEL_COS], "3e9404ebf0aedb59d93c0998804bd1e1e03a8f67a57791ceb9c7599a509a5b93")
ENTRIES[0x8033071C] = ("cos", "libm", [(0x8032D6E0, 0x8033071C, 0x803307F0), _N7_REM_PIO2, _N7_KERNEL_SIN,
                                       _N7_KERNEL_COS], "4fb8c6e625079ee7f1d50fd8b8edc11e667bfbce70ecaf7d17b9a16fbbb211a5")
ENTRIES[0x80330D5C] = ("tan", "libm", [(0x8032D6E0, 0x80330D5C, 0x80330DD4), _N7_REM_PIO2,
                                       (0x8032D6E0, 0x803302E0, 0x803304F4)], "56399d3612830daa0ea4a10b212df2ff9680d88901cd4fd2504570d87fdec2c0")
ENTRIES[0x80247C4C] = ("cBgW::MakeBlckMinMax", "bgblk", [_N7_BLCK_MIN_MAX], "5d51b4f4def170de0682b2546457867909a4a157db2e3520781b8ca79a9be7ed")
ENTRIES[0x80247CD4] = ("cBgW::MakeBlckBnd", "bgblk",
                       [(0x802456E0, 0x80247CD4, 0x80247E48), _N7_BLCK_MIN_MAX,
                        (0x802456E0, 0x80247BF8, 0x80247C4C), _N7_PSVEC_ADD], "3383bcf326f463846d93f442f26758a917757bbf993186d6a66597887135d138")
ENTRIES[0x80301150] = ("JMAEulerToQuat", "rot", [(0x802FD6E0, 0x80301150, 0x80301218)], "fe49d931e2e66a7620e394a3d7630ec358caa12022e51f2faaed524126d965df")
_N7_ATAN2S = [(0x802456E0, 0x802460D0, 0x80246270), (0x802456E0, 0x8024609C, 0x802460D0)]
ENTRIES[0x802460D0] = ("cM_atan2s", "calc", _N7_ATAN2S, "cc522d464d6d55a358b7694870d18ee081db110e1ca9e1fdd25f2b242db9b451")
ENTRIES[0x8024A6F0] = ("cM3d_CalcPla", "geom",
                       [(0x802496E0, 0x8024A6F0, 0x8024A7BC), (0x8030D6E0, 0x8030DD04, 0x8030DD28),
                        (0x8030D6E0, 0x8030DECC, 0x8030DF08), (0x8030D6E0, 0x8030DE68, 0x8030DEAC),
                        (0x8030D6E0, 0x8030DD28, 0x8030DD44), (0x8030D6E0, 0x8030DEAC, 0x8030DECC)], "495bbfcdbf608e71c6e8981b18f57e7ced99f4918354cb87aa1c444487f10db0")
ENTRIES[0x80254214] = ("cSPolar::Val", "geom",
                       [(0x802516E0, 0x80254214, 0x80254420), (0x802456E0, 0x80246270, 0x802462B8),
                        (0x802456E0, 0x802460D0, 0x80246270), (0x802456E0, 0x8024609C, 0x802460D0),
                        (0x802516E0, 0x80253C4C, 0x80253C54), (0x802516E0, 0x802540F0, 0x802541B0),
                        (0x802516E0, 0x80253BE0, 0x80253C10), (0x802516E0, 0x80253E14, 0x80253E44),
                        (0x802516E0, 0x80253C40, 0x80253C4C), (0x802516E0, 0x80253D30, 0x80253D40),
                        (0x802516E0, 0x80253DB8, 0x80253DE4)], "10ed8d8c91c5162d739aff6eca07c213709d6a355c7ac1f96de007832e6aa8d0")
ENTRIES[0x8008A230] = ("dKyw_pntwind_get_info", "geom",
                       [(0x800896E0, 0x8008A230, 0x8008A4C8), (0x800896E0, 0x8008AB94, 0x8008ABB4),
                        (0x800896E0, 0x8008AB3C, 0x8008AB94), (0x800896E0, 0x8008AA30, 0x8008AB3C),
                        (0x8030D6E0, 0x8030E0B4, 0x8030E0DC)], "75a157aa0ed5da750247a1ffa39350fa1a0458d6a6946f34fed0ec9c23ca0931")
_N7_OSC_CALC = [(0x8028D6E0, 0x8028E238, 0x8028E5EC), (0x802896E0, 0x8028AAE4, 0x8028AAEC),
                (0x803256E0, 0x80328E10, 0x80328E6C)]
ENTRIES[0x8028DF2C] = ("JASystem::TOscillator::getOffset", "jas",
                       [(0x8028D6E0, 0x8028DF2C, 0x8028E070)] + _N7_OSC_CALC, "7cb77832418511d9ad74d93d9d9657cb8c7c6040f215e6193208099688495c3f")
ENTRIES[0x8028E238] = ("JASystem::TOscillator::calc", "jas", _N7_OSC_CALC, "da25f5b3a551431910c890ed98403ad4064be6e8b563ff40961e1dee7f78f280")
ENTRIES[0x8028C3A8] = ("JASystem::TChannel::updateEffectorParam", "jas",
                       [(0x802896E0, 0x8028C3A8, 0x8028C62C), (0x802896E0, 0x8028CABC, 0x8028CB88),
                        (0x802896E0, 0x8028CB88, 0x8028CC90), (0x802896E0, 0x8028CD90, 0x8028CEA8),
                        (0x802896E0, 0x8028CEA8, 0x8028D128), (0x802796E0, 0x8027A9C8, 0x8027A9F4),
                        (0x802796E0, 0x8027A9F4, 0x8027AA20), (0x802896E0, 0x8028A740, 0x8028A764),
                        (0x802896E0, 0x8028AAC4, 0x8028AACC), (0x802896E0, 0x8028AACC, 0x8028AAD4),
                        (0x802896E0, 0x8028AADC, 0x8028AAE4)], "47d638b4641fd2e7fa2b783e59a6d2fa9b7e310d2f7620322edd90255eebeeaf")
_entry_hash_sixth = entry_hash


def entry_hash(texts, entry):  # noqa: F811 (the seventh set's: the extracted loops too)
    """A seventh-set native also replays the loops the translator extracted
    from its functions: each `static void loop_X` a fragment or its prepaid
    copies call (`label_X: loop_X(ctx); ...`) sits outside the chunk's
    function, so the fragments alone would not see it change. Their text is
    hashed after the fragments'."""
    digest, named = _entry_hash_sixth(texts, entry)
    _, group, fragments, _ = ENTRIES[entry]
    if digest is None or group not in SEVENTH_GROUPS:
        return digest, named
    loops = []
    for chunk, start, end in fragments:
        body, copies = fragment(main_function(texts[chunk], chunk), start, end)
        for head in sorted(set(re.findall(r"\bloop_([0-9A-F]{8})\(ctx\);", body + "".join(copies)))):
            m = re.search(rf"\nstatic void loop_{head}\(CPUState\* ctx_param\) \{{\n.*?\n\}}\n", texts[chunk], re.S)
            if m is None:
                return None, named
            loops.append(canonical(m.group(0)))
    if loops:
        digest = hashlib.sha256("\n".join([digest] + loops).encode()).hexdigest()
    return digest, named
# --- end of the seventh set's entries ---


def certify(root, watched, report=False):
    certified = {}
    for entry, (name, _, _, expected) in ENTRIES.items():
        sets = translations(root, entry)
        if sets is None:
            print(f"{name}: no translation found; not hooked")
            continue
        ok = True
        for paths in sets:
            texts = {chunk: path.read_text(encoding="utf-8") for chunk, path in paths.items()}
            digest, named = entry_hash(texts, entry)
            where = ", ".join(sorted(p.relative_to(root).as_posix() for p in paths.values()))
            if report:
                print(f"{entry:08X} {name}: {digest} ({where})")
            if digest != expected:
                if not report:
                    print(f"{name}: not the certified translation ({where}); not hooked")
                ok = False
            hit = sorted(a for a in named if a in watched or (a & ~0x40000000) in watched)
            if hit:
                print(f"{name}: the host watches {', '.join(f'{a:08X}' for a in hit)}; not hooked")
                ok = False
        if ok:
            certified[entry] = name
    return certified


def transform(text, chunk, certified):
    """Hooks for the certified entries whose first fragment is this chunk;
    stale hooks (entries no longer certified) removed."""
    for entry, (_, _, fragments, _) in ENTRIES.items():
        if fragments[0][0] == chunk and entry not in certified:
            text = text.replace(hook_text(entry, chunk), "")
    done = 0
    for entry in sorted(certified):
        _, group, fragments, _ = ENTRIES[entry]
        if fragments[0][0] != chunk:
            continue
        label = f"\nlabel_{entry:08X}:\n"
        hook = hook_text(entry, chunk)
        if label + hook in text:
            continue
        if text.count(label) != 1 or f"\nreturn_dispatch_{chunk:08X}:\n" not in text:
            raise ValueError(f"no entry label or return dispatch for {entry:08X}")
        text = add_header(text.replace(label, label + hook, 1), f'#include "{GROUPS[group][0]}"\n')
        done += 1
    return text, done


def add_header(text, header):
    """The group's header among the includes under this script's mark."""
    if MARK not in text:
        if INCLUDE not in text:
            raise ValueError("no generated.h include")
        text = text.replace(INCLUDE, INCLUDE + MARK, 1)
    at = text.index(MARK) + len(MARK)
    line = at
    while text.startswith('#include "', line):
        end = text.index("\n", line) + 1
        if text[line:end] == header:
            return text
        line = end
    return text[:at] + header + text[at:]


def main():
    args = sys.argv[1:]
    report = args[:1] == ["--hashes"]
    if report:
        args = args[1:]
    if len(args) != 1:
        sys.exit(__doc__)
    root = Path(args[0])
    watched = watched_addresses()
    certified = certify(root, watched, report)
    if report:
        return
    chunks = sorted({fragments[0][0] for _, _, fragments, _ in ENTRIES.values()})
    hooks = files = 0
    for chunk in chunks:
        for path in chunk_files(root, chunk):
            with open(path, encoding="utf-8", newline="") as file:
                original = file.read()
            converted, count = transform(original, chunk, certified)
            if converted != original:
                temporary = path.with_suffix(".c.tmp")
                with open(temporary, "w", encoding="utf-8", newline="") as file:
                    file.write(converted)
                temporary.replace(path)
                files += 1
            hooks += count
    print(f"native entries: {len(certified)}/{len(ENTRIES)} certified, {hooks} new hooks in {files} chunks")


if __name__ == "__main__":
    main()
