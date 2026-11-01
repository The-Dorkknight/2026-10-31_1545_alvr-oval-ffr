# Edge-case analysis (v0.2.0)

Done 2026-11-01 on v0.1.0, fixes in v0.2.0. Method: read every hook and file-format path looking for
"what if the input is odd", wrote a test for each suspicion, ran the tests against the **old** layer to
see which suspicions were real, then fixed and re-ran.

**Status words**

| Status | Meaning |
|---|---|
| **Fixed, tested** | A test fails on v0.1.0 (or would) and passes now, on Linux |
| **Fixed, logic tested** | The decision logic is tested on Linux, the Windows-only part around it is not |
| **Fixed, not run** | Code changed, but it is Windows/GPU code that has only been compiled |
| **Open** | Known, not fixed |
| **Unverified** | A guess or a report I could not confirm |

## Confirmed defects in v0.1.0 (the old layer fails the new tests)

| # | Case | What happened | Status |
|---|---|---|---|
| 1 | Two slider saves within the same second | Layer compared file modification **time** (1 s resolution on many filesystems), so the second save was ignored until the next change | **Fixed, tested.** Now compares the file's content |
| 2 | `h=nan` or `v=inf` in `config.ini` | NaN slipped through clamping, every tile compared false, the **whole view became the lowest rate** | **Fixed, tested.** Non-finite values fall back to the defaults, in C++ and in the Python panel |
| 3 | Runtime that wiggles its FOV by a tiny amount every frame | Mask rebuilt (GPU upload + fence wait) **every frame** | **Fixed, tested.** Changes under 0.004 are ignored (hysteresis) |
| 4 | Config file deleted while a game runs | Old settings stuck | **Fixed, tested.** Falls back to defaults |
| 5 | Python panel reads a `config.ini` containing a stray byte (or a BOM from Notepad) | `UnicodeDecodeError`, panel crashed | **Fixed, tested** |
| 6 | Python and C++ disagreeing on `tile=0` or a NaN centre | Preview would not match what the GPU gets | **Fixed, tested** (parity test has both cases) |

## Behaviour gaps closed

| # | Case | Now | Status |
|---|---|---|---|
| 7 | Game uses **one array swapchain for both eyes** | Both eyes share one mask centred between them (was "last eye wins") | **Fixed, tested** against the mock backend; the D3D12 side of it is compile-only |
| 8 | **Quad-view** apps (4 views, e.g. foveated headsets) | Foveation switches itself off with the reason `quad views`, and says so in the panel | **Fixed, tested** (mock) |
| 9 | An **overlay or helper app** loads the layer after the game and overwrote `status.txt` | One file per process: `status-<pid>.txt`; panel shows the newest running one; the panel deletes status files older than 7 days | **Fixed, tested** |
| 10 | Status after the game closed | `ended=1` is written; panel says "that program has since closed" | **Fixed, tested** |
| 11 | Second VR session in the same process | Eye centres reset and re-learned | **Fixed, tested** |
| 12 | Absurd frame size or tile size (0, negative, billions) | Refused (empty mask) instead of allocating gigabytes | **Fixed, tested** (fuzzing, 5,000 random cases plus 2,000 random byte strings) |
| 13 | Exceptions escaping our code into the game's OpenXR calls | Every hook body is wrapped; our failure becomes "pass-through" | **Fixed, not run** (cannot force an exception from outside) |

## Windows / Direct3D 12 (compiled with mingw-w64, never run on a GPU)

