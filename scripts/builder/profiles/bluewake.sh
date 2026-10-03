# BlueWake game profile for scripts/builder/build.sh: The Legend of Zelda:
# The Wind Waker, GameCube USA (GZLE01, revision 0).
#
# A profile is sourced by the generic pipeline. It sets the variables below and
# defines the hook functions the pipeline calls in order. Everything specific to
# this game, its translator and its app lives here; the pipeline only
# orchestrates, logs, embeds, signs, packages and checks. A new port adds its own
# profile file with the same variables and hooks (docs/BUILDER.md).
#
# Hooks run with the pipeline's helpers (run LOG cmd..., die, step) and its
# variables (root, out, logs, jobs, iso, mods, opt_level, accept_new, ...):
#   profile_check_tools   extra tools this game needs
#   profile_dependencies  fetch pinned runtime and translator sources
#   profile_extract       disc image -> $out/game (verifies the disc)
#   profile_translate     $out/game -> translated C
#   profile_generate      translated C -> $out/composite-src, verified by digest
#   profile_mods          optional code mods into the composite source
#   profile_compile       composite source -> sets module=<path to PROFILE_MODULE>
#   profile_build_app     the app bundle -> sets app=<path to .app>

PROFILE_NAME=bluewake
PROFILE_TITLE="The Legend of Zelda: The Wind Waker (GameCube USA GZLE01 rev 0)"
PROFILE_APP_NAME=BlueWake
PROFILE_BUNDLE_ID=dev.bluewake.BlueWake
PROFILE_MODULE=gGZLE01_recomp.dylib
PROFILE_DEFAULT_OUT=build/device
PROFILE_HAS_MODS=1
# Bundled optimization profiles, trained on the macOS host: the runtime's
# dispatch and memory code, and the app's host code. Neither names or contains
# any game code (both pass release_gate.py). About 5 percent faster on the
# iPad than none; docs/BUILDER.md.
PROFILE_COMPOSITE_PGO=scripts/builder/profiles/bluewake/composite-rt.profdata
PROFILE_HOST_PGO=scripts/builder/profiles/bluewake/host.profdata

# Wind Waker Recomp builds from its own copies of BlueWake's RecompCore and
# DolRecomp (chrissotraidis 2d60636 and 5c91d6e, each plus this fork's changes:
# patches/recompcore/0098-0131 and the save states' two, patches/dolrecomp/0019).
RECOMPCORE_URL=https://github.com/elliotttate/RecompCore.git
RECOMPCORE_SHA=201e909e918452b4ccf8453cb7e1b21b8dbe7e12
DOLRECOMP_SHA=b8b534591cba8ca7cd43943a655ee6e2591cf5de
DAWN_URL=https://github.com/encounter/dawn/releases/download/v20260618.032059/dawn-ios-arm64.tar.gz
DAWN_SHA256=ada0bafc173152d80eba7c3b2f9609a71185d5809cbd5dd3251b91a0803a7ae2
# Digest of the generated composite source (scripts/ios/composite_manifest.py)
# for GZLE01 USA rev 0 with the translator above: 754 files (the mod-ready
# dispatcher and an empty mod_variants.inc since 2026-09-26; docs/MODS.md).
COMPOSITE_DIGEST=54f54434c3f9c899d43a96373dc0b4c1aed0e50db8b820b9698dfa76571a770a

profile_check_tools() {
    # The mods need only python3's standard library: Better Wind Waker's
    # settings are game options built from mods/betterww/options.txt, not its
    # patcher (which needed PyYAML and Pillow).
    :
}

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

    deps=$root/build/deps
    mkdir -p "$deps"
    local dawn_tar=$deps/dawn-ios-arm64.tar.gz
    if [ ! -f "$dawn_tar" ] || [ "$(shasum -a 256 "$dawn_tar" | awk '{print $1}')" != "$DAWN_SHA256" ]; then
        run dawn-download curl -fL -o "$dawn_tar" "$DAWN_URL"
    fi
    [ "$(shasum -a 256 "$dawn_tar" | awk '{print $1}')" = "$DAWN_SHA256" ] || die "Dawn package checksum mismatch"
    if [ ! -f "$deps/dawn-ios/lib/cmake/Dawn/DawnConfig.cmake" ]; then
        rm -rf "$deps/dawn-ios" && mkdir -p "$deps/dawn-ios"
        tar xzf "$dawn_tar" -C "$deps/dawn-ios"
    fi
    echo "Dawn iOS package $DAWN_SHA256"
}

