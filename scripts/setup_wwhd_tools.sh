#!/usr/bin/env bash
# Install the local texture importer's pinned tools; never download game data.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
venv="$root/build/wwhd-tools"
"${PYTHON:-python3}" -m venv "$venv"
"$venv/bin/python" -m pip install -r "$root/scripts/wwhd/requirements.txt"
if command -v "${CC:-cc}" >/dev/null; then
    "${CC:-cc}" -O3 -Wall -Wextra -shared -fPIC "$root/scripts/wwhd/yaz0_native.c" \
        -o "$venv/libwwhd_yaz0.so"
else
    echo 'No C compiler found; the importer will use its slower Python Yaz0 decoder.'
fi
echo "Ready: $venv/bin/python $root/scripts/import_wwhd_textures.py --help"