| # | Case | Status |
|---|---|---|
| 14 | New session created before the old one is destroyed: the old backend's destructor used to switch the **new** one off | **Fixed, logic tested** (`ActiveOwner`, unit test). D3D12 wiring: not run |
| 15 | Descriptor slot reused for a **null** render target kept pointing at the old texture | **Fixed, not run** |
| 16 | Two threads rebuilding masks at once shared one fence and one event | **Fixed, not run** (upload lock) |
| 17 | GPU hung or device removed while the layer waited on its fence | **Fixed, not run.** Layer marks itself `broken`, stops touching the GPU, panel says so |
| 18 | A hook running at the exact moment the session is destroyed | **Open.** Narrow window; a reference-counted hook state would close it |
| 19 | Games using `BeginRenderPass` (D3D12 render-pass API) | **Open.** Not hooked, so no foveation there |
| 20 | Render targets copied with `CopyDescriptors`, or created before the session | **Open** (already listed in the README) |
| 21 | User name with non-ASCII characters in the install path (ANSI vs UTF-16 loading) | **Open / unverified.** Not tried on Windows |
| 22 | `dxgi.dll` missing or `EnumAdapterByGpuPreference` unavailable (older Windows 10) | **Fixed, logic tested.** Adapter list stays empty, verdict is "unknown", nothing breaks. The Windows enumeration code itself: not run |

## Installer and panel

| # | Case | Status |
|---|---|---|
| 23 | Reinstalling while SteamVR holds the DLL open (Windows) | **Fixed, logic tested.** File is copied to `.new` then swapped; a permission error becomes "close SteamVR, the ALVR dashboard and any VR game, then press Install again". Real Windows file locking: not run |
| 24 | Uninstall could not delete a file and said nothing | **Fixed.** Errors are returned and shown. Failure path only simulated |
| 25 | `XDG_DATA_HOME` set on Linux (loader looks there) | **Fixed, tested** |
| 26 | Live status refresh stopped for good after one exception, and rebuilt widgets every 3 s | **Fixed.** Checked only by the headless UI self-test |
| 27 | Config save fails (read-only folder, full disk) | Already reported in the status bar; **unchanged** |

## Dual-GPU laptops (see the README section "Laptops with two graphics cards")

| # | Case | Status |
|---|---|---|
| 28 | SteamVR/ALVR starts on the integrated GPU and drags every game with it | Layer detects it (runtime adapter vs. fastest adapter) and the panel offers the fix. **Detection logic tested; real DXGI enumeration not run** |
| 29 | Registry value already holds other settings (`SwapEffectUpgradeEnable=1;`) | **Fixed, tested.** Only `GpuPreference` is changed; the rest is kept |
| 30 | User clicks the button twice, then Undo | **Fixed, tested.** First (original) value is the one restored; a program that had no entry gets its entry removed |
| 31 | Windows ignores or rejects the write | **Fixed, tested** with a fake store: every write is read back; a failed read-back is reported as a failure |
| 32 | Journal written after the registry (crash in between = no undo) | **Fixed, tested.** Journal first |
| 33 | Damaged undo journal | **Fixed, tested.** Treated as empty |
| 34 | GPU maker's own control panel (NVIDIA / AMD) has a per-program setting too | **Unverified.** I did not find out which one wins |
| 35 | Remote-desktop session or virtual machine (only a basic adapter visible) | **Unverified.** Expected to report "unknown" or "single GPU" |
| 36 | Only `vrserver.exe` is backed by a user report that this fixed the problem | **Unverified for the others** (`vrcompositor`, `vrstartup`, `vrmonitor`, ALVR dashboard): same process family, not confirmed individually |
| 37 | Linux: Flatpak app does not inherit the wrapper's variables | **Documented** (Flatpak override command in the script header and README). **Unverified** on Bazzite |

## Auto foveation (v0.3.0)

