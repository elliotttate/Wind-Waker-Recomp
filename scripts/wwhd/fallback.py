"""Compose a local fallback pack without overriding any primary texture key."""
from collections import defaultdict
import hashlib
from pathlib import Path
import re
import shutil

from .formats import FormatError

# Aurora accepts Dolphin's optional mipmapped marker and arbitrary-mipmap
# suffix. Neither changes the source texture key. PNG/DDS also share a key.
_NAME = re.compile(
    r"^tex1_(\d+)x(\d+)_(?:m_)?([0-9a-f]{16})(?:_([0-9a-f]{16}))?"
    r"_(\d+)(?:_arb)?(?:_mip(\d+))?\.(png|dds)$", re.I)


def texture_file(name: str):
    match = _NAME.fullmatch(name)
    if not match:
        return None
    w, h, pixels, palette, fmt, mip, extension = match.groups()
    if not int(w) or not int(h):
        return None
    key = f"tex1_{int(w)}x{int(h)}_{pixels.lower()}"
    if palette:
        key += "_" + palette.lower()
    return key + "_" + str(int(fmt)), int(mip or 0), extension.lower()


def add_fallback(destination: Path, primary_files: list[str], source: Path,
                 excluded_keys=()) -> dict:
    """Copy only missing keys, with their matching sidecars, into destination.

    Scan nested pack folders rather than depending on a particular Dolphin
    distribution layout. Only texture files are copied; other packaged files
    are not part of a replacement pack.
    """
    source = source.expanduser().resolve()
    if not source.is_dir():
        raise FormatError("fallback pack must be an existing folder")
    primary = {parsed[0] for name in primary_files if (parsed := texture_file(name))}
    excluded = set(excluded_keys)
    groups = defaultdict(list)
    unrecognized = []
    for path in sorted(source.rglob("*")):
        if not path.is_file() or path.suffix.lower() not in (".png", ".dds"):
            continue
        parsed = texture_file(path.name)
        if parsed:
            groups[parsed[0]].append((path, parsed[1], parsed[2]))
        elif path.name.startswith("tex1_"):
            unrecognized.append(path.relative_to(source).as_posix())
    base_keys = {key for key, files in groups.items() if any(mip == 0 for _, mip, _ in files)}
    if not base_keys:
        raise FormatError("fallback folder contains no supported Dolphin texture filenames")
    folder = destination / "fallback"
    folder.mkdir()
    copied, duplicates = [], []
    for key in sorted(base_keys - primary - excluded):
        bases = [(p, extension) for p, mip, extension in groups[key] if mip == 0]
        base, extension = bases[0]
        if len(bases) > 1:
            duplicates.append({"key": key, "selected": base.relative_to(source).as_posix(),
                               "ignored": [p.relative_to(source).as_posix() for p, _ in bases[1:]]})
        files = [(base, 0)]
        # Use only the selected base file's own sidecars. Mixing mip chains
        # from another base image could change its colors or transparency.
        for p, mip, ext in groups[key]:
            if (mip and ext == extension and p.parent == base.parent and
                    p.stem == base.stem + f"_mip{mip}"):
                files.append((p, mip))
        hashes = {}
        for path, mip in files:
            name = key + (f"_mip{mip}" if mip else "") + "." + extension
            target = folder / name
            shutil.copy2(path, target)
            with target.open("rb") as f:
                hashes["fallback/" + name] = hashlib.file_digest(f, "sha256").hexdigest()
        copied.append({"key": key, "source": base.relative_to(source).as_posix(), "files_sha256": hashes})
    return {"source": str(source), "texture_count": len(copied),
            "file_count": sum(len(t["files_sha256"]) for t in copied),
            "primary_overlap_count": len(base_keys & primary),
            "excluded_keys": sorted(base_keys & excluded),
            "duplicates": duplicates, "unrecognized_files": unrecognized,
            "textures": copied}
