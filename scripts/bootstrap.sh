#!/usr/bin/env bash
# BlueWake bootstrap: clone pinned public dependencies into ignored ref/
# Requires: git. Does NOT require or download any game data.
set -euo pipefail
cd "$(dirname "$0")/.."

LOCK=config/dependencies.lock.json
if [ ! -f "$LOCK" ]; then echo "ERROR: $LOCK not found"; exit 1; fi

mkdir -p ref

# Read each dependency URL and SHA from the lock and clone if missing
python3 - "$LOCK" << 'PYEOF'
import json, subprocess, sys, pathlib
lock = json.load(open(sys.argv[1]))
ref_dir = pathlib.Path("ref")
for dep in lock["dependencies"]:
    dep_id = dep["id"]
    url = dep["url"]
    sha = dep["sha"]
    if dep.get("submodule_of"):
        print(f"  {dep_id}: provided by {dep['submodule_of']} at {dep['submodule_path']}")
        continue
    local = dep.get("local_checkout")
    path = pathlib.Path(local["path"]) if local else ref_dir / dep_id
    if path.exists() and (path / ".git").exists():
        current = subprocess.check_output(["git", "-C", str(path), "rev-parse", "HEAD"], text=True).strip()
        status = "OK" if current == sha else f"STALE (at {current[:12]}, want {sha[:12]})"
        print(f"  {dep_id}: {status}")
    else:
        print(f"  {dep_id}: cloning {url} at {sha[:12]}...")
        subprocess.run(["git", "clone", "--quiet", url, str(path)], check=True)
        subprocess.run(["git", "-C", str(path), "checkout", "--quiet", sha], check=True)
        print(f"  {dep_id}: cloned at {sha[:12]}")
PYEOF

# DolRecomp is RecompCore's DolRecomp submodule (elliotttate/DolRecomp, from chrissotraidis/DolRecomp).
if [ -e ref/recompcore/.git ]; then
    git -C ref/recompcore submodule sync -q -- DolRecomp
    git -C ref/recompcore submodule update --init -- DolRecomp
fi

python3 scripts/apply_recompcore_patches.py

echo "Bootstrap complete."
echo "For the iPad app, scripts/ios/build_device.sh fetches what it needs on its own (docs/status/DEVICE_BUILD.md)."
