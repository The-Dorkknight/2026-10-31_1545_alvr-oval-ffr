# Performance ideas: what was researched, what was built

Written 2026-11-01 for v0.3.0, updated for v0.4.0. Question asked: could OvalFFR do **peripheral frame
generation**, **upscale the low-res periphery**, or use other cheap tricks to get more fps? Each idea
below has a verdict and the reason. Two were built: Auto foveation (v0.3.0) and Smooth low-res edge (v0.4.0).

| # | Idea | Verdict | Why |
|---|---|---|---|
| 1 | **Peripheral frame-rate reduction** (render the edges less often, reuse old pixels) | **Not possible from a layer** | The research needs the *game* to skip drawing those regions on alternate frames. An OpenXR layer only sees finished frames. See below |
| 2 | **Whole-frame frame generation / reprojection** | **Out of scope** | SteamVR Motion Smoothing and ALVR already do this in the runtime; a layer cannot ask a runtime to do it better |
| 3 | **Upscale / smooth the low-res periphery** | **Built in v0.4.0, off by default** | First judged "quality only" (v0.3.0). Matthew's point: a smooth edge lets you render *more* of the view coarse at the same look, so the saving comes from a smaller sharp area. See section 3 |
| 4 | **Render-resolution scale** | **Not built** | ALVR (and OpenXR Toolkit) already have it; no gain from duplicating |
| 5 | **Content-adaptive shading rate** (analyse the last frame, shade flat areas coarser) | **Not built, maybe later** | Microsoft's own example gained 14%, but it needs a GPU analysis pass; cannot be tested without a GPU. Candidate for a future version |
| 6 | **Auto foveation** (shrink the sharp area only while frames are being missed) | **Built in v0.3.0** | Same idea as Meta's dynamic fixed foveation; pure timing logic that can be tested here without a GPU |

## 1. Peripheral frame-rate reduction, in detail

The study "Evaluating Foveated Frame Rate Reduction in Virtual Reality for Head-Mounted Displays"
(arXiv 2505.03682) renders the centre every frame and updates concentric outer regions every 2nd to 5th
frame, reusing stale pixels, with **no** reprojection.

* 15 participants, HTC VIVE Pro Eye (**eye-tracked**), 90 Hz, stimuli of only 10-15 seconds.
* Estimated pixel savings (pixel counts, **not measured frame times**): 42% (config 11112), 64% (11223), 78% (12345).
* Visible tearing at region boundaries, worse with moving objects and head motion. Fewer than half the
  participants called 12222 / 11223 / 11233 "no or barely noticeable".

Why it does not transfer to OvalFFR: those savings come from the game *not drawing* the periphery on
skipped frames. OvalFFR sits between the game and the runtime and only receives the finished image.
The nearest thing a layer could do, reusing the whole previous frame, is what the runtime's motion
smoothing already does. A second paper, "Time-Warped Foveated Rendering for Virtual Reality Headsets",
looks closely related, but its page could not be fetched, so **I only know its title**.

## 3. Smooth low-res edge (built in v0.4.0)

