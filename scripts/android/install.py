#!/usr/bin/env python3
"""Install a BlueWake Android build on a device over adb, with the game files it reads.

    python scripts/android/install.py [--out build/android] [--serial SERIAL] [options]

Installs OUT/WindWakerRecomp.apk, then pushes the game files the builder prepared from
your disc to the app's folder on the device (/sdcard/Android/data/<package>/files/game):
main.dol, the 415 RELs and the disc image (about 1.4 GB, pushed once; a copy of the same
size already there is kept). These files come from your own disc: they stay on your PC and
your device.

Saves, settings and session logs are in /sdcard/Android/data/<package>/files. Uninstalling
the app deletes that folder, saves included: back up GZLE01.card first (--pull-saves).
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
ACTIVITY = "dev.bluewake.android.BlueWakeActivity"
DEFAULT_PACKAGE = "dev.bluewake.BlueWake"


def find_adb(explicit):
    if explicit:
        return str(explicit)
    for candidate in (shutil.which("adb"),
                      os.environ.get("ANDROID_HOME") and Path(os.environ["ANDROID_HOME"]) / "platform-tools/adb.exe",
                      Path(os.environ.get("LOCALAPPDATA", "")) / "Android/Sdk/platform-tools/adb.exe"):
        if candidate and Path(candidate).exists():
            return str(candidate)
    sys.exit("adb not found: pass --adb PATH")


def pick_serial(adb, explicit):
    if explicit:
        return explicit
    out = subprocess.check_output([adb, "devices", "-l"], text=True)
    devices = [line.split() for line in out.splitlines()[1:] if " device " in f" {line} "]
    # A phone, not a headset, when both are plugged in.
    phones = [d for d in devices if not any(w.startswith("model:Quest") for w in d)]
    if len(phones) != 1:
        sys.exit(f"pass --serial: {len(phones)} candidate devices\n{out}")
    return phones[0][0]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, default=ROOT / "build/android")
    parser.add_argument("--serial", help="adb serial of the device (default: the one phone connected)")
    parser.add_argument("--adb", type=Path, help="adb executable")
    parser.add_argument("--apk", type=Path, help="the APK (default OUT/WindWakerRecomp.apk)")
    parser.add_argument("--no-apk", action="store_true", help="push the game files only")
    parser.add_argument("--no-game", action="store_true", help="install the APK only")
    parser.add_argument("--env", action="append", default=[], metavar="NAME=VALUE",
                        help="launch settings written to files/launch.env (repeat; --env '' clears it)")
    parser.add_argument("--launch", action="store_true", help="start the game afterwards")
    parser.add_argument("--pull-saves", type=Path, metavar="DIR", help="copy the memory card and settings to DIR")
    parser.add_argument("--package", help="the application id (default: the one OUT was built with)")
    args = parser.parse_args()

    # The application id the build used (build.py --package), so the files go
    # where that app looks for them.
    package = args.package
    provenance_file = args.out / "BuilderProvenance.json"
    if package is None and provenance_file.exists():
        package = json.loads(provenance_file.read_text()).get("package")
    package = package or DEFAULT_PACKAGE
    remote = f"/sdcard/Android/data/{package}/files"

    adb = find_adb(args.adb)
    serial = pick_serial(adb, args.serial)

    def run(*command, check=True, capture=False):
        full = [adb, "-s", serial, *[str(c) for c in command]]
        if capture:
            return subprocess.run(full, check=check, capture_output=True, text=True).stdout
        print("  " + " ".join(str(c) for c in command)[:200], flush=True)
        return subprocess.run(full, check=check)

    def remote_size(path):
        out = run("shell", f"stat -c %s '{path}' 2>/dev/null || echo -1", capture=True).strip()
        try:
            return int(out.splitlines()[-1])
        except (ValueError, IndexError):
            return -1

    def remote_digest(path):
        out = run("shell", f"sha1sum '{path}' 2>/dev/null || echo -", capture=True).strip()
        return out.split()[0] if out else "-"

    def local_digest(path):
        digest = hashlib.sha1()
        with open(path, "rb") as file:
            for block in iter(lambda: file.read(1 << 22), b""):
                digest.update(block)
        return digest.hexdigest()

    def same_file(local, remote_path):
        """The device's copy is this file: the same size, then the same SHA-1."""
        size = local.stat().st_size
        return remote_size(remote_path) == size and remote_digest(remote_path) == local_digest(local)

    print(f"device {serial}, app {package}")
    if args.pull_saves:
        args.pull_saves.mkdir(parents=True, exist_ok=True)
        for name in ("GZLE01.card", "sram.bin", "settings.ini"):
            if remote_size(f"{remote}/{name}") >= 0:
                run("pull", f"{remote}/{name}", args.pull_saves / name)
        return

    if not args.no_apk:
        apk = args.apk or args.out / "WindWakerRecomp.apk"
        if not apk.exists():
            sys.exit(f"{apk} not found: build it with scripts/android/build.py")
        print(f"installing {apk.name} ({apk.stat().st_size >> 20} MB)")
        run("install", "-r", "-g", apk)

    run("shell", f"mkdir -p '{remote}/game/rels'")
    if not args.no_game:
        game = args.out / "game"
        rels = sorted((game / "rels").glob("*.rel"))
        if not (game / "main.dol").exists() or len(rels) != 415:
            sys.exit(f"{game} does not hold main.dol and the 415 RELs: run scripts/android/build.py first")
        provenance = args.out / "BuilderProvenance.json"
        iso = Path(json.loads(provenance.read_text())["iso"]) if provenance.exists() else None
        if iso is None or not iso.exists():
            iso = next((p for p in (ROOT.parent / "disc/GZLE01.iso", args.out / "disc/GZLE01.iso") if p.exists()), None)
        if iso is None:
            sys.exit("the disc image was not found (BuilderProvenance.json names it)")
        print("pushing the prepared game files")
        if not same_file(game / "main.dol", f"{remote}/game/main.dol"):
            run("push", game / "main.dol", f"{remote}/game/main.dol")
        else:
            print("  main.dol is already there")
        # Every REL by name and size (one listing), so that an interrupted push
        # is finished rather than taken for a complete one.
        listing = run("shell", f"cd '{remote}/game/rels' 2>/dev/null && stat -c '%n %s' *.rel 2>/dev/null; true",
                      capture=True)
        there = dict(line.rsplit(" ", 1) for line in listing.splitlines() if line.count(" ") >= 1)
        if any(there.get(rel.name) != str(rel.stat().st_size) for rel in rels):
            run("push", game / "rels", f"{remote}/game/")
        else:
            print("  the RELs are already there")
        if not same_file(iso, f"{remote}/game/GZLE01.iso"):
            print(f"pushing the disc image ({iso.stat().st_size >> 20} MB)")
            run("push", iso, f"{remote}/game/GZLE01.iso")
        else:
            print("  the disc image is already there")
        # The folders adb makes in the app's storage are shell's (group
        # ext_data_rw, 2770). On the Fold 7 (Android 16) a newly installed app
        # could not search them and stopped with "the prepared game executable
        # is missing"; the files themselves were readable.
        run("shell", f"chmod 2775 '{remote}/game' '{remote}/game/rels'")

    if args.env:
        lines = [e for e in args.env if e]
        local = args.out / "launch.env"
        local.write_text("".join(f"{e}\n" for e in lines), encoding="utf-8", newline="\n")
        if lines:
            run("push", local, f"{remote}/launch.env")
        else:
            run("shell", f"rm -f '{remote}/launch.env'")

    if args.launch:
        run("shell", "am", "start", "-n", f"{package}/{ACTIVITY}")
    print("done")


if __name__ == "__main__":
    main()
