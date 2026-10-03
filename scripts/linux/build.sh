#!/usr/bin/env bash
# Linux native build: host + Aurora + DSP donor + game module, from a GZLE01
# rev 0 disc. Reuses the shared builder pipeline (scripts/builder/build.sh) but
# compiles the host and the composite for this Linux system instead of iOS.
#
#   scripts/linux/build.sh /path/to/GZLE01.iso [--no-mods] [--no-train] [--jobs N] [--out DIR]
#
# The game module (gGZLE01_recomp.so) is translated from YOUR disc and stays
# private; the host, Aurora and DSP donor are the port's own code.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"

iso="" out="$root/build/linux" jobs=$(nproc 2>/dev/null || echo 4) mods=1 train=1
while [ $# -gt 0 ]; do
    case "$1" in
        --no-mods) mods=0; shift ;;
        --no-train) train=0; shift ;;
        --jobs) jobs=$2; shift 2 ;;
        --out) out=$2; shift 2 ;;
        -h|--help) sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) [ -z "$iso" ] || { echo "one disc image only" >&2; exit 1; }; iso=$1; shift ;;
    esac
done
[ -n "$iso" ] || { echo "usage: scripts/linux/build.sh DISC.iso [options]" >&2; exit 1; }
[ -f "$iso" ] || { echo "disc image not found: $iso" >&2; exit 1; }

mkdir -p "$out"

echo "==> 1/5 dependencies (Dawn prebuilt package)"
dawn_url=https://github.com/encounter/dawn/releases/download/v20260618.032059/dawn-linux-x86_64.tar.gz
deps=$out/deps
mkdir -p "$deps"
dawn_tar=$deps/dawn-linux-x86_64.tar.gz
if [ ! -f "$dawn_tar" ]; then
    curl -fL -o "$dawn_tar" "$dawn_url"
fi
if [ ! -f "$deps/dawn-linux/lib/cmake/Dawn/DawnConfig.cmake" ]; then
    rm -rf "$deps/dawn-linux" && mkdir -p "$deps/dawn-linux"
    tar xzf "$dawn_tar" -C "$deps/dawn-linux"
fi
echo "Dawn: $dawn_url"

echo "==> 2/5 dependencies (RecompCore + DolRecomp)"
python3 scripts/apply_recompcore_patches.py >/dev/null

echo "==> 3/5 extract, translate, generate composite"
env root="$root" out="$out" iso="$iso" jobs="$jobs" mods="$mods" \
    bash -c '
        . "$root/scripts/builder/profiles/bluewake.sh"
        PROFILE_COMPOSITE_PGO= PROFILE_HOST_PGO=   # no bundled macOS profiles
        profile_dependencies
        profile_extract
        profile_translate
        profile_generate
        [ "$mods" -eq 0 ] || profile_mods
    '

echo "==> 4/5 compile the game module (gGZLE01_recomp.so)"
cmake -S cmake/composite -B "$out/composite-linux" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCOMPOSITE_OPTIMIZATION_LEVEL=2 \
    -DCOMPOSITE_DIR="$out/composite-src" \
    -DGXRUNTIME_DIR="$root/ref/recompcore/GXRuntime" \
    -DABI_DIR="$root/ref/recompcore/Source/Core/Core/PowerPC/StaticRecomp"
cmake --build "$out/composite-linux" -j "$jobs"

echo "==> 5/5 build the host (bluewake_host + Aurora + DSP donor)"
cmake -S scripts/builder/training -B "$out/host-linux" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    -DAURORA_DAWN_PROVIDER=system \
    -DCMAKE_PREFIX_PATH="$deps/dawn-linux" \
    -DAURORA_SDL3_PROVIDER=vendor -DAURORA_SDL3_LINKAGE=shared \
    -DAURORA_DAWN_LINKAGE=static \
    -DBLUEWAKE_ENABLE_DSP_ADAPTER=OFF
cmake --build "$out/host-linux" --target bluewake_host -j "$jobs"

libexec="$out/run/lib/wind-waker-recomp"
mkdir -p "$libexec"
cp "$out/host-linux/host/bluewake_host" "$libexec/"
cp "$out/composite-linux/gGZLE01_recomp.so" "$libexec/"
cp "$root/ref/recompcore/Data/Sys/GC/dsp_rom.bin" "$libexec/"
cp "$root/ref/recompcore/Data/Sys/GC/dsp_coef.bin" "$libexec/"
clang -O2 -o "$libexec/disc_extract" scripts/ios/disc_extract.c \
    apple/ios/src/disc_import.c -Iapple/ios/src -lcrypto \
    2>/dev/null || gcc -O2 -o "$libexec/disc_extract" scripts/ios/disc_extract.c \
    apple/ios/src/disc_import.c -Iapple/ios/src -lcrypto
mkdir -p "$out/run/bin"
cp scripts/linux/run.sh "$out/run/bin/wind-waker-recomp"
chmod +x "$out/run/bin/wind-waker-recomp"

echo
echo "built: $out/run (launch with $out/run/bin/wind-waker-recomp)"
echo "game module: $(shasum -a 256 "$libexec/gGZLE01_recomp.so" | awk '{print $1}')"
