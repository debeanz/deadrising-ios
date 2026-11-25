#pragma once

// Reading Dead Rising's D3D device and resource objects in guest memory (layout: docs/d3d_map.md).

#include <cstdint>
#include <kernel/memory.h>
#include <string>

namespace GuestDevice
{
    inline uint32_t Load32(uint32_t address)
    {
        return __builtin_bswap32(*static_cast<const uint32_t*>(g_memory.Translate(address)));
    }

    inline float LoadFloat(uint32_t address)
    {
        uint32_t value = Load32(address);
        float result;
        memcpy(&result, &value, sizeof(result));
        return result;
    }

    inline uint8_t Load8(uint32_t address)
    {
        return *static_cast<const uint8_t*>(g_memory.Translate(address));
    }

    inline uint16_t Load16(uint32_t address)
    {
        return __builtin_bswap16(*static_cast<const uint16_t*>(g_memory.Translate(address)));
    }

    inline void Store8(uint32_t address, uint8_t value)
    {
        *static_cast<uint8_t*>(g_memory.Translate(address)) = value;
    }

    inline void Store16(uint32_t address, uint16_t value)
    {
        *static_cast<uint16_t*>(g_memory.Translate(address)) = __builtin_bswap16(value);
    }

    inline void Store32(uint32_t address, uint32_t value)
    {
        *static_cast<uint32_t*>(g_memory.Translate(address)) = __builtin_bswap32(value);
    }

    inline void StoreFloat(uint32_t address, float value)
    {
        uint32_t bits;
        memcpy(&bits, &value, sizeof(bits));
        Store32(address, bits);
    }

    // Device fields.
    enum : uint32_t
    {
        VertexShaderConstants = 0x700,  // 256 float4
        PixelShaderConstants = 0x1700,  // 256 float4
        VertexBooleans = 0x2700,        // bool i = bit (i % 32) of word i / 32
        PixelBooleans = 0x2710,
        IndexBuffer = 0x2F8C,
        RenderTarget0 = 0x2F90,
        DepthStencil = 0x2FA0,          // render targets 0-3 at 0x2F90-0x2F9C
        StreamBuffers = 0x2FA4,         // + 4 n
        StreamStrides = 0x2FE8,         // + n, in dwords
        Textures = 0x2FF8,              // + 4 s, texture object of sampler (fetch constant) s
        Viewport = 0x3060,              // X, Y, Width, Height (u32), MinZ, MaxZ (float)
        PixelShader = 0x3088,
        VertexShader = 0x3364,
    };

    // Vertex fetch constant of stream n (built by SetStreamSource): physical base address of
    // buffer + offset, then size - offset.
    inline uint32_t StreamFetchConstant(uint32_t n) { return 0x670 + (17 - n) * 8; }

    // Xenos register shadows: D3D writes these blocks into the command buffer with sub_8244FA00
    // (register base + k <- device field + 4k). Returns 0 for registers outside the blocks.
    inline uint32_t RegisterField(uint32_t reg)
    {
        struct Block { uint32_t reg, field, count; };
        static constexpr Block blocks[] =
        {
            { 0x2000, 0x2800, 19 }, { 0x2100, 0x284C, 21 }, { 0x2180, 0x28A0, 5 }, { 0x2200, 0x28B4, 12 },
            { 0x2280, 0x28E4, 21 }, { 0x2300, 0x2938, 38 }, { 0x2380, 0x29D0, 16 },
        };
        for (auto& b : blocks)
            if (reg >= b.reg && reg < b.reg + b.count)
                return b.field + (reg - b.reg) * 4;
        return 0;
    }

    inline uint32_t Register(uint32_t device, uint32_t reg)
    {
        uint32_t field = RegisterField(reg);
        return field != 0 ? Load32(device + field) : 0;
    }

    inline float RegisterFloat(uint32_t device, uint32_t reg)
    {
        uint32_t value = Register(device, reg);
        float result;
        memcpy(&result, &value, sizeof(result));
        return result;
    }

