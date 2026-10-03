#!/usr/bin/env bash
# Package the Linux port as a self-contained AppImage with zsync auto-update
# metadata. Consumes the staged run tree produced by scripts/linux/build.sh.
#
#   scripts/linux/make_appimage.sh [--libexec DIR] [--out FILE] [--version X.Y.Z]
#
# The AppImage bundles the host, the translated game module, Aurora/Dawn (Dawn
# is statically linked in), SDL3, the DSP donor ROMs and disc_extract. The
# player still supplies their own GZLE01 rev 0 disc at runtime (bluewake_disc_check).
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"

libexec="${BLUEWAKE_LIBEXEC:-$root/build/linux/run/lib/wind-waker-recomp}"
out="${OUT:-$root/dist/Wind-Waker-Recomp-x86_64.AppImage}"
version="${VERSION:-$(git describe --tags --always 2>/dev/null || echo 0.0.0)}"

while [ $# -gt 0 ]; do
    case "$1" in
        --libexec) libexec=$2; shift 2 ;;
        --out) out=$2; shift 2 ;;
        --version) version=$2; shift 2 ;;
        *) echo "unknown arg: $1" >&2; exit 1 ;;
    esac
done

for f in bluewake_host dsp_rom.bin dsp_coef.bin disc_extract; do
    [ -f "$libexec/$f" ] || { echo "missing $libexec/$f (run scripts/linux/build.sh first)" >&2; exit 1; }
done

appdir=$(mktemp -d /tmp/wwr-appdir.XXXXXX)
trap 'rm -rf "$appdir"' EXIT
mkdir -p "$appdir/usr/bin" "$appdir/usr/lib" "$appdir/usr/share/applications" \
         "$appdir/usr/share/icons/hicolor/256x256/apps"

cp "$libexec/bluewake_host"    "$appdir/usr/bin/"
cp "$libexec/disc_extract"     "$appdir/usr/bin/"
cp "$libexec/dsp_rom.bin"      "$appdir/usr/bin/"
cp "$libexec/dsp_coef.bin"     "$appdir/usr/bin/"

# The translated game module ships in the AppImage (the player's disc is a
# runtime verification key, not a build input -- AGENTS.md). It is absent only
# when building from a host-only checkout; warn but do not block packaging.
if [ -f "$libexec/gGZLE01_recomp.so" ]; then
    cp "$libexec/gGZLE01_recomp.so" "$appdir/usr/bin/"
else
    echo "WARNING: gGZLE01_recomp.so not present -- AppImage has no game module" >&2
fi

# Bundle every shared library the host needs except the glibc/libstdc++/libgcc
# baseline (bundling those breaks on newer/older distros -- the classic
# AppImage glibc trap). Everything else (SDL3, abseil, freetype, png, sqlite,
# brotli, bz2, zstd, ...) goes in so the image runs on a stock desktop.
exclude='libc\.so|libm\.so|libpthread\.so|libdl\.so|librt\.so|ld-linux|libgcc_s\.so|libstdc\+\+\.so'
ldd "$appdir/usr/bin/bluewake_host" | awk '/=> \// {print $3} /^\t\// {print $1}' | sort -u | while read -r lib; do
    [ -n "$lib" ] || continue
    base=$(basename "$lib")
    case "$base" in
        libc.so*|libm.so*|libpthread.so*|libdl.so*|librt.so*|ld-linux*|libgcc_s.so*|libstdc++.so*) continue ;;
    esac
    if [ -f "$lib" ] && [ ! -f "$appdir/usr/lib/$base" ]; then
        cp -L "$lib" "$appdir/usr/lib/$base"
        echo "bundled: $base"
    fi
done

# Launcher (AppRun): resolve the bundled host and exec it, passing the module.
cat > "$appdir/AppRun" <<'APPRUN'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
export BLUEWAKE_LIBEXEC="$HERE/usr/bin"
export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$HERE/usr/bin/bluewake_host" "$HERE/usr/bin/gGZLE01_recomp.so" "$@"
APPRUN
chmod +x "$appdir/AppRun"

# The AppImage runtime resolves Exec= against AppRun.
cat > "$appdir/wind-waker-recomp.desktop" <<'DESKTOP'
[Desktop Entry]
Type=Application
Name=Wind Waker Recomp
Comment=Native Linux port of The Legend of Zelda: The Wind Waker (recompiled)
Exec=wind-waker-recomp
Icon=wind-waker-recomp
Terminal=false
Categories=Game;
DESKTOP
cp "$appdir/wind-waker-recomp.desktop" "$appdir/usr/share/applications/"
ln -sf wind-waker-recomp.desktop "$appdir/AppRun.desktop" 2>/dev/null || true

# Minimal 256x256 icon (solid colour PNG generated in pure Python, no deps).
python3 - "$appdir/usr/share/icons/hicolor/256x256/apps/wind-waker-recomp.png" <<'ICON'
import struct, zlib, sys
def chunk(t, d):
    c = t + d
    return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
w = h = 256
row = b"\x00" + b"\x18\x7f\xb7\xff" * w  # a Zelda-ish teal
raw = b"".join(row for _ in range(h))
png = (b"\x89PNG\r\n\x1a\n"
       + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
       + chunk(b"IDAT", zlib.compress(raw))
       + chunk(b"IEND", b""))
open(sys.argv[1], "wb").write(png)
ICON
cp "$appdir/usr/share/icons/hicolor/256x256/apps/wind-waker-recomp.png" "$appdir/wind-waker-recomp.png"

mkdir -p "$(dirname "$out")"
echo "packaging: $out (version $version)"
ARCH=x86_64 appimagetool "$appdir" "$out" >/dev/null
echo "AppImage: $out ($(du -h "$out" | awk '{print $1}'))"

# zsync delta metadata for AppImageUpdate auto-update from GitHub releases.
zsyncmake -u "$(basename "$out")" -o "$out.zsync" "$out"
echo "zsync: $out.zsync"
sha256sum "$out" "$out.zsync" > "$(dirname "$out")/SHA256SUMS"
echo "checksums: $(dirname "$out")/SHA256SUMS"
