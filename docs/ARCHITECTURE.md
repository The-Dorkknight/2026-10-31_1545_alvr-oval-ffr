# Architecture

```
 VR game (OpenXR, D3D12)
        |  xrCreateSession / xrCreateSwapchain / xrLocateViews / xrEndFrame ...
        v
 OvalFFR API layer  (layer/src/layer.cpp)        <- config.ini  (written by the UI, polled every 400 ms)
        |  tells a Backend: swapchain images, eye centres, config
        v
 Backend (layer/src/backend_d3d12.cpp)           -> status-<pid>.txt, layer.log  (read by the UI "Check")
        |  shading-rate image per eye (R8_UINT), bound with RSSetShadingRateImage
        v
 OpenXR runtime (SteamVR / ALVR path)
```

## Pieces

| File | Role |
|---|---|
| `layer/include/ovalffr_core.h`, `src/ovalffr_core.cpp` | Mask maths, config parse/write, savings estimate, optical-axis centre. No GPU, no OpenXR. |
| `layer/src/layer.cpp` | Loader negotiation, hooks 8 OpenXR calls, config hot reload, status/log files. |
| `layer/src/backend.h` | The small interface a graphics backend implements. |
| `layer/src/backend_d3d12.cpp` | Windows D3D12 VRS tier 2 backend. |
| `layer/src/backend_null.cpp` | Non-Windows: pass-through, plus a mock backend for tests. |
| `layer/include/ovalffr_gpu.h`, `src/ovalffr_gpu.cpp` | Graphics-card verdict: which card the runtime and the game are on, versus the fastest one. Pure logic, unit-tested. |
| `layer/include/ovalffr_dyn.h`, `src/ovalffr_dyn.cpp` | Auto foveation controller: frame intervals in, sharp-area scale out. Pure logic, tested with simulated games. |
| `layer/src/gpu_dxgi.cpp` | Windows only: lists adapters through DXGI (loaded at run time). |
| `layer/src/active_owner.h` | Tiny helper: "newest live session" pointer that an older session cannot clear. |
| `src/ovalffr/gpu.py` | Windows per-program GPU preference (registry, journal, read-back, undo); Linux environment. |
| `tools/ovalffr-gpu-linux.sh` | One-launch PRIME offload wrapper. |
| `src/main.py`, `src/ovalffr/*.py` | tkinter UI, Python mirror of the mask maths, installer, text mode. |

## Shading-rate codes

`(log2(x) << 2) | log2(y)`: `0` = 1x1, `5` = 2x2, `10` = 4x4. This is the D3D12 encoding.
If the GPU does not report `AdditionalShadingRatesSupported`, the layer never emits 4x4.

## D3D12 backend, step by step

1. `xrCreateSession` with a D3D12 binding: query `D3D12_FEATURE_D3D12_OPTIONS6`. If the variable
   shading rate tier is below 2, report pass-through and do nothing.
2. Patch two shared COM vtable slots once per process (never removed; originals always called first):
   `ID3D12Device::CreateRenderTargetView` (slot 20) and
   `ID3D12GraphicsCommandList::OMSetRenderTargets` (slot 46). Both slot numbers are checked by
   compile-time asserts against Microsoft's headers (see the notes in `docs/RESEARCH_NOTES.md`).
3. `CreateRenderTargetView` hook: remember descriptor handle -> (resource, width, height).
4. `OMSetRenderTargets` hook: after the original call, if the first target is an eye-sized target or a
   known OpenXR swapchain image, call `RSSetShadingRate(1x1, {PASSTHROUGH, OVERRIDE})` and
   `RSSetShadingRateImage(mask)` on that command list. If a different target is bound on the same list
   later, clear them again.
5. Masks are rebuilt (upload + barrier to `SHADING_RATE_SOURCE`, fence wait) when the config or an
   eye's field of view changes. Old masks are freed 3 s later so in-flight frames never lose one.

## Auto foveation flow

`xrWaitFrame` is hooked. The time between two returns is the game's frame interval; with
`predictedDisplayPeriod` it goes to `DynamicFoveation::update()`. When the scale changes, the layer
multiplies the user's `h`/`v` by it (`effective_config_locked`), calls `Backend::onConfig` (masks are
rebuilt, in D3D12 a copy plus a fence wait) and rewrites `status-<pid>.txt` (`auto`, `auto_scale`,
`auto_state`). Idle frames (`shouldRender` false) are ignored. Details and test results:
[PERF_IDEAS.md](PERF_IDEAS.md).

## Smooth low-res edge flow (v0.4.0)