**The idea (Matthew's).** Render the periphery cheaply and spend a little GPU time making it look smooth.
If the edge is indistinguishable when seen out of the corner of your eye, the sharp area can be smaller
or the edge can use 4x4 instead of 2x2, and *that* saves the time. Example with his numbers: save 45%,
spend 10% on the upscale, keep about 30% overall.

**What a layer can and cannot do.** A layer cannot make the game draw a smaller image: it only sees
the game's finished frames. What it already controls is Variable Rate Shading, which is a low-res render
of the periphery in all but name: one shading result per 2x2 or 4x4 block, copied to every pixel of the
block (a free nearest-neighbour upscale). Outlines stay full resolution, because VRS still decides per
pixel which object it belongs to. So the "upscale" is a pass that turns those copied blocks into a bilinear upscale.

**How it works** (`layer/shaders/smooth.hlsl`, `layer/src/backend_d3d12.cpp`):

* Hooks `xrAcquireSwapchainImage` (which image the game draws into) and `xrReleaseSwapchainImage`. On
  release the game's drawing is already queued, so the pass is queued behind it on the game's own
  D3D12 queue, before the runtime reads the image.
* Copies the eye image, then draws one full-screen triangle back into it. The pixel shader works out
  the shading rate of the pixel with exactly the same rule as the mask (tested to agree on 120,000 sample pixels).
* **Discards** (does not write) every pixel in the sharp area, and every pixel whose block is not an exact copy
  (all four corners must match it). Real detail is therefore never blurred, and a game that did not draw coarse into this image is left alone.
* Blends each remaining pixel towards the neighbouring blocks by its distance to them (bilinear), but
  ignores a neighbour whose colour differs by more than half, per colour channel (edge-preserving).
* Shader compiled at run time by `d3dcompiler_47.dll` (part of Windows 10/11); root signature embedded in
  the shader. Its own render-target views and binding go straight to the original D3D12 functions, so
  OvalFFR's own VRS hook cannot make the smoothing pass itself coarse.
* Never waits for the GPU: at most three passes are in flight; if the oldest is not done, that frame
  is not smoothed. Every failure switches off the smoothing only, with the reason in the panel and `layer.log`.

**Numbers (simulation on synthetic images, `tests/smooth_test.cpp`).** VRS is modelled as it really works:
coverage per pixel, colour once per block per object.

| Check | Result |
|---|---|
| Difference to the full-detail picture in the low-res zones, soft wavy scene | 0.0189 -> 0.0022 (**88 % lower**), strength 100 % |
| Same, ball in front of a wall | 0.0043 -> 0.0005 (88 % lower) |
| Blocks an outline runs through | 0 pixels changed |
| Outlines along a block border | moved at most 0.005 |
| Full-detail input (random noise / smooth scene) | 0 / 27 of 307,200 pixels changed |
| Sharp area | bit-for-bit unchanged |

Two flaws were found by these tests and fixed before release. A block's corner pixel passed the "exact
copy" test by matching itself, which changed 26,432 pixels of a noise image. And comparing brightness only blurred an
orange-on-blue outline by 0.27; colour is now compared per channel.

**The maths on cost.** Matthew's 10% for the upscale is probably pessimistic: the pass is a copy plus a
few texture reads per low-res pixel. My estimate is roughly 150 MB of memory traffic per frame for two
2000x2000 eyes, which I'd put at roughly 0.3-0.5 ms on a mid-range card (a few percent of an 11 ms frame). **Not
measured.** The 45% saving is the shading estimate; Microsoft and Meta report 14-25% real gains
from VRS, so the net gain depends on how much smaller the sharp area can get before you notice.

**Limits.**

* Only helps where the game draws coarse straight into the image it hands to OpenXR. Many games draw into their own
  buffer and post-process first; then the blocks are not exact copies and the pass does nothing.
  The same applies to the VRS saving itself (see the README's known limits).
* Film grain, dithering and temporal anti-aliasing break the exact-copy test, so nothing is smoothed.
* Shimmer from coarse shading (thin lines crawling as you move) is not fixed: smoothing cannot recover detail that was never shaded.
* The shader passes glslang's HLSL front end, **not** Microsoft's compiler; if Microsoft's compiler rejects it,
  the panel says so and nothing else changes.

## 5. Content-adaptive shading rate

Microsoft's VRS write-up reports, for one game at 4K on an RTX 2060 (~53 fps baseline): about +20% with
per-draw (tier 1) rates and +14% with a screen-space rate image (tier 2), with edge-detection-driven
images that were "almost impossible" to tell apart from VRS off. It gives no cost for building the rate
image each frame. Making that image needs a compute pass that reads the previous frame; I would not ship
that without being able to run it.

## 6. Auto foveation (built)

**What it does.** Your sliders become the *sharpest* the oval will ever be. While the game keeps missing
display refreshes, the layer shrinks the sharp area in 5% steps down to a floor you choose (default
60% of your sliders). When frames have been clean for a long time it grows back, slowly.

**How it decides.** The time between two `xrWaitFrame` returns is the game's real frame interval.
Compared with the runtime's display period:

* trigger: at least 25% of the last 30 frames more than 1.35x the period, **or** the typical (median) frame over 30
  frames at least 10% over the period (so it tolerates up to 10% overrun on purpose);
* frames more than 8x the period (loading, alt-tab, pause) and the first 120 frames are ignored;
* after each change it waits three windows before judging again (Meta's guidance: avoid frequent level
  changes, the jumps are noticeable);
* **no-gain protection:** if it has shrunk all the way to the floor and the interval (averaged over three
  windows, so one jittery window proves nothing) is not at least 5% shorter than before, the GPU is not the bottleneck (CPU-bound, Wi-Fi, ...). It returns to full size and
  stops trying for a while; each repeat waits twice as long. The verdict uses the median frame and three windows, and while it stays at the floor it is repeated every 20 windows, so one noisy verdict cannot stick. Judged only at the floor, because a runtime
  that halves the frame rate on every miss shows no improvement at all until the GPU finally keeps up;
* **oscillation protection:** if growing back is quickly followed by shrinking again, it waits twice as
  long next time and will not grow past the scale that failed (for about 8 minutes at 90 Hz).

**What was measured (simulation, not hardware).** `tests/core_test.cpp` runs the controller closed-loop
against modelled games:

| Modelled game | Result |
|---|---|
| Healthy | never changes anything |
| GPU-bound, smooth timings | settles near on time, 3 changes in 20,000 frames |
| GPU-bound, runtime halves rate on every miss | finds the answer (0.70), 8 changes |
| CPU-bound (shrinking changes nothing) | restores full size and locks; **7.7% of 40,000 frames** spent below full size (about 34 seconds out of 7.4 minutes at 90 Hz) |
| Load switching on and off every 10 s | 42 changes in 36,000 frames |
| CPU-bound with heavy spiky timing noise | 8.4% of 60,000 frames below full size |
| Loading hitches, NaN / negative / zero timings | ignored |

`tests/layer_test.cpp` also runs the real layer library against the fake runtime with a slow or fast
`xrWaitFrame`: a GPU-bound fake game ends with a smaller sharp area, a CPU-bound one ends "locked" at full
size.

**What is not known.** Whether SteamVR's or ALVR's `xrWaitFrame` timing shows missed frames the way the
model assumes; whether the saving is worth the visible shrink in real games; how it interacts with a
game's own dynamic resolution. **No fps number has been measured.** It is off by default and labelled
experimental.

## Sources

* [Evaluating Foveated Frame Rate Reduction in VR (arXiv 2505.03682)](https://arxiv.org/html/2505.03682v1)
* [Time-Warped Foveated Rendering for VR Headsets (Wiley; page not readable from here, title only)](https://onlinelibrary.wiley.com/doi/full/10.1111/cgf.14176)
* [Microsoft DirectX blog: Variable Rate Shading, a scalpel in a world of sledgehammers](https://devblogs.microsoft.com/directx/variable-rate-shading-a-scalpel-in-a-world-of-sledgehammers/)
* [Meta: fixed foveated rendering (levels, dynamic cap, guidance)](https://developers.meta.com/horizon/documentation/native/android/os-fixed-foveated-rendering/)
* [OpenXR Toolkit repository (feature list only; no numbers on that page)](https://github.com/mbucchia/OpenXR-Toolkit)
