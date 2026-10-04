# BlueWake game profile for the Linux build (scripts/linux/build.sh):
# The Legend of Zelda: The Wind Waker, GameCube USA (GZLE01, revision 0).
#
# Mirrors scripts/builder/profiles/bluewake.sh (the iOS profile) with the
# platform-specific bits replaced: Dawn is the linux-x86_64 prebuilt, the game
# module is a .so, and profile_compile / profile_build_app compile for this
# host instead of arm64 iOS. Everything game-specific (disc check, translator,
# composite digest, mods) is identical.
#
# Hooks run with the driver's helpers (run LOG cmd..., die, step) and variables
# (root, out, logs, jobs, iso, mods, opt_level, ...).

PROFILE_NAME=bluewake-linux
PROFILE_TITLE="The Legend of Zelda: The Wind Waker (GameCube USA GZLE01 rev 0)"
PROFILE_APP_NAME="Wind Waker Recomp"
PROFILE_MODULE=gGZLE01_recomp.so
PROFILE_DEFAULT_OUT=build/linux
PROFILE_HAS_MODS=1

RECOMPCORE_URL=https://github.com/elliotttate/RecompCore.git
RECOMPCORE_SHA=8ab24daee9c641634fda5cac30389ad4b2cfda5e
DOLRECOMP_SHA=b8b534591cba8ca7cd43943a655ee6e2591cf5de
DAWN_URL=https://github.com/encounter/dawn/releases/download/v20260618.032059/dawn-linux-x86_64.tar.gz
DAWN_SHA256=650a5479d8ecdfaad1a74079715010427fc30dab6c266fe46320948340897962
# Digest of the generated composite source (scripts/ios/composite_manifest.py)
# for GZLE01 USA rev 0 with the pinned translator: identical to the iOS profile.
COMPOSITE_DIGEST=54f54434c3f9c899d43a96373dc0b4c1aed0e50db8b820b9698dfa76571a770a

profile_check_tools() { :; }

profile_dependencies() {
    recompcore=$root/ref/recompcore
    local fresh_clone=0
    if [ ! -e "$recompcore/.git" ]; then
        if [ -e "$recompcore" ] && [ -n "$(ls -A "$recompcore")" ]; then
            die "ref/recompcore exists but is not a git checkout: move it aside and rerun"
        fi
        mkdir -p "$recompcore"
        git -C "$recompcore" init -q
        fresh_clone=1
    fi
    if [ "$(git -C "$recompcore" rev-parse HEAD 2>/dev/null || true)" != "$RECOMPCORE_SHA" ]; then
        if [ -n "$(git -C "$recompcore" status --porcelain --untracked-files=no 2>/dev/null || true)" ]; then
            die "ref/recompcore has local changes and is not at $RECOMPCORE_SHA: move it aside and rerun"
        fi
        echo "fetching RecompCore $RECOMPCORE_SHA (GXRuntime, vendored Aurora, DolRecomp pointer)"
        git -C "$recompcore" remote remove bluewake >/dev/null 2>&1 || true
        git -C "$recompcore" remote add bluewake "$RECOMPCORE_URL"
        if [ "$fresh_clone" -eq 1 ]; then
            run recompcore-fetch git -C "$recompcore" fetch --depth 1 bluewake "$RECOMPCORE_SHA"
        else
            run recompcore-fetch git -C "$recompcore" fetch bluewake "$RECOMPCORE_SHA"
        fi
        git -C "$recompcore" checkout -q --detach FETCH_HEAD
    fi
    [ "$(git -C "$recompcore" rev-parse HEAD)" = "$RECOMPCORE_SHA" ] || die "ref/recompcore is not at $RECOMPCORE_SHA"
    git -C "$recompcore" submodule sync -q -- DolRecomp
    if [ "$(git -C "$recompcore/DolRecomp" rev-parse HEAD 2>/dev/null || true)" != "$DOLRECOMP_SHA" ]; then
        run dolrecomp-fetch git -C "$recompcore" submodule update --init --depth 1 -- DolRecomp
    fi
    [ "$(git -C "$recompcore/DolRecomp" rev-parse HEAD)" = "$DOLRECOMP_SHA" ] || die "ref/recompcore/DolRecomp is not at $DOLRECOMP_SHA"
    run recompcore-patches python3 "$root/scripts/apply_recompcore_patches.py"
    if [ -n "$(git -C "$recompcore/DolRecomp" status --porcelain --untracked-files=no)" ]; then
        die "ref/recompcore/DolRecomp has local changes; the build must use the pinned translator exactly"
    fi
    echo "RecompCore $RECOMPCORE_SHA, DolRecomp $DOLRECOMP_SHA"

    deps=$out/deps
    mkdir -p "$deps"
    local dawn_tar=$deps/dawn-linux-x86_64.tar.gz
    if [ ! -f "$dawn_tar" ] || [ "$(sha256sum "$dawn_tar" | awk '{print $1}')" != "$DAWN_SHA256" ]; then
        run dawn-download curl -fL -o "$dawn_tar" "$DAWN_URL"
    fi
    [ "$(sha256sum "$dawn_tar" | awk '{print $1}')" = "$DAWN_SHA256" ] || die "Dawn package checksum mismatch"
    if [ ! -f "$deps/dawn-linux/lib/cmake/Dawn/DawnConfig.cmake" ]; then
        rm -rf "$deps/dawn-linux" && mkdir -p "$deps/dawn-linux"
        tar xzf "$dawn_tar" -C "$deps/dawn-linux"
    fi
    echo "Dawn linux-x86_64 package $DAWN_SHA256"
}

