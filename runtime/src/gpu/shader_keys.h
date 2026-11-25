#pragma once

// Table xfb2xenos generates (analysis/shaders_2008/shader_keys.cpp): the key the runtime computes
// when D3D initialises a shader (hash of the 2006 container + microcode, see gpu/shaders.cpp)
// -> the XenosRecomp shader cache hash. Sorted by key.

#include <cstddef>
#include <cstdint>

struct ShaderKey
{
    uint64_t key;
    uint64_t cacheHash;
};

extern const ShaderKey g_shaderKeys[];
extern const size_t g_shaderKeyCount;
