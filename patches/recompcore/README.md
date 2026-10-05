# Historical RecompCore patches

These patches record BlueWake's RecompCore changes as they were made. They are history, not a build
input: the series starts at 0008 (0001-0007 were never exported), so it does not apply to the
upstream base 5c3611e, and the local head it led to (3476998) was never published.

The build uses a fork instead. BlueWake's is https://github.com/chrissotraidis/RecompCore, branch
`bluewake`, commit 2d6063614a9bc899f6b4d11c7e7b3cd66e4d96f3: it contains the changes here through 0097
(some were revised by later ones), the files that were never committed on the development Mac, and the
DolRecomp submodule pointing at https://github.com/chrissotraidis/DolRecomp (5c91d6e). Wind Waker Recomp
builds from its own copy, https://github.com/elliotttate/RecompCore, commit 400728a (branch
`windows-release`): 8ab24da (branch `bluewake`) plus 0117, the render worker paused while the
swapchain changes (a fullscreen crash), 0118, guest MEM1 through a global array where the module
provides one, 0119, up to 7 in-between frames and none while the game runs slow, 0120, the
dual-texture post transform (the lava's colour), 0121, Dawn's device lock (a crash after a save state
loads), 0122, the game's speed from the median of its last 60 frame gaps, 0123, pipelines compiled on
several threads, 0124, a draw's attribute arrays found once, 0125, a cloth's strips blended vertex by
vertex, 0126, colours blended in the in-between frames, 0127, a draw's vertex constants kept when its
transform state is the draw before's, and 0128-0129, the ubershader for draws whose pipelines are still
compiling (on by default on D3D12), 0130, a draw's transform state copied only when its
version changed, and 0131, the Mac's water and HUD (0170 below without its lava part, which is 0120)
merged with 0125's cloth and 0126's colours, and 0132, an HD replacement's own mip levels sampled
(HD packs no longer shimmer while the camera turns), and 0133, only meshes whose positions the game
wrote blended vertex by vertex (0131's water path without its cost for static models), 0134, the present
log's per-frame data totals, and 0135, a display list's primitives fused into one draw (a tenth of the
draws through the GX worker and Smooth Motion's matching), 0136-0137, the GPU profiler without Tracy
(`DOL_AURORA_GPU_PROF=1`: each pass's GPU time and the bytes the frame uploads), and 0138, a compact
decoded-vertex layout for the draws that need no more (60 bytes, from 132), 0139, two texture identities
kept per guest address (a CI texture drawn under two palettes is no longer decoded and uploaded again at
every alternation), 0141, frame_interp_test's tex2 draws in the full layout, and 0142, the vertex block
as three uniform bindings each staged as far as its shader reads (uniform uploads 50-69 percent smaller),
0143, a fast path for the vertex decode (the GX worker 10-18 percent faster at the sea and in Hyrule), and
0144, render passes' command lists kept from frame to frame, and 0145, vertices the GPU buffer already holds
neither staged nor copied (81-95 percent of the vertex bytes at the sea and in Hyrule), and 0146, a gxcore draw's
constants as immediate data read from storage (two bind groups a draw no longer set: the render worker's encoding
14-30 percent faster). 8ab24da is branch `bluewake`'s 6892947 (that tree plus 0098
to 0110), the Windows port's 0111 to 0116
(the shader and pipeline caches where the host says, gather-pipe writes as a run of bytes, the GX stall
watchdog on Mac and Linux only, constant blocks compared against a copy where staging is upload memory,
the GX worker's draws cheaper for a slower CPU, the graphics threads, slow presents and the CPU in the
log), and the Mac line's save states and its merge of that Windows work (numbered 0111 and 0112 there,
before the two lines met: the files named 0111-save-states-... and 0112-merge-windows-release-...),
with DolRecomp at https://github.com/elliotttate/DolRecomp
(b8b5345, 5c91d6e plus patches/dolrecomp/0019). The Builder fetches it at the commit pinned in
`scripts/builder/profiles/bluewake.sh`; see docs/status/DEVICE_BUILD.md.

The Mac checkout also applies the exact delta recorded in
`config/recompcore-patches.json`. Patch 0170 combines the indexed, CPU-deformed
vertex interpolation fix from 0113, smooth screen-sprite matching from 0160
(superseding the broad HUD exclusion in 0140), and the Windows lava fix from 0120
(RecompCore `81d7345`, with the test's line endings corrected in `7c62903`).
The combined patch applies to the pinned `8ab24da` base. Bootstrap, desktop/iOS
CMake and the builder verify its checksum and reject unrelated dependency edits. The Windows line's
pin (400728a) already carries all of 0170, so its lock names no working-tree patch and the same
script only checks that ref/recompcore is that commit, unmodified.
