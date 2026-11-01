# Linux / Vulkan: how much work is it?

Written 2026-11-01 for v0.2.0. These are **my estimates, not measurements**. Nothing in this document has
been run on a Vulkan GPU yet.

## Short answer

| Scope | Effort (one developer, focused days) | What you get |
|---|---|---|
| **A. Vulkan backend, games that use dynamic rendering** | **about 10-14 days** | Foveation on Linux for native Vulkan OpenXR apps that render with `vkCmdBeginRendering` |
| **B. A plus classic render passes** | **+ about 3-4 days** (13-18 total) | Also covers apps that use `VkRenderPass`/`VkFramebuffer`, which is most older Vulkan code |
| C. Both, tested on three GPU vendors | already counted below, but allow **calendar weeks**, not days, because hardware access is the bottleneck | Something I would call "beta" |

Roughly: **three weeks to a first working Vulkan version, four to five weeks to something solid.**
The code is not the hard part. The unknowns are (1) which games actually reach this code path on Linux
and (2) getting access to enough GPUs.

## What carries over unchanged

The OpenXR layer (`layer.cpp`), the mask maths, the config file, the UI, the installer and the status
files are all API-neutral. A Vulkan backend is a new file next to `backend_d3d12.cpp` that implements
the same small `Backend` interface (`layer/src/backend.h`). The Linux build of the layer already loads
and passes its tests; today it reports "pass-through" because it has no backend.

## What has to be written

| # | Piece | Days | Why |
|---|---|---|---|
| 1 | Backend skeleton: recognise `XrGraphicsBindingVulkanKHR` / `...Vulkan2KHR`, collect swapchain `VkImage`s | 1 | Same job as the D3D12 session/swapchain bookkeeping |
| 2 | Turn the feature on: device extension `VK_KHR_fragment_shading_rate` plus the three features (`pipelineFragmentShadingRate`, `primitiveFragmentShadingRate`, `attachmentFragmentShadingRate`) | 1-2.5 | With `XR_KHR_vulkan_enable2` the app creates its device through `xrCreateVulkanDeviceKHR`, which an OpenXR layer can intercept and patch (my reading of the OpenXR specification; not tried). With the older `XR_KHR_vulkan_enable` the app calls `vkCreateDevice` itself, which needs a separate **Vulkan implicit layer** as well. That is the uncertain part |
| 3 | Shading-rate image: `R8_UINT`, `FRAGMENT_SHADING_RATE_ATTACHMENT` usage, size from the device's attachment texel size, one layer per eye if multiview, upload through a staging buffer and transition to the shading-rate layout | 1-1.5 | Same idea as `upload()` in the D3D12 backend |
| 4 | Dynamic-rendering path: hook `vkCmdBeginRendering`, and when the colour attachment is an eye swapchain image chain a `VkRenderingFragmentShadingRateAttachmentInfoKHR` | 1 | Needs a `VkImageView` -> `VkImage` map (hook `vkCreateImageView`), like the D3D12 RTV map |
| 5 | Pipeline combiner: the default combiner is KEEP/KEEP, which **ignores** the image. Pipelines must be created with `{KEEP, REPLACE}` (or the dynamic state set per command buffer) | 1.5-2 | Hook `vkCreateGraphicsPipelines`, copy the create-info, add the state. Easy to get subtly wrong with pipeline libraries and derivatives |
| 6 | Classic render passes (**scope B**): rewrite `vkCreateRenderPass`/`vkCreateRenderPass2` to add the shading-rate attachment to each subpass, and `vkCreateFramebuffer` to add the extra view | 3-4 | Most invasive piece: the app's attachment indices shift |
| 7 | Hybrid-laptop launch wrapper | 0.5 | Already written for 0.2.0 (`tools/ovalffr-gpu-linux.sh`) |
| 8 | Hardware testing and fixes: NVIDIA, AMD (RADV), Intel (ANV) | 4-8 | The big range. Each vendor has its own texel size and quirks |

Dynamic-rendering-only total (rows 1-5, 7, 8): **about 10-14 days**. Add row 6 for scope B.

## What I can and cannot test without hardware

* On the development box here only **lavapipe** (software Vulkan) is available, and it does not offer
  fragment shading rate. So the plumbing, the validation layers and the create-info rewriting can be
  tested here, but **not** whether the picture is actually coarser.
* A **ROG Ally (RDNA3) on Bazzite** would be a good first real test machine for AMD (RADV).
  The Linux Mint laptop's GPU is not known yet.

## GPU support on Linux (from the sources, not from testing)

* Vulkan feature: `VK_KHR_fragment_shading_rate`, with a per-pixel "attachment" mode, per the
  [Vulkan proposal document](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_fragment_shading_rate.html).
* AMD **RADV**: the Phoronix headline at the time said support was limited to **GFX10.3 (RDNA 2)** and
  newer ([RADV article](https://www.phoronix.com/news/RADV-fragment-shading-rate)). Newer Mesa may differ.
* Intel **ANV**: support was added ([ANV article](https://www.phoronix.com/news/Intel-ANV-Fragment-Shading-Rate));
  which generations support the attachment mode I have not confirmed.
* NVIDIA proprietary driver: Turing (GTX 16 / RTX 20) and newer is the usual statement; not re-checked
  for Linux.

## Open questions I could not settle from here

1. **Which games reach this?** Many Linux VR users run Windows games through Proton. Those are D3D11/D3D12
   translated by DXVK/VKD3D-Proton, so whether an OpenXR layer is loaded for them at all, and whether a
   Vulkan-level layer sees their render passes, is **unverified**. Native Vulkan OpenXR apps are the
   safe target.
2. **Which OpenXR runtime does a Linux ALVR user end up with** (SteamVR's, or Monado)? The layer should
   work with either because the OpenXR loader loads it, but I have not seen it run on either.
3. **Exact bit layout of the shading-rate image texel** in Vulkan versus the D3D12 encoding the core
   uses today. Check the specification text before coding; do not assume they match.
4. **Flatpak (Bazzite):** whether a sandboxed app sees `~/.local/share/openxr/1/api_layers/implicit.d`.
   Unverified.
5. **DXVK and VKD3D-Proton** and dynamic rendering: how often they use it. Unverified.

## Recommendation

Start with scope A on a native Vulkan OpenXR test app, on the ROG Ally. If that works end to end,
decide on scope B based on which games the Linux users actually run. A D3D11 backend (for the many
older OpenXR games on Windows) is a separate project of similar size and would help more people than
Linux/Vulkan would.

## Sources

* [VK_KHR_fragment_shading_rate proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_fragment_shading_rate.html)
* [VK_KHR_fragment_shading_rate reference page](https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VK_KHR_fragment_shading_rate.html)
* [VK_KHR_dynamic_rendering reference page](https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VK_KHR_dynamic_rendering.html)
* [Phoronix: RADV fragment shading rate](https://www.phoronix.com/news/RADV-fragment-shading-rate)
* [Phoronix: ANV fragment shading rate](https://www.phoronix.com/news/Intel-ANV-Fragment-Shading-Rate)
* [Phoronix: RADV dynamic rendering](https://www.phoronix.com/news/RADV-Dynamic-Rendering)
