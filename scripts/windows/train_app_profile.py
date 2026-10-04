#!/usr/bin/env python3
"""Record the app's optimization profile, windows/pgo/app.profdata.

  python scripts/windows/train_app_profile.py DISC.iso [--out build/windows]

Run after scripts/windows/build.py (it uses that build's game files and game
module, build/windows/game and build/windows/composite). BlueWake.exe is built
instrumented (build/windows/app-pgogen: the builder's app options plus
-fprofile-instr-generate), then plays what the builder's training plays - a new
game's opening to player control on Outset, Link running, and the tour of the
game (towns, islands, dungeons, the sea) - in a window with the real renderer
and Smooth Motion, so the GX worker, Smooth Motion's helper and the render
worker run as they do in play. The counts are merged (sparse) into
windows/pgo/app.profdata, which build.py then compiles the app with, with
ThinLTO.

The profile counts the app's own code only (BlueWake.exe: the host, Aurora,
the GX front end): no game code or data. A window opens for about ten minutes;
a new card in its own folder, the player's saves are never touched. Run it
again when the app's hot code changes a lot: functions changed since are
compiled without counts.
"""
import argparse
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import build  # noqa: E402  (scripts/windows/build.py)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("disc", type=Path, help="your GZLE01 revision 0 disc image (the one build.py used)")
    parser.add_argument("--out", type=Path, default=build.ROOT / "build/windows")
    parser.add_argument("--march", default="x86-64-v3")
    parser.add_argument("--jobs", type=int, default=build.default_jobs())
    args = parser.parse_args()
    args.console = False
    args.jobs_auto = False
    args.no_app_pgo = True

    b = build.Builder(args)
    b.check_tools()
    b.iso = args.disc.resolve()
    module = args.out / "composite" / build.MODULE
    if not module.exists() or not (args.out / "game/main.dol").exists():
        build.die(f"run scripts/windows/build.py first: {module} or the game files are missing")
    llvm_profdata = Path(b.env["PATH"].split(";")[0]) / "llvm-profdata.exe"

    build.step("the app, instrumented")
    b.configure_app(args.out / "app-pgogen", instrument=True)
    exe = b.build_app()

    build.step("the opening and the tour, in a window")
    run = args.out / "app-pgo-train"
    if run.exists():
        for old in run.rglob("*"):
            if old.is_file():
                old.unlink()
    raw = b.training_run(exe, module, run, None, tour=True, headless=False)

    build.step("the profile")
    target = build.ROOT / "windows/pgo/app.profdata"
    target.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(llvm_profdata), "merge", "--sparse", "-o", str(target), *map(str, raw)], check=True)
    print(f"{target} ({target.stat().st_size // 1024} KB) from {len(raw)} recording(s)")


if __name__ == "__main__":
    main()
