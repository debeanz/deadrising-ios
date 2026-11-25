# Dead Rising – Direct3D map (XDK 2.0.2920)

Goal: replace Dead Rising's statically linked Direct3D the way UnleashedRecomp does (`gpu/video.cpp`):
a fake device whose memory layout matches what the game's *inlined* D3D code reads and writes, plus
host versions of the D3D functions the game calls. This file records what is known and how it was
found.

Confidence: **confirmed** = seen at runtime *and* in the code; **likely** = strong evidence, one check
left; **lead** = candidate only.

## How the evidence was gathered

* Call profiler (`DR_CALL_PROFILER`, `scripts/gen_profiler.sh`, `build_profile.bat`,
  `run_runtime.ps1 -Profile`): every recompiled function is wrapped. It counts calls, notes when
  `r3` is the D3D device (`0xA9200800`), and for D3D-range functions keeps up to 6 distinct calls
  with arguments, the data behind pointer arguments and the return value.
  Data of the 60 s run used below: `analysis/profile2/` (`call_samples.txt`, `call_profile.csv`,
  `d3d_device.bin`; ~570 frames).
* `analysis/text.dis` (full `.text` disassembly), `scripts/fninfo.sh ADDR`, `scripts/containing.sh ADDR`.
* Argument shapes compared with UnleashedRecomp's hook signatures (API-level, same across XDKs).

## Resource objects

Every D3D object starts with a 0x18-byte common header; the low bits of the first word are the type:

| Type | Object | Seen as |
|-|-|-|
| 1 | vertex buffer (`0x00200001`) | fetch constant (2 dwords) at `+0x18`: GPU address, size at `+0x1C` (SetStreamSource reads them) |
| 2 | index buffer (`0x20000002`; bit 29 = format flag) | |
| 3 | texture (front buffer `0xA1000884`) | 6-dword fetch constant template at `+0x1C…+0x30` (SetTexture merges it) |
| 4 | surface (render targets, depth) | |
| 5 | vertex declaration (`0x00100005`, kept in device memory) | |
| 6 | vertex shader | |
| 7 | pixel shader | |

Dead Rising (MT Framework) mostly builds resources **in place** in its own memory and calls init
functions, not `Create*`; textures are set up outside the D3D range (XGraphics helpers – not found
yet). A renderer can therefore also create host resources lazily from these headers the first time
they are bound.

## Device layout (`GuestDevice` for this D3D build)

| Offset | Contents | Evidence | Unleashed (2008 D3D) |
|-|-|-|-|
| `0x000–0x037` | dirty flags, 7 × u64 | setters OR bits into `+0x10`/`+0x18` | 8 × u64 |
| `0x038–0x1B7` | **render-state setter table**, entry at `0x38 + D3DRS` | **confirmed** (init loop + D3D defaults; `analysis/render_states.md`) | `0x040 + D3DRS` |
| `0x1B8–0x207` | **sampler-state setter table**, 0x14 entries | **confirmed** | `0x1D4` |
| `0x208–0x22F` | 10 × `0x821421E0` | unknown | – |
| `0x230–0x3D7` | second table of D3D functions (likely getters) | lead | – |
| `0x400–0x6FF` | fetch constants: textures at `0x400 + sampler × 0x18`; vertex fetch for stream *n* at `0x670 + (17 − n) × 8` | **confirmed** (SetTexture, SetStreamSource) | `0x480` |
| `0x700–0x16FF` | VS float constants | **confirmed** (SetVertexShaderConstantF: `+0x700 + reg × 16`) | `0x780` |
| `0x1700–0x26FF` | PS float constants | **confirmed** (SetPixelShaderConstantF: `+0x1700 + reg × 16`) | `0x1780` |
| `0x2700` / `0x2710` | VS / PS bool constants | **confirmed** for PS (SetPixelShaderConstantB) | `0x2780/0x2790` |
| `0x2800–0x2AA4` | Xenos register shadows (`0x2800` RB_SURFACE_INFO, `0x2888–0x289C` viewport transform, …) | **confirmed** | – |
| `0x2A10` / `0x2A14` | GPU-shared blocks (read-pointer write-back, interrupt block) | **confirmed** | – |
| `0x2A38` | ring buffer base | **confirmed** | – |
| `0x2C34` | → deferred-callback queue (`device+0x2A34`) | **confirmed** | – |
| `0x2D18` | **vertex declaration** | **confirmed** (SetVertexDeclaration) | `0x2E2C` |
| `0x2F8C` | **index buffer** | **confirmed** (SetIndices; DrawIndexed reads its address at +0x18, bit 31 of the common word = 32-bit indices) | – |
| `0x2F90–0x2F9F` | **render targets 0–3** | **confirmed** (SetRenderTarget, Resolve) | – |
| `0x2FA0` | **depth-stencil surface** | **confirmed** (SetDepthStencilSurface writes it; Resolve with source index 4 reads it) | – |
| `0x2FA4 + n × 4` | **vertex buffer of stream n** | **confirmed** (SetStreamSource) | – |
| `0x2FE8 + n` | stride of stream n, in dwords | **confirmed** | – |
| `0x2FF8 + s × 4` | **texture of sampler s** | **confirmed** (SetTexture) | – |
| `0x3060–0x3077` | `D3DVIEWPORT9` {X, Y, Width, Height: u32; MinZ, MaxZ: float} | **confirmed** (SetViewport) | `0x3168`, all floats |
| `0x3088` | **pixel shader** | **confirmed** (SetPixelShader; reads the container at object + 0x38) | – |
| `0x3364` | **vertex shader** | **confirmed** (SetVertexShader) | – |
| `0x3B10–0x3B20` | vblank callback, vblank counter, swap interval countdown | **confirmed** | – |

