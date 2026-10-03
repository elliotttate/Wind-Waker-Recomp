#!/usr/bin/env python3
"""BlueWake Builder for Android (arm64): turn your own game disc into your own APK, on your PC.

    python scripts/android/build.py DISC [--out build/android] [options]

The Android counterpart of scripts/windows/build.py, and built on it: the same
pinned RecompCore and DolRecomp, the same disc checks, translation, verified
composite digest, mods and source steps run on the Windows PC exactly as the
Windows builder runs them. Only the target differs: the game module and the
app are compiled for arm64 Android with the NDK's clang, the renderer is
Aurora through Dawn on Vulkan, and the result is an APK.

Steps, each logged under OUT/logs:
  1 tools        a Windows clang (for the translator), the Android NDK and SDK, a JDK
  2 dependencies the pinned RecompCore and DolRecomp sources (ref/recompcore)
  3 disc         check the disc (an .iso; convert an .rvz with Dolphin first)
  4 extract      main.dol and the 415 RELs from the disc (the disc is verified)
  5 translate    the game's PowerPC code to C (DolRecomp)
  6 generate     the composite source, compared with the verified digest
  7 mods         widescreen 16:9 and 16:10 and Better Wind Waker's options (--no-mods skips)
  8 compile      the game module, libgGZLE01_recomp.so, for arm64 Android (the long step)
  9 app          libmain.so: the host, GXRuntime, Aurora (Dawn/Vulkan), SDL3 and the DSP
 10 package      the APK, and the game files to push to the device (scripts/android/install.py)

The APK contains code translated from YOUR disc: it is yours alone. Never share
or upload it.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
from xml.sax.saxutils import escape as xml_escape
import zipfile

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("bluewake_windows_build", ROOT / "scripts/windows/build.py")
wb = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(wb)

die, step, sha256_file = wb.die, wb.step, wb.sha256_file
MODULE = "libgGZLE01_recomp.so"
# The Java activity (android/java); the application id and the label are options.
ACTIVITY = "dev.bluewake.android.BlueWakeActivity"
DEFAULT_LABEL = "Wind Waker Recomp"
MIN_SDK = 33  # execinfo backtrace() (the GX stall watchdog)
TARGET_SDK = 35


def first_existing(*candidates):
    for c in candidates:
        if c and Path(c).exists():
            return Path(c)
    return None


class AndroidBuilder(wb.Builder):
    def __init__(self, args):
        super().__init__(args)
        self.app_build = self.out / "host-tools"
        self.app_id = args.package or wb.profile_value("PROFILE_BUNDLE_ID")

    # --- 1 tools -----------------------------------------------------------
    def check_tools(self):
        if sys.version_info < (3, 10):
            die("Python 3.10 or newer is required")
        # The Windows side (the translator, the disc extractor): clang-cl with
        # Visual Studio's headers and libraries. Any Visual Studio with the C++
        # workload; clang comes from --llvm (a portable LLVM for Windows) when
        # Visual Studio has none of its own.
        env = self.msvc_env()
        llvm = self.args.llvm
        if llvm is not None:
            if not (llvm / "bin/clang-cl.exe").exists():
                die(f"no clang-cl.exe in {llvm / 'bin'}")
            env["PATH"] = str(llvm / "bin") + os.pathsep + env["PATH"]
        if shutil.which("clang-cl", path=env["PATH"]) is None:
            die("no clang for Windows: pass --llvm DIR (an LLVM for Windows, e.g. clang+llvm-21-x86_64-pc-windows-msvc)")
        ninja = shutil.which("ninja", path=env["PATH"])
        if ninja is None:
            for sdk_cmake in sorted((self.sdk / "cmake").glob("*/bin")) if self.sdk else []:
                if (sdk_cmake / "ninja.exe").exists():
                    env["PATH"] = str(sdk_cmake) + os.pathsep + env["PATH"]
                    ninja = str(sdk_cmake / "ninja.exe")
        if ninja is None:
            die("ninja is missing (pip install ninja)")
        self.env = env
        self.clang = shutil.which("clang", path=env["PATH"])
        version = subprocess.check_output(["cmake", "--version"], text=True).split()[2]
        if tuple(int(x) for x in version.split(".")[:2]) < (3, 25):
            die(f"CMake 3.25 or newer is required (found {version})")

        # The Android side.
        if self.sdk is None:
            die("the Android SDK was not found: pass --sdk DIR")
        ndks = sorted((self.sdk / "ndk").glob("*"), key=lambda p: [int(x) for x in re.findall(r"\d+", p.name)])
        self.ndk = self.args.ndk or (ndks[-1] if ndks else None)
        if self.ndk is None or not (self.ndk / "build/cmake/android.toolchain.cmake").exists():
            die("the Android NDK was not found: pass --ndk DIR (r27 or newer)")
        self.ndk_bin = self.ndk / "toolchains/llvm/prebuilt/windows-x86_64/bin"
        self.clang_version = subprocess.check_output([self.ndk_bin / "clang.exe", "--version"],
                                                     text=True).splitlines()[0]
        self.llvm_profdata = self.ndk_bin / "llvm-profdata.exe"
        tools = sorted((self.sdk / "build-tools").glob("*"),
                       key=lambda p: [int(x) for x in re.findall(r"\d+", p.name)])
        if not tools:
            die(f"no build-tools in {self.sdk}")
        self.build_tools = tools[-1]
        platforms = sorted((self.sdk / "platforms").glob("android-*"),
                           key=lambda p: int(re.sub(r"\D", "", p.name) or 0))
        if not platforms:
            die(f"no platforms in {self.sdk}")
        self.android_jar = platforms[-1] / "android.jar"
        java_home = self.args.jdk or first_existing(os.environ.get("JAVA_HOME"))
        if java_home is None or not (java_home / "bin/javac.exe").exists():
            die("a JDK 17 is required: pass --jdk DIR")
        self.jdk = java_home
        print(f"Windows clang: {subprocess.check_output([self.clang, '--version'], text=True, env=env).splitlines()[0]}")
        print(f"NDK {self.ndk.name}: {self.clang_version}")
        print(f"SDK build-tools {self.build_tools.name}, {platforms[-1].name}; JDK {self.jdk}")
        print(f"cmake {version}; {self.args.jobs} jobs; -mcpu={self.args.cpu}")

    @property
    def sdk(self):
        return self.args.sdk or first_existing(os.environ.get("ANDROID_HOME"), os.environ.get("ANDROID_SDK_ROOT"),
                                               Path(os.environ.get("LOCALAPPDATA", "")) / "Android/Sdk")

    def msvc_env(self):
        vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / \
            "Microsoft Visual Studio/Installer/vswhere.exe"
        if not vswhere.exists():
            die("Visual Studio 2022 or newer with the C++ workload is required (vswhere.exe not found)")
        found = json.loads(subprocess.check_output(
            [str(vswhere), "-all", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
             "-format", "json", "-utf8"], text=True, encoding="utf-8"))
        if not found:
            die("no Visual Studio with the C++ workload was found")
        install = Path(sorted(found, key=lambda v: v.get("installationVersion", ""))[-1]["installationPath"])
        vcvars = install / "VC/Auxiliary/Build/vcvars64.bat"
        path = [p.strip('"') for p in os.environ.get("PATH", "").split(os.pathsep)]
        base = dict(os.environ, PATH=os.pathsep.join(p for p in path if p and "(" not in p and ")" not in p))
        output = subprocess.run(f'cmd /d /c ""{vcvars}" >nul 2>&1 && set"', capture_output=True, text=True,
                                env=base, shell=False)
        if output.returncode != 0 or "INCLUDE=" not in output.stdout:
            die(f"{vcvars} failed")
        env = {}
        for line in output.stdout.splitlines():
            key, sep, value = line.partition("=")
            if sep and key:
                env[key] = value
        if (install / "VC/Tools/Llvm/x64/bin/clang.exe").exists():
            env["PATH"] = str(install / "VC/Tools/Llvm/x64/bin") + os.pathsep + env.get("PATH", "")
        print(f"Visual Studio: {install}")
        return env

    # --- 4 extract: the disc extractor, built for this PC ------------------
    def configure_app(self):
        """The Windows builder configures its whole app here for the extractor;
        only the extractor is needed on this side, built directly."""
        self.app_build.mkdir(parents=True, exist_ok=True)
        exe = self.app_build / "bluewake_disc_extract.exe"
        sources = [ROOT / "scripts/ios/disc_extract.c", ROOT / "apple/ios/src/disc_import.c",
                   ROOT / "windows/compat/bw_posix_compat.c"]
        if exe.exists() and exe.stat().st_mtime > max(s.stat().st_mtime for s in sources):
            return
        compat = ROOT / "windows/compat"
        self.run("disc-extract-build", [
            self.clang, "-O2", "-D_CRT_SECURE_NO_WARNINGS", "-D_CRT_NONSTDC_NO_DEPRECATE", "-D_USE_MATH_DEFINES",
            "-I", compat / "include", "-I", compat, "-I", ROOT / "apple/ios/src",
            "-include", compat / "bw_posix_compat.h",
            *sources, "-lbcrypt", "-o", exe], env=self.env)

    def extract(self, iso, game, accept_sha1=None):
        env = dict(self.env)
        if accept_sha1:
            env["BLUEWAKE_ACCEPT_DOL_SHA1"] = accept_sha1
        self.run("disc-extract", [self.app_build / "bluewake_disc_extract.exe", iso, game], env=env)
        rels = len(list((game / "rels").glob("*.rel")))
        if rels != 415:
            die(f"expected 415 RELs in {game / 'rels'}, found {rels}")

    # --- 8 compile: the game module for arm64 Android -----------------------
    def android_cmake_args(self):
        return ["-G", "Ninja", f"-DCMAKE_TOOLCHAIN_FILE={(self.ndk / 'build/cmake/android.toolchain.cmake').as_posix()}",
                "-DANDROID_ABI=arm64-v8a", f"-DANDROID_PLATFORM=android-{MIN_SDK}", "-DANDROID_STL=c++_static",
                "-DCMAKE_BUILD_TYPE=Release"]

    def compile_module(self):
        flags = []
        if self.profile is not None:
            flags = [f"-fprofile-instr-use={self.profile.as_posix()}", "-Wno-profile-instr-unprofiled",
                     "-Wno-profile-instr-out-of-date", "-Wno-backend-plugin"]
            print(f"with the optimization profile {self.profile.name}")
        return self.compile_composite(self.out / "composite", self.args.opt_level, flags, [], "composite")

    def compile_composite(self, build, opt_level, extra_flags, extra_link_flags, name, extra_cmake=()):
        rc = self.recompcore
        # The Windows builder's two compile-time limits apply to AArch64 too:
        # each chunk is one very large function (docs/WINDOWS.md).
        flags = " ".join([f"-mcpu={self.args.cpu}", "-fno-slp-vectorize",
                          "-mllvm", "-large-interval-freq-threshold=10", *extra_flags])
        # 16 KB page alignment: required of new apps on Android 15+ devices
        # with 16 KB pages, harmless on 4 KB ones.
        link_flags = " ".join(["-Wl,-z,max-page-size=16384", "-Wl,--gc-sections", *extra_link_flags])
        self.run(f"{name}-configure", [
            "cmake", "-S", ROOT / "cmake/composite", "-B", build, *self.android_cmake_args(),
            f"-DCMAKE_C_FLAGS={flags}", f"-DCMAKE_SHARED_LINKER_FLAGS={link_flags}",
            f"-DCOMPOSITE_OPTIMIZATION_LEVEL={opt_level}", f"-DCOMPOSITE_DIR={(self.out / 'composite-src').as_posix()}",
            f"-DGXRUNTIME_DIR={(rc / 'GXRuntime').as_posix()}",
            f"-DABI_DIR={(rc / 'Source/Core/Core/PowerPC/StaticRecomp').as_posix()}", *extra_cmake])
        gb_per_job = 1.0 if opt_level == "0" else 1.25
        jobs = wb.default_jobs(gb_per_job) if self.args.jobs_auto else self.args.jobs
        print(f"  {jobs} parallel compiles")
        crashes = 0
        while True:
            try:
                self.run(f"{name}-build", ["cmake", "--build", build, "-j", jobs, "--", "-k", "0"], ninja=True)
                break
            except wb.BuildError:
                log = (self.logs / f"{name}-build.log").read_text(errors="replace")
                crashed = "frontend command failed due to signal" in log
                if jobs <= 1 and not crashed or not ("out of memory" in log or crashed) or crashes >= 2:
                    raise
                crashes += crashed
                jobs = max(1, jobs // 2)
                print(f"  some chunks failed for memory; compiling the rest with {jobs} jobs")
        module = build / "gGZLE01_recomp.so"
        if not module.exists():
            die("the game module was not produced")
        return module

    # --- 9 app: libmain.so ---------------------------------------------------
    def build_app(self, build=None, profile=None, generate=False, name="app"):
        build = build or self.out / "app"
        self.run(f"{name}-configure", ["cmake", "-S", ROOT / "android", "-B", build, *self.android_cmake_args(),
                                       f"-DBLUEWAKE_ANDROID_CPU={self.args.cpu}",
                                       f"-DBLUEWAKE_APP_PROFILE_GENERATE={'ON' if generate else 'OFF'}",
                                       f"-DBLUEWAKE_APP_PROFILE_USE={profile.as_posix() if profile else ''}"])
        self.run(f"{name}-build", ["cmake", "--build", build, "--target", "main", "-j", self.args.jobs], ninja=True)
        lib = build / "libmain.so"
        if not lib.exists():
            die("libmain.so was not produced")
        return lib

    def app_profile(self):
        """The app's trained profile (OUT/pgo-app), if there is one."""
        profile = self.out / "pgo-app/app.profdata"
        if self.args.no_app_profile or not profile.exists():
            return None
        print(f"the app with its optimization profile ({profile.name})")
        return profile

    # --- 10 package: the APK --------------------------------------------------
    def sdl_java_dir(self, lib):
        # SDL's source as the app build being packaged fetched it: OUT/app, or
        # OUT/app-train for the app's training APK, which can come first.
        for build in (lib.parent, self.out / "app"):
            found = sorted((build / "_deps").glob("sdl-src/android-project/app/src/main/java"))
            if found:
                return found[0]
        die("SDL's Java sources were not found in the app build (_deps/sdl-src)")

    def package(self, module, lib, apk_name="WindWakerRecomp.apk", pkg_dir="apk", provenance=True):
        pkg = self.out / pkg_dir
        shutil.rmtree(pkg, ignore_errors=True)
        (pkg / "classes").mkdir(parents=True)
        # SDL's Java, with one change: the thread that runs SDL_main (and so the
        # game, whose translated code recurses on the host stack as the guest did
        # on the GameCube's) gets a 64 MB stack, as windows/CMakeLists.txt links
        # it, instead of Java's default of about 1 MB.
        sdl_java = pkg / "sdl-java"
        shutil.copytree(self.sdl_java_dir(lib), sdl_java)
        activity = sdl_java / "org/libsdl/app/SDLActivity.java"
        text = activity.read_text(encoding="utf-8")
        original = 'new Thread(new SDLMain(), "SDLThread")'
        if original not in text:
            die(f"SDLActivity.java no longer creates its thread as {original}")
        activity.write_text(text.replace(original, 'new Thread(null, new SDLMain(), "SDLThread", 64L << 20)'),
                            encoding="utf-8")
        # Aurora on Android draws only while SDL's surface is ready, which SDL's
        # Java tells it through a native Aurora provides
        # (Java_org_libsdl_app_SDLSurface_auroraNativeSetSurfaceReady in
        # lib/window.cpp): ready once SDL has the surface, not ready before SDL
        # lets it go. Stock SDL never calls it, and the game waited forever for
        # its first frame.
        surface = sdl_java / "org/libsdl/app/SDLSurface.java"
        text = surface.read_text(encoding="utf-8")
        edits = [
            ("        SDLActivity.onNativeSurfaceChanged();\n",
             "        SDLActivity.onNativeSurfaceChanged();\n        auroraNativeSetSurfaceReady(true);\n"),
            ("        mIsSurfaceReady = false;\n        SDLActivity.onNativeSurfaceDestroyed();\n",
             "        mIsSurfaceReady = false;\n        auroraNativeSetSurfaceReady(false);\n"
             "        SDLActivity.onNativeSurfaceDestroyed();\n"),
            ("    // Called when we have a valid drawing surface\n",
             "    static native void auroraNativeSetSurfaceReady(boolean ready);\n\n"
             "    // Called when we have a valid drawing surface\n"),
        ]
        for old, new in edits:
            if text.count(old) != 1:
                die(f"SDLSurface.java no longer matches the surface-ready patch ({old.strip()})")
            text = text.replace(old, new)
        surface.write_text(text, encoding="utf-8")
        java = [str(p) for p in sdl_java.rglob("*.java")]
        java += [str(p) for p in (ROOT / "android/java").rglob("*.java")]
        jdk_bin = self.jdk / "bin"
        self.run("apk-javac", [jdk_bin / "javac.exe", "-nowarn", "-encoding", "UTF-8", "--release", "11",
                               "-classpath", self.android_jar, "-d", pkg / "classes", *java], env=os.environ)
        d8 = self.build_tools / "d8.bat"
        classes = [str(p) for p in (pkg / "classes").rglob("*.class")]
        self.run("apk-d8", [d8, "--release", "--min-api", str(MIN_SDK), "--lib", self.android_jar,
                            "--output", pkg, *classes], env=dict(os.environ, JAVA_HOME=str(self.jdk)))
        manifest = (ROOT / "android/AndroidManifest.xml").read_text(encoding="utf-8")
        manifest = manifest.replace("@PACKAGE@", self.app_id).replace(
            "@LABEL@", xml_escape(self.args.label, {'"': "&quot;", "'": "&apos;"}))
        manifest = manifest.replace("@DEBUGGABLE@", "true" if getattr(self.args, "debuggable", False) else "false")
        (pkg / "AndroidManifest.xml").write_text(manifest, encoding="utf-8")
        res_zip = pkg / "res.zip"
        self.run("apk-aapt2-compile", [self.build_tools / "aapt2.exe", "compile", "--dir", ROOT / "android/res",
                                       "-o", res_zip])
        # Aurora's bundled pipeline cache (apple/ios/resources): Aurora opens
        # it through SDL at its base path, which on Android is the APK's assets.
        assets = pkg / "assets"
        assets.mkdir()
        shutil.copy2(ROOT / "apple/ios/resources/initial_pipeline_cache.db", assets / "initial_pipeline_cache.db")
        unsigned = pkg / "unsigned.apk"
        self.run("apk-aapt2-link", [self.build_tools / "aapt2.exe", "link", "-o", unsigned, "-A", assets, "-0", "db",
                                    "--manifest", pkg / "AndroidManifest.xml", "-I", self.android_jar,
                                    "--min-sdk-version", str(MIN_SDK), "--target-sdk-version", str(TARGET_SDK),
                                    "--version-code", str(int(time.time()) // 60), "--version-name", "0.1-android",
                                    res_zip])
        # The native libraries go in stored (uncompressed, page-aligned by
        # zipalign -P 16), so Android maps them from the APK in place.
        # The NDK's toolchain compiles with -g: the debug information (most of
        # the game module's 800 MB) stays in the build, out of the APK.
        (pkg / "lib").mkdir()
        strip = self.ndk_bin / "llvm-strip.exe"
        stripped_lib = pkg / "lib/libmain.so"
        self.run("apk-strip-main", [strip, "--strip-debug", "-o", stripped_lib, lib])
        stripped_module = None
        if module is not None:  # None: a shell-only test package
            stripped_module = pkg / f"lib/{MODULE}"
            self.run("apk-strip-module", [strip, "--strip-debug", "-o", stripped_module, module])
        with zipfile.ZipFile(unsigned, "a") as apk:
            apk.write(pkg / "classes.dex", "classes.dex", compress_type=zipfile.ZIP_DEFLATED)
            apk.write(stripped_lib, "lib/arm64-v8a/libmain.so", compress_type=zipfile.ZIP_STORED)
            if stripped_module is not None:
                apk.write(stripped_module, f"lib/arm64-v8a/{MODULE}", compress_type=zipfile.ZIP_STORED)
        aligned = pkg / "aligned.apk"
        self.run("apk-zipalign", [self.build_tools / "zipalign.exe", "-f", "-P", "16", "4", unsigned, aligned])
        keystore = self.out / "debug.keystore"
        if not keystore.exists():
            self.run("apk-keystore", [jdk_bin / "keytool.exe", "-genkeypair", "-keystore", keystore,
                                      "-storepass", "android", "-keypass", "android", "-alias", "androiddebugkey",
                                      "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000",
                                      "-dname", "CN=Android Debug,O=Android,C=US"], env=os.environ)
        apk_out = self.out / apk_name
        self.run("apk-sign", [self.build_tools / "apksigner.bat", "sign", "--ks", keystore,
                              "--ks-pass", "pass:android", "--out", apk_out, aligned],
                 env=dict(os.environ, JAVA_HOME=str(self.jdk), PATH=str(jdk_bin) + os.pathsep + os.environ["PATH"]))
        if not provenance:
            return apk_out
        # What install.py pushes to the device beside the APK: the prepared game
        # files and the disc (never packaged, never shared).
        provenance = {
            "profile": "bluewake-android",
            "package": self.app_id,
            "containsTranslatedGameCode": True,
            "source_commit": self.git("rev-parse", "HEAD"),
            "source_modified": bool(self.git("status", "--porcelain")),
            "composite_digest": (self.out / "composite-src.digest").read_text().strip(),
            "mods": bool(self.mods),
            "cpu": self.args.cpu,
            "local_training": self.profile is not None,
            "compiler": self.clang_version,
            "module_sha256": sha256_file(module) if module is not None else "",
            "iso": str(self.iso),
            "built": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        }
        (self.out / "BuilderProvenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
        return apk_out

    # --- local optimization training, on the device -----------------------------
    # The Windows builder's training (scripts/windows/build.py train), played on
    # the phone: an instrumented game module (-O0, -fprofile-instr-generate) in a
    # training APK plays the opening headless and unpaced to player control on
    # Outset, plain and then with widescreen and Better Wind Waker's options;
    # the counts it writes guide the real module's compile. The profile is made
    # from the game, so it stays in OUT/pgo-device and is never shared. The
    # training installs over the app (same package, same key): the player's saves
    # are untouched (the runs use their own cards), and the real APK goes back on
    # with install.py afterwards.
    @property
    def DEVICE_FILES(self):
        return f"/storage/emulated/0/Android/data/{self.app_id}/files"

    def adb(self, *command, check=True, capture=False, timeout=None):
        full = [str(self.adb_exe), "-s", self.args.device, *[str(c) for c in command]]
        result = subprocess.run(full, capture_output=True, text=True, timeout=timeout)
        if check and result.returncode != 0:
            die(f"adb {' '.join(str(c) for c in command)[:120]} failed: {result.stdout}{result.stderr}")
        return result.stdout if capture else result

    def train_on_device(self, lib):
        work = self.out / "pgo-device"
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
        self.adb_exe = self.sdk / "platform-tools/adb.exe"
        devices = subprocess.run([str(self.adb_exe), "devices"], capture_output=True, text=True).stdout
        if self.args.device not in devices:
            die(f"device {self.args.device} is not connected (adb devices)")
        start = time.monotonic()
        module = self.compile_composite(
            work / "composite", "0", ["-fprofile-instr-generate"], ["-fprofile-instr-generate"],
            "training-composite",
            [f"-DCOMPOSITE_EXTRA_SOURCES={(ROOT / 'android/src/profile_flush.c').as_posix()}"])
        exported = subprocess.run([str(self.ndk_bin / "llvm-nm.exe"), "-D", "--defined-only", str(module)],
                                  capture_output=True, text=True).stdout
        if " bluewake_profile_write" not in exported:
            die("the instrumented module does not export bluewake_profile_write")
        print(f"instrumented game module built ({int(time.monotonic() - start) // 60} min)")
        apk = self.package(module, lib, "WindWakerRecomp-training.apk", "apk-training", provenance=False)
        print(f"installing the training APK on {self.args.device}")
        self.adb("shell", "am", "force-stop", self.app_id)
        self.adb("install", "-r", "-g", apk, timeout=900)
        for old in work.glob("run-*"):
            shutil.rmtree(old, ignore_errors=True)
        raw = []
        runs = [("plain", None)]
        if self.mods:
            runs.append(("mods", "widescreen,betterww"))
        try:
            # A short run first: it must write a profile, or the long runs would
            # be wasted. It reaches no player control and is not merged.
            self.device_training_run(work / "run-check", "check", None, retraces=600)
            for name, mods in runs:
                raw += self.device_training_run(work / f"run-{name}", name, mods)
        finally:
            self.adb("shell", "rm", "-f", f"{self.DEVICE_FILES}/launch.env", check=False)
            self.adb("shell", "am", "force-stop", self.app_id, check=False)
        self.run("training-merge", [self.llvm_profdata, "merge", "-o", profile, *raw])
        stats = subprocess.run([self.llvm_profdata, "show", "--all-functions", profile], capture_output=True,
                               text=True).stdout
        executed = [int(c) for c in re.findall(
            r"(?m)^  func_[0-9A-Fa-f]+\S*:\n(?:    .*\n)*?    Function count: (\d+)", stats)]
        ran = sum(1 for count in executed if count > 0)
        if ran == 0:
            die("the optimization profile counted no translated game functions")
        print(f"optimization profile: {ran} translated functions ran ({len(executed)} in the module)")
        receipt.write_text(json.dumps({"fingerprint": key, "profile": sha256_file(profile),
                                       "trained": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}, indent=2))
        print("the training APK is still installed: put the real one back with scripts/android/install.py")
        return self.hashed_profile(profile)

    # The app's own training (--train-app): libmain.so instrumented
    # (-fprofile-instr-generate: the host, GXRuntime, Aurora and SDL; Dawn is
    # prebuilt) with the real game module, one drawn and paced playback of the
    # opening, then libmain.so compiled again with those counts. The module's
    # training cannot stand in for it: headless, it never runs the GX worker's
    # translation or the render thread, the app's busiest code (the GX worker
    # alone is most of a core on the sea).
    def app_training_fingerprint(self, module):
        """What the app's counts depend on: the game module it played with, the
        app's own sources (android/ and the host), RecompCore (GXRuntime and
        Aurora), the compiler and the CPU. --retrain trains again regardless."""
        key = hashlib.sha256()
        key.update(f"{self.clang_version}\n{self.args.cpu}\n".encode())
        key.update(f"{self.git('-C', str(self.recompcore), 'rev-parse', 'HEAD')}\n".encode())
        key.update(f"{sha256_file(module)}\n".encode())
        for top in ("android", "runtime/host/src"):
            for path in sorted((ROOT / top).rglob("*")):
                if path.is_file():
                    key.update(path.relative_to(ROOT).as_posix().encode())
                    key.update(path.read_bytes())
        return key.hexdigest()

    def train_app_on_device(self, module):
        work = self.out / "pgo-app"
        work.mkdir(parents=True, exist_ok=True)
        profile = work / "app.profdata"
        receipt = work / "training.json"
        key = self.app_training_fingerprint(module)
        if profile.exists() and receipt.exists() and not self.args.retrain:
            try:
                previous = json.loads(receipt.read_text())
            except ValueError:
                previous = {}
            if previous.get("fingerprint") == key and previous.get("profile") == sha256_file(profile):
                print("reusing the app's optimization profile trained for these inputs")
                return
        self.adb_exe = self.sdk / "platform-tools/adb.exe"
        devices = subprocess.run([str(self.adb_exe), "devices"], capture_output=True, text=True).stdout
        if self.args.device not in devices:
            die(f"device {self.args.device} is not connected (adb devices)")
        lib = self.build_app(self.out / "app-train", generate=True, name="app-train")
        apk = self.package(module, lib, "WindWakerRecomp-app-training.apk", "apk-app-training", provenance=False)
        print(f"installing the app's training APK on {self.args.device}")
        self.adb("shell", "am", "force-stop", self.app_id)
        self.adb("install", "-r", "-g", apk, timeout=900)
        for old in work.glob("run-*"):
            shutil.rmtree(old, ignore_errors=True)
        try:
            raw = self.device_training_run(work / "run-drawn", "app-drawn", None, rendered=True)
        finally:
            self.adb("shell", "rm", "-f", f"{self.DEVICE_FILES}/launch.env", check=False)
            self.adb("shell", "am", "force-stop", self.app_id, check=False)
        self.run("app-training-merge", [self.llvm_profdata, "merge", "-o", profile, *raw])
        stats = subprocess.run([self.llvm_profdata, "show", profile], capture_output=True, text=True).stdout
        functions = re.search(r"Total functions: (\d+)", stats)
        print(f"the app's optimization profile: {functions[1] if functions else '?'} functions")
        receipt.write_text(json.dumps({"fingerprint": key, "profile": sha256_file(profile),
                                       "trained": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}, indent=2))
        print("the app's training APK is still installed: put the real one back with scripts/android/install.py")

    def device_training_run(self, run, name, mods, retraces=None, rendered=False):
        run.mkdir(parents=True)
        remote = f"{self.DEVICE_FILES}/pgo/{name}"
        self.adb("shell", "rm", "-rf", remote)
        self.adb("shell", "mkdir", "-p", remote)
        # The game module's training runs headless and unpaced. The app's
        # (train_app_on_device) draws every frame at the game's own pace, with
        # the player's settings: its counts are of the GX worker, Aurora and the
        # render thread at work.
        env = {
            "LLVM_PROFILE_FILE": f"{remote}/%m-%p.profraw",
            "BLUEWAKE_CARD_PATH": f"{remote}/training.card", "BLUEWAKE_SRAM": f"{remote}/sram.bin",
            "BLUEWAKE_SETTINGS": "" if rendered else "none", "BLUEWAKE_RENDERER": "" if rendered else "headless",
            "BLUEWAKE_MAX_RETRACES": str(retraces or self.TRAINING_RETRACES),
            "BLUEWAKE_WALL_PACE": "" if rendered else "0",
            "BLUEWAKE_PLAYER_PROBE": "1", "BLUEWAKE_PAD_BUTTONS": "0x0100",
            # The player-control milestone waits on the overlap phase this
            # observation latches (as in the Windows builder's training).
            "BLUEWAKE_OVERLAP_OBSERVATION": "1",
            "BLUEWAKE_PAD_PULSE_ON_TITLE_READY": "1", "BLUEWAKE_PAD_PULSE_LENGTH": "2",
            "BLUEWAKE_PAD_CONFIRM_EVENT": "any",
            "BLUEWAKE_PAD_SCRIPT": ",".join([f"{n}:0x0100:2" for n in range(17800, 22001, 150)] + self.TRAINING_RUN),
            "BLUEWAKE_MODS": mods or "",
        }
        local_env = run / "launch.env"
        local_env.write_text("".join(f"{k}={v}\n" for k, v in env.items() if v), encoding="utf-8", newline="\n")
        self.adb("push", local_env, f"{self.DEVICE_FILES}/launch.env")
        before = set(self.adb("shell", f"ls {self.DEVICE_FILES}/logs", capture=True, check=False).split())
        self.adb("shell", "input", "keyevent", "KEYCODE_WAKEUP", check=False)
        self.adb("shell", "am", "force-stop", self.app_id)
        self.adb("shell", "am", "start", "-n", f"{self.app_id}/{ACTIVITY}", "--ez", "showWhenLocked", "true")
        print(f"  training playback {name} on the device ({'drawn, paced' if rendered else 'headless, unpaced'})",
              flush=True)
        start = time.monotonic()
        log_name = None
        text = ""
        while True:
            time.sleep(15)
            elapsed = int(time.monotonic() - start)
            if log_name is None:
                now = set(self.adb("shell", f"ls {self.DEVICE_FILES}/logs", capture=True, check=False).split())
                new = sorted(now - before)
                log_name = new[-1] if new else None
            if log_name is not None:
                text = self.adb("shell", f"cat {self.DEVICE_FILES}/logs/{log_name}", capture=True, check=False)
                if "[android] host returned" in text:
                    break
                retraces = re.findall(r"retrace=(\d+)", text[-20000:])
                print(f"  {name}: {elapsed // 60}m {elapsed % 60:02d}s, retrace "
                      f"{retraces[-1] if retraces else 0}/{self.TRAINING_RETRACES}", flush=True)
            if elapsed > 60 and not self.adb("shell", "pidof", self.app_id, check=False).stdout.strip():
                die(f"the training playback {name} stopped early (device log {log_name})")
            if elapsed > 3 * 3600:
                die(f"the training playback {name} did not finish in 3 hours")
        # The instrumented module writes its counts after the host returns
        # (android_entry.c), a large file on shared storage: wait for the
        # entry's line and for the process to end before looking for it, or
        # stopping the app would cut the write short.
        written = time.monotonic()
        while True:
            text = self.adb("shell", f"cat {self.DEVICE_FILES}/logs/{log_name}", capture=True, check=False)
            done = "optimization profile written" in text or "the module is not instrumented" in text
            alive = self.adb("shell", "pidof", self.app_id, check=False).stdout.strip()
            if done and not alive:
                break
            if time.monotonic() - written > 600:
                die(f"the training playback {name} did not finish writing its profile in 10 minutes")
            time.sleep(5)
        (run / "session.log").write_text(text, encoding="utf-8")
        if retraces is None and "[player-milestone] control-admitted" not in text:
            die(f"the training playback {name} did not reach player control; profile rejected "
                f"(see {run / 'session.log'})")
        files = [f for f in self.adb("shell", f"ls {remote}", capture=True).split() if f.endswith(".profraw")]
        if not files:
            die(f"the training playback {name} wrote no profile (see {run / 'session.log'})")
        raw = []
        for f in files:
            self.adb("pull", f"{remote}/{f}", run / f, timeout=600)
            raw.append(run / f)
        print(f"  {name}: done in {int(time.monotonic() - start) // 60} min, {len(raw)} profile(s)")
        return raw

    # --- the pipeline -------------------------------------------------------------
    def build(self):
        args = self.args
        self.mods = not args.no_mods
        self.mods_pending = self.mods
        self.train_pgo = False
        self.profile = args.profile.resolve() if args.profile else None
        print(f"Building The Legend of Zelda: The Wind Waker (GameCube USA GZLE01 rev 0) for Android from {args.disc}")
        step("1/10 tools")
        self.check_tools()
        step("2/10 dependencies")
        self.dependencies()
        step("3/10 disc")
        self.disc()
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
            print(f"\nsource check passed: {self.out / 'composite-src'}")
            return
        step("7/10 mods")
        if not self.mods:
            print("skipped")
        elif not self.mods_pending:
            print("mods already in the composite source")
        else:
            self.build_mods()
        step("the last source steps (the Windows builder's, unchanged)")
        self.finish_in_place()
        lib = None
        if args.device and self.profile is None and not args.app_only:
            step("9/10 build the app (libmain.so), for the training APK")
            lib = self.build_app(profile=self.app_profile())
            step(f"local optimization training on {args.device} (the first time: an instrumented game module, "
                 "then two headless playbacks of the opening on the device)")
            self.profile = self.train_on_device(lib)
        if args.app_only:
            module = self.out / "composite/gGZLE01_recomp.so"
            if not module.exists():
                die("--app-only needs a compiled game module")
        else:
            step(f"8/10 compile the game module (-O{args.opt_level}, -mcpu={args.cpu}; this is the long step)")
            start = time.monotonic()
            module = self.compile_module()
            print(f"game module: {module} ({int(time.monotonic() - start) // 60} min)")
        if args.train_app:
            if not args.device:
                die("--train-app needs --device")
            step(f"the app's optimization training on {args.device} (an instrumented app, then one drawn "
                 "playback of the opening at the game's pace)")
            self.train_app_on_device(module)
            lib = None
        if lib is None or args.train_app:
            step("9/10 build the app (libmain.so)")
            lib = self.build_app(profile=self.app_profile())
        print(f"app: {lib}")
        step("10/10 package")
        apk = self.package(module, lib)
        print(f"\nAPK: {apk}")
        print(f"  install with: python scripts/android/install.py --out {self.out}")
        print("  It contains game code translated from your disc: keep it for yourself.")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("disc", type=Path, help="your GZLE01 revision 0 disc image (.iso or .gcm)")
    parser.add_argument("--out", type=Path, default=ROOT / "build/android",
                        help="build directory (default build/android; must be git-ignored inside the checkout)")
    parser.add_argument("--jobs", type=int, default=None, help="parallel compile jobs")
    parser.add_argument("--cpu", default="cortex-a78",
                        help="-mcpu for the game module and the app (default cortex-a78: any ARMv8.2 phone or "
                             "headset of the last few years, the Quest 3 included; oryon-1 for a Snapdragon 8 Elite only)")
    parser.add_argument("--opt-level", choices=("0", "1", "2"), default="2", help="game module optimization level")
    parser.add_argument("--no-mods", action="store_true", help="skip the widescreen and Better Wind Waker variants")
    parser.add_argument("--profile", type=Path, help="an optimization profile (.profdata) for the game module")
    parser.add_argument("--device", metavar="SERIAL",
                        help="train the optimization profile on this adb device (the phone the game is for)")
    parser.add_argument("--retrain", action="store_true", help="train again (the module, and with --train-app the app) although nothing it depends on changed")
    parser.add_argument("--train-app", action="store_true",
                        help="also train the app (libmain.so) on --device, with a drawn playback; the profile "
                             "(OUT/pgo-app) is then used by every app build")
    parser.add_argument("--no-app-profile", action="store_true", help="build the app without its trained profile")
    parser.add_argument("--accept-new-composite", action="store_true",
                        help="continue if the generated source differs from the verified one")
    parser.add_argument("--source-only", action="store_true", help="stop after generating the source")
    parser.add_argument("--app-only", action="store_true", help="reuse the compiled game module; rebuild the app")
    parser.add_argument("--package", help="the application id (default: the profile's bundle id, dev.bluewake.BlueWake); an existing install keeps its saves only under its own id")
    parser.add_argument("--label", default=DEFAULT_LABEL, help=f"the app's name on the device (default {DEFAULT_LABEL})")
    parser.add_argument("--debuggable", action="store_true",
                        help="mark the APK debuggable (run-as, debuggerd); it is always profileable by simpleperf")
    parser.add_argument("--llvm", type=Path, help="an LLVM for Windows (clang-cl) for the translator and extractor")
    parser.add_argument("--ndk", type=Path, help="the Android NDK (default: the newest in the SDK)")
    parser.add_argument("--sdk", type=Path, help="the Android SDK (default: ANDROID_HOME)")
    parser.add_argument("--jdk", type=Path, help="a JDK 17 (default: JAVA_HOME)")
    args = parser.parse_args()
    args.jobs_auto = args.jobs is None
    if args.jobs is None:
        args.jobs = wb.default_jobs()
    args.no_train = True
    args.no_pgo = True
    args.march = f"android-{args.cpu}"  # the training fingerprint's CPU level
    args.check_only = False
    args.out = args.out.resolve()
    try:
        rel = args.out.relative_to(ROOT)
    except ValueError:
        rel = None
    if rel is not None:
        ignored = subprocess.run(["git", "check-ignore", "-q", str(args.out) + os.sep], cwd=ROOT).returncode == 0
        if str(rel) == "." or not ignored:
            parser.error("--out inside this checkout must be git-ignored; use build/android")
    args.out.mkdir(parents=True, exist_ok=True)
    try:
        AndroidBuilder(args).build()
    except wb.BuildError as error:
        print(f"\nbuilder: {error}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("\nbuilder: interrupted; rerun the same command to continue", file=sys.stderr)
        sys.exit(130)


if __name__ == "__main__":
    main()