profile_extract() {
    mkdir -p "$out/tools"
    run disc-extract-build clang -O2 -o "$out/tools/disc_extract" scripts/ios/disc_extract.c \
        apple/ios/src/disc_import.c -Iapple/ios/src
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
    elif [ "$accept_new" -eq 1 ]; then
        echo "composite source digest $digest differs from the verified $COMPOSITE_DIGEST (accepted)"
    else
        die "composite source digest $digest differs from the verified $COMPOSITE_DIGEST (wrong disc revision or translator?); --accept-new-composite overrides"
    fi
    # Reuse only a verified tree made by the same generators and mod selection.
    # Checking the base digest alone used to retain mod variants after --no-mods.
    local inputs current saved
    inputs=$( { printf '%s\n' "$digest" "$mods"; shasum -a 256 \
        "$root/scripts/mods/"*.py "$root/scripts/mods/"*.sh \
        "$root/mods/widescreen/"*.gecko "$root/mods/betterww/options.txt"; } | shasum -a 256 | awk '{print $1}')
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

profile_train() {
    local args=(--disc "$iso" --out "$out" --jobs "$jobs")
    [ -z "$training_save" ] || args+=(--save "$training_save")
    run local-training python3 "$root/scripts/builder/train_local_pgo.py" "${args[@]}"
    [ -s "$out/pgo-local/composite.profdata" ] || die "local training produced no game profile"
    composite_pgo+=("$out/pgo-local/composite.profdata")
    # Headless boot training does not exercise the renderer; retain the
    # existing host profile until rendered local training is validated.
}

profile_compile() {
    run native-gpr-prepare python3 "$root/scripts/mods/prepare_native_gpr.py" "$out/composite-src"
    if [ -f "$out/composite-src/simulation_60hz.json" ]; then
        run timing-recertify python3 "$root/scripts/mods/prepare_simulation_60hz.py" "$out/composite-src"
    fi
    run native-gpr-certify python3 "$root/scripts/mods/prepare_native_gpr.py" "$out/composite-src"
    run native-math-certify python3 "$root/scripts/mods/prepare_native_math.py" "$out/composite-src"
    local flags="-mcpu=$device_cpu"
    if [ ${#composite_pgo[@]} -gt 0 ]; then
        run composite-pgo-merge xcrun llvm-profdata merge -o "$out/composite.profdata" "${composite_pgo[@]}"
        # The profile is a compiler input but not a C header dependency. Put
        # its hash in the flag so Ninja recompiles when the counters change.
        local profile_hash profile_path
        profile_hash=$(shasum -a 256 "$out/composite.profdata" | awk '{print $1}')
        mkdir -p "$out/profiles"
        profile_path=$out/profiles/composite-$profile_hash.profdata
        cp "$out/composite.profdata" "$profile_path"
        flags="$flags $(pgo_flags "$profile_path")"
        echo "with the composite profile(s): ${composite_pgo[*]}"
    fi
    run composite-configure cmake -S cmake/composite -B "$out/composite-ios" -G Ninja \
        -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_FLAGS=$flags" \
        -DCOMPOSITE_OPTIMIZATION_LEVEL="$opt_level" \
        -DCOMPOSITE_DIR="$out/composite-src" -DGXRUNTIME_DIR="$recompcore/GXRuntime" \
        -DABI_DIR="$recompcore/Source/Core/Core/PowerPC/StaticRecomp"
    run composite-build cmake --build "$out/composite-ios" -j "$jobs"
    module=$out/composite-ios/$PROFILE_MODULE
}

profile_build_app() {
    local host_flags=""
    if [ -n "$host_pgo" ]; then
        local profile_hash profile_path
        profile_hash=$(shasum -a 256 "$host_pgo" | awk '{print $1}')
        mkdir -p "$out/profiles"
        profile_path=$out/profiles/host-$profile_hash.profdata
        cp "$host_pgo" "$profile_path"
        host_flags=$(pgo_flags "$profile_path")
        echo "with the host profile $host_pgo"
    fi
    run app-configure cmake -S apple/ios -B "$out/app" -G Ninja \
        -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
        -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DBUILD_SHARED_LIBS=OFF -DPNG_SHARED=OFF \
        -DAURORA_DAWN_PROVIDER=system -DDawn_DIR="$deps/dawn-ios/lib/cmake/Dawn" \
        -DAURORA_SDL3_PROVIDER=vendor -DAURORA_SDL3_LINKAGE=static -DAURORA_DAWN_LINKAGE=static \
        "-DCMAKE_C_FLAGS=$host_flags" "-DCMAKE_CXX_FLAGS=$host_flags" \
        '-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew;/usr/local' -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON
    run app-build cmake --build "$out/app" --target BlueWake -j "$jobs"
    app=$out/app/BlueWake.app
}
