// Xenos texture decoding (see texture_decode.h). Tiled layout: Xenia's TiledOffset2DOuter/Inner.

#include <stdafx.h>
#include "texture_decode.h"

#include <plume_render_interface_types.h>

using plume::RenderFormat;
using plume::RenderSwizzle;

namespace TextureDecode
{
    namespace
    {
        enum class Convert : uint8_t
        {
            Copy,        // same layout on the host
            R5G6B5,      // -> RGBA8
            R5G5B5A1,    // -> RGBA8
            R4G4B4A4,    // -> RGBA8
            R10G10B10A2, // -> RGBA16 unorm
            DXT3A,       // 4-bit alpha blocks -> R8
            CTX1,        // two-channel blocks -> RG8
        };

        struct Format
        {
            RenderFormat host = RenderFormat::UNKNOWN;
            RenderFormat hostSigned = RenderFormat::UNKNOWN; // all components signed
            uint8_t blockWidth = 1;       // texels per block side (4 for block-compressed)
            uint8_t bytesPerBlock = 0;    // guest
            Convert convert = Convert::Copy;
            uint8_t hostBlockWidth = 1;   // after conversion
            uint8_t hostBytesPerBlock = 0;
        };

        Format Lookup(uint32_t format)
        {
            switch (format)
            {
            case 2:  return { RenderFormat::R8_UNORM, RenderFormat::R8_SNORM, 1, 1, Convert::Copy, 1, 1 };            // k_8
            case 3:  return { RenderFormat::R8G8B8A8_UNORM, RenderFormat::UNKNOWN, 1, 2, Convert::R5G5B5A1, 1, 4 };   // k_1_5_5_5
            case 4:  return { RenderFormat::R8G8B8A8_UNORM, RenderFormat::UNKNOWN, 1, 2, Convert::R5G6B5, 1, 4 };     // k_5_6_5
            case 6:
            case 50: return { RenderFormat::R8G8B8A8_UNORM, RenderFormat::R8G8B8A8_SNORM, 1, 4, Convert::Copy, 1, 4 }; // k_8_8_8_8
            case 7:
            case 54: return { RenderFormat::R16G16B16A16_UNORM, RenderFormat::UNKNOWN, 1, 4, Convert::R10G10B10A2, 1, 8 }; // k_2_10_10_10
            case 10: return { RenderFormat::R8G8_UNORM, RenderFormat::R8G8_SNORM, 1, 2, Convert::Copy, 1, 2 };        // k_8_8
            case 15: return { RenderFormat::R8G8B8A8_UNORM, RenderFormat::UNKNOWN, 1, 2, Convert::R4G4B4A4, 1, 4 };   // k_4_4_4_4
            case 18:
            case 51: return { RenderFormat::BC1_UNORM, RenderFormat::UNKNOWN, 4, 8, Convert::Copy, 4, 8 };            // k_DXT1
            case 19:
            case 52: return { RenderFormat::BC2_UNORM, RenderFormat::UNKNOWN, 4, 16, Convert::Copy, 4, 16 };          // k_DXT2_3
            case 20:
            case 53: return { RenderFormat::BC3_UNORM, RenderFormat::UNKNOWN, 4, 16, Convert::Copy, 4, 16 };          // k_DXT4_5
            case 24: return { RenderFormat::R16_UNORM, RenderFormat::R16_SNORM, 1, 2, Convert::Copy, 1, 2 };          // k_16
            case 25: return { RenderFormat::R16G16_UNORM, RenderFormat::R16G16_SNORM, 1, 4, Convert::Copy, 1, 4 };    // k_16_16
            case 26: return { RenderFormat::R16G16B16A16_UNORM, RenderFormat::R16G16B16A16_SNORM, 1, 8, Convert::Copy, 1, 8 }; // k_16_16_16_16
            case 30: return { RenderFormat::R16_FLOAT, RenderFormat::UNKNOWN, 1, 2, Convert::Copy, 1, 2 };            // k_16_FLOAT
            case 31: return { RenderFormat::R16G16_FLOAT, RenderFormat::UNKNOWN, 1, 4, Convert::Copy, 1, 4 };         // k_16_16_FLOAT
            case 32: return { RenderFormat::R16G16B16A16_FLOAT, RenderFormat::UNKNOWN, 1, 8, Convert::Copy, 1, 8 };   // k_16_16_16_16_FLOAT
            case 33: return { RenderFormat::R32_UINT, RenderFormat::UNKNOWN, 1, 4, Convert::Copy, 1, 4 };             // k_32
            case 36: return { RenderFormat::R32_FLOAT, RenderFormat::UNKNOWN, 1, 4, Convert::Copy, 1, 4 };            // k_32_FLOAT
            case 37: return { RenderFormat::R32G32_FLOAT, RenderFormat::UNKNOWN, 1, 8, Convert::Copy, 1, 8 };         // k_32_32_FLOAT
            case 38: return { RenderFormat::R32G32B32A32_FLOAT, RenderFormat::UNKNOWN, 1, 16, Convert::Copy, 1, 16 }; // k_32_32_32_32_FLOAT
            case 49: return { RenderFormat::BC5_UNORM, RenderFormat::BC5_SNORM, 4, 16, Convert::Copy, 4, 16 };        // k_DXN
            case 58: return { RenderFormat::R8_UNORM, RenderFormat::UNKNOWN, 4, 8, Convert::DXT3A, 1, 1 };            // k_DXT3A
            case 59: return { RenderFormat::BC4_UNORM, RenderFormat::BC4_SNORM, 4, 8, Convert::Copy, 4, 8 };          // k_DXT5A
            case 60: return { RenderFormat::R8G8_UNORM, RenderFormat::UNKNOWN, 4, 8, Convert::CTX1, 1, 2 };           // k_CTX1
            default: return {};
            }
        }

