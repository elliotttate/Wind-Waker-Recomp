# Wind Waker Recomp for macOS

The Apple Silicon release of Wind Waker Recomp: *The Legend of Zelda: The Wind Waker* statically
recompiled from its GameCube code to run natively on the Mac, with Metal rendering, Smooth Motion
(60 or 120 frames a second from the game's 30), widescreen, controller support and Better Wind Waker's
settings.

## Requirements

- An Apple Silicon Mac (M1 or newer). Intel Macs are not supported.
- macOS 15 or later. The host and its bundled libraries are built for macOS 15; testing was done on
  an M3 Max.
- Your own disc image of **The Legend of Zelda: The Wind Waker, USA (GZLE01), revision 0**: a plain
  `.iso` or `.gcm`, or a Dolphin `.rvz`, `.gcz`, `.wia` or `.ciso`. Other regions and revisions are
  refused; the app checks `main.dol` (SHA-1 `8d28bab68bb5078c38e43f29206f0bd01f7e7a67`).

The download contains the recompiled game code and the runtime. It contains no disc image, game files,
textures, audio, saves or settings: the game runs only with your own legally obtained disc.

## Install and play

1. Download `WindWakerRecomp-<version>-macos-arm64.zip` from the Releases page and unzip it.
2. Drag **Wind Waker Recomp.app** to Applications and open it.
3. The app is ad hoc signed, not notarized. If macOS blocks the first launch, open
   **System Settings → Privacy & Security** and choose **Open Anyway** for Wind Waker Recomp, if you
   trust the download ([Apple's guidance](https://support.apple.com/en-us/102445)).
4. Choose your disc image when asked. The app checks it and prepares the game's executable and modules
   from it (a few seconds). A compressed image (`.rvz` and the like) is first unpacked once into a plain
   `GZLE01.iso` (1.4 GB) in the app's data folder, which the game then reads; a plain image stays where
   it is and is read in place, so choose it again if you move it.

The first launches compile rendering pipelines as new scenes appear, so the first minutes can hitch;
later launches reuse them.

## Controls

A game controller works as a GameCube pad. On the keyboard:

| GameCube | Key |
| --- | --- |
| Control stick | W A S D |
| C-stick (camera) | T F G H |
| A / B / X / Y | J / K / U / I |
| L / R / Z | E / R / Q |
| Start | Return |
| D-pad | arrow keys |

On a controller the right stick turns the camera directly, with no easing, and aims in first person and
with items; clicking it is first person (and back out). In the telescope and the Picto Box the left
stick zooms. The left bumper jumps and a click of the left stick sprints. The game's own right stick
(the GameCube C-stick) is an option under Controls.

Click the game to turn the camera with the mouse (left click is A, the wheel zooms); Esc gives the
mouse back. **Esc** (with the mouse free), **F1** or a controller's Back button opens the options:
aspect ratio, fullscreen, render resolution, Smooth Motion (Off, 60 or 120), texture filtering,
HD texture packs, Better Wind Waker's settings and controller vibration (Controls: Enhanced, Classic or Off,
its strength, and trigger feedback).

**Controller vibration.** The game's own vibration (hits, falls, explosions, bosses, quakes) is felt as the
game timed it, but shaped by its strength on both motors instead of the GameCube's plain on and off; strong
hits also kick an Xbox controller's impulse triggers or a DualSense's triggers (the DualSense over USB or
Bluetooth through SDL's own driver). Nothing is felt while the options are open or the game is in the
background. The game's own Vibration option still turns it off.

## Climbing

**Climb any wall** (Gameplay options, off by default) lets Link climb steep walls the way he climbs ivy,
as in *Breath of the Wild*: walk or jump into a wall to grab it. A stamina wheel beside him drains while
he climbs (more slowly while he holds still); when it runs out he lets go, and he can climb again once
it has refilled on the ground. Ivy, ladders, ledges he pulls himself onto and walls he sidles along work
as before, and ivy costs no stamina. The wheel's length is set under **Climbing stamina** (12 seconds by
default).

## Save states

Like an emulator's, a save state keeps the whole running game, to come back to that exact moment:
**F5** saves one and **F9** loads the latest (on a MacBook keyboard, hold **fn**), or use **Save state**
and **Load latest state** in the options. They are kept in the data folder's `states/` (about 20 MB
each) and work across launches of the same version; a state from another version of the app may be
refused. They are meant for trying things again and for reporting problems; keep saving in the game
as well.

## Where your data is

`~/Library/Application Support/Wind Waker Recomp` holds the memory card (`GZLE01.card`), settings
(`settings.ini`), save states (`states/`), the compiled shader caches, the files prepared from your disc (`game/`), where your disc is (`disc.txt`) and a log
per session (`logs/`), and the unpacked `GZLE01.iso` if you chose a compressed image. Replacing the
app keeps them. To use another disc image, delete `disc.txt`.

HD texture packs in Dolphin's format for GZLE01 can be chosen in the options.

## Known issues

- Lava in Dragon Roost Cavern's areas renders as flat orange instead of its bright pattern.
- The busiest scenes can dip below 120 frames a second at 120 Hz Smooth Motion; 60 is steadier.
- Switching away from the full-screen game can hitch briefly as macOS slides it out.

## Source

`Contents/Resources/BUILD.json` records the source revisions (Wind Waker Recomp and RecompCore), the
checksums of the host, the game module and the launcher, and the pinned libraries. The host is built
by `scripts/mac/build_release_deps.sh` and a macOS 15 build of `scripts/builder/training`, and the app
by `scripts/mac/package_release.sh`. Licenses and notices are in `Contents/Resources/licenses`.

Wind Waker Recomp is not affiliated with or endorsed by Nintendo. *The Legend of Zelda: The Wind Waker*
is Nintendo's.
