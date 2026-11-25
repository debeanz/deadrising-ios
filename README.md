# Dead Rising (Xbox 360) → iOS static recompilation

Working folder for porting Dead Rising's Xbox 360 `default.xex` to iOS with
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp), following the approach of
[UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp) and its iOS fork
[UnleashedRecompilediOS](https://github.com/DevZer0D4Y/UnleashedRecompilediOS).

The game files stay where they are (one folder up). Nothing here ships game data.
The runtime in `runtime/` reuses UnleashedRecomp code and is GPL-3.0 (`runtime/COPYING`).

## What the game is

| | |
|-|-|
| Executable | `default.xex`, XEX2, retail-encrypted + compressed (XenonUtils decrypts it) |
| Internal name | `Snatcher.pe` |
| Image | base `0x82000000`, size `0x950000`, entry `0x82440710` |
| Code | `.text` `0x82100000–0x827AE7BC`, plus 7 small `.embsec_*` code sections; 25,808 functions |
| XDK | **2.0.2920** (early 2006) – D3D9, D3DX9, XGRAPHC, XAPILIB, XAUD, X3DAUD, XMEDIA, XNET, XONLINE, LIBCMT, all static |
| Kernel imports | **182** functions + 10 variables (xboxkrnl + xam) |
| Title update | none in this dump (`default.xexp` not needed) |
| Data | 1172 `.arc` (MT Framework archives), 85 `.ast`, 22 `.snd`; the game asks for flat names such as `d:\arcromboot_eur.arc` |

Of the 182 imports, UnleashedRecomp has real code for 77; 85 are only logging stubs there (Unleashed
replaces Sonic's XAPI/D3D code before they are reached) and 20 are missing. Dead Rising runs its own
XAPI code, so the runtime implements those 105 itself (`runtime/src/kernel/imports_dr.cpp`).
`config/imports_status.txt` has the full list.

## Milestone 1 – recompilation (done)

```
tools/XenonRecomp      XenonRecomp, branch "deadrising" (changes also in patches/)
tools/UnleashedRecomp  reference: runtime, kernel, GPU, audio
tools/UnleashedRecompilediOS  reference: iOS port (Metal, IPA packaging)
xexinfo/               helper: sections, imports, save/restore helpers, image dump, disassembly
config/DR.toml         XenonRecomp config
config/DR_switch_tables.toml  830 jump tables from XenonAnalyse
ppc/                   generated C++ (105 files, ~134 MB) – regenerate, don't edit
```

Result: **0 unimplemented instructions, 0 bad jump tables, 0 warnings**; every generated file compiles
with clang for x64 and ARM64 (the iOS architecture, through SIMDe).

### Changes made to XenonRecomp (`patches/XenonRecomp-deadrising.patch`)

1. **Jump-table-aware function analysis.** 89 small leaf functions (not in `.pdata`) were cut off at
   their `bctr`. The analyzer now follows jump table cases (keyed by the `lis` that starts each table)
   and accepts the inline table data before them.
2. **Jump table length.** Inline absolute tables stop where their own case code begins (fixes
   `0x823A8A08`, whose bound check lives in a caller).
3. **Jump tables without a bound check** (e.g. indexed by `XGetVideoMode`'s result): taken from the
   inline table when it sits right after the `bctr`. 11 found (`0x82728020` and others).
4. **34 missing instructions** (mostly 16-bit SIMD): `vslh vsrh vsrah vsrab vrlh vspltish vsubshs
   vaddsbs vaddsws vsububm vmaxsh vminsh vavguh vandc vpkswss vpkswus vpkshss vpkuhus vctuxs
   vcfpuxws128 vcmpgtsh[.] vcmpgtsw[.] vcmpequh[.] vsel128 bso bns bsolr cror crorc lhbrx mulhd mulhdu`.
5. **Bug fix:** `vcmpgtuh.` never set cr6.

The new instructions compile but are **not yet checked against hardware results** (XenonTests).

## Milestone 2 – Windows runtime (boots to the title sequence)

`runtime/` is a CMake project (clang-cl) that links the generated code with a kernel layer:

| File | What it does |
|-|-|
| `src/main.cpp` | 4 GB guest memory, XEX decode + load, import variables, crash log, entry |
| `src/kernel/imports.cpp` | UnleashedRecomp's kernel (threads, events, waits, TLS, …) + timed waits, APC delivery |
| `src/kernel/imports_dr.cpp` | Dead Rising's imports: files, virtual memory, printf family, XEX queries, Vd*, XAudio render driver and XMA contexts (`src/apu/`), XAM, network-down |
| `src/kernel/virtual_memory.cpp` | `NtAllocateVirtualMemory` pages at `0x40000000–0x7F000000` |
| `src/kernel/io/file_system_dr.cpp` | `game:`/`d:`/`\Device\Cdrom0` → game folder, cache/hdd → `_recomp_save`, case-insensitive and flattened-name lookup |
| `src/kernel/xam.cpp` | UnleashedRecomp's XAM (content, enumerators, notifications); input = one idle pad |
| `src/gpu/command_processor.cpp` | Minimal Xenos command processor: consumes D3D's PM4 ring buffer (fences, scratch registers, memory writes, interrupts) and fires 60 Hz vblank. **Does not draw.** |

What happens today (`run/DRRecomp.log`):

1. Image decoded and loaded, 10 kernel variables resolved, guest starts at `0x82440710`.
2. D3D creates its device: GPU engines, interrupt callback, ring buffer, first `VdSwap`.
3. The command processor runs D3D's command stream (draws `DRAW_INDX` arrive but are ignored).
4. The game loads `ntvX\system\shader.arc`, `arcromboot_eur.arc`, `arcromareaff9/ffa.arc`,
   `arcromcommon_1st_lang_jpn.arc`, `arcromcommon_effect.arc`, `arcromsystem_common.arc` and
   `arcromeventtitletitle.arc` (the title screen) through real async I/O with completion routines.
5. It keeps presenting frames (`VdSwap` #128+ within 30 s), audio client registered, ~200 XMA contexts.

### Build and run

```
build_tools.bat                 # XenonAnalyse + XenonRecomp (only after changing them)
tools\XenonRecomp\build\XenonAnalyse\XenonAnalyse.exe ..\default.xex config\DR_switch_tables.toml
tools\XenonRecomp\build\XenonRecomp\XenonRecomp.exe config\DR.toml tools\XenonRecomp\XenonUtils\ppc_context.h
build_runtime.bat               # runtime\build\DRRecomp.exe, log in build_runtime.log
powershell -File run_runtime.ps1 -Seconds 30 [-Trace]   # runs it against ..\, log in run\DRRecomp.log
```

If XenonRecomp ever produces fewer `ppc_recomp.N.cpp` files than before, delete the leftover
higher-numbered ones (they cause duplicate-symbol link errors).

`-Trace` (environment variable `DR_TRACE=1`) logs every kernel import call and APC delivery.

Later fixes that let it run without stalls or crashes (60 s+, 1000+ frames, title loop):

* **Vblank status register.** D3D's interrupt handler ignored every vblank because GPU register
  `0x1951` read 0, so the swap-pending flag was never cleared and every frame stalled 200 ms.
  The display registers D3D reads through MMIO are now preset (`gpu/command_processor.cpp`).
* **Divide by zero.** PPC does not trap on division; XenonRecomp now guards `divw/divwu/divd/divdu`.
* **Physical memory aliases.** The console maps its 512 MB at `0xA0000000`, `0xC0000000` and
  `0xE0000000`; D3D writes through one view and reads through another. On Windows the runtime now
  maps one 512 MB section three times (`kernel/memory.cpp`; iOS still needs the same with `vm_remap`).
* The interrupt dispatch sets the CPU number the handler reads from the PCR.

### Known gaps (in order of what blocks the game next)

1. **No renderer.** Nothing is displayed. Work in progress: see Milestone 3.
2. ~~**Audio**~~ done later: `src/apu/audio.cpp` (render driver) and `src/apu/xma.cpp` (XMA, FFmpeg).
3. **Input:** one connected pad that never presses anything.
4. `XexGetProcedureAddress` returns "not found"; `XeCryptSha` is not real SHA-1 (save signing);
   STFS (save containers) and networking report failure.

## Milestone 3 – D3D hooks like UnleashedRecomp (in progress)

**[docs/d3d_map.md](docs/d3d_map.md)** has Dead Rising's D3D device layout and function map with the
evidence for each entry. Mapped: the whole per-draw API (render/sampler-state setter tables,
SetTexture, SetStreamSource, SetIndices, SetVertexDeclaration, Set{Vertex,Pixel}Shader, shader
constants, SetRenderTarget, SetDepthStencilSurface, SetViewport, Clear, Resolve, the indexed draw,
BeginVertices/EndVertices, Present), predicated tiling (BeginTiling/EndTiling), buffer, declaration
and shader creation, and every device field these touch. Dead Rising renders 720p with 4× MSAA in
3 tiles and draws UI through BeginVertices, neither of which Sonic Unleashed does.
Still to find: texture header setup (outside the D3D range), GetBackBuffer, any Lock/Unlock.

### Renderer port – stage 1 (window, hooks)

* `runtime/src/gpu/video.cpp`: SDL2 window + plume device (D3D12, Vulkan fallback) on its own
  thread; each guest Present produces a host frame. The picture is still a placeholder; the window
  title shows live per-frame counts (title scene: ~1,550 draws, 65 vertex batches, 53 resolves,
  ~2,300 texture binds, 30 fps).
* `runtime/src/gpu/d3d_hooks.cpp`: hooks on the mapped D3D functions in "tap" mode – record, then
  run the original – so D3D's own GPU synchronisation keeps working while the host renderer grows.
* Command processor: **predicated tiling** (`SET_BIN_MASK/SELECT`, predicate bit) implemented as
  in Xenia. D3D replays each command buffer per tile; without predication its CPU-callback interrupt
  ran once per tile and D3D reported "Unanticipated CPU_INTERRUPT" and crashed.
* Shaders: `ntvX/system/shader.arc` holds MT Framework **XFB** shader packages
  (`scripts/unarc.ps1` unpacks `.arc` files), built with the 2006 shader compiler (2.0.2638), whose
  containers (`0x102A0Exx`) differ from the 2008 ones XenosRecomp reads (`0x102A11xx`).

* **All shaders translate** – see [docs/shaders.md](docs/shaders.md): `xfb2xenos/` parses the XFB
  packages the way the game's loader does (microcode lives in a separate physical block per
  shader group, not after the container) and converts all 828 shaders (559 PS + 269 VS);
  **all 777 unique shaders translate and compile to DXIL and SPIR-V** with the Dead Rising
  changes to XenosRecomp (`patches/XenosRecomp-deadrising.patch`). Compiling is not yet proof of
  correct output: nothing is drawn with them yet.

* **Shaders hooked in the runtime** – the runtime links the shader cache; hooks on D3D's shader
  init functions identify every shader the game creates (all 828 found, 0 unknown) and host
  shaders are built for all 777 cache entries on D3D12 (DXIL linked at runtime with DXC, as in
  UnleashedRecomp) and on Vulkan (`DR_GPU=vulkan`), 0 failures. `scripts\build_shaders.bat`
  regenerates the shader files.

* **First real draws** – see [docs/renderer.md](docs/renderer.md): the hooks record every draw,
  clear and resolve with its state (vertex declarations, byte-swapped vertex/index data,
  constants, render state decoded from the device's Xenos register shadows) and the video thread
  replays the frame with plume. EDRAM is modelled by base and format, predicated tiling and
  reversed Z are handled, and resolves feed render-to-texture. The title scene renders (untextured,
  white) at 30 fps on D3D12 and Vulkan; `DR_DUMP=N` writes every pass of a frame to BMP files.

* **Title screen renders** – textures are decoded from their GPU fetch constants (untiled,
  endian-swapped, BC1–5 and 8/16/32-bit formats, swizzle as view mapping, Xenos sampler state), so
  the game's own textures and post-processing produce the real Dead Rising title screen at 30 fps.

* **Playable start: title → menus → prologue** – the 3D attract scene, sky, foliage, shadows,
  lightmaps, post-processing and auto exposure render (docs/renderer.md lists the fixes:
  strip resets, packed vertex formats, EDRAM restore through MRT, reversed Z, resolves by format,
  resolve write-back to guest memory). Controller and keyboard input work (SDL), and the XAM
  dialogs complete the way the game expects (storage device, "No saved game found. Begin a new
  game?", XN_SYS_UI notifications), so Start → New Game → 72 Hour Mode reaches the opening
  helicopter cutscene. Physical memory uses a best-fit allocator (the prologue ran out of the
  512 MB with o1heap's power-of-two blocks). Screenshots: `docs/images/`.

  Controls: any SDL game controller (Xbox, PlayStation, MFi), or the keyboard: WASD move, IJKL
  camera, arrows D-pad, Enter Start, Backspace / Tab Back, Space A, E B, Q X, R Y, 1 / 3
  shoulders, Left Shift / F triggers, Ctrl stick clicks. `DR_AUTO_INPUT="700:start,1000:a"`
  presses buttons on given frames for unattended tests (sticks too: `9000:lup*300` holds the
  left stick up for 300 frames).

* **Prologue looks right** – skinned characters (faces, hands, the pilot: bone matrices in a
  vertex texture, copied at present and fetched texel-exact), the D3D9 half-pixel offset (the
  "pixelated" image came from full-screen passes sampling on texel edges), back-face culling (black
  lapel triangles), the HDR 7e3 raw-bit round trip (bloom fringes), XenosRecomp's `SETP_INV`
  (title screen speckle and tint), FXAA in place of the console's 4X MSAA, and 16:9 with black bars
  on other window shapes. Details in docs/renderer.md.

* **Into gameplay** – the corrected time base (`mftb` at the console's 49.875 MHz) fixed the
  cutscene timing, Xbox 360 clear flags and mip/cube-face resolves fixed the helicopter photo
  segment, and caching converted vertex and index data brought gameplay on the mall roof to a
  steady 30 fps. The XAudio render driver runs the game's mixer every 5.33 ms into SDL.

* **Sound, sharp text, motion blur** – XMA audio (FFmpeg's XMA2 decoder built by
  `scripts\build_ffmpeg_win.bat`, the console's context handling after Xenia): music, effects and
  voices play. Decoded textures carry their mip chains (Xenos layout, packed small levels), which
  fixed jagged text and shimmering distance. Stencil (depth buffers are D32 + S8) lets the game's
  camera motion blur leave the moving characters out, as on the console. Tutorial pop-ups lost a stray
  diagonal line (line strips with restart indices). Controller rumble (`DR_NO_RUMBLE=1` off).
  Details in docs/renderer.md.
* **Smooth edges, sound driven like the console** – the 3D scene renders with the console's 4X MSAA
  (`DR_MSAA=0` off). The XMA decoder follows the game's
  kick / lock / clear registers (stores marked MMIO reach the runtime through `ppc_detail.h`) and
  only writes back what it changed. The texture cache checks every page of a texture (mip levels
  included), not a sample of its base. `DR_TEX_WATCH=1` reports texture memory that changes.

Next: the rest of the mall, and the iOS build (docs/ios.md). F12 captures a frame of anything that looks wrong.

Profiling build (wraps every recompiled function with a call counter):

```
bash scripts/gen_profiler.sh          # after every recompile
build_profile.bat                     # runtime\build_profile\DRRecomp.exe
powershell -File run_runtime.ps1 -Seconds 40 -Profile   # run\call_profile.csv, run\d3d_device.bin
bash scripts/fninfo.sh 8245BA18       # size, callees (imports named), device fields of a function
```

## Leads (verified at runtime)

| Address | Role |
|-|-|
| `0x8244A608` | D3D graphics interrupt callback (data `0xA9200800` = D3D device) |
| `sub_8244F020` / `sub_82727C08` | D3D device creation path (`VdInitializeEngines`, ring buffer) |
| `sub_8245BA18` | Present → `VdSwap` |
| `0x8256F6E0` | XAudio render driver callback |
| `0x825460E8` | XAPI async I/O completion routine (APC) |
| `0x82728020` | video-mode switch (640×480 / 1280×720 / 1920×1080 …) |

## Roadmap

1. ~~Recompile~~ ✔  ~~Runtime that boots on Windows~~ ✔ (title loop, no picture)
2. **Graphics** – chosen: hook D3D like UnleashedRecomp (Milestone 3, docs/d3d_map.md). The alternatives were:
   - *Hook D3D like UnleashedRecomp* (~40 D3D functions to find in this 2006 D3D build; fast, needs
     MT Framework/D3D reverse engineering), or
   - *Translate PM4 like Xenia* (the command processor above is the start; generic, more work in the
     GPU backend, slower). Either way shaders go through XenosRecomp, and the backend is plume (Metal
     on iOS, as in UnleashedRecompilediOS).
3. ~~**Audio**~~ ✔ – the render driver callback `0x8256F6E0` runs every 5.33 ms; XMA through FFmpeg.
4. **Input, saves, settings** – SDL gamepad, `XamContent*` save folder, touch controls on iOS.
5. **iOS** – prepared (docs/ios.md): Vulkan through MoltenVK (the Vulkan path works on Windows),
   POSIX guest memory with the aliased physical views, SDL on the main thread, the app bundle with
   the extended-memory entitlements, and a GitHub Actions build (public source repository +
   private repository with your default.xex and shader.arc, as lm-ios), on-screen touch controls
   (tested on Windows with `DR_TOUCH=2`, D3D12 and Vulkan). Not compiled for iOS yet.
