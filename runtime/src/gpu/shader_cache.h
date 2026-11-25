#pragma once

// Interface of the shader cache XenosRecomp generates (analysis/shader_cache/shader_cache.cpp, see
// docs/shaders.md). Same layout as UnleashedRecomp's; the extra field is filled by the runtime.

#include <cstddef>
#include <cstdint>

struct ShaderCacheEntry
{
    const uint64_t hash;
    const uint32_t dxilOffset;
    const uint32_t dxilSize;
    const uint32_t spirvOffset;
    const uint32_t spirvSize;
    const uint32_t specConstantsMask;
    const uint32_t pixelOutputs;   // Dead Rising: colour targets 0-3 (bits 0-3) and depth (bit 4) a pixel shader writes
    const uint32_t floatConstantsEnd; // Dead Rising: the float constants it reads are below this register (0: unknown, all)
    struct HostShader* hostShader; // runtime state (gpu/shaders.cpp), null until first use
};

extern ShaderCacheEntry g_shaderCacheEntries[];
extern const size_t g_shaderCacheEntryCount;

extern const uint8_t g_compressedDxilCache[];
extern const size_t g_dxilCacheCompressedSize;
extern const size_t g_dxilCacheDecompressedSize;

extern const uint8_t g_compressedSpirvCache[];
extern const size_t g_spirvCacheCompressedSize;
extern const size_t g_spirvCacheDecompressedSize;