        uint32_t Log2(uint32_t value)
        {
            uint32_t result = 0;
            while ((1u << result) < value)
                result++;
            return result;
        }

        uint32_t Align(uint32_t value, uint32_t alignment)
        {
            return (value + alignment - 1) / alignment * alignment;
        }

        // Byte offset of block (x, y) in a tiled surface whose pitch (in blocks) is a multiple of 32.
        uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t pitch, uint32_t log2Bpb)
        {
            const uint32_t macroRow = ((y >> 5) * (pitch >> 5)) << (log2Bpb + 7);
            const uint32_t microRow = ((y & 6) << 2) << log2Bpb;
            const uint32_t rowOffset = macroRow + ((microRow & ~15u) << 1) + (microRow & 15) + ((y & 8) << (3 + log2Bpb)) + ((y & 1) << 4);

            const uint32_t macro = (x >> 5) << (log2Bpb + 7);
            const uint32_t micro = (x & 7) << log2Bpb;
            const uint32_t offset = rowOffset + macro + ((micro & ~15u) << 1) + (micro & 15);
            return ((offset & ~511u) << 3) + ((offset & 448) << 2) + (offset & 63) + ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
        }

        void SwapEndian(uint8_t* data, size_t size, uint32_t endian)
        {
            switch (endian)
            {
            case 1: // 8in16
                for (size_t i = 0; i + 2 <= size; i += 2)
                    std::swap(data[i], data[i + 1]);
                break;
            case 2: // 8in32
                for (size_t i = 0; i + 4 <= size; i += 4)
                    *reinterpret_cast<uint32_t*>(data + i) = __builtin_bswap32(*reinterpret_cast<uint32_t*>(data + i));
                break;
            case 3: // 16in32
                for (size_t i = 0; i + 4 <= size; i += 4)
                {
                    uint32_t v = *reinterpret_cast<uint32_t*>(data + i);
                    *reinterpret_cast<uint32_t*>(data + i) = (v >> 16) | (v << 16);
                }
                break;
            default:
                break;
            }
        }

        uint8_t Expand(uint32_t value, uint32_t bits)
        {
            return uint8_t((value * 255 + ((1u << bits) - 1) / 2) / ((1u << bits) - 1));
        }

