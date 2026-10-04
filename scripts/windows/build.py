#!/usr/bin/env python3
"""BlueWake Builder for Windows: turn your own game disc into your own game, on your PC.

    python scripts/windows/build.py DISC [--out build/windows] [options]

DISC is your own The Wind Waker (GameCube, USA GZLE01 revision 0) image: an .iso or
.gcm, or a Dolphin .rvz/.wia/.gcz/.ciso/.nfs image (converted to an ISO with
nodtool, which is built from crates.io with Rust's cargo on first use).

The Windows counterpart of scripts/builder/build.sh with the bluewake profile.
It reads that profile's pins (RecompCore, DolRecomp) and verified source digest,
so both builders translate the same code; docs/WINDOWS.md explains the port.

Steps, each logged under OUT/logs:
  1 tools        Visual Studio's clang and the Windows SDK, CMake 3.25+, Ninja, git
  2 dependencies the pinned RecompCore and DolRecomp sources (ref/recompcore)
  3 disc         check the disc; convert a compressed image to an ISO
  4 extract      main.dol and the 415 RELs from the disc (the disc is verified)
  5 translate    the game's PowerPC code to C (DolRecomp)
  6 generate     the composite source, compared with the verified digest
  7 mods         widescreen 16:9 and 16:10 and Better Wind Waker's options (--no-mods skips)
  8 compile      the game module, gGZLE01_recomp.dll (the long step)
  9 app          BlueWake.exe, Aurora (Direct3D 12 through Dawn), SDL3 and the DSP
 10 package      the app folder OUT/BlueWake, ready to run

The app folder contains code translated from YOUR disc and a copy of the disc:
it is yours alone. Never share or upload it. Your saves live in
%APPDATA%\\BlueWake, outside the build, so rebuilding never touches them.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
PROFILE = ROOT / "scripts/builder/profiles/bluewake.sh"
MODULE = "gGZLE01_recomp.dll"
# nodtool (https://github.com/encounter/nod) converts Dolphin's compressed
# formats to ISO and checks the result against Redump.
NODTOOL_VERSION = "2.0.0-alpha.9"
DISC_FORMATS = (".rvz", ".wia", ".gcz", ".ciso", ".nfs", ".wbfs", ".tgc")
GC_MAGIC = 0xC2339F3D


class BuildError(Exception):
    pass


def die(message):
    raise BuildError(message)


def step(title):
    print(f"\n==> {title}", flush=True)


def default_jobs(gb_per_job=2.5):
    """All cores, but no more parallel compiles than memory allows: running out
    of commit kills the compiler ("LLVM ERROR: out of memory"; compile_module
    retries). gb_per_job is one compile's peak commit plus room for the
    others' growth; what the chunks take is measured in compile_composite."""
    cores = os.cpu_count() or 8
    try:
        import ctypes

        class MemoryStatus(ctypes.Structure):
            _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong),
                        ("ullTotalPhys", ctypes.c_ulonglong), ("ullAvailPhys", ctypes.c_ulonglong),
                        ("ullTotalPageFile", ctypes.c_ulonglong), ("ullAvailPageFile", ctypes.c_ulonglong),
                        ("ullTotalVirtual", ctypes.c_ulonglong), ("ullAvailVirtual", ctypes.c_ulonglong),
                        ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]
        status = MemoryStatus()
        status.dwLength = ctypes.sizeof(MemoryStatus)
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
            available = min(status.ullAvailPhys, status.ullAvailPageFile)
            return max(1, min(cores, int(available // (gb_per_job * 2**30))))
    except (AttributeError, OSError):
        pass
    return cores


def profile_value(name):
    """A NAME=value pin from the bluewake profile, the builders' one source."""
    match = re.search(rf"^{name}=(\S+)$", PROFILE.read_text(), re.M)
    if not match:
        die(f"{PROFILE} has no {name}")
    return match.group(1)


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 22), b""):
            digest.update(block)
    return digest.hexdigest()


def sync_tree(new, current):
    """Make `current` the same tree as `new`, replacing only files that differ,
    so the unchanged ones keep their timestamps and Ninja does not recompile
    them. `new` is removed."""
    current.mkdir(parents=True, exist_ok=True)
    wanted = set()
    for source in new.rglob("*"):
        rel = source.relative_to(new)
        target = current / rel
        wanted.add(rel)
        if source.is_dir():
            target.mkdir(exist_ok=True)
            continue
        if (target.is_file() and target.stat().st_size == source.stat().st_size
                and target.read_bytes() == source.read_bytes()):
            continue
        os.replace(source, target)
    for target in sorted(current.rglob("*"), reverse=True):
        if target.relative_to(current) not in wanted:
            if target.is_dir():
                shutil.rmtree(target)
            else:
                target.unlink()
    shutil.rmtree(new)


def tree_digest(root):
    """scripts/ios/composite_manifest.py's digest of a generated tree."""
    out = subprocess.check_output([sys.executable, str(ROOT / "scripts/ios/composite_manifest.py"), str(root)],
                                  text=True)
    return out.split()[0]


class Builder:
    def __init__(self, args):
        self.args = args
        self.out = args.out
        self.logs = self.out / "logs"
        self.env = None
        self.recompcore = ROOT / "ref/recompcore"
        self.iso = None

    # --- helpers -------------------------------------------------------
    def run(self, name, command, *, env=None, cwd=None, ninja=False):
        """Run a command with a complete log and progress every 15 seconds."""
        self.logs.mkdir(parents=True, exist_ok=True)
        log = self.logs / f"{name}.log"
        environment = dict(env or self.env or os.environ)
        if ninja:
            environment["NINJA_STATUS"] = "[%f/%t] "
        start = time.monotonic()
        print(f"  {name} (log: {log})", flush=True)
        with open(log, "wb") as stream:
            process = subprocess.Popen([str(c) for c in command], cwd=cwd or ROOT, stdout=stream,
                                       stderr=subprocess.STDOUT, env=environment)
            last = start
            while True:
                try:
                    status = process.wait(timeout=5)
                    break
                except subprocess.TimeoutExpired:
                    pass
                except KeyboardInterrupt:
                    process.terminate()
                    raise
                now = time.monotonic()
                if now - last >= 15:
                    last = now
                    detail = ""
                    try:
                        with open(log, "rb") as recent:
                            recent.seek(max(0, log.stat().st_size - 16384))
                            tail = recent.read()
                        units = re.findall(rb"\[(\d+/\d+)\]", tail)
                        retraces = re.findall(rb"retrace=(\d+)", tail)
                        if units:
                            detail = f", {units[-1].decode()}"
                        elif retraces:
                            detail = f", retrace {retraces[-1].decode()}"
                    except OSError:
                        pass
                    elapsed = int(now - start)
                    print(f"  {name}: {elapsed // 60}m {elapsed % 60:02d}s{detail}", flush=True)
        if status != 0:
            tail = log.read_bytes()[-4000:].decode(errors="replace")
            print(tail, file=sys.stderr)
            die(f"{name} failed (exit {status}); full log {log}")
        elapsed = int(time.monotonic() - start)
        if elapsed >= 60:
            print(f"  {name}: done in {elapsed // 60}m {elapsed % 60:02d}s", flush=True)
        return log

    def git(self, *args, cwd=None):
        return subprocess.check_output(["git", *args], cwd=cwd or ROOT, text=True,
                                       stderr=subprocess.DEVNULL).strip()

    # --- 1 tools -----------------------------------------------------
    def check_tools(self):
        if platform.system() != "Windows":
            die("this builder is for Windows; on a Mac use scripts/builder/build.sh")
        if platform.machine().lower() not in ("amd64", "x86_64"):
            die(f"an x86-64 PC is required (this is {platform.machine()})")
        if sys.version_info < (3, 10):
            die("Python 3.10 or newer is required")
        for tool in ("git", "cmake", "ninja"):
            if shutil.which(tool) is None:
                die(f"missing {tool}: install Git, CMake 3.25+ and Ninja (pip install cmake ninja)")
        version = subprocess.check_output(["cmake", "--version"], text=True).split()[2]
        if tuple(int(x) for x in version.split(".")[:2]) < (3, 25):
            die(f"CMake 3.25 or newer is required (found {version})")
        self.env = self.visual_studio_env()
        clang = shutil.which("clang", path=self.env["PATH"])
        self.clang = clang
        if clang is None:
            die("Visual Studio's clang is missing: in the Visual Studio Installer, add "
                "'C++ Clang Compiler for Windows' (and 'MSBuild support for LLVM')")
        clang_version = subprocess.check_output([clang, "--version"], text=True, env=self.env).splitlines()[0]
        major = int(re.search(r"version (\d+)", clang_version).group(1))
        if major < 17:
            die(f"clang 17 or newer is required ({clang_version})")
        self.clang_version = clang_version
        self.llvm_profdata = shutil.which("llvm-profdata", path=self.env["PATH"])
        if self.llvm_profdata is None and self.train_pgo:
            die("llvm-profdata is missing from Visual Studio's LLVM; add the clang component, or pass --no-train")
        self.check_march()
        print(f"{clang_version}; cmake {version}; ninja {subprocess.check_output(['ninja', '--version'], text=True).strip()}; "
              f"{self.args.jobs} jobs; -march={self.args.march}")

    def visual_studio_env(self):
        """The x64 developer environment of the newest Visual Studio with clang."""
        vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / \
            "Microsoft Visual Studio/Installer/vswhere.exe"
        if not vswhere.exists():
            die("Visual Studio 2022 or newer with the C++ workload is required (vswhere.exe not found)")
        found = json.loads(subprocess.check_output(
            [str(vswhere), "-all", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
             "-format", "json", "-utf8"], text=True, encoding="utf-8"))
        candidates = [Path(v["installationPath"]) for v in found
                      if (Path(v["installationPath"]) / "VC/Tools/Llvm/x64/bin/clang.exe").exists()]
        if not candidates:
            die("no Visual Studio with the C++ workload and its clang was found: in the Visual Studio "
                "Installer, add 'Desktop development with C++' and 'C++ Clang Compiler for Windows'")
        install = sorted(candidates, key=lambda p: [v for v in found if Path(v["installationPath"]) == p][0]
                         .get("installationVersion", ""))[-1]
        vcvars = install / "VC/Auxiliary/Build/vcvars64.bat"
        # vcvars fails on quoted PATH entries or ones with parentheses (some
        # SDK installers add them); keep the clean ones for the build.
        path = [p.strip('"') for p in os.environ.get("PATH", "").split(os.pathsep)]
        base = dict(os.environ, PATH=os.pathsep.join(p for p in path if p and "(" not in p and ")" not in p))
        output = subprocess.run(f'cmd /d /c ""{vcvars}" >nul 2>&1 && set"', capture_output=True, text=True,
                                env=base, shell=False)
        if output.returncode != 0 or "INCLUDE=" not in output.stdout:
            die(f"{vcvars} failed; open a 'x64 Native Tools Command Prompt' and rerun from there")
        env = {}
        for line in output.stdout.splitlines():
            key, sep, value = line.partition("=")
            if sep and key:
                env[key] = value
        env["PATH"] = str(install / "VC/Tools/Llvm/x64/bin") + os.pathsep + env.get("PATH", "")
        print(f"Visual Studio: {install}")
        return env

    def check_march(self):
        """The game module is compiled for --march; refuse a level this CPU lacks."""
        levels = {"x86-64": set(), "x86-64-v2": {"sse4.2", "popcnt"},
                  "x86-64-v3": {"sse4.2", "popcnt", "avx", "avx2", "fma", "bmi1", "bmi2", "movbe", "lzcnt"}}
        if self.args.march not in levels:
            return
        # CPUID and XGETBV directly (__builtin_cpu_supports needs compiler-rt,
        # which the MSVC target does not link); AVX also needs the OS to save
        # the YMM state.
        probe = self.out / "tools/march_probe.c"
        probe.parent.mkdir(parents=True, exist_ok=True)
        probe.write_text(r"""#include <immintrin.h>
#include <intrin.h>
#include <stdio.h>
int main(void) {
    int r[4];
    __cpuid(r, 1);
    const int ecx1 = r[2];
    __cpuidex(r, 7, 0);
    const int ebx7 = r[1];
    __cpuid(r, 0x80000001);
    const int ecx81 = r[2];
    const int os_avx = ((ecx1 >> 27) & 1) && (_xgetbv(0) & 6) == 6;
    printf("sse4.2=%d popcnt=%d movbe=%d fma=%d avx=%d avx2=%d bmi1=%d bmi2=%d lzcnt=%d\n",
           (ecx1 >> 20) & 1, (ecx1 >> 23) & 1, (ecx1 >> 22) & 1, ((ecx1 >> 12) & 1) && os_avx,
           ((ecx1 >> 28) & 1) && os_avx, ((ebx7 >> 5) & 1) && os_avx, (ebx7 >> 3) & 1,
           (ebx7 >> 8) & 1, (ecx81 >> 5) & 1);
    return 0;
}
""")
        exe = probe.with_suffix(".exe")
        subprocess.run([self.clang, "-O1", "-mxsave", str(probe), "-o", str(exe)], env=self.env, check=True,
                       capture_output=True)
        report = subprocess.check_output([str(exe)], text=True).split()
        have = {name for name, bit in (item.split("=") for item in report) if bit == "1"}
        if not levels[self.args.march] <= have:
            fallback = "x86-64-v2" if levels["x86-64-v2"] <= have else "x86-64"
            print(f"this CPU lacks {self.args.march} ({', '.join(sorted(levels[self.args.march] - have))}); "
                  f"using -march={fallback}")
            self.args.march = fallback

    # --- 2 dependencies ------------------------------------------------
    def dependencies(self):
        sha = profile_value("RECOMPCORE_SHA")
        dolrecomp_sha = profile_value("DOLRECOMP_SHA")
        url = profile_value("RECOMPCORE_URL")
        rc = self.recompcore
        if not (rc / ".git").exists():
            if rc.exists() and any(rc.iterdir()):
                die(f"{rc} exists but is not a git checkout: move it aside and rerun")
            rc.mkdir(parents=True, exist_ok=True)
            subprocess.check_call(["git", "init", "-q"], cwd=rc)
            # A new checkout gets the sources as committed (LF), whatever the
            # global core.autocrlf says; an existing one keeps its own setting,
            # which its clean status below depends on.
            subprocess.check_call(["git", "config", "core.autocrlf", "false"], cwd=rc)
            subprocess.check_call(["git", "config", "core.longpaths", "true"], cwd=rc)
        head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=rc, capture_output=True, text=True).stdout.strip()
        if head != sha:
            if self.git("status", "--porcelain", "--untracked-files=no", cwd=rc):
                die(f"{rc} has local changes and is not at {sha}: move it aside and rerun")
            print(f"fetching RecompCore {sha}")
            subprocess.run(["git", "remote", "remove", "bluewake"], cwd=rc, capture_output=True)
            subprocess.check_call(["git", "remote", "add", "bluewake", url], cwd=rc)
            self.run("recompcore-fetch", ["git", "-C", rc, "fetch", "--depth", "1", "bluewake", sha], env=os.environ)
            subprocess.check_call(["git", "checkout", "-q", "--detach", "FETCH_HEAD"], cwd=rc)
        if self.git("rev-parse", "HEAD", cwd=rc) != sha:
            die(f"{rc} is not at {sha}")
        subprocess.check_call(["git", "submodule", "sync", "-q", "--", "DolRecomp"], cwd=rc)
        sub = rc / "DolRecomp"
        current = subprocess.run(["git", "rev-parse", "HEAD"], cwd=sub, capture_output=True, text=True).stdout.strip() \
            if (sub / ".git").exists() else ""
        if current != dolrecomp_sha:
            # The submodule follows its parent's line endings.
            lf = subprocess.run(["git", "config", "--local", "core.autocrlf"], cwd=rc, capture_output=True,
                                text=True).stdout.strip() == "false"
            self.run("dolrecomp-fetch", ["git", "-C", rc, *(["-c", "core.autocrlf=false"] if lf else []),
                                         "submodule", "update", "--init", "--depth", "1", "--", "DolRecomp"],
                     env=os.environ)
            if lf:
                subprocess.check_call(["git", "config", "core.autocrlf", "false"], cwd=sub)
        if self.git("rev-parse", "HEAD", cwd=sub) != dolrecomp_sha:
            die(f"{sub} is not at {dolrecomp_sha}")
        if self.git("status", "--porcelain", "--untracked-files=no", cwd=rc) or \
                self.git("status", "--porcelain", "--untracked-files=no", cwd=sub):
            die(f"{rc} has local changes; the build must use the pinned source exactly")
        print(f"RecompCore {sha}, DolRecomp {dolrecomp_sha}")

    # --- 3 disc ----------------------------------------------------------
    def disc(self):
        source = self.args.disc.resolve()
        if not source.is_file():
            die(f"disc image not found: {source}")
        if source.suffix.lower() in DISC_FORMATS:
            iso = self.out / "disc/GZLE01.iso"
            stamp = self.out / "disc/source.json"
            key = {"source": str(source), "size": source.stat().st_size, "mtime": source.stat().st_mtime_ns,
                   "nodtool": NODTOOL_VERSION}
            if iso.exists() and stamp.exists() and json.loads(stamp.read_text()) == key:
                print(f"reusing {iso}")
            else:
                nodtool = self.nodtool()
                iso.parent.mkdir(parents=True, exist_ok=True)
                pending = iso.with_name("GZLE01.pending.iso")  # nodtool picks the format by extension
                self.run("disc-convert", [nodtool, "--no-color", "convert", source, pending], env=os.environ)
                log = (self.logs / "disc-convert.log").read_text(encoding="utf-8", errors="replace")
                if "Redump:" in log and "\u2705" not in log.split("Redump:")[1].splitlines()[0]:
                    print("warning: nodtool did not match this image to a Redump entry")
                os.replace(pending, iso)
                stamp.write_text(json.dumps(key))
            self.iso = iso
        else:
            self.iso = source
        with open(self.iso, "rb") as disc:
            header = disc.read(0x20)
        if len(header) < 0x20 or int.from_bytes(header[0x1C:0x20], "big") != GC_MAGIC:
            die(f"{self.iso} is not a GameCube disc image")
        if header[:6] != b"GZLE01":
            die(f"this is a GameCube disc, but not The Wind Waker (USA, GZLE01): its id is "
                f"{header[:6].decode(errors='replace')}")
        if header[7] != 0:
            die(f"this is GZLE01 revision {header[7]}; BlueWake supports revision 0 only")
        print(f"disc: {self.iso} (GZLE01 revision 0)")

    def nodtool(self):
        root = ROOT / "build/tools/nodtool"
        exe = root / "bin/nodtool.exe"
        if exe.exists():
            return exe
        if shutil.which("cargo") is None:
            die("this disc image is compressed; converting it needs Rust's cargo (https://rustup.rs) "
                "to build nodtool, or convert it to .iso yourself (Dolphin: right-click the game, "
                "Convert File..., format ISO) and pass the .iso")
        print(f"building nodtool {NODTOOL_VERSION} from crates.io (once)")
        self.run("nodtool-install", ["cargo", "install", "nodtool", "--version", NODTOOL_VERSION, "--locked",
                                     "--root", root], env=os.environ)
        return exe

    # --- tools built from source ---------------------------------------
    def build_dolrecomp(self):
        build = self.out / "dolrecomp"
        # binmode.obj: the CRT opens files in binary mode by default, so the
        # translator writes the same bytes (LF) as on macOS and the generated
        # source matches the verified digest.
        self.run("dolrecomp-configure", ["cmake", "-S", self.recompcore / "DolRecomp", "-B", build, "-G", "Ninja",
                                         "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=clang-cl",
                                         "-DDOLRECOMP_WARNINGS_AS_ERRORS=OFF",
                                         "-DCMAKE_EXE_LINKER_FLAGS=binmode.obj"])
        self.run("dolrecomp-build", ["cmake", "--build", build, "--target", "dolrecomp", "-j", self.args.jobs],
                 ninja=True)
        return build / "dolrecomp.exe"

    APP_PROFILE = ROOT / "windows/pgo/app.profdata"

    def configure_app(self, build=None, instrument=False):
        """The app's build, by default build/windows/app. With the committed
        profile of the app's own code (windows/pgo/app.profdata,
        scripts/windows/train_app_profile.py) it is compiled with it and with
        ThinLTO: on four of the i9's E-cores that took the GX worker's CPU per
        game frame from 16.6-17.2 ms to 13.8-14.3 and the game thread's from
        23.9-24.3 to 22.4-23.2 (2026-10-02). `instrument` builds it to record
        such a profile instead."""
        self.app_build = build or self.out / "app"
        profile, link = "", ""
        if instrument:
            profile = link = "-fprofile-instr-generate"
        elif self.APP_PROFILE.exists() and not getattr(self.args, "no_app_pgo", False):
            # Functions changed since the profile was recorded are compiled
            # without counts (the warnings say so; they are expected).
            profile = (f"-fprofile-instr-use={self.APP_PROFILE.as_posix()} -Wno-profile-instr-unprofiled "
                       "-Wno-profile-instr-out-of-date -Wno-backend-plugin -flto=thin")
            link = "-flto=thin"
        # The app for the same CPU level as the game module: the FIFO worker's
        # matrix work for Smooth Motion needs AVX2 and FMA to keep up (at the
        # baseline level it held the game below 30 FPS on Outset, 2026-09-29).
        self.run("app-configure", [
            "cmake", "-S", ROOT / "windows", "-B", self.app_build, "-G", "Ninja",
            "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++", "-DCMAKE_BUILD_TYPE=Release",
            # Debug information in a PDB beside the build's BlueWake.exe (the
            # package copies only the exe and DLLs), so a crash address names its
            # function; /OPT:REF,ICF keep the code what it is without /DEBUG.
            f"-DCMAKE_C_FLAGS=-march={self.args.march} -g -gcodeview {profile}",
            f"-DCMAKE_CXX_FLAGS=-march={self.args.march} -g -gcodeview {profile}",
            "-DBUILD_TESTING=OFF", f"-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld -Wl,/DEBUG -Wl,/OPT:REF -Wl,/OPT:ICF {link}",
            f"-DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld {link}",
            f"-DBLUEWAKE_WINDOWS_CONSOLE={'ON' if self.args.console else 'OFF'}"])

    # --- 4 extract ---------------------------------------------------------
    def extract(self, iso, game, accept_sha1=None):
        self.run("disc-extract-build", ["cmake", "--build", self.app_build, "--target", "bluewake_disc_extract"],
                 ninja=True)
        env = dict(self.env)
        if accept_sha1:
            env["BLUEWAKE_ACCEPT_DOL_SHA1"] = accept_sha1
        # disc_extract checks the disc id (GZLE01) and the executable's hash
        # (revision 0) and refuses anything else.
        self.run(f"disc-extract{'-' + game.parent.name if accept_sha1 else ''}",
                 [self.app_build / "bluewake_disc_extract.exe", iso, game], env=env)
        rels = len(list((game / "rels").glob("*.rel")))
        if rels != 415:
            die(f"expected 415 RELs in {game / 'rels'}, found {rels}")

    # --- 5 translate -------------------------------------------------------
    def translate(self, dol, out, rels=None, name="translate", sites=()):
        """DolRecomp over main.dol (and the RELs); `sites` is --option-sites FILE
        for Better Wind Waker's options (scripts/mods/build_mods.sh)."""
        pending = out.with_name(out.name + ".new")
        shutil.rmtree(pending, ignore_errors=True)
        pending.mkdir(parents=True)
        self.run(f"{name}-dol", [self.dolrecomp, "--gamecube", "--backend", "c", "--cpu", "gekko",
                                 "--partition-instructions", "4096", *sites, dol, pending / "dol",
                                 "-j", self.args.jobs])
        if rels is not None:
            # With option sites, name the RELs' folder with "/" and a trailing
            # "/": DolRecomp matches a REL's sites (d_a_ship.rel+0x...) to the
            # file name after the last "/" of its path, and on Windows it joins
            # a folder and a file with "\" unless the folder already ends in a
            # separator, so the sites in the RELs would never match.
            rels_arg = rels.as_posix().rstrip("/") + "/" if sites else rels
            self.run(f"{name}-rels", [self.dolrecomp, "--gamecube", "--backend", "c", "--cpu", "gekko",
                                      "--rel-base", "0xC0400000", *sites, rels_arg, pending / "rels",
                                      "-j", self.args.jobs])
        shutil.rmtree(out, ignore_errors=True)
        os.replace(pending, out)

    def composite(self, dol_dir, rels_dir, rels_bin, main_dol, out, name):
        shutil.rmtree(out, ignore_errors=True)
        self.run(name, [sys.executable, ROOT / "scripts/generate_composite.py", "--dol-dir", dol_dir,
                        "--rels-dir", rels_dir, "--rels-bin-dir", rels_bin, "--main-dol", main_dol,
                        "--output-dir", out])

    # --- 6 generate ------------------------------------------------------
    def generate(self):
        o = self.out
        new = o / "composite-src.new"
        self.composite(o / "translated/dol/generated", o / "translated/rels/generated/rels", o / "game/rels",
                       o / "game/main.dol", new, "composite-generate")
        expected = profile_value("COMPOSITE_DIGEST")
        digest = tree_digest(new)
        if digest == expected:
            print(f"composite source digest {digest}: the verified tree")
        elif self.args.accept_new_composite:
            print(f"composite source digest {digest} differs from the verified {expected} (accepted)")
        else:
            die(f"composite source digest {digest} differs from the verified {expected} (wrong disc revision "
                f"or translator?); --accept-new-composite overrides")
        # Keep an identical tree in place: rewriting 750 files would make the
        # compile start over. Mods are part of the recorded inputs.
        inputs = hashlib.sha256()
        inputs.update(f"{digest}\n{int(self.mods)}\n".encode())
        # The Windows source steps (finish_tree) too: a changed step must start
        # from a fresh tree, since each leaves a chunk it has finished as it is.
        # (package_release.py only packages a finished app: not an input.)
        for f in (sorted((ROOT / "scripts/mods").glob("*")) + sorted((ROOT / "scripts/windows").glob("*.py"))
                  + sorted((ROOT / "mods/widescreen").glob("*.gecko"))
                  + [ROOT / "mods/betterww/options.txt", Path(__file__)]):
            if f.is_file() and f.name != "package_release.py":
                inputs.update(f.read_bytes())
        # The guest addresses the host names (direct_calls.py's watch list): a
        # boundary at one must reach the host's edge service, so a host that
        # names a new one (a new hook) needs the chunks' direct calls made
        # again. The addresses, not the host's sources: other host changes do
        # not touch the game source.
        spec = importlib.util.spec_from_file_location("direct_calls", ROOT / "scripts/windows/direct_calls.py")
        direct_calls = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(direct_calls)
        inputs.update(",".join(f"{a:08X}" for a in sorted(direct_calls.watched_addresses())).encode())
        inputs = inputs.hexdigest()
        current = o / "composite-src"
        saved = (o / "composite-final.digest").read_text().strip() if (o / "composite-final.digest").exists() else ""
        same_inputs = (o / "composite-inputs.digest").exists() and \
            (o / "composite-inputs.digest").read_text().strip() == inputs
        if current.exists() and same_inputs and saved and tree_digest(current) == saved:
            shutil.rmtree(new)
            print("the existing composite source is current")
            self.mods_pending = (o / "mods.done").read_text().strip() != "complete" \
                if (o / "mods.done").exists() else self.mods
        else:
            if self.mods and not self.args.source_only:
                # The mods step adds the variants to this tree and puts the
                # result in place. Putting the plain tree there first would
                # rewrite the files the variants change, and Ninja would
                # compile those again on every rerun.
                base = o / "mods/composite-src.base"
                base.parent.mkdir(parents=True, exist_ok=True)
                shutil.rmtree(base, ignore_errors=True)
                os.replace(new, base)
                (base.parent / "variants.started").unlink(missing_ok=True)  # a fresh base (build_mods)
            else:
                self.finish_tree(new)
                sync_tree(new, current)
            (o / "composite-src.digest").write_text(digest + "\n")
            (o / "composite-inputs.digest").write_text(inputs + "\n")
            (o / "composite-final.digest").write_text(tree_digest(current) + "\n")
            (o / "composite-mods.digest").write_text(digest + "\n")
            (o / "mods.done").write_text("pending\n")
            self.mods_pending = self.mods

    # --- 7 mods --------------------------------------------------------------
    def build_mods(self):
        """Widescreen 16:9 and 16:10, Better Wind Waker's options, and each
        widescreen with them, as variants compiled into the same module: the
        steps of scripts/mods/build_mods.sh (docs/MODS.md)."""
        o, m = self.out, self.out / "mods"
        m.mkdir(parents=True, exist_ok=True)
        options = ROOT / "mods/betterww/options.txt"
        # The site list DolRecomp reads, with LF line ends (Python's stdout
        # writes CRLF on Windows).
        listed = subprocess.run([sys.executable, ROOT / "scripts/mods/game_options.py", "sites", options],
                                capture_output=True, check=True).stdout
        (m / "option-sites.txt").write_bytes(listed.replace(b"\r\n", b"\n"))
        sites = ("--option-sites", m / "option-sites.txt")

        for name, gecko in (("widescreen", "GZLE01.gecko"), ("widescreen1610", "GZLE01-16x10.gecko")):
            print(name)
            (m / name).mkdir(exist_ok=True)
            self.run(f"mods-{name}-gecko", [sys.executable, ROOT / "scripts/mods/gecko_apply.py",
                                           ROOT / "mods/widescreen" / gecko, o / "game/main.dol",
                                           m / name / "main.dol", m / name / "runtime.json"])
            self.translate(m / name / "main.dol", m / name / "translated", name=f"mods-{name}-translate")
            self.composite(m / name / "translated/dol/generated", o / "translated/rels/generated/rels",
                           o / "game/rels", m / name / "main.dol", m / name / "composite-src",
                           f"mods-{name}-composite")

        # The game's own executable and modules translated with the option
        # sites; the variants are the chunks that hold a site.
        print("Better Wind Waker options")
        (m / "betterww").mkdir(exist_ok=True)
        self.translate(o / "game/main.dol", m / "betterww/translated", o / "game/rels",
                       name="mods-betterww-translate", sites=sites)
        self.composite(m / "betterww/translated/dol/generated", m / "betterww/translated/rels/generated/rels",
                       o / "game/rels", o / "game/main.dol", m / "betterww/composite-src", "mods-betterww-composite")

        for combo, widescreen in (("combo", "widescreen"), ("combo1610", "widescreen1610")):
            print(f"{widescreen} + Better Wind Waker options")
            (m / combo).mkdir(exist_ok=True)
            self.translate(m / widescreen / "main.dol", m / combo / "translated", name=f"mods-{combo}-translate",
                           sites=sites)
            self.composite(m / combo / "translated/dol/generated", m / "betterww/translated/rels/generated/rels",
                           o / "game/rels", m / widescreen / "main.dol", m / combo / "composite-src",
                           f"mods-{combo}-composite")

        print("variants into the composite source")
        base = m / "composite-src.base"  # the verified tree, from step 6
        # The variants go into the base tree, which is then finished in place.
        # A run stopped after that began left a tree the variants would be
        # added to again, each chunk then differing from its mod's and copied
        # as a variant of every mod (3,740 chunks instead of 65, an instrumented
        # module past the 4 GB a Windows image can be): make the base again.
        started = m / "variants.started"
        if started.exists():
            print("  the last run stopped while adding the variants: the base tree is made again")
            shutil.rmtree(base, ignore_errors=True)
        if not (base / "generated.h").exists():
            self.composite(o / "translated/dol/generated", o / "translated/rels/generated/rels", o / "game/rels",
                           o / "game/main.dol", base, "mods-base-composite")
        # The --mod and --combo specs are colon-separated, and a Windows path
        # has a colon after its drive letter: run in the build directory and
        # name the mod trees relative to it. The mods keep build_mods.sh's
        # order, which numbers them.
        started.write_text("the variants are going into composite-src.base\n")
        self.run("mods-variants", [
            sys.executable, ROOT / "scripts/mods/build_mod_variants.py", "--composite-src", base,
            "--base-dol", o / "game/main.dol",
            "--mod", "widescreen:mods/widescreen/composite-src:mods/widescreen/main.dol:mods/widescreen/runtime.json",
            "--mod", "betterww:mods/betterww/composite-src:game/main.dol",
            "--mod", "widescreen1610:mods/widescreen1610/composite-src:mods/widescreen1610/main.dol:"
                     "mods/widescreen1610/runtime.json",
            "--combo", "widescreen+betterww:mods/combo/composite-src",
            "--combo", "widescreen1610+betterww:mods/combo1610/composite-src",
            "--exclusive", "widescreen,widescreen1610",
            "--options", options, "--rels-bin-dir", o / "game/rels"], cwd=o)
        dst = o / "composite-src"
        (o / "composite-mods.digest").write_text(tree_digest(base) + "\n")
        # The base tree plus the variants, finished, in place of the last
        # build's. Finished before it is synced, so a rerun compares finished
        # chunks with finished ones and replaces (recompiles) only real changes.
        self.finish_tree(base)
        sync_tree(base, dst)
        (o / "composite-final.digest").write_text(tree_digest(dst) + "\n")
        (o / "mods.done").write_text("complete\n")
        started.unlink()

    # --- 7b the last source steps --------------------------------------------
    def finish_tree(self, root):
        """The Windows source steps after the mods, in order: the chunks address
        the guest CPU state at its fixed address (scripts/windows/global_guest_cpu.py),
        run the register save and restore routines inline at their calls
        (scripts/windows/inline_save_restore_gpr.py), include the inline
        helpers (gather-pipe stores, floating point: scripts/windows/chunk_headers.py),
        call across chunks without the chassis loop where the host has nothing
        to do (scripts/windows/direct_calls.py), run J3DModel::calcWeightEnvelopeMtx
        natively where its translation is the verified one
        (scripts/windows/native_skin.py), give every block a copy without the
        per-instruction machinery for when it has prepaid its cycles
        (scripts/windows/fast_blocks.py), take the pc and suffix stores of
        those copies' plain loads and stores into their out-of-line paths
        (scripts/windows/lean_memory.py), run the certified recovered J3D
        transform matrices natively (scripts/mods/prepare_native_j3d.py),
        then the opt-in 60 Hz
        gameplay's timing sites (docs/SIMULATION_60HZ.md; off unless asked
        for), whose manifest hashes the chunks as they end up. Each leaves a
        finished chunk as it is."""
        self.source_step("guest-cpu", "scripts/windows/global_guest_cpu.py", root)
        self.source_step("gpr-inline", "scripts/windows/inline_save_restore_gpr.py", root)
        self.source_step("chunk-headers", "scripts/windows/chunk_headers.py", root)
        self.source_step("direct-calls", "scripts/windows/direct_calls.py", root)
        self.source_step("native-skin", "scripts/windows/native_skin.py", root)
        # Certified game math entry hooks; before prepaid copies/manifests.
        self.source_step("native-game-math", "scripts/windows/native_game_math.py", root)
        self.source_step("fast-blocks", "scripts/windows/fast_blocks.py", root)
        # The copies' plain loads and stores without their pc and suffix stores.
        self.source_step("lean-memory", "scripts/windows/lean_memory.py", root)
        self.source_step("native-j3d", "scripts/mods/prepare_native_j3d.py", root)
        # Certified native entries, the second set (FIFO matrix loads, collision
        # checks, PSMTXMultVecSR, joint matrix calculations): after the prepaid
        # copies they certify, before the manifests that hash the chunks as they end up.
        self.source_step("native-entries", "scripts/windows/native_entries.py", root)
        self.source_step("simulation-prepare", "scripts/mods/prepare_simulation_60hz.py", root)
        # Last: its manifest hashes whole chunk files, as they end up.
        self.source_step("native-math", "scripts/mods/prepare_native_math.py", root)
        print((self.logs / "native-game-math.log").read_text(errors="replace").strip().splitlines()[-1])
        for name in ("guest-cpu", "gpr-inline", "chunk-headers", "direct-calls", "native-skin", "fast-blocks",
                     "lean-memory", "native-j3d", "native-entries", "simulation-prepare", "native-math"):
            print((self.logs / f"{name}.log").read_text(errors="replace").strip().splitlines()[-1])

    def source_step(self, name, script, root):
        """One source step. A run that crashed (a Windows exception status, not an
        error the script reports) is run once more: each step leaves a chunk it
        has finished as it is, so the second run completes the first. Python
        once stopped on an access violation part of the way through
        fast_blocks.py, and the same run again finished."""
        command = [sys.executable, ROOT / script, root]
        try:
            self.run(name, command)
        except BuildError as error:
            status = re.search(r"\(exit (\d+)\)", str(error))
            if status is None or int(status.group(1)) < 0xC0000000:
                raise
            print(f"  {name} crashed (exit 0x{int(status.group(1)):08X}); running it again")
            self.run(name, command)

    def finish_in_place(self):
        """A composite source finished before these steps existed: finish it
        where it is (a no-op once it is)."""
        o = self.out
        src = o / "composite-src"
        if not (o / "composite-mods.digest").exists():
            shutil.copy2(o / "composite-final.digest", o / "composite-mods.digest")
        self.finish_tree(src)
        (o / "composite-final.digest").write_text(tree_digest(src) + "\n")

    # --- 8 compile -----------------------------------------------------------
    def compile_module(self):
        flags = []
        if self.profile is not None:
            # The profile is a compiler input but not a header dependency: its
            # hash in the file name makes Ninja recompile when the counts change.
            # Code the training never ran is optimized as cold; that saves size
            # and costs nothing in the scenes that matter (docs/BUILDER.md).
            flags = [f"-fprofile-instr-use={self.profile.as_posix()}", "-Wno-profile-instr-unprofiled",
                     "-Wno-profile-instr-out-of-date", "-Wno-backend-plugin"]
            print(f"with the optimization profile {self.profile.name}")
        tiered = self.profile is not None and not getattr(self.args, "no_tiered", False)
        cold = self.cold_sources() if tiered else None
        return self.compile_composite(self.out / "composite", self.args.opt_level, flags, [], "composite", cold)

    def cold_sources(self):
        """The chunks whose function the training never ran, listed for
        cmake/composite to compile at -O1 without GVN's memory dependence
        analysis (the module's longest passes on its largest functions), as
        DeepSea compiles its cold actor code: the module compiles in about 15
        minutes instead of 39, and Gohma's room, which no training visits, ran
        as fast as with the whole module at -O2 (2026-10-02; the profile had
        already compiled those chunks for size). --no-tiered compiles them all
        at -O2. A chunk is one function, named func_<its file's address>."""
        stats = subprocess.run([self.llvm_profdata, "show", "--all-functions", self.profile], capture_output=True,
                               text=True).stdout
        counts = {name.upper(): int(count) for name, count in
                  re.findall(r"(?m)^  func_([0-9A-Fa-f]+):\n(?:    .*\n)*?    Function count: (\d+)", stats)}
        src = self.out / "composite-src"
        cold = []
        for path in sorted(src.glob("chunks_*/*.c")):
            address = path.stem.rsplit("_", 1)[-1].upper()
            if counts.get(address) == 0:
                cold.append(path.relative_to(src).as_posix())
        listing = self.out / "composite-cold-sources.txt"
        listing.write_text("\n".join(cold) + "\n", encoding="utf-8")
        print(f"tiered: {len(cold)} chunks the training never ran at -O1")
        return listing

    def compile_composite(self, build, opt_level, extra_flags, extra_link_flags, name, cold=None):
        rc = self.recompcore
        # Each chunk is one very large function, and two LLVM passes are
        # superlinear on it (clang 22, x86-64, measured with -ftime-report):
        # - the SLP vectorizer took 92 percent of a typical large chunk's time;
        #   -fno-slp-vectorize took the 1.8 MB chunks from over 30 minutes each
        #   to about 2. There is little to vectorize across register moves.
        # - the register coalescer took 95 percent of d_a_movie_player's 44
        #   minutes, joining copies into the context pointer's function-long
        #   live interval over and over. Capping that per large interval took
        #   it to about 2 minutes, at the cost of a few register copies.
        flags = " ".join([f"-march={self.args.march}", "-fno-slp-vectorize",
                          "-mllvm", "-large-interval-freq-threshold=10", *extra_flags])
        link_flags = " ".join(["-fuse-ld=lld", *extra_link_flags])
        self.run(f"{name}-configure", [
            "cmake", "-S", ROOT / "cmake/composite", "-B", build, "-G", "Ninja", "-DCMAKE_C_COMPILER=clang",
            "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_C_FLAGS={flags}", f"-DCMAKE_SHARED_LINKER_FLAGS={link_flags}",
            f"-DCOMPOSITE_OPTIMIZATION_LEVEL={opt_level}", f"-DCOMPOSITE_DIR={self.out / 'composite-src'}",
            f"-DGXRUNTIME_DIR={rc / 'GXRuntime'}", f"-DABI_DIR={rc / 'Source/Core/Core/PowerPC/StaticRecomp'}",
            f"-DCOMPOSITE_COLD_SOURCES_FILE={cold if cold is not None else ''}"])
        # -k 0: a chunk that fails does not stop the others. The usual cause is
        # memory (clang reports "out of memory" when several of the largest
        # chunks peak together), so what failed is retried with fewer jobs.
        # Near the commit limit clang can also crash outright instead (an
        # access violation in code generation, gone on a rerun), so a crash is
        # retried the same way, at most twice.
        # Peak commit per compile, measured on the largest and the slowest
        # chunks of the finished source (clang 22, x86-64): 0.4-0.5 GB, with or
        # without the profile; twice that leaves room for the others' growth.
        # (Before finish_tree's steps the slowest took 100-250 s and more
        # memory: CorrelatedValuePropagation chasing the CPU-state pointer
        # through one function per chunk was 77 percent of it.)
        gb_per_job = 1.0 if opt_level == "0" else 1.25
        jobs = default_jobs(gb_per_job) if self.args.jobs_auto else self.args.jobs
        print(f"  {jobs} parallel compiles")
        crashes = 0
        while True:
            try:
                self.run(f"{name}-build", ["cmake", "--build", build, "-j", jobs, "--", "-k", "0"], ninja=True)
                break
            except BuildError:
                log = (self.logs / f"{name}-build.log").read_text(errors="replace")
                crashed = "frontend command failed due to signal" in log
                if jobs <= 1 and not crashed or not ("out of memory" in log or crashed) or crashes >= 2:
                    raise
                crashes += crashed
                jobs = max(1, jobs // 2)
                reason = "clang crashed on some chunks" if crashed else "some chunks ran out of memory"
                print(f"  {reason}; compiling the rest with {jobs} jobs")
        module = build / MODULE
        if not module.exists():
            die("the game module was not produced")
        return module

    # --- local optimization training ---------------------------------------
    # The counterpart of scripts/builder/train_local_pgo.py: the game module is
    # compiled with LLVM's instrumentation, the normal app plays the opening to
    # player control headless with it, and the counts it records guide the
    # optimized compile. The profile is made from the game, so it is private and
    # stays in the build directory (docs/BUILDER.md). Unlike the Mac, the
    # bundled Apple-silicon profiles are not used: this one covers the runtime
    # in the module too, from this compiler.
    TRAINING_VERSION = "4"
    TRAINING_RETRACES = 23000
    # After player control (retrace 20,257 on the lookout), Link runs - off the
    # lookout, around Outset, turning - instead of standing until the end: the
    # collision, movement and animation code that play spends its time in is
    # then trained hot, not compiled cold for size. retrace:buttons:length:x:y.
    TRAINING_RUN = ["20400:0:700:0:127", "21100:0:500:90:110", "21600:0:500:-90:110", "22100:0:900:0:127"]
    # Then the plain playback tours the game (BLUEWAKE_TEST_WARP, a scene change
    # as a door makes it): towns, islands, dungeons, the sea from the boat. The
    # opening and Outset alone left most of the game's code untrained (288 of
    # 813 translated functions ran), so a dungeon's or the sea's code was
    # compiled cold, for size. Each stop: the scene loads, then Link runs,
    # turns and runs on. stage:room:point.
    TRAINING_TOUR = ["sea:11:1",        # Windfall Island
                     "sea:13:0",        # Dragon Roost Island
                     "M_NewD2:0:0",     # Dragon Roost Cavern
                     "sea:41:0",        # Forest Haven
                     "kindan:0:0",      # the Forbidden Woods
                     "Siren:0:0",       # the Tower of the Gods
                     "majroom:0:0",     # the Forsaken Fortress
                     "sea:1:100",       # the sea by the Fortress, on the boat
                     "Hyrule:0:0",      # Hyrule Castle
                     "sea:44:0"]        # back to Outset
    TOUR_START = 23200
    TOUR_STOP = 1500  # retraces at each stop

    def training_tour(self):
        """The tour's warps, its runs at each stop, and the retraces it ends at."""
        warps, moves = [], []
        for i, place in enumerate(self.TRAINING_TOUR):
            at = self.TOUR_START + i * self.TOUR_STOP
            warps.append(f"{at}:{place}")
            moves += [f"{at + 450}:0:300:0:127", f"{at + 780}:0:300:110:60", f"{at + 1110}:0:300:-110:60"]
        return warps, moves, self.TOUR_START + len(self.TRAINING_TOUR) * self.TOUR_STOP + 300

    def training_fingerprint(self):
        """What the recorded counts depend on: the game source (mods included),
        the runtime compiled into the module (RecompCore, cmake/composite), the
        compiler, the CPU level and this recipe. Bump TRAINING_VERSION when the
        route, or the host's pacing and input, change what the game runs. The
        host and Windows shell code around the module are left out on purpose:
        a profile slightly out of date costs little (clang matches what it
        can), and training takes the better part of an hour. --retrain forces it."""
        key = hashlib.sha256()
        key.update(f"{self.TRAINING_VERSION}\n{self.clang_version}\n{self.args.march}\n{self.mods}\n".encode())
        # The tree before the 60 Hz timing sites: they change a few chunks'
        # code only when that mode is on, and the training plays at 30 Hz.
        key.update((self.out / "composite-mods.digest").read_bytes())
        # The parts of RecompCore compiled into the module (cmake/composite):
        # GXRuntime's CPU core and its headers (the module includes nothing
        # else of GXRuntime's) and the recompiler's ABI. Aurora and the rest of
        # the runtime live in the app.
        for part in ("GXRuntime/src/core", "GXRuntime/include/core", "Source/Core/Core/PowerPC/StaticRecomp"):
            key.update(f"{part} {self.git('-C', str(self.recompcore), 'rev-parse', f'HEAD:{part}')}\n".encode())
        # The module's own runtime that runs with the game (its dispatch loop and
        # entry points); its build script and cold helpers (the guest CPU's
        # storage, the 60 Hz timing adapters) do not change what the profile counts.
        for name in ("dispatch_loop.c", "dispatch_loop.h", "module_export.c", "inline_fp.h", "gather_pipe.h",
                     "gather_pipe_batch.h", "direct_calls.h", "direct_calls.c", "native_skin.h", "native_skin.c",
                     "native_vec.h", "native_vec.c", "native_j3d.h", "native_j3d.c"):
            path = ROOT / "cmake/composite" / name
            key.update(path.relative_to(ROOT).as_posix().encode())
            key.update(path.read_bytes())
        # The source steps that rewrite every chunk after the mods (finish_tree):
        # clang matches counts to a function by the shape of its code, and a
        # chunk is one function, so a step that changes that shape leaves the
        # old counts matching nothing.
        for name in ("global_guest_cpu.py", "inline_save_restore_gpr.py", "chunk_headers.py", "direct_calls.py",
                     "native_skin.py", "fast_blocks.py", "lean_memory.py"):
            path = ROOT / "scripts/windows" / name
            key.update(path.relative_to(ROOT).as_posix().encode())
            key.update(path.read_bytes())
        path = ROOT / "scripts/mods/prepare_native_j3d.py"
        key.update(path.relative_to(ROOT).as_posix().encode())
        key.update(path.read_bytes())
        # Certified game math: isolated additions to the local PGO cache key.
        for path in (ROOT / "cmake/composite/native_game_math.c", ROOT / "cmake/composite/native_game_math.h",
                     ROOT / "scripts/windows/native_game_math.py"):
            key.update(path.relative_to(ROOT).as_posix().encode())
            key.update(path.read_bytes())
        # Certified native entries, the second set: their sources and hooks.
        for path in (ROOT / "cmake/composite/native_fifo.c", ROOT / "cmake/composite/native_fifo.h",
                     ROOT / "cmake/composite/native_bg.c", ROOT / "cmake/composite/native_bg.h",
                     ROOT / "cmake/composite/native_mtxcalc.c", ROOT / "cmake/composite/native_mtxcalc.h",
                     ROOT / "scripts/windows/native_entries.py"):
            key.update(path.relative_to(ROOT).as_posix().encode())
            key.update(path.read_bytes())
        # Certified native entries, the third set (the actor search by name):
        # its sources (its hooks are native_entries.py's, above).
        for path in (ROOT / "cmake/composite/native_search.c", ROOT / "cmake/composite/native_search.h"):
            key.update(path.relative_to(ROOT).as_posix().encode())
            key.update(path.read_bytes())
        # Certified native entries, the fourth set: its sources (its hooks are
        # native_entries.py's, above).
        for name in ("native_replay.h", "native_kankyo.c", "native_kankyo.h", "native_anim.c", "native_anim.h",
                     "native_cc.c", "native_cc.h"):
            path = ROOT / "cmake/composite" / name
            key.update(path.relative_to(ROOT).as_posix().encode())
            key.update(path.read_bytes())
        # Certified native entries, the fifth set: its sources (its hooks are
        # native_entries.py's, above; native_gx_gen.py writes native_gx_gen.inc and native_gx_list.h).
        for name in ("native_gx_run.h", "native_gx.c", "native_gx.h", "native_gx_list.h", "native_gx_gen.inc"):
            path = ROOT / "cmake/composite" / name
            key.update(path.relative_to(ROOT).as_posix().encode())
            key.update(path.read_bytes())
        return key.hexdigest()

    def train(self):
        work = self.out / "pgo-local"
        work.mkdir(parents=True, exist_ok=True)
        profile = work / "composite.profdata"
        receipt = work / "training.json"
        key = self.training_fingerprint()
        if profile.exists() and receipt.exists() and not self.args.retrain:
            try:
                previous = json.loads(receipt.read_text())
            except ValueError:
                previous = {}
            if previous.get("fingerprint") == key and previous.get("profile") == sha256_file(profile):
                print("reusing the optimization profile trained for these inputs")
                return self.hashed_profile(profile)

        exe = self.build_app()
        start = time.monotonic()
        module = self.compile_composite(work / "composite", "0", ["-fprofile-instr-generate"],
                                        ["-fprofile-instr-generate"], "training-composite")
        print(f"instrumented game module built ({int(time.monotonic() - start) // 60} min)")
        for old in work.glob("run-*"):
            shutil.rmtree(old, ignore_errors=True)
        raw = []
        # The opening as a new player plays it, then again with widescreen and
        # Better Wind Waker's options, so the chunks those mods replace are
        # optimized for play too rather than as code that never ran.
        runs = [("plain", None)]
        if self.mods:
            runs.append(("mods", "widescreen,betterww"))
        for name, mods in runs:
            raw += self.training_run(exe, module, work / f"run-{name}", mods, tour=name == "plain")
        self.run("training-merge", [self.llvm_profdata, "merge", "-o", profile, *raw])
        stats = subprocess.run([self.llvm_profdata, "show", "--all-functions", profile], capture_output=True,
                               text=True).stdout
        executed = [int(c) for c in re.findall(r"(?m)^  func_[0-9A-Fa-f]+\S*:\n(?:    .*\n)*?    Function count: (\d+)",
                                               stats)]
        ran = sum(1 for count in executed if count > 0)
        if ran == 0:
            die("the optimization profile counted no translated game functions")
        print(f"optimization profile: {ran} translated functions ran ({len(executed)} in the module)")
        receipt.write_text(json.dumps({"fingerprint": key, "profile": sha256_file(profile),
                                       "trained": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}, indent=2))
        return self.hashed_profile(profile)

    def training_run(self, exe, module, run, mods, tour=False, headless=True):
        """One headless playback of the opening: boot, A at the title, the
        opening cutscene's text confirmed, player control on Outset, and with
        `tour` the tour of the game after it. A new card in its own folder; the
        player's saves are never touched. headless=False draws it in a window
        (the app's own training, scripts/windows/train_app_profile.py)."""
        warps, tour_moves, tour_end = self.training_tour() if tour else ([], [], self.TRAINING_RETRACES)
        run.mkdir(parents=True)
        env = {k: v for k, v in os.environ.items() if not k.startswith(("BLUEWAKE_", "DOL_", "LLVM_PROFILE_"))}
        env.update({
            "LLVM_PROFILE_FILE": str(run / "%m-%p.profraw"),
            "BLUEWAKE_DATA_DIR": str(run), "BLUEWAKE_NO_DIALOG": "1",
            "BLUEWAKE_DOL": str(self.out / "game/main.dol"), "BLUEWAKE_RELS_DIR": str(self.out / "game/rels"),
            "BLUEWAKE_DISC": str(self.iso),
            "BLUEWAKE_DSP_IROM": str(self.recompcore / "Data/Sys/GC/dsp_rom.bin"),
            "BLUEWAKE_DSP_COEF": str(self.recompcore / "Data/Sys/GC/dsp_coef.bin"),
            "BLUEWAKE_MAX_RETRACES": str(tour_end), "BLUEWAKE_WALL_PACE": "0",
            "BLUEWAKE_PLAYER_PROBE": "1", "BLUEWAKE_PAD_BUTTONS": "0x0100",
            # The player-control milestone below waits on the overlap phase
            # this observation latches; the Windows app turns it off for play.
            "BLUEWAKE_OVERLAP_OBSERVATION": "1",
            "BLUEWAKE_PAD_PULSE_ON_TITLE_READY": "1", "BLUEWAKE_PAD_PULSE_LENGTH": "2",
            "BLUEWAKE_PAD_CONFIRM_EVENT": "any",
            "BLUEWAKE_PAD_SCRIPT": ",".join([f"{n}:0x0100:2" for n in range(17800, 22001, 150)] + self.TRAINING_RUN +
                                            tour_moves),
        })
        if warps:
            env["BLUEWAKE_TEST_WARP"] = ",".join(warps)
        if headless:
            env["BLUEWAKE_RENDERER"] = "headless"
        if mods:
            env["BLUEWAKE_MODS"] = mods
        # A playback that crashes leaves no counts to merge, and another run of
        # the same opening records the same ones, so a crash is retried once.
        for attempt in (1, 2):
            for stale in run.glob("*.profraw"):
                stale.unlink()
            try:
                log = self.run(f"training-playback-{run.name[4:]}", [exe, "--module", module], env=env)
                break
            except BuildError:
                if attempt == 2:
                    raise
                print("  the playback ended abnormally; running it again")
        text = log.read_text(errors="replace")
        if "[player-milestone] control-admitted" not in text:
            die(f"the training playback did not reach player control; profile rejected (see {log})")
        raw = sorted(run.glob("*.profraw"))
        if not raw:
            die(f"the training playback wrote no profile (see {log})")
        return raw

    def hashed_profile(self, profile):
        folder = self.out / "profiles"
        folder.mkdir(exist_ok=True)
        target = folder / f"composite-{sha256_file(profile)[:16]}.profdata"
        if not target.exists():
            shutil.copy2(profile, target)
        return target

    # --- 9 app ---------------------------------------------------------------
    def build_app(self):
        self.run("app-build", ["cmake", "--build", self.app_build, "--target", "BlueWake", "-j", self.args.jobs],
                 ninja=True)
        exe = self.app_build / "BlueWake.exe"
        if not exe.exists():
            die("BlueWake.exe was not produced")
        return exe

    # --- 10 package ----------------------------------------------------------
    def package(self, module):
        app = self.out / "BlueWake"
        (app / "game").mkdir(parents=True, exist_ok=True)
        (app / "dsp").mkdir(exist_ok=True)
        copied = []
        for f in sorted(self.app_build.iterdir()):
            if f.suffix.lower() in (".exe", ".dll") and f.name != "bluewake_disc_extract.exe" or \
                    f.name == "initial_pipeline_cache.db":
                shutil.copy2(f, app / f.name)
                copied.append(f.name)
        shutil.copy2(module, app / MODULE)
        # The Visual C++ runtime, app-local, so the folder also runs on a PC
        # without the redistributable installed (the UCRT ships with Windows).
        redist = Path(self.env.get("VCToolsRedistDir", "")) / "x64"
        crt = sorted(redist.glob("Microsoft.VC*.CRT")) if redist.is_dir() else []
        if crt:
            # msvcp140_atomic_wait: Dawn's std::atomic waits (webgpu_dawn.dll).
            for name in ("msvcp140.dll", "msvcp140_atomic_wait.dll", "vcruntime140.dll", "vcruntime140_1.dll"):
                if (crt[-1] / name).exists():
                    shutil.copy2(crt[-1] / name, app / name)
        else:
            print("note: the Visual C++ runtime DLLs were not found to copy; the app needs the "
                  "Visual C++ Redistributable on other PCs")
        shutil.copy2(self.out / "game/main.dol", app / "game/main.dol")
        rels = app / "game/rels"
        shutil.rmtree(rels, ignore_errors=True)
        shutil.copytree(self.out / "game/rels", rels)
        for name in ("dsp_rom.bin", "dsp_coef.bin"):
            shutil.copy2(self.recompcore / "Data/Sys/GC" / name, app / "dsp" / name)
        self.place(self.iso, app / "game/GZLE01.iso")
        dirty = bool(self.git("status", "--porcelain"))
        provenance = {
            "profile": "bluewake-windows",
            "containsTranslatedGameCode": True,
            "source_commit": self.git("rev-parse", "HEAD"),
            "source_modified": dirty,
            "composite_digest": (self.out / "composite-src.digest").read_text().strip(),
            "mods": bool(self.mods),
            "march": self.args.march,
            "local_training": self.profile is not None,
            "composite_profile_sha256": sha256_file(self.profile) if self.profile else "",
            "compiler": self.clang_version,
            "module_sha256": sha256_file(app / MODULE),
            "built": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        }
        (app / "BuilderProvenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
        (app / "README.txt").write_text(README)
        return app

    @staticmethod
    def place(source, target):
        """The disc goes beside the game: a hard link when it can, else a copy."""
        if target.exists():
            if os.path.samefile(source, target) or \
                    (target.stat().st_size == source.stat().st_size and
                     target.stat().st_mtime_ns >= source.stat().st_mtime_ns):
                return
            target.unlink()
        try:
            os.link(source, target)
        except OSError:
            print(f"copying {source.name} into the app folder ({source.stat().st_size >> 20} MB)")
            shutil.copy2(source, target)

    # --- the pipeline ------------------------------------------------------------
    def build(self):
        args = self.args
        self.mods = not args.no_mods
        self.mods_pending = self.mods
        self.train_pgo = not (args.no_train or args.no_pgo)
        self.profile = None
        print(f"Building The Legend of Zelda: The Wind Waker (GameCube USA GZLE01 rev 0) for Windows from {args.disc}")
        step("1/10 tools")
        self.check_tools()
        step("2/10 dependencies")
        self.dependencies()
        step("3/10 disc")
        self.disc()
        if args.check_only:
            print("\ntools, dependencies and disc are ready.")
            return
        step("4/10 extract the game from the disc")
        self.dolrecomp = self.build_dolrecomp()
        self.configure_app()
        game = self.out / "game"
        stamp = self.out / "game.json"
        key = {"iso": str(self.iso), "size": self.iso.stat().st_size, "mtime": self.iso.stat().st_mtime_ns,
               "extractor": sha256_file(ROOT / "apple/ios/src/disc_import.c")}
        if (game / "main.dol").exists() and stamp.exists() and json.loads(stamp.read_text()) == key:
            print(f"reusing main.dol and the RELs in {game}")
        else:
            self.extract(self.iso, game)
            stamp.write_text(json.dumps(key))
            print(f"main.dol and 415 RELs in {game}")
        step("5/10 translate")
        self.translate(game / "main.dol", self.out / "translated", game / "rels")
        chunks = len(list((self.out / "translated/dol/generated/chunks").glob("*.c")))
        print(f"translated: {chunks} DOL chunks and 415 RELs")
        step("6/10 generate the composite source")
        self.generate()
        if args.source_only:
            print(f"\nsource check passed: {self.out / 'composite-src'}. Rerun without --source-only to "
                  f"compile and build the app.")
            return
        step("7/10 mods")
        if not self.mods:
            print("skipped")
        elif not self.mods_pending:
            print("mods already in the composite source")
        else:
            self.build_mods()
        step("the last source steps: the guest CPU's fixed address, 60 Hz gameplay's timing sites")
        self.finish_in_place()
        if self.train_pgo:
            step("local optimization training (the first time: an instrumented game module and two "
                 "headless playbacks of the opening)")
            self.profile = self.train()
        else:
            step("local optimization training")
            print("skipped: the game module is compiled without an optimization profile")
        step(f"8/10 compile the game module (-O{args.opt_level}, -march={args.march}; this is the long step)")
        start = time.monotonic()
        module = self.compile_module()
        print(f"game module: {module} ({int(time.monotonic() - start) // 60} min)")
        step("9/10 build the app")
        exe = self.build_app()
        print(f"app: {exe}")
        step("10/10 package")
        app = self.package(module)
        print(f"\nBlueWake: {app}")
        print(f"  run {app / 'BlueWake.exe'}")
        print("  It contains game code translated from your disc and a copy of the disc: keep it for yourself.")
        print("  Saves: %APPDATA%\\BlueWake (outside the build).")


README = """BlueWake for Windows: The Legend of Zelda: The Wind Waker (GameCube USA),
statically recompiled from your own disc. See docs/WINDOWS.md in the source.

This folder is a personal build: gGZLE01_recomp.dll is code translated from
your disc and game\\ holds your disc image. Never share or upload it.

Run BlueWake.exe. Options (BlueWake.exe --help lists them all):
  --widescreen    16:9 (--aspect 16:10 for 16:10)
  --smooth        Smooth Motion: 60 FPS with in-between frames
  --betterww      Better Wind Waker's settings (--options to change them)
  --fullscreen    start in fullscreen

Keyboard: arrows D-pad, J A, K B, U X, I Y, W/A/S/D control stick,
H/F/T/G C-stick, E/R L/R, Q Z, Return START. Game controllers work too.
Mouse: click the game and move the mouse to turn the camera; Esc releases it.
F11 fullscreen, F10 Smooth Motion, F9 frame rate.

Saves, settings and session logs: %APPDATA%\\BlueWake
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("disc", type=Path, help="your GZLE01 revision 0 disc image (.iso, .gcm, .rvz, ...)")
    parser.add_argument("--out", type=Path, default=ROOT / "build/windows",
                        help="build directory (default build/windows; must be git-ignored inside the checkout)")
    parser.add_argument("--jobs", type=int, default=None,
                        help="parallel compile jobs (default: the cores, limited by free memory)")
    parser.add_argument("--march", default="x86-64-v3",
                        help="CPU level for the game module (default x86-64-v3: AVX2, FMA, BMI2 and MOVBE, "
                             "any Intel Haswell or AMD Zen or newer; lowered automatically on older CPUs)")
    parser.add_argument("--opt-level", choices=("1", "2"), default="2", help="game module optimization level")
    parser.add_argument("--no-tiered", action="store_true",
                        help="compile every chunk at -O2, not only those the optimization training ran "
                             "(a build about 25 minutes longer)")
    parser.add_argument("--no-mods", action="store_true", help="skip the widescreen and Better Wind Waker variants")
    parser.add_argument("--no-train", action="store_true",
                        help="skip local optimization training: faster to build, slower in game")
    parser.add_argument("--no-pgo", action="store_true", help="no optimization profile at all (implies --no-train)")
    parser.add_argument("--retrain", action="store_true",
                        help="train again even though the game source, runtime and compiler are unchanged")
    parser.add_argument("--console", action="store_true", help="build BlueWake.exe as a console program")
    parser.add_argument("--no-app-pgo", action="store_true",
                        help="build the app without its committed optimization profile and ThinLTO")
    parser.add_argument("--accept-new-composite", action="store_true",
                        help="continue if the generated source differs from the verified one")
    parser.add_argument("--source-only", action="store_true",
                        help="stop after generating the source: checks tools, disc and translation in minutes")
    parser.add_argument("--check-only", action="store_true", help="check tools, dependencies and the disc only")
    args = parser.parse_args()
    # Chosen again when each compile starts (compile_composite): memory freed
    # during a long build is used, and each kind of compile has its own size.
    args.jobs_auto = args.jobs is None
    if args.jobs is None:
        args.jobs = default_jobs()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    args.out = args.out.resolve()
    try:
        rel = args.out.relative_to(ROOT)
    except ValueError:
        rel = None
    if rel is not None:
        if str(rel) == ".":
            parser.error("--out must not be the checkout itself; use build/windows")
        ignored = subprocess.run(["git", "check-ignore", "-q", str(args.out) + os.sep], cwd=ROOT).returncode == 0
        if not ignored:
            parser.error("--out inside this checkout must be git-ignored; use build/windows")
    args.out.mkdir(parents=True, exist_ok=True)
    try:
        Builder(args).build()
    except BuildError as error:
        print(f"\nbuilder: {error}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("\nbuilder: interrupted; rerun the same command to continue", file=sys.stderr)
        sys.exit(130)


if __name__ == "__main__":
    main()
