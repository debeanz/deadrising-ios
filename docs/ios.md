# iPhone / iPad build

Dead Rising runs on iOS the same way it runs on Windows: the game's code is recompiled from your
`default.xex`, its shaders are translated from your `shader.arc`, and the runtime replays the
game's Direct3D on the phone's GPU. On iOS the renderer uses Vulkan through **MoltenVK** (Vulkan on
Metal), the same Vulkan path that runs on Windows with `DR_GPU=vulkan`, so the SPIR-V shader cache
is shared.

Status: **prepared, not built yet.** The Windows build runs the game to the prologue on both D3D12
and Vulkan; the iOS-specific code (memory, window, MoltenVK loading, main thread, paths) is written
but has never been compiled for iOS. The first GitHub Actions run will show what still needs
fixing.

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

* Never compiled for iOS yet (see Status).
* Needs an iPhone or iPad with an A14 chip or newer (Metal 3: buffer device addresses and
  descriptor indexing, which the shaders use through MoltenVK).