profile_extract() {
    mkdir -p "$out/tools"
    run disc-extract-build clang -O2 -o "$out/tools/disc_extract" scripts/ios/disc_extract.c \
        apple/ios/src/disc_import.c -Iapple/ios/src -lcrypto
    # disc_extract checks the disc ID (GZLE01) and the executable's hash
    # (revision 0) and refuses anything else.
    run disc-extract "$out/tools/disc_extract" "$iso" "$out/game"
    [ "$(ls "$out/game/rels" | wc -l | tr -d ' ')" = 415 ] || die "expected 415 RELs in $out/game/rels"
    echo "main.dol and 415 RELs in $out/game"
}

profile_translate() {
    run dolrecomp-configure cmake -S "$recompcore/DolRecomp" -B "$out/dolrecomp" -G Ninja -DCMAKE_BUILD_TYPE=Release
    run dolrecomp-build cmake --build "$out/dolrecomp" --target dolrecomp -j "$jobs"
    local dolrecomp=$out/dolrecomp/dolrecomp
    rm -rf "$out/translated.new" && mkdir -p "$out/translated.new"
    run translate-dol "$dolrecomp" --gamecube --backend c --cpu gekko --partition-instructions 4096 \
        "$out/game/main.dol" "$out/translated.new/dol" -j "$jobs"
    run translate-rels "$dolrecomp" --gamecube --backend c --cpu gekko --rel-base 0xC0400000 \
        "$out/game/rels" "$out/translated.new/rels" -j "$jobs"
    rm -rf "$out/translated" && mv "$out/translated.new" "$out/translated"
    echo "translated: $(ls "$out/translated/dol/generated/chunks" | wc -l | tr -d ' ') DOL chunks, $(ls "$out/translated/rels/generated/rels" | wc -l | tr -d ' ') RELs"
}

