# iPhone / iPad build

Dead Rising runs on iOS the same way it runs on Windows: the game's code is recompiled from your
`default.xex`, its shaders are translated from your `shader.arc`, and the runtime replays the
game's Direct3D on the phone's GPU. On iOS the renderer uses Vulkan through **MoltenVK** (Vulkan on
Metal), the same Vulkan path that runs on Windows with `DR_GPU=vulkan`, so the SPIR-V shader cache
is shared.

Status: **built, not yet run on a device** (2026-11-25, GitHub Actions run 10: `DeadRising-10.ipa`,
13.7 MB: the 39.5 MB arm64 executable and MoltenVK 1.4.2; release `ios-10` of the private
repository). What the first runs needed:

* MoltenVK v1.2.11 has no release files: v1.4.2.
* XenosRecomp crashed on macOS (status 139): one vertex shader exports to register 63 (point size /
  edge flag / kill vertex), which has no interpolator; the translator read `interpolators.end()`
  (an empty name on Windows by luck, garbage with libc++). Such exports are now dropped (on
  Windows that shader had been writing its value into `oTexCoord1.x`). Found by translating every
  shader with an AddressSanitizer build on Windows. The Mac run translates on one thread
  (`XENOS_THREADS`) and prints a stack trace if it faults.
* FFmpeg's libraries: `find_library(... NO_CMAKE_FIND_ROOT_PATH)` (CMake looked inside the iOS SDK).
* Objective-C enabled at the top level on Apple (plume_apple.mm, SDL).
* The log macros with no arguments (`LOGFN("text")`): `__VA_OPT__(,)` (clang-cl dropped the comma,
  Apple clang does not); `ERROR_INVALID_PARAMETER` defined outside Windows.

