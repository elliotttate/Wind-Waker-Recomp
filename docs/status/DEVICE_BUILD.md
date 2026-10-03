# Building BlueWake for an iPad or iPhone

This builds the signed BlueWake app for a physical device from a fresh checkout and your own disc
image of The Legend of Zelda: The Wind Waker (GZLE01, USA, revision 0). One script does it all:

```bash
scripts/ios/build_device.sh "/path/to/The Legend Of Zelda The Wind Waker.iso"
```

It is the Builder (`scripts/builder/build.sh`, [BUILDER.md](../BUILDER.md)) with the BlueWake profile,
and takes the same options; `--ipa FILE` also writes an unsigned IPA for sideloading
([BUILD_YOUR_OWN.md](../BUILD_YOUR_OWN.md)). The mods are built in (`--no-mods` skips them). It needs no
`build/`, `generated/` or `ref/` directory beforehand and no files from any other machine. Game-derived outputs stay under `build/` (git ignores it), downloaded dependencies also use `ref/`,
and nothing is uploaded.

## What you need

- A Mac with Apple silicon and Xcode (verified with Xcode 26.6, iOS SDK 26.5, macOS 26.6). Run
  `xcode-select -s /Applications/Xcode.app` once if the command-line tools point elsewhere.
- CMake 3.25 or newer and Ninja: `brew install cmake ninja` (verified with CMake 3.27.1, Ninja
  1.13.2). Python 3, git and curl come with macOS and Xcode; the mods' Python packages
  (PyYAML, Pillow) are installed into `build/python` when missing.
- Network access to GitHub on the first run, and at least 25 GB of free disk space.
- The disc image, GZLE01 USA revision 0. Other revisions and regions are refused.
- To install: an Apple ID in Xcode, the iPad or iPhone (A13 or newer, iOS/iPadOS 17 or newer)
  with Developer Mode on, and a USB cable or the same network.

## What the script does

| Step | What happens | Time (M2 MacBook Air) |
| --- | --- | --- |
| 1 dependencies | Fetches the pinned RecompCore fork (GXRuntime, the vendored Aurora renderer, the Dolphin DSP sources) into `ref/recompcore`, its DolRecomp submodule (the translator), and the pinned Dawn iOS package (checksum checked) into `build/deps` | under a minute |
| 2 extract | Builds the app's own disc importer (`apple/ios/src/disc_import.c`) for the Mac and extracts `main.dol` and the 415 RELs | seconds |
| 3 translate | Builds DolRecomp and translates the DOL and all 415 RELs to C | under a minute |
| 4 composite | `scripts/generate_composite.py` merges them into one tree (748 chunks, 415 REL modules) and the script checks its digest against the verified tree | under a minute |
| 5 compile | Compiles that tree for arm64 iOS (`-mcpu=apple-a13`, iOS 17.0) into `gGZLE01_recomp.dylib` | about 4 hours on an idle Air (11.4 hours on 2026-09-25 with other apps loading it); far less on an M3 Max |
| 6 app | Builds BlueWake.app (SDL is downloaded by CMake), embeds the composite in `Frameworks`, signs it | 6 minutes |

`--source-only` stops after step 4, so you can check the tools, the dependencies and your disc in a
couple of minutes before the long compile. The compile resumes where it stopped if interrupted: rerun
the same command. The log of every step is in `build/device/logs`.

The result is `build/device/app/BlueWake.app`. Without signing options it is signed ad hoc, which
proves the build but will not install on a device.

## Signing and installing on the iPad

1. **A signing certificate.** In Xcode, Settings > Accounts, add your Apple ID. Select the team and
   click Manage Certificates > + > Apple Development. Check it:
   ```bash
   security find-identity -v -p codesigning
   # 1) 0123ABCD... "Apple Development: Your Name (TEAMID1234)"
   ```
   A free Personal Team works; its apps expire after seven days (rerun step 5 below to reinstall).
2. **Developer Mode.** Connect the iPad to the Mac, unlock it and trust the computer. Open Xcode's
   Window > Devices and Simulators once so the iPad is prepared. On the iPad, Settings > Privacy &
   Security > Developer Mode, turn it on and restart.
3. **A provisioning profile for `dev.bluewake.BlueWake`.** The simplest way: in Xcode, File > New >
   Project > iOS App, set the Bundle Identifier to `dev.bluewake.BlueWake` and your Team, choose the
   connected iPad as the run destination and press Run once. Xcode registers the iPad and makes
   the profile ("iOS Team Provisioning Profile: dev.bluewake.BlueWake"). Find its file:
   ```bash
   for f in ~/Library/Developer/Xcode/UserData/Provisioning\ Profiles/*.mobileprovision \
            ~/Library/MobileDevice/Provisioning\ Profiles/*.mobileprovision; do
     [ -f "$f" ] && security cms -D -i "$f" 2>/dev/null | grep -q 'dev.bluewake.BlueWake' && echo "$f"
   done
   ```
   With a paid account you can instead register the App ID and the iPad in the developer portal
   and download an iOS App Development profile. If BlueWake is already installed, do not run a
   placeholder app over it or delete it: back up its saves and obtain the provisioning profile without
   replacing the installed app.