        // Converts one guest block (after endian swap) into host texels at dst (host row pitch given).
        void ConvertBlock(const Format& f, const uint8_t* src, uint8_t* dst, uint32_t dstPitch, uint32_t texelsX, uint32_t texelsY)
        {
            switch (f.convert)
            {
            case Convert::Copy:
                memcpy(dst, src, f.bytesPerBlock);
                break;
            case Convert::R5G6B5:
            {
                const uint16_t v = *reinterpret_cast<const uint16_t*>(src);
                dst[0] = Expand(v & 31, 5); dst[1] = Expand((v >> 5) & 63, 6); dst[2] = Expand((v >> 11) & 31, 5); dst[3] = 255;
                break;
            }
            case Convert::R5G5B5A1:
            {
                const uint16_t v = *reinterpret_cast<const uint16_t*>(src);
                dst[0] = Expand(v & 31, 5); dst[1] = Expand((v >> 5) & 31, 5); dst[2] = Expand((v >> 10) & 31, 5); dst[3] = (v >> 15) ? 255 : 0;
                break;
            }
            case Convert::R4G4B4A4:
            {
                const uint16_t v = *reinterpret_cast<const uint16_t*>(src);
                dst[0] = Expand(v & 15, 4); dst[1] = Expand((v >> 4) & 15, 4); dst[2] = Expand((v >> 8) & 15, 4); dst[3] = Expand(v >> 12, 4);
                break;
            }
            case Convert::R10G10B10A2:
            {
                const uint32_t v = *reinterpret_cast<const uint32_t*>(src);
                uint16_t* out = reinterpret_cast<uint16_t*>(dst);
                out[0] = uint16_t((v & 1023) * 65535 / 1023);
                out[1] = uint16_t(((v >> 10) & 1023) * 65535 / 1023);
                out[2] = uint16_t(((v >> 20) & 1023) * 65535 / 1023);
                out[3] = uint16_t((v >> 30) * 65535 / 3);
                break;
            }
            case Convert::DXT3A:
            {
                uint64_t bits;
                memcpy(&bits, src, 8);
                for (uint32_t y = 0; y < texelsY; y++)
                    for (uint32_t x = 0; x < texelsX; x++)
                        dst[y * dstPitch + x] = uint8_t(((bits >> (4 * (y * 4 + x))) & 15) * 17);
                break;
            }
            case Convert::CTX1:
            {
                const uint8_t c[4][2] = {
                    { src[0], src[1] }, { src[2], src[3] },
                    { uint8_t((2 * src[0] + src[2]) / 3), uint8_t((2 * src[1] + src[3]) / 3) },
                    { uint8_t((src[0] + 2 * src[2]) / 3), uint8_t((src[1] + 2 * src[3]) / 3) } };
                uint32_t indices;
                memcpy(&indices, src + 4, 4);
                for (uint32_t y = 0; y < texelsY; y++)
                    for (uint32_t x = 0; x < texelsX; x++)
                    {
                        const uint32_t i = (indices >> (2 * (y * 4 + x))) & 3;
                        dst[y * dstPitch + x * 2] = c[i][0];
                        dst[y * dstPitch + x * 2 + 1] = c[i][1];
                    }
                break;
            }
            }
        }

        struct Layout
        {
            Format format;
            uint32_t blocksX, blocksY, pitchBlocks, log2Bpb;
            uint32_t sliceSize, layerStride, layers;
            uint32_t levels;      // mip levels decoded (1: base only)
            uint32_t packedLevel; // first level stored in the packed tail (UINT32_MAX: none)
        };

        uint32_t Log2Floor(uint32_t value)
        {
            uint32_t result = 0;
            while ((value >> (result + 1)) != 0)
                result++;
            return result;
        }