1. `xrEnumerateSwapchainImages`: the backend learns each colour swapchain's images, format and sample count.
2. `xrAcquireSwapchainImage`: remembers which image index the game is drawing into.
3. `xrReleaseSwapchainImage`: before forwarding, the backend records on one of three ring command lists:
   barrier RENDER_TARGET -> COPY_SOURCE, `CopyResource` into a private copy texture, barrier back, then one
   full-screen triangle per array slice with `layer/shaders/smooth.hlsl` (root constants = mask parameters,
   one SRV of the copy). It executes it on the game's queue and signals its own fence. If the oldest ring slot
   is still busy, that frame is skipped (never waits).
4. Views are created with the original `CreateRenderTargetView` and bound with the original
   `OMSetRenderTargets`, so OvalFFR's own VRS hook never applies a shading-rate image to this pass.
5. View format: the texture's own format if it is typed, otherwise the format the app asked for.
   MSAA and unusual formats are skipped per swapchain (logged).
6. Destroyed swapchains: copy texture and descriptor slots are freed only after the fence passes.

`layer/src/ovalffr_smooth.cpp` is a line-by-line C++ copy of the shader, used by `tests/smooth_test.cpp`.

## fps box, zones, VRS check, keys (v0.5.0)

* `xrWaitFrame`: frame interval and time blocked inside it, every frame. Each second: fps, Hz, late
  frames (> 1.35x the display period), spare time, the VRS-check result from the backend, the A/B
  test state machine, the status file, and one second added to the current fps-log block (one row
  per setting per 2 minutes). Then Windows key polling (`GetAsyncKeyState`, Ctrl+Alt+key, on
  key-down) and `commands.txt` (lines `<unix time> <command>`), both only while the session is focused.
* `xrPollEvent`: follows the session state, so only the FOCUSED app takes keys and commands.
* The layer builds the two text lines and hands `Backend::Overlay {text, zones, line1, line2}` to the
  backend when something changed.
* D3D12, in the release-time command list after the optional smoothing: `CSCount` (compute, one
  thread per 16x16 pixel cell, `InterlockedAdd` into a 16-byte slot per ring entry, copied to a
  persistently mapped read-back buffer and summed when that ring slot comes round again), then
  `PSOverlay` (alpha-blended, colour channels only) for the text box and the zone tint. Root
  signature: 36 root constants (mask parameters, flags, text position/scale, 64 glyph indices), one
  SRV table (the copy), one root UAV (the counter).
* Font: 5x7, 50 glyphs, generated into the shader and `layer/src/ovalffr_font.h` by `tools/gen_font.py`.

## Why the GPU hook acts on SteamVR and not only on the game

OpenXR makes the **runtime** choose the adapter: the game calls `xrGetD3D12GraphicsRequirementsKHR`, gets
the runtime's adapter LUID back and must create its device on that card
([spec](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrGetD3D12GraphicsRequirementsKHR.html)).
The layer hooks that call to learn the runtime's LUID, compares it with the DXGI adapter list, and
reports `gpu_verdict` = `ok`, `single`, `game_on_weak`, `runtime_on_weak` or `unknown`. Unknown cards are
never reported as wrong (no false alarms).

## Files the layer and UI share

Windows `%LOCALAPPDATA%\OvalFFR\`, Linux `~/.config/ovalffr/`, macOS `~/Library/Application Support/OvalFFR/`.

* `config.ini` UI -> layer
* `status-<pid>.txt` layer -> UI, one per VR program: `state=active|passthrough`, `app=`, `backend=`, `gpu=` or `reason=`, the `gpu_*` verdict lines, and `ended=1` once the program has closed. The panel shows the newest one still running and deletes files older than 7 days.
* `gpu_changes.json` UI only: what the GPU-preference buttons changed, so Undo restores it exactly
* `layer.log` layer -> you (restarts at 256 KB)

## Tests

* `tests/core_test.cpp` unit tests for the maths (savings >= 10 % by default, zones, clamping, FOV).
* `tests/core_test.cpp` also simulates games against the Auto controller.
* `tests/fuzz_test.cpp` random configs/sizes/bytes/FOVs with fixed seed; checks invariants.
* `tests/layer_test.cpp` loads the real shared library against a fake OpenXR runtime (includes the edge cases).
* `tests/test_gpu.py` registry logic against an in-memory store, undo journal, verdict text.
* `tests/test_mask_parity.py` Python preview == C++ mask.
* `tests/test_installer.py`, `test_config.py`, `test_launchers.py`.
