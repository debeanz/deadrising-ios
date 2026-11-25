# Dead Rising shaders → XenosRecomp

## Where the shaders are

`ntvX/system/shader.arc` (MT Framework archive, unpack with `scripts/unarc.ps1`) holds 20 shader
packages (`system\xfshader\Xf*`, MT Framework **XFB** format, magic `\0XFB`). Inside them are
**828 Xbox 360 shaders – 559 pixel + 269 vertex**, exactly the number of pixel/vertex shaders the
game creates at runtime (seen with the call profiler); 777 of them are unique. Compiler: Xbox 360
Shader Compiler 2.0.2638 (2006).

## XFB package layout (from the game's loader, `sub_82770DC8`)

```
"\0XFB"  u16 version (1)  u16 groupCount  u32 parameterCount
parameterCount x 8 bytes    { u32 parameter hash, u8 type, u8 ... }   (bindings point here)
groupCount x 16 bytes       { u32 hash, u32 virtualSize, u32 physicalSize, u32 runtime pointer }
for each group:  virtual blob (virtualSize bytes), then physical blob (physicalSize bytes)
```

The physical blob holds **all microcode of the group**; the loader copies it into physical memory.
Virtual blob (offsets relative to its start, relocated by the loader): `+0x08` pixel shader count,
`+0x0C` vertex shader count, `+0x10` technique table, `+0x14` vertex shader record offsets,
`+0x18` pixel shader record offsets. A shader record is

```
u32 microcode offset (into the group's physical blob)
u32 flags            (low 7 bits: binding count n)
u32 bindings[n]      (u16 parameter index, ...; patched by the loader)
D3D object           (0x250 bytes VS / 0x38 PS, zero in the file; sub_82452620 / sub_82452498
                      initialise it in place with the microcode address)
2006 shader container (directly after the object; D3D finds it at object + size)
```

So the microcode is **not** behind the container. (An earlier version of `xfb2xenos` assumed it
was, which made every translation read the wrong bytes; 126 shaders compiled by accident.)
Check: decoding every control-flow program now finds exactly one `exec_end` in each of the 828.

## Container format (2006, flags `0x102A0Exx`) vs XenosRecomp's (2008, `0x102A11xx`)

| | 2006 (Dead Rising) | 2008 (Sonic Unleashed / XenosRecomp) |
|-|-|-|
| header | 0x18 bytes: flags, virtualSize, physicalSize, defTable, constantTable, shader | 0x24 bytes: flags, virtualSize, physicalSize, ?, constantTable, defTable, shader, 0, 0 |
| microcode | elsewhere (see above) | behind the virtual part |
| constant table | `{u32 size; CTAB}` – **missing in 645 of 828 shaders** | always present |
| literals (`def`) | 2nd definition section, values **inline**: `{reg \| dword count, values…}` | float4: `{reg \| count, physical offset}`; int4: `{reg \| count, values…, 0}` |
| shader header | interpolator info at word 4; PS: outputs at word 7, interpolators from word 8; VS: element count at word 7, 3 words, elements, interpolators | interpolator info at word 5; PS: outputs at word 7; VS: field18 / count / field20, array |
| VPOS | flag `0x08000000` in PS word 2 (assumption, 217 PS; not verified) | register in `fieldC >> 8` |

**Literal registers** are byte offsets into the D3D device's constant shadow, relative to
device + 0x400 (the fetch constants), which matches the device map in [d3d_map.md](d3d_map.md):

| register | device | meaning |
|-|-|-|
| `0x300 + 16·c` | `0x700 + 16·c` | VS float constant c (c = 0–255) |
| `0x1300 + 16·c` | `0x1700 + 16·c` | PS float constant c |
| `0x2300` | `0x2700` | booleans |
| `0x2320 + 4·i` | `0x2720 + 4·i` | loop constant i (same encoding XenosRecomp expects for int4) |

The compiler allocates float literals from the top (VS `0x12F0` = c255, PS `0x22C0` = c252) and
Dead Rising uses loop constant i31 (36 shaders). Pixel shader booleans are addressed absolutely
in the microcode (128 + index).

Interpolator and vertex element encodings are identical between the two formats (e.g. `0x3050` =
TEXCOORD0 in r0, `0xF1A0` = COLOR0 in r1, element `0x0000300A` = NORMAL from fetch slot 0xA).
Usages seen: interpolators TEXCOORD0–7, COLOR0, NORMAL0/1, TANGENT0; vertex inputs POSITION0,
NORMAL0, TANGENT0, TEXCOORD0–3, COLOR0, BLENDINDICES0/1, BLENDWEIGHT0/1.

## Tools

* `xfb2xenos/` (`xfb2xenos.exe <out dir> <in…>`): parses each XFB package as above and rewrites
  every 2006 container as a 2008 one – new header, rebuilt shader header, a **synthetic full-range
  constant table** (float4 array over all registers, samplers 0–31, bools 0–15; also used when the
  shader has a real table, since the renderer uploads the whole range), float literals moved
  behind the microcode, loop constants as int4 definitions. Writes `listing.txt` into the output
  folder: XenosRecomp hash → package, offsets, raw 2006 header words (to trace a failing shader).
