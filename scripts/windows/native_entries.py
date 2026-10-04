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
GROUPS["gx"] = ("native_gx.h", "bluewake_native_gx_enabled", "bluewake_native_gx(ctx, 0x{entry:08X}u)")
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
ENTRIES[0x80326B80] = ("GXCallDisplayList", "gx", [(_GX_0201, 0x80326B80, 0x80326BF0)], "e621c45ae139631aca2cf9d0247f9918414e003d700b8ef57f78e6772142b37c")
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
# The vertex format and the TEV, channel and pixel state. GXSetVtxDesc runs
# across the boundary between chunks 0199 and 0200.
ENTRIES[0x80321608] = ("GXSetVtxDesc", "gx", [(_GX_0199, 0x80321608, 0x803216E0), (_GX_0200, 0x803216E0, 0x80321958)], "16b88f38d170206a39c9a99b8d7b10c52ad33258db5d3212a96e40770a147b62")
ENTRIES[0x80321AD0] = ("GXClearVtxDesc", "gx", [(_GX_0200, 0x80321AD0, 0x80321B08)], "e9ebc2526a6a2a14a055ae5945c821cbe08ead2abc8fdc6806c52abefefde89f")
ENTRIES[0x80321B08] = ("GXSetVtxAttrFmt", "gx", [(_GX_0200, 0x80321B08, 0x80321E60)], "232d31611a11cb2dec59d3042337b89ffb0d3e04dac384e1b25c22fd47868e93")
ENTRIES[0x80322604] = ("GXSetTexCoordGen2", "gx", [(_GX_0200, 0x80322604, 0x803228D4), _GX_SET_MATRIX_INDEX], "1439617621dd5b80bc69edbb6ce727042f67ef8517b08799c3bc308571b951dd")
ENTRIES[0x803228D4] = ("GXSetNumTexGens", "gx", [(_GX_0200, 0x803228D4, 0x80322914)], "5efc2b471817a42487c38ca3311e4ac35b5e53ee8dfdb9ef21543a9b9c69f5d8")
ENTRIES[0x80323328] = ("GXSetCullMode", "gx", [(_GX_0200, 0x80323328, 0x80323374)], "13e8f0c5d0c268a7ae571e8f166430624eb8487f3e0389edb1cde0ba056ad38b")
ENTRIES[0x80324390] = ("GXSetChanAmbColor", "gx", [(_GX_0200, 0x80324390, 0x80324484)], "1f515c934c1836f9bf37ce699f8f8ac35627f2d8b471ece8e61e8c67b38ad02a")
ENTRIES[0x80324484] = ("GXSetChanMatColor", "gx", [(_GX_0200, 0x80324484, 0x80324578)], "fcda2494a487596bdc6dce874aabde41651f3ad68de04146171a9c23b29ac2dc")
ENTRIES[0x80324578] = ("GXSetNumChans", "gx", [(_GX_0200, 0x80324578, 0x803245BC)], "8f4e5b4524b2f7255bb5ea17bb3374a04df56c4a9bd626be57df23d449ef4a3f")
ENTRIES[0x803245BC] = ("GXSetChanCtrl", "gx", [(_GX_0200, 0x803245BC, 0x80324688)], "d516a0552692157aa3ada31ea64b14b381f06698739b77a72c967c33df49b539")
ENTRIES[0x80324D28] = ("GXGetTexObjFmt", "gx", [(_GX_0200, 0x80324D28, 0x80324D30)], "bbcac18f9ee7636d2ceebd84c8a894e02ba0491f2d3d88e94d5d8b39a1bc3680")
ENTRIES[0x80325774] = ("GXSetTevIndirect", "gx", [(_GX_0201, 0x80325774, 0x80325810)], "d4789c9ed3b98c430d0ca3f514aa7f2fb63b94c99a0246adbb3dd412e682da8a")
ENTRIES[0x80325C00] = ("GXSetNumIndStages", "gx", [(_GX_0201, 0x80325C00, 0x80325C28)], "a0f88007ca59b2c1598302ce4bbd13a5c43afa08a096b2651525765d61001f7d")
ENTRIES[0x80325C28] = ("GXSetTevDirect", "gx", [(_GX_0201, 0x80325C28, 0x80325C70), (_GX_0201, 0x80325774, 0x80325810)], "a218d60f24b38b3f694c1f0a7028b7bbfcb133c57a6ee93b7dff473bcd617739")
ENTRIES[0x80325E50] = ("GXSetTevColorIn", "gx", [(_GX_0201, 0x80325E50, 0x80325E94)], "66cd3bdbff0e8c98657f221b1b10242b3288ae938515ea0907dd3702e00b4f38")
ENTRIES[0x80325E94] = ("GXSetTevAlphaIn", "gx", [(_GX_0201, 0x80325E94, 0x80325ED8)], "a09004e8b36cf0f0af881da63a0a585b71bbc64454d871b252d7867214e44a00")
ENTRIES[0x80325ED8] = ("GXSetTevColorOp", "gx", [(_GX_0201, 0x80325ED8, 0x80325F40)], "c39d20908f8973144905fa69428320ac2873c08f08ba4b71ba06cf1cdd1c63c2")
ENTRIES[0x80325F40] = ("GXSetTevAlphaOp", "gx", [(_GX_0201, 0x80325F40, 0x80325FA8)], "2d76095ac298a7428e9765e48eebf4de5eae2c912a773b04242ffc5f7b0aa25d")
ENTRIES[0x80326104] = ("GXSetTevKColorSel", "gx", [(_GX_0201, 0x80326104, 0x80326170)], "29c118d2faf08745a9689b9cbc7a5fb66e4fc30d789a5d0e4fc01be36710b14e")
ENTRIES[0x80326170] = ("GXSetTevKAlphaSel", "gx", [(_GX_0201, 0x80326170, 0x803261DC)], "242fe3cf1112174dd10a025bbc6f77878faa84a1a8f2df31038fde2c28a9550d")
ENTRIES[0x803261DC] = ("GXSetTevSwapMode", "gx", [(_GX_0201, 0x803261DC, 0x80326230)], "d2588aa246132a904221caef8a297c1b197604c3cc1c26c0f71e23f77756618c")
ENTRIES[0x803262C8] = ("GXSetAlphaCompare", "gx", [(_GX_0201, 0x803262C8, 0x8032631C)], "48ddeecac3d718fd4551c7212e9394aa78c578f4202ce8684fbf4af976068c6f")
ENTRIES[0x80326578] = ("GXSetNumTevStages", "gx", [(_GX_0201, 0x80326578, 0x803265A8)], "9941b8dc471530cf9880225c70e0886fcb7dd4d5cb497585aced88fe437089fa")
ENTRIES[0x80326858] = ("GXSetBlendMode", "gx", [(_GX_0201, 0x80326858, 0x803268AC)], "3bd735dd16a29c53d3d018f4292b1f346fa5fa99bea725b3893323c2ac2f232b")
ENTRIES[0x803268AC] = ("GXSetColorUpdate", "gx", [(_GX_0201, 0x803268AC, 0x803268D8)], "aa1aa19d6c291367a3fa136a82d4e5046ad11a718f1b5b1cc42b270f971209f7")
ENTRIES[0x803268D8] = ("GXSetAlphaUpdate", "gx", [(_GX_0201, 0x803268D8, 0x80326904)], "27f8d7419dcfd645a1eb4a6ff88fa073033678bd503cb43ebaf6a21077ffa03b")
ENTRIES[0x80326904] = ("GXSetZMode", "gx", [(_GX_0201, 0x80326904, 0x80326938)], "c8c045cbf777017fad1be07df2abe1bbb0f588e942187826f19257d8fccde1fa")
ENTRIES[0x80326938] = ("GXSetZCompLoc", "gx", [(_GX_0201, 0x80326938, 0x80326970)], "9be877c171bc06d68bcea37b86794bcac74ad98a6bef8bcc3ac11b7e6cacd05f")
ENTRIES[0x80326A8C] = ("GXSetDstAlpha", "gx", [(_GX_0201, 0x80326A8C, 0x80326AC8)], "b45ebd3b2c6baf040029002a94c957e6448c6891b46a43a30088bc58e2797ea6")
ENTRIES[0x80326FD8] = ("GXSetCurrentMtx", "gx", [(_GX_0201, 0x80326FD8, 0x80327010), _GX_SET_MATRIX_INDEX], "6ab6ebcd301b1d79e5ef5878cdb8146ca72bde211818b54a5f0ed3f16ebd0f21")
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
# left out of the check.
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