        // Xenia's GetPackedMipOffset (2D): once the shorter side of a level is 16 texels or less,
        // that level and all smaller ones share one 32x32 tile, each at its own position. Block
        // offset of 'level' within that tile; false if the level is not packed.
        bool PackedMipOffset(uint32_t width, uint32_t height, uint32_t blockWidth, uint32_t level, uint32_t& xBlocks, uint32_t& yBlocks)
        {
            const uint32_t log2W = Log2(width), log2H = Log2(height), log2Size = std::min(log2W, log2H);
            xBlocks = yBlocks = 0;
            if (log2Size > 4 + level)
                return false;
            const uint32_t packedBase = log2Size > 4 ? log2Size - 4 : 0;
            const uint32_t packed = level - packedBase;
            uint32_t x = 0, y = 0;
            if (packed < 3)
            {
                if (log2W > log2H)
                    y = 16 >> packed; // wider than tall: down the tile
                else
                    x = 16 >> packed; // taller than wide or square: across
            }
            else if (log2W > log2H)
                x = (1u << (log2W - packedBase)) >> (packed - 2);
            else
                y = (1u << (log2H - packedBase)) >> (packed - 2);
            xBlocks = x / blockWidth;
            yBlocks = y / blockWidth;
            return true;
        }

        // A mip surface stored from MipAddress(): levels 1+ each have their own surface, sized from
        // the base rounded up to powers of two (Xenia's GetGuestTextureLayout); the packed tail is
        // one surface holding every level from packedLevel down.
        struct MipSurface
        {
            uint32_t offset;      // bytes from MipAddress()
            uint32_t pitchBlocks; // row pitch (tiled: multiple of 32; linear: rows 256-byte aligned)
            uint32_t sliceSize;   // bytes of one layer
            uint32_t stride;      // bytes between layers
        };

        MipSurface GetMipSurface(const FetchConstant& fetch, const Layout& layout, uint32_t level)
        {
            const Format& f = layout.format;
            const uint32_t width = fetch.Width(), height = fetch.Dimension() == 0 ? 1 : fetch.Height();
            // The surface that holds 'level': its own, or the packed tail's.
            const uint32_t stored = level >= layout.packedLevel ? layout.packedLevel : level;
            MipSurface surface{ 0, 0, 0, 0 };
            for (uint32_t l = layout.packedLevel == 0 ? 0 : 1; l <= stored; l++)
            {
                const uint32_t pitchTexels = std::max((1u << Log2(width)) >> l, 1u);
                const uint32_t rowsTexels = std::max((1u << Log2(height)) >> l, 1u);
                uint32_t pitchBlocks = Align((pitchTexels + f.blockWidth - 1) / f.blockWidth, 32);
                if (!fetch.Tiled())
                    pitchBlocks = Align(pitchBlocks * f.bytesPerBlock, 256) / f.bytesPerBlock;
                const uint32_t rows = Align((rowsTexels + f.blockWidth - 1) / f.blockWidth, 32);
                const uint32_t sliceSize = pitchBlocks * rows * f.bytesPerBlock;
                const uint32_t stride = Align(sliceSize, 4096);
                if (l == stored)
                {
                    surface.pitchBlocks = pitchBlocks;
                    surface.sliceSize = sliceSize;
                    surface.stride = stride;
                    break;
                }
                surface.offset += stride * layout.layers;
            }
            return surface;
        }