| # | Case | Status |
|---|---|---|
| 38 | CPU-bound or network-bound game: shrinking can never help | **Fixed, tested (simulation + closed loop).** Judged at the floor, then back to full size and paused; each repeat waits twice as long. Cost in the model: 7.7% of 40,000 frames below full size |
| 39 | Runtime that halves the frame rate on every miss (no gradual improvement) | **Tested (simulation).** Settles at 0.70 in 8 changes. A first design judged "no gain" too early and was changed after this test |
| 40 | Game running up to 10% over the refresh period | **By design not reacted to** (avoids chasing noise) |
| 41 | Loading hitches, alt-tab, pause | **Tested.** Frames over 8x the period and the first 120 frames are ignored |
| 42 | NaN, negative or zero timings | **Tested.** Ignored |
| 43 | Hunting: grow back, miss, shrink, repeat | **Tested (simulation).** A failed grow-back sets a ceiling and doubles the wait. 42 changes over 36,000 frames of load that switches every 10 s |
| 44 | User raises the floor while Auto is below it | **Tested.** Jumps up to the new floor |
| 45 | User switches Auto off mid-game | **Tested (fake runtime).** Full size restored |
| 46 | Status file did not show Auto until the first scale change | **Fixed, tested.** Found by the layer test; status is now rewritten on every config change |
| 47 | Each change rebuilds the masks on the GPU inside `xrWaitFrame` (copy + fence wait, up to 2 s timeout) | **Unverified on a GPU.** Changes are seconds apart, but a slow rebuild would itself cost a frame |
| 48 | Game with its own dynamic resolution | **Unverified.** It may keep frames on time, so Auto never engages, or the two may fight |
| 49 | Whether SteamVR / ALVR `xrWaitFrame` timing reveals missed frames the way the model assumes | **Unverified. This is the biggest open question** |
| 50 | Noisy timings (busy PC) fooling the "did shrinking help?" verdict, leaving a CPU-bound game stuck at the floor | **Fixed, tested.** Found by running the layer test with 6 CPU-hogging processes on 2 cores (it failed about half the time). The verdict now uses the median frame, averages three windows, and is repeated every 20 windows while at the floor. Same stress: 16 of 16 pass. Simulation with heavy noise: 8.4% of frames below full size |
| 51 | The preview picture shows your sliders, not the live shrunken size | **Documented.** The status row shows the live percentage |


## Smooth low-res edge (v0.4.0)

| # | Case | Status |
|---|---|---|
| 52 | Sharp area must never change | **Tested.** The shader discards those pixels; C++ copy is bit-for-bit unchanged there |
| 53 | Game did not draw coarse into the image (own buffer + post-processing, film grain, dithering) | **Tested (simulation).** Exact-copy test fails, nothing is written: 0 of 307,200 noise pixels, 27 of 307,200 smooth-scene pixels. The copy and pass still cost time |
| 54 | A block's corner pixel passes the "exact copy" test by matching itself | **Found by test, fixed.** 26,432 noise pixels were changed; now all four corners must match |
| 55 | Outline running through a block | **Tested.** Left alone (0 pixels changed) |
| 56 | Outline along a block border, objects of similar brightness but different colour | **Found by test, fixed.** Brightness-only comparison moved an orange/blue outline by 0.27; colour is now compared per channel, worst 0.005 |
| 57 | The smoothing pass itself picked up OvalFFR's VRS rate image (our hook sees every `OMSetRenderTargets`) | **Avoided by design.** Own views and binding call the original functions. Unverified on a GPU |
| 58 | GPU busy | **By design.** Never waits; at most 3 passes in flight, otherwise that frame is not smoothed. Unverified on a GPU |
| 59 | sRGB or typeless swapchain textures | **By design.** Views use the texture's own typed format, or the app's format for typeless textures. Unverified on a GPU |
| 60 | MSAA or unusual swapchain format | **By design.** That swapchain is skipped and logged |
| 61 | `d3dcompiler_47.dll` missing, or Microsoft's compiler rejects the shader | **By design.** Smoothing off, reason in the panel and `layer.log`; the game is unaffected. The shader passes glslang's HLSL front end only |
| 62 | Swapchain destroyed (or re-enumerated) while a pass is in flight | **By design.** Copy texture and descriptor slots are freed only after the fence passes |
| 63 | First use compiles the shader inside `xrReleaseSwapchainImage` | **Known.** A one-time hitch when the option is first used in a session; size unmeasured |
| 64 | Smoothing on while foveation is off; switching it off mid-game | **Tested (fake runtime).** No pass is queued; status says off |


