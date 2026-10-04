#!/usr/bin/env bash
# Linux native build: host + Aurora + DSP donor + game module, from a GZLE01
# rev 0 disc. Same pipeline as scripts/builder/build.sh (the Mac driver), but
# sources the bluewake-linux profile and drops the iOS/app-signing steps.
#
#   scripts/linux/build.sh /path/to/GZLE01.iso [--no-mods] [--jobs N] [--out DIR]
#
# The disc must be an uncompressed .iso or .gcm. The game module
# (gGZLE01_recomp.so) is translated from YOUR disc and stays private; the
# host, Aurora and DSP donor are the port's own code.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"

iso="" out="$root/build/linux" jobs=$(nproc 2>/dev/null || echo 4) mods=1 opt_level=2
while [ $# -gt 0 ]; do
    case "$1" in
        --no-mods) mods=0; shift ;;
        --jobs) jobs=$2; shift 2 ;;
        --out) out=$2; shift 2 ;;
        -h|--help) awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"; exit 0 ;;
        *) [ -z "$iso" ] || { echo "one disc image only" >&2; exit 1; }; iso=$1; shift ;;
    esac
done
[ -n "$iso" ] || { echo "usage: scripts/linux/build.sh DISC.iso [options] (--help)" >&2; exit 1; }
[ -f "$iso" ] || { echo "disc image not found: $iso" >&2; exit 1; }
iso=$(cd "$(dirname "$iso")" && pwd)/$(basename "$iso")
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo "--jobs must be a positive integer" >&2; exit 1; }

# --- pipeline helpers, same contract as scripts/builder/build.sh ---
die() { echo "builder: $*" >&2; exit 1; }
step() { echo; echo "==> $*"; }
logs=$out/logs
mkdir -p "$logs"
run() {  # run LOGNAME command...: periodic progress plus complete file log
    local log=$logs/$1.log; shift
    if ! python3 "$root/scripts/builder/run_stage.py" --log "$log" -- "$@"; then
        die "failed: $* (full log $log)"
    fi
}
# Cold code would trigger clang's machine outliner; keep it at plain -O2.
pgo_flags() {
    python3 - "$1" <<'PY_FLAGS'
import shlex, sys
print(shlex.quote('-fprofile-instr-use=' + sys.argv[1]),
      '-mllvm -enable-machine-outliner=never',
      '-Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-backend-plugin')
PY_FLAGS
}

# No bundled PGO profiles on Linux yet (they are trained on the macOS host),
# so we compile the game module and host without profile data.
composite_pgo=()

profile_file=$root/scripts/builder/profiles/bluewake-linux.sh
[ -f "$profile_file" ] || die "no profile $profile_file"
. "$profile_file"

mkdir -p "$out"

step "tools"
for tool in clang cmake ninja python3 git curl sha256sum; do
    command -v "$tool" >/dev/null || die "missing $tool"
done
cmake_version=$(cmake --version | head -1 | awk '{print $3}')
python3 - "$cmake_version" <<'EOF' || die "CMake 3.25 or newer is required"
import sys
v = tuple(int(x) for x in sys.argv[1].split('.')[:2])
sys.exit(0 if v >= (3, 25) else 1)
EOF
profile_check_tools
echo "clang $(clang --version | head -1 | awk '{print $3}'), cmake $cmake_version, $jobs jobs"

step "dependencies"
profile_dependencies

step "extract the game from the disc"
profile_extract

step "translate"
profile_translate

step "generate the composite source"
profile_generate

step "mods"
if [ "$mods" -eq 1 ]; then profile_mods; else echo "skipped"; fi

step "compile the game module (gGZLE01_recomp.so; this is the long step)"
start=$(date +%s)
module=""
profile_compile
[ -f "$module" ] || die "the game module was not produced"
echo "game module built in $(( ($(date +%s) - start) / 60 )) min: $module"

step "build the host (bluewake_host + Aurora + DSP donor)"
app=""
profile_build_app
[ -x "$app/bluewake_host" ] || die "the host was not produced"

# --- assemble the runnable tree (what make_appimage.sh consumes) ---
libexec="$out/run/lib/wind-waker-recomp"
mkdir -p "$libexec"
cp "$app/bluewake_host" "$libexec/"
cp "$module" "$libexec/"
cp "$root/ref/recompcore/Data/Sys/GC/dsp_rom.bin" "$libexec/"
cp "$root/ref/recompcore/Data/Sys/GC/dsp_coef.bin" "$libexec/"
cp "$out/tools/disc_extract" "$libexec/"
mkdir -p "$out/run/bin"
cp scripts/linux/run.sh "$out/run/bin/wind-waker-recomp"
chmod +x "$out/run/bin/wind-waker-recomp"

echo
echo "built: $out/run (launch with $out/run/bin/wind-waker-recomp)"
echo "game module: $(sha256sum "$module" | awk '{print $1}')"