* `tools/XenosRecomp` (squidbus fork) with Dead Rising changes (`patches/XenosRecomp-deadrising.patch`):
  * registers up to r127 (shaders use up to r62); literal constants take priority over the table;
  * **conditional exec blocks honour their condition** (`cond_exec*` on a boolean, `cond_exec_pred*`
    on the predicate; upstream ran them unconditionally – 223 + 213 Dead Rising shaders use them);
    `cond_exec_pred_clean_end` now ends the shader;
  * pixel shader boolean addresses 128+ map to their constants;
  * **32 sampler slots** per texture type (vertex shaders fetch from fetch constants 16/17): the
    shared constants block is now 2D[32], 3D[32], Cube[32], Sampler[32], then `g_Booleans` at
    byte 512 (`c32` in the DXIL cbuffer) – the runtime must use this layout;
  * vertex shaders sample with `SampleLevel(…, 0)` (`XENOS_VERTEX_SHADER` define);
  * NORMAL0/1 and TANGENT0 interpolators; vertex input locations 12/13 for BLENDINDICES1 /
    BLENDWEIGHT1 (TEXCOORD4/5 moved to 14/15; TEXCOORD6/7 and POSITION1 dropped – unused);
  * 1D texture fetches become 2D fetches at v = 0.5; up to 16 `cube` directions per shader;
  * a failing or crashing shader is logged and kept in `failed/` (HLSL, or the container for a
    crash) instead of stopping the run.
* `scripts/dxc_failed.sh`: compiles every HLSL in `failed/` on its own with `dxc.exe` and records
  the first error per shader (`analysis/shader_cache/dxc_errors.txt`, logs in `dxc_logs/`) – the
  error output of the parallel run is interleaved.

```
xfb2xenos\xfb2xenos.exe analysis\shaders_2008 analysis\shader_arc
tools\XenosRecomp\build\XenosRecomp\XenosRecomp.exe analysis\shaders_2008 analysis\shader_cache\shader_cache.cpp tools\XenosRecomp\XenosRecomp\shader_common.h
```
(`build_xenos.bat` builds XenosRecomp; copy `thirdparty\dxc-bin\bin\x64\*.dll` next to the exe.)

## Status: all 777 unique shaders translate and compile to DXIL + SPIR-V

Spot check: a debug pixel shader from `XfDevelop` translates to "vertex colour × (texture enabled ?
texture : 1)", as its constant table names (`XfDebugBaseSampler`, `gXfTextureEnable`) suggest.

Not verified yet (needs drawing with them):
* the VPOS flag (`0x08000000`, PS word 2): a shader that does not use VPOS only gets an unused
  register initialised, but one that does would read the wrong value if the flag is something else;
* `cube` + cube fetch use XenosRecomp's approximation (the direction is kept, the face math the
  shader does on the `cube` result is ignored);
* texture LOD (fixed): `setTexLOD` / UseRegisterLOD, UseComputedLOD off, the instruction's LOD
  bias, register gradients and `getCompTexLOD` are translated (`texLod`, `tfetch*Level`,
  `tfetch*Bias`, `tfetch*Grad`; see renderer.md, "Reflections"). Before, every fetch used the
  derivative LOD, which left the reflection probes' small levels black.
  `XENOS_DUMP_HLSL=<folder> XENOS_DUMP_ONLY=1` translates without compiling (about a second), for
  surveys like that one;
* the real constant tables (183 shaders) are ignored; the synthetic names (`g_VsConstants(n)`,
  `g_Bool3`, `g_Sampler2`) are register numbers.

## Runtime (`runtime/src/gpu/shaders.cpp`)

`scripts/build_shaders.bat` regenerates everything the runtime links: the cache
(`analysis/shader_cache/shader_cache.cpp`) and `analysis/shaders_2008/shader_keys.cpp`.

* **Lookup key.** The cache is keyed by the hash of the *converted* container, which the game
  never has. `xfb2xenos` therefore also writes `shader_keys.cpp`: key → cache hash, where key =
  `XXH3(microcode, seed = XXH3(2006 container virtual part))` – data the game has when it creates
  the shader. 780 keys map onto the 777 cache entries (a few shaders differ only in their constant
  table, which the conversion replaces).
* **Hooks** (`gpu/d3d_hooks.cpp`, tap: the original still runs): `sub_82452620` (VS init) and
  `sub_82452498` (PS init) get `r3` = object, `r4` = microcode; the container is at object + 0x250 /
  + 0x38. The hook computes the key and maps the guest object to its cache entry.
  SetVertexShader / SetPixelShader (`r4` = object, stored at device + 0x3364 / + 0x3088) look it up.
* **Host shaders** are built as UnleashedRecomp does: SPIR-V (smol-v decoded) on Vulkan, with the
  alpha test as a specialization constant at pipeline creation; on D3D12 every pixel shader is a
  DXIL library (it uses `g_SpecConstants()` for the alpha test) and is linked at runtime with DXC
  against `uint g_SpecConstants() { return N; }` (`dxcompiler.dll` + `dxil.dll` ship next to the exe).
  A worker thread builds each new shader once (both alpha-test variants) to check the whole cache.
* `DR_GPU=vulkan` forces the Vulkan backend.

Verified (45 s from boot to the title loop, D3D12 and Vulkan): the game creates 269 VS + 559 PS,
**all found in the cache** (0 unknown keys, 0 objects without a container), host shaders built for
all 777 entries with **0 failures**, and every SetVertexShader / SetPixelShader binds a known
object (or null).

Next: pipelines – vertex declarations (`sub_82452860`) to input layouts, render state to pipeline
state, constant upload (VS/PS float constants at device + 0x700 / + 0x1700, booleans, the shared
constants block with 32 sampler slots) – then the first real draws. The Metal path for iOS still
needs XenosRecomp's Metal output.