## fps box, zones, keys, A/B test, fps log (v0.5.0)

| # | Case | Status |
|---|---|---|
| 65 | Two programs with the layer loaded (game + an overlay app): a toggle would be applied twice and cancel out | **Fixed by design, tested (fake runtime).** Only the session with input focus (`xrPollEvent` state FOCUSED) takes keys and commands; the other leaves `commands.txt` alone |
| 66 | Command written while no game runs, applied much later by surprise | **Tested.** Commands older than 15 s are ignored |
| 67 | Unknown command in `commands.txt` | **Tested.** Ignored and logged |
| 68 | Ctrl+Alt equals AltGr on UK/European keyboards: AltGr+letter typed in another program also triggers | **Known, documented.** Keys only act while a VR game runs and has focus |
| 69 | Game already at the headset's full refresh rate: fps cannot rise, the A/B test would show "no change" | **Handled.** The result says so and gives spare time instead; README explains raising the render resolution |
| 70 | "Spare time" (time blocked in `xrWaitFrame`) as a headroom figure | **Rough, unverified** with SteamVR/ALVR pacing; labelled rough everywhere |
| 71 | VRS check fooled by flat areas (identical pixels with or without VRS) or lowered by outlines | **Tested (simulation).** 100 % with VRS, 0-14 % without in the test scenes; samples one block per 16x16 pixels |
| 72 | VRS check needs a copy of the image | **By design.** Runs only while zones or smoothing are on; the text-only fps box makes no copy |
| 73 | The overlay costs GPU time and could bias the fps it shows | **Known.** Text only: one blended full-screen draw that discards nearly every pixel. The A/B test has the same overlay in both phases. Not measured |
| 74 | fps log too chatty (first version: one row per second) | **Changed on request.** One row per setting per 2 minutes, a shorter row on a change after at least 20 s, loading screens excluded, A/B phases as their own rows. Tested with shortened timings |
| 75 | A setting changed by a key while the panel shows old values, then the panel saves over it | **Fixed, tested (panel self-test).** The panel re-reads `config.ini` every second and shows outside changes |
| 76 | Text box position in each eye | **By design, unverified in a headset.** Under each lens centre so both eyes see it at the same place; array swapchains use each slice's own centre |

## How it was tested

* `core_test`, `fuzz_test` (fixed seed, 5,000 random configs and sizes with NaN, infinity and huge values, 2,000 random byte strings, 5,000 random FOVs), `layer_test` (real shared library against a fake OpenXR runtime): all pass, also under **AddressSanitizer + UndefinedBehaviorSanitizer** with leak detection.
* The old v0.1.0 layer fails 19 of the new `layer_test` checks. Five of them correspond to defects 1-4 above (same-second save, NaN config, deleted config, FOV jitter); the others are features the old layer simply did not have (per-process status files, GPU lines, quad-view handling, array-swapchain eye).
* 39 Python tests, run with warnings treated as errors.
* Smooth low-res edge: `smooth_test` (C++ copy of the shader on synthetic scenes with a realistic VRS model; shader rate rule checked against the mask on 120,000 sample pixels), `layer_test` checks for the acquire/release hooks, HLSL compiled with glslang's HLSL front end. Also under the sanitizers.
* Auto foveation: controller simulations in `core_test`, closed-loop run in `layer_test` (repeated 3 times to check for flakiness), also under the sanitizers.
* fps box, zones and VRS check: C++ copy of the shader in `smooth_test` (font checked against the hand-drawn letters, zone colours checked exactly); commands, focus, live fps, log blocks and the A/B test in `layer_test` with shortened timings.
* **Not done:** thread-sanitizer or multi-threaded stress run; any run on Windows; any run on a GPU; Microsoft's shader compiler; real ALVR/SteamVR.
