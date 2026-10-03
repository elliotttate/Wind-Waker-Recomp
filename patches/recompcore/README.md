# Historical RecompCore patches

These patches record BlueWake's RecompCore changes as they were made. They are history, not a build
input: the series starts at 0008 (0001-0007 were never exported), so it does not apply to the
upstream base 5c3611e, and the local head it led to (3476998) was never published.

The build uses a fork instead. BlueWake's is https://github.com/chrissotraidis/RecompCore, branch
`bluewake`, commit 2d6063614a9bc899f6b4d11c7e7b3cd66e4d96f3: it contains the changes here through 0097
(some were revised by later ones), the files that were never committed on the development Mac, and the
DolRecomp submodule pointing at https://github.com/chrissotraidis/DolRecomp (5c91d6e). Wind Waker Recomp
builds from its own copy, https://github.com/elliotttate/RecompCore, commit 44e5c2c (branch
`windows-release`): 8ab24da (branch `bluewake`) plus 0117, the render worker paused while the
swapchain changes (a fullscreen crash), 0118, guest MEM1 through a global array where the module
provides one, 0119, up to 7 in-between frames and none while the game runs slow, 0120, the
dual-texture post transform (the lava's colour), 0121, Dawn's device lock (a crash after a save state
loads), 0122, the game's speed from the median of its last 60 frame gaps, 0123, pipelines compiled on
several threads, 0124, a draw's attribute arrays found once, 0125, a cloth's strips blended vertex by
vertex, 0126, colours blended in the in-between frames, 0127, a draw's vertex constants kept when its
transform state is the draw before's, and 0128-0129, the ubershader for draws whose pipelines are still
compiling (on by default on D3D12). 8ab24da is branch `bluewake`'s 6892947 (that tree plus 0098
to 0110), the Windows port's 0111 to 0116
(the shader and pipeline caches where the host says, gather-pipe writes as a run of bytes, the GX stall
watchdog on Mac and Linux only, constant blocks compared against a copy where staging is upload memory,
the GX worker's draws cheaper for a slower CPU, the graphics threads, slow presents and the CPU in the
log), and the Mac line's save states and its merge of that Windows work (numbered 0111 and 0112 there,
before the two lines met: the files named 0111-save-states-... and 0112-merge-windows-release-...),
with DolRecomp at https://github.com/elliotttate/DolRecomp
(b8b5345, 5c91d6e plus patches/dolrecomp/0019). The Builder fetches it at the commit pinned in
`scripts/builder/profiles/bluewake.sh`; see docs/status/DEVICE_BUILD.md.
