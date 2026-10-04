#!/usr/bin/env python3
"""Apply the checked-in runtime delta to its exact pinned dependency base.

Accept only a clean checkout or exactly the recorded delta, including on
repeat builds. Never reset, overwrite, or stash other local changes.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys


def git(checkout, *args):
    return subprocess.check_output(["git", "-C", str(checkout), *args])


def apply(checkout, base, patch, expected_hash, check_only=False):
    delta = patch.read_bytes()
    if hashlib.sha256(delta).hexdigest() != expected_hash:
        raise ValueError("runtime patch checksum differs from the manifest")
    if git(checkout, "rev-parse", "HEAD").decode().strip() != base:
        raise ValueError(f"runtime patch requires RecompCore {base}")
    current = git(checkout, "diff", "--no-ext-diff", "--no-renames", "--binary", "--full-index", "HEAD", "--")
    if current == delta:
        return "already applied"
    if current:
        raise ValueError("RecompCore has other local changes; preserve them and use a separate checkout")
    subprocess.run(["git", "-C", str(checkout), "apply", "--check", str(patch)], check=True)
    if not check_only:
        subprocess.run(["git", "-C", str(checkout), "apply", str(patch)], check=True)
        if git(checkout, "diff", "--no-ext-diff", "--no-renames", "--binary", "--full-index", "HEAD", "--") != delta:
            raise ValueError("applied runtime diff differs from the recorded patch")
    return "ready" if check_only else "applied"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--checkout", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    lock = json.loads((root / "config/dependencies.lock.json").read_text())
    entry = next(d for d in lock["dependencies"] if d["id"] == "recompcore")
    pin = entry["sha"]
    checkout = (args.checkout or root / "ref/recompcore").resolve()
    # A pin that already contains the runtime delta (the Windows line's fork
    # commits) names no working-tree patch: the checkout must be that commit
    # exactly.
    if "working_tree_patch_manifest" not in entry:
        if git(checkout, "rev-parse", "HEAD").decode().strip() != pin:
            raise ValueError(f"ref/recompcore is not at the pinned {pin}")
        if git(checkout, "status", "--porcelain", "--untracked-files=no"):
            raise ValueError("ref/recompcore has local changes; the build must use the pinned source exactly")
        print("RecompCore runtime patch: none for this pin (the pinned commit carries it)")
        return
    manifest = json.loads((root / entry["working_tree_patch_manifest"]).read_text())
    if manifest["base_sha"] != pin:
        raise ValueError("runtime patch base differs from the dependency lock")
    patch = root / manifest["patch"]
    print("RecompCore runtime patch:", apply(checkout, pin, patch, manifest["sha256"], args.check))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, subprocess.CalledProcessError) as error:
        sys.exit(f"runtime patches: {error}")
