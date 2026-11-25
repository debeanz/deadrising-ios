# Host renderer (stage 1)

`runtime/src/gpu/renderer.cpp`. Dead Rising's own D3D still runs (tap hooks, `gpu/d3d_hooks.cpp`);
the renderer records what each call needs and replays the frame on the host with plume
(D3D12 or Vulkan; `DR_GPU=vulkan`). Status: **the title, the 3D attract scene, the menus and the
prologue (helicopter cutscene) render**, at 30 fps: lighting, lightmaps, shadows, sky, foliage,
the game's post-processing and auto exposure. Screenshots in `docs/images/`. Known problems: the
bloom shows coloured fringes (cyan / green / yellow) around bright areas, some red / green
speckle on banners, parts of characters' faces are missing in cutscenes.

## Recording (guest thread)

Each hook reads the D3D device (`0xA9200800`, layout in [d3d_map.md](d3d_map.md)) and appends a
command plus its data to the frame. Present hands the frame to the video thread.

* **Draws** - `DrawIndexed(device, primitive, baseVertex, startIndex, indexCount)` and
  `BeginVertices(device, primitive, count, stride)` → pointer / `EndVertices`:
  * shaders: device + 0x3364 / + 0x3088 → shader cache entries (`gpu/shaders.cpp`);
  * vertex declaration: device + 0x2D18, parsed when `CreateVertexDeclaration` (`sub_82452860`)
    returns it. Dead Rising's 12 declarations use FLOAT2/3, D3DCOLOR, USHORT4N (positions),
    UBYTE4(N), DEC3N (normals), FLOAT16_2;
  * vertex data: stream n's buffer object at device + 0x2FA4 + 4n holds its vertex fetch
    constant (address | 3, size in dwords << 2); SetStreamSource folds the stream offset into
    the device's fetch constant at 0x670 + (17 − n)·8; stride/4 at 0x2FE8 + n. The used vertex
    range is copied and **byte-swapped per element** (16-bit components per 16 bits, the rest per
    32 bits), so no shader-side reordering is needed;
  * indices: index buffer at device + 0x2F8C (address at +0x18, bit 31 of the common word =
    32-bit), copied, swapped and rebased; quad lists and fans become triangle lists; RECTLISTs
    (UI) get their fourth corner computed on the CPU;
  * render state from the **Xenos register shadows** (below): depth, blend, colour mask, cull,
    alpha test (as XenosRecomp's specialization constant), plus the D3DVIEWPORT9 at 0x3060;
  * constants: VS 256 / PS 224 float4 (device + 0x700 / + 0x1700, swapped, deduplicated),
    booleans (0x2700 / 0x2710 → `g_Booleans`), texture descriptor indices.
* **Clear**, **Resolve** (`device, flags, srcRect, destTexture, destPoint, level, slice,
  clearColor, clearZ`; flags: bits 0–2 source RT 0–3 / 4 = depth, 0x100 / 0x200 clear colour /
  depth afterwards).

### Register shadows

The device keeps Xenos register blocks that `sub_8244FA00` streams into the command buffer
(register base + k ← device field + 4k): 0x2000 ← 0x2800, 0x2100 ← 0x284C, 0x2180 ← 0x28A0,
0x2200 ← 0x28B4, 0x2280 ← 0x28E4, 0x2300 ← 0x2938, 0x2380 ← 0x29D0; also VS/PS constants
0x4000/0x4400 ← 0x700/0x1700 and booleans 0x4900 ← 0x2700. Checked against the render-state
setters (ZENABLE → RB_DEPTHCONTROL, CULLMODE → PA_SU_SC_MODE_CNTL, …), so render state is decoded
with Xenia's register semantics.

## Replay (video thread)

* Pipeline layout, bindless texture/sampler heaps and constant binding as UnleashedRecomp
  (root CBVs on D3D12, buffer addresses in push constants on Vulkan); pipelines cached by state.
* **EDRAM model**: colour buffers by (EDRAM base, format class), depth buffers by EDRAM base -
  the game draws and resolves through different surface objects over the same EDRAM, and
  re-declares its HDR scene (`2_10_10_10_FLOAT`) as `2_10_10_10` to resolve it, so the 10:10:10:2
  formats share one RGBA16F buffer.
* **Predicated tiling**: the host renders the whole screen at once; a tile resolve (source rect
  inside the tile-sized surface, bigger destination) reads the screen area at its destination
  point.
* **Resolves** copy into host textures by guest address; draws that sample those addresses get
  them (render-to-texture works for post-processing inputs).
* **Reversed Z**: the scene uses an inverted viewport depth range (MinZ 1, MaxZ 0, Xenos ZSCALE −1)
  with GEQUAL and clears to 0. The host keeps depth = 1 − guest depth (normal range, mirrored
  comparison, mirrored clears), which every backend supports.
* **Textures** (`gpu/texture_decode.cpp`): the texture object bound to each sampler (device +
  0x2FF8 + 4s) holds its GPU fetch constant (virtual base address, format, size, pitch, tiling,
  endianness, swizzle, signs). The base level is untiled (Xenia's tiled layout), endian-swapped and
  uploaded as BC1-5 / 8-, 16-, 32-bit formats (565, 1555, 4444, 2_10_10_10, DXT3A and CTX1
  converted); the fetch constant's swizzle becomes the view's component mapping. Decoded once per
  fetch constant, re-uploaded when a sample hash of the data changes. Samplers come from the
  device's copy of the fetch constant (clamp modes, filters, anisotropy). The title screen uses 176
  textures (DXT1/DXT5/DXT5A/CTX1/8888), all supported.
* The window shows the game's final image (the frame's last full-screen resolve); `DR_SHOW=scene`
  shows the HDR scene as the tiled resolves copied it out instead.

## A Dead Rising frame (title screen)

shadow map (depth only, 1024×1024, resolved) → luminance downsample chain (16×16 … into 128×128
ping-pong textures) → scene: depth prepass + colour pass, 3 tiles of 1280×256/256/208, HDR
`2_10_10_10_FLOAT`, depth resolved, colour resolved into the front buffer texture → bloom at
640×360 → tone mapping / final passes into the 8-bit front buffer. ~1,550 draws, 53 resolves.

## What the 3D scene needed (all fixed)

* **Strip resets**: the world is triangle strips joined by reset indices (0xFFFF). They used to be
  replaced by a real vertex, which made fans of huge triangles (smeared textures, z-fighting,
  black). Strips are now split at resets into triangle lists (winding kept per strip).
* **Packed vertex formats**: lightmap UVs (`TEXCOORD2`) are 10_11_11, normals / tangents
  DEC3N or signed bytes (type bit 8 = signed, bit 9 = integer). Formats the host cannot read, and
  anything non-float feeding the uint4 NORMAL/TANGENT inputs, are decoded to float4 on the CPU
  into an extra vertex stream (slot 8 + stream).
* **Render state after the flush**: D3D computes some registers (EDRAM mode, colour mask, depth
  control) when it flushes dirty state inside the draw, so DrawIndexed is recorded after the
  original runs (the sky was drawn with the prepass's "depth only" mode before).
* **EDRAM restore through MRT**: the scene is drawn once without MSAA, resolved (colour and
  depth), then restored into the 4X MSAA tiled pass by a full-screen quad that writes colour
  through render target 1 (a fixed-point view aliasing the HDR buffer) and depth through oDepth.
  The pixel shader's exports are in the shader cache (`pixelOutputs`); when written targets alias
  render target 0's EDRAM buffer, `SPEC_MRT_ALIAS` copies the highest export into oC0.
* **Reversed Z without mirroring**: inverted viewport ranges are handled in the vertex shader
  (`SPEC_FLIP_Z`: z' = w - z, range swapped), so the depth buffer holds the game's own values
  (oDepth, clears and depth resolves need nothing special). Depth resolves are on.
* **Resolve textures by format**: the game resolves into the same address in several formats per
  frame (HDR scene, then 8-bit post-processing). Host textures and descriptors are per (address,
  format class, host format), and draws sample the one resolved into last at that point of the
  recording (`g_latestResolve`); descriptors never change while a frame uses them.
* **Alpha to coverage** (foliage, glass, `RB_COLORCONTROL` bit 4) is an alpha test at 0.5 on the
  host (hardware alpha to coverage with `DR_A2C=1`; see the MSAA section for why).
* **Resolve write-back**: the game copies its luminance downsample on the CPU into textures the
  exposure pass samples. Small 8-bit resolves (≤ 256×256) are read back and written into guest
  memory in the texture's tiled layout and byte order (`QueueWriteback`), a frame late.
* **BeginIndexedVertices** batches are drawn (vertex and index data inline in the command
  buffer, pointers at device + 0x3328 / 0x332C).
* **HDR raw bits (7e3)**: resolves from the fixed-point view (formats 2/10) encode the host's
  float values back to 7e3 bits; draws into that view decode them (`SPEC_DECODE_7E3`).

## Characters, sharpness and grading (fixed later)

* **Skinned meshes (faces, hands, the pilot)** read their bone matrices from a 768×128
  `32_32_32_32_FLOAT` vertex texture (fetch constant 16, sometimes set straight through the
  fetch constant with no texture object, physical address). The CPU fills it while it builds the
  frame (double-buffered), so vertex textures are copied when the frame is presented and uploaded
  ahead of its draws (`FlushDynamicTextures`). The shaders fetch rows at texel *edges* with point
  sampling; sampled on the host that picked neighbouring texels (wrong matrix rows, meshes thrown
  off screen), so point-sampled vertex fetches load the texel directly (`Load`). Vertex fetches
  whose fetch constant filters keep sampling (bit 31 of the sampler index).
* **Half-pixel offset**: `PA_SU_VTX_CNTL.PIX_CENTER` is 0 (D3D9: pixel k sampled at k), so every
  draw moves half a pixel (`g_HalfPixelOffset = (1/w, -1/h)`). Without it full-screen passes
  sampled exactly on texel edges: point-sampled copies picked neighbours and the game's 4×4 EDRAM
  pixel shuffle (pixel shaders F3F97E7B / E52354E8, an inverse pair around the MSAA restore)
  stopped cancelling out — the "pixelated, blocky" image.
* **Culling**: `PA_SU_SC_MODE_CNTL` (bit 0 front, bit 1 back, bit 2 clockwise front). Drawing both
  sides showed the jacket lining (black lapel triangles) and hair card backs.
* **Predicate counter (XenosRecomp)**: `SETP_INV` turns 1 into 0; it kept 1, so nested branches
  (the title screen's RGB → HSV grading) took wrong paths — red/green speckle, green tint.
* **Vertex position `precise`** in every vertex shader (no fused multiply-add reordering between
  the depth pre-pass and colour-pass shaders), `tx_coord_denorm` fetches handled (none in DR).
* **Presentation**: FXAA on the final image (`DR_AA=0` turns it off; it smooths what MSAA does not
  cover: post-processing and the UI),
  16:9 kept with black bars on other window shapes (phones), clamped linear sampler.

## Photo segment, timing and speed (fixed after that)

* **Clear flags**: the Xbox 360's D3DCLEAR bits are 0-3 render targets 0-3, 0x10 depth, 0x20
  stencil (read from Clear, `sub_82458B10`), not PC D3D9's 1 / 2 / 4. The main pass's clear (0x31)
  never cleared depth and the post-processing clears (0xF, z 1) filled it with "nearest": in the
  helicopter photo segment most of the town failed the depth test (black).
* **Resolves into mip levels and cube faces**: Resolve passes a level and a face (r8, r9). The
  game renders environment cube maps face by face and level by level (128 ... 8); resolve
  textures now have the destination's mip chain and six faces, with matching views (cube views
  bound as cube textures).
* **Vertex shader registers are `precise`** (all temporaries, not only oPos): DXC only traced
  oPos partway back for SPIR-V, and the Vulkan driver fused the rest differently per pass.
* **Time base (XenonRecomp)**: `mftb` reads a 49.875 MHz counter derived from the steady clock
  (`PPC_QUERY_TIMEBASE`), the frequency KeQueryPerformanceFrequency reports. It was the host's
  cycle counter (x86: ~70x fast, ARM: half speed), which broke cutscene timing.
* **Recording cost**: converted vertex streams and processed index lists are cached (by source
  range and declaration / primitive, checked with a hash of the guest data); a mesh drawn again in
  the same frame reuses the frame data. The guest thread's share fell from ~25 to ~3-6 ms a frame
  and gameplay holds 30 fps. Texture descriptors are recycled (textures unused for 20 s are
  evicted, their host copies freed).
* The 5 s log line `timing per frame` splits the frame: guest thread (and how much of it waits for
  the video thread or runs renderer hooks), replay, GPU; `hook stages` splits the hooks.

## Audio, mip levels and text (fixed later still)

* **XMA audio**: `apu/xma.cpp` services the game's XMA contexts like the console's decoder (Xenia's
  context layout: two input buffers of 2 KB packets, loops, an output ring of 256-byte blocks of
  big-endian 16-bit samples) and decodes with FFmpeg's XMA2 decoder, packet by packet.
  `scripts\build_ffmpeg_win.bat` builds just that decoder (FFmpeg 7.1, `--disable-everything
  --enable-decoder=xma1,xma2`; `scripts/ffmpeg/`: Windows has no make, so a CMake file compiles
  the files FFmpeg's own Makefiles list for that configuration) into `deps/ffmpeg-win`, which
  `build_runtime.bat` passes as `DR_FFMPEG_DIR`. The iOS workflow builds the same configuration with
  make. `apu/audio.cpp` downmixes the game's 5.1 mix to stereo (ITU: centre and surrounds at -3 dB).
  `DR_AUDIO_DUMP=<file>` writes the output (48 kHz stereo float) for checking:
  `ffmpeg -f f32le -ar 48000 -ac 2 -i <file> -lavfi showspectrumpic=s=1600x600 spec.png`.
* **XMA registers**: the game's XAudio drives the decoder through the APU's registers at
  `0x7FEA0000`: it reads the context array's physical address once (register 0x600, `lwbrx`) and
  numbers its contexts by their offset in it; kick (0x650+, `sub_8255B020`), lock (0x690+,
  `sub_8255AD58`) and clear (0x6A0+, `sub_8255AC88` right after creating a context) take one bit
  per context. The runtime now owns that array (320 contexts, `Xma::Init` before the game starts),
  hands contexts out of it in XMACreateContext, and decodes only kicked contexts; lock waits for a
  pass in progress, clear resets the buffers. Stores XenonRecomp marks as MMIO (followed by
  `eieio`) reach the runtime through `src/ppc_detail/ppc_detail.h` (XenonRecomp's
  `PPC_INCLUDE_DETAIL` hook): `PPC_MM_STORE_U32` still stores, then calls `DrMmioWrite32`.
  Before, every context with a valid output was decoded whether the game had started or stopped
  it, and each pass wrote all 64 bytes back, which could undo the game's own updates of a live
  context (new buffer pointers). A pass now writes back only the bits it changed (atomically per
  dword). The log has a line every 30 s with kick / lock / clear counts. `DR_XMA_LEGACY=1` restores
  the old behaviour for comparison.
* **Cutscene audio (multi-stream XMA)**: cutscene tracks (`sound\event\evmNN`, rSoundAst, "ASTB")
  are 5.1 made of six mono XMA streams (48 kHz, one context each) interleaved in one block; each
  packet's skip count leads to that stream's next packet. A skip past the end of an input buffer
  carries into the next one (streams 1-5 continued 1, 2, 3, 16, 17 packets into it); the decoder
  restarted every stream at packet 0, which desynchronised five of six and stalled the game's
  whole cutscene mix for half a minute (0 kicks, digital silence, dialog late or missing). The
  overshoot is now the read offset in the next buffer, and a swap the decoder makes itself no
  longer flushes FFmpeg as if a new stream had started. `DR_XMA_STREAMS=1` logs each stream's
  packets / samples / peak and the carries; `DR_AUDIO_DUMP=<file>` with `DR_SHOT` logs the audio
  sample at each shot (`[sync]`), to line subtitles up with the recording.
* **Stalls**: `[hitch]` log lines (frames over 50 ms, split into game work and waiting for the
  video thread). Game time stands still in a stall (fixed step) while audio plays on, so a stall
  at a cutscene's start leaves the voices early by that much for the rest of the scene.
* The game's own debug output (`sub_82531E68`, printf-style) is logged as `[game]`.
* **Texture memory watch** (`DR_TEX_WATCH=1`, debugging): keeps a hash of every 4 KB page of each
  cached texture and every 2 s re-hashes the ones in use; pages that changed without a re-decode
  (the cache only re-decodes when a sample of the data changes) are logged with their new first
  bytes, and the runtime's own writes into guest memory (XMA output, resolve write-back) are
  checked against the textures' pages. Level loads show up as expected (textures replaced).
* **Old resolve targets no longer hide textures loaded over them**: draws sampled the host copy
  of the last resolve into an address (`g_latestResolve`, keyed by address and a coarse format
  class that lumps DXT with float formats) for the rest of the session. Levels reuse memory, so a
  texture loaded later where a render target had been resolved showed that render target instead
  (flat colours, white). A draw now takes the resolve only if its texture has the destination's
  size and is not block-compressed, which no resolve writes, and, once the target has not been
  resolved for two frames, only while its guest memory is unchanged since that resolve (memory
  alone cannot decide for live targets: the CPU writes into the bloom target between frames).
  Dropped claims are logged ("overwritten since").
* The HUD's white rounded square at the top right is the empty item slot (frame atlas art; the
  camera is not an inventory item), not a missing texture.
* **The draw's mip range**: the device's fetch constant carries the mip levels a draw may use
  (dword 4: `mip_min_level` bits 2-5, `mip_max_level` bits 6-9; D3D merges MAXMIPLEVEL and the
  texture's levels into it). Host samplers now clamp to that range (`minLOD` / `maxLOD`). Without it
  the reflection pass read levels the game never samples: the reflection cube (rebuilt every frame
  from two 128x128 probe cubes the game renders and resolves level by level) came out pink, which
  showed on rough surfaces (floors, fuse boxes) and as red / yellow dots near coloured lights.
  `DR_NO_MIP_CLAMP=1` (debugging) turns the clamp off. `DR_WATCH_ADDR=<hex>` now also captures the
  first four frames that resolve into that address.
* **Texture cache checks pages, not just a sample**: an entry used to be re-decoded only when a
  hash of 16 small chunks of its base level changed. Mip levels were never checked, and a texture
  streamed back in part by part (after a cutscene, say) could keep a decode taken half-way. Each
  entry now keeps a hash of every 4 KB page it decoded (base and mips) and re-checks two pages a
  frame in turn; any change re-decodes it.

## Real 4X MSAA (fixed after that)

The draw log's `RB_SURFACE_INFO` (bits 16-17: 1X, 2X, 4X) shows the whole 3D scene pass in 4X
(`0x14020500`; one scene in 2X), shadow maps and post-processing in 1X; the scene pass ends in a
three-tile depth resolve and a 7e3 colour resolve. The host renders each pass with the console's
sample count (`SurfaceSamples`): host targets and depth buffers exist per (EDRAM key, format,
sample count), pipelines carry the count, and in MSAA passes the game's alpha to mask is the
alpha test at 0.5, not hardware alpha to coverage: that blacked out whole scenes (the roof on
Vulkan, a 2X staff corridor on D3D12) and turned the glass of the staff area red (`DR_NO_A2C=1`
isolated it; `DR_A2C=1` turns it back on to look into why). The 5 s log line counts 2X and 4X draws. Resolves from an MSAA target first average the rectangle into the
target's single-sample copy (`resolveTextureRegion`, `SingleSampled`) and copy, encode (7e3) or
dump from there; the copy is also what `target.descriptor` samples. MSAA depth cannot be copied to
a buffer, so depth resolves from it draw sample 0 of each pixel (as the console resolves depth)
through an MS view of the depth plane (`mainDepthMsaaPS`, set 1 of the same descriptor heap) into
an R32F target, which then takes the place of the staging copy. `DR_MSAA=0` renders every pass
with one sample (the old look, FXAA only).

Some frames draw the scene single-sampled and then restore it into the 4X pass with a full-screen
quad that writes colour and depth from the shader (PS 8CF02D5E). That restore writes one depth per
pixel at the pixel centre; the redraws that follow (floor, characters, "equal or nearer") test each
sample at its own position, and on steep surfaces most samples failed, which left the floor and the
characters black on the roof until something reset the state. A depth-writing draw in an MSAA pass
now makes that pass single-sampled for the rest of the frame (`g_singleSampleSurfaces`, logged), so
it shares the scene's host target as it did before MSAA. Frames that draw the whole scene in 4X
(no restore) keep MSAA.

That left 4X MSAA on about 1% of gameplay draws (the restore happens every gameplay frame), so
the MSAA option seemed to do nothing. Now the restore draw stays in 4X and the depth-tested draws
after it in that pass get a slope-scaled bias toward the viewer (`kPipelineMsaaRestoreBias`: 16 units
+ 1.0 x slope, LESS / LEQUAL / GREATER / GEQUAL), enough for samples 3/8 pixel off the centre.
`DR_MSAA_DEMOTE=1` restores the single-sampled frame.

Clears take the sample count of the pass they belong to: D3D writes `RB_SURFACE_INFO` only when
the next draw flushes state, so a clear still sees the previous pass's value. Clears are kept
pending (`g_pendingClears`) and get the sample count of the first draw or resolve after them
(`SettlePendingClears`); before, a 1X clear could land on the 4X target of the same EDRAM key.

## Reflections: register LOD in the shaders (fixed after that)

Red, pink and black reflections on floors, metal and glass everywhere came from the shader
translation, not the renderer. Shiny surfaces sample a reflection cube (`AC7D0000`, 128x128, 5
levels) that the game rebuilds every frame from two probe cubes (`XfEnvmap` PS 8DA54A62, level n
from the probes' level n). Each probe level is a blurred copy of the next larger one: PS 401C14E7
draws level n from level n-1 (`setTexLOD` with c7.x = n-1, nine cube fetches with a Phong
weight), largest level first, so each capture moves the blur one level down. The materials
(`XfMaterialStandard`, `XfPrim`, water) pick the cube level from their gloss with `setTexLOD` too.

XenosRecomp ignored `setTexLOD` and the fetch's LOD fields, so every fetch used the derivative LOD.
For the probe filter that is the level being drawn: each level sampled itself, stayed at the black
it was cleared to at startup, and the cube's small levels were black. The materials then read
black, or whatever the wrong level held, where the console reads a blurred reflection.

The translator now follows Xenia: `setTexLOD` sets a `texLod` register; a fetch with
UseRegisterLOD calls `tfetch2DLevel` / `tfetchCubeLevel` / `tfetch3DLevel` (`SampleLevel`), one
with UseComputedLOD off samples level 0, and the instruction's LOD bias (signed, 1/16 steps) is
added (`...Bias` with the computed LOD). `setGradientH/V` + UseRegisterGradients map to
`SampleGrad`, and `getCompTexLOD` to `CalculateLevelOfDetail`. Point-sampled vertex fetches with a
register LOD load the texel of that level, as level-0 ones already did. In this game only the
register LOD occurs: 548 `setTexLOD`, 941 fetches in 107 pixel and 33 vertex shaders. The sampler
still applies the fetch constant's mip range and bias.

With the cube filled, the reflections turned red instead of black (glass domes on the roof,
Frank's jacket): the materials multiply the reflection by a DXT5A gloss mask (format 59), fetched
with an XYZW swizzle and read as a colour (`.zyx`). Xenos fills the components a format lacks with
its last one (Xenia: k_8, k_16, DXT5A RRRR; two-component formats RGGG; 5_6_5 RGBB), the host's
BC4 view returns (v, 0, 0, 1), so only red survived. The texture views now take that fill before
the fetch swizzle (`texture_decode.cpp`). Resolved render targets keep four-component host formats.
* **Mip levels of decoded textures**: textures carry their mip chain (fetch constant: mip address,
  min / max level, packed flag). Levels 1+ are stored from the mip address, each sized from the
  base rounded up to powers of two (tiles of 32x32 blocks, 4 KB aligned); once a level's shorter
  side is 16 texels or less, it and all smaller levels share one tile, each at its own offset
  (Xenia's `GetPackedMipOffset`). That also applies to the base of small textures with packed
  mips (an 8x8 texture sits at x = 16 in its tile, not at 0). Without mips, text drawn from the
  1024x1024 font atlas at a third of its size had jagged edges, and distant surfaces shimmered.
  The sampler takes the fetch constant's LOD bias. `DR_NO_MIPS=1` decodes base levels only.
* **Stencil (camera motion blur on Frank)**: gameplay frames end with a velocity pass: moving
  characters are drawn again with this and last frame's bone matrices (s16 / s17) and stencil
  REPLACE, then a full-screen pass computes camera motion from depth only where stencil still
  holds the clear value (EQUAL), and the blur pass smears the scene along the result. Without
  stencil the full-screen pass overwrote the characters' own velocity with camera motion, so
  Frank, whom the camera follows, was blurred like the background whenever he moved. Host depth
  buffers are now D32_FLOAT_S8_UINT; pipelines take RB_DEPTHCONTROL's stencil fields (func,
  fail, z pass, z fail; back faces when bit 7 is set) and RB_STENCILREFMASK (0x210D: reference,
  read and write masks); Clear's 0x20 and Resolve's depth clear (its ClearStencil argument is on
  the stack, r1 + 92) clear stencil. Depth resolves copy the depth plane (R32 footprint; plume's
  Vulkan buffer copies pick one aspect). `DR_NO_STENCIL=1` turns it off; the 5 s log line counts
  stencil draws.
* **Depth tolerance of that redraw**: the velocity pass first writes the scene's depth back
  (a depth-export pass from the resolved depth texture), then redraws the characters with a
  different vertex shader (EB9A4011 instead of the scene's 7C5FAB69) and "nearer or equal". The
  console's 24-bit depth rounds away the last-bit differences between the two; 32-bit float
  depth does not, so every redraw failed the test, marked nothing, and the frame came out the
  same as without stencil. Draws that mark stencil on a depth pass (z pass op REPLACE) get a
  small bias toward the viewer (64 units + 0.5 slope). `DR_NO_STENCIL_BIAS=1` turns it off.
* **Restart index in line strips and fans**: the tutorial pop-ups ("TIME", "SAVING") draw their
  white frame as an indexed line strip with a reset index. Resets were only handled for triangle
  strips; elsewhere they became the lowest index (DrawIndexed) or vertex 0 (the indexed
  BeginVertices path the UI uses), which for a line strip draws a real line (the diagonal across
  every pop-up). Line strips now become line lists and fans triangle lists, restarting at each
  reset, in both paths.
* The game never sets polygon offset (PA_SU_SC_MODE_CNTL bits 11-13; a survey logs it if it does).

## Resolution scale (options menu: 720p, 1080p, 1440p, 2160p = 1x, 1.5x, 2x, 3x)

`Config::ResolutionScale` renders above the console's 720p. Commands stay in guest pixels; the
frame's scale (`Frame::scale`, `FrameScale()`, read when the guest starts recording a frame) is
applied when the video thread replays them.

* **Per pass** (`PassScale`): passes whose render target, with the viewport, is larger than 256
  pixels take the frame's scale (the scene in its 1280x256 tiles, full- and half-resolution
  post-processing, the 1024 / 2048 shadow maps); the small ones (auto-exposure reductions
  64x64 ... 1x1, which the CPU reads back, and the 128 reflection probe levels) stay at 1x. A
  pass is capped at 4096 host pixels a side (the 2048 shadow map stays at 2x at 3x). Host buffers
  are storage shared by every pass that uses them: each pass scales its own guest size and the
  buffer grows to the largest; when the scale changes they are made again at the new sizes
  (keeping their descriptors), so switching back down frees the memory.
* **Draws**: the viewport is scaled. **Half-pixel offset** (D3D9 pixel centres), at every scale
  including 1x: the geometry stays put, so guest pixel k covers host pixels [s k, s k + s) with
  all MSAA samples, and the pixel shaders evaluate their inputs s / 2 host pixels up and left from
  their derivatives (`g_InputShift`, shared constant byte 536, emitted by XenosRecomp at the top
  of every pixel shader): the same texture coordinates as moving the geometry. Moving the
  geometry (the old way; `DR_GEOMETRY_HALF_PIXEL=1` brings it back at 1x) left the first column
  and row, or their left MSAA samples, uncovered: above 1x a dark bar on the top and left that
  the blurs spread over ~10 pixels; at 1x (and half a host pixel above it) wrong depth in some
  scene tiles' first column, a bright first column in the scene and a half-dark one after the
  EDRAM restore. Camera motion blur samples along the motion and smeared that column in from the
  left while turning (720p too). Found with `DR_DUMP` / F12 captures, measuring the first columns
  of every resolve; at 1x the change moves the 3D geometry half a pixel and nothing else (title
  frame: mean difference 1 / 255, same brightness).
  **Resolves**: the source rectangle and destination point and size
  are scaled edge by edge (`ScaleEdge`); the destination texture is created at the pass's scale,
  and `LatestResolve::scale` tells later draws. **Write-back** (8-bit resolves the CPU reads)
  copies the scaled rectangle and averages each guest texel's block on the CPU.
* **Shaders** (XenosRecomp, `shader_common.h`): VPOS is in guest pixels
  (`iPos.xy / g_ResolutionScale - 0.5`, shared constant at byte 532, the draw's pass scale; 217
  pixel shaders read VPOS). Each bound render target copy carries its scale x 4 in bits 24-29 of
  the sampler index (`ScaleCode`); `getGuestTexture2DDimensions()` turns unnormalized
  (`tx_coord_denorm`) coordinates back into guest texels. Texel offsets and `getWeights2D` stay
  in host texels: the game filters its 7e3 HDR scene by hand (taps half a texel apart weighted by
  `getWeights2D`), which on the host grid is an exact copy at any scale and on the guest grid
  blurred. Every sampler heap access masks the index (`SAMPLER_INDEX`).
* **The EDRAM restore shuffle** (what made 1.5x jagged, with doubled letters on the title's mall
  sign): after the tiled HDR resolves, PS F3F97E7B restores the scene (colour and depth export)
  with its pixels permuted inside 4x4 blocks (position = texcoord x c4.xy, the screen in pixels;
  the integer part mapped 0,1,2,3 -> 0,3,1,2 with x and y swapped; fetched at position x c4.zw),
  and after a resolve PS E52354E8 undoes it. With c4 in guest pixels the blocks are guest pixels:
  at 2x and 3x whole host blocks, so the pair still cancels; at 1.5x a guest pixel is 1.5 host
  pixels and it did not. These two draws get a copy of their pixel constants with c4 in host
  pixels (PrepareDraw), so the permutation works on host pixels and cancels at any scale. Found
  with `DR_DUMP` at 1.5x: the scene resolve was clean, the resolve after the second restore not.
* **Final blit**: a picture larger than the window (2x, 3x, or any scale in a small window) is
  averaged over each window pixel's footprint (up to 4x4 bilinear taps, `mainPS`); one bilinear
  tap per pixel skipped texels and looked pixelated. FXAA only runs when the picture is not
  shrunk.
* **Impossible surfaces**: before the auto-exposure passes the game clears through a surface
  object that reports 16384x32768 (viewport to match). Clears and resolves through a surface
  larger than EDRAM could hold no longer size the host buffers (`LimitImpossibleSurface`); they
  had made 4096x4096 colour and depth buffers out of nothing, 8192x8192 at 2x (about 1 GB).
* Cost on a GTX 1080 Ti (title and roof, 30 fps): GPU about 6 ms per frame at 1x, 9-11 ms at 2x,
  about 11 ms at 3x (title); render targets and copies about 0.4 GB at 2x, 0.9 GB at 3x.
* `DR_RES_SCALE=<1|1.5|2|3>` (testing; half steps) overrides the setting. The 5 s log line
  `[renderer] memory at scale ...` sums the targets, depth buffers and copies;
  `[renderer] colour target ...` logs each growth (first 40).

## Performance (mall at 2160p, 60 fps)

The mall at 3x ran at 30 fps: every frame copied about 108 MB of vertex and index data into the
upload buffer, the guest spent 17-22 ms in the renderer's hooks, and the video thread waited for
the GPU after each frame. Now:

* **Resident geometry**: vertex streams and index lists whose hash has not changed for
  `kResidentAfter` (8) frames are copied once into `g_geometryBuffer` (768 MB, device local,
  `GeometryHeap` first-fit allocator) and drawn from there; the frame only carries what changed.
  A changed stream frees its region (reused 4 frames later), evicted textures' memory goes the
  same way, and a frame dropped with copies into the heap pending resets residency. Within one
  frame, streams seen before (hash) are recorded once. Frame data 108 -> 4.4 MB, hooks 17-22 ->
  about 5.7 ms, replay 9-12 -> 1.6 ms.
* `FrameBuffer`: the frame data grows without zero-filling (a `std::vector` cleared megabytes per
  frame).
* **Two frames in flight** (video.cpp `kFrameSlots`, renderer `g_frameSlot`): the video thread
  records and submits frame N while the GPU draws N - 1. Per slot: command list, fence, acquire
  semaphore, upload buffer, menu upload, GPU profiler queries, and what the frame retires
  (textures, framebuffers, views, buffers), its write-backs and dumps (`SlotState`), all released
  once the slot comes round. The bindless texture set is shared: freed descriptors wait for their
  slot before they turn white and go back to the guest, and pointing a descriptor at a different
  texture (textures made again at a new size, targets that grow, resolve textures) waits for the
  other frame first (`ReplaceDescriptor`; counted in the timing line). Resolve write-backs reach
  guest memory one frame later than before. `DR_FRAMES_IN_FLIGHT=1` waits after every frame, as
  before. Before this, a GPU frame of 13 ms plus the replay left the guest waiting up to 10 ms
  for the video thread (55-58 fps in the mall at 3x).
* **GPU profiler** (`DR_GPU_PROFILE=1`): timestamps around uploads, each resolve (by type and
  size), clears and the draws into each target, read back when the slot comes round; every 5 s
  `[gpu profile]` lists the total and the costliest kinds. Mall at 3x (GTX 1080 Ti), about 12 ms:
  4X MSAA scene 5.3 ms, RGBA8 post 1.2 ms, RGBA16F 1.0 ms, 7e3 resolves about 1.5 ms, colour
  resolves 0.6 ms, depth resolves about 0.7 ms.
* The 5 s `[renderer] timing per frame` line: guest frame time and how much of it waited for the
  video thread, the hooks, the replay, and the video thread's waits for the GPU.
* **Command processor** (gpu/command_processor.cpp): the busy mall still ran at 45-50 fps with
  the GPU at 12 ms. A CPU profile (`DR_CPU_PROFILE=1`, below) showed the game's main thread
  spinning half of every frame in D3D's wait for ring buffer space (`sub_8244B050` /
  `sub_8244ED10`): the command processor thread that drains the ring slept 100 us between polls
  (about 1 ms on Windows) and formatted a history string for every packet and register write.
  The ring write pointer, an MMIO store (`DrMmioWrite32` in apu/xma.cpp), now wakes it at once
  (`CommandProcessor::OnRegisterWrite`); it spins briefly before sleeping, also in WAIT_REG_MEM;
  the history keeps raw entries and packets reuse a buffer per nesting depth. The busy mall
  (3,200-3,700 draws) then held 59-61 fps.
* **Texture decodes off the game thread**: the first draw of a texture decoded it on the game
  thread: 80-100 ms when an area or a mall section loaded (the `[hitch]` frames). The draw now
  reserves the descriptor and an empty command; `DecodePool` (half the cores, up to 8) decodes
  while the game goes on and `FinishDecodes` at present turns each result into the upload in its
  command (a failed decode leaves the command empty and the texture white). Cube maps, texture
  dumps and logs (`DR_TEX_DUMP`, `DR_TEX_LOG`, `DR_TEX_WATCH`, F12 captures) decode in place;
  `DR_TEX_ASYNC=0` does it the old way.
* **Pipelines in parallel**: at present the guest asks for every pipeline of the frame's draws
  that does not exist yet (`RequestPipelines`); worker threads make them while the video thread
  gets to them and replay waits for any not done (`GetPipeline`). One by one at replay they cost
  about 1.3 ms each: an area load's 30 new pipelines took 40 ms. Area load replay 63 -> 28 ms, the
  game thread no longer stalls there, the title's 147 ms hitch is gone.
* **Vulkan pipeline cache** (iOS / MoltenVK, `DR_GPU=vulkan`): plume's Vulkan backend makes every
  pipeline through one VkPipelineCache, filled at start from `_recomp_cache/vulkan_pipelines.bin`
  in the game folder and saved there when pipelines were added (at most every 30 s) and when the
  window closes (`SetVulkanPipelineCachePath` / `SaveVulkanPipelineCache` in plume_vulkan.cpp).
  On Vulkan the title's first pipelines took 80-97 ms without it (NVIDIA's own driver cache hides
  that on later runs; MoltenVK has none).
* `[hitch profile]` (default on, `DR_HITCH_PROFILE=0` off; profiler/cpu_sampler.cpp): a thread
  samples up to four threads every 4 ms into a ring each (the last 2 s): the threads that present
  (`WatchHitches`; the main thread at boot, then the game's render thread), the thread that draws
  (`OnDrawIndexed`; the same as the presenting one here) and the command processor
  (`WatchHitchThread`). For a frame with over 50 ms of game work plus present work it logs, per
  thread, its samples and share waiting, and for a busy one the self time, recompiled functions
  inclusive and commonest call paths, on the sampler thread so symbol loading never stalls the game.
  Added 2026-11-25 for a 261 ms frame of pure game code in the user's play. Findings: the boot
  frame's 250 ms is file opens (NtCreateFile under `sub_8243EEA8`). The main game thread
  (`sub_8218ECF8` loop) waits in `sub_827254C0` (with +41 set, no timeout, on the event at +92) for
  the game's render thread (`sub_827275D0` loop), which builds the draws (`sub_82722888`) and
  presents (`sub_82721660` -> `sub_8245BA18`, our Present hook); `sub_824408F8` is
  WaitForSingleObjectEx, `sub_8244B050` D3D waiting for ring space. `_NLG_Return2` in a profile is
  the C runtime's memcpy (its nearest symbol).
* **Decoded textures taken, not copied, at present** (2026-11-25): the load hitches (50-140 ms in the
  user's sessions) were the render thread at present copying a load's decoded textures into the
  frame data (`MakeUpload`: about 40 ms in the title and save-load tests, plus freeing them),
  while the main thread waited for it. `FinishDecodes` now moves each result into the frame's
  `textures` list; `Replay` (video thread) copies them into the upload buffer after the fixed
  regions (`TextureUploadBase`; the copy it already made for the frame data, now from these
  buffers) and hands them to `g_bufferReaper`, a thread that frees them. `DR_TEXTURE_COPY=1` copies
  as before (comparison). `[hitch]` lines now also give the present's work and its texture-decode
  wait. Same build, two runs each (boot, title, save load at 60 fps, 4K): the copy path's title
  load frame 79 / 79 ms (present 29-30 ms) and save load 86 / 75 ms (present 34-39 ms); taken,
  60 / 59 ms (present 11 ms, all waiting for decodes still running) and no save-load hitch.
  Both paths then wait 40-80 ms for the video thread after the title load: the 4K targets made
  and first used once (`render targets` in the replay, then the GPU).
* **Lock hand-over** (2026-11-25, kernel/imports.cpp): a 279 ms frame in the user's play was the
  audio engine's wait for its mixer thread (`sub_82558D00`, under `sub_822D4178` ...
  `sub_825714A8`): while a voice is still in its list it releases its spin lock (0x828AA82C) and
  critical section and takes them back, in a loop. Our critical section and spin locks let the
  releasing thread win again at once (the woken waiter is not running yet); the console's kernel
  counts waiters (LockCount) and queues a thread that asks again behind them. Now waiters are
  counted per lock (a 4096-entry table by address, with when the current waiters began) and a
  thread asking again for a lock it just released while a waiter had waited over 2 ms lets the
  waiters take it first, for at most 0.5 ms (`DR_LOCK_HANDOVER=0` off; `[locks]` lines count the
  hand-overs). Handing over on every contended release first cost the main thread about 8 ms in a
  loading frame (a Windows wake-up each time); the user's session with it had no slow audio wait
  in over 1,000 calls. `[audio wait]` lines log that function's waits over 5 ms and its
  call count at 1, 100, 1000... calls. Not reproduced in the save-load test (one call there); no
  change in frame time.
* Still seen in the user's play: the prologue cutscene start (NPC archives) where the render thread
  converts many new models' vertex data in our draw hooks (`sub_82549E20`: memcpy, CopyAux,
  SwapInPlace, DecodeElement; 110-118 ms, in two sessions). A new stream is hashed, copied and
  swapped into the frame, its aux decoded, and a CPU copy kept (`cached.converted`) until it has
  been stable for `kResidentAfter` (8) frames. `[hitch]   draw hook stages` lines (after each
  `[hitch]` frame) now give that frame's prepare / indices / vertex streams / index upload time and
  any growth of the frame data buffer (a reallocation that copies it).
* `[hitch]` lines: a guest frame over 50 ms, with the time on that thread and on all threads in
  file reads, texture decodes and hooks (profiler/hitch_stats.h); `[hitch] replay of frame N`: a
  replay over 25 ms, with new pipelines and texture upload time; `[hitch] video thread, frame N`:
  a video-thread loop over 50 ms (not counting its wait for the game's frame), split into window
  and device events, input / rumble / options menu, swap chain resize, waiting for the GPU,
  acquire, replay, submit, present and title / caches, with the SDL, controller and window event
  counts (gpu/video.cpp). `[video] window lost focus / got focus / minimized / restored` and
  `[input] rumble ... took N ms` (a rumble call over 10 ms) go with it. Added 2026-11-25 for
  stalls of 200-340 ms in the user's play log (Security Room cutscene, frames 7593-8715, in
  pairs and threes) with no slow replay and little GPU wait: the game waited for the video thread
  and jumped up to 10 frames (`mDeltaTime` 10.4) after each. They did not come back in the
  user's next session (no cause seen). That session's video-thread lines showed 120-135 ms at area
  loads at 4K where the replay itself took 25-60 ms: `[hitch] before the replay of frame N`
  (Renderer::Render) times what comes first, and it was the upload buffer growing (each frame's
  data, with the textures it uploads, goes through its slot's upload buffer; a bigger one was made
  on the spot, and Windows zeroes the memory: 16 -> 110 MB 16 ms, 30 -> 164 MB 37 ms in a 4K test,
  more with little free RAM). Both slots' buffers are now made at 160 MB in Renderer::Init (21 ms,
  before the game runs; area loads were seen to need up to 109 MB; `DR_UPLOAD_MB=<n>`, 0 = grow as
  before); a buffer still grown in game logs `[renderer] upload buffer of slot N grown`. Checked
  at 4K: the save load's frame went from 69 ms to under 50; the title's first frames still take
  60 ms for render targets made at 4K (36 ms).
* **CPU profiler** (`DR_CPU_PROFILE=1`, profiler/cpu_sampler.cpp, Windows): every guest thread,
  the command processor and the video thread are sampled about once a millisecond (stack walk
  with the exe's unwind data, names from DRRecomp.pdb next to the exe); every 10 s, per thread:
  time waiting, top self and inclusive functions (recompiled ones as sub_XXXXXXXX) and the
  commonest call paths. Crash reports use the same stack walk (the guest call chain) and log all
  32 guest registers.

## Crashes in the busy mall (2026-11-25)

X in the busy mall crashed in Frank's action code three ways (`sub_822BECE8` reading item table
0x82803270 at an empty index, `sub_822A65B0` with no held item and no special move entry,
`sub_822A9498` calling the held item's method when drinking a coffee jug: null). His held item
(+0xE78) comes from `sub_8215FC48` / `sub_8215FDE8`, which allocate it from the game's "Unit"
heap (MT Framework allocator, *0x828AED14, 18 MB; every zombie, survivor, item and effect lives
there) and return null when that fails; the item then stays "held" with no object. The Unit heap
is now 36 MB (`sub_827614D0` hook in gpu/d3d_hooks.cpp; `DR_UNIT_HEAP_MB=18` for the console's):
the game used at most about 400 of the 512 MB of physical memory in the tests. Guards at all
three sites skip the action (the drink / eat one ends it, nothing consumed) with a `[guard]` line
instead of crashing; `[item] item N could not
be made` logs a failed allocation with the heaps and unit counts; `[heap]` every 1800 steps lists
every heap (used / size) and physical memory, plus the commonest unit classes once Unit is over
60% full (every 120 steps over 90%); `[held]` logs each change of Frank's held item, its index or
his item id (+0x1180) as it happens, and the last 96 changes of those, his action and the special
entry are dumped by the guards and crash reports.

Cause (found with the user's next crash, 2026-11-25): not the heap (Unit 6.7 of 36 MB at the
crash). At 60 fps every one-frame animation event was set on three updates in a row instead of
one (docs/frame_rate.md, "Animation events fired two or three times"): the drink action uses and
clears the held item on the "use" event, so the second update called through null; the X actions
read the held item the same way. Fixed in the motion update (`sub_826E7F08` wrapper). The next
crash was in `sub_822A65B0` with both +0xE78 and +0xE7C null although its guard checks them on
entry and the function makes no call before the read: something cleared them in between, most
likely another game thread (unit moves run as parallel jobs, `sub_8242DCA0`). The guard now also
covers a special entry whose id does not match (the other null path). As a net for such races,
reads through a null guest pointer no longer crash: on Windows the first guest page stays
inaccessible, a read fault there makes it read-only for that one instruction (trap flag) and logs
`[null] read of guest 0x.. at DRRecomp.exe+0x.. (guest lr ..); read as zero` with the call chain
(first 20); writes still crash. `DR_NULL_READS=crash` turns it off, `DR_NULL_TEST=1` tests it (two
reads, then exit). Elsewhere (iOS) the page is mapped read-only. `DR_IMAGE_DUMP=<file>` writes the
loaded game image and exits, for reading constants and tables offline (game data: keep private).

## Debugging

* **F12** (Windows) captures the frame after next: every draw logged as with `DR_DRAW_LOG`, every
  texture it binds dumped with all mip levels and cube faces (`_L<level>_F<face>`), and its render
  targets and resolves as with `DR_DUMP`, into `dump/` next to the log. `DR_TEX_DUMP=frame` with
  `DR_DRAW_LOG=<frame>` does the same for a chosen frame, `DR_TEX_DUMP=cube` dumps every cube map,
  `DR_TEX_DUMP=<hex>,<hex>` the textures at those addresses (all levels).
* `DR_TEX_PAGECHECK=0`: the texture cache checks its sample only (the old behaviour).
* `DR_DUMP=N`: every N frames, BMPs in `run/dump/` of every render target, every resolve (in
  order, with its source for tiled ones) and the scene snapshot.
* `DR_NO_RESOLVE_CLEAR=color|depth|all`: skips the clears resolves ask for.
* `DR_TEX_WHITE=<formats>` (Xenos format numbers, comma-separated), `DR_TEX_WHITE_ADDR=<lo>-<hi>`
  (hex addresses), `DR_TEX_WHITE_SLOT=<sampler bit mask>`: those textures stay white.
* `DR_TEX_LOG=1` logs every texture decode, `DR_TEX_DUMP=1` writes them as BMPs (colour | alpha),
  `DR_TEX_REUPLOAD=<n>` re-uploads every texture every n frames.
* `DR_DRAW_LOG=<guest frame>`: every draw of that frame (shaders, declaration, render state,
  textures and sampler words, VS / PS constants, the first vertices decoded), clears, resolves.
* `DR_DRAW_RANGE=<a>-<b>` replays only those draws, `DR_DRAW_SKIP=<a>-<b>` leaves them out
  (command indices as in the draw log).
* `DR_DEBUG_UV=1`: every 2D texture fetch returns its coordinate (frac) instead.
* `DR_NO_DEPTH_TEST=1`, `DR_DEFAULT_SAMPLER=1`, `DR_NO_ALPHA_TEST=1`, `DR_NO_DEPTH_RESOLVE=1`,
  `DR_NO_WRITEBACK=1`, `DR_FILL_RESOLVE=<address>:<byte>`.
* By shader instead of draw index (indices move between runs): `DR_SKIP_VS=<hash>,...`,
  `DR_SKIP_PS=<hash>,...`, `DR_ZALWAYS_VS=<hash>` (depth test always passes, depth still written).
* `DR_NO_HALF_PIXEL=1`, `DR_NO_CULL=1`, `DR_FLIP_WINDING=1`, `DR_STATIC_VTF=1` (vertex textures
  decoded at first bind, the old way), `DR_AA=0`, `DR_WATCH_ADDR=<hex>` (log resolves into it).
* With `DR_DRAW_LOG`, vertex texture rows and bone matrices are logged at draw and present time,
  and the dump frame reads the uploaded bone texture back from the GPU. Dumps include
  `frameN_shown_WxH.bmp`, the image as presented (FXAA included).
* `DR_AUTO_INPUT` takes buttons and sticks (`lup`, `lleft`, `rright`, ..., diagonals `lupleft`,
  `lupright`, `ldownleft`, `ldownright`) with an optional hold length: `9000:lup*300` holds the
  left stick up for 300 video frames. With the save in the Security Room (60 fps):
  `700:start,900:a,1000:a,1080:a` loads it (Frank there by frame ~1180); then
  `1300:lleft*130,1440:b` goes through its door, `1560:ldown*40,1601:lleft*85` to the foot of the
  stairs (the roof and the duct to Paradise Plaza are further; not scripted yet). The pause menu's
  Options page: `1300:start,1420:down,1445:down,1470:down,1495:down,1520:down,1560:a`.
  New game: `700:start,900:a,1000:down,1060:a,1150:a`.
* `DR_DUMP_DRAWS=<a>-<b>` dumps the colour target after each of those draws (dump frame);
  `DR_NO_DEPTH_CLIP=1`, `DR_VBLANK_HZ=<rate>`; float dumps report NaN / infinity counts.
* With the time base fixed, runs are reproducible enough to compare one frame between runs
  (e.g. frame 6000 is the same photo-segment shot).
* `scripts/scene_probe.ps1` (one dump frame, average brightness), `scripts/run_sheet.ps1`
  (contact sheet of the final image every N frames, with `DR_AUTO_INPUT` presses),
  `scripts/bmpavg.pl`. Bisect by measuring, not by eye.
* XenosRecomp: `XENOS_DUMP_HLSL=<folder>` keeps every translated shader.
* `DR_SHOT=<step>[:<first>-<last>]`: only the shown image (`frameN_shown_WxH.bmp`), every <step>
  guest frames in that range; cheap enough for whole runs (motion, cutscenes).

## Next

3D textures, render targets in separate EDRAM buffers (MRT that does not alias). At 3x: the 4X
MSAA scene pass (5 ms) and the depth resolves (a staging-buffer copy that could be one pass).

## Test windows and the minimized window

* `DR_SETTINGS="FrameRate=30;ResolutionScale=2;..."` overrides settings for one run and keeps the
  options menu from saving (`DR_NO_SAVE=1` alone only stops saving): the player's controller
  reaches every window, and closing the menu in a test window had written the test's values into
  recomp_config.toml. `DR_NO_PAD=1` ignores real controllers, the keyboard and F1, so scripted runs
  (`DR_AUTO_INPUT`) are repeatable.
* Minimized (empty swap chain or no image to acquire), the video thread still replays each frame
  without presenting (`RenderOffscreen` in video.cpp, `Renderer::Render` with no back buffer).
  Skipping it left the game waiting a second for the video thread on every frame (1 fps, the
  "frozen" window) and would have lost the frame's texture uploads and resolves.
