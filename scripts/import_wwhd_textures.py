#!/usr/bin/env python3
"""Make a private Dolphin/Aurora texture pack from two user-supplied games.

Run --help for usage. Extracted art and the detailed report stay local;
the importer ships only code, dependency notices, and generated test data.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from datetime import datetime, timezone
import hashlib
import importlib.metadata
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import sys
import tempfile

from prepare import read_fst, validate_disc_header
from wwhd.formats import FormatError, HDTexture, MAX_FILE, bfres, bflim, for_gc_format, gc_textures, leaves
from wwhd.fallback import add_fallback
from wwhd.title import title_image
from wwhd.layout import HEARTS, SWIM_ICONS, heart_image, inventory_names
from wwhd.matches import candidate_priority
from wwhd.font import BitmapFont
from wwhd.ui import LAYOUT_SPRITES, OPENING_PANELS, fit_sprite, font_sprites, opening_sprite

ROOT = Path(__file__).resolve().parent.parent
SUPPORT = Path.home() / "Library/Application Support/Wind Waker Recomp"


def resource(path: str) -> tuple[str, ...]:
    """Use archive identity; stage Room0 alone is not a unique resource."""
    parts = [p.lower() for p in PurePosixPath(path.split("#")[0]).parts]
    # Permanent packs contain stage archives without a Stage/ parent folder.
    # Their room-qualified names still identify exactly the same resource.
    for part in reversed(parts):
        if part.endswith(".szs"):
            match = re.fullmatch(r"(.+)_(room\d+|stage)\.szs", part)
            if match:
                return "stage", match[1], match[2]
    if "stage" in parts:
        i = parts.index("stage")
        rest = parts[i + 1:]
        first = PurePosixPath(rest[0]).stem
        if len(rest) >= 2 and rest[1].endswith(".arc"):
            return "stage", first, PurePosixPath(rest[1]).stem
        stage, sep, room = first.rpartition("_")
        return ("stage", stage, room) if sep else ("stage", first)
    archives = [PurePosixPath(p).stem for p in parts if p.endswith((".arc", ".szs"))]
    if archives:
        return "object", archives[-1]
    return "object", PurePosixPath(parts[-1]).stem


def compatible(gc, hd) -> bool:
    # Tiny placeholders and GameCube shader ramp textures must retain their
    # original meaning; Wii U material/shader changes cannot be imported.
    if min(gc.width, gc.height) < 16 or gc.name.lower().startswith(("zbtoon", "toon")):
        return False
    # These Always HUD textures use different transparency/material rules in
    # HD. Copying the artwork into the GameCube draw creates opaque panels.
    # Keep them on the existing fallback pack (or the original disc textures).
    if resource(gc.source) == ("object", "always") and gc.name.lower().startswith(
            ("camera_", "map_frame", "map_check")):
        return False
    # BFRES logo layers have changed masks and Japanese text. Use the
    # transparent English BFLIM layout artwork for the main title instead.
    if ".bfres#" in hd.source.lower() and resource(gc.source) in (("object", "tlogo"), ("object", "tlogoe")) and gc.name.lower() in (
            "logo_zelda_main", "logo_zelda_jpa", "logo_sub"):
        return False
    return (hd.width >= gc.width and hd.height >= gc.height and
            hd.width * gc.height == hd.height * gc.width)


def source_textures(data: bytes, path: str, targets, layout=None):
    """Yield verified model, inventory, HUD and English HD title matches."""
    if path.endswith(".bffnt") and "/permanent_2d_UsEnglish.pack/" in path:
        font_name = PurePosixPath(path).stem
        bindings = [(res, name, text, targets.get((res, name), []))
                    for res, name, text in font_sprites(font_name)]
        bindings = [b for b in bindings if b[3]]
        if not bindings:
            return
        font = BitmapFont(data)
        for _, name, text, candidates in bindings:
            image = (font.glyph(font.mapping[text]) if isinstance(text, int)
                     else font.render(text))
            for gc in candidates:
                adapted = fit_sprite(image, gc)
                hd = HDTexture(path + "#GameCube-sprite:" + name, name,
                               adapted.width, adapted.height, 0x1A,
                               (0, 1, 2, 3), [adapted.tobytes()])
                yield hd, [gc]
    elif "/Opening_00.szs/timg/" in path and (panels := OPENING_PANELS.get(PurePosixPath(path).name)):
        image = next(bflim(data, path).images())
        for name, box in panels:
            for res in ("opening", "open1", "open2", "open3"):
                for gc in targets.get((("object", res), name), []):
                    adapted = opening_sprite(image, box, gc)
                    hd = HDTexture(path + "#GameCube-panel:" + name, name,
                                   adapted.width, adapted.height, 0x1A,
                                   (0, 1, 2, 3), [adapted.tobytes()])
                    yield hd, [gc]
    elif path.endswith(".bflim") and (binding := next(
            (v for (archive, filename), v in LAYOUT_SPRITES.items()
             if f"/{archive}.szs/timg/{filename}" in path), None)):
        image = next(bflim(data, path).images())
        for res, name in binding:
            for gc in targets.get((res, name), []):
                adapted = fit_sprite(image, gc)
                hd = HDTexture(path + "#GameCube-sprite:" + name, name,
                               adapted.width, adapted.height, 0x1A,
                               (0, 1, 2, 3), [adapted.tobytes()])
                yield hd, [gc]
    elif data[:4] == b"FRES":
        identity = resource(path)
        names = {name for res, name in targets if res == identity}
        for hd in bfres(data, path, names):
            yield hd, targets[(identity, hd.name.lower())]
    elif path.endswith(".bflim") and (names := inventory_names(path)):
        hd = bflim(data, path)
        hd.name = names[0]
        yield hd, [t for name in names for t in targets.get((("object", "itemicon"), name), [])]
    elif "/SwimTime_00.szs/timg/" in path:
        name = SWIM_ICONS.get(PurePosixPath(path).name)
        if name:
            hd = bflim(data, path)
            hd.name = name
            yield hd, targets.get((("object", "swimres"), name), [])
    elif layout is not None and "/Heart_00.szs/timg/" in path:
        name = HEARTS.get(PurePosixPath(path).name)
        if name:
            base = next(bflim(layout["HeartBase_00^t.bflim"], path + "#outline").images())
            fill = next(bflim(data, path).images())
            adapted = heart_image(base, fill)
            hd = HDTexture(path + "#GameCube-layout", name, 96, 96,
                           0x1A, (0, 1, 2, 3), [adapted.tobytes()])
            yield hd, targets.get((("object", "menures"), name), [])
    elif layout is not None and "/Title_00.szs/timg/" in path:
        mapping = {"TitleLogoZelda_00^l.bflim": "logo_zelda_main",
                   "TitleLogoWindwaker_00^l.bflim": "logo_sub_e"}
        name = mapping.get(PurePosixPath(path).name)
        if name:
            image = next(bflim(data, path).images())
            badge = (next(bflim(layout["TitleLogoHD_00^l.bflim"], path + "#HD-badge").images())
                     if name == "logo_sub_e" else None)
            candidates = [t for res in (("object", "tlogo"), ("object", "tlogoe"))
                          for t in targets.get((res, name), [])]
            sizes = {(t.width, t.height) for t in candidates}
            for w, h in sorted(sizes):
                adapted = title_image((w * 4, h * 4), image, badge)
                hd = HDTexture(path + "#GameCube-layout", name, adapted.width,
                               adapted.height, 0x1A, (0, 1, 2, 3), [adapted.tobytes()])
                yield hd, [t for t in candidates if (t.width, t.height) == (w, h)]


def index_gc(path: Path, errors: list):
    textures = []
    with path.open("rb") as f:
        header = validate_disc_header(f)
        if header["revision"] != 0:
            raise FormatError("only GZLE01 revision 0 is supported")
        entries = read_fst(f, header["fst_offset"], header["fst_size"])
        for e in entries:
            if e["is_dir"] or not e["path"].lower().endswith((".arc", ".bti", ".bmd", ".bdl", ".bmt")):
                continue
            if e["size"] > MAX_FILE:
                raise FormatError("GameCube resource exceeds size limit")
            f.seek(e["offset"])
            try:
                for name, data in leaves(f.read(e["size"]), e["path"]):
                    try:
                        textures.extend(gc_textures(data, name))
                    except (FormatError, UnicodeError) as ex:
                        errors.append({"source": name, "reason": str(ex)})
            except (FormatError, UnicodeError) as ex:
                errors.append({"source": e["path"], "reason": str(ex)})
    return textures


def hd_files(path: Path, args):
    suffixes = (".szs", ".sarc", ".pack", ".bfres")
    if path.is_dir():
        for p in sorted(path.rglob("*")):
            if p.is_file() and p.suffix.lower() in suffixes:
                if args.only and not any(s.lower() in p.as_posix().lower() for s in args.only):
                    continue
                if p.stat().st_size > MAX_FILE:
                    raise FormatError("HD resource exceeds size limit")
                yield p.relative_to(path).as_posix(), p.read_bytes()
    elif path.suffix.lower() in (".wux", ".wud", ".iso"):
        from wwhd.disc import WiiUDisc, read_key
        disc_key = args.disc_key or path.parent / "game.key"
        title_key = read_key(args.title_key) if args.title_key else None
        common = args.common_key or path.parent / "common.key"
        common_key = read_key(common) if common.is_file() and title_key is None else None
        disc = WiiUDisc(path, read_key(disc_key), title_key, common_key)
        print(f"HD disc validated: {disc.identifier}; {len(disc.entries)} files", flush=True)
        try:
            for e in disc.entries:
                if e.path.lower().endswith(suffixes) and not e.path.startswith("content/Cafe/"):
                    if args.only and not any(s.lower() in e.path.lower() for s in args.only):
                        continue
                    yield e.path, disc.read(e)
        finally:
            disc.close()
    else:
        raise FormatError("HD input must be a WUD/WUX disc or decrypted content folder")


def file_sha256(path: Path) -> str:
    with path.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def install(pack: Path, settings: Path):
    text = settings.read_text() if settings.exists() else ""
    value = "DOL_AURORA_TEXTURE_PACK=" + str(pack.resolve())
    if re.search(r"^DOL_AURORA_TEXTURE_PACK=.*$", text, flags=re.M):
        new = re.sub(r"^DOL_AURORA_TEXTURE_PACK=.*$", lambda _: value, text, flags=re.M)
    else:
        new = text + ("\n" if text and not text.endswith("\n") else "") + value + "\n"
    settings.parent.mkdir(parents=True, exist_ok=True)
    if settings.exists():
        backup = settings.with_name(settings.name + ".before-wwhd-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ"))
        shutil.copy2(settings, backup)
    with tempfile.NamedTemporaryFile(mode="w", dir=settings.parent, delete=False) as f:
        f.write(new)
        tmp = Path(f.name)
    tmp.replace(settings)


def build(args) -> dict:
    output = args.output.expanduser().resolve()
    if output.exists():
        raise FormatError("output already exists; choose a new folder to preserve existing packs")
    if output.is_relative_to(ROOT) and output.relative_to(ROOT).parts[0] not in ("build", "local-research", "game", "generated"):
        raise FormatError("inside the checkout, extracted art must stay in ignored build/ or local-research/")
    errors = []
    originals = index_gc(args.gc_disc, errors)
    targets = defaultdict(list)
    for t in originals:
        targets[(resource(t.source), t.name.lower())].append(t)
    print(f"Indexed {len(originals)} GameCube textures in {len(targets)} resource/name groups", flush=True)
    output.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix="." + output.name + "-", dir=output.parent))
    matches, conflicts, unmatched = {}, set(), []
    scanned, decoded = 0, 0
    try:
        for source, archive in hd_files(args.hd_source, args):
            if args.only and not any(s.lower() in source.lower() for s in args.only):
                continue
            scanned += 1
            if scanned % 20 == 0:
                print(f"Read {scanned} HD resources; {len(matches) - len(conflicts)} replacement keys", flush=True)
            try:
                members = (list(leaves(archive, source)) if source.endswith(("/Title_00.szs", "_2d_UsEnglish.pack"))
                           else leaves(archive, source))
                layouts = defaultdict(dict)
                if isinstance(members, list):
                    for path, data in members:
                        if path.endswith(".bflim"):
                            p = PurePosixPath(path)
                            layouts[str(p.parent)][p.name] = data
                for path, data in members:
                    layout = layouts.get(str(PurePosixPath(path).parent))
                    for hd, candidates in source_textures(data, path, targets, layout):
                        gc = [t for t in candidates if compatible(t, hd)]
                        if not gc:
                            unmatched.append({"source": hd.source, "reason": "dimensions, placeholder, or shader texture"})
                            continue
                        try:
                            images = list(hd.images())
                            # A complete generated mip chain prevents shimmering
                            # when the source archive stops above 1x1.
                            while images[-1].size != (1, 1):
                                w, h = images[-1].size
                                from PIL import Image
                                images.append(images[-1].resize((max(1, w // 2), max(1, h // 2)), Image.Resampling.LANCZOS))
                            variants = {fmt: for_gc_format(images, fmt, layout_mask=any(
                                            ext in hd.source for ext in (".bflim", ".bffnt")))
                                        for fmt in {t.format for t in gc}}
                            digests = {fmt: hashlib.sha256(b"".join(im.tobytes() for im in variant)).hexdigest()
                                       for fmt, variant in variants.items()}
                            decoded += 1
                            for original in gc:
                                variant, digest = variants[original.format], digests[original.format]
                                key = original.filename()
                                priority = candidate_priority(hd, resource(hd.source))
                                originals_for_key, alternatives = [original.source], []
                                if key in matches:
                                    previous = matches[key]
                                    if priority < previous["priority"]:
                                        previous["alternatives"].append(hd.source)
                                        previous["originals"].append(original.source)
                                        continue
                                    if priority == previous["priority"]:
                                        if previous["image_sha256"] != digest:
                                            conflicts.add(key)
                                            previous["alternatives"].append(hd.source)
                                        else:
                                            previous["originals"].append(original.source)
                                        continue
                                    # A later audited match can resolve a prior
                                    # disagreement between unpreferred variants.
                                    originals_for_key += previous["originals"]
                                    alternatives = previous["alternatives"] + [previous["hd"]]
                                    for name in previous["generated_files"]:
                                        (staging / name).unlink()
                                    conflicts.discard(key)
                                generated = []
                                for i, image in enumerate(variant):
                                    name = key if not i else key.removesuffix(".png") + f"_mip{i}.png"
                                    image.save(staging / name)
                                    generated.append(name)
                                matches[key] = {"file": key, "hd": hd.source, "originals": originals_for_key,
                                                "priority": priority, "alternatives": alternatives,
                                                "original_size": [original.width, original.height],
                                                "hd_size": [hd.width, hd.height], "mipmaps": len(images),
                                                "image_sha256": digest, "generated_files": generated}
                        except (FormatError, OSError, ValueError) as ex:
                            errors.append({"source": hd.source, "reason": str(ex)})
            except (FormatError, UnicodeError, OSError) as ex:
                errors.append({"source": source, "reason": str(ex)})
        for key in conflicts:
            for name in matches[key]["generated_files"]:
                (staging / name).unlink()
        accepted = [v for k, v in sorted(matches.items()) if k not in conflicts]
        if not accepted:
            raise FormatError("no compatible textures matched; nothing installed")
        for texture in accepted:
            if texture.pop("priority"):
                texture["selection"] = "audited shared-artwork variant"
            texture["alternatives"] = sorted(set(texture["alternatives"]))
            texture["originals"] = sorted(set(texture["originals"]))
            texture["files_sha256"] = {name: file_sha256(staging / name)
                                       for name in texture.pop("generated_files")}
        # Some packs replace Nintendo's title copyright with their own banner.
        # Keep the disc's copyright instead of importing that replacement.
        excluded = {t.filename().removesuffix(".png") for t in originals
                    if resource(t.source) == ("object", "tlogoe") and
                    t.name.lower() == "c_nintendo_e"}
        fallback = (add_fallback(staging, [t["file"] for t in accepted], args.fallback_pack, excluded)
                    if getattr(args, "fallback_pack", None) else None)
        tool_sources = [Path(__file__), *sorted((ROOT / "scripts/wwhd").rglob("*.py")),
                        ROOT / "scripts/wwhd/requirements.txt", ROOT / "scripts/wwhd/yaz0_native.c"]
        report = {"format": 1, "name": "Wind Waker HD (local disc import)",
                  "created_utc": datetime.now(timezone.utc).isoformat(),
                  "gc_disc_sha256": file_sha256(args.gc_disc),
                  "tool_sources_sha256": {p.relative_to(ROOT).as_posix(): file_sha256(p) for p in tool_sources},
                  "dependencies": {name: importlib.metadata.version(name) for name in ("Pillow", "xxhash", "pycryptodome")},
                  "surface_addressing": json.loads((ROOT / "scripts/wwhd/vendor/SOURCE.json").read_text()),
                  "hd_disc_sha256": file_sha256(args.hd_source) if args.hd_source.is_file() else None,
                  "hd_resource_count": scanned, "decoded_textures": decoded,
                  "original_texture_count": len(originals), "replacement_count": len(accepted),
                  "fallback": fallback,
                  "combined_replacement_count": len(accepted) + (fallback["texture_count"] if fallback else 0),
                  "conflicts_skipped": sorted(conflicts), "unsupported": errors,
                  "unmatched": unmatched, "textures": accepted,
                  "limits": "Compatible BFRES artwork, audited shared-artwork variants, inventory/collection/chart icons, HUD hearts and swimming meter, adapted English title/opening layouts, font-derived English action/menu text, counters and A/B/X/Y symbols, and adapted key/cursor sprites; HD shaders, models, lighting and other redesigned HUD are not imported."}
        (staging / "import-report.json").write_text(json.dumps(report, indent=2) + "\n")
        (staging / "README.txt").write_text("Private artwork extracted from your own Wind Waker HD disc.\n"
                                          "Select this folder as the HD texture pack, then restart.\n"
                                          "Do not distribute this folder with app builds or source releases.\n")
        staging.replace(output)
        if args.install:
            install(output, args.settings)
        return report
    except BaseException:
        shutil.rmtree(staging, ignore_errors=True)
        raise


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("hd_source", type=Path, help="USA Wind Waker HD WUX/WUD, or decrypted content folder")
    p.add_argument("--gc-disc", type=Path, default=SUPPORT / "GZLE01.iso", help="GameCube GZLE01 rev 0 ISO")
    p.add_argument("--disc-key", type=Path, help="WUD disc key file (default: game.key beside image)")
    p.add_argument("--common-key", type=Path, help="common key file to decrypt the disc's own ticket")
    p.add_argument("--title-key", type=Path, help="disc-specific decrypted title key file, instead of common key")
    p.add_argument("--output", type=Path, default=SUPPORT / "Load/Textures/WWHD", help="new output folder")
    p.add_argument("--fallback-pack", type=Path, help="existing pack for keys not replaced by the HD disc")
    p.add_argument("--install", action="store_true", help="select the completed pack for the next Mac app launch")
    p.add_argument("--settings", type=Path, default=SUPPORT / "settings.ini")
    p.add_argument("--only", action="append", help="limit to HD resource paths containing this text (for development)")
    args = p.parse_args()
    try:
        report = build(args)
    except (OSError, ValueError, ImportError) as ex:
        print(f"Import failed: {ex}", file=sys.stderr)
        return 1
    print(f"Created {report['replacement_count']} replacements; {len(report['conflicts_skipped'])} conflicts skipped.\n"
          + (f"Added {report['fallback']['texture_count']} fallback replacements.\n" if report['fallback'] else "")
          + f"Pack: {args.output.expanduser().resolve()}\n"
          + ("Selected for the next app launch.\n" if args.install else "Choose this folder in Display > HD texture pack.\n"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
