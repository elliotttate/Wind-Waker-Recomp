#!/usr/bin/env bash
# Linux launcher. Game files (main.dol + 415 RELs) are prepared from the
# player's disc into their data directory on first run; they are never
# installed alongside the host. The host binary and the translated game module
# live in $BLUEWAKE_LIBEXEC (or ../lib/wind-waker-recomp next to this script).
set -euo pipefail

libexec=${BLUEWAKE_LIBEXEC:-$(cd "$(dirname "$0")/../lib/wind-waker-recomp" && pwd)}
data_home=${XDG_DATA_HOME:-${HOME:?HOME is not set}/.local/share}/wind-waker-recomp
state_home=${XDG_STATE_HOME:-${HOME:?HOME is not set}/.local/state}/wind-waker-recomp
game=$data_home/game
disc=${BLUEWAKE_DISC:-$PWD/GZLE01.iso}

if [ ! -f "$game/main.dol" ] || [ "$(find "$game/rels" -maxdepth 1 -name '*.rel' 2>/dev/null | wc -l)" -ne 415 ]; then
    [ -f "$disc" ] || {
        echo "wind-waker-recomp: set BLUEWAKE_DISC to your GZLE01 revision 0 disc image" >&2
        echo "(when run from the checkout, GZLE01.iso is used automatically)" >&2
        exit 1
    }
    pending=$data_home/game.pending
    rm -rf "$pending"
    mkdir -p "$data_home"
    "$libexec/disc_extract" "$disc" "$pending"
    rm -rf "$game"
    mv "$pending" "$game"
fi

mkdir -p "$state_home/states"
export BLUEWAKE_DOL=${BLUEWAKE_DOL:-$game/main.dol}
export BLUEWAKE_RELS_DIR=${BLUEWAKE_RELS_DIR:-$game/rels}
export BLUEWAKE_DISC=$disc
export BLUEWAKE_DSP_IROM=${BLUEWAKE_DSP_IROM:-$libexec/dsp_rom.bin}
export BLUEWAKE_DSP_COEF=${BLUEWAKE_DSP_COEF:-$libexec/dsp_coef.bin}
export BLUEWAKE_CARD_PATH=${BLUEWAKE_CARD_PATH:-$state_home/memory.card}
export BLUEWAKE_STATE_DIR=${BLUEWAKE_STATE_DIR:-$state_home/states}

exec "$libexec/bluewake_host" "$libexec/gGZLE01_recomp.so" "$@"