## Functions

### Per-draw API (all **confirmed** unless noted)

| Address | Function | Evidence |
|-|-|-|
| `sub_8245BA18` | Present / Swap | calls `VdSwap`, `VdGetSystemCommandBuffer`, `VdPersistDisplay`; once per frame |
| `sub_82446C78` | SetViewport | writes viewport transform shadows + `D3DVIEWPORT9` at `0x3060` |
| `sub_82447A00` → `sub_82447440` | SetRenderTarget(dev, index, surface) | public entry is a 1-instruction jump; called at 4 sites with index 0–3, slot `0x2F90 + 4i` |
| `sub_82447748` | SetDepthStencilSurface(dev, surface) | slot `0x2F9C`; surfaces of type 4 |
| `sub_82449860` | SetTexture(dev, sampler, texture, dirtyMask) | merges the texture's fetch constant into `0x400 + 0x18·s`, stores at `0x2FF8 + 4s` |
| `sub_82446EA0` | SetStreamSource(dev, stream, vb, offset, stride) | builds the vertex fetch constant, stores VB and stride |
| `sub_82447050` | SetIndices(dev, ib) | argument objects of type 2 |
| `sub_82452840` | SetVertexDeclaration(dev, decl) | stores at `0x2D18`, dirty bit 12 |
| `sub_82452838` → `sub_824526E8` | SetVertexShader(dev, vs) | stores at `0x3364`; objects of type 6 |
| `sub_824524F8` | SetPixelShader(dev, ps) | **confirmed**: stores at `0x3088`, dirty bits 49 + 52; objects of type 7 |
| `sub_824520D8` | SetVertexShaderConstantF(dev, start, data, count) | destination `+0x700 + 16·start` |
| `sub_824521E8` | SetPixelShaderConstantF(dev, start, data, count) | destination `+0x1700 + 16·start` |
| `sub_82452350` | SetPixelShaderConstantB(dev, start, data, count) | bit words at `+0x2710` |
| `0x82444xxx–0x82446xxx` | SetRenderState / SetSamplerState (88 setters) | `analysis/render_states.md` |
| `sub_82549E20` | DrawIndexed(dev, prim, baseVertex, startIndex, indexCount) | **confirmed**: base vertex → VGT_INDX_OFFSET, index buffer from `0x2F8C`, writes `DRAW_INDX` |
| `sub_82549008` | BeginVertices(dev, prim, count, stride) → pointer | **confirmed**: the game writes the vertices there (inside the command buffer); prim 8 (RECTLIST) / 6 |
| `sub_825494D8` | BeginIndexedVertices | **likely**: same shape, indexed |
| `sub_825494B8` | EndVertices / EndIndexedVertices | **likely**: called exactly as often as both Begins together (16,498 + 5,469 = 21,967) |
| `sub_824593C8` → `sub_82458B10` | Clear(dev, count, rects, flags, color, z, stencil) | flags `0x31/0x1F/0x3F` (target/z/stencil), tile-sized rects; draws a rect |
| `sub_8245AA30` | Resolve(dev, flags, srcRect, destTexture, destPoint, level, slice, clearColor, clearZ) | **confirmed**: flags bits 0–2 = source RT 0–3 or 4 = depth (`0x2FA0`), 0x100 / 0x200 clear colour / depth afterwards; tile resolves use tile-local rects and the tile position as destPoint |
| `sub_825483F0` | BeginTiling(dev, flags, count = 3, rects, clearColor, …) | **likely**: 3 tiles = 256 + 256 + 208 lines |
| `sub_825488B8` | EndTiling | **likely**: called as often as BeginTiling, right after the scene |
| `sub_82548290` | SetPredication | lead: 22/frame, tile masks 0/3, writes the command buffer |

### Resources

