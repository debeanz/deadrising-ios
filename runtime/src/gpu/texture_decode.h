#pragma once

// Xbox 360 (Xenos) textures -> host texture data: untiling, endian swap, format conversion.
// Layouts and formats follow Xenia's documentation of the Xenos texture fetch constant
// (xenos.h, texture_util.cc).

#include <cstdint>
#include <vector>

namespace plume
{
    enum class RenderFormat;
    enum class RenderSwizzle : uint8_t;
}

namespace TextureDecode
{
    // The fields of a GPU texture fetch constant (6 big-endian dwords, already byte-swapped).
    struct FetchConstant
    {
        uint32_t dword[6];

        uint32_t Format() const { return dword[1] & 0x3F; }
        uint32_t Endian() const { return (dword[1] >> 6) & 3; }
        uint32_t BaseAddress() const { return dword[1] & 0xFFFFF000; }
        bool Tiled() const { return (dword[0] >> 31) != 0; }
        uint32_t Pitch() const { return ((dword[0] >> 22) & 0x1FF) * 32; } // texels
        uint32_t Width() const { return (dword[2] & 0x1FFF) + 1; }
        uint32_t Height() const { return ((dword[2] >> 13) & 0x1FFF) + 1; }
        uint32_t Dimension() const { return (dword[5] >> 9) & 3; }        // 0 1D, 1 2D, 2 3D, 3 cube
        uint32_t Swizzle() const { return (dword[3] >> 1) & 0xFFF; }      // 3 bits per output: X Y Z W 0 1
        uint32_t Signs() const { return (dword[0] >> 2) & 0xFF; }         // 2 bits per component
        uint32_t ClampX() const { return (dword[0] >> 10) & 7; }
        uint32_t ClampY() const { return (dword[0] >> 13) & 7; }
        uint32_t ClampZ() const { return (dword[0] >> 16) & 7; }
        uint32_t MagFilter() const { return (dword[3] >> 19) & 3; }
        uint32_t MinFilter() const { return (dword[3] >> 21) & 3; }
        uint32_t MipFilter() const { return (dword[3] >> 23) & 3; }
        uint32_t AnisoFilter() const { return (dword[3] >> 25) & 7; }
        uint32_t MipMinLevel() const { return (dword[4] >> 2) & 0xF; }
        uint32_t MipMaxLevel() const { return (dword[4] >> 6) & 0xF; }
        bool PackedMips() const { return ((dword[5] >> 11) & 1) != 0; }  // small levels share one 32x32 tile
        uint32_t MipAddress() const { return dword[5] & 0xFFFFF000; }     // levels 1+ (0: none)
    };

    constexpr uint32_t kMaxLevels = 14;

    struct Decoded
    {
        plume::RenderFormat format;
        uint32_t width = 0, height = 0, layers = 1; // layers: 6 for cube maps
        bool cube = false;
        uint32_t rowWidth = 0;   // base level: texels per row in 'data' (rows padded to 256 bytes)
        uint32_t rowPitch = 0;   // bytes
        uint32_t layerPitch = 0; // bytes, multiple of 512
        uint32_t levels = 1;     // mip levels in 'data'
        struct Level
        {
            uint32_t width, height, rowWidth, rowPitch, layerPitch;
            uint32_t offset; // in 'data', multiple of 512; layers follow each other
        } level[kMaxLevels];
        plume::RenderSwizzle swizzle[4];
        std::vector<uint8_t> data;
    };

    // Decodes the texture at 'source' (host pointer to the guest data of the base level) and, when
    // 'mips' (host pointer to MipAddress()) is given, its mip levels in the Xenos layout.
    // Returns false for unsupported formats or dimensions; 'reason' says why.
    bool Decode(const FetchConstant& fetch, const uint8_t* source, const uint8_t* mips, Decoded& out, const char*& reason);

    // Bytes of guest memory the base level spans (for change detection).
    uint32_t GuestSize(const FetchConstant& fetch);

    // Mip levels the texture has (1: base only) and the bytes its levels 1+ span from MipAddress().
    uint32_t LevelCount(const FetchConstant& fetch);
    uint32_t MipsSize(const FetchConstant& fetch);

    // Debugging: the first layer of a decoded texture as RGBA8 (BC1-3, RGBA8, R8, RG8), before the
    // swizzle. False for other formats.
    bool ToRGBA8(const Decoded& decoded, std::vector<uint8_t>& rgba);
}

namespace TextureDecode
{
    // Byte offset of texel (x, y) of the base level of an uncompressed texture in guest memory
    // (tiled or linear), for writing resolved data back where the CPU expects it.
    uint32_t TexelOffset(const FetchConstant& fetch, uint32_t x, uint32_t y, uint32_t bytesPerTexel);
}
