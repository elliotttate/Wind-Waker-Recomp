# Rights and licenses

## BlueWake's code

BlueWake is licensed under the GNU General Public License, version 3 or (at your option) any later
version. The full text is in [LICENSE](LICENSE).

GPLv3 is the license the parts BlueWake is built from allow together:

- the compatibility runtime, [RecompCore](https://github.com/chrissotraidis/RecompCore), is derived from
  [Dolphin](https://dolphin-emu.org/) (GPLv2 or later), and RecompCore's `COPYING` states that the fork
  as a whole is compatible with GPLv3
- the touch control overlay is adapted from SunPad (GPL-3.0)
- the translator, [DolRecomp](https://github.com/chrissotraidis/DolRecomp), and the Aurora renderer
  vendored in RecompCore keep their own licenses, recorded in those repositories
- the recovered J3D rotation/translation formulas in `cmake/composite/native_j3d.c` are adapted from
  [zeldaret/tww](https://github.com/zeldaret/tww), revision `09de0609`,
  `src/JSystem/J3DGraphBase/J3DTransform.cpp`, released under
  [CC0 1.0 Universal](https://github.com/zeldaret/tww/blob/09de0609/LICENSE)
- the certified game math natives in `cmake/composite/native_game_math.c` (collision, clipping, animation
  and matrix functions) reproduce GZLE01's translated code, written with the same revision of
  [zeldaret/tww](https://github.com/zeldaret/tww) (CC0 1.0) as the guide to what each function does

Wind Waker Recomp is released as source and as a Mac app ([docs/MACOS_RELEASE.md](docs/MACOS_RELEASE.md))
that runs only with the player's own disc. iPad apps are built by their player, on their own Mac, from
their own disc ([docs/BUILD_YOUR_OWN.md](docs/BUILD_YOUR_OWN.md)).

## Game content

BlueWake is an independent, unofficial project, not affiliated with or endorsed by Nintendo. *The
Legend of Zelda: The Wind Waker*, its code, data, characters, names and imagery, and the GameCube
trademark remain the property of their owners. BlueWake cannot grant rights it does not hold.

This repository contains no disc image, playable game assets, saves or code translated from the game.
Documentation screenshots depict the game and are not covered by BlueWake's software license.

The Mac release app contains code recompiled from the game (`Frameworks/gGZLE01_recomp.dylib`), as the
maintainer's other recompilation projects do, but no disc image, game files, textures, audio or saves:
it runs only with your own legally obtained USA `GZLE01` revision 0 disc, which it checks, and reads the
game's data from that disc. An iPad app you build yourself contains code translated from your disc and
is for your own use. The software license does not grant any rights in game-derived code, and running
a GPL-covered translator does not by itself place its output under the GPL.

## Mods

Better Wind Waker, the widescreen code from Dolphin's game settings and HD texture packs are the work
of their authors and keep their own terms. The repository carries only the widescreen code's text
(`mods/widescreen/GZLE01.gecko`); the build fetches Better Wind Waker at a pinned commit and applies it
to your disc on your Mac, and you add texture packs yourself.

The optional Wind Waker HD texture importer (`scripts/import_wwhd_textures.py`)
extracts artwork only from the user's local disc. Its output, discs, tickets and
keys are personal data and must never be included in a public release. The
vendored Wii U surface address library is AboodXD's BFRES-Tool addrlib under
GPL-3.0-or-later; its license, copyright and pinned source are recorded in
`scripts/wwhd/vendor/`. Format reader attribution is in `docs/WWHD_TEXTURES.md`.

## Runtime metadata

`apple/ios/resources/initial_pipeline_cache.db` contains Aurora rendering-pipeline descriptions
recorded during testing: structural GPU state, configuration versions and usage order. It does not
contain textures, models, game executables or compiled shaders. Aurora builds shaders from these
descriptions using its own renderer. The cache reduces missing draws while first-use pipelines compile.

The bundled `composite-rt.profdata` and `host.profdata` contain LLVM profiling metadata for the
compatibility runtime and host. They contain function identifiers and execution counts, not machine
code. The translated-game optimization profile used by the developer is not distributed; player
builds must generate their own from their disc. See [Builder status](docs/BUILDER.md#optimization-profiles).
