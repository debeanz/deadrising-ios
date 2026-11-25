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

## Controls

On-screen touch controls (`runtime/src/hid/touch_controls.cpp`, drawn by the renderer over the
game; screenshot `docs/images/touch_controls.png`): left stick bottom left, D-pad next to it,
A / B / X / Y bottom right in the Xbox colours, LB / LT top left, RB / RT top right, Back / Start
top centre; dragging anywhere else on the right half turns the camera. They hide while a game
controller (Xbox, PlayStation, MFi) is connected and come back when the screen is touched. On
Windows, `DR_TOUCH=1` turns them on with the mouse as a finger (`DR_TOUCH=2`: always shown).

## Known limits

* Not yet run on a device (see Status); no app icon yet.
* Needs an iPhone or iPad with an A14 chip or newer (Metal 3: buffer device addresses and
  descriptor indexing, which the shaders use through MoltenVK).