    // Xenos registers used by the renderer (names as in Xenia's register table).
    enum : uint32_t
    {
        RB_SURFACE_INFO = 0x2000,
        RB_COLOR_INFO = 0x2001,
        RB_DEPTH_INFO = 0x2002,
        RB_STENCILREFMASK = 0x210D,
        PA_SC_WINDOW_SCISSOR_TL = 0x2081,
        PA_SC_WINDOW_SCISSOR_BR = 0x2082,
        RB_COLOR_MASK = 0x2104,
        RB_BLEND_RED = 0x2105,
        RB_ALPHA_REF = 0x210E,
        RB_DEPTHCONTROL = 0x2200,
        RB_BLENDCONTROL0 = 0x2201,
        RB_COLORCONTROL = 0x2202,
        PA_SU_SC_MODE_CNTL = 0x2205,
        PA_CL_VTE_CNTL = 0x2206,
        RB_MODECONTROL = 0x2208,
    };

    // The physical address D3D derives from a virtual one (0xE0000000+ pages are offset by 4 KB).
    inline uint32_t PhysicalAddress(uint32_t address)
    {
        return (address & 0x1FFFFFFF) + ((address >> 20) >= 0xE00 ? 0x1000 : 0);
    }

    // Resource objects (common header 0x18 bytes).
    // Vertex buffer: its GPU vertex fetch constant - address | type (3), then endian (bits 0-1) and
    // size in dwords (bits 2-25).
    inline uint32_t VertexBufferAddress(uint32_t buffer) { return Load32(buffer + 0x18) & ~3u; }
    inline uint32_t VertexBufferSize(uint32_t buffer) { return Load32(buffer + 0x1C) & 0x03FFFFFC; }
    // Index buffer: address at +0x18.
    inline uint32_t IndexBufferAddress(uint32_t buffer) { return Load32(buffer + 0x18); }
    inline bool IndexBufferIs32Bit(uint32_t buffer) { return (Load32(buffer) & 0x80000000) != 0; }

    // Texture (type 3): its GPU texture fetch constant (6 dwords) at +0x1C. dword1: format (bits
    // 0-5), base address (bits 12-31); dword2 (2D): width - 1 (bits 0-12), height - 1 (bits 13-25).
    inline uint32_t TextureAddress(uint32_t texture) { return Load32(texture + 0x20) & 0xFFFFF000; }
    inline uint32_t TextureFormat(uint32_t texture) { return Load32(texture + 0x20) & 0x3F; }
    inline uint32_t TextureWidth(uint32_t texture) { return (Load32(texture + 0x24) & 0x1FFF) + 1; }
    inline uint32_t TextureHeight(uint32_t texture) { return ((Load32(texture + 0x24) >> 13) & 0x1FFF) + 1; }

    // Surface (type 4), as GetSurfaceDesc (sub_82449760) reads it.
    inline uint32_t SurfaceWidth(uint32_t surface) { return ((Load32(surface + 0x24) >> 18) & 0x3FFF) + 1; }
    inline uint32_t SurfaceHeight(uint32_t surface) { return ((Load32(surface + 0x24) >> 3) & 0x7FFF) + 1; }
    inline uint32_t SurfaceFormat(uint32_t surface) { return Load32(surface + 0x28); }
}

namespace GuestDevice
{
    // MT Framework class name of an object: vtable slot 4 is getDTI ("lis rX,hi / addi r3,rX,lo /
    // blr"), the DTI's name pointer is at +4. Empty when the object does not look like one.
    inline std::string ClassName(uint32_t object)
    {
        if (object < 0x40000000 || object >= 0xC0000000)
            return {};
        const uint32_t vtable = Load32(object);
        if (vtable < 0x82000000 || vtable >= 0x83000000)
            return {};
        const uint32_t getDti = Load32(vtable + 16);
        if (getDti < 0x82000000 || getDti >= 0x83000000)
            return {};
        const uint32_t i0 = Load32(getDti), i1 = Load32(getDti + 4);
        if ((i0 >> 26) != 15 || (i1 >> 26) != 14 || ((i1 >> 21) & 31) != 3) // lis / addi r3
            return {};
        const uint32_t dti = ((i0 & 0xFFFF) << 16) + uint32_t(int32_t(int16_t(i1 & 0xFFFF)));
        const uint32_t name = Load32(dti + 4);
        if (name < 0x82000000 || name >= 0x83000000)
            return {};
        std::string result;
        for (uint32_t a = name; result.size() < 64; a++)
        {
            const char c = char(Load8(a));
            if (c == 0)
                break;
            result += c;
        }
        return result;
    }
}
