# Changelog

Semantic versioning. Dates are UTC.

## [0.5.0] - 2026-11-01 (work in progress)
### Added
- **In-headset fps box** (Ctrl+Alt+O, off by default): fps, refresh rate, FFR on/off, estimated saving and the VRS check, drawn under each lens centre (also visible in the SteamVR mirror).
- **Zone highlight** (Ctrl+Alt+Z): yellow 2x2, red 4x4, purple where the mask asks for low-res but the image the headset gets is full detail.
- **VRS check**: a compute pass samples one low-res block per 16x16 pixels and reports how many are exact copies (proof that VRS reached the image); in the fps box, the panel, the mini window and the log.
- **Keys while playing** (Ctrl+Alt + O Z F [ ] E S A T) and **commands.txt** (same commands from the panel/mini window); only the VR app with input focus reacts (hooks `xrPollEvent`).
- **A/B test** (Ctrl+Alt+T or a button): FFR off/on/off/on, 10 s each, 3 s warm-up ignored; result in the headset, the panel and the log.
- **Live fps** in the status file every second: fps, Hz, late frames, spare time (rough headroom).
- **fps log** `fps_log.csv`: one row per setting averaged over 2 minutes, a shorter row on a change after at least 20 s, loading screens excluded.
- **Mini fps window** (panel button or `--mini`): always on top, click for quick settings, zones, A/B test, keys and the fps-per-setting table. `--cli fps` / `--cli fps send <command>`.
- The panel follows setting changes made by keys or the mini window; "Show fps in the headset" checkbox; live and A/B rows in the status area.
- Tests: overlay text/font, zone colours and VRS check in `smooth_test`; commands, focus, fps, log blocks and the A/B test against the fake runtime in `layer_test`; `tests/test_live.py`; panel self-test for outside changes. `docs/overlay_demo.png`, `docs/mini_window.png`.
### Notes
- The D3D12 overlay and VRS-check passes are compile-checked only; the shaders pass glslang's HLSL front end, not Microsoft's compiler; the Windows key handling has never run.

## [0.4.0] - 2026-11-01 (work in progress)
### Added
- **Smooth low-res edge** (experimental, off by default): after the game draws each eye, a small D3D12 pass blends the blocky 2x2/4x4 pixels into a bilinear upscale, so a smaller sharp area can look the same. Edge-preserving per colour channel; never writes the sharp area or any block that is not an exact copy. Shader in `layer/shaders/smooth.hlsl`, compiled at run time by `d3dcompiler_47.dll`; any failure switches off only the smoothing. Panel checkbox and strength slider, status row, `--cli set smooth=1 smooth_strength=60`. Hooks `xrAcquireSwapchainImage` / `xrReleaseSwapchainImage`.
- `tests/smooth_test.cpp` (C++ copy of the shader against a realistic VRS model), `tests/smooth_demo.cpp` + `tools/make_smooth_demo.py` for `docs/smooth_demo.png`, layer-test checks for the new hooks, 2 Python tests.
- Edge cases 52-64 in `docs/EDGE_CASES.md`; `docs/PERF_IDEAS.md` section 3.
### Notes
- The D3D12 pass is compile-checked only (mingw-w64), the HLSL passes glslang's HLSL front end but has never been through Microsoft's compiler, and nothing has run on a GPU. Cost not measured.

## [0.3.0] - 2026-11-01 (work in progress)
### Added
- **Auto foveation** (experimental, off by default): shrinks the sharp area in small steps only while the game misses display refreshes, never below a floor; gives quality back if shrinking does not help; hooks `xrWaitFrame` for frame timing. Panel checkbox, floor slider, live status row; `--cli set auto=1 auto_floor=50`.
- `docs/PERF_IDEAS.md`: research on peripheral frame-rate reduction, frame generation, edge upscaling, content-adaptive shading and Auto foveation, with a verdict for each.
- Panel scrolls when the screen is too short; status bar always visible.
### Notes
- Simulated-game tests for the controller, a closed-loop run against the fake runtime, and the sanitizer run all pass. No real game, GPU or fps number yet.

## [0.2.0] - 2026-11-01 (work in progress)
### Added
- **Dual-GPU laptops:** the layer reports which graphics card the VR runtime and the game use and whether that is the fastest one (`gpu_verdict` in the status file, shown in the panel). Windows: one-click, confirmed, read-back and undoable per-program "High performance" preference for SteamVR/ALVR and any program you add (panel buttons and `--cli gpu ...`). Linux: `tools/ovalffr-gpu-linux.sh` sets PRIME offload variables for one launch.
- Edge-case analysis: `docs/EDGE_CASES.md` (37 cases with a status each).
- Linux/Vulkan effort estimate: `docs/LINUX_VULKAN_PLAN.md`.
- Fuzz test, AddressSanitizer/UBSan clean run, 28 Python tests (GPU registry logic against an in-memory store).
### Fixed
- Two config saves within one second: the second was missed (compared modification time; now compares content).
- NaN/infinity in `config.ini` turned the whole view into the lowest shading rate; now falls back to defaults (C++ and Python agree).
- A runtime that jitters its field of view rebuilt the masks every frame; now has hysteresis.
- Deleted config file left old settings active.
- Panel crashed on a config file with a stray byte or BOM.
- Array-texture swapchains (both eyes in one) now use one shared mask; quad-view apps switch foveation off with a stated reason.
- An overlay app loading the layer could overwrite the game's status; status is now one file per process.
- D3D12 (compile-checked only): stale descriptor slots, session overlap, concurrent uploads, GPU hang handling, exceptions never cross into the game.
- Installer: locked DLL gives a plain-words message; uninstall reports files it could not remove; honours `XDG_DATA_HOME`; panel no longer rebuilds its status rows every 3 s.
### Not yet
- Everything listed under "Not yet" in 0.1.0, plus: real Windows registry write and real adapter enumeration have never been run.

## [0.1.0] - 2026-10-31 (work in progress)
### Added
- OpenXR API layer (`XR_APILAYER_OVALFFR_oval_ffr`): loader negotiation, hooks, hot-reloaded config, status and log files.
- Oval/slot shading-rate masks (superellipse), 3 zones (1x1 / 2x2 / 2x2 or 4x4), centred on each lens from the runtime FOV.
- Direct3D 12 Variable Rate Shading backend (**compile-checked only, not yet run in a game**).
- tkinter control panel: horizontal and vertical sliders, shape, ring, edge strength, live picture of the low-res area, estimated saving.
- Install / Check / Uninstall with read-back verification and "did a VR app actually load it" status.
- Click-and-launch scripts for Linux, Windows and Mac; text-mode fallback.
- Tests: maths, fake-runtime layer test, C++/Python mask parity, installer round trip, launchers.
### Not yet
- Direct3D 11 and Vulkan backends; real-game testing; Windows registry install path tested on Windows.