First run on an iPhone (build 10, the user's log): the game files were found and the game
started, then hung after its first frame, right after creating its XMA contexts (on PC the next
calls are the network start-up and the first input poll). Build 12: the audio thread kept the
game's mixer waiting while the device queue was full (the iOS audio session not playing), and the
game's audio set-up waits on its mixer; the mixer now runs in real time regardless and drops the
output when the device does not play (`[audio] the device is not playing`). Build 12 also has
the hang watchdog (`runtime/src/os/hang_watchdog.cpp`): after 15 s without a frame, `[watchdog]`
lines give each game thread's last import, how many it made, and on Apple its stack as `sub_`
functions. Not yet confirmed on the device.

Build 12 on the iPhone (watchdog): still black; the main game thread slept in the allocator's
`RtlEnterCriticalSection` (from `sub_827612B0`) with 0 imports in 15 s while every other game
thread waited for work, so no thread held the lock: a lost wake-up. The kernel's critical
sections, events, semaphores and thread suspension waited with C++20 `std::atomic` wait /
`notify_one`; Apple's C++ library keeps 1- and 4-byte waiters in a shared hash table where
`notify_one` can wake another address's waiter. Build 13 waits through `kernel/park.h`
(a mutex and condition variable per address bucket, the condition re-checked under the lock,
wakes to the whole bucket, a 10 ms re-check). Build 13 also shows a status panel when the game
gives no picture for 2 s (ui/options_menu.cpp, drawn by the renderer over the last picture or
black): what is happening and the last 36 log lines; `DR_TEST_STALL=<frame>:<seconds>` holds the
game to test it. Stack names in `[watchdog]` lines are approximate (the nearest recompiled
function by host address); the guest `lr` values are exact.

Build 13 on the iPhone: still black, the same thread asleep in `RtlEnterCriticalSection` with the
lock free. The cause was `compare_exchange_weak`, which on ARM may fail without another owner (it
can on load-linked / store-conditional); the code then slept waiting for an owner that did not
exist. Build 15 uses `compare_exchange_strong` there and in the events, semaphores and
`RtlTryEnterCriticalSection`, and retries when the lock turns out free. Build 15 also moves
everything that is not the game to UIKit (below) and renders on a thread of its own.

Build 15 on the iPhone: past the freeze (start screen, native overlay, the logos' first frames),
then the app ended at guest frame 8-16 with nothing in the log, right as the first texture decode
started. Most iPhone GPUs have no BC (DXT) formats (Metal's `supportsBCTextureCompression`), and
nearly every texture of the game is DXT. Build 16:

* decodes DXT1-5, DXN and DXT5A on the CPU when the GPU lacks BC (`gpu/texture_decode.cpp`, to
  RGBA8 / RG8 / R8; `DR_SOFTWARE_BC=1` does it on a PC for testing; checked against the GPU path on
  the title screen). That costs memory (title: 71 MB as BC, 389 MB as RGBA8), so large 2D textures
  with mip levels start at their second level there (127 MB; `DR_SOFTWARE_BC_FULL=1` keeps them);
* has a crash handler on Apple platforms (`os/hang_watchdog.cpp`): `[crash]` lines with the signal,
  address, thread, its last import and the stack (recompiled functions as `sub_`, the rest as
  symbol and executable offset; `DeadRising-N.symbols.gz` in the release names the offsets),
  uncaught C++ exceptions with their message; `DR_CRASH_HANDLER=0` off;
* logs memory (`[memory]`: the footprint and what iOS leaves before it ends the app, every 5 s,
  when big buffers are made, on iOS memory warnings and below 600 MB left);
* makes the resident geometry buffer 192 MB (768 on a PC) and the upload buffers 48 MB (they
  grow when a frame needs more);
* lists directories without throwing (an exception nothing catches ends the app on iOS).

Build 16 on the iPhone: the GPU has BC formats after all; the crash report showed MoltenVK binding a
null pipeline (plume returned an object when `vkCreateGraphicsPipelines` failed). Build 18 skips pipelines
that could not be made and logs stderr (MoltenVK's reasons) as `[stderr]` lines. Build 18 on the
iPhone: the Capcom logo, the title screen and sound, then a crash freeing a framebuffer whose depth
view read its already freed texture (the renderer freed a frame's textures before its framebuffers);
4 pipelines failed with "vertex attribute type mismatch" (UBYTE4 TEXCOORD1/2 as `R8G8B8A8_UINT` into
the shaders' float4 inputs: desktop drivers pass the bits, MoltenVK refuses). Build 21: framebuffers
and views are freed before textures (and plume's view keeps its device), and integer formats feeding
float inputs are converted to float4 on the CPU like the other formats hosts cannot read.

Changes go to the public repository with `scripts/ios/sync-source-repo.ps1` (copies this project's
source into `deadrising-ios`, the same file lists as `make-source-repo.ps1`, and refuses anything
made from the game), then commit, push and run the workflow. Tool changes go in through
`patches/`: regenerate a patch with `git -C tools/<tool> diff --ignore-cr-at-eol` (LF line ends)
and check it applies to a clean checkout of the pinned commit.

## How the build works (GitHub Actions, like lm-ios)

Two repositories, made by `scripts/ios/make-source-repo.ps1` and `scripts/ios/publish.ps1`:

| Repository | Visibility | Holds |
|-|-|-|
| `deadrising-ios` | public | source only (this project minus anything from the game); runs `.github/workflows/ios.yml` on a free macOS runner |
| `deadrising-ios-data` | **private** | release `game-data`: your `default.xex` and `ntvX/system/shader.arc`; releases `ios-N`: the IPA and the full build logs |

The workflow (`macos-15`, about 1–2 hours):

1. downloads `default.xex` and `shader.arc` from the private repository (secret
   `GAME_DATA_TOKEN`, variable `GAME_DATA_REPO`);
2. downloads MoltenVK (KhronosGroup release, dynamic `MoltenVK.framework` for `ios-arm64`) and
   builds FFmpeg 7.1 with only the XMA decoders for iOS arm64 (`scripts/ffmpeg/configure.sh`, the
   game's audio);
3. `scripts/ios/codegen.sh`: applies `patches/` to XenonRecomp, XenosRecomp and plume, builds them
   for the Mac, recompiles the game into `ppc/`, unpacks the shader archive, converts the shaders
   (`xfb2xenos`) and translates them to SPIR-V (`XenosRecomp`);
4. builds `runtime/` for iOS (arm64, iOS 16+) as `DeadRising.app` with MoltenVK embedded;
5. zips the IPA and publishes it with the logs as a release of the private repository.

The public log never shows generated game code (messages about `ppc/` are only counted).

Builds reuse what did not change (2026-11-25: a build took about 8 minutes, 4 of them compiling the
same generated code again):

* the generated code (`ppc/`, the translated shaders) is kept as `codegen-<key>.tar.zst` in the
  private data repository's release `build-cache`; the key is a hash of the game files, the two
  recompilers' commits and patches, `codegen.sh`, `unarc.ps1`, `xfb2xenos` and `config/`. On a match
  `codegen.sh` only applies the tools' patches (`DR_CODEGEN_PATCH_ONLY=1`);
* the compiler cache (ccache: every object file by its inputs) is `ccache.tar` in the same release,
  updated after every build;
* FFmpeg (nothing from the game) is in this repository's own Actions cache.

Delete the `build-cache` release to start from scratch; the next build makes it again.

## One-time setup

```powershell
powershell -File scripts\ios\make-source-repo.ps1          # F:\downloads\deadrising-ios
powershell -File scripts\ios\publish.ps1 -GameDir "F:\downloads\Dead Rising"
```

`publish.ps1` creates both repositories under your GitHub account, uploads the two game files to
the private one, pushes the source, asks for the token (fine-grained, repository access: only
`deadrising-ios-data`, permission Contents: read and write), and starts the first build. Later
builds: Actions → iOS IPA → Run workflow.

## Installing

1. Sign and install the IPA with SideStore, AltStore, Sideloadly or LiveContainer. **Keep the
   entitlements** (`DeadRising.entitlements` is in the release): the game reserves the Xbox 360's
   4 GB address space, which iOS only allows with `extended-virtual-addressing`.
2. Copy your whole game folder (`default.xex`, the `arcrom*.arc` files, `ntvX/`, about 6 GB) to
   **Files → On My iPhone → Dead Rising → game**.
3. Start the app. The log (`DRRecomp.log`) and saves are in the same Dead Rising folder.

## Performance on iPhone and iPad (render passes)

Build 21 on an iPhone 15 (2026-11-25): GPU-bound, 9 fps at 2160p and about 27 fps at 1080p, the video
thread waiting 20-30 ms a frame for the GPU. Apple GPUs render in tiles: every render pass loads its
attachments from memory at the start and stores them at the end. A PC GPU pays nothing like it, which is
why the PC ran the same frames at 60 fps. Measured on the PC under Vulkan with what a tile GPU would
move (the `[video]` line every 5 s: "render passes N a frame (a tile-based GPU loads X MB and stores Y
MB)"; `DR_PASS_LOG=<n>` lists the commands of the n-th frame that began passes):

| 1080p, 4X MSAA | passes | loaded | stored |
|-|-|-|-|
| title, before | 103 | 1683 MB | 1683 MB |
| title, build 22 | 75 | 333 MB | 621 MB |
| Security Room, before | 80 | 1375 MB | 1562 MB |
| Security Room, build 22 | 82 | 459 MB | 636 MB |

What build 22 changed (pictures compared with the old path on the title and in the Security Room:
the same):

* plume (Vulkan): setting the same framebuffer again keeps its render pass (each resolve that cleared
  was followed by a second pass for the next draw);
* plume (Vulkan): full clears made while no pass is active become the next pass's CLEAR load
  operations (render pass variants per framebuffer), so the pass loads nothing;
* the renderer keeps the clears that resolves ask for (the game clears each 4X tile behind its
  resolve) until the target is drawn or read, even into the next frame, and makes strips with the same
  value that cover the target one clear (`DR_DEFER_CLEARS=0`: at once, as before);
* passes smaller than their host target (the environment probes' 8x8 to 128x128 faces share one with
  full-screen passes) draw through a framebuffer of their size over the same textures
  (`DR_SIZED_FRAMEBUFFERS=0`: always the whole target; plume `RenderFramebufferDesc::width/height`);
* 2160p is gone on iPhone and iPad (`Config::kMaxResolutionScale`); 720p moves about half of 1080p.

With these, 4X MSAA costs little (1080p: 459 / 636 MB with it, a little less without). Still to look
at: the CPU side (the draw hooks take 7-17 ms a frame on the phone).

Build 22 on the iPhone 15 (720p): the GPU waits 0.2-2 ms a frame now; the game thread is the limit. The draw
hooks (the renderer recording the game's draws) took 15-32 ms a frame there against 3-6 ms on a PC, mostly
"prepare" (14.9 ms a frame on average). Four replays took 1.1-2.1 s, all making new pipelines (MoltenVK compiles
Metal shaders on first use): the game stopped (the cutscene stutter; its clock stood still while the dialogue
played on), and the next frame replaced the one not yet taken, losing its texture uploads (Carlito and the roof
door stayed white). Build 23:

* the game waits up to 30 s (not 1 s) for the video thread to take a frame; a frame dropped all the same makes
  every texture be checked and uploaded again;
* iPhone / iPad: a draw whose pipeline is still being made waits at most 4 ms a frame in all, then is skipped
  until the pipeline is ready (`DR_ASYNC_PIPELINES`: 0 always waits, 1 skips on a PC too);
* the game's threads, the render thread and the GPU command processor ask for the fast cores
  (`os/thread_qos.h`, QoS user-interactive);
* `[cpu]` lines every 10 s on Apple platforms: a sampling profile (every 2 ms) of the game thread, the drawing
  thread, the render thread and the command processor, functions by self and total time
  (`profiler/cpu_sampler.cpp`; `DR_HITCH_PROFILE=0` off). The next step for the draw hooks comes from these.

## Screen scaling and widescreen

Screen Scaling (start screen and Settings; `ScreenScaling` in recomp_config.toml): Fit (the game's
16:9 with black bars), Fill (stretched over the whole screen), Fill 1.943:1 (stretched over a centred
2106 x 1084 frame, the bars beside the Dynamic Island), as in the user's Tunic port.

Real widescreen (a wider view, not a stretched picture) is not done yet: the 3D camera, its frustum
culling and the HUD layout all assume 16:9 and need patching in the game code. The only 16:9 constant
in the image (0x82021BC8) belongs to a video-mode helper (`sub_82572B48`: pixel aspect from the display
information), not the camera.

## Controls and everything that is not the game

On iPhone and iPad everything that is not the game is UIKit and Core Animation on the main thread
(`runtime/src/os/ios_overlay.mm`, after the user's Tunic port), so it never waits for the game's
frames; the game renders on a thread of its own (`gpu/video.cpp`). The renderer draws no menu,
status panel, log, FPS text or touch controls there (`ui/options_menu.cpp` and
`hid/touch_controls.cpp` are PC only).

* **Start screen** (`runtime/ios/Launcher.swift`, SwiftUI): resolution (720p-1440p), screen scaling, frame rate, MSAA, FXAA,
  motion blur, V-Sync, Show FPS; warns when the game files are missing.
* **Touch controls**: the mobile kit's layout on a 1080-high canvas in the safe area. Left stick
  bottom left (optionally floating), D-pad above it, right stick and L3 / R3, A / B / X / Y bottom
  right in the Xbox colours, LB / LT top left, RB / RT top right, Back / Start top centre; haptics.
  Hidden while a controller (Xbox, PlayStation, MFi) is connected.
* **Gear** (top centre, right of Start), **three-finger tap**, **Start + Back** on a controller or
  **F1**: the Settings panel (frame rate, resolution, screen scaling, anti-aliasing, FXAA, motion blur, V-Sync,
  Show FPS, touch controls on / off, opacity, size, fixed / floating stick, vibration). Changes apply
  at once and are saved when it closes. Touch, the on-screen D-pad or a controller move through it.
* **Controller button** (top centre, left of Back): hides / shows the touch controls.
* **Four-finger tap**: the live log (the last 200 lines; only then, never by itself).
* **FPS counter** (Show FPS): a UIKit label in the top-left corner.
* Leaving the app pauses rendering (no GPU work in the background); the game waits.

Touch settings use the Tunic port's `NSUserDefaults` keys (`PortTouchControls`,
`PortTouchOpacity`, `PortTouchSize`, `PortFloatingStick`, `PortHaptics`). On Windows,
`DR_TOUCH=1` still turns on the old renderer-drawn controls with the mouse as a finger
(`DR_TOUCH=2`: always shown).

## Known limits

* Not yet run on a device (see Status); no app icon yet.
* Needs an iPhone or iPad with an A14 chip or newer (Metal 3: buffer device addresses and
  descriptor indexing, which the shaders use through MoltenVK).
