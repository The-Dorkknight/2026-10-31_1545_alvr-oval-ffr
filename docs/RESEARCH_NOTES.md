# Research notes: OvalFFR

Written 2026-10-31 while building v0.1.0; the dual-GPU section was added 2026-11-01 for v0.2.0. Everything here was read on that date; links at the end.
"Not verified" means I did not test it myself.

## The question

Make VR games run faster over ALVR by rendering the periphery at lower resolution, using an
oval/slot-shaped sharp region (the corners of the eye image are barely visible in a lens), with
horizontal and vertical size sliders and a picture of the low-res area.

## What ALVR already does, and why it is not enough

* ALVR has **foveated encoding**. Its own documentation says this differs from native foveated
  rendering: it compresses the outskirts of the frame *before transmission* and re-expands it on the
  headset after decoding. It saves Wi-Fi bandwidth and encoder work, **not game GPU work**.
* So "boost fps by reducing GPU rendering" has to happen on the PC, inside the game's rendering,
  before ALVR ever sees the frame.

## How other tools do it

* **OpenXR Toolkit** (Matthieu Bucchianeri and Jean-Luc Dupiot, MIT licence) is an OpenXR **API
  layer**: it sits between the game and the OpenXR runtime. It offers Fixed Foveated Rendering on
  DX11 and DX12. Each view is divided into regions (ellipses), each with its own shading rate, using
  the GPU's **Variable Rate Shading (VRS)**. Its author reported the fixed (non-eye-tracked) centre
  works "pretty ok" because few headsets have eye tracking. Not verified: I did not read or copy its
  source; OvalFFR is written from the public API documentation.
* **API layers** are a standard OpenXR loader feature: the loader can insert layers in front of the
  runtime so a third party can intercept calls. They work with any runtime, so this applies to
  ALVR's streaming path for OpenXR applications. Not verified in a real ALVR session.
* **VRS** lets the GPU run the pixel shader once for a block of pixels. With a *shading-rate image*
  the application supplies one rate per tile (NVIDIA's description: per 16x16 pixel region; D3D12
  reports the tile size and it can differ by GPU, so the code queries it).
* **Quest-style** OpenXR extensions (`XR_FB_foveation`, `XR_FB_foveation_vulkan`, Qualcomm fragment
  density map) exist for standalone headsets. They need runtime support and are a different route
  from a PC API layer, so they are not used here.
* **Half-Life: Alyx** got PC foveated rendering on 2026-10-26 (VR.org, 2026-10-27): a full-resolution
  centre, a middle band at a quarter of the shading, an outer ring at a sixteenth, a fixed mode for
  any headset without eye tracking, and a GPU list (NVIDIA GTX 16 / RTX 20 or newer; AMD RX 6000 and
  Intel Arc under Vulkan). That is the same three-zone idea OvalFFR uses (1x1 / 2x2 / 4x4), so the
  defaults are in line with what a shipping game does. The same article warns that a game with its
  own dynamic resolution may spend the saved time on a sharper image, so fps may not move: test with
  dynamic resolution pinned.
* Unity's documentation notes fixed foveation is easier to notice than eye-tracked foveation, since
  you can look at the periphery. That is why the sliders are adjustable and the default is gentle.

## Design decisions

| Decision | Why |
|---|---|
| OpenXR API layer, not an ALVR change | Works for any OpenXR game, independent of ALVR releases, no fork to maintain. |
| Variable Rate Shading with a shading-rate image | Real GPU savings, no shader changes in the game. |
| Superellipse `|x/a|^n + |y/b|^n = 1`, n from 2 (oval) to 4.5 (slot) | One formula gives both the oval and the slot shape asked for. |
| Three zones: 1x1 centre, 2x2 ring, 2x2 or 4x4 edge | Matches shipping practice; the ring hides the step. |
| Corners get the coarsest rate | The lens does not show them, so no visible cost. |
| Centre the oval on the **optical axis** from the runtime's FOV | Eye images are asymmetric; centring on the image centre would be off. |
| Config file + hot reload (400 ms poll) | The sliders work while the game runs, no restart. |
| Estimated saving shown in the UI | Mean of 1/(rate area) per tile. Shading only, so the real fps gain is smaller. |
| No GPU required to *run the app* | Project rule (7th gen Core i3): the layer simply stays a pass-through when VRS tier 2 is missing. |
| Shared core (`ovalffr_core`) with a Python mirror | The preview is the same maths as the layer; a test compares them. |

