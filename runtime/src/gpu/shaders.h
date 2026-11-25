#pragma once

#include <cstdint>

namespace plume
{
    struct RenderDevice;
    struct RenderShader;
}

struct ShaderCacheEntry;

// Dead Rising's shaders, translated ahead of time by xfb2xenos + XenosRecomp (docs/shaders.md).
// The game creates its shader objects in place: D3D's init functions get the object and the
// microcode address, and the 2006 shader container follows the object. The hooks on those functions
// identify each shader by a key over container + microcode and map the guest object to its
// translated shader in the cache.
namespace Shaders
{
    // Specialization constants (bits of XenosRecomp's specConstantsMask).
    enum : uint32_t
    {
        SPEC_ALPHA_TEST = 1 << 1, // SPEC_CONSTANT_ALPHA_TEST in shader_common.h
        SPEC_MRT_ALIAS = 1 << 5,  // SPEC_CONSTANT_MRT_ALIAS: oC0 = the highest colour export
        SPEC_FLIP_Z = 1 << 6,     // SPEC_CONSTANT_FLIP_Z: vertex shaders output z' = w - z
        SPEC_DECODE_7E3 = 1 << 7, // SPEC_CONSTANT_DECODE_7E3: oC0 holds 7e3 bits (fixed-point view of the HDR buffer)
    };

    // Video thread, once the device exists: unpacks the cache for the backend (SPIR-V for Vulkan,
    // DXIL for D3D12). Shaders created before this are registered and resolved afterwards.
    bool Init(plume::RenderDevice* device, bool spirv);

    // Hooks on D3D's shader init (guest thread), before the original runs.
    void OnVertexShaderInit(uint32_t object, uint32_t microcode);
    void OnPixelShaderInit(uint32_t object, uint32_t microcode);

    // The cache entry of a guest D3D shader object (null for unknown objects and null shaders).
    ShaderCacheEntry* Find(uint32_t object);

    // The host shader of a cache entry for the given specialization constants (thread-safe; built
    // on first use). Null if the shader could not be created.
    plume::RenderShader* GetHostShader(ShaderCacheEntry* entry, uint32_t specConstants);

    // Counts a SetVertexShader / SetPixelShader of a guest object (diagnostics).
    void OnBind(uint32_t object);

    // Video thread: builds the host shaders of newly created guest shaders (both alpha test
    // variants for pixel shaders, so every shader the game uses is checked once) and logs totals.
    void Update();
}