| Address | Function | Evidence |
|-|-|-|
| `sub_8254AF68` | vertex buffer init (length, usage, …, object) → object | returns type-1 object |
| `sub_8254B000` → `sub_8244A348` | index buffer init (length, …, object); attach data | returns type-2 object |
| `sub_82452860` | CreateVertexDeclaration(elements) → decl | **confirmed**: 12-byte D3DVERTEXELEMENT9 (stream u16, offset u16, type u32, method, usage, usage index, pad), 12 declarations in total, cached in device memory |
| `sub_82452620` | vertex shader init(object, microcode) | **confirmed**: clears a 0x250-byte object in place (type 6); the 2006 container follows it; called by the XFB loader `sub_82770DC8` (docs/shaders.md) |
| `sub_82452498` | pixel shader init(object, microcode) | **confirmed**: clears a 0x38-byte object (type 7), stores the microcode address at +0x18; container follows the object |
| `sub_82449760` | GetSurfaceDesc(surface, desc) | lead |
| `sub_8254B090` | render-target size for (width, height, format, multisample) | lead: 1280×256, multisample 2 (4× MSAA) |
| `sub_8254B5A8` | surface update (37k calls, same sites as above) | lead |

Textures: created outside the sampled ranges – find the XGraphics header setup (`XGSetTextureHeader`
equivalent) or create host textures lazily from the fetch constant on first SetTexture.

### Infrastructure

| Address | Function | Evidence |
|-|-|-|
| `sub_8244F020` ← `sub_82727C08` | device creation ← game wrapper | **likely**: only path to `VdInitializeEngines` / `VdInitializeRingBuffer` |
| `sub_8244A608` | graphics interrupt handler | **confirmed** |
| `sub_82457FE0` | deferred-callback thread | **confirmed** |
| `sub_8244FA00` | PM4 packet writer | **likely** |
| `sub_8244FDA0`, `sub_824508E8` | VS / PS constant upload at draw time | **confirmed** |
| `sub_8244B688` | command buffer reserve / kick-off | lead |
| `sub_82452940` | (not Clear – called with count 0 at device init, at frame start) | lead |

## Object layouts used by the renderer

* Vertex buffer (type 1): its vertex fetch constant at +0x18 (address | 3) and +0x1C (size in dwords in
  bits 2–25, endian bits 0–1). SetStreamSource adds the stream offset in the device copy at
  `0x670 + (17 − n)·8` (physical address, +0x1000 for 0xE0000000+ pages) and stores stride/4 at `0x2FE8 + n`.
* Index buffer (type 2): address at +0x18; bit 31 of the common word = 32-bit indices, data is
  big-endian (16-bit swap).
* Texture (type 3): GPU texture fetch constant at +0x1C (dword1 = format | base address,
  dword2 = width − 1 | (height − 1) << 13).
* Surface (type 4): +0x24 = (width − 1) << 18 | (height − 1) << 3, +0x28 = format.
* Device register shadows: see [renderer.md](renderer.md) (seven blocks, written by `sub_8244FA00`).

## What the rendering looks like

Dead Rising renders 720p with **4× MSAA through predicated tiling** (3 horizontal tiles), resolving
each tile into the front buffer, and draws its 2D/UI geometry with BeginVertices/EndVertices (vertex
data written straight into the command buffer). Neither is in Sonic Unleashed, so the host renderer
needs: tiling functions as no-ops around a single full-resolution pass, a resolve that copies the
whole render target once, and a BeginVertices that hands the game a guest-memory scratch buffer.

## Things the runtime had to emulate for D3D to run (done)

* GPU register reads through MMIO `0x7FC80000`: `0x1951` (vblank status) = 1, `0x194C`/`0x1961`.
* Physical memory visible at `0xA0000000`, `0xC0000000` and `0xE0000000` as real aliases.
* The graphics interrupt runs with the right CPU number in the PCR (`r13+0x10C`).

## Hook plan

1. Port UnleashedRecomp's `gpu/` (plume renderer, render queue, shader cache) as `runtime/src/gpu`;
   rewrite its `GuestDevice` with the layout above. The fields Unleashed reads at draw time all have a
   Dead Rising equivalent: render states via the setter table, sampler/fetch constants at `0x400`,
   shader constants at `0x700`/`0x1700`, bools at `0x2700`/`0x2710`, declaration `0x2D18`, viewport
   `0x3060` (ints, not floats), streams `0x2FA4`, textures `0x2FF8`, targets `0x2F90`.
2. Replace device creation (`sub_8244F020`): allocate the fake device, fill the setter tables at
   `0x38` and `0x1B8` with host functions (as Unleashed's `CreateDevice`).
3. Hook everything in the per-draw and resource tables above at once (the original functions share
   the command buffer with Present, so D3D cannot be half replaced), plus BeginTiling/EndTiling/
   SetPredication as no-ops.
4. Shaders: hook the two shader init functions to collect microcode, and feed it to XenosRecomp
   (offline cache like Unleashed's `shader.ar`, built from the game's `.arc` files and
   `ntvX/system/shader.arc`).
5. Still to find before step 3: texture header setup, any Lock/Unlock the game uses (none seen in
   the D3D range so far – MT Framework writes resource memory directly), GetBackBuffer.