profile_generate() {
    rm -rf "$out/composite-src.new"
    run composite-generate python3 scripts/generate_composite.py \
        --dol-dir "$out/translated/dol/generated" --rels-dir "$out/translated/rels/generated/rels" \
        --rels-bin-dir "$out/game/rels" --main-dol "$out/game/main.dol" --output-dir "$out/composite-src.new"
    tail -1 "$logs/composite-generate.log"
    local digest
    digest=$(python3 scripts/ios/composite_manifest.py "$out/composite-src.new" | awk '{print $1}')
    if [ "$digest" = "$COMPOSITE_DIGEST" ]; then
        echo "composite source digest $digest: the verified tree"
    else
        die "composite source digest $digest differs from the verified $COMPOSITE_DIGEST (wrong disc revision or translator?)"
    fi
    # Reuse only a verified tree made by the same generators and mod selection.
    local inputs current saved
    inputs=$( { printf '%s\n' "$digest" "$mods"; sha256sum \
        "$root/scripts/mods/"*.py "$root/scripts/mods/"*.sh \
        "$root/mods/widescreen/"*.gecko "$root/mods/betterww/options.txt"; } | sha256sum | awk '{print $1}')
    current=""
    if [ -d "$out/composite-src" ]; then
        current=$(python3 scripts/ios/composite_manifest.py "$out/composite-src" | awk '{print $1}')
    fi
    saved=$(cat "$out/composite-final.digest" 2>/dev/null || true)
    if [ -n "$current" ] && [ "$current" = "$saved" ] && \
       [ "$(cat "$out/composite-inputs.digest" 2>/dev/null || true)" = "$inputs" ]; then
        rm -rf "$out/composite-src.new"
    else
        if [ -d "$out/composite-src" ]; then
            local previous
            previous=$(mktemp -d "$out/composite-previous.XXXXXX")
            mv "$out/composite-src" "$previous/source"
            echo "previous generated source preserved at $previous/source"
        fi
        mv "$out/composite-src.new" "$out/composite-src"
        echo "$digest" > "$out/composite-src.digest"
        echo "$inputs" > "$out/composite-inputs.digest"
        echo "$digest" > "$out/composite-final.digest"
        printf '%s\n' pending > "$out/mods.done"
    fi
}

profile_mods() {
    # Widescreen, Better Wind Waker's options and both together, as variants
    # compiled into the same module (docs/MODS.md). Done once per composite source.
    if [ "$(cat "$out/mods.done" 2>/dev/null || true)" = complete ]; then
        echo "mods already in $out/composite-src"
        return
    fi
    run mods scripts/mods/build_mods.sh "$out" "$iso"
    python3 scripts/ios/composite_manifest.py "$out/composite-src" | awk '{print $1}' > "$out/composite-final.digest"
    printf '%s\n' complete > "$out/mods.done"
    echo "widescreen and Better Wind Waker variants added"
}

profile_compile() {
    local flags=""
    if [ ${#composite_pgo[@]} -gt 0 ]; then
        run composite-pgo-merge llvm-profdata merge -o "$out/composite.profdata" "${composite_pgo[@]}"
        local profile_hash profile_path
        profile_hash=$(sha256sum "$out/composite.profdata" | awk '{print $1}')
        mkdir -p "$out/profiles"
        profile_path=$out/profiles/composite-$profile_hash.profdata
        cp "$out/composite.profdata" "$profile_path"
        flags=$(pgo_flags "$profile_path")
        echo "with the composite profile(s): ${composite_pgo[*]}"
    fi
    run composite-configure cmake -S cmake/composite -B "$out/composite-linux" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_FLAGS=$flags" \
        -DCOMPOSITE_OPTIMIZATION_LEVEL="$opt_level" \
        -DCOMPOSITE_DIR="$out/composite-src" -DGXRUNTIME_DIR="$recompcore/GXRuntime" \
        -DABI_DIR="$recompcore/Source/Core/Core/PowerPC/StaticRecomp"
    run composite-build cmake --build "$out/composite-linux" -j "$jobs"
    module=$out/composite-linux/$PROFILE_MODULE
}

profile_build_app() {
    run host-configure cmake -S scripts/builder/training -B "$out/host-linux" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
        -DAURORA_DAWN_PROVIDER=system -DCMAKE_PREFIX_PATH="$deps/dawn-linux" \
        -DAURORA_SDL3_PROVIDER=vendor -DAURORA_SDL3_LINKAGE=shared \
        -DAURORA_DAWN_LINKAGE=static -DBLUEWAKE_ENABLE_DSP_ADAPTER=OFF
    run host-build cmake --build "$out/host-linux" --target bluewake_host -j "$jobs"
    app=$out/host-linux/host
}