4. **The device id.**
   ```bash
   xcrun devicectl list devices      # the Identifier column, or the device name
   ```
5. **Build, sign and install** (after a first full build, only the signing and install run again;
   steps 1-4 take a minute and the compile is reused):
   ```bash
   scripts/ios/build_device.sh "/path/to/The Legend Of Zelda The Wind Waker.iso" \
       --identity "Apple Development: Your Name (TEAMID1234)" \
       --profile "/path/to/profile.mobileprovision" \
       --install <device identifier>
   ```
   The script embeds the profile, signs the composite and the app with the profile's
   entitlements, verifies the signature and installs with `xcrun devicectl`. It refuses a profile
   made for another bundle id. With a free team, the first launch may say "Untrusted Developer":
   Settings > General > VPN & Device Management, trust your Apple ID, then launch again.
6. **The disc on the iPad.** The app contains translated game code, but needs your disc for assets. Copy the same disc image to the iPad:
   in Finder, select the iPad, open the Files tab and drag the ISO onto BlueWake (or use local **On My iPad** storage in the Files app). On first launch BlueWake shows what is missing,
   imports the disc through the document picker (a file named `GZLE01.iso` in BlueWake's folder is
   picked up too), checks that it is GZLE01 USA revision 0 and prepares `main.dol` and `rels/` on the
   device. Saves stay in the app's container.

What to check on the iPad is listed in [IPAD_STATE_2026-09-24.md](IPAD_STATE_2026-09-24.md).

## Optimization and performance

