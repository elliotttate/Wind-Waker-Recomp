# Mac HUD: smooth animation with stable sprite matching

At the Fire Mountain interior checkpoint (`MiniKaz`), 120-Hz Smooth Motion
could briefly draw a countdown digit between its timer slot and the rupee
counter on the right. Hearts could jump during their pulse/damage animation.

The matcher treated direct-position screen sprites as repeated instances of
one object, keyed by texture and occurrence. A reused glyph changed the draw
order: the original trace paired a timer digit at `(412, 412)` with the counter
and placed its first in-between at `(585.2, 435.2)`. Occurrence is also unstable
when the heart animation adds or removes sprite copies.

The initial `0140` fix excluded screen-space HUD draws from interpolation.
The refined `0160` fix restores smooth UI animation. Direct-position
orthographic draws with depth testing and writes disabled use artwork identity
(texture, atlas UVs and vertex RGB) and their transformed screen bounds. Fade
alpha, animated position/size and stale particle tags do not change that
identity. Each previous sprite can match only once, within 20 percent of the
smaller sprite dimension (at least two coordinate units). New digits and
large discontinuities keep their current frame; nearby motion, matrix pulses
and direct-vertex pulses interpolate. Perspective world motion, water,
particles and orthographic depth-buffer geometry retain their existing paths.

The reproducible Mac delta `0170`, selected and checksummed by
`config/recompcore-patches.json`, combines this refinement with water `0113`
and the Windows lava post-transform fix `0120` on the unchanged pinned
RecompCore base `8ab24da`. `0160` is the incremental change from `0150`; the
historical broad HUD exclusion is superseded.

Validation:

- The exact native host rebuilt successfully, and the interpolation regression
  test passed. Checks cover reordered shared glyphs, a newly returning digit,
  one-to-one matching, stale particle tags, atlas/tint identity, smooth heart
  matrix and direct-vertex pulses, invalid bounds, world particles,
  CPU-deformed water, camera turns and 120-Hz pacing.
- The runtime patch application test passed and the strict manifest check
  confirmed the complete dependency delta.
- Private copies of the player's checkpoint and card loaded for comparison;
  40 game frames and their three in-betweens were captured. The countdown stays
  in its slots through digit changes, the heart pulse remains anchored and
  changes across intermediate frames, and the world continues to interpolate.
- Traces of frames 215–217 contain 482 matched screen-sprite blends and three
  rejected new glyphs. The largest matched uniform translation to the current
  frame is 0.85 coordinate units. Frame 217's newly returning timer glyph at
  `(412, 412)` is correctly rejected instead of borrowing the rupee glyph.
- Both personal Mac apps were updated with the tested host. Every Mach-O
  section matches the build after signing, and the translated module, latest
  user checkpoint and settings retain their checksums. The installed
  `/Applications/Wind Waker Recomp.app` was relaunched into that checkpoint;
  the inventory and 120-FPS display counter were visibly verified.

Private captures, logs, copied saves and installation backups live under
`build/wwhd-playtest/hud-interpolation/`; none are release or repository assets.