        bool GetLayout(const FetchConstant& fetch, Layout& layout, const char*& reason)
        {
            layout.format = Lookup(fetch.Format());
            if (layout.format.bytesPerBlock == 0)
            {
                reason = "format";
                return false;
            }

            const uint32_t dimension = fetch.Dimension();
            if (dimension == 2)
            {
                reason = "3D";
                return false;
            }
            layout.layers = dimension == 3 ? 6 : 1;

            const Format& f = layout.format;
            const uint32_t width = fetch.Width();
            const uint32_t height = dimension == 0 ? 1 : fetch.Height();
            layout.blocksX = (width + f.blockWidth - 1) / f.blockWidth;
            layout.blocksY = (height + f.blockWidth - 1) / f.blockWidth;
            const uint32_t pitchTexels = std::max(fetch.Pitch(), width);
            layout.pitchBlocks = (pitchTexels + f.blockWidth - 1) / f.blockWidth;
            layout.log2Bpb = Log2(f.bytesPerBlock);

            layout.packedLevel = fetch.PackedMips() && dimension != 0 ? (Log2(std::min(width, height)) > 4 ? Log2(std::min(width, height)) - 4 : 0) : UINT32_MAX;
            if (fetch.Tiled())
            {
                layout.pitchBlocks = Align(layout.pitchBlocks, 32);
                layout.sliceSize = layout.pitchBlocks * Align(layout.blocksY, 32) * f.bytesPerBlock;
            }
            else if (layout.packedLevel == 0)
            {
                // The base sits somewhere in its packed tile: the whole 32-block-high tile.
                layout.sliceSize = layout.pitchBlocks * Align(layout.blocksY, 32) * f.bytesPerBlock;
            }
            else
            {
                layout.sliceSize = layout.pitchBlocks * layout.blocksY * f.bytesPerBlock;
            }
            layout.layerStride = Align(layout.sliceSize, 4096);

            // Mip levels (Xenia's GetSubresourcesFromFetchConstant): none without a mip address.
            layout.levels = 1;
            if (fetch.MipAddress() != 0 && fetch.MipMaxLevel() != 0)
                layout.levels = std::min({ fetch.MipMaxLevel(), Log2Floor(std::max(width, height)), kMaxLevels - 1 }) + 1;
            return true;
        }
    }

    namespace
    {
        void Rgb565(uint16_t v, uint8_t out[4])
        {
            out[0] = Expand(v >> 11, 5);
            out[1] = Expand((v >> 5) & 63, 6);
            out[2] = Expand(v & 31, 5);
            out[3] = 255;
        }

        // BC1 colour block -> 4x4 RGBA (alpha from the 3-colour mode only).
        void DecodeBC1Block(const uint8_t* block, uint8_t texels[16][4], bool forceFourColor)
        {
            uint16_t c0, c1;
            uint32_t indices;
            memcpy(&c0, block, 2);
            memcpy(&c1, block + 2, 2);
            memcpy(&indices, block + 4, 4);
            uint8_t palette[4][4];
            Rgb565(c0, palette[0]);
            Rgb565(c1, palette[1]);
            if (c0 > c1 || forceFourColor)
            {
                for (int c = 0; c < 3; c++)
                {
                    palette[2][c] = uint8_t((2 * palette[0][c] + palette[1][c]) / 3);
                    palette[3][c] = uint8_t((palette[0][c] + 2 * palette[1][c]) / 3);
                }
                palette[2][3] = palette[3][3] = 255;
            }
            else
            {
                for (int c = 0; c < 3; c++)
                {
                    palette[2][c] = uint8_t((palette[0][c] + palette[1][c]) / 2);
                    palette[3][c] = 0;
                }
                palette[2][3] = 255;
                palette[3][3] = 0;
            }
            for (int i = 0; i < 16; i++)
                memcpy(texels[i], palette[(indices >> (2 * i)) & 3], 4);
        }

        void DecodeBC3AlphaBlock(const uint8_t* block, uint8_t texels[16][4])
        {
            const uint8_t a0 = block[0], a1 = block[1];
            uint64_t bits = 0;
            memcpy(&bits, block + 2, 6);
            uint8_t palette[8] = { a0, a1 };
            if (a0 > a1)
                for (int i = 1; i < 7; i++)
                    palette[i + 1] = uint8_t(((7 - i) * a0 + i * a1) / 7);
            else
            {
                for (int i = 1; i < 5; i++)
                    palette[i + 1] = uint8_t(((5 - i) * a0 + i * a1) / 5);
                palette[6] = 0;
                palette[7] = 255;
            }
            for (int i = 0; i < 16; i++)
                texels[i][3] = palette[(bits >> (3 * i)) & 7];
        }
    }

    bool ToRGBA8(const Decoded& d, std::vector<uint8_t>& rgba)
    {
        rgba.assign(size_t(d.width) * d.height * 4, 0);
        auto put = [&](uint32_t x, uint32_t y, const uint8_t* texel)
        {
            if (x < d.width && y < d.height)
                memcpy(&rgba[(size_t(y) * d.width + x) * 4], texel, 4);
        };

        switch (d.format)
        {
        case RenderFormat::BC1_UNORM:
        case RenderFormat::BC2_UNORM:
        case RenderFormat::BC3_UNORM:
        {
            const uint32_t blockBytes = d.format == RenderFormat::BC1_UNORM ? 8 : 16;
            for (uint32_t by = 0; by < (d.height + 3) / 4; by++)
                for (uint32_t bx = 0; bx < (d.width + 3) / 4; bx++)
                {
                    const uint8_t* block = d.data.data() + size_t(by) * d.rowPitch + size_t(bx) * blockBytes;
                    uint8_t texels[16][4];
                    if (d.format == RenderFormat::BC1_UNORM)
                        DecodeBC1Block(block, texels, false);
                    else
                    {
                        DecodeBC1Block(block + 8, texels, true);
                        if (d.format == RenderFormat::BC3_UNORM)
                            DecodeBC3AlphaBlock(block, texels);
                        else
                            for (int i = 0; i < 16; i++)
                                texels[i][3] = uint8_t(((block[i / 2] >> (4 * (i & 1))) & 15) * 17);
                    }
                    for (int i = 0; i < 16; i++)
                        put(bx * 4 + (i & 3), by * 4 + (i >> 2), texels[i]);
                }
            return true;
        }
        case RenderFormat::BC4_UNORM:
        case RenderFormat::BC5_UNORM:
        {
            // Single / two channels, each a BC3 alpha block: grey, or red and green.
            const bool two = d.format == RenderFormat::BC5_UNORM;
            const uint32_t blockBytes = two ? 16 : 8;
            for (uint32_t by = 0; by < (d.height + 3) / 4; by++)
                for (uint32_t bx = 0; bx < (d.width + 3) / 4; bx++)
                {
                    const uint8_t* block = d.data.data() + size_t(by) * d.rowPitch + size_t(bx) * blockBytes;
                    uint8_t red[16][4], green[16][4], texels[16][4];
                    DecodeBC3AlphaBlock(block, red);
                    if (two)
                        DecodeBC3AlphaBlock(block + 8, green);
                    for (int i = 0; i < 16; i++)
                    {
                        texels[i][0] = red[i][3];
                        texels[i][1] = two ? green[i][3] : red[i][3];
                        texels[i][2] = two ? 0 : red[i][3];
                        texels[i][3] = 255;
                        put(bx * 4 + (i & 3), by * 4 + (i >> 2), texels[i]);
                    }
                }
            return true;
        }
        case RenderFormat::R8G8B8A8_UNORM:
            for (uint32_t y = 0; y < d.height; y++)
                for (uint32_t x = 0; x < d.width; x++)
                    put(x, y, d.data.data() + size_t(y) * d.rowPitch + size_t(x) * 4);
            return true;
        case RenderFormat::R8_UNORM:
        case RenderFormat::R8G8_UNORM:
        {
            const uint32_t bpp = d.format == RenderFormat::R8_UNORM ? 1 : 2;
            for (uint32_t y = 0; y < d.height; y++)
                for (uint32_t x = 0; x < d.width; x++)
                {
                    const uint8_t* p = d.data.data() + size_t(y) * d.rowPitch + size_t(x) * bpp;
                    const uint8_t texel[4] = { p[0], bpp == 2 ? p[1] : p[0], bpp == 2 ? uint8_t(0) : p[0], 255 };
                    put(x, y, texel);
                }
            return true;
        }
        default:
            return false;
        }
    }

    uint32_t GuestSize(const FetchConstant& fetch)
    {
        Layout layout;
        const char* reason;
        if (!GetLayout(fetch, layout, reason))
            return 0;
        return layout.layerStride * (layout.layers - 1) + layout.sliceSize;
    }

    uint32_t LevelCount(const FetchConstant& fetch)
    {
        Layout layout;
        const char* reason;
        return GetLayout(fetch, layout, reason) ? layout.levels : 1;
    }

    uint32_t MipsSize(const FetchConstant& fetch)
    {
        Layout layout;
        const char* reason;
        if (!GetLayout(fetch, layout, reason) || layout.levels < 2)
            return 0;
        const MipSurface last = GetMipSurface(fetch, layout, layout.levels - 1);
        return last.offset + last.stride * (layout.layers - 1) + last.sliceSize;
    }

    bool Decode(const FetchConstant& fetch, const uint8_t* source, const uint8_t* mips, Decoded& out, const char*& reason)
    {
        Layout layout;
        if (!GetLayout(fetch, layout, reason))
            return false;
        if (mips == nullptr)
            layout.levels = 1;

        const Format& f = layout.format;

        // Signed formats (normal maps): every component the format has is signed.
        bool allSigned = true;
        for (uint32_t c = 0; c < 4; c++)
            if (((fetch.Signs() >> (c * 2)) & 3) != 1)
                allSigned = false;
        out.format = (allSigned && f.hostSigned != RenderFormat::UNKNOWN) ? f.hostSigned : f.host;

        out.width = fetch.Width();
        out.height = fetch.Dimension() == 0 ? 1 : fetch.Height();
        out.layers = layout.layers;
        out.cube = layout.layers == 6;
        out.levels = layout.levels;

        // Host layout: level after level, each with its layers; rows 256-byte, levels and layers
        // 512-byte aligned (upload footprints).
        uint32_t size = 0;
        for (uint32_t l = 0; l < out.levels; l++)
        {
            Decoded::Level& level = out.level[l];
            level.width = std::max(out.width >> l, 1u);
            level.height = std::max(out.height >> l, 1u);
            const uint32_t hostBlocksX = (level.width + f.hostBlockWidth - 1) / f.hostBlockWidth;
            const uint32_t hostBlocksY = (level.height + f.hostBlockWidth - 1) / f.hostBlockWidth;
            level.rowPitch = Align(hostBlocksX * f.hostBytesPerBlock, 256);
            level.rowWidth = level.rowPitch / f.hostBytesPerBlock * f.hostBlockWidth;
            level.layerPitch = Align(level.rowPitch * hostBlocksY, 512);
            level.offset = size;
            size += level.layerPitch * out.layers;
        }
        out.rowPitch = out.level[0].rowPitch;
        out.rowWidth = out.level[0].rowWidth;
        out.layerPitch = out.level[0].layerPitch;
        out.data.assign(size, 0);

        static thread_local std::vector<uint8_t> slice;

        // One level from a guest surface: its blocks start at block (x0, y0) of the surface.
        auto decodeLevel = [&](uint32_t l, const uint8_t* surface, uint32_t pitchBlocks, uint32_t sliceSize, uint32_t layerStride, uint32_t x0, uint32_t y0)
        {
            const Decoded::Level& level = out.level[l];
            const uint32_t blocksX = (level.width + f.blockWidth - 1) / f.blockWidth;
            const uint32_t blocksY = (level.height + f.blockWidth - 1) / f.blockWidth;
            slice.resize(sliceSize);
            for (uint32_t layer = 0; layer < layout.layers; layer++)
            {
                memcpy(slice.data(), surface + size_t(layer) * layerStride, sliceSize);
                SwapEndian(slice.data(), slice.size(), fetch.Endian());

                uint8_t* dstLayer = out.data.data() + level.offset + size_t(layer) * level.layerPitch;
                for (uint32_t by = 0; by < blocksY; by++)
                {
                    for (uint32_t bx = 0; bx < blocksX; bx++)
                    {
                        const uint32_t offset = fetch.Tiled()
                            ? TiledOffset(x0 + bx, y0 + by, pitchBlocks, layout.log2Bpb)
                            : ((y0 + by) * pitchBlocks + x0 + bx) * f.bytesPerBlock;
                        if (offset + f.bytesPerBlock > sliceSize)
                            continue;

                        // Host position: block formats keep blocks, conversions expand to texels.
                        uint8_t* dst;
                        uint32_t texelsX = 1, texelsY = 1;
                        if (f.hostBlockWidth == f.blockWidth)
                            dst = dstLayer + size_t(by) * level.rowPitch + size_t(bx) * f.hostBytesPerBlock;
                        else
                        {
                            dst = dstLayer + size_t(by * f.blockWidth) * level.rowPitch + size_t(bx * f.blockWidth) * f.hostBytesPerBlock;
                            texelsX = std::min<uint32_t>(f.blockWidth, level.width - bx * f.blockWidth);
                            texelsY = std::min<uint32_t>(f.blockWidth, level.height - by * f.blockWidth);
                        }
                        ConvertBlock(f, slice.data() + offset, dst, level.rowPitch, texelsX, texelsY);
                    }
                }
            }
        };

        // Base level: at the start of its surface, or at its place in the packed tile.
        uint32_t x0 = 0, y0 = 0;
        if (layout.packedLevel == 0)
            PackedMipOffset(out.width, out.height, f.blockWidth, 0, x0, y0);
        decodeLevel(0, source, layout.pitchBlocks, layout.sliceSize, layout.layerStride, x0, y0);

        for (uint32_t l = 1; l < out.levels; l++)
        {
            const MipSurface surface = GetMipSurface(fetch, layout, l);
            x0 = y0 = 0;
            if (l >= layout.packedLevel)
                PackedMipOffset(out.width, out.height, f.blockWidth, l, x0, y0);
            decodeLevel(l, mips + surface.offset, surface.pitchBlocks, surface.sliceSize, surface.stride, x0, y0);
        }

        // Xenos fills the components a format lacks with its last one before the fetch swizzle
        // picks from them (Xenia: k_8 / k_16 / DXT5A read RRRR, two-component formats RGGG, 5_6_5
        // RGBB). The host formats have 0 / 1 there instead: the game's DXT5A gloss masks, read with
        // an XYZW swizzle as a colour, came out (v, 0, 0) and every reflection they mask turned red.
        RenderSwizzle components[4] = { RenderSwizzle::R, RenderSwizzle::G, RenderSwizzle::B, RenderSwizzle::A };
        switch (fetch.Format())
        {
        case 2:  // k_8
        case 24: // k_16
        case 30: // k_16_FLOAT
        case 36: // k_32_FLOAT
        case 58: // k_DXT3A
        case 59: // k_DXT5A
            components[1] = components[2] = components[3] = RenderSwizzle::R;
            break;
        case 10: // k_8_8
        case 25: // k_16_16
        case 31: // k_16_16_FLOAT
        case 37: // k_32_32_FLOAT
        case 49: // k_DXN
        case 60: // k_CTX1
            components[2] = components[3] = RenderSwizzle::G;
            break;
        case 4:  // k_5_6_5 (decoded to RGBA8)
            components[3] = RenderSwizzle::B;
            break;
        }

        // Fetch constant swizzle: per output component, which of X Y Z W or 0 / 1.
        for (uint32_t c = 0; c < 4; c++)
        {
            const uint32_t select = (fetch.Swizzle() >> (c * 3)) & 7;
            if (select < 4)
                out.swizzle[c] = components[select];
            else
                out.swizzle[c] = select == 5 ? RenderSwizzle::ONE : RenderSwizzle::ZERO;
        }
        return true;
    }
}

namespace TextureDecode
{
    uint32_t TexelOffset(const FetchConstant& fetch, uint32_t x, uint32_t y, uint32_t bytesPerTexel)
    {
        const uint32_t log2Bpb = Log2(bytesPerTexel);
        uint32_t pitch = std::max(fetch.Pitch(), fetch.Width());
        if (fetch.Tiled())
        {
            pitch = Align(pitch, 32);
            return TiledOffset(x, y, pitch, log2Bpb);
        }
        return (y * pitch + x) * bytesPerTexel;
    }
}
