#!/usr/bin/env python3
"""Generate private optimization profiles from the player's disc on this Mac.

Called after extraction, translation and optional mods. No downloaded save or
pretrained game profile is used. The optional --save is copied before playback.
A completed run is reusable only for identical source, toolchain and input data.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import signal
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]


def run(command, log, *, env=None, timeout=7200):
    print(f"training: {log.stem} (log: {log})", flush=True)
    start = time.monotonic()
    with log.open("w") as stream:
        process = subprocess.Popen([str(x) for x in command], cwd=ROOT,
                                   stdout=stream, stderr=subprocess.STDOUT, env=env, start_new_session=True)
        try:
            while True:
                try:
                    status = process.wait(timeout=15)
                    break
                except subprocess.TimeoutExpired:
                    elapsed = int(time.monotonic() - start)
                    # Report real compiler units/retraces when the child logs
                    # them. Elapsed time stays useful when no total is known.
                    with log.open("rb") as recent:
                        recent.seek(max(0, log.stat().st_size - 32768))
                        tail = recent.read().decode(errors="replace")
                    units = re.findall(r"\[(\d+/\d+)\]", tail)
                    retraces = re.findall(r"\bretraces?=(\d+)", tail)
                    detail = (f", units {units[-1]}" if units else
                              f", last logged retrace {retraces[-1]}" if retraces else "")
                    print(f"training: {log.stem}, elapsed {elapsed // 60}m {elapsed % 60}s{detail}", flush=True)
                    if elapsed > timeout:
                        raise RuntimeError(f"{log.stem} exceeded {timeout // 60} minutes; see {log}")
            if status:
                print(log.read_text(errors="replace")[-5000:], file=sys.stderr)
                raise RuntimeError(f"{log.stem} failed ({status}); see {log}")
        finally:
            # A direct child can exit before its descendants. Check the
            # process group independently, including children ignoring TERM.
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            deadline = time.monotonic() + 10
            while True:
                process.poll()  # reap the direct child as soon as it exits
                try:
                    os.killpg(process.pid, 0)
                except ProcessLookupError:
                    break
                if time.monotonic() >= deadline:
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    break
                time.sleep(0.1)
            process.wait()
    print(f"training: {log.stem} complete in {int(time.monotonic() - start)}s", flush=True)


def fingerprint(args, compiler):
    digest = hashlib.sha256(compiler.encode())
    # The source includes the generated base and optional mod variants. Host
    # and recipe edits also invalidate the training result.
    roots = [args.out / "composite-src", ROOT / "runtime/host", ROOT / "cmake/composite",
             ROOT / "scripts/builder/training", ROOT / "apple/ios/dsp_generated"]
    for root in roots:
        for path in sorted(root.rglob("*")):
            if path.is_file():
                digest.update(str(path.relative_to(root)).encode())
                digest.update(path.read_bytes())
    digest.update(Path(__file__).read_bytes())
    digest.update((ROOT / "apple/ios/src/dsp_common_shim.cpp").read_bytes())
    digest.update(subprocess.check_output(["git", "-C", str(ROOT / "ref/recompcore"), "rev-parse", "HEAD"]))
    # The renderer delta is applied on top of the pinned commit. Its recorded
    # checksum must also invalidate profiles trained before a runtime change.
    digest.update((ROOT / "config/recompcore-patches.json").read_bytes())
    with args.disc.open("rb") as disc:
        for chunk in iter(lambda: disc.read(4 * 1024 * 1024), b""):
            digest.update(chunk)
    for path in [args.out / "game/main.dol", *sorted((args.out / "game/rels").glob("*"))]:
        digest.update(path.read_bytes())
    if args.save:
        digest.update(args.save.read_bytes())
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disc", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--save", type=Path, help="optional personal BlueWake .card container; only a copy is used")
    args = parser.parse_args()
    system = platform.system()
    if system == "Darwin":
        cc = ["xcrun", "clang"]
        profdata = ["xcrun", "llvm-profdata"]
        arch_flag = ["-DCMAKE_OSX_ARCHITECTURES=arm64"]
        module_suffix = ".dylib"
        extra_linker = "-Wl,-no_compact_unwind"
    elif system == "Linux":
        cc = ["clang"]
        profdata = ["llvm-profdata"]
        arch_flag = []
        module_suffix = ".so"
        extra_linker = ""
    else:
        parser.error("local training requires macOS (Apple Silicon) or Linux x86-64")
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    args.disc, args.out = args.disc.resolve(), args.out.resolve()
    if args.save:
        args.save = args.save.resolve()
    required = [args.disc, args.out / "game/main.dol", args.out / "composite-src/generated.h",
                ROOT / "ref/recompcore/GXRuntime/CMakeLists.txt",
                ROOT / "ref/recompcore/Data/Sys/GC/dsp_rom.bin",
                ROOT / "ref/recompcore/Data/Sys/GC/dsp_coef.bin"]
    if args.save:
        required.append(args.save)
    for path in required:
        if not path.is_file():
            parser.error(f"missing {path}; run the builder's source stages first")
    runtime_patch = subprocess.run(
        [sys.executable, str(ROOT / "scripts/apply_recompcore_patches.py")],
        capture_output=True, text=True)
    if runtime_patch.returncode:
        parser.error(runtime_patch.stderr.strip())
    work = args.out / "pgo-local"
    work.mkdir(parents=True, exist_ok=True)
    logs = work / "logs"
    logs.mkdir(exist_ok=True)
    compiler = subprocess.check_output([*cc, "--version"], text=True)
    key = fingerprint(args, compiler)
    receipt = work / "training.json"
    outputs = [work / "composite.profdata", work / "host.profdata"]
    if receipt.exists():
        try:
            previous = json.loads(receipt.read_text())
        except (ValueError, OSError):
            previous = {}
        if previous.get("fingerprint") == key and all(p.is_file() for p in outputs):
            hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in outputs}
            if hashes == previous.get("profiles"):
                print("training: reusing verified local profiles for these inputs", flush=True)
                return
    donor = ROOT / "ref/recompcore"
    host_build = work / "host"
    module_build = work / "composite"
    common = ["-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", *arch_flag,
              "-DCMAKE_C_FLAGS=-fprofile-instr-generate", "-DCMAKE_CXX_FLAGS=-fprofile-instr-generate"]
    run(["cmake", "-S", ROOT / "scripts/builder/training", "-B", host_build, *common,
         "-DCMAKE_EXE_LINKER_FLAGS=-fprofile-instr-generate", "-DAURORA_DAWN_PROVIDER=package",
         "-DAURORA_SDL3_PROVIDER=vendor", "-DAURORA_SDL3_LINKAGE=static", "-DAURORA_DAWN_LINKAGE=static",
         *(["-DSDL_UNIX_CONSOLE_BUILD=ON"] if system == "Linux" else [])], logs / "host-configure.log")
    run(["cmake", "--build", host_build, "--target", "bluewake_host", "-j", args.jobs], logs / "host-build.log")
    # Frontend instrumentation uses the same function counters independently
    # of optimization level. O0 shortens this disposable first compilation.
    run(["cmake", "-S", ROOT / "cmake/composite", "-B", module_build, *common,
         f"-DCMAKE_SHARED_LINKER_FLAGS=-fprofile-instr-generate{(' ' + extra_linker) if extra_linker else ''}", "-DCOMPOSITE_OPTIMIZATION_LEVEL=0",
         f"-DCOMPOSITE_DIR={args.out / 'composite-src'}", f"-DGXRUNTIME_DIR={donor / 'GXRuntime'}",
         f"-DABI_DIR={donor / 'Source/Core/Core/PowerPC/StaticRecomp'}"], logs / "composite-configure.log")
    run(["cmake", "--build", module_build, "-j", args.jobs], logs / "composite-build.log")
    # A unique directory isolates raw profiles and card writes from earlier
    # runs and from the user's normal saves. Never merge stale raw profiles.
    import tempfile
    session = Path(tempfile.mkdtemp(prefix="run-", dir=work))
    card = session / "training.card"
    if args.save:
        shutil.copy2(args.save, card)
    environment = {k: v for k, v in os.environ.items()
                   if not k.startswith(("BLUEWAKE_", "DOL_", "LLVM_PROFILE_"))}
    environment.update({
        "LLVM_PROFILE_FILE": str(session / "%m-%p.profraw"),
        "BLUEWAKE_ROOT": str(ROOT), "BLUEWAKE_RENDERER": "headless",
        "BLUEWAKE_DOL": str(args.out / "game/main.dol"), "BLUEWAKE_DISC": str(args.disc),
        "BLUEWAKE_RELS_DIR": str(args.out / "game/rels"), "BLUEWAKE_CARD_PATH": str(card),
        "BLUEWAKE_DSP_IROM": str(donor / "Data/Sys/GC/dsp_rom.bin"),
        "BLUEWAKE_DSP_COEF": str(donor / "Data/Sys/GC/dsp_coef.bin"),
        "BLUEWAKE_MAX_BLOCKS": "100000000000", "BLUEWAKE_MAX_RETRACES": "23000",
        "BLUEWAKE_CYCLE_CAP": "16384", "BLUEWAKE_PLAYER_PROBE": "1",
        "BLUEWAKE_DSP_MODE": "hle",
        "BLUEWAKE_PAD_BUTTONS": "0x0100", "BLUEWAKE_PAD_PULSE_ON_TITLE_READY": "1",
        "BLUEWAKE_PAD_PULSE_LENGTH": "2", "BLUEWAKE_PAD_CONFIRM_EVENT": "any",
        "BLUEWAKE_PAD_SCRIPT": ",".join(f"{n}:0x0100:2" for n in range(17800, 22001, 150)),
    })
    run([host_build / "host/bluewake_host", module_build / f"gGZLE01_recomp{module_suffix}"],
        logs / "playback.log", env=environment, timeout=3600)
    playback = (logs / "playback.log").read_text(errors="replace")
    if "[player-milestone] control-admitted" not in playback:
        raise RuntimeError("training did not reach player control; profile rejected (see playback.log)")
    profiles = sorted(session.glob("*.profraw"))
    if len(profiles) < 2:
        raise RuntimeError("both host and game module must produce raw profiles")
    # A combined file is valid for both targets: Clang picks matching function
    # names/hashes. Keep conventional output names for the builder interface.
    run([*profdata, "merge", "-o", outputs[0], *profiles], logs / "profile-merge.log")
    stats = subprocess.check_output([*profdata, "show", "--all-functions", str(outputs[0])], text=True)
    executed = re.findall(r"(?m)^  func_[0-9A-Fa-f]+:\n    Hash: [^\n]+\n    Counters: [^\n]+\n    Function count: ([0-9]+)", stats)
    if not any(int(count) > 0 for count in executed):
        raise RuntimeError("merged profile has no executed translated game function counters")
    shutil.copy2(outputs[0], outputs[1])
    receipt.write_text(json.dumps({"fingerprint": key, "compiler": compiler,
        "route": "local boot through player-control, 23000 retraces", "performance_verified": False,
        "dsp_mode": "hle", "renderer": "headless",
        "executed_translated_functions": sum(int(count) > 0 for count in executed),
        "profiles": {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in outputs}}, indent=2) + "\n")
    print("training: local game counters generated; optimized device performance still requires testing", flush=True)


if __name__ == "__main__":
    def interrupted(_signum, _frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupted)
    try:
        main()
    except KeyboardInterrupt:
        sys.exit("training: interrupted; completed compile objects are retained for resuming")
    except (RuntimeError, subprocess.CalledProcessError) as error:
        sys.exit(f"training: {error}")
