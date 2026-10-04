# Water interpolation checkpoint check, 2026-10-02

The swimming checkpoint beside the King of Red Lions exposed cracks and
flickering in the ocean's 64 indexed wave strips. The interpolation matcher
estimated a separate rigid transform from three vertices in each strip. The
waves deform rather than move rigidly, so adjacent strips were pulled apart.

RecompCore patch `0113` records and interpolates all positions in bounded,
indexed draws without a position-matrix index. Matrix-skinned draws retain
their bone interpolation. The matcher uses position indices and material,
so animated direct UVs and freshly allocated position buffers do not give the
same strip a different key. These draws keep independent vertex ranges and
their original depth/color submission order on the renderer's helper thread.
Tagged-particle lifetime checks continue to apply only to tagged particles.

The follow-up fixes surface-pattern stepping during movement. The indexed
ocean grid also regenerates its direct UVs as it follows Link. Previously,
blended positions used the next game frame's UVs, so the texture could still
step at 30 Hz while the wave geometry moved smoothly. The renderer now keeps
and blends the corresponding direct UVs at every intermediate step, including
texture-only animation on an otherwise stationary mesh. Unchanged UVs avoid
extra blending. Large UV jumps (atlas switches/wraps), tagged particles and
matrix-skinned geometry retain their current attributes.

The reproducible input is RecompCore
`8ab24daee9c641634fda5cac30389ad4b2cfda5e` plus
`patches/recompcore/0113-interpolate-indexed-cpu-deformed-vertices.patch`.
The current `config/recompcore-patches.json` selects patch `0170`, combining
this delta with the Windows lava post-transform fix (`0120`) and smooth HUD matching (`0160`), and records its
checksum. Bootstrap, the builder, local PGO training and desktop/iOS CMake
apply the same verified delta. The
helper accepts a clean checkout or that exact delta and refuses other tracked
edits. Local PGO fingerprints include the patch manifest.

Validation used the native macOS training wrapper, Release/arm64, against
the desktop source at `e1084379638c4933fcf867a5300f0d77c2363de8` plus these
local changes. The translated game module was reused from the personal play
app. No translation or texture changes are required for this fix.

- `bluewake_frame_interp_test` passes, including adjacent deforming strips
  at large world coordinates, three intermediate frames, moving camera and
  mesh, changing direct UVs, texture-only animation, and UV wrap rejection.
  Tests check that UVs describe the same intermediate world point as the
  wave vertex, for all three steps and multiple UV channels. Existing camera, CPU skinning and particle
  checks also pass.
- Native, bounded runs reload the same private checkpoint every time and use
  copied memory cards. Comparisons covered interpolation on/off and the HD
  pack enabled/disabled.
- Final traces at frames 15 and 16 match all 64 wave strips and report vertex
  interpolation for each. All strips use the same interpolated camera matrix.
  Ordinary helper-thread captures, without draw tracing, retain the boat,
  tower and HUD and no longer show the interpolation cracks.
- Patch-helper tests cover read-only checks, clean application, repeat builds,
  staged exact deltas, and preservation/rejection of unrelated edits. The
  actual patch was also applied and checked twice on a separate clean clone.
- A short run without GPU frame dumps logged no FPS dips. This is a smoke
  check, not a long-route performance result or a verified refresh-rate claim.
- The UV follow-up was tested while swimming and while sailing from a newly
  saved checkpoint of the current play session. Traces at frames 15 and 16
  show position and UV interpolation on all 64 wave strips in both routes.
  Their UVs match the blended world coordinates within `1e-5`. Ordinary
  helper-thread captures also exercise all three intermediate frames and
  retain the boat, Link and HUD. The short sailing run without frame dumps
  logs no FPS dips. Private results are in `water-uv-evidence.json`.
- The before/after sailing captures with interpolation disabled are pixel
  identical. The UV follow-up changes the intermediate rendering path.
- The updated personal macOS app was signed, launched and restored to the
  newly saved sailing checkpoint. The visible in-game counter showed 120 FPS
  with approximately 30 game updates per second during the final check.
  That observation does not replace a sustained performance measurement.

The broad texture-pattern line still visible with interpolation off is
separate. Diagnostic draw colors put both sides inside the nearby indexed
ocean mesh, rather than at its join with the flat distant ocean. Shared mesh
edges have identical positions and UVs. Ray/triangle checks locate the line
at a wave crest that occludes farther water, creating a sharp change in
texture scale. It also occurs with original textures and anisotropy reduced
to one. This patch fixes interpolation cracks; it does not smooth or replace
the game's coarse wave geometry. A reference-emulator visual comparison has
not been performed.

Private evidence stays under ignored `build/wwhd-playtest/`: the repro
harness, before/on/off captures, final `water-verified-on-hd` and
`water-verified-trace-hd` logs, and `water-verified-evidence.json`. Checkpoints,
cards, extracted art and texture packs are not part of this source change.