## Dual-GPU laptops (added for v0.2.0)

* **The runtime picks the card.** `xrGetD3D12GraphicsRequirementsKHR` returns the adapter LUID the
  application must use ([Khronos spec](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrGetD3D12GraphicsRequirementsKHR.html)).
  So with SteamVR/ALVR the lever is SteamVR's own process, not just the game.
* **Windows per-program preference:** Settings > System > Display > Graphics stores one value per program
  under `HKCU\Software\Microsoft\DirectX\UserGpuPreferences`; value name = full exe path, data
  `GpuPreference=2;` for High performance. The first Microsoft page I opened described a `(Default)`
  value, which did not match the per-program format, so I cross-checked it against two other write-ups
  ([NinjaOne](https://www.ninjaone.com/blog/restore-gpu-preferences-for-apps-in-windows/),
  [TrayTrigger wiki](https://github.com/stephenh678/TrayTrigger/wiki/Windows-High-Performance-GPU-Preference-%28profile%29)),
  which agree with each other. The Microsoft answer page is
  [here](https://learn.microsoft.com/en-us/answers/a/6800271). Not verified on a real Windows install.
* **Which programs:** one community thread reports setting `vrserver.exe` fixed SteamVR using the wrong
  card ([thread](https://steamcommunity.com/app/250820/discussions/0/1353742967809419403)). The other
  SteamVR programs are included as the same process family, not individually confirmed.
* **Linux:** NVIDIA's README: `__NV_PRIME_RENDER_OFFLOAD=1` loads the `VK_LAYER_NV_optimus` Vulkan layer;
  GLX additionally needs `__GLX_VENDOR_LIBRARY_NAME=nvidia`; `__VK_LAYER_NV_optimus=NVIDIA_only` reports
  only NVIDIA GPUs; most Vulkan apps use the first GPU reported
  ([NVIDIA PRIME render offload](https://download.nvidia.com/XFree86/Linux-x86_64/535.54.03/README/primerenderoffload.html)).
  Mesa has a Vulkan device-select layer that honours `MESA_VK_DEVICE_SELECT` and then `DRI_PRIME`
  ([Khronos news item](https://www.khronos.org/news/permalink/mesa-20.1-adds-a-vulkan-device-selection-layer-for-better-handling-multi-gpu-setups));
  the accepted values were not given there, so the script uses the well-known `DRI_PRIME=1`.
* **Vulkan on Linux:** see [LINUX_VULKAN_PLAN.md](LINUX_VULKAN_PLAN.md).

## More performance ideas (added for v0.3.0)

Full write-up with verdicts: [PERF_IDEAS.md](PERF_IDEAS.md). In short: peripheral frame-rate reduction
(arXiv 2505.03682) needs the game to skip drawing regions, so it cannot be done from a layer; whole-frame
frame generation already exists in the runtimes; content-adaptive shading is a candidate for later;
Auto foveation (inspired by Meta's dynamic fixed foveation) was built. Edge upscaling was first
judged quality-only; in v0.4.0 it was built as *Smooth low-res edge*, because a smooth edge lets the
sharp area shrink (Matthew's argument), see PERF_IDEAS.md section 3.

## The user's 10 % requirement

I read "at least 10 percent overhead" as a target of **at least 10 % less shading work**. The default
(70 % x 70 % oval, ring 50 %, 4x4 edge) is estimated at about **49 %** less pixel-shading work, and a
unit test asserts the default stays at or above 10 %. This is an estimate of shading invocations
only. Measure fps on your own games.

## Known limits (be honest with users)

1. **Only a Direct3D 12 backend exists.** Direct3D 11 (most older OpenXR games) and Vulkan (what ALVR
   uses on Linux) are not implemented. D3D11 only has vendor routes (e.g. NVAPI); Vulkan needs a
   render-pass hook. On those the layer reports "pass-through".
2. **The D3D12 backend is compile-checked, not run.** It was built with mingw-w64 and its vtable slots
   were verified with static asserts against Microsoft's headers, but it has not been run in a real
   game on real hardware.
3. It only catches render targets whose descriptor was created **after** the OpenXR session, and
   descriptors **copied** with `CopyDescriptors` are not followed. Games that render into their own
   buffers created earlier may see no effect.
4. Array-texture swapchains (both eyes in one texture) share one rate image, centred on the average of
   the two eyes.
5. OpenVR-only games do not use OpenXR, so the layer does not see them (unless they run through a
   translation layer such as OpenComposite; not tested).
6. Games with their own dynamic resolution may not show an fps change (see above).

## Sources

* ALVR repository documentation: https://github.com/alvr-org/alvr
* OpenXR Toolkit repository (MIT): https://github.com/mbucchia/OpenXR-Toolkit
* OpenXR Toolkit FFR announcement and explanation: https://forums.flightsimulator.com/t/openxr-toolkit-upscaling-world-scale-hand-tracking-release-thread/493924
* OpenXR API layers and VRS explained: https://varjo.com/blog/dynamic-foveated-variable-shader-rate-with-openxr-toolkit-and-aero-a-k-a-foveated-rendering
* Half-Life: Alyx PC foveated rendering (VR.org, 2026-10-27): https://vr.org/articles/half-life-alyx-foveated-rendering-pc-eye-tracking-13-headsets-2026
* Unity: foveated rendering in OpenXR: https://docs.unity3d.com/Packages/com.unity.xr.openxr@1.19/manual/features/foveatedrendering.html
* OpenXR `XR_FB_foveation_vulkan`: https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrSwapchainImageFoveationVulkanFB.html
* Microsoft DirectX-Headers (MIT): https://github.com/microsoft/DirectX-Headers
* Khronos OpenXR-SDK headers: https://github.com/KhronosGroup/OpenXR-SDK
* Foveated frame-rate reduction study: https://arxiv.org/html/2505.03682v1
* Microsoft VRS blog: https://devblogs.microsoft.com/directx/variable-rate-shading-a-scalpel-in-a-world-of-sledgehammers/
* Meta fixed foveated rendering: https://developers.meta.com/horizon/documentation/native/android/os-fixed-foveated-rendering/
* Khronos `xrGetD3D12GraphicsRequirementsKHR`: https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrGetD3D12GraphicsRequirementsKHR.html
* Windows per-app GPU preference: https://learn.microsoft.com/en-us/answers/a/6800271 , https://www.ninjaone.com/blog/restore-gpu-preferences-for-apps-in-windows/ , https://github.com/stephenh678/TrayTrigger/wiki/Windows-High-Performance-GPU-Preference-%28profile%29
* SteamVR on the wrong GPU (community thread): https://steamcommunity.com/app/250820/discussions/0/1353742967809419403
* NVIDIA PRIME render offload: https://download.nvidia.com/XFree86/Linux-x86_64/535.54.03/README/primerenderoffload.html
* Mesa Vulkan device-select layer: https://www.khronos.org/news/permalink/mesa-20.1-adds-a-vulkan-device-selection-layer-for-better-handling-multi-gpu-setups
* Vulkan fragment shading rate: https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_fragment_shading_rate.html , https://www.phoronix.com/news/RADV-fragment-shading-rate , https://www.phoronix.com/news/Intel-ANV-Fragment-Shading-Rate