See [the Builder's current optimization status](../BUILDER.md#optimization-profiles) for the
measured difference between baseline and developer builds. Runtime and host profiles are bundled;
the developer's translated-game profile is private and is not an input players should obtain.
The builder generates the game profile on each player's Mac from their own disc by default
(`--no-train` skips it); `--training-save FILE` optionally supplies a copy of the player's own
BlueWake card. See [the player guide](../BUILD_YOUR_OWN.md#local-optimization). iPad performance of
locally trained builds is still being measured.

The advanced `--composite-pgo FILE` (repeatable) and `--host-pgo FILE` options accept profiles you
created locally; use a separate `--out build/device-pgo` directory when changing compiler flags.
Older research scripts refer to private development saves and build directories and are not the
player onboarding path.

## Where the sources come from

| Component | Source | Pinned |
| --- | --- | --- |
| RecompCore (GXRuntime, Aurora, DSP) | https://github.com/elliotttate/RecompCore, branch `windows-release` (chrissotraidis/RecompCore 2d60636 plus patches/recompcore 0098-0129 and the save states' two) | `44e5c2c2451f713c9e21ed9a4a5f560f3d057ff4` |
| DolRecomp (translator) | https://github.com/elliotttate/DolRecomp, branch `bluewake` (RecompCore's `DolRecomp` submodule; chrissotraidis/DolRecomp 5c91d6e plus patches/dolrecomp/0019) | `b8b534591cba8ca7cd43943a655ee6e2591cf5de` |
| Aurora | vendored in RecompCore at `GXRuntime/graphics/aurora` (plain files) | with RecompCore |
| Dawn (WebGPU) for iOS | https://github.com/encounter/dawn/releases v20260618.032059, `dawn-ios-arm64.tar.gz` | sha256 `ada0bafc...a7ae2` |
| SDL 3.4.10, fmt, xxhash and the rest | fetched by Aurora's CMake at configure time | Aurora's pins |

The RecompCore fork is the upstream base `5c3611e` (ExpansionPak/RecompCore) plus the 76 BlueWake
commits that were only on the development Mac, including the former local head `3476998`, plus the
files that were never committed there (`GXRuntime/include/core/cpu.h`, `GXRuntime/src/core/cpu.c`,
`Source/Core/Core/DSP/Interpreter/DSPIntTables.cpp` and `.h`). The DolRecomp fork is the translator
exactly as it produced the shipped composite; a fresh build of it is byte-identical to the one used.

`patches/recompcore` and `patches/dolrecomp` are history. The RecompCore series starts at 0008
(0001-0007 were never exported), so it cannot apply to the base; the fork commit replaces it.

## How the composite is generated

These are the commands the script runs (step 3 and 4), recovered on 2026-09-25:

```bash
dolrecomp --gamecube --backend c --cpu gekko --partition-instructions 4096 main.dol OUT_DOL -j8
dolrecomp --gamecube --backend c --cpu gekko --rel-base 0xC0400000 rels/ OUT_RELS -j8
python3 scripts/generate_composite.py --dol-dir OUT_DOL/generated \
    --rels-dir OUT_RELS/generated/rels --rels-bin-dir rels/ --main-dol main.dol --output-dir COMPOSITE
```

The REL namespace failure recorded earlier came from a stale REL translation made with the old
low aperture (0x80400000); the RELs must be translated with `--rel-base 0xC0400000`, and
`--rels-dir` must name `generated/rels` (naming `generated` finds no RELs). The result passes the
generator's checks (748 chunks, 415 REL modules, 417 code ranges). Its five metadata files and all
206 DOL chunks are identical to the composite shipped to the simulator. The 542 REL chunk files
differ from that tree, which had been assembled by hand from two translator versions (its DOL chunks
regenerated after DolRecomp patch 0018, its REL chunks still from before it); the fresh tree has
every file from the same translator. The runtime checks below were run on the fresh tree.

The script pins the digest of the generated base tree
(`python3 scripts/ios/composite_manifest.py DIR`, 754 files,
`54f54434c3f9c899d43a96373dc0b4c1aed0e50db8b820b9698dfa76571a770a`) and stops if a disc or
translator produces anything else; `--accept-new-composite` overrides that for development.

## Historical verification (2026-09-25)

The measurements below describe the specified older commits and machines, not the current
Builder's complete local-training workflow. Current build comparisons are in [BUILDER.md](../BUILDER.md).

On 2026-09-25, on an M2 MacBook Air (Xcode 26.6, CMake 3.27.1), from fresh clones of this
repository with no `ref/`, `build/` or `generated/`:

- **Sources.** Steps 1-4 fetched RecompCore 086f282 and DolRecomp 5c91d6e from GitHub and Dawn from
  its release (checksum matched), and the source check passed in 1 minute 40 seconds: 206 DOL
  chunks, 415 RELs, generator PASS (748 chunks, 415 REL modules, 417 code ranges), composite digest
  `9e4a847d...` equal to the pinned one. It was reproduced in a second fresh clone.
- **Extraction and translation.** The extractor's `main.dol` and 415 RELs are byte-identical to the
  files the development Mac used; the DolRecomp built from the fork is byte-identical to the one
  that made the shipped composite.
- **The app's host code** builds with the StaticRecomp ABI header from RecompCore (no ModernGekko):
  the macOS host and its 218 tests pass (218/218) and the simulator app builds.
- **The optional profiles** apply to the fresh tree: the composite runtime files compile for iOS
  against the merged composite profile with no out-of-date or unprofiled-function warnings.
- **Device code on the Mac.** The composite is plain C linked only against libSystem, so the device
  dylib retagged to macOS (`scripts/ios/retag_macho_platform.py`) runs in the macOS host. The device
  composite of 2026-09-24 retagged this way reproduces the certified route digest `83d2590d...` over
  1,050 records. The same check is used on the clean build below.

**The complete clean build.** One run of `scripts/ios/build_device.sh DISC.iso --jobs 7` in a fresh
clone of commit c9f064b (the build inputs of e68f3c9 and later are the same; only documentation
changed) finished with exit status 0: steps 1-4 in under a minute, the composite compile in 684
minutes (this Air shared its cores with other applications all day; the 2026-09-24 device composite
took about 4 hours on the idle machine), the app in 6 minutes. `codesign -v --strict --deep` passes;
the app and the composite are platform IOS, minos 17.0; the app is 426 MB; the build directory took
3.4 GB. The composite's sha256 was `5bcfffee...74e1` (not pinned: it depends on the Xcode version).
Nothing in it refers to local paths (the only `/Users/` strings are Dawn's CI source paths).

**The clean composite at run time,** with the device code itself (no profiles):

- `scripts/verify_abi_coverage.py` on it retagged to macOS: PASS (417 code ranges, 748 chunks,
  415 REL modules, 8,079 REL sections).
- The certified headless route on the macOS host: digest `83d2590d...` over 1,050 records, the
  same as the tested build, stopping at the same blocks and pcs (10,557,120 at 0x80307ef4 and
  11,174,010 at 0x8027fa30). 232.7 M instructions a retrace against 218.4 M for the profiled
  2026-09-24 device composite.
- `scripts/ios/sim_save_acceptance.sh` on the iPad simulator with this composite (retagged for the
  simulator, code byte-identical): PASS. Opening complete at retrace 13,861 and play at 13,921
  (certified 13,850 and 13,910), pause-menu save written, the guest's own reset, stereo audio
  captured, reload into control at retrace 831.
- Speed without the profiles, the simulator's heavy Outset view back to back: median retrace time
  55.7 ms against 45.6 ms for the profiled 2026-09-24 device composite, about 20 percent slower. The
  absolute rates that afternoon (12-14 retraces a second for both, and 13.3 for the macOS composite
  that measured 51-54 on the idle Mac) were set by the load on the Mac, not by the build.

**Not verified here:** installing and running on a physical iPad (this Mac has no signing identity)
and the `--identity`, `--profile` and `--install` path, which only runs with them.
