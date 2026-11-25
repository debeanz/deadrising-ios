// Host rendering of Dead Rising's draws, stage 1 (see renderer.h and docs/renderer.md).
// Pipeline layout, descriptor heaps and root descriptors follow UnleashedRecomp (GPL-3.0) so the
// XenosRecomp shaders bind the same way they do there.

#include <stdafx.h>
#include <profiler/hitch_stats.h>
#include "renderer.h"
#include "video.h"
#include <apu/audio.h>
#include <ui/options_menu.h>
#include <user/config.h>
#include "guest_device.h"
#include "shader_cache.h"
#include "shaders.h"
#include "texture_decode.h"
#include "blit_spirv.h" // scripts/build_blit_spirv.ps1 (Vulkan)
#include <hid/input.h>
#include <hid/touch_controls.h>
#include <os/logger.h>
#include <os/hang_watchdog.h>
#include <os/memory_info.h>
#include <profiler/cpu_sampler.h>

#include <cfloat>
#include <cmath>
#include <deque>
#include <plume_render_interface.h>
#include <plume_render_interface_builders.h>

#ifdef _WIN32
#include <wrl/client.h>
#include <dxcapi.h>
using Microsoft::WRL::ComPtr;
#endif

using namespace plume;
using namespace GuestDevice;

namespace
{
    constexpr uint32_t kMaxStreams = 15;  // stream 15 is the slot for inputs the declaration lacks
    constexpr uint32_t kDummySlot = 15;
    constexpr uint32_t kSamplerSlots = 32; // shared constants layout (XenosRecomp, Dead Rising patch)

    // Host depth buffers: 32-bit float depth (reversed Z) and 8-bit stencil; depth resolves copy
    // the depth plane (R32 footprint).
    constexpr RenderFormat kDepthFormat = RenderFormat::D32_FLOAT_S8_UINT;
    constexpr RenderFormat kDepthCopyFormat = RenderFormat::R32_TYPELESS;

    // Frame capture (debugging): DR_DRAW_LOG=<guest frame> at start-up, or F12 in the game window
    // (Windows), logs every draw of one frame with its textures and shader constants and dumps its
    // textures (every level and face), render targets and resolves to dump/ (DR_TEX_DUMP=frame for
    // the textures with DR_DRAW_LOG).
    std::atomic<uint32_t> g_captureFrame{ getenv("DR_DRAW_LOG") ? uint32_t(atoi(getenv("DR_DRAW_LOG"))) : UINT32_MAX };
    std::atomic<bool> g_captureTextures{ getenv("DR_TEX_DUMP") != nullptr && strcmp(getenv("DR_TEX_DUMP"), "frame") == 0 };

    constexpr uint32_t kTextureDescriptorCount = 4096;
    constexpr uint32_t kSamplerDescriptorCount = 64;
    constexpr uint32_t kBlitSampler = kSamplerDescriptorCount - 1; // linear, clamped: the final blit (BLIT_SAMPLER)
    enum : uint32_t { DESCRIPTOR_WHITE_2D, DESCRIPTOR_NULL_3D, DESCRIPTOR_NULL_CUBE, DESCRIPTOR_FIRST_FREE };

    // Xbox 360 D3DDECLTYPE values (UnleashedRecomp's video.h).
    enum : uint32_t
    {
        D3DDECLTYPE_FLOAT1 = 0x2C83A4, D3DDECLTYPE_FLOAT2 = 0x2C23A5, D3DDECLTYPE_FLOAT3 = 0x2A23B9,
        D3DDECLTYPE_FLOAT4 = 0x1A23A6, D3DDECLTYPE_D3DCOLOR = 0x182886, D3DDECLTYPE_UBYTE4 = 0x1A2286,
        D3DDECLTYPE_UBYTE4_2 = 0x1A2386, D3DDECLTYPE_SHORT2 = 0x2C2359, D3DDECLTYPE_SHORT4 = 0x1A235A,
        D3DDECLTYPE_UBYTE4N = 0x1A2086, D3DDECLTYPE_UBYTE4N_2 = 0x1A2186, D3DDECLTYPE_SHORT2N = 0x2C2159,
        D3DDECLTYPE_SHORT4N = 0x1A215A, D3DDECLTYPE_USHORT2N = 0x2C2059, D3DDECLTYPE_USHORT4N = 0x1A205A,
        D3DDECLTYPE_UINT1 = 0x2C82A1, D3DDECLTYPE_UDEC3 = 0x2A2287, D3DDECLTYPE_DEC3N = 0x2A2187,
        D3DDECLTYPE_DEC3N_2 = 0x2A2190, D3DDECLTYPE_DEC3N_3 = 0x2A2390, D3DDECLTYPE_FLOAT16_2 = 0x2C235F,
        D3DDECLTYPE_FLOAT16_4 = 0x1A2360, D3DDECLTYPE_UNUSED = 0xFFFFFFFF,
    };

    enum : uint32_t { USAGE_POSITION = 0, USAGE_BLENDWEIGHT = 1, USAGE_BLENDINDICES = 2, USAGE_NORMAL = 3,
        USAGE_TEXCOORD = 5, USAGE_TANGENT = 6, USAGE_BINORMAL = 7, USAGE_COLOR = 10 };

    constexpr const char* kUsageNames[] = { "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD",
        "TANGENT", "BINORMAL", "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE" };

    // Vertex input locations of the translated shaders (XenosRecomp USAGE_LOCATIONS, Dead Rising patch).
    struct Location { uint32_t usage, usageIndex, location; };
    constexpr Location kLocations[] =
    {
        { USAGE_POSITION, 0, 0 }, { USAGE_NORMAL, 0, 1 }, { USAGE_TANGENT, 0, 2 }, { USAGE_BINORMAL, 0, 3 },
        { USAGE_TEXCOORD, 0, 4 }, { USAGE_TEXCOORD, 1, 5 }, { USAGE_TEXCOORD, 2, 6 }, { USAGE_TEXCOORD, 3, 7 },
        { USAGE_COLOR, 0, 8 }, { USAGE_BLENDINDICES, 0, 9 }, { USAGE_BLENDWEIGHT, 0, 10 }, { USAGE_COLOR, 1, 11 },
        { USAGE_BLENDINDICES, 1, 12 }, { USAGE_BLENDWEIGHT, 1, 13 }, { USAGE_TEXCOORD, 4, 14 }, { USAGE_TEXCOORD, 5, 15 },
    };

    // Shader input types (XenosRecomp USAGE_TYPES): these usages are uint4, the rest float4.
    bool IsUintUsage(uint32_t usage)
    {
        return usage == USAGE_BLENDINDICES || usage == USAGE_NORMAL || usage == USAGE_TANGENT || usage == USAGE_BINORMAL;
    }

    enum class Swap : uint8_t { None, Swap16, Swap32 };

    enum class Kind : uint8_t { Float, Byte4, Short, Other }; // for RECTLIST corner arithmetic

    struct DeclType
    {
        RenderFormat format = RenderFormat::UNKNOWN;
        uint8_t size = 0;
        Swap swap = Swap::None;
        Kind kind = Kind::Other;
    };

    DeclType ConvertDeclType(uint32_t type)
    {
        switch (type)
        {
        case D3DDECLTYPE_FLOAT1: return { RenderFormat::R32_FLOAT, 4, Swap::Swap32, Kind::Float };
        case D3DDECLTYPE_FLOAT2: return { RenderFormat::R32G32_FLOAT, 8, Swap::Swap32, Kind::Float };
        case D3DDECLTYPE_FLOAT3: return { RenderFormat::R32G32B32_FLOAT, 12, Swap::Swap32, Kind::Float };
        case D3DDECLTYPE_FLOAT4: return { RenderFormat::R32G32B32A32_FLOAT, 16, Swap::Swap32, Kind::Float };
        // 8-bit formats: the GPU reads a 32-bit word; after swapping it, x is the low byte (as Unleashed).
        case D3DDECLTYPE_D3DCOLOR: return { RenderFormat::B8G8R8A8_UNORM, 4, Swap::Swap32, Kind::Byte4 };
        // Type bit 8 = signed, bit 9 = integer (not normalized): the "_2" variants are signed.
        case D3DDECLTYPE_UBYTE4: return { RenderFormat::R8G8B8A8_UINT, 4, Swap::Swap32, Kind::Byte4 };
        case D3DDECLTYPE_UBYTE4_2: return { RenderFormat::R8G8B8A8_SINT, 4, Swap::Swap32, Kind::Other };
        case D3DDECLTYPE_UBYTE4N: return { RenderFormat::R8G8B8A8_UNORM, 4, Swap::Swap32, Kind::Byte4 };
        case D3DDECLTYPE_UBYTE4N_2: return { RenderFormat::R8G8B8A8_SNORM, 4, Swap::Swap32, Kind::Other };
        // 16-bit formats are swapped per component, so they need no shader-side reordering.
        case D3DDECLTYPE_SHORT2: return { RenderFormat::R16G16_SINT, 4, Swap::Swap16, Kind::Short };
        case D3DDECLTYPE_SHORT4: return { RenderFormat::R16G16B16A16_SINT, 8, Swap::Swap16, Kind::Short };
        case D3DDECLTYPE_SHORT2N: return { RenderFormat::R16G16_SNORM, 4, Swap::Swap16, Kind::Short };
        case D3DDECLTYPE_SHORT4N: return { RenderFormat::R16G16B16A16_SNORM, 8, Swap::Swap16, Kind::Short };
        case D3DDECLTYPE_USHORT2N: return { RenderFormat::R16G16_UNORM, 4, Swap::Swap16, Kind::Short };
        case D3DDECLTYPE_USHORT4N: return { RenderFormat::R16G16B16A16_UNORM, 8, Swap::Swap16, Kind::Short };
        case D3DDECLTYPE_FLOAT16_2: return { RenderFormat::R16G16_FLOAT, 4, Swap::Swap16, Kind::Other };
        case D3DDECLTYPE_FLOAT16_4: return { RenderFormat::R16G16B16A16_FLOAT, 8, Swap::Swap16, Kind::Other };
        case D3DDECLTYPE_UINT1:
        case D3DDECLTYPE_UDEC3:
        case D3DDECLTYPE_DEC3N:
        case D3DDECLTYPE_DEC3N_2:
        case D3DDECLTYPE_DEC3N_3: return { RenderFormat::R32_UINT, 4, Swap::Swap32, Kind::Other };
        default:
            // Packed 32-bit formats the declaration table above does not name (converted on the CPU).
            switch (type & 0x3F)
            {
            case 7: case 16: case 17: return { RenderFormat::R32_UINT, 4, Swap::Swap32, Kind::Other };
            default: return {};
            }
        }
    }

    // Packed vertex formats hosts cannot read (2_10_10_10, 10_11_11, 11_11_10), and anything that
    // is not 32-bit float feeding a uint4 input (NORMAL/TANGENT/BINORMAL: the translated shaders
    // read those as raw float bits), are converted to float4 on the CPU into an extra vertex stream
    // per guest stream (slot kAuxSlotBase + stream).
    constexpr uint32_t kAuxSlotBase = 8;

    float DecodeComponent(uint32_t bits, uint32_t width, bool isSigned, bool isInteger)
    {
        if (isSigned)
        {
            const int32_t value = int32_t(bits << (32 - width)) >> (32 - width);
            return isInteger ? float(value) : std::max(float(value) / float((1u << (width - 1)) - 1), -1.0f);
        }
        return isInteger ? float(bits) : float(bits) / float((1u << width) - 1);
    }

    float HalfBitsToFloat(uint16_t h)
    {
        const uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
        float value = exponent == 0 ? ldexpf(float(mantissa), -24) : exponent == 31 ? 65504.0f : ldexpf(float(mantissa | 0x400), int(exponent) - 25);
        return sign ? -value : value;
    }

    // Decodes one element (already byte-swapped to host order) of the given D3DDECLTYPE.
    void DecodeElement(uint32_t type, const uint8_t* p, float out[4])
    {
        const uint32_t format = type & 0x3F;
        const bool isSigned = (type & 0x100) != 0, isInteger = (type & 0x200) != 0;
        out[0] = out[1] = out[2] = 0.0f;
        out[3] = 1.0f;
        uint32_t v;
        memcpy(&v, p, 4);
        switch (format)
        {
        case 6: // 8_8_8_8: x is the low byte of the swapped word
            for (int c = 0; c < 4; c++)
                out[c] = DecodeComponent((v >> (8 * c)) & 0xFF, 8, isSigned, isInteger);
            break;
        case 7: // 2_10_10_10
            out[0] = DecodeComponent(v & 0x3FF, 10, isSigned, isInteger);
            out[1] = DecodeComponent((v >> 10) & 0x3FF, 10, isSigned, isInteger);
            out[2] = DecodeComponent((v >> 20) & 0x3FF, 10, isSigned, isInteger);
            out[3] = DecodeComponent(v >> 30, 2, isSigned, isInteger);
            break;
        case 16: // 10_11_11: x 11 bits, y 11 bits, z 10 bits from the LSB
            out[0] = DecodeComponent(v & 0x7FF, 11, isSigned, isInteger);
            out[1] = DecodeComponent((v >> 11) & 0x7FF, 11, isSigned, isInteger);
            out[2] = DecodeComponent(v >> 22, 10, isSigned, isInteger);
            break;
        case 17: // 11_11_10: x 10 bits, y 11 bits, z 11 bits from the LSB
            out[0] = DecodeComponent(v & 0x3FF, 10, isSigned, isInteger);
            out[1] = DecodeComponent((v >> 10) & 0x7FF, 11, isSigned, isInteger);
            out[2] = DecodeComponent(v >> 21, 11, isSigned, isInteger);
            break;
        case 25: case 26: // 16_16(_16_16), swapped per component
            for (int c = 0; c < (format == 25 ? 2 : 4); c++)
            {
                uint16_t h;
                memcpy(&h, p + 2 * c, 2);
                out[c] = DecodeComponent(h, 16, isSigned, isInteger);
            }
            break;
        case 31: case 32: // 16-bit float
            for (int c = 0; c < (format == 31 ? 2 : 4); c++)
            {
                uint16_t h;
                memcpy(&h, p + 2 * c, 2);
                out[c] = HalfBitsToFloat(h);
            }
            break;
        case 36: case 37: case 38: case 57: // 32-bit float
            memcpy(out, p, format == 36 ? 4 : format == 37 ? 8 : format == 57 ? 12 : 16);
            break;
        default:
            break;
        }
    }

    struct DeclElement
    {
        uint32_t stream, offset, type, usage, usageIndex;
        DeclType info;
        bool converted = false;  // decoded to float4 into the auxiliary stream
        uint32_t auxOffset = 0;
    };

    struct VertexDeclaration
    {
        uint32_t guest = 0;
        std::vector<DeclElement> elements;
        std::vector<RenderInputElement> inputElements; // with placeholders on the dummy slot
        uint32_t streamMask = 0;  // guest streams
        uint32_t auxMask = 0;     // auxiliary host slots (kAuxSlotBase + stream)
        uint32_t auxStride[kAuxSlotBase] = {};
        int positionElement = -1;
    };

    std::mutex g_declarationMutex;
    ankerl::unordered_dense::map<uint32_t, std::unique_ptr<VertexDeclaration>> g_declarations;

    const VertexDeclaration* FindDeclaration(uint32_t guest)
    {
        std::lock_guard lock(g_declarationMutex);
        auto it = g_declarations.find(guest);
        return it != g_declarations.end() ? it->second.get() : nullptr;
    }

    // ---- Recording (guest thread) -----------------------------------------------------------

    struct PipelineKey
    {
        const ShaderCacheEntry* vertexShader;
        const ShaderCacheEntry* pixelShader;
        const VertexDeclaration* declaration;
        uint16_t strides[kMaxStreams + 1];
        RenderPrimitiveTopology topology;
        RenderFormat colorFormat;
        uint32_t depthControl;  // RB_DEPTHCONTROL: z enable, z write, z func; stencil fields when stencil is on
        uint32_t blendControl;  // RB_BLENDCONTROL0
        uint32_t colorMask;
        uint32_t cull;          // PA_SU_SC_MODE_CNTL bits 0-2
        uint32_t specConstants;
        uint32_t stencilRefMask; // RB_STENCILREFMASK (ref, read mask, write mask) when stencil is on
        uint32_t samples;        // the pass's MSAA samples (SurfaceSamples)
        uint32_t flags;          // kPipelineAlphaToCoverage
    };
    static_assert(sizeof(PipelineKey) == 96);
    constexpr uint32_t kPipelineAlphaToCoverage = 1;
    constexpr uint32_t kPipelineMsaaRestoreBias = 2; // after a depth-export restore in an MSAA pass (see PrepareDraw)

    struct Command
    {
        enum Type : uint8_t { Draw, Clear, Resolve, Upload } type;
        uint32_t colorSurface, depthSurface;
        uint32_t colorKey, depthKey; // EDRAM base + 1 of the colour / depth buffer (0: none)
        uint32_t width, height;
        RenderFormat colorFormat;
        uint32_t samples;            // MSAA samples of the surface (1, 2, 4): host buffers per count

        // Clear
        uint32_t clearFlags;
        RenderColor clearColor;
        float clearZ;
        uint32_t clearStencil;

        // Draw
        PipelineKey key;
        RenderViewport viewport;
        uint32_t vertexConstants, pixelConstants, sharedConstants; // frame data offsets
        uint32_t streamOffset[kMaxStreams], streamSize[kMaxStreams];
        uint32_t streamMask;
        uint32_t residentStreams;   // slots whose streamOffset is in the geometry buffer, not the frame's data
        bool residentIndices;       // indexOffset is in the geometry buffer
        bool indexed;
        uint32_t indexOffset, indexSize;
        uint32_t count;
        int32_t baseVertex;

        // Resolve (EDRAM -> texture): source rectangle, destination texture and position.
        uint32_t resolveFlags;
        int32_t sourceRect[4];
        uint32_t destAddress, destKey, destDescriptor, destWidth, destHeight;
        uint32_t destFetch[6]; // the destination texture's fetch constant (write-back to guest memory)
        bool encode7e3;        // from the fixed-point view of the HDR buffer: the destination gets 7e3 bits
        int32_t destX, destY;
        uint32_t destLevel, destSlice;   // mip level and cube face / array slice resolved into
        uint32_t destLevels;             // mip levels of the destination texture (fetch constant)
        bool destCube;
        uint32_t surfaceWidth, surfaceHeight; // EDRAM allocation of the source surface
        bool resolveDepthFlipped; // the source depth buffer holds 1 - guest depth (reversed Z)

        // Upload (texture data decoded on the guest thread): descriptor it goes to, data in the frame.
        uint32_t uploadDescriptor, uploadOffset, uploadRowWidth, uploadLayerPitch, uploadLayers;
        uint32_t uploadLevels, uploadLevelTable; // mip levels; their TextureDecode::Decoded::Level table in the frame data
        RenderFormat uploadFormat;
        RenderSwizzle uploadSwizzle[4];
        bool uploadCube;
        bool uploadTaken; // uploadOffset is in the frame's taken textures (TextureUploadBase), not its data
    };

    // Textures the game resolves render targets into, by guest address. Their descriptors are
    // allocated when the guest records the resolve, so later draws in the same frame can bind them;
    // the video thread creates the host textures when it replays the resolve.
    std::atomic<uint32_t> g_descriptorCounter{ DESCRIPTOR_FIRST_FREE };
    std::mutex g_resolveMutex;
    ankerl::unordered_dense::map<uint32_t, uint32_t> g_resolveDescriptors; // by ResolveKey + host format (bits 2-11)
    // By ResolveKey: the descriptor last resolved into, the destination's size, and when and over
    // what guest memory. Levels reuse memory, so a texture loaded later where a render target was
    // resolved earlier must not take the resolve (the title's 128x128 captures hid the corridors'
    // reflection cube maps: red reflections). Draws take it only when the texture they bind has the
    // destination's size and is not block-compressed (no resolve writes those), and, once the
    // target has not been resolved for two frames, only while the guest memory is as it was at
    // the last resolve: the CPU writes into live render targets between frames (the bloom target),
    // so memory alone cannot decide for those.
    struct LatestResolve
    {
        uint32_t descriptor = 0, width = 0, height = 0;
        uint32_t address = 0, size = 0, frame = 0;
        uint64_t memory = 0;
        float scale = 1.0f; // resolution scale of the pass it was resolved from (PassScale)
    };

    // ---- Resolution scale (Config::ResolutionScale; docs/renderer.md, "Resolution scale") -----
    // Passes into screen-sized render targets (the scene, post-processing, the shadow map) are drawn
    // at the scale chosen in the options; small ones (luminance reductions down to 1x1, which the
    // CPU reads back, and the reflection probes' levels) stay at the console's size. Commands are
    // recorded in guest pixels and scaled when the video thread replays them, edge by edge
    // (ScaleEdge), so neighbouring rectangles still meet at fractional scales. Shaders get the
    // pass's scale (VPOS in guest pixels) and each render target copy's scale (sampler index bits
    // 24-29: texel offsets in guest texels), see shader_common.h.
    // Whole scales only (1x, 2x, 3x): at 1.5x the guest's pixel grid falls between host pixels and
    // passes that sample on it (point-sampled copies at guest texel positions) skip and repeat host
    // rows and columns, which made edges and text jagged. Xenia limits its scale the same way.
    float FrameScale()
    {
        // DR_RES_SCALE=<scale> (testing) overrides the setting.
        static const float forced = getenv("DR_RES_SCALE") != nullptr ? float(atof(getenv("DR_RES_SCALE"))) : 0.0f;
        const float scale = forced > 0.0f ? forced : Config::ResolutionScale.load();
        return std::clamp(std::round(scale * 2.0f) / 2.0f, 1.0f, 3.0f);
    }

    float PassScale(uint32_t width, uint32_t height, float frameScale)
    {
        const uint32_t size = std::max(width, height);
        if (size <= 256 || frameScale == 1.0f)
            return 1.0f;
        // At most 4096 host pixels a side: the 2048 shadow map stays at 2x when 3x is chosen.
        const float limit = std::floor(4096.0f / float(size));
        return std::max(1.0f, std::min(frameScale, limit));
    }

    int32_t ScaleEdge(int32_t value, float scale)
    {
        return scale == 1.0f ? value : int32_t(std::lround(double(value) * scale));
    }

    uint32_t ScaleCode(float scale)
    {
        return scale == 1.0f ? 0u : (uint32_t(std::lround(scale * 4.0f)) & 0x3Fu) << 24;
    }
    ankerl::unordered_dense::map<uint32_t, LatestResolve> g_latestResolve;
    std::vector<uint32_t> g_newResolveDescriptors; // not initialised on the video thread yet

    // Samplers by Xenos sampler state (clamp modes, filters); index 0 is the default sampler.
    std::mutex g_samplerMutex;
    ankerl::unordered_dense::map<uint64_t, uint32_t> g_samplerIndices;
    std::vector<std::pair<uint64_t, uint32_t>> g_newSamplers; // (state, index) the video thread has not created yet

    uint32_t SamplerIndex(const TextureDecode::FetchConstant& fetch)
    {
        // Bits 32-39: the mip range the draw allows (D3D's MAXMIPLEVEL and the texture's levels, merged
        // into the device's fetch constant); the game samples nothing outside it.
        const uint64_t state = fetch.ClampX() | (fetch.ClampY() << 3) | (fetch.ClampZ() << 6) | (fetch.MagFilter() << 9) |
            (fetch.MinFilter() << 11) | (fetch.MipFilter() << 13) | (fetch.AnisoFilter() << 15) |
            (((fetch.dword[4] >> 12) & 0x3FF) << 18) | // LOD bias (signed, 1/32 steps)
            (uint64_t(fetch.MipMinLevel()) << 32) | (uint64_t(fetch.MipMaxLevel()) << 36);
        std::lock_guard lock(g_samplerMutex);
        auto [it, inserted] = g_samplerIndices.try_emplace(state, 0);
        if (inserted)
        {
            it->second = uint32_t(g_samplerIndices.size()); // 1, 2, ...
            if (it->second >= kBlitSampler)
                it->second = 0; // heap full: default sampler
            else
                g_newSamplers.emplace_back(state, it->second);
        }
        return it->second;
    }

    // Resolve textures are keyed by address and format class: the game resolves into the same
    // address in different formats within a frame (HDR scene, then 8-bit post-processing), and a
    // descriptor must not change while the frame that samples it is in flight.
    uint32_t ResolveKey(uint32_t address, uint32_t guestFormat)
    {
        const uint32_t formatClass = guestFormat == 6 ? 1 : (guestFormat == 22 || guestFormat == 23) ? 2 : 0;
        return (address & ~0xFFFu) | formatClass;
    }

    uint32_t ResolveDescriptor(uint32_t key)
    {
        std::lock_guard lock(g_resolveMutex);
        auto [it, inserted] = g_resolveDescriptors.try_emplace(key, 0);
        if (inserted)
        {
            it->second = g_descriptorCounter++;
            g_newResolveDescriptors.push_back(it->second);
        }
        return it->second;
    }

    // A frame's data (vertices, indices, constants, texture uploads). Grows without clearing:
    // std::vector<uint8_t>::resize zero-filled every byte it added, tens of MB a frame in the
    // mall, on the game thread. Every allocation is written in full before it is read.
    class FrameBuffer
    {
    public:
        uint8_t* data() { return m_data.get(); }
        const uint8_t* data() const { return m_data.get(); }
        size_t size() const { return m_size; }
        void clear() { m_size = 0; }
        void resize(size_t size)
        {
            if (size > m_capacity)
            {
                const auto start = std::chrono::steady_clock::now();
                const size_t capacity = std::max(size, m_capacity + m_capacity / 2 + (size_t(1) << 20));
                std::unique_ptr<uint8_t[]> grown(new uint8_t[capacity]); // not value-initialised
                if (m_size != 0)
                    memcpy(grown.get(), m_data.get(), m_size);
                m_data = std::move(grown);
                m_capacity = capacity;
                s_grownUs += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
                s_grownTo = capacity;
            }
            m_size = size;
        }

        // Growth since the last call (the "[hitch]" line): time copying, and the largest capacity.
        static std::pair<uint64_t, size_t> TakeGrowth()
        {
            const auto taken = std::make_pair(s_grownUs, s_grownTo);
            s_grownUs = 0;
            s_grownTo = 0;
            return taken;
        }

    private:
        std::unique_ptr<uint8_t[]> m_data;
        size_t m_size = 0, m_capacity = 0;
        static inline uint64_t s_grownUs = 0; // recording thread only
        static inline size_t s_grownTo = 0;
    };

    // Data the video thread copies from the frame's data into the geometry buffer before the draws.
    struct GeometryUpload
    {
        uint32_t source, destination, size;
    };

    // Large buffers are freed here, off the game's and the video thread's way (freeing a load's
    // decoded textures took several ms of NtFreeVirtualMemory at present).
    class BufferReaper
    {
    public:
        void Free(std::vector<std::vector<uint8_t>>&& buffers)
        {
            if (buffers.empty())
                return;
            std::call_once(m_started, [this] { std::thread([this] { Run(); }).detach(); });
            {
                std::lock_guard lock(m_mutex);
                for (auto& buffer : buffers)
                    m_queue.push_back(std::move(buffer));
            }
            buffers.clear();
            m_work.notify_one();
        }

    private:
        void Run()
        {
            std::vector<std::vector<uint8_t>> freeing;
            while (true)
            {
                {
                    std::unique_lock lock(m_mutex);
                    m_work.wait(lock, [this] { return !m_queue.empty(); });
                    freeing.swap(m_queue);
                }
                freeing.clear();
            }
        }

        std::once_flag m_started;
        std::mutex m_mutex;
        std::condition_variable m_work;
        std::vector<std::vector<uint8_t>> m_queue;
    } g_bufferReaper;

    struct Frame
    {
        FrameBuffer data;
        std::vector<Command> commands;
        std::vector<GeometryUpload> geometryUploads;
        uint32_t leadingUploads = 0; // vertex texture uploads put ahead of the recorded commands
        bool capture = false;        // F12 capture: the video thread dumps this frame's targets and resolves
        float scale = 1.0f;          // resolution scale the frame was recorded with (FrameScale)
        // Textures the decode workers made, taken over at present instead of being copied into
        // 'data' (that copy was 40 ms of a load's frame, 2026-11-25): offset in the texture region
        // of the upload buffer (TextureUploadBase), and the bytes. The video thread copies them in
        // at replay and hands them to g_bufferReaper.
        std::vector<std::vector<uint8_t>> textures;
        std::vector<size_t> textureOffsets;
        size_t texturesSize = 0;

        uint32_t Allocate(size_t size, size_t alignment)
        {
            size_t offset = (data.size() + alignment - 1) & ~(alignment - 1);
            data.resize(offset + size);
            return uint32_t(offset);
        }

        uint32_t TakeTexture(std::vector<uint8_t>&& bytes)
        {
            const size_t offset = (texturesSize + 511) & ~size_t(511);
            texturesSize = offset + bytes.size();
            textureOffsets.push_back(offset);
            textures.push_back(std::move(bytes));
            return uint32_t(offset);
        }

        void ReleaseTextures()
        {
            g_bufferReaper.Free(std::move(textures));
            textures.clear();
            textureOffsets.clear();
            texturesSize = 0;
        }

        void Clear()
        {
            capture = false;
            data.clear();
            commands.clear();
            geometryUploads.clear();
            leadingUploads = 0;
            ReleaseTextures();
        }
    };

    // The upload buffer of a frame: its data, the fixed regions after it (dummy vertex slot, blit
    // constants, 64 constant slots for depth resolves, touch overlay, options menu: Replay, Render),
    // then its taken textures.
    size_t FixedUploadEnd(const Frame& frame)
    {
        return ((frame.data.size() + 255) & ~size_t(255)) + 512 + 256 * 64 + 1280 + 256;
    }
    size_t TextureUploadBase(const Frame& frame)
    {
        return (FixedUploadEnd(frame) + 511) & ~size_t(511);
    }

    // Textures from guest memory, by fetch constant (texture fields only). Decoded on the guest
    // thread when first bound (and again when a sample of their data changes), uploaded by the
    // video thread before the frame's draws.
    struct TextureEntry
    {
        uint32_t descriptor = 0;
        uint64_t sample = 0;      // hash of a sample of the guest data
        uint32_t checkedFrame = UINT32_MAX;
        uint32_t lastUsed = 0;    // guest frame
        bool cube = false;
        // Hash of every 4 KB page decoded (base level, then mip levels), checked a few pages a
        // frame in turn: a texture streamed in part by part, or mip levels written after the base,
        // changes pages the sample above does not cover.
        std::vector<uint64_t> pages;
        uint32_t mipAddress = 0, mipsSize = 0, nextPage = 0;
        uint64_t decodedHash = 0;   // of the decoded data (captures report textures that were stale)
        uint32_t decodedFrame = 0;
    };

    // Texture descriptors are recycled: entries unused for a while are evicted on the guest thread,
    // the video thread frees their host textures and returns the descriptors (g_freeDescriptors).
    std::vector<uint32_t> g_releasedDescriptors; // guest -> video thread (g_resolveMutex)
    std::vector<uint32_t> g_freeDescriptors;     // video -> guest thread (g_resolveMutex)
    std::atomic<uint32_t> g_textureCacheSize{ 0 };

    constexpr uint32_t kNotResident = UINT32_MAX; // no region in the geometry buffer (GeometryHeap)

    // Converted vertex streams by source range, declaration and stream (RecordStreams).
    struct StreamKey
    {
        uint32_t source, size, declaration, stream;
        bool operator==(const StreamKey&) const = default;
    };
    struct StreamKeyHash
    {
        using is_avalanching = void;
        uint64_t operator()(const StreamKey& k) const { return XXH3_64bits(&k, sizeof(k)); }
    };
    struct StreamCacheEntry
    {
        uint64_t hash = 0;
        bool valid = false;
        uint32_t misses = 0;
        uint32_t recordedFrame = UINT32_MAX, lastUsed = 0;
        uint32_t offset = 0, auxOffset = 0; // in the frame recorded as recordedFrame
        uint32_t stableFrames = 0;          // frames in a row its data was unchanged at the first draw
        std::vector<uint8_t> converted, aux; // host-order vertices and decoded auxiliary stream
        uint32_t resident = kNotResident, residentAux = kNotResident; // regions in the geometry buffer
        uint32_t residentSize = 0, residentAuxSize = 0;
        bool offsetResident = false;        // offset / auxOffset are in the geometry buffer
    };
    ankerl::unordered_dense::map<StreamKey, StreamCacheEntry, StreamKeyHash> g_streamCache;

    // Processed index lists (OnDrawIndexed): rebased to the lowest index, strips as lists.
    struct IndexKey
    {
        uint32_t address, count, primitive;
        bool operator==(const IndexKey&) const = default;
    };
    struct IndexKeyHash
    {
        using is_avalanching = void;
        uint64_t operator()(const IndexKey& k) const { return XXH3_64bits(&k, sizeof(k)); }
    };
    struct IndexCacheEntry
    {
        uint64_t hash = 0;
        bool valid = false, skip = false;
        uint32_t minIndex = 0, maxIndex = 0;
        uint32_t recordedFrame = UINT32_MAX, lastUsed = 0, offset = 0;
        uint32_t stableFrames = 0;
        std::vector<uint32_t> rebased;
        uint32_t resident = kNotResident, residentSize = 0;
        bool offsetResident = false;
    };
    ankerl::unordered_dense::map<IndexKey, IndexCacheEntry, IndexKeyHash> g_indexCache;
    uint32_t AllocateDescriptor();
    ankerl::unordered_dense::map<uint64_t, TextureEntry> g_textureCache;
    uint32_t g_guestFrame = 0;

    // ---- Resident geometry. Vertex streams and index lists unchanged for kResidentAfter frames
    // move into one device-local buffer (g_geometryBuffer, video thread), uploaded once, instead of
    // being copied into every frame's data: 108 MB a frame in the mall at 4K, copied twice on the
    // CPU and read by the GPU from upload memory. A region is freed when its data changes or its
    // entry is evicted, and reused only after the frames that drew from it have finished.
#if defined(__APPLE__) && TARGET_OS_IPHONE
    // iPhone / iPad: GPU memory is the app's own memory there, and the system ends an app that goes over its
    // limit (2026-11-25: the iPhone build ended at frame 8, when the first 768 MB buffer was made). What does
    // not fit stays in each frame's data, as before.
    constexpr uint64_t kGeometryHeapSize = 192ull << 20;
#else
    constexpr uint64_t kGeometryHeapSize = 768ull << 20;
#endif
    constexpr uint32_t kResidentAfter = 8;

    struct GeometryHeap
    {
        std::map<uint32_t, uint32_t> free{ { 0u, uint32_t(kGeometryHeapSize) } }; // offset -> size, coalesced
        std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> pending;           // frame freed, offset, size
        uint64_t used = 0;

        static uint32_t Aligned(uint32_t size) { return (size + 255) & ~255u; }

        uint32_t Allocate(uint32_t size)
        {
            size = Aligned(size);
            for (auto it = free.begin(); it != free.end(); ++it)
                if (it->second >= size)
                {
                    const uint32_t offset = it->first, rest = it->second - size;
                    free.erase(it);
                    if (rest != 0)
                        free.emplace(offset + size, rest);
                    used += size;
                    return offset;
                }
            return kNotResident;
        }

        void Free(uint32_t offset, uint32_t size)
        {
            if (offset != kNotResident)
                pending.emplace_back(g_guestFrame, offset, Aligned(size));
        }

        // A frame recorded now is replayed and finished on the GPU within a few frames (the guest
        // waits for the video thread to take each frame, the video thread for the GPU).
        void Reclaim()
        {
            for (size_t i = 0; i < pending.size();)
            {
                const auto [frame, offset, size] = pending[i];
                if (g_guestFrame - frame < 4)
                {
                    i++;
                    continue;
                }
                Insert(offset, size);
                used -= size;
                pending[i] = pending.back();
                pending.pop_back();
            }
        }

        void Insert(uint32_t offset, uint32_t size)
        {
            auto next = free.lower_bound(offset);
            if (next != free.begin())
            {
                auto previous = std::prev(next);
                if (previous->first + previous->second == offset)
                {
                    offset = previous->first;
                    size += previous->second;
                    free.erase(previous);
                }
            }
            if (next != free.end() && offset + size == next->first)
            {
                size += next->second;
                free.erase(next);
            }
            free.emplace(offset, size);
        }
    } g_geometryHeap;

    // Copies data into the frame once; the video thread puts it into the geometry buffer.
    uint32_t UploadResident(Frame& frame, const void* data, uint32_t size)
    {
        const uint32_t destination = g_geometryHeap.Allocate(size);
        if (destination == kNotResident)
            return kNotResident;
        const uint32_t source = frame.Allocate(size, 16);
        memcpy(frame.data.data() + source, data, size);
        frame.geometryUploads.push_back({ source, destination, size });
        return destination;
    }

    struct TextureStats
    {
        std::atomic<uint32_t> decoded{ 0 }, failed{ 0 }, reuploaded{ 0 };
        std::atomic<uint64_t> bytes{ 0 };
    } g_textureStats;

    uint64_t SampleHash(const uint8_t* data, uint32_t size)
    {
        if (size <= 4096)
            return XXH3_64bits(data, size);
        uint64_t hash = 0;
        for (uint32_t i = 0; i < 16; i++)
            hash = XXH3_64bits_withSeed(data + uint64_t(size - 256) * i / 15, 256, hash);
        return hash;
    }

    uint32_t ResolveDescriptor(uint32_t address);

    // ---- Debugging: DR_TEX_WATCH=1 watches the guest memory of cached textures -----------------
    // The cache only re-decodes a texture when a sample of its data changes. This keeps a hash of
    // every 4 KB page of each texture (base and mip levels) and, every 2 s, re-hashes the textures
    // in use: pages that changed without a re-decode are logged with their new first bytes. Writes
    // the runtime itself makes into guest memory (CheckGuestWrite) are checked against the
    // textures' ranges. Addresses are compared as physical (low 29 bits).
    struct WatchedTexture
    {
        uint32_t base = 0, baseSize = 0, mips = 0, mipsSize = 0, format = 0, width = 0, height = 0;
        uint32_t baseGuest = 0, mipsGuest = 0; // the addresses the decoder read
        std::vector<uint64_t> pages; // base pages, then mip pages
        uint32_t lastUsed = 0;
        bool reported = false;
    };
    const bool g_texWatch = getenv("DR_TEX_WATCH") != nullptr;
    std::mutex g_texWatchMutex;
    ankerl::unordered_dense::map<uint64_t, WatchedTexture> g_texWatched;
    ankerl::unordered_dense::map<uint32_t, uint64_t> g_texWatchPages; // physical page -> texture key

    void WatchPages(uint64_t key, uint32_t start, uint32_t size, bool add)
    {
        for (uint32_t page = start >> 12; size != 0 && page <= (start + size - 1) >> 12; page++)
        {
            if (add)
                g_texWatchPages[page] = key;
            else if (auto it = g_texWatchPages.find(page); it != g_texWatchPages.end() && it->second == key)
                g_texWatchPages.erase(it);
        }
    }

    void UnwatchTexture(uint64_t key)
    {
        std::lock_guard lock(g_texWatchMutex);
        auto it = g_texWatched.find(key);
        if (it == g_texWatched.end())
            return;
        WatchPages(key, it->second.base, it->second.baseSize, false);
        WatchPages(key, it->second.mips, it->second.mipsSize, false);
        g_texWatched.erase(it);
    }

    void HashPages(const uint8_t* data, uint32_t size, std::vector<uint64_t>& out)
    {
        for (uint32_t offset = 0; offset < size; offset += 4096)
            out.push_back(XXH3_64bits(data + offset, std::min(4096u, size - offset)));
    }

    void WatchTexture(uint64_t key, const TextureDecode::FetchConstant& fetch, uint32_t mipAddress, uint32_t mipsSize)
    {
        WatchedTexture w;
        w.base = fetch.BaseAddress() & 0x1FFFFFFF;
        w.baseGuest = fetch.BaseAddress();
        w.baseSize = TextureDecode::GuestSize(fetch);
        w.mips = mipAddress & 0x1FFFFFFF;
        w.mipsGuest = mipAddress;
        w.mipsSize = mipsSize;
        w.format = fetch.Format();
        w.width = fetch.Width();
        w.height = fetch.Height();
        w.lastUsed = g_guestFrame;
        HashPages(static_cast<const uint8_t*>(g_memory.Translate(fetch.BaseAddress())), w.baseSize, w.pages);
        if (mipsSize != 0)
            HashPages(static_cast<const uint8_t*>(g_memory.Translate(mipAddress)), mipsSize, w.pages);

        // A re-decode of the same texture after only some of its pages changed: a texture loaded
        // anew rewrites all of them, a stray write only a few.
        {
            std::lock_guard lock(g_texWatchMutex);
            if (auto it = g_texWatched.find(key); it != g_texWatched.end() && it->second.pages.size() == w.pages.size() &&
                it->second.baseGuest == w.baseGuest && it->second.mipsGuest == w.mipsGuest)
            {
                uint32_t changed = 0, first = UINT32_MAX;
                for (size_t page = 0; page < w.pages.size(); page++)
                    if (w.pages[page] != it->second.pages[page])
                    {
                        changed++;
                        first = std::min(first, uint32_t(page));
                    }
                if (changed != 0 && changed < w.pages.size())
                    LOGFN_WARNING("[texwatch] frame {}: texture {:08X} fmt {} {}x{} re-decoded after {} of its {} pages changed (first: page {})",
                        g_guestFrame, w.base, w.format, w.width, w.height, changed, w.pages.size(), first);
            }
        }
        UnwatchTexture(key);
        std::lock_guard lock(g_texWatchMutex);
        WatchPages(key, w.base, w.baseSize, true);
        WatchPages(key, w.mips, w.mipsSize, true);
        g_texWatched[key] = std::move(w);
    }

    void VerifyWatchedTextures()
    {
        if (!g_texWatch || g_guestFrame % 60 != 30)
            return;
        // Every watched texture, in use or not: one corrupted while out of view would otherwise only
        // show up re-decoded. Whole textures replaced (level loads) are counted; partial changes are
        // logged with the first changed page's new bytes.
        std::lock_guard lock(g_texWatchMutex);
        uint32_t checked = 0, replaced = 0, partial = 0;
        for (auto& [key, w] : g_texWatched)
        {
            if (w.reported)
                continue;
            checked++;
            const uint32_t basePages = (w.baseSize + 4095) / 4096;
            uint32_t changed = 0, first = UINT32_MAX;
            for (uint32_t page = 0; page < w.pages.size(); page++)
            {
                const bool inBase = page < basePages;
                const uint32_t start = inBase ? w.baseGuest + page * 4096 : w.mipsGuest + (page - basePages) * 4096;
                const uint32_t end = inBase ? w.baseGuest + w.baseSize : w.mipsGuest + w.mipsSize;
                const uint32_t size = std::min(4096u, end - start);
                if (XXH3_64bits(g_memory.Translate(start), size) != w.pages[page])
                {
                    changed++;
                    first = std::min(first, page);
                }
            }
            if (changed == 0)
                continue;
            w.reported = true;
            if (changed == w.pages.size() && w.pages.size() > 1)
            {
                replaced++;
                continue;
            }
            partial++;
            const bool inBase = first < basePages;
            const uint32_t start = inBase ? w.baseGuest + first * 4096 : w.mipsGuest + (first - basePages) * 4096;
            const uint8_t* data = static_cast<const uint8_t*>(g_memory.Translate(start));
            std::string bytes;
            for (uint32_t i = 0; i < 48; i++)
                bytes += fmt::format("{:02X}", data[i]);
            if (auto it = g_textureCache.find(key); it != g_textureCache.end())
                w.lastUsed = it->second.lastUsed;
            LOGFN_WARNING("[texwatch] frame {}: texture {:08X} fmt {} {}x{} (last used frame {}): {} of {} pages changed without a re-decode, first {} page {} at {:08X}: {}",
                g_guestFrame, w.base, w.format, w.width, w.height, w.lastUsed, changed, w.pages.size(), inBase ? "base" : "mips",
                inBase ? first : first - basePages, start, bytes);
        }
        static uint32_t summaries;
        if (summaries++ % 30 == 0 || partial != 0)
            LOGFN("[texwatch] frame {}: {} textures watched, {} checked, {} replaced, {} partly changed", g_guestFrame, g_texWatched.size(), checked, replaced, partial);
    }

    uint32_t AllocateDescriptor()
    {
        {
            std::lock_guard lock(g_resolveMutex);
            if (!g_freeDescriptors.empty())
            {
                const uint32_t descriptor = g_freeDescriptors.back();
                g_freeDescriptors.pop_back();
                return descriptor;
            }
        }
        const uint32_t descriptor = g_descriptorCounter++;
        if (descriptor >= kTextureDescriptorCount)
        {
            static bool logged;
            if (!logged)
            {
                logged = true;
                LOGFN_WARNING("[renderer] texture descriptor heap full ({}): new textures show white", kTextureDescriptorCount);
            }
            return DESCRIPTOR_WHITE_2D;
        }
        return descriptor;
    }

    // 'take': the decoded data moves into the frame's texture list instead of being copied into its
    // data (the decode workers' results at present; the video thread copies it into the upload buffer).
    Command MakeUpload(Frame& frame, uint32_t descriptor, TextureDecode::Decoded& decoded, bool take = false)
    {
        Command cmd;
        memset(static_cast<void*>(&cmd), 0, sizeof(cmd));
        cmd.type = Command::Upload;
        cmd.uploadDescriptor = descriptor;
        if (take)
        {
            cmd.uploadOffset = frame.TakeTexture(std::move(decoded.data));
            cmd.uploadTaken = true;
        }
        else
        {
            cmd.uploadOffset = frame.Allocate(decoded.data.size(), 512);
            memcpy(frame.data.data() + cmd.uploadOffset, decoded.data.data(), decoded.data.size());
        }
        cmd.uploadLevels = decoded.levels;
        cmd.uploadLevelTable = frame.Allocate(sizeof(decoded.level[0]) * decoded.levels, 16);
        memcpy(frame.data.data() + cmd.uploadLevelTable, decoded.level, sizeof(decoded.level[0]) * decoded.levels);
        cmd.uploadRowWidth = decoded.rowWidth;
        cmd.uploadLayerPitch = decoded.layerPitch;
        cmd.uploadLayers = decoded.layers;
        cmd.uploadFormat = decoded.format;
        cmd.uploadCube = decoded.cube;
        memcpy(cmd.uploadSwizzle, decoded.swizzle, sizeof(cmd.uploadSwizzle));
        cmd.width = decoded.width;
        cmd.height = decoded.height;
        return cmd;
    }

    // Texture decodes off the game thread (2026-11-25): the first draw of a texture (or of one whose
    // memory changed) decoded it there, synchronously - 80-100 ms when an area or a mall section
    // streamed in, the game's "[hitch]" frames. Now the draw only reserves the descriptor and an
    // empty command in the frame; worker threads decode while the game goes on, and at present
    // each finished decode becomes that command's upload (FinishDecodes). A decode that fails
    // leaves the command empty (replay skips it) and the texture white, as before. Cube maps and
    // debugging dumps still decode in place.
    struct DecodeJob
    {
        TextureDecode::FetchConstant fetch;
        const uint8_t* source = nullptr;
        const uint8_t* mips = nullptr;
        size_t command = 0;      // index of the reserved command in the frame
        uint32_t descriptor = 0;
        TextureDecode::Decoded decoded;
        bool ok = false;
        bool done = false;       // under DecodePool::mutex
    };

    class DecodePool
    {
    public:
        void Submit(DecodeJob* job)
        {
            Start();
            {
                std::lock_guard lock(mutex);
                queue.push_back(job);
            }
            work.notify_one();
        }

        void Wait(DecodeJob* job)
        {
            std::unique_lock lock(mutex);
            finished.wait(lock, [job] { return job->done; });
        }

    private:
        void Start()
        {
            std::call_once(started, [this] {
                const uint32_t threads = std::clamp(std::thread::hardware_concurrency() / 2, 2u, 8u);
                for (uint32_t i = 0; i < threads; i++)
                    std::thread([this] { Run(); }).detach();
                LOGFN("[renderer] {} texture decode threads", threads);
            });
        }

        void Run()
        {
            HangWatchdog::PrepareThread(); // crash reports, also of a stack overflow
            while (true)
            {
                DecodeJob* job;
                {
                    std::unique_lock lock(mutex);
                    work.wait(lock, [this] { return !queue.empty(); });
                    job = queue.front();
                    queue.pop_front();
                }
                const char* reason = "";
                {
                    HitchStats::Timer timer(HitchStats::TextureDecode);
                    job->ok = TextureDecode::Decode(job->fetch, job->source, job->mips, job->decoded, reason);
                }
                if (!job->ok)
                {
                    static std::mutex logMutex;
                    static std::set<uint32_t> loggedFormats;
                    std::lock_guard logLock(logMutex);
                    if (loggedFormats.insert(job->fetch.Format() | (job->fetch.Dimension() << 8)).second)
                        LOGFN_WARNING("[renderer] texture format {} (dimension {}) not supported ({}), shown white", job->fetch.Format(), job->fetch.Dimension(), reason);
                }
                {
                    std::lock_guard lock(mutex);
                    job->done = true;
                }
                finished.notify_all();
            }
        }

        std::once_flag started;
        std::mutex mutex;
        std::condition_variable work, finished;
        std::deque<DecodeJob*> queue;
    } g_decodePool;

    std::vector<std::unique_ptr<DecodeJob>> g_decodeJobs; // this frame's (guest thread)

    // At present: every decode of the frame done, its upload in the command it reserved.
    void FinishDecodes(Frame& frame)
    {
        for (auto& job : g_decodeJobs)
        {
            g_decodePool.Wait(job.get());
            if (!job->ok)
            {
                g_textureStats.failed++;
                continue;
            }
            g_textureStats.bytes += job->decoded.data.size();
            static const bool copy = getenv("DR_TEXTURE_COPY") != nullptr; // the old path (copied at present), for comparison
            frame.commands[job->command] = MakeUpload(frame, job->descriptor, job->decoded, !copy);
        }
        g_decodeJobs.clear();
    }

    void RequestPipelines(const Frame& frame); // with the pipelines, below

    // Vertex textures (fetch constants 16+) hold data the CPU writes while it builds the frame:
    // the bone matrices of skinned meshes (faces, hands), in a double-buffered float texture filled
    // model by model. A copy taken when the first draw binds it misses every model after that one,
    // so these are copied when the frame is presented and uploaded ahead of the frame's draws.
    ankerl::unordered_dense::map<uint64_t, TextureDecode::FetchConstant> g_dynamicTextures;

    // Descriptor for a texture's fetch constant (white placeholder if unsupported).
    uint32_t TextureDescriptor(Frame& frame, const TextureDecode::FetchConstant& fetch, bool& cube, bool dynamic)
    {
        cube = fetch.Dimension() == 3;

        // Key on the texture fields only (not the sampler state D3D merges into the device's copy).
        uint32_t key[6] = { fetch.dword[0] & ~(0x1FFu << 10), fetch.dword[1], fetch.dword[2], fetch.dword[3] & 0x7FFFF, (fetch.dword[4] >> 2) & 0xFF, fetch.dword[5] & 0xFFFFFE00 };
        const uint64_t keyHash = XXH3_64bits(key, sizeof(key));
        auto& entry = g_textureCache[keyHash];
        entry.lastUsed = g_guestFrame;
        if (dynamic && !cube)
        {
            const uint32_t size = TextureDecode::GuestSize(fetch);
            if (fetch.BaseAddress() < 0x10000 || size == 0 || uint64_t(fetch.BaseAddress()) + size > 0xFFFF0000ull)
                return DESCRIPTOR_WHITE_2D;
            if (entry.descriptor == 0 || entry.descriptor == DESCRIPTOR_WHITE_2D)
            {
                entry.descriptor = AllocateDescriptor();
                entry.sample = 0;
                if (entry.descriptor == DESCRIPTOR_WHITE_2D)
                    return entry.descriptor;
                std::lock_guard lock(g_resolveMutex);
                g_newResolveDescriptors.push_back(entry.descriptor);
            }
            g_dynamicTextures.try_emplace(keyHash, fetch);
            return entry.descriptor;
        }
        if (entry.checkedFrame == g_guestFrame)
            return entry.descriptor;
        entry.checkedFrame = g_guestFrame;

        // Debugging: DR_TEX_WHITE=<format,format,...> shows those formats as white.
        static const std::set<uint32_t> whiteFormats = []
        {
            std::set<uint32_t> formats;
            if (const char* list = getenv("DR_TEX_WHITE"))
                for (const char* p = list; *p; )
                {
                    formats.insert(uint32_t(strtoul(p, const_cast<char**>(&p), 10)));
                    while (*p == ',' || *p == ' ')
                        p++;
                }
            return formats;
        }();

        // Debugging: DR_TEX_WHITE_ADDR=<lo>-<hi> (hex) shows textures based in [lo, hi) as white.
        static const std::pair<uint32_t, uint32_t> whiteRange = []
        {
            std::pair<uint32_t, uint32_t> range{ 0, 0 };
            if (const char* text = getenv("DR_TEX_WHITE_ADDR"))
            {
                char* end;
                range.first = uint32_t(strtoul(text, &end, 16));
                range.second = *end == '-' ? uint32_t(strtoul(end + 1, nullptr, 16)) : range.first + 1;
            }
            return range;
        }();

        const uint32_t address = fetch.BaseAddress();
        const uint32_t size = TextureDecode::GuestSize(fetch);
        const bool whiteAddress = address >= whiteRange.first && address < whiteRange.second;
        if (address < 0x10000 || size == 0 || uint64_t(address) + size > 0xFFFF0000ull || whiteFormats.count(fetch.Format()) || whiteAddress)
        {
            entry.descriptor = DESCRIPTOR_WHITE_2D;
            return entry.descriptor;
        }

        const uint8_t* source = static_cast<const uint8_t*>(g_memory.Translate(address));
        const uint64_t sample = SampleHash(source, size);
        // Debugging: DR_TEX_REUPLOAD=<n> re-uploads every texture every n guest frames.
        static const uint32_t reuploadEvery = getenv("DR_TEX_REUPLOAD") ? uint32_t(atoi(getenv("DR_TEX_REUPLOAD"))) : 0;
        // Debugging: DR_TEX_DUMP=frame dumps every texture bound in the DR_DRAW_LOG frame (decoded again there).
        const bool dumpFrame = g_captureTextures.load(std::memory_order_relaxed);
        const uint32_t dumpFrameNumber = g_captureFrame.load(std::memory_order_relaxed);
        const bool forced = (reuploadEvery != 0 && g_guestFrame % reuploadEvery == 0) || (dumpFrame && g_guestFrame == dumpFrameNumber);
        bool pageChanged = false;
        static const bool pageCheck = !(getenv("DR_TEX_PAGECHECK") && atoi(getenv("DR_TEX_PAGECHECK")) == 0); // debugging: 0 = sample only
        if (pageCheck && entry.descriptor != 0 && entry.sample == sample && !entry.pages.empty())
        {
            // Two pages a frame, in turn (all of a 2 MB texture within ~8 s at 30 fps).
            const uint32_t basePages = (size + 4095) / 4096;
            for (uint32_t n = 0; n < 2 && !pageChanged; n++)
            {
                const uint32_t page = entry.nextPage++ % uint32_t(entry.pages.size());
                const bool inBase = page < basePages;
                const uint32_t start = inBase ? address + page * 4096 : entry.mipAddress + (page - basePages) * 4096;
                const uint32_t end = inBase ? address + size : entry.mipAddress + entry.mipsSize;
                pageChanged = XXH3_64bits(g_memory.Translate(start), std::min(4096u, end - start)) != entry.pages[page];
            }
        }
        if (entry.descriptor != 0 && entry.sample == sample && !pageChanged && !forced)
            return entry.descriptor;
        const bool reupload = entry.descriptor != 0 && entry.descriptor != DESCRIPTOR_WHITE_2D;
        const uint64_t previousSample = entry.sample;
        entry.sample = sample;

        // Mip levels (from the fetch constant's mip address; physical addresses map at 0xA0000000).
        // DR_NO_MIPS=1 (debugging) decodes base levels only.
        static const bool noMips = getenv("DR_NO_MIPS") != nullptr;
        const uint8_t* mips = nullptr;
        uint32_t mipAddress = 0, mipsSize = 0;
        if (!noMips && TextureDecode::LevelCount(fetch) > 1)
        {
            mipAddress = fetch.MipAddress();
            if ((mipAddress & 0xE0000000) == 0)
                mipAddress |= 0xA0000000;
            mipsSize = TextureDecode::MipsSize(fetch);
            if (mipsSize != 0 && uint64_t(mipAddress) + mipsSize <= 0xFFFF0000ull)
                mips = static_cast<const uint8_t*>(g_memory.Translate(mipAddress));
            else
                mipsSize = 0;
        }

        // Decoded by the worker threads (DecodePool), unless something needs the result here.
        static const bool decodeInPlace = getenv("DR_TEX_DUMP") != nullptr || getenv("DR_TEX_LOG") != nullptr ||
            (getenv("DR_TEX_ASYNC") && atoi(getenv("DR_TEX_ASYNC")) == 0) || g_texWatch;
        if (!cube && !decodeInPlace && !(dumpFrame && g_guestFrame == dumpFrameNumber))
        {
            if (!reupload)
            {
                entry.descriptor = AllocateDescriptor();
                if (entry.descriptor == DESCRIPTOR_WHITE_2D)
                    return entry.descriptor;
                // White until the video thread has replayed the upload.
                std::lock_guard lock(g_resolveMutex);
                g_newResolveDescriptors.push_back(entry.descriptor);
            }
            entry.decodedHash = 0;
            entry.decodedFrame = g_guestFrame;
            entry.cube = false;
            entry.pages.clear();
            entry.nextPage = 0;
            entry.mipAddress = mips != nullptr ? mipAddress : 0;
            entry.mipsSize = mips != nullptr ? mipsSize : 0;
            HashPages(source, size, entry.pages);
            if (mips != nullptr)
                HashPages(mips, mipsSize, entry.pages);
            (reupload ? g_textureStats.reuploaded : g_textureStats.decoded)++;

            auto job = std::make_unique<DecodeJob>();
            job->fetch = fetch;
            job->source = source;
            job->mips = mips;
            job->descriptor = entry.descriptor;
            job->command = frame.commands.size();
            Command reserved;
            memset(static_cast<void*>(&reserved), 0, sizeof(reserved)); // an empty draw: replay skips it
            frame.commands.push_back(reserved);
            g_decodePool.Submit(job.get());
            g_decodeJobs.push_back(std::move(job));
            return entry.descriptor;
        }

        static thread_local TextureDecode::Decoded decoded;
        const char* reason = "";
        bool decodedOk;
        {
            HitchStats::Timer timer(HitchStats::TextureDecode);
            decodedOk = TextureDecode::Decode(fetch, source, mips, decoded, reason);
        }
        if (!decodedOk)
        {
            static std::set<uint32_t> loggedFormats;
            if (loggedFormats.insert(fetch.Format() | (fetch.Dimension() << 8)).second)
                LOGFN_WARNING("[renderer] texture format {} (dimension {}) not supported ({}), shown white", fetch.Format(), fetch.Dimension(), reason);
            g_textureStats.failed++;
            entry.descriptor = cube ? DESCRIPTOR_NULL_CUBE : DESCRIPTOR_WHITE_2D;
            return entry.descriptor;
        }

        {
            static uint32_t logged;
            static const bool logAll = getenv("DR_TEX_LOG") != nullptr; // debugging: every decode
            if (logged++ < 24 || logAll)
            {
                uint64_t sum = 0;
                for (size_t i = 0; i < decoded.data.size(); i++)
                    sum += decoded.data[i];
                LOGFN("[renderer] texture frame {} {:08X}: fetch {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} -> {}x{} fmt {} tiled {} endian {} swizzle {:03X}, "
                      "guest first bytes {:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}, decoded average {:.1f}",
                    g_guestFrame, address, fetch.dword[0], fetch.dword[1], fetch.dword[2], fetch.dword[3], fetch.dword[4], fetch.dword[5],
                    decoded.width, decoded.height, fetch.Format(), fetch.Tiled(), fetch.Endian(), fetch.Swizzle(),
                    source[0], source[1], source[2], source[3], source[4], source[5], source[6], source[7],
                    decoded.data.empty() ? 0.0 : double(sum) / decoded.data.size());
            }
        }

        // Debugging: DR_TEX_DUMP=1 writes each decoded texture (colour | alpha, at most 512 wide)
        // to dump/tex_<address>_<format>_<w>x<h>.bmp.
        // DR_TEX_DUMP=<hex>,<hex>,... only textures based at those addresses, full size.
        static const std::set<uint32_t> dumpAddresses = []
        {
            std::set<uint32_t> addresses;
            if (const char* list = getenv("DR_TEX_DUMP"))
                for (const char* p = list; *p; )
                {
                    const uint32_t value = uint32_t(strtoul(p, const_cast<char**>(&p), 16));
                    if (value >= 0x10000)
                        addresses.insert(value);
                    while (*p == ',' || *p == ' ')
                        p++;
                    if (*p != 0 && !isxdigit(uint8_t(*p)))
                        break;
                }
            return addresses;
        }();
        static const bool dumpTexturesEnv = getenv("DR_TEX_DUMP") != nullptr;
        const bool dumpTextures = dumpTexturesEnv || (dumpFrame && g_guestFrame == dumpFrameNumber);
        // Frame dumps (DR_TEX_DUMP=frame) write every mip level and cube face: _L<level>_F<face>.
        // DR_TEX_DUMP=cube: every cube map, all levels and faces.
        static const bool dumpCubes = getenv("DR_TEX_DUMP") != nullptr && strcmp(getenv("DR_TEX_DUMP"), "cube") == 0;
        const bool dumpAll = (dumpFrame && g_guestFrame == dumpFrameNumber) || (!dumpFrame && dumpAddresses.count(address) != 0) || (dumpCubes && decoded.cube);
        for (uint32_t level = 0; dumpTextures && level < (dumpAll ? decoded.levels : 1u); level++)
        for (uint32_t face = 0; face < (dumpAll ? decoded.layers : 1u); face++)
        if (dumpTextures && (dumpCubes ? decoded.cube : dumpFrame ? g_guestFrame == dumpFrameNumber : (dumpAddresses.empty() || dumpAddresses.count(address))))
        {
            std::vector<uint8_t> rgba;
            static thread_local TextureDecode::Decoded view;
            const auto& lv = decoded.level[level];
            view.format = decoded.format;
            view.width = lv.width;
            view.height = lv.height;
            view.rowWidth = lv.rowWidth;
            view.rowPitch = lv.rowPitch;
            view.data.assign(decoded.data.begin() + lv.offset + size_t(face) * lv.layerPitch,
                decoded.data.begin() + std::min<size_t>(decoded.data.size(), lv.offset + size_t(face + 1) * lv.layerPitch));
            if (TextureDecode::ToRGBA8(view, rgba))
            {
                const auto& d = view;
                const uint32_t step = dumpAddresses.empty() && !dumpFrame ? std::max(1u, d.width / 512) : 1;
                const uint32_t w = d.width / step, h = std::max(1u, d.height / step);
                const uint32_t rowBytes = (w * 2 * 3 + 3) & ~3u;
                std::vector<uint8_t> bmp(54 + size_t(rowBytes) * h);
                auto put32 = [&](size_t at, uint32_t v) { memcpy(&bmp[at], &v, 4); };
                bmp[0] = 'B'; bmp[1] = 'M';
                put32(2, uint32_t(bmp.size())); put32(10, 54); put32(14, 40); put32(18, w * 2); put32(22, uint32_t(-int32_t(h)));
                bmp[26] = 1; bmp[28] = 24;
                for (uint32_t y = 0; y < h; y++)
                    for (uint32_t x = 0; x < w; x++)
                    {
                        const uint8_t* t = &rgba[(size_t(y * step) * d.width + x * step) * 4];
                        uint8_t* o = &bmp[54 + size_t(y) * rowBytes];
                        o[x * 3] = t[2]; o[x * 3 + 1] = t[1]; o[x * 3 + 2] = t[0];
                        o[(w + x) * 3] = o[(w + x) * 3 + 1] = o[(w + x) * 3 + 2] = t[3];
                    }
                std::filesystem::create_directories("dump");
                std::ofstream((dumpAll ? fmt::format("dump/tex_{:08X}_{}_{}x{}_frame{}_L{}_F{}.bmp", address, fetch.Format(), decoded.width, decoded.height, g_guestFrame, level, face)
                         : fmt::format("dump/tex_{:08X}_{}_{}x{}_frame{}.bmp", address, fetch.Format(), decoded.width, decoded.height, g_guestFrame)), std::ios::binary)
                    .write(reinterpret_cast<const char*>(bmp.data()), bmp.size());
            }
        }

        if (!reupload)
        {
            entry.descriptor = AllocateDescriptor();
            if (entry.descriptor == DESCRIPTOR_WHITE_2D)
                return entry.descriptor;
        }
        if (!reupload)
        {
            // White until the video thread has replayed the upload.
            std::lock_guard lock(g_resolveMutex);
            g_newResolveDescriptors.push_back(entry.descriptor);
        }
        // Captures (F12) decode every bound texture again: one that comes out different was stale,
        // i.e. its guest memory changed without the cache noticing.
        {
            const uint64_t decodedHash = XXH3_64bits(decoded.data.data(), decoded.data.size());
            if (dumpFrame && g_guestFrame == dumpFrameNumber && reupload && entry.decodedHash != 0 && decodedHash != entry.decodedHash)
                LOGFN_WARNING("[renderer] capture: texture {:08X} fmt {} {}x{} levels {} was stale (decoded at frame {}, sample {} / pages {})",
                    address, fetch.Format(), decoded.width, decoded.height, decoded.levels, entry.decodedFrame,
                    previousSample == sample ? "same" : "changed", pageChanged ? "changed" : "unchecked or same");
            entry.decodedHash = decodedHash;
            entry.decodedFrame = g_guestFrame;
        }
        entry.cube = decoded.cube;
        entry.pages.clear();
        entry.nextPage = 0;
        entry.mipAddress = mips != nullptr ? mipAddress : 0;
        entry.mipsSize = mips != nullptr ? mipsSize : 0;
        HashPages(source, size, entry.pages);
        if (mips != nullptr)
            HashPages(mips, mipsSize, entry.pages);
        if (g_texWatch)
            WatchTexture(keyHash, fetch, mips != nullptr ? mipAddress : 0, mips != nullptr ? mipsSize : 0);
        (reupload ? g_textureStats.reuploaded : g_textureStats.decoded)++;
        g_textureStats.bytes += decoded.data.size();

        frame.commands.push_back(MakeUpload(frame, entry.descriptor, decoded));
        return entry.descriptor;
    }

    // Every 2 s: textures no draw used for 20 s leave the cache; their descriptors go to the video
    // thread, which frees the host textures and hands the descriptors back for reuse.
    void EvictTextures()
    {
        if (g_guestFrame % 60 != 0)
            return;
        constexpr uint32_t kUnusedFrames = 600;
        std::vector<uint64_t> keys;
        std::vector<uint32_t> released;
        for (auto& [key, entry] : g_textureCache)
            if (g_guestFrame - entry.lastUsed > kUnusedFrames)
            {
                keys.push_back(key);
                if (entry.descriptor >= DESCRIPTOR_FIRST_FREE && entry.descriptor < kTextureDescriptorCount)
                    released.push_back(entry.descriptor);
            }
        for (uint64_t key : keys)
        {
            g_textureCache.erase(key);
            if (g_texWatch)
                UnwatchTexture(key);
        }
        g_textureCacheSize = uint32_t(g_textureCache.size());

        std::vector<StreamKey> streamKeys;
        for (auto& [key, entry] : g_streamCache)
            if (g_guestFrame - entry.lastUsed > kUnusedFrames)
            {
                streamKeys.push_back(key);
                g_geometryHeap.Free(entry.resident, entry.residentSize);
                g_geometryHeap.Free(entry.residentAux, entry.residentAuxSize);
            }
        for (auto& key : streamKeys)
            g_streamCache.erase(key);

        std::vector<IndexKey> indexKeys;
        for (auto& [key, entry] : g_indexCache)
            if (g_guestFrame - entry.lastUsed > kUnusedFrames)
            {
                indexKeys.push_back(key);
                g_geometryHeap.Free(entry.resident, entry.residentSize);
            }
        for (auto& key : indexKeys)
            g_indexCache.erase(key);
        if (!released.empty())
        {
            std::lock_guard lock(g_resolveMutex);
            g_releasedDescriptors.insert(g_releasedDescriptors.end(), released.begin(), released.end());
        }
    }

    // Copies the frame's vertex textures as the CPU left them and uploads them before its draws.
    void FlushDynamicTextures(Frame& frame)
    {
        if (g_dynamicTextures.empty())
            return;
        std::vector<Command> uploads;
        static thread_local TextureDecode::Decoded decoded;
        for (auto& [keyHash, fetch] : g_dynamicTextures)
        {
            auto& entry = g_textureCache[keyHash];
            const uint32_t size = TextureDecode::GuestSize(fetch);
            const uint8_t* source = static_cast<const uint8_t*>(g_memory.Translate(fetch.BaseAddress()));
            const uint64_t hash = XXH3_64bits(source, size);
            const uint32_t drawLogFrame = g_captureFrame.load(std::memory_order_relaxed);
            if (g_guestFrame == drawLogFrame && fetch.Format() == 38 && !fetch.Tiled())
            {
                const uint32_t* data = reinterpret_cast<const uint32_t*>(source);
                std::string rows;
                for (uint32_t y = 0; y < fetch.Height(); y++)
                {
                    uint32_t nonzero = 0;
                    for (uint32_t x = 0; x < fetch.Pitch() * 4; x++)
                        nonzero += data[size_t(y) * fetch.Pitch() * 4 + x] != 0;
                    if (nonzero)
                        rows += fmt::format(" {}:{}", y, nonzero);
                }
                LOGFN("[renderer] present: vtex {:08X} changed {} rows{}", fetch.BaseAddress(), hash != entry.sample, rows);
                auto texel = [&](uint32_t x, uint32_t y)
                {
                    const uint32_t* t = data + (size_t(y) * fetch.Pitch() + x) * 4;
                    auto f = [](uint32_t v) { v = (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24); float r; memcpy(&r, &v, 4); return r; };
                    return fmt::format("({:.4g} {:.4g} {:.4g} {:.4g})", f(t[0]), f(t[1]), f(t[2]), f(t[3]));
                };
                for (uint32_t row : { 1u, 2u })
                    for (uint32_t bone : { 0u, 44u, 45u })
                        LOGFN("[renderer]   row {} bone {}: {} {} {}", row, bone, texel(bone * 3, row), texel(bone * 3 + 1, row), texel(bone * 3 + 2, row));
            }
            if (hash == entry.sample && entry.checkedFrame != UINT32_MAX)
                continue;
            const char* reason = "";
            if (!TextureDecode::Decode(fetch, source, nullptr, decoded, reason))
            {
                static std::set<uint32_t> loggedFormats;
                if (loggedFormats.insert(fetch.Format()).second)
                    LOGFN_WARNING("[renderer] vertex texture format {} not supported ({})", fetch.Format(), reason);
                continue;
            }
            const bool first = entry.checkedFrame == UINT32_MAX;
            entry.sample = hash;
            entry.checkedFrame = g_guestFrame;
            (first ? g_textureStats.decoded : g_textureStats.reuploaded)++;
            g_textureStats.bytes += decoded.data.size();
            uploads.push_back(MakeUpload(frame, entry.descriptor, decoded));
        }
        g_dynamicTextures.clear();
        frame.commands.insert(frame.commands.begin(), uploads.begin(), uploads.end());
        frame.leadingUploads = uint32_t(uploads.size());
    }

    struct SharedConstants
    {
        uint32_t texture2D[kSamplerSlots];
        uint32_t texture3D[kSamplerSlots];
        uint32_t textureCube[kSamplerSlots];
        uint32_t sampler[kSamplerSlots];
        uint32_t booleans;
        uint32_t swappedTexcoords;
        float halfPixelOffset[2];
        float alphaThreshold;
        float resolutionScale; // host pixels per guest pixel of the pass (VPOS)
        float inputShift;      // host pixels the pixel shader's inputs are evaluated up and left (shader_recompiler.cpp)
    };

    Frame g_recording;
    // Depth buffers (EDRAM key) the game last drew into with an inverted viewport depth range.
    ankerl::unordered_dense::map<uint32_t, bool> g_depthFlipped;
    // Last uploaded constants of the frame being recorded (consecutive draws often share them).
    struct ConstantCache { std::vector<uint8_t> raw; uint32_t offset = UINT32_MAX; } g_vsCache, g_psCache, g_sharedCache;

    std::mutex g_frameMutex;
    std::condition_variable g_frameTaken; // the video thread took the pending frame
    std::atomic<bool> g_rendererActive{ false }; // Renderer::Init succeeded: frames get consumed
    Frame g_pending;
    bool g_pendingValid = false;
    Frame g_rendering;

    struct Stats
    {
        std::atomic<uint32_t> recorded{ 0 }, noShader{ 0 }, noDeclaration{ 0 }, noBuffers{ 0 },
            unsupportedPrimitive{ 0 }, tooLarge{ 0 }, indexedVertices{ 0 }, stencil{ 0 };
        std::atomic<uint32_t> samples2{ 0 }, samples4{ 0 }; // draws in 2X / 4X MSAA passes
    } g_stats;

    // Where the frame time goes (microseconds, summed between the 5 s log lines).
    struct Timing
    {
        std::atomic<uint64_t> guestFrameUs{ 0 }, guestWaitUs{ 0 }, replayUs{ 0 }, gpuWaitUs{ 0 }, hookUs{ 0 };
        std::atomic<uint32_t> guestFrames{ 0 }, replayedFrames{ 0 };
    } g_timing;

    // Time spent in the renderer's D3D hooks on the guest thread (recording draws, copying data).
    struct HookTimer
    {
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        ~HookTimer()
        {
            const uint64_t us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
            g_timing.hookUs += us;
            HitchStats::Add(HitchStats::Hooks, us);
        }
    };

    // Finer split of the hook time (guest thread only, so plain counters).
    enum Stage { StagePrepare, StageIndices, StageStreams, StageIndexUpload, StageCount };
    uint64_t g_stageNs[StageCount] = {};
    uint64_t g_frameStageNs[StageCount] = {}; // this frame's (the "[hitch]" line)
    struct StageTimer
    {
        Stage stage;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        explicit StageTimer(Stage s) : stage(s) {}
        ~StageTimer()
        {
            const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
            g_stageNs[stage] += ns;
            g_frameStageNs[stage] += ns;
        }
    };

    struct PendingVertices
    {
        bool active = false;
        uint32_t primitive = 0, count = 0, stride = 0, data = 0;
        // BeginIndexedVertices
        bool indexed = false, indices32 = false;
        int32_t baseVertex = 0;
        uint32_t indexCount = 0, indexData = 0;
    } g_pendingVertices;

    void SwapInPlace(uint8_t* p, uint32_t size, Swap swap)
    {
        if (swap == Swap::Swap32)
            for (uint32_t i = 0; i + 4 <= size; i += 4)
                *reinterpret_cast<uint32_t*>(p + i) = __builtin_bswap32(*reinterpret_cast<uint32_t*>(p + i));
        else if (swap == Swap::Swap16)
            for (uint32_t i = 0; i + 2 <= size; i += 2)
                *reinterpret_cast<uint16_t*>(p + i) = __builtin_bswap16(*reinterpret_cast<uint16_t*>(p + i));
    }

    // Copies vertexCount vertices of one stream into the frame, swapping each element to host order.
    uint32_t CopyVertices(Frame& frame, const VertexDeclaration& decl, uint32_t stream, uint32_t source, uint32_t stride, uint32_t vertexCount)
    {
        const uint32_t size = vertexCount * stride;
        const uint32_t offset = frame.Allocate(size, 16);
        uint8_t* dst = frame.data.data() + offset;
        memcpy(dst, g_memory.Translate(source), size);

        for (uint32_t v = 0; v < vertexCount; v++)
            for (auto& e : decl.elements)
                if (e.stream == stream && e.offset + e.info.size <= stride)
                    SwapInPlace(dst + v * stride + e.offset, e.info.size, e.info.swap);

        return offset;
    }

    // Fills the auxiliary stream of 'stream' (converted elements as float4) from vertices already
    // copied and swapped at frame offset 'offset'; returns its frame offset.
    uint32_t CopyAux(Frame& frame, const VertexDeclaration& decl, uint32_t stream, uint32_t offset, uint32_t stride, uint32_t vertexCount)
    {
        const uint32_t auxStride = decl.auxStride[stream];
        const uint32_t aux = frame.Allocate(vertexCount * auxStride, 16);
        const uint8_t* src = frame.data.data() + offset;
        uint8_t* dst = frame.data.data() + aux;
        for (uint32_t v = 0; v < vertexCount; v++)
            for (auto& e : decl.elements)
            {
                if (!e.converted || e.stream != stream)
                    continue;
                float value[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                if (e.offset + e.info.size <= stride)
                    DecodeElement(e.type, src + size_t(v) * stride + e.offset, value);
                memcpy(dst + size_t(v) * auxStride + e.auxOffset, value, sizeof(value));
            }
        return aux;
    }

    void RecordAux(Frame& frame, const VertexDeclaration& decl, uint32_t stream, uint32_t offset, uint32_t stride, uint32_t vertexCount, Command& cmd)
    {
        if (stream >= kAuxSlotBase || decl.auxStride[stream] == 0)
            return;
        const uint32_t slot = kAuxSlotBase + stream;
        cmd.key.strides[slot] = uint16_t(decl.auxStride[stream]);
        cmd.streamOffset[slot] = CopyAux(frame, decl, stream, offset, stride, vertexCount);
        cmd.streamSize[slot] = vertexCount * decl.auxStride[stream];
        cmd.streamMask |= 1u << slot;
    }

    uint32_t RecordConstants(Frame& frame, ConstantCache& cache, const void* source, uint32_t size, bool swap)
    {
        if (cache.offset != UINT32_MAX && cache.raw.size() == size && memcmp(cache.raw.data(), source, size) == 0)
            return cache.offset;

        cache.raw.assign(static_cast<const uint8_t*>(source), static_cast<const uint8_t*>(source) + size);
        cache.offset = frame.Allocate(size, 256);
        memcpy(frame.data.data() + cache.offset, source, size);
        if (swap)
            SwapInPlace(frame.data.data() + cache.offset, size, Swap::Swap32);
        return cache.offset;
    }

    RenderFormat ColorFormatOf(uint32_t colorInfo)
    {
        switch ((colorInfo >> 16) & 0xF)
        {
        case 0: case 1: return RenderFormat::R8G8B8A8_UNORM;   // 8_8_8_8 (+ gamma)
        // The 10:10:10:2 formats alias the same EDRAM bits: Dead Rising draws its HDR scene as
        // 2_10_10_10_FLOAT (3, 12) and resolves it as 2_10_10_10 (2) into an HDR texture, so all of
        // them share one host buffer, kept as 16-bit float.
        case 2: case 3: case 10: case 12: return RenderFormat::R16G16B16A16_FLOAT;
        case 14: return RenderFormat::R32_FLOAT;
        case 15: return RenderFormat::R32G32_FLOAT;
        case 4: case 6: return RenderFormat::R16G16_FLOAT;
        default: return RenderFormat::R16G16B16A16_FLOAT;     // 2_10_10_10_FLOAT, 16_16_16_16(_FLOAT)
        }
    }

    // MSAA samples of the bound surface: RB_SURFACE_INFO bits 16-17 (1X, 2X, 4X). The 3D scene pass
    // is 4X (some scenes 2X), shadow maps and post-processing 1X. The host renders each pass with
    // the console's sample count; DR_MSAA=0 renders every pass with one sample.
    // Clears recorded since the last draw or resolve (indices into the recording). D3D writes a new
    // surface setup (RB_SURFACE_INFO) only when it next draws, so a clear right after switching render
    // targets reads the previous pass's sample count and would clear the wrong host target; the
    // next draw or resolve on the same buffers settles it.
    std::vector<size_t> g_pendingClears;

    template <typename CommandList>
    void SettlePendingClears(CommandList& commands, uint32_t colorKey, uint32_t depthKey, uint32_t samples)
    {
        for (size_t index : g_pendingClears)
            if (index < commands.size() && commands[index].colorKey == colorKey && commands[index].depthKey == depthKey)
                commands[index].samples = samples;
        g_pendingClears.clear();
    }

    // Surface setups (RB_SURFACE_INFO) drawn single-sampled for the rest of the frame: an MSAA pass
    // that starts by restoring a single-sample scene (a full-screen quad writing depth from the
    // shader). The restore writes one depth per pixel, at the pixel centre; the redraws that follow
    // (floor, characters: "equal or nearer") test every sample at its own position, and on steep
    // surfaces most failed, which left them black. The scene of such frames is single-sample
    // anyway, and single-sampled the pass shares the scene's host target as before MSAA.
    std::vector<uint32_t> g_singleSampleSurfaces;
    std::vector<uint32_t> g_restoredMsaaSurfaces; // MSAA passes a depth-export draw restored this frame

    uint32_t SurfaceSamples(uint32_t device)
    {
        static const bool off = getenv("DR_MSAA") != nullptr && atoi(getenv("DR_MSAA")) <= 1;
        if (off || !Config::MSAA)
            return 1;
        const uint32_t surfaceInfo = Register(device, RB_SURFACE_INFO);
        if (std::find(g_singleSampleSurfaces.begin(), g_singleSampleSurfaces.end(), surfaceInfo) != g_singleSampleSurfaces.end())
            return 1;
        const uint32_t mode = (Register(device, RB_SURFACE_INFO) >> 16) & 3;
        return mode == 2 ? 4 : mode == 1 ? 2 : 1;
    }

    // Fills everything of a draw that does not depend on its vertices. False: skip the draw.
    bool PrepareDraw(Frame& frame, uint32_t device, uint32_t primitive, Command& cmd, const VertexDeclaration*& decl)
    {
        memset(static_cast<void*>(&cmd), 0, sizeof(cmd));
        cmd.type = Command::Draw;

        auto& key = cmd.key;
        key.vertexShader = Shaders::Find(Load32(device + VertexShader));
        key.pixelShader = Shaders::Find(Load32(device + PixelShader));
        if (key.vertexShader == nullptr)
        {
            g_stats.noShader++;
            return false;
        }

        // Debugging: DR_SKIP_VS=<hash>,<hash>,... leaves out the draws of those vertex shaders.
        static const std::set<uint64_t> skipShaders = []
        {
            std::set<uint64_t> hashes;
            if (const char* list = getenv("DR_SKIP_VS"))
                for (const char* p = list; *p; )
                {
                    hashes.insert(strtoull(p, const_cast<char**>(&p), 16));
                    while (*p == ',' || *p == ' ')
                        p++;
                }
            return hashes;
        }();
        if (!skipShaders.empty() && skipShaders.count(key.vertexShader->hash))
            return false;
        // DR_SKIP_PS=<hash>,... the same for pixel shaders.
        static const std::set<uint64_t> skipPixelShaders = []
        {
            std::set<uint64_t> hashes;
            if (const char* list = getenv("DR_SKIP_PS"))
                for (const char* p = list; *p; )
                {
                    hashes.insert(strtoull(p, const_cast<char**>(&p), 16));
                    while (*p == ',' || *p == ' ')
                        p++;
                }
            return hashes;
        }();
        if (!skipPixelShaders.empty() && key.pixelShader != nullptr && skipPixelShaders.count(key.pixelShader->hash))
            return false;

        decl = FindDeclaration(Load32(device + 0x2D18));
        if (decl == nullptr)
        {
            g_stats.noDeclaration++;
            return false;
        }
        key.declaration = decl;

        switch (primitive)
        {
        case 1: key.topology = RenderPrimitiveTopology::POINT_LIST; break;
        case 2: key.topology = RenderPrimitiveTopology::LINE_LIST; break;
        case 3: key.topology = RenderPrimitiveTopology::LINE_STRIP; break;
        case 6: key.topology = RenderPrimitiveTopology::TRIANGLE_STRIP; break;
        case 4: case 5: case 8: case 13: key.topology = RenderPrimitiveTopology::TRIANGLE_LIST; break; // converted
        default:
            g_stats.unsupportedPrimitive++;
            return false;
        }

        // Render state from the Xenos register shadows.
        const uint32_t depthControl = Register(device, RB_DEPTHCONTROL);
        key.depthControl = depthControl & 0x76; // z enable (1), z write (2), z func (4-6)
        // Stencil (bit 0; func / fail / z pass / z fail from bit 8, back faces from bit 20 when bit
        // 7 is set) with RB_STENCILREFMASK: the game marks moving characters in stencil so its
        // camera motion blur leaves them out. DR_NO_STENCIL=1 (debugging) ignores it.
        static const bool noStencil = getenv("DR_NO_STENCIL") != nullptr;
        if ((depthControl & 1) && !noStencil && Load32(device + DepthStencil) != 0)
        {
            key.depthControl |= depthControl & ((depthControl & 0x80) ? 0xFFFFFF81u : 0x000FFF01u);
            key.stencilRefMask = Register(device, RB_STENCILREFMASK) & 0xFFFFFF;
            g_stats.stencil++;
        }
        static const bool noDepthTest = getenv("DR_NO_DEPTH_TEST") != nullptr; // debugging
        if (noDepthTest)
            key.depthControl &= ~0x2u;
        // Debugging: DR_ZALWAYS_VS=<hash> draws that vertex shader with the depth test passing always
        // (depth still written, to see its depth in the depth resolve dumps).
        static const uint64_t zAlwaysShader = getenv("DR_ZALWAYS_VS") ? strtoull(getenv("DR_ZALWAYS_VS"), nullptr, 16) : 0;
        if (zAlwaysShader != 0 && key.vertexShader->hash == zAlwaysShader)
            key.depthControl |= 0x70;
        key.blendControl = Register(device, RB_BLENDCONTROL0) & 0x1FFF1FFF;
        key.colorMask = Register(device, RB_COLOR_MASK) & 0xF;
        if ((Register(device, RB_MODECONTROL) & 7) == 5) // depth only
            key.colorMask = 0;
        key.cull = Register(device, PA_SU_SC_MODE_CNTL) & 7;
        // Survey: polygon offset (decals) is not mapped yet; log what the game asks for.
        if (const uint32_t modeCntl = Register(device, PA_SU_SC_MODE_CNTL); (modeCntl & 0x3800) != 0)
        {
            static uint32_t logged;
            if (logged++ < 12)
                LOGFN("[renderer] polygon offset: mode cntl {:08X} front scale {} offset {} back scale {} offset {} (ps {:016X})", modeCntl,
                    RegisterFloat(device, 0x2380), RegisterFloat(device, 0x2381), RegisterFloat(device, 0x2382), RegisterFloat(device, 0x2383),
                    key.pixelShader ? key.pixelShader->hash : 0);
        }

        const uint32_t colorControl = Register(device, RB_COLORCONTROL);
        // Alpha to coverage (bit 4: foliage and hair, with the 4X MSAA scene pass) is the host's alpha
        // to coverage in MSAA passes, and an alpha test at 0.5 without MSAA (DR_MSAA=0).
        static const bool noAlphaToMask = getenv("DR_NO_A2C") != nullptr; // debugging
        const bool alphaToMask = (colorControl & 0x10) != 0 && !noAlphaToMask;
        key.samples = SurfaceSamples(device);
        // Gameplay frames draw the scene single-sampled, restore its colour and depth into the 4X pass
        // with a depth-export draw (PS 8CF02D5E: one depth per pixel, at its centre), then redraw some
        // of it in 4X with "nearer or equal". Each sample sits up to 3/8 pixel off the centre, so on
        // sloped surfaces half of them failed against the restored depth (black floors and characters
        // on the roof). The draws after the restore get a slope-scaled bias toward the viewer instead
        // (kPipelineMsaaRestoreBias). DR_MSAA_DEMOTE=1: the old fix, the pass single-sampled for the
        // frame (which left 4X MSAA on about 1% of the draws in gameplay).
        static const bool demote = getenv("DR_MSAA_DEMOTE") != nullptr;
        const uint32_t surfaceInfo = Register(device, RB_SURFACE_INFO);
        if (key.samples > 1 && key.pixelShader != nullptr && (key.pixelShader->pixelOutputs & 0x10) != 0)
        {
            if (demote)
            {
                g_singleSampleSurfaces.push_back(surfaceInfo);
                key.samples = 1;
            }
            else if (std::find(g_restoredMsaaSurfaces.begin(), g_restoredMsaaSurfaces.end(), surfaceInfo) == g_restoredMsaaSurfaces.end())
                g_restoredMsaaSurfaces.push_back(surfaceInfo);
        }
        else if (key.samples > 1 && std::find(g_restoredMsaaSurfaces.begin(), g_restoredMsaaSurfaces.end(), surfaceInfo) != g_restoredMsaaSurfaces.end())
            key.flags |= kPipelineMsaaRestoreBias;
        if (key.samples == 2)
            g_stats.samples2++;
        else if (key.samples == 4)
            g_stats.samples4++;
        // Hardware alpha to coverage blacked out whole scenes (the roof on Vulkan, a staff corridor in
        // 2X on D3D12), so these draws keep the alpha test at 0.5; DR_A2C=1 (debugging) uses it.
        static const bool hardwareCoverage = getenv("DR_A2C") != nullptr && atoi(getenv("DR_A2C")) != 0;
        const bool coverage = hardwareCoverage && alphaToMask && key.samples > 1;
        if (coverage)
            key.flags |= kPipelineAlphaToCoverage;
        const bool alphaTest = ((colorControl & 8) != 0 && (colorControl & 7) != 7) || (alphaToMask && !coverage);
        static const bool noAlphaTest = getenv("DR_NO_ALPHA_TEST") != nullptr; // debugging
        if (alphaTest && key.pixelShader != nullptr && !noAlphaTest)
            key.specConstants |= Shaders::SPEC_ALPHA_TEST;

        // Render targets. Render targets 1-3 the pixel shader exports to and that alias render
        // target 0's EDRAM buffer (the EDRAM restore writes its colour through RT1, a fixed-point
        // view of the HDR buffer): the highest one wins, as on the console.
        uint32_t colorInfo = Register(device, RB_COLOR_INFO);
        cmd.colorSurface = Load32(device + RenderTarget0);
        if (key.pixelShader != nullptr && cmd.colorSurface != 0 && (key.pixelShader->pixelOutputs & 0xE) != 0)
        {
            static constexpr uint32_t kColorInfo[4] = { RB_COLOR_INFO, 0x2003, 0x2004, 0x2005 };
            const uint32_t mask = Register(device, RB_COLOR_MASK);
            for (uint32_t rt = 3; rt >= 1; rt--)
            {
                const uint32_t surface = Load32(device + RenderTarget0 + rt * 4);
                const uint32_t info = Register(device, kColorInfo[rt]);
                if ((key.pixelShader->pixelOutputs & (1u << rt)) == 0 || surface == 0 || ((mask >> (rt * 4)) & 0xF) == 0)
                    continue;
                if ((info & 0xFFF) == (colorInfo & 0xFFF))
                {
                    colorInfo = info;
                    cmd.colorSurface = surface;
                    key.colorMask = (mask >> (rt * 4)) & 0xF;
                    if (key.pixelShader->pixelOutputs & 1)
                        key.specConstants |= Shaders::SPEC_MRT_ALIAS;
                }
                else
                {
                    static bool logged;
                    if (!logged)
                        LOGFN_WARNING("[renderer] render target {} in its own EDRAM buffer ({:08X} vs {:08X}): not drawn", rt, info, colorInfo);
                    logged = true;
                }
                break;
            }
        }
        cmd.depthSurface = Load32(device + DepthStencil);
        cmd.colorKey = cmd.colorSurface != 0 ? (colorInfo & 0xFFF) + 1 : 0;
        cmd.depthKey = cmd.depthSurface != 0 ? (Register(device, RB_DEPTH_INFO) & 0xFFF) + 1 : 0;
        const uint32_t sizeSurface = cmd.colorSurface != 0 ? cmd.colorSurface : cmd.depthSurface;
        if (sizeSurface == 0)
            return false;
        key.colorFormat = cmd.colorSurface != 0 ? ColorFormatOf(colorInfo) : RenderFormat::UNKNOWN;
        cmd.colorFormat = key.colorFormat;
        cmd.samples = key.samples;
        SettlePendingClears(frame.commands, cmd.colorKey, cmd.depthKey, cmd.samples);

        // The 10:10:10:2 fixed-point views (formats 2, 10) of the HDR 7e3 buffer take raw bits: the
        // pixel shader's output is decoded into the values the host buffer holds.
        const uint32_t guestColorFormat = (colorInfo >> 16) & 0xF;
        if (key.pixelShader != nullptr && cmd.colorSurface != 0 && (guestColorFormat == 2 || guestColorFormat == 10) &&
            key.colorFormat == RenderFormat::R16G16B16A16_FLOAT)
            key.specConstants |= Shaders::SPEC_DECODE_7E3;

        {
            static uint32_t lastColorInfo = UINT32_MAX, logged;
            const uint32_t colorInfo = Register(device, RB_COLOR_INFO);
            if (colorInfo != lastColorInfo && frame.commands.size() > 200 && logged < 40)
            {
                logged++;
                LOGFN("[renderer] draw #{}: surface {:08X} colorinfo {:08X} depthinfo {:08X} surface info {:08X} depthcontrol {:08X} modecontrol {:08X} viewport z {}..{} vport zscale {} zoffset {} vte {:08X} clip {:08X}",
                    frame.commands.size(), cmd.colorSurface, colorInfo, Register(device, RB_DEPTH_INFO), Register(device, RB_SURFACE_INFO),
                    Register(device, RB_DEPTHCONTROL), Register(device, RB_MODECONTROL), LoadFloat(device + Viewport + 16), LoadFloat(device + Viewport + 20),
                    RegisterFloat(device, 0x2113), RegisterFloat(device, 0x2114), Register(device, PA_CL_VTE_CNTL), Register(device, 0x2204));
            }
            lastColorInfo = colorInfo;
        }

        cmd.viewport.x = float(Load32(device + Viewport));
        cmd.viewport.y = float(Load32(device + Viewport + 4));
        cmd.viewport.width = float(Load32(device + Viewport + 8));
        cmd.viewport.height = float(Load32(device + Viewport + 12));
        cmd.viewport.minDepth = LoadFloat(device + Viewport + 16);
        cmd.viewport.maxDepth = LoadFloat(device + Viewport + 20);

        // Reversed Z: the game inverts the viewport depth range (MinZ 1, MaxZ 0) and tests GEQUAL
        // against a depth buffer cleared to 0. Host APIs do not all accept an inverted range: the
        // vertex shader outputs z' = w - z instead (SPEC_FLIP_Z) and the range is swapped, which
        // gives the same depth values, so clears, depth exports and depth resolves need nothing.
        const bool flipZ = cmd.viewport.minDepth > cmd.viewport.maxDepth;
        if (flipZ)
        {
            std::swap(cmd.viewport.minDepth, cmd.viewport.maxDepth);
            key.specConstants |= Shaders::SPEC_FLIP_Z;
        }
        if (cmd.depthKey != 0)
            g_depthFlipped[cmd.depthKey] = false;

        // Tiled surfaces are smaller than the screen they cover: size the host target by both.
        cmd.width = std::max(SurfaceWidth(sizeSurface), uint32_t(cmd.viewport.x + cmd.viewport.width));
        cmd.height = std::max(SurfaceHeight(sizeSurface), uint32_t(cmd.viewport.y + cmd.viewport.height));
        cmd.width = std::min(cmd.width, 4096u);
        cmd.height = std::min(cmd.height, 4096u);
        if (cmd.width > 2048 || cmd.height > 2048)
        {
            static uint32_t logged;
            if (logged++ < 24)
                LOGFN("[renderer] large pass: draw {} {}x{} surface {}x{} viewport {} {} {} {} colorinfo {:08X} depthinfo {:08X} surface info {:08X} vs {:016X} ps {:016X}",
                    frame.commands.size(), cmd.width, cmd.height, SurfaceWidth(sizeSurface), SurfaceHeight(sizeSurface), cmd.viewport.x, cmd.viewport.y,
                    cmd.viewport.width, cmd.viewport.height, Register(device, RB_COLOR_INFO), Register(device, RB_DEPTH_INFO), Register(device, RB_SURFACE_INFO),
                    key.vertexShader->hash, key.pixelShader ? key.pixelShader->hash : 0);
        }

        // Constants.
        cmd.vertexConstants = RecordConstants(frame, g_vsCache, g_memory.Translate(device + VertexShaderConstants), 256 * 16, true);
        cmd.pixelConstants = RecordConstants(frame, g_psCache, g_memory.Translate(device + PixelShaderConstants), 224 * 16, true);

        // The EDRAM restore pair (PS F3F97E7B, then E52354E8 undoes it) moves pixels around inside
        // 4x4 blocks: position = texcoord x c4.xy (the screen in pixels), integer part permuted,
        // fetched at position x c4.zw. At a resolution scale its blocks must be host pixels, or
        // the two passes do not cancel (at 1.5x a guest pixel is 1.5 host pixels: doubled rows and
        // letters). This draw gets its own copy of the constants with c4 in host pixels.
        const bool restoreShuffle = key.pixelShader != nullptr &&
            (key.pixelShader->hash == 0xF3F97E7BE96257CEull || key.pixelShader->hash == 0xE52354E8EC7D87D5ull);
        if (restoreShuffle)
        {
            const float scale = PassScale(cmd.width, cmd.height, frame.scale);
            if (scale != 1.0f)
            {
                const uint32_t copy = frame.Allocate(224 * 16, 256);
                memcpy(frame.data.data() + copy, frame.data.data() + cmd.pixelConstants, 224 * 16);
                float* c4 = reinterpret_cast<float*>(frame.data.data() + copy + 4 * 16);
                c4[0] *= scale;
                c4[1] *= scale;
                c4[2] /= scale;
                c4[3] /= scale;
                cmd.pixelConstants = copy;
            }
        }

        SharedConstants shared{};
        for (uint32_t i = 0; i < kSamplerSlots; i++)
        {
            shared.texture2D[i] = DESCRIPTOR_WHITE_2D; // textures are not translated yet
            shared.texture3D[i] = DESCRIPTOR_NULL_3D;
            shared.textureCube[i] = DESCRIPTOR_NULL_CUBE;
            shared.sampler[i] = 0;
        }
        // Textures: the game's render targets (resolve destinations) as the host rendered them,
        // everything else decoded from guest memory. Sampler state from the device's fetch constant.
        for (uint32_t s = 0; s < 26; s++)
        {
            const uint32_t texture = Load32(device + Textures + s * 4);
            TextureDecode::FetchConstant samplerFetch;
            for (int i = 0; i < 6; i++)
                samplerFetch.dword[i] = Load32(device + 0x400 + s * 0x18 + i * 4);

            // Vertex textures (fetch constants 16+) are often set straight through the fetch
            // constant, with no texture object. Its address is physical.
            TextureDecode::FetchConstant fetch;
            if (texture != 0)
            {
                for (int i = 0; i < 6; i++)
                    fetch.dword[i] = Load32(texture + 0x1C + i * 4);
            }
            else if (s >= 16 && (samplerFetch.dword[0] & 3) == 2 && samplerFetch.BaseAddress() != 0)
            {
                fetch = samplerFetch;
                if ((fetch.dword[1] & 0xE0000000) == 0)
                    fetch.dword[1] |= 0xA0000000;
            }
            else
                continue;

            static const bool defaultSampler = getenv("DR_DEFAULT_SAMPLER") != nullptr; // debugging
            shared.sampler[s] = defaultSampler ? 0 : SamplerIndex(samplerFetch);
            // Vertex fetches (16+) load texels unless the fetch constant asks for filtering (bit 31,
            // shader_common.h); 2D only, as the 3D and cube fetches use the index as it is.
            if (s >= 16 && fetch.Dimension() <= 1 && (samplerFetch.MagFilter() == 1 || samplerFetch.MinFilter() == 1))
                shared.sampler[s] |= 0x80000000u;

            // Debugging: DR_TEX_WHITE_SLOT=<bitmask> shows the textures of those samplers as white
            // (resolve targets included).
            static const uint32_t whiteSlots = getenv("DR_TEX_WHITE_SLOT") ? uint32_t(strtoul(getenv("DR_TEX_WHITE_SLOT"), nullptr, 0)) : 0;
            if (whiteSlots & (1u << s))
                continue;

            bool resolved = false;
            {
                std::lock_guard lock(g_resolveMutex);
                auto it = g_latestResolve.find(ResolveKey(fetch.BaseAddress(), fetch.Format()));
                // Block-compressed textures (DXT, DXN, CTX1, DXT3A/5A) are never resolve destinations:
                // one over memory a render target was resolved into earlier is a texture loaded since.
                const uint32_t format = fetch.Format();
                const bool compressed = (format >= 18 && format <= 20) || format == 49 || (format >= 51 && format <= 53) || (format >= 58 && format <= 61);
                if (it != g_latestResolve.end() && compressed)
                {
                    static uint32_t logged;
                    if (logged++ < 32)
                        LOGFN("[renderer] frame {}: compressed texture {:08X} fmt {} {}x{} over an earlier resolve target ({}x{}): decoded",
                            g_guestFrame, fetch.BaseAddress(), format, fetch.Width(), fetch.Height(), it->second.width, it->second.height);
                    it = g_latestResolve.end();
                }
                if (it != g_latestResolve.end() && g_guestFrame - it->second.frame > 2 && it->second.size != 0 &&
                    SampleHash(static_cast<const uint8_t*>(g_memory.Translate(it->second.address)), it->second.size) != it->second.memory)
                {
                    // Not resolved lately, and the CPU has loaded something else there since.
                    static uint32_t logged;
                    if (logged++ < 32)
                        LOGFN("[renderer] frame {}: resolve target {:08X} ({}x{}, last resolved frame {}) overwritten since: texture fmt {} {}x{} decoded",
                            g_guestFrame, it->second.address, it->second.width, it->second.height, it->second.frame, format,
                            fetch.Width(), fetch.Height());
                    g_latestResolve.erase(it);
                    it = g_latestResolve.end();
                }
                if (it != g_latestResolve.end() && (fetch.Width() != it->second.width || fetch.Height() != it->second.height))
                {
                    // Another texture object over that memory (a different size): not the render target.
                    static uint32_t logged;
                    if (logged++ < 32)
                        LOGFN("[renderer] frame {}: texture {:08X} fmt {} {}x{} is not the {}x{} resolve target there: decoded",
                            g_guestFrame, fetch.BaseAddress(), fetch.Format(), fetch.Width(), fetch.Height(), it->second.width, it->second.height);
                    it = g_latestResolve.end();
                }
                if (it != g_latestResolve.end())
                {
                    shared.sampler[s] |= ScaleCode(it->second.scale);
                    // Cube maps rendered face by face (environment captures) have a cube view.
                    if (fetch.Dimension() == 3)
                        shared.textureCube[s] = it->second.descriptor;
                    else
                        shared.texture2D[s] = it->second.descriptor;
                    resolved = true;
                }
            }
            if (resolved)
                continue;

            bool cube = false;
            static const bool staticVertexTextures = getenv("DR_STATIC_VTF") != nullptr; // debugging: old behaviour
            const uint32_t descriptor = TextureDescriptor(frame, fetch, cube, s >= 16 && !staticVertexTextures);
            if (cube)
                shared.textureCube[s] = descriptor == DESCRIPTOR_WHITE_2D ? DESCRIPTOR_NULL_CUBE : descriptor;
            else
                shared.texture2D[s] = descriptor;
        }
        // Debugging: DR_DRAW_LOG=<guest frame> logs every draw of that frame (shaders, textures).
        const uint32_t drawLogFrame = g_captureFrame.load(std::memory_order_relaxed);
        if (g_guestFrame == drawLogFrame)
        {
            std::string textures;
            for (uint32_t s = 16; s < 20; s++)
                if (Load32(device + Textures + s * 4) == 0 && (Load32(device + 0x400 + s * 0x18) & 3) == 2)
                    textures += fmt::format(" fetch{}=[{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}]", s,
                        Load32(device + 0x400 + s * 0x18), Load32(device + 0x404 + s * 0x18), Load32(device + 0x408 + s * 0x18),
                        Load32(device + 0x40C + s * 0x18), Load32(device + 0x410 + s * 0x18), Load32(device + 0x414 + s * 0x18));
            for (uint32_t s = 0; s < 26; s++)
                if (const uint32_t texture = Load32(device + Textures + s * 4))
                {
                    TextureDecode::FetchConstant f;
                    for (int i = 0; i < 6; i++)
                        f.dword[i] = Load32(texture + 0x1C + i * 4);
                    textures += fmt::format(" s{}={:08X}/{}/{}x{} [{:08X} {:08X} {:08X} {:08X}]", s, f.BaseAddress(), f.Format(), f.Width(), f.Height(),
                        Load32(device + 0x400 + s * 0x18), Load32(device + 0x400 + s * 0x18 + 4), Load32(device + 0x400 + s * 0x18 + 12), Load32(device + 0x400 + s * 0x18 + 16));
                }
            LOGFN("[renderer] draw {} prim {} vs {:016X} ps {:016X} decl {:08X} rt {:08X} depth {:08X} cmask {:X} blend {:08X} z {:X} dc {:08X} srm {:06X}:{}",
                frame.commands.size(), primitive, key.vertexShader->hash, key.pixelShader ? key.pixelShader->hash : 0, decl->guest,
                cmd.colorKey, cmd.depthKey, key.colorMask, key.blendControl, key.depthControl, Register(device, RB_DEPTHCONTROL),
                Register(device, RB_STENCILREFMASK) & 0xFFFFFF, textures);
            std::string constants;
            for (uint32_t c : { 0u, 1u, 2u, 3u, 4u, 8u, 9u, 10u, 11u, 12u, 13u })
                constants += fmt::format(" c{}=({:.3g} {:.3g} {:.3g} {:.3g})", c,
                    LoadFloat(device + VertexShaderConstants + c * 16), LoadFloat(device + VertexShaderConstants + c * 16 + 4),
                    LoadFloat(device + VertexShaderConstants + c * 16 + 8), LoadFloat(device + VertexShaderConstants + c * 16 + 12));
            LOGFN("[renderer]   vs constants{}", constants);
            // Vertex textures: the rows of bone data written by now (c5/c7 select them).
            if (const uint32_t vtex = Load32(device + Textures + 16 * 4))
            {
                TextureDecode::FetchConstant f;
                for (int i = 0; i < 6; i++)
                    f.dword[i] = Load32(vtex + 0x1C + i * 4);
                if (f.Format() == 38 && !f.Tiled())
                {
                    const uint32_t* data = static_cast<const uint32_t*>(g_memory.Translate(f.BaseAddress()));
                    std::string rows;
                    for (uint32_t y = 0; y < f.Height(); y++)
                    {
                        uint32_t nonzero = 0;
                        for (uint32_t x = 0; x < f.Pitch() * 4; x++)
                            nonzero += data[size_t(y) * f.Pitch() * 4 + x] != 0;
                        if (nonzero)
                            rows += fmt::format(" {}:{}", y, nonzero);
                    }
                    LOGFN("[renderer]   vtex {:08X} c5=({:.4g} {:.4g} {:.4g} {:.4g}) c7=({:.4g} {:.4g} {:.4g} {:.4g}) rows{}", f.BaseAddress(),
                        LoadFloat(device + VertexShaderConstants + 80), LoadFloat(device + VertexShaderConstants + 84),
                        LoadFloat(device + VertexShaderConstants + 88), LoadFloat(device + VertexShaderConstants + 92),
                        LoadFloat(device + VertexShaderConstants + 112), LoadFloat(device + VertexShaderConstants + 116),
                        LoadFloat(device + VertexShaderConstants + 120), LoadFloat(device + VertexShaderConstants + 124), rows);
                }
            }
            std::string psConstants;
            for (uint32_t c = 0; c < 12; c++)
                psConstants += fmt::format(" c{}=({:.3g} {:.3g} {:.3g} {:.3g})", c,
                    LoadFloat(device + PixelShaderConstants + c * 16), LoadFloat(device + PixelShaderConstants + c * 16 + 4),
                    LoadFloat(device + PixelShaderConstants + c * 16 + 8), LoadFloat(device + PixelShaderConstants + c * 16 + 12));
            LOGFN("[renderer]   ps constants{}", psConstants);
            std::string psConstants2;
            for (uint32_t c = 12; c < 48; c++)
                psConstants2 += fmt::format(" c{}=({:.3g} {:.3g} {:.3g} {:.3g})", c,
                    LoadFloat(device + PixelShaderConstants + c * 16), LoadFloat(device + PixelShaderConstants + c * 16 + 4),
                    LoadFloat(device + PixelShaderConstants + c * 16 + 8), LoadFloat(device + PixelShaderConstants + c * 16 + 12));
            LOGFN("[renderer]   ps constants 12-47{} bools vs {:08X} ps {:08X}", psConstants2, Load32(device + VertexBooleans), Load32(device + PixelBooleans));
            LOGFN("[renderer]   colorcontrol {:08X} alpharef {} spec {:X}", Register(device, RB_COLORCONTROL), RegisterFloat(device, RB_ALPHA_REF), key.specConstants);
            LOGFN("[renderer]   clip cntl {:08X} sc mode cntl {:08X}", Register(device, 0x2204), Register(device, PA_SU_SC_MODE_CNTL));
            LOGFN("[renderer]   vtx cntl {:08X} vte {:08X} vport scale/offset x {} {} y {} {} z {} {}; viewport {} {} {} {}", Register(device, 0x2302), Register(device, PA_CL_VTE_CNTL),
                RegisterFloat(device, 0x210F), RegisterFloat(device, 0x2110), RegisterFloat(device, 0x2111), RegisterFloat(device, 0x2112),
                RegisterFloat(device, 0x2113), RegisterFloat(device, 0x2114), cmd.viewport.x, cmd.viewport.y, cmd.viewport.width, cmd.viewport.height);
            LOGFN("[renderer]   targets {:08X} {:08X} {:08X} {:08X} color info {:08X} {:08X} {:08X} {:08X} mask {:08X} surface {:08X} depth {:08X}",
                Load32(device + RenderTarget0), Load32(device + RenderTarget0 + 4), Load32(device + RenderTarget0 + 8), Load32(device + RenderTarget0 + 12),
                Register(device, RB_COLOR_INFO), Register(device, 0x2003), Register(device, 0x2004), Register(device, 0x2005),
                Register(device, RB_COLOR_MASK), Register(device, RB_SURFACE_INFO), Register(device, RB_DEPTH_INFO));
        }

        shared.booleans = (Load32(device + VertexBooleans) & 0xFFFF) | ((Load32(device + PixelBooleans) & 0xFFFF) << 16);
        static const bool debugUV = getenv("DR_DEBUG_UV") != nullptr; // debugging: 2D fetches return their coordinate
        if (debugUV)
            shared.swappedTexcoords |= 0x80000000u;
        shared.alphaThreshold = ((Register(device, RB_COLORCONTROL) & 0x18) == 0x10) ? 0.5f : RegisterFloat(device, RB_ALPHA_REF);
        shared.resolutionScale = PassScale(cmd.width, cmd.height, frame.scale);
        // Pixel centres: PA_SU_VTX_CNTL.PIX_CENTER 0 is the D3D9 convention (pixel k sampled at
        // window coordinate k); host APIs sample at k + 0.5, so the shaders' inputs must be
        // evaluated half a guest pixel up and left. Without it, full-screen passes sample exactly
        // on texel edges (point-sampled copies pick neighbours, and the game's 4x4 EDRAM pixel
        // shuffle stops cancelling out).
        // The geometry stays put, so guest pixel k covers host pixels [s k, s k + s) with every
        // MSAA sample, and the pixel shaders evaluate their inputs s / 2 host pixels up and left
        // (shared.inputShift, shader_recompiler.cpp): the texture coordinates the console's pixel
        // centres give. Moving the geometry half a pixel instead (the old way, DR_GEOMETRY_HALF_PIXEL=1)
        // left the first column and row, or their left MSAA samples, uncovered: a half-dark first
        // column (a dark bar above 1x) and wrong depth there, which camera motion blur smeared in
        // from the left while turning (at 720p too).
        static const bool noHalfPixel = getenv("DR_NO_HALF_PIXEL") != nullptr; // debugging
        static const bool geometryHalfPixel = getenv("DR_GEOMETRY_HALF_PIXEL") != nullptr; // debugging: the old way (1x)
        if ((Register(device, 0x2302) & 1) == 0 && cmd.viewport.width > 0.0f && cmd.viewport.height > 0.0f && !noHalfPixel)
        {
            if (geometryHalfPixel && shared.resolutionScale == 1.0f)
            {
                shared.halfPixelOffset[0] = 1.0f / cmd.viewport.width;
                shared.halfPixelOffset[1] = -1.0f / cmd.viewport.height;
            }
            else
                shared.inputShift = shared.resolutionScale * 0.5f;
        }
        cmd.sharedConstants = RecordConstants(frame, g_sharedCache, &shared, sizeof(shared), false);
        if (g_guestFrame == drawLogFrame)
            LOGFN("[renderer]   descriptors t2D {} {} {} {} | s16 t2D {} sampler {} | samplers {} {} {} {}", shared.texture2D[0], shared.texture2D[1],
                shared.texture2D[2], shared.texture2D[3], shared.texture2D[16], shared.sampler[16], shared.sampler[0], shared.sampler[1], shared.sampler[2], shared.sampler[3]);
        return true;
    }

    // Records the vertex streams [firstVertex, firstVertex + vertexCount) of the current declaration.
    bool RecordStreams(Frame& frame, uint32_t device, const VertexDeclaration& decl, int64_t firstVertex, uint32_t vertexCount, Command& cmd)
    {
        for (uint32_t n = 0; n < kMaxStreams; n++)
        {
            if ((decl.streamMask & (1u << n)) == 0)
                continue;

            const uint32_t buffer = Load32(device + StreamBuffers + n * 4);
            const uint32_t stride = Load8(device + StreamStrides + n) * 4;
            const uint32_t bufferAddress = buffer != 0 ? VertexBufferAddress(buffer) : 0;
            const uint32_t fetch0 = Load32(device + StreamFetchConstant(n));
            const uint32_t fetch1 = Load32(device + StreamFetchConstant(n) + 4);

            auto fail = [&](const char* reason)
            {
                static std::atomic<uint32_t> logged;
                if (logged++ < 12)
                    LOGFN_WARNING("[renderer] stream {} not usable ({}): buffer {:08X} address {:08X} size {:08X} stride {} fetch {:08X} {:08X} first {} count {}",
                        n, reason, buffer, bufferAddress, buffer != 0 ? VertexBufferSize(buffer) : 0, stride, fetch0, fetch1, firstVertex, vertexCount);
                return false;
            };

            if (buffer == 0)
                return fail("no buffer");
            if (stride == 0 || stride > 256)
                return fail("stride");

            // SetStreamSource folds the offset into the fetch constant's physical address.
            const uint32_t offset = (fetch0 & ~3u) - PhysicalAddress(bufferAddress);
            if (offset > VertexBufferSize(buffer))
                return fail("offset");

            const int64_t source = int64_t(bufferAddress) + offset + firstVertex * stride;
            if (source < 0x10000 || source + int64_t(vertexCount) * stride > 0xFFFF0000ll)
                return fail("range");

            cmd.key.strides[n] = uint16_t(stride);
            cmd.streamSize[n] = vertexCount * stride;
            cmd.streamMask |= 1u << n;
            const bool hasAux = n < kAuxSlotBase && decl.auxStride[n] != 0;
            if (hasAux)
            {
                cmd.key.strides[kAuxSlotBase + n] = uint16_t(decl.auxStride[n]);
                cmd.streamSize[kAuxSlotBase + n] = vertexCount * decl.auxStride[n];
                cmd.streamMask |= 1u << (kAuxSlotBase + n);
            }

            // The game draws the same meshes several times a frame (shadow map, depth pre-pass,
            // colour pass, cube map faces) and the same static meshes every frame: converted
            // streams are cached by source range and declaration, checked by a hash of the data.
            const uint32_t size = vertexCount * stride;
            const uint8_t* guest = static_cast<const uint8_t*>(g_memory.Translate(uint32_t(source)));
            StreamKey key{ uint32_t(source), size, decl.guest, n };
            auto& cached = g_streamCache[key];
            // A stream unchanged for several frames, drawn again in this frame (shadow map, depth
            // pre-pass, colour pass): not hashed again. Streams that change are hashed every draw.
            const bool again = cached.valid && cached.recordedFrame == g_guestFrame && cached.stableFrames >= 4;
            const uint64_t hash = again ? cached.hash : XXH3_64bits(guest, size);
            if (!again && cached.lastUsed != g_guestFrame)
                cached.stableFrames = cached.valid && cached.hash == hash ? cached.stableFrames + 1 : 0;
            cached.lastUsed = g_guestFrame;
            const uint32_t slotBits = (1u << n) | (hasAux ? 1u << (kAuxSlotBase + n) : 0u);
            if (cached.hash == hash && cached.recordedFrame == g_guestFrame && cached.valid)
            {
                cmd.streamOffset[n] = cached.offset;
                if (hasAux)
                    cmd.streamOffset[kAuxSlotBase + n] = cached.auxOffset;
                if (cached.offsetResident)
                    cmd.residentStreams |= slotBits;
            }
            else if (cached.hash == hash && cached.valid && cached.resident != kNotResident)
            {
                cmd.streamOffset[n] = cached.resident;
                if (hasAux)
                    cmd.streamOffset[kAuxSlotBase + n] = cached.residentAux;
                cmd.residentStreams |= slotBits;
            }
            else if (cached.hash == hash && cached.valid && !cached.converted.empty())
            {
                // Unchanged for a while: into the geometry buffer, once (the CPU copy is dropped).
                if (cached.stableFrames >= kResidentAfter)
                {
                    cached.resident = UploadResident(frame, cached.converted.data(), uint32_t(cached.converted.size()));
                    cached.residentSize = uint32_t(cached.converted.size());
                    if (cached.resident != kNotResident && hasAux)
                    {
                        cached.residentAux = UploadResident(frame, cached.aux.data(), uint32_t(cached.aux.size()));
                        cached.residentAuxSize = uint32_t(cached.aux.size());
                        if (cached.residentAux == kNotResident)
                        {
                            g_geometryHeap.Free(cached.resident, cached.residentSize);
                            cached.resident = kNotResident;
                        }
                    }
                }
                if (cached.resident != kNotResident)
                {
                    cmd.streamOffset[n] = cached.resident;
                    if (hasAux)
                        cmd.streamOffset[kAuxSlotBase + n] = cached.residentAux;
                    cmd.residentStreams |= slotBits;
                    cached.converted.clear();
                    cached.converted.shrink_to_fit();
                    cached.aux.clear();
                    cached.aux.shrink_to_fit();
                }
                else
                {
                    cmd.streamOffset[n] = frame.Allocate(cached.converted.size(), 16);
                    memcpy(frame.data.data() + cmd.streamOffset[n], cached.converted.data(), cached.converted.size());
                    if (hasAux)
                    {
                        cmd.streamOffset[kAuxSlotBase + n] = frame.Allocate(cached.aux.size(), 16);
                        memcpy(frame.data.data() + cmd.streamOffset[kAuxSlotBase + n], cached.aux.data(), cached.aux.size());
                    }
                }
            }
            else
            {
                // New or changed data: its geometry buffer regions (if any) go.
                if (cached.resident != kNotResident)
                {
                    g_geometryHeap.Free(cached.resident, cached.residentSize);
                    g_geometryHeap.Free(cached.residentAux, cached.residentAuxSize);
                    cached.resident = cached.residentAux = kNotResident;
                }
                cmd.streamOffset[n] = CopyVertices(frame, decl, n, uint32_t(source), stride, vertexCount);
                if (hasAux)
                    cmd.streamOffset[kAuxSlotBase + n] = CopyAux(frame, decl, n, cmd.streamOffset[n], stride, vertexCount);
                // Streams that change every frame (dynamic buffers) are not kept.
                cached.misses = cached.hash == hash ? 0 : cached.misses + 1;
                if (cached.misses < 3)
                {
                    const uint8_t* converted = frame.data.data() + cmd.streamOffset[n];
                    cached.converted.assign(converted, converted + size);
                    if (hasAux)
                    {
                        const uint8_t* aux = frame.data.data() + cmd.streamOffset[kAuxSlotBase + n];
                        cached.aux.assign(aux, aux + vertexCount * decl.auxStride[n]);
                    }
                }
                else
                {
                    cached.converted.clear();
                    cached.converted.shrink_to_fit();
                    cached.aux.clear();
                    cached.aux.shrink_to_fit();
                }
                cached.hash = hash;
                cached.valid = true;
            }
            cached.recordedFrame = g_guestFrame;
            cached.offset = cmd.streamOffset[n];
            cached.offsetResident = (cmd.residentStreams & (1u << n)) != 0;
            if (hasAux)
                cached.auxOffset = cmd.streamOffset[kAuxSlotBase + n];

            // Debugging (DR_DRAW_LOG): the first vertex of each stream, decoded (frame data only).
            const uint32_t drawLogFrame = g_captureFrame.load(std::memory_order_relaxed);
            if (g_guestFrame == drawLogFrame && (cmd.residentStreams & (1u << n)) == 0)
                for (uint32_t vertex = 0; vertex < std::min(vertexCount, 6u); vertex++)
                {
                    std::string text;
                    for (auto& e : decl.elements)
                        if (e.stream == n && e.offset + e.info.size <= stride)
                        {
                            float v[4];
                            DecodeElement(e.type, frame.data.data() + cmd.streamOffset[n] + vertex * stride + e.offset, v);
                            text += fmt::format(" {}{}=({:.3f} {:.3f} {:.3f} {:.3f})", kUsageNames[e.usage], e.usageIndex, v[0], v[1], v[2], v[3]);
                        }
                    LOGFN("[renderer]   stream {} stride {} count {} v{}:{}", n, stride, vertexCount, vertex, text);
                }
        }
        return true;
    }

    // Converts a quad list or triangle fan into a triangle list.
    void ExpandIndices(uint32_t primitive, std::vector<uint32_t>& indices)
    {
        static thread_local std::vector<uint32_t> out;
        out.clear();
        if (primitive == 13) // quad list
        {
            for (size_t i = 0; i + 3 < indices.size(); i += 4)
                out.insert(out.end(), { indices[i], indices[i + 1], indices[i + 2], indices[i], indices[i + 2], indices[i + 3] });
        }
        else if (primitive == 5) // triangle fan
        {
            for (size_t i = 1; i + 1 < indices.size(); i++)
                out.insert(out.end(), { indices[0], indices[i], indices[i + 1] });
        }
        else
        {
            return;
        }
        indices.swap(out);
    }

    uint32_t RecordIndices(Frame& frame, const std::vector<uint32_t>& indices, uint32_t rebase)
    {
        const uint32_t offset = frame.Allocate(indices.size() * 4, 16);
        auto* dst = reinterpret_cast<uint32_t*>(frame.data.data() + offset);
        for (size_t i = 0; i < indices.size(); i++)
            dst[i] = indices[i] - rebase;
        return offset;
    }
}

// ---- Guest thread ---------------------------------------------------------------------------

void Renderer::OnCreateVertexDeclaration(uint32_t elements, uint32_t declaration)
{
    if (declaration == 0)
        return;

    auto decl = std::make_unique<VertexDeclaration>();
    decl->guest = declaration;

    std::string description;
    bool used[std::size(kLocations)]{};

    for (uint32_t e = elements; ; e += 12)
    {
        const uint32_t streamOffset = Load32(e);
        const uint32_t type = Load32(e + 4);
        const uint32_t stream = streamOffset >> 16;
        if (stream == 0xFF || type == D3DDECLTYPE_UNUSED || decl->elements.size() >= 32)
            break;

        DeclElement element{};
        element.stream = stream;
        element.offset = streamOffset & 0xFFFF;
        element.type = type;
        element.usage = Load8(e + 9);
        element.usageIndex = Load8(e + 10);
        element.info = ConvertDeclType(type);
        description += fmt::format(" [s{} +{} {:06X} {}{}]", stream, element.offset, type,
            element.usage < std::size(kUsageNames) ? kUsageNames[element.usage] : "?", element.usageIndex);

        if (element.info.format == RenderFormat::UNKNOWN || stream >= kMaxStreams)
        {
            LOGFN_WARNING("[renderer] declaration {:08X}: unsupported element type {:06X} skipped", declaration, type);
            continue;
        }

        uint32_t location = UINT32_MAX;
        for (size_t i = 0; i < std::size(kLocations); i++)
            if (kLocations[i].usage == element.usage && kLocations[i].usageIndex == element.usageIndex)
            {
                location = kLocations[i].location;
                used[i] = true;
            }
        if (location == UINT32_MAX)
            continue; // not an input of the translated shaders

        RenderFormat format = element.info.format;
        // uint4 inputs read the raw bits (XenosRecomp: asfloat for FLOAT3 normals, as Unleashed does).
        if (IsUintUsage(element.usage) && element.info.kind == Kind::Float)
            format = element.info.size == 12 ? RenderFormat::R32G32B32_UINT : element.info.size == 16 ? RenderFormat::R32G32B32A32_UINT : RenderFormat::R32_UINT;

        // Formats the host cannot read as the shader expects: float4 in the auxiliary stream.
        const uint32_t guestFormat = type & 0x3F;
        const bool packed = guestFormat == 7 || guestFormat == 16 || guestFormat == 17;
        const bool rawFloatInput = IsUintUsage(element.usage) && element.usage != USAGE_BLENDINDICES && element.info.kind != Kind::Float;
        if ((packed || rawFloatInput) && stream < kAuxSlotBase)
        {
            element.converted = true;
            element.auxOffset = decl->auxStride[stream];
            decl->auxStride[stream] += 16;
            decl->auxMask |= 1u << (kAuxSlotBase + stream);
            decl->inputElements.emplace_back(kUsageNames[element.usage], element.usageIndex, location,
                IsUintUsage(element.usage) ? RenderFormat::R32G32B32A32_UINT : RenderFormat::R32G32B32A32_FLOAT,
                kAuxSlotBase + stream, element.auxOffset);
            description += "(converted)";
        }
        else
            decl->inputElements.emplace_back(kUsageNames[element.usage], element.usageIndex, location, format, stream, element.offset);
        if (element.usage == USAGE_POSITION && element.usageIndex == 0)
            decl->positionElement = int(decl->elements.size());
        decl->elements.push_back(element);
        decl->streamMask |= 1u << stream;
    }

    // Inputs the declaration does not provide read zeros from the dummy slot.
    for (size_t i = 0; i < std::size(kLocations); i++)
        if (!used[i])
            decl->inputElements.emplace_back(kUsageNames[kLocations[i].usage], kLocations[i].usageIndex, kLocations[i].location,
                IsUintUsage(kLocations[i].usage) ? RenderFormat::R32G32B32A32_UINT : RenderFormat::R32G32B32A32_FLOAT, kDummySlot, 0);

    std::lock_guard lock(g_declarationMutex);
    auto& slot = g_declarations[declaration];
    if (slot != nullptr)
        return; // D3D hands out cached declarations; recorded frames may still point at this one
    LOGFN("[renderer] vertex declaration {:08X}:{}", declaration, description);
    slot = std::move(decl);
}

namespace
{
    void RestartsToList(uint32_t primitive, std::vector<uint32_t>& indices); // below
}

void Renderer::OnDrawIndexed(uint32_t device, uint32_t primitive, int32_t baseVertex, uint32_t startIndex, uint32_t indexCount)
{
    HookTimer hookTimer;
    static thread_local bool t_watched = false; // "[hitch profile]" samples the thread that draws too
    if (!t_watched)
    {
        t_watched = true;
        CpuSampler::WatchHitchThread("drawing thread");
    }
    if (indexCount == 0 || indexCount > (1u << 22))
        return;

    Command cmd;
    const VertexDeclaration* decl = nullptr;
    {
        StageTimer timer(StagePrepare);
        if (!PrepareDraw(g_recording, device, primitive, cmd, decl))
            return;
    }
    if (primitive == 8) // indexed rectangle lists are not expected
    {
        g_stats.unsupportedPrimitive++;
        return;
    }

    std::optional<StageTimer> indexTimer(std::in_place, StageIndices);
    const uint32_t indexBuffer = Load32(device + IndexBuffer);
    if (indexBuffer == 0)
    {
        g_stats.noBuffers++;
        return;
    }

    const bool is32 = IndexBufferIs32Bit(indexBuffer);
    const uint32_t address = IndexBufferAddress(indexBuffer) + startIndex * (is32 ? 4 : 2);
    const uint32_t restart = is32 ? 0xFFFFFFFF : 0xFFFF;

    // Processed index lists are cached like vertex streams (by range and primitive, checked by a
    // hash of the guest indices): most meshes are drawn several times a frame and every frame.
    const uint8_t* rawIndices = static_cast<const uint8_t*>(g_memory.Translate(address));
    auto& entry = g_indexCache[IndexKey{ address, indexCount, primitive | (is32 ? 0x100u : 0u) }];
    // As for vertex streams: an index list unchanged for several frames is hashed once a frame.
    const bool again = entry.valid && entry.lastUsed == g_guestFrame && entry.stableFrames >= 4;
    const uint64_t rawHash = again ? entry.hash : XXH3_64bits(rawIndices, size_t(indexCount) * (is32 ? 4 : 2));
    if (!again && entry.lastUsed != g_guestFrame)
        entry.stableFrames = entry.valid && entry.hash == rawHash ? entry.stableFrames + 1 : 0;
    entry.lastUsed = g_guestFrame;
    if (!entry.valid || entry.hash != rawHash)
    {
        g_geometryHeap.Free(entry.resident, entry.residentSize); // changed: its geometry buffer region goes
        entry.resident = kNotResident;
        entry.valid = true;
        entry.hash = rawHash;
        entry.recordedFrame = UINT32_MAX;
        entry.skip = false;

        static thread_local std::vector<uint32_t> indices;
        indices.resize(indexCount);
        uint32_t minIndex = UINT32_MAX, maxIndex = 0;
        for (uint32_t i = 0; i < indexCount; i++)
        {
            uint32_t index = is32 ? __builtin_bswap32(reinterpret_cast<const uint32_t*>(rawIndices)[i])
                                  : __builtin_bswap16(reinterpret_cast<const uint16_t*>(rawIndices)[i]);
            if (index == restart)
                index = UINT32_MAX; // primitive restart
            else
            {
                minIndex = std::min(minIndex, index);
                maxIndex = std::max(maxIndex, index);
            }
            indices[i] = index;
        }
        if (minIndex == UINT32_MAX || maxIndex - minIndex >= (1u << 20))
            entry.skip = true;
        else
        {
            // The reset index (0xFFFF / 0xFFFFFFFF) starts a new strip: Dead Rising draws its
            // world as triangle strips joined by resets. Strips become triangle lists (winding
            // kept per strip).
            if (primitive == 6)
            {
                static thread_local std::vector<uint32_t> list;
                list.clear();
                size_t start = 0;
                for (size_t i = 0; i <= indices.size(); i++)
                {
                    if (i < indices.size() && indices[i] != UINT32_MAX)
                        continue;
                    for (size_t k = start; k + 2 < i; k++)
                    {
                        const uint32_t a = indices[k], b = indices[k + 1], c = indices[k + 2];
                        if ((k - start) & 1)
                            list.insert(list.end(), { b, a, c });
                        else
                            list.insert(list.end(), { a, b, c });
                    }
                    start = i + 1;
                }
                indices.swap(list);
            }
            else if (primitive == 3 || primitive == 5)
                RestartsToList(primitive, indices); // line strips and fans restart at resets too
            else
            {
                for (auto& index : indices)
                    if (index == UINT32_MAX)
                        index = minIndex; // reset in a list: degenerate instead
            }
            if (primitive == 13)
                ExpandIndices(primitive, indices);
            entry.minIndex = minIndex;
            entry.maxIndex = maxIndex;
            entry.rebased.resize(indices.size());
            for (size_t i = 0; i < indices.size(); i++)
                entry.rebased[i] = indices[i] - minIndex;
            entry.skip = entry.rebased.empty();
        }
    }
    if (entry.skip)
    {
        g_stats.tooLarge++;
        return;
    }
    if (primitive == 6)
        cmd.key.topology = RenderPrimitiveTopology::TRIANGLE_LIST;
    else if (primitive == 3)
        cmd.key.topology = RenderPrimitiveTopology::LINE_LIST;
    indexTimer.reset();

    {
        StageTimer timer(StageStreams);
        if (!RecordStreams(g_recording, device, *decl, int64_t(baseVertex) + entry.minIndex, entry.maxIndex - entry.minIndex + 1, cmd))
        {
            g_stats.noBuffers++;
            return;
        }
    }

    StageTimer uploadTimer(StageIndexUpload);
    cmd.indexed = true;
    if (entry.recordedFrame == g_guestFrame)
    {
        cmd.indexOffset = entry.offset;
        cmd.residentIndices = entry.offsetResident;
    }
    else
    {
        // Unchanged for a while: into the geometry buffer, once (RecordStreams).
        if (entry.resident == kNotResident && entry.stableFrames >= kResidentAfter)
        {
            entry.resident = UploadResident(g_recording, entry.rebased.data(), uint32_t(entry.rebased.size() * 4));
            entry.residentSize = uint32_t(entry.rebased.size() * 4);
        }
        if (entry.resident != kNotResident)
        {
            cmd.indexOffset = entry.resident;
            cmd.residentIndices = true;
        }
        else
        {
            cmd.indexOffset = g_recording.Allocate(entry.rebased.size() * 4, 16);
            memcpy(g_recording.data.data() + cmd.indexOffset, entry.rebased.data(), entry.rebased.size() * 4);
        }
        entry.recordedFrame = g_guestFrame;
        entry.offset = cmd.indexOffset;
        entry.offsetResident = cmd.residentIndices;
    }
    cmd.indexSize = uint32_t(entry.rebased.size() * 4);
    cmd.count = uint32_t(entry.rebased.size());

    // Debugging (DR_DRAW_LOG): line draws with their indices and positions (host order).
    {
        const uint32_t drawLogFrame = g_captureFrame.load(std::memory_order_relaxed);
        if (g_guestFrame == drawLogFrame && (primitive == 2 || primitive == 3) && decl->positionElement >= 0 &&
            decl->elements[decl->positionElement].info.kind == Kind::Float &&
            (cmd.residentStreams & (1u << decl->elements[decl->positionElement].stream)) == 0 && !cmd.residentIndices)
        {
            const auto& position = decl->elements[decl->positionElement];
            std::string text;
            for (uint32_t i = 0; i < std::min<uint32_t>(indexCount, 16); i++)
            {
                const uint32_t index = is32 ? __builtin_bswap32(reinterpret_cast<const uint32_t*>(rawIndices)[i])
                                            : __builtin_bswap16(reinterpret_cast<const uint16_t*>(rawIndices)[i]);
                text += fmt::format(" {}", index == restart ? std::string("R") : std::to_string(index));
            }
            text += " |";
            for (uint32_t r = 0; r < std::min<uint32_t>(uint32_t(entry.rebased.size()), 16); r++)
            {
                const uint32_t v = entry.rebased[r];
                const float* p = reinterpret_cast<const float*>(g_recording.data.data() + cmd.streamOffset[position.stream] +
                    size_t(v) * cmd.key.strides[position.stream] + position.offset);
                text += fmt::format(" ({:.1f} {:.1f})", p[0], p[1]);
            }
            LOGFN("[renderer] lines #{}: primitive {} indices {} base {} min {}:{}", g_recording.commands.size(), primitive, indexCount, baseVertex, entry.minIndex, text);
        }
    }

    g_recording.commands.push_back(cmd);
    g_stats.recorded++;
}

void Renderer::OnBeginVertices(uint32_t device, uint32_t primitive, uint32_t vertexCount, uint32_t stride, uint32_t data)
{
    g_pendingVertices = {};
    g_pendingVertices.active = data != 0;
    g_pendingVertices.primitive = primitive;
    g_pendingVertices.count = vertexCount;
    g_pendingVertices.stride = stride;
    g_pendingVertices.data = data;
}

void Renderer::OnBeginIndexedVertices(uint32_t device, uint32_t primitive, int32_t baseVertex, uint32_t vertexCount, uint32_t indexCount,
    bool indices32, uint32_t stride, uint32_t vertexData, uint32_t indexData)
{
    g_pendingVertices = {};
    g_pendingVertices.active = vertexData != 0 && indexData != 0;
    g_pendingVertices.primitive = primitive;
    g_pendingVertices.count = vertexCount;
    g_pendingVertices.stride = stride;
    g_pendingVertices.data = vertexData;
    g_pendingVertices.indexed = true;
    g_pendingVertices.indices32 = indices32;
    g_pendingVertices.baseVertex = baseVertex;
    g_pendingVertices.indexCount = indexCount;
    g_pendingVertices.indexData = indexData;
}

namespace
{
    // Line strips (3) and triangle fans (5) with reset indices (UINT32_MAX after reading) -> line
    // and triangle lists, restarting at each reset. Substituting an index for the reset instead
    // joins the strips: the tutorial pop-ups' white frame (an indexed line strip with a reset)
    // got a diagonal across the box.
    void RestartsToList(uint32_t primitive, std::vector<uint32_t>& indices)
    {
        static thread_local std::vector<uint32_t> list;
        list.clear();
        size_t start = 0;
        for (size_t i = 0; i <= indices.size(); i++)
        {
            if (i < indices.size() && indices[i] != UINT32_MAX)
                continue;
            if (primitive == 3)
                for (size_t k = start; k + 1 < i; k++)
                    list.insert(list.end(), { indices[k], indices[k + 1] });
            else
                for (size_t k = start + 1; k + 1 < i; k++)
                    list.insert(list.end(), { indices[start], indices[k], indices[k + 1] });
            start = i + 1;
        }
        indices.swap(list);
    }

    // Triangle strips with reset indices (UINT32_MAX after reading) -> triangle list.
    void StripToList(std::vector<uint32_t>& indices)
    {
        static thread_local std::vector<uint32_t> list;
        list.clear();
        size_t start = 0;
        for (size_t i = 0; i <= indices.size(); i++)
        {
            if (i < indices.size() && indices[i] != UINT32_MAX)
                continue;
            for (size_t k = start; k + 2 < i; k++)
            {
                const uint32_t a = indices[k], b = indices[k + 1], c = indices[k + 2];
                if ((k - start) & 1)
                    list.insert(list.end(), { b, a, c });
                else
                    list.insert(list.end(), { a, b, c });
            }
            start = i + 1;
        }
        indices.swap(list);
    }

    // EndVertices of a BeginIndexedVertices batch: vertices and indices the game wrote inline.
    void EndIndexedVertices(uint32_t device, const PendingVertices& pending)
    {
        if (pending.indexCount == 0 || pending.indexCount > (1u << 20))
            return;

        Command cmd;
        const VertexDeclaration* decl = nullptr;
        if (!PrepareDraw(g_recording, device, pending.primitive, cmd, decl))
            return;
        if (decl->streamMask != 1 || pending.primitive == 8)
        {
            g_stats.noBuffers++;
            return;
        }

        static thread_local std::vector<uint32_t> indices;
        indices.resize(pending.indexCount);
        const uint32_t restart = pending.indices32 ? 0xFFFFFFFF : 0xFFFF;
        for (uint32_t i = 0; i < pending.indexCount; i++)
        {
            uint32_t index = pending.indices32 ? Load32(pending.indexData + i * 4)
                : __builtin_bswap16(*static_cast<const uint16_t*>(g_memory.Translate(pending.indexData + i * 2)));
            if (index == restart)
                index = UINT32_MAX;
            else
            {
                const int64_t vertex = int64_t(index) + pending.baseVertex;
                index = (vertex >= 0 && vertex < int64_t(pending.count)) ? uint32_t(vertex) : 0;
            }
            indices[i] = index;
        }
        // Debugging (DR_DRAW_LOG): line draws, their indices (R = reset) and guest positions.
        {
            const uint32_t drawLogFrame = g_captureFrame.load(std::memory_order_relaxed);
            if (g_guestFrame == drawLogFrame && (pending.primitive == 2 || pending.primitive == 3) && decl->positionElement >= 0)
            {
                const uint32_t po = decl->elements[decl->positionElement].offset;
                std::string text;
                for (uint32_t i = 0; i < std::min<uint32_t>(uint32_t(indices.size()), 16); i++)
                    text += indices[i] == UINT32_MAX ? std::string(" R")
                        : fmt::format(" {}({:.0f} {:.0f})", indices[i], LoadFloat(pending.data + indices[i] * pending.stride + po),
                            LoadFloat(pending.data + indices[i] * pending.stride + po + 4));
                LOGFN("[renderer] up lines #{}: primitive {} indices {} vertices {} base {}:{}", g_recording.commands.size(), pending.primitive,
                    pending.indexCount, pending.count, pending.baseVertex, text);
            }
        }
        if (pending.primitive == 6)
        {
            StripToList(indices);
            cmd.key.topology = RenderPrimitiveTopology::TRIANGLE_LIST;
        }
        else if (pending.primitive == 3 || pending.primitive == 5)
        {
            RestartsToList(pending.primitive, indices);
            cmd.key.topology = pending.primitive == 3 ? RenderPrimitiveTopology::LINE_LIST : RenderPrimitiveTopology::TRIANGLE_LIST;
        }
        else
            for (auto& index : indices)
                if (index == UINT32_MAX)
                    index = 0;
        if (pending.primitive == 13)
            ExpandIndices(pending.primitive, indices);
        if (indices.empty())
            return;

        const uint32_t offset = CopyVertices(g_recording, *decl, 0, pending.data, pending.stride, pending.count);
        cmd.key.strides[0] = uint16_t(pending.stride);
        cmd.streamOffset[0] = offset;
        cmd.streamSize[0] = pending.count * pending.stride;
        cmd.streamMask = 1;
        RecordAux(g_recording, *decl, 0, offset, pending.stride, pending.count, cmd);

        cmd.indexed = true;
        cmd.indexOffset = RecordIndices(g_recording, indices, 0);
        cmd.indexSize = uint32_t(indices.size() * 4);
        cmd.count = uint32_t(indices.size());
        g_recording.commands.push_back(cmd);
        g_stats.recorded++;
        g_stats.indexedVertices++;
    }
}

void Renderer::OnEndVertices(uint32_t device)
{
    HookTimer hookTimer;
    PendingVertices pending = g_pendingVertices;
    g_pendingVertices.active = false;
    if (!pending.active || pending.count == 0 || pending.count > 65536 || pending.stride == 0 || pending.stride > 256)
        return;
    if (pending.indexed)
    {
        EndIndexedVertices(device, pending);
        return;
    }

    Command cmd;
    const VertexDeclaration* decl = nullptr;
    if (!PrepareDraw(g_recording, device, pending.primitive, cmd, decl))
        return;
    if (decl->streamMask != 1)
    {
        g_stats.noBuffers++;
        return;
    }

    uint32_t count = pending.count;
    uint32_t offset = CopyVertices(g_recording, *decl, 0, pending.data, pending.stride, count);

    // Debugging (DR_DRAW_LOG): primitive and the first positions of BeginVertices draws (UI).
    {
        const uint32_t drawLogFrame = g_captureFrame.load(std::memory_order_relaxed);
        if (g_guestFrame == drawLogFrame && decl->positionElement >= 0 && decl->elements[decl->positionElement].info.kind == Kind::Float)
        {
            const uint32_t po = decl->elements[decl->positionElement].offset;
            std::string positions;
            for (uint32_t i = 0; i < std::min(count, 12u); i++)
            {
                const float* p = reinterpret_cast<const float*>(g_recording.data.data() + offset + i * pending.stride + po);
                positions += fmt::format(" ({:.2f} {:.2f})", p[0], p[1]);
            }
            LOGFN("[renderer] vertices #{}: primitive {} count {} stride {}{}", g_recording.commands.size(), pending.primitive, count, pending.stride, positions);
        }
    }

    if (pending.primitive == 8)
    {
        // Rectangle list: three corners per rectangle; the fourth is a + b - corner, where the
        // corner is the vertex with the right angle (judged on the input positions, fine for the
        // screen-space quads the game draws this way).
        const uint32_t rects = count / 3;
        const uint32_t expanded = g_recording.Allocate(rects * 6 * pending.stride, 16);
        const uint8_t* src = g_recording.data.data() + offset;
        uint8_t* dst = g_recording.data.data() + expanded;

        for (uint32_t r = 0; r < rects; r++)
        {
            const uint8_t* v[3] = { src + (r * 3) * pending.stride, src + (r * 3 + 1) * pending.stride, src + (r * 3 + 2) * pending.stride };
            int corner = 0;
            if (decl->positionElement >= 0 && decl->elements[decl->positionElement].info.kind == Kind::Float)
            {
                const uint32_t po = decl->elements[decl->positionElement].offset;
                auto pos = [&](int i, int c) { return reinterpret_cast<const float*>(v[i] + po)[c]; };
                float best = FLT_MAX;
                for (int k = 0; k < 3; k++)
                {
                    int a = (k + 1) % 3, b = (k + 2) % 3;
                    float dot = (pos(a, 0) - pos(k, 0)) * (pos(b, 0) - pos(k, 0)) + (pos(a, 1) - pos(k, 1)) * (pos(b, 1) - pos(k, 1));
                    if (fabsf(dot) < best) { best = fabsf(dot); corner = k; }
                }
            }
            const int a = (corner + 1) % 3, b = (corner + 2) % 3;

            uint8_t fourth[256];
            memcpy(fourth, v[a], pending.stride);
            for (auto& e : decl->elements)
            {
                if (e.offset + e.info.size > pending.stride)
                    continue;
                if (e.info.kind == Kind::Float)
                    for (uint32_t c = 0; c < e.info.size / 4u; c++)
                    {
                        auto f = [&](const uint8_t* p) { return reinterpret_cast<const float*>(p + e.offset)[c]; };
                        reinterpret_cast<float*>(fourth + e.offset)[c] = f(v[a]) + f(v[b]) - f(v[corner]);
                    }
                else if (e.info.kind == Kind::Byte4)
                    for (uint32_t c = 0; c < 4; c++)
                        fourth[e.offset + c] = uint8_t(std::clamp(int(v[a][e.offset + c]) + v[b][e.offset + c] - v[corner][e.offset + c], 0, 255));
            }

            const uint8_t* order[6] = { v[corner], v[a], v[b], v[a], fourth, v[b] };
            for (int i = 0; i < 6; i++)
                memcpy(dst + (r * 6 + i) * pending.stride, order[i], pending.stride);
        }
        offset = expanded;
        count = rects * 6;
    }

    cmd.key.strides[0] = uint16_t(pending.stride);
    cmd.streamOffset[0] = offset;
    cmd.streamSize[0] = count * pending.stride;
    cmd.streamMask = 1;
    RecordAux(g_recording, *decl, 0, offset, pending.stride, count, cmd);

    if (pending.primitive == 5 || pending.primitive == 13)
    {
        static thread_local std::vector<uint32_t> indices;
        indices.resize(count);
        for (uint32_t i = 0; i < count; i++)
            indices[i] = i;
        ExpandIndices(pending.primitive, indices);
        cmd.indexed = true;
        cmd.indexOffset = RecordIndices(g_recording, indices, 0);
        cmd.indexSize = uint32_t(indices.size() * 4);
        cmd.count = uint32_t(indices.size());
    }
    else
    {
        cmd.count = count;
    }

    g_recording.commands.push_back(cmd);
    g_stats.recorded++;
}

namespace
{
    // Clears and resolves through a surface larger than EDRAM could hold (before its auto-exposure
    // passes, 64x64 and 1x1, the game clears through a surface object that reports 16384x32768, with
    // the viewport to match) do not size the host buffers: they cover what the draws made. They
    // made 4096x4096 colour and depth buffers (8192x8192 at 2x resolution scale) out of nothing.
    void LimitImpossibleSurface(uint32_t device, Command& cmd, uint32_t sizeSurface)
    {
        if (uint64_t(SurfaceWidth(sizeSurface)) * SurfaceHeight(sizeSurface) <= 2048ull * 2048ull)
            return;
        static uint32_t logged;
        if (logged++ < 4)
            LOGFN("[renderer] {} through a {}x{} surface (viewport {}x{}, color info {:08X}): host buffers keep their size",
                cmd.type == Command::Clear ? "clear" : "resolve", SurfaceWidth(sizeSurface), SurfaceHeight(sizeSurface),
                Load32(device + Viewport + 8), Load32(device + Viewport + 12), Register(device, RB_COLOR_INFO));
        cmd.width = cmd.height = 1;
    }
}

void Renderer::OnClear(uint32_t device, uint32_t flags, uint32_t color, float z, uint32_t stencil)
{
    Command cmd;
    memset(static_cast<void*>(&cmd), 0, sizeof(cmd));
    cmd.type = Command::Clear;
    cmd.colorSurface = Load32(device + RenderTarget0);
    cmd.depthSurface = Load32(device + DepthStencil);
    cmd.colorKey = cmd.colorSurface != 0 ? (Register(device, RB_COLOR_INFO) & 0xFFF) + 1 : 0;
    cmd.depthKey = cmd.depthSurface != 0 ? (Register(device, RB_DEPTH_INFO) & 0xFFF) + 1 : 0;
    const uint32_t sizeSurface = cmd.colorSurface != 0 ? cmd.colorSurface : cmd.depthSurface;
    if (sizeSurface == 0)
        return;
    cmd.colorFormat = cmd.colorSurface != 0 ? ColorFormatOf(Register(device, RB_COLOR_INFO)) : RenderFormat::UNKNOWN;
    cmd.samples = SurfaceSamples(device);
    cmd.width = std::min(std::max(SurfaceWidth(sizeSurface), Load32(device + Viewport) + Load32(device + Viewport + 8)), 4096u);
    cmd.height = std::min(std::max(SurfaceHeight(sizeSurface), Load32(device + Viewport + 4) + Load32(device + Viewport + 12)), 4096u);
    LimitImpossibleSurface(device, cmd, sizeSurface);
    cmd.clearFlags = flags;
    cmd.clearColor = RenderColor(((color >> 16) & 0xFF) / 255.0f, ((color >> 8) & 0xFF) / 255.0f, (color & 0xFF) / 255.0f, (color >> 24) / 255.0f);
    cmd.clearZ = z;
    if (cmd.depthKey != 0 && g_depthFlipped[cmd.depthKey])
        cmd.clearZ = 1.0f - z;
    cmd.clearStencil = stencil;
    static uint32_t loggedClears;
    const uint32_t drawLogFrame = g_captureFrame.load(std::memory_order_relaxed);
    if (loggedClears++ < 6 || g_guestFrame == drawLogFrame)
        LOGFN("[renderer] clear #{} flags {:X} color {:08X} z {} stencil {} colorinfo {:08X} depthinfo {:08X} surface info {:08X}", g_recording.commands.size(), flags, color, z, stencil,
            Register(device, RB_COLOR_INFO), Register(device, RB_DEPTH_INFO), Register(device, RB_SURFACE_INFO));
    g_recording.commands.push_back(cmd);
    g_pendingClears.push_back(g_recording.commands.size() - 1);
}

void Renderer::OnResolve(uint32_t device, uint32_t flags, uint32_t sourceRect, uint32_t destTexture, uint32_t destPoint,
    uint32_t destLevel, uint32_t destSlice, uint32_t clearColor, float clearZ, uint32_t clearStencil)
{
    HookTimer hookTimer;
    Command cmd;
    memset(static_cast<void*>(&cmd), 0, sizeof(cmd));
    cmd.type = Command::Resolve;
    cmd.resolveFlags = flags;

    // Flags: bits 0-2 source (render target 0-3, 4 = depth), 0x100 / 0x200 clear colour / depth
    // afterwards. The host target is keyed by render target 0 as for draws.
    cmd.colorSurface = Load32(device + RenderTarget0);
    cmd.depthSurface = Load32(device + DepthStencil);
    cmd.colorKey = cmd.colorSurface != 0 ? (Register(device, RB_COLOR_INFO) & 0xFFF) + 1 : 0;
    cmd.depthKey = cmd.depthSurface != 0 ? (Register(device, RB_DEPTH_INFO) & 0xFFF) + 1 : 0;
    const uint32_t sizeSurface = cmd.colorSurface != 0 ? cmd.colorSurface : cmd.depthSurface;
    if (sizeSurface == 0)
        return;
    cmd.colorFormat = cmd.colorSurface != 0 ? ColorFormatOf(Register(device, RB_COLOR_INFO)) : RenderFormat::UNKNOWN;
    cmd.samples = SurfaceSamples(device);
    cmd.width = std::min(std::max(SurfaceWidth(sizeSurface), Load32(device + Viewport) + Load32(device + Viewport + 8)), 4096u);
    cmd.height = std::min(std::max(SurfaceHeight(sizeSurface), Load32(device + Viewport + 4) + Load32(device + Viewport + 12)), 4096u);
    LimitImpossibleSurface(device, cmd, sizeSurface);

    cmd.surfaceWidth = SurfaceWidth(sizeSurface);
    cmd.surfaceHeight = SurfaceHeight(sizeSurface);
    if (sourceRect != 0)
        for (int i = 0; i < 4; i++)
            cmd.sourceRect[i] = int32_t(Load32(sourceRect + i * 4));
    else
    {
        cmd.sourceRect[2] = int32_t(SurfaceWidth(sizeSurface));
        cmd.sourceRect[3] = int32_t(SurfaceHeight(sizeSurface));
    }
    if (destPoint != 0)
    {
        cmd.destX = int32_t(Load32(destPoint));
        cmd.destY = int32_t(Load32(destPoint + 4));
    }
    if (destTexture != 0)
    {
        cmd.destAddress = TextureAddress(destTexture);
        cmd.destWidth = TextureWidth(destTexture);
        cmd.destHeight = TextureHeight(destTexture);
        // One host texture per (address, format class, host format of the source): the game
        // resolves different buffers into the same address within a frame. Draws sample the one
        // resolved into last (in recording order), as the console's memory would hold it.
        const uint32_t baseKey = ResolveKey(cmd.destAddress, TextureFormat(destTexture));
        // A colour resolve through the 10:10:10:2 fixed-point view (formats 2, 10) of the HDR buffer copies
        // the raw 7e3 bits on the console: the host encodes them (16-bit unorm texture).
        const uint32_t sourceFormat = (Register(device, RB_COLOR_INFO) >> 16) & 0xF;
        cmd.encode7e3 = (flags & 7) != 4 && (sourceFormat == 2 || sourceFormat == 10) && cmd.colorFormat == RenderFormat::R16G16B16A16_FLOAT;
        const RenderFormat hostFormat = (flags & 7) == 4 ? RenderFormat::R32_FLOAT
            : cmd.encode7e3 ? RenderFormat::R16G16B16A16_UNORM : cmd.colorFormat;
        cmd.destKey = baseKey | ((uint32_t(hostFormat) & 0x3FF) << 2);
        for (int i = 0; i < 6; i++)
            cmd.destFetch[i] = Load32(destTexture + 0x1C + i * 4);
        // Mip level and cube face: environment maps are rendered level by level (128, 64, ... 8)
        // and face by face into one texture, sampled later with mip filtering.
        cmd.destCube = ((cmd.destFetch[5] >> 9) & 3) == 3;
        const uint32_t maxLevel = (cmd.destFetch[4] >> 6) & 0xF;
        uint32_t fullChain = 1;
        while ((std::max(cmd.destWidth, cmd.destHeight) >> fullChain) != 0)
            fullChain++;
        cmd.destLevels = std::clamp(maxLevel + 1, 1u, fullChain);
        cmd.destLevel = std::min(destLevel, cmd.destLevels - 1);
        cmd.destSlice = cmd.destCube ? std::min(destSlice, 5u) : 0;
        cmd.destDescriptor = ResolveDescriptor(cmd.destKey);
        LatestResolve latest;
        latest.descriptor = cmd.destDescriptor;
        latest.width = cmd.destWidth;
        latest.height = cmd.destHeight;
        latest.address = cmd.destAddress;
        latest.frame = g_guestFrame;
        latest.scale = PassScale(cmd.width, cmd.height, g_recording.scale);
        TextureDecode::FetchConstant destFetch;
        memcpy(destFetch.dword, cmd.destFetch, sizeof(destFetch.dword));
        latest.size = std::min(TextureDecode::GuestSize(destFetch), 16u << 20);
        if (latest.size != 0 && cmd.destAddress >= 0x10000 && uint64_t(cmd.destAddress) + latest.size <= 0xFFFF0000ull)
            latest.memory = SampleHash(static_cast<const uint8_t*>(g_memory.Translate(cmd.destAddress)), latest.size);
        else
            latest.size = 0;
        std::lock_guard lock(g_resolveMutex);
        g_latestResolve[baseKey] = latest;
    }
    if (clearColor != 0)
        cmd.clearColor = RenderColor(LoadFloat(clearColor), LoadFloat(clearColor + 4), LoadFloat(clearColor + 8), LoadFloat(clearColor + 12));
    cmd.clearStencil = clearStencil & 0xFF;
    cmd.clearZ = clearZ;
    cmd.resolveDepthFlipped = cmd.depthKey != 0 && g_depthFlipped[cmd.depthKey];
    if (cmd.depthKey != 0 && g_depthFlipped[cmd.depthKey])
        cmd.clearZ = 1.0f - clearZ;

    static uint32_t logged;
    const uint32_t drawLogFrame = g_captureFrame.load(std::memory_order_relaxed);
    // Debugging: DR_WATCH_ADDR=<hex> logs every resolve into that texture address.
    static const uint32_t watchAddress = getenv("DR_WATCH_ADDR") ? uint32_t(strtoul(getenv("DR_WATCH_ADDR"), nullptr, 16)) : 0;
    SettlePendingClears(g_recording.commands, cmd.colorKey, cmd.depthKey, cmd.samples);
    // The first four frames that resolve into the watched address are captured (targets and
    // resolves dumped, as with F12) to see what the game renders into it.
    static uint32_t watchCaptures = 0;
    if (watchAddress != 0 && cmd.destAddress == watchAddress && !g_recording.capture && watchCaptures < 4)
    {
        watchCaptures++;
        g_recording.capture = true;
        g_captureFrame = g_guestFrame; // draw log for the rest of this frame
        LOGFN("[renderer] DR_WATCH_ADDR: capturing guest frame {}", g_guestFrame);
    }
    if ((logged < 60 && g_recording.commands.size() > 200) || g_guestFrame == drawLogFrame || (watchAddress != 0 && cmd.destAddress == watchAddress)) // a frame of the running game, not of loading
    {
        logged++;
        LOGFN("[renderer] frame {} level {} slice {}/{}{}", g_guestFrame, cmd.destLevel, cmd.destSlice, cmd.destLevels, cmd.destCube ? " cube" : "");
        LOGFN("[renderer] resolve #{} clearZ {} clearColor {}/{}/{}/{} flags {:X} colorinfo {:08X} depthinfo {:08X} surface info {:08X} rect ({},{})-({},{}) -> texture {:08X} ({}x{} fmt {}) at ({},{}); target {:08X} {}x{}",
            g_recording.commands.size(), clearZ, cmd.clearColor.r, cmd.clearColor.g, cmd.clearColor.b, cmd.clearColor.a, flags, Register(device, RB_COLOR_INFO), Register(device, RB_DEPTH_INFO), Register(device, RB_SURFACE_INFO), cmd.sourceRect[0], cmd.sourceRect[1], cmd.sourceRect[2], cmd.sourceRect[3],
            cmd.destAddress, cmd.destWidth, cmd.destHeight, destTexture != 0 ? TextureFormat(destTexture) : 0,
            cmd.destX, cmd.destY, cmd.colorSurface, cmd.width, cmd.height);
    }

    // Debugging: DR_FILL_RESOLVE=<address>:<byte> fills the guest memory of resolves into that
    // address (to see whether the CPU reads them, e.g. for auto exposure).
    static const std::pair<uint32_t, int> fill = []
    {
        std::pair<uint32_t, int> value{ 0, -1 };
        if (const char* text = getenv("DR_FILL_RESOLVE"))
        {
            char* end;
            value.first = uint32_t(strtoul(text, &end, 16));
            value.second = *end == ':' ? int(strtoul(end + 1, nullptr, 16)) : 0x40;
        }
        return value;
    }();
    if (fill.second >= 0 && destTexture != 0 && cmd.destAddress == fill.first)
        memset(g_memory.Translate(cmd.destAddress), fill.second, size_t(cmd.destWidth) * cmd.destHeight * 4);

    g_recording.commands.push_back(cmd);
}

void Renderer::AddGpuWait(uint32_t microseconds)
{
    g_timing.gpuWaitUs += microseconds;
}

void Renderer::CheckGuestWrite(const char* who, uint32_t address, uint32_t size)
{
    if (!g_texWatch || size == 0)
        return;
    address &= 0x1FFFFFFF;
    std::lock_guard lock(g_texWatchMutex);
    for (uint32_t page = address >> 12; page <= (address + size - 1) >> 12; page++)
    {
        auto it = g_texWatchPages.find(page);
        if (it == g_texWatchPages.end())
            continue;
        static std::atomic<uint32_t> logged{ 0 };
        if (logged++ < 64)
        {
            const auto watched = g_texWatched.find(it->second);
            if (watched != g_texWatched.end())
                LOGFN_WARNING("[texwatch] {} writes {:08X}+{:X} into texture {:08X} fmt {} {}x{} (mips at {:08X})", who, address, size,
                    watched->second.base, watched->second.format, watched->second.width, watched->second.height, watched->second.mips);
        }
        return;
    }
}

static std::chrono::steady_clock::time_point g_lastHitchCheck = std::chrono::steady_clock::now();

void Renderer::OnPresent(uint32_t device)
{
    CpuSampler::RegisterCurrentThread("presenting thread"); // DR_CPU_PROFILE=1 (the guest thread registered it first)
    static thread_local bool t_watched = false;
    if (!t_watched)
    {
        t_watched = true;
        CpuSampler::WatchHitches(); // "[hitch profile]" for frames with over 50 ms of game work (DR_HITCH_PROFILE=0 off)
    }
    // DR_TEST_STALL=<frame>:<seconds> holds the game thread at that present (tests the status panel
    // and the hang watchdog; frame 1: before the first picture reaches the window).
    static const std::pair<uint32_t, uint32_t> testStall = [] {
        const char* value = getenv("DR_TEST_STALL");
        if (value == nullptr || strchr(value, ':') == nullptr)
            return std::make_pair(0u, 0u);
        return std::make_pair(uint32_t(atoi(value)), uint32_t(atoi(strchr(value, ':') + 1)));
    }();
    if (testStall.second != 0 && g_guestFrame + 1 == testStall.first)
    {
        LOGFN("[test] holding the game for {} s at frame {} (DR_TEST_STALL)", testStall.second, testStall.first);
        std::this_thread::sleep_for(std::chrono::seconds(testStall.second));
    }
    HangWatchdog::OnPresent();
    const auto presentStart = std::chrono::steady_clock::now();
    static auto lastPresent = presentStart;
    g_timing.guestFrameUs += uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(presentStart - lastPresent).count());
    lastPresent = presentStart;
    g_timing.guestFrames++;
    uint32_t decodeWaitUs = 0, presentWorkUs = 0; // for the "[hitch]" line
    {
        HookTimer hookTimer;
        FinishDecodes(g_recording);
        decodeWaitUs = uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - presentStart).count());
        FlushDynamicTextures(g_recording);
        RequestPipelines(g_recording);
        EvictTextures();
        VerifyWatchedTextures();
        g_geometryHeap.Reclaim();
    }
    presentWorkUs = uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - presentStart).count());
    if (g_guestFrame % 150 == 0)
    {
        LOGFN("[renderer] hook stages over 150 frames (ms per frame): prepare {:.1f}, indices {:.1f}, vertex streams {:.1f}, index upload {:.1f}",
            g_stageNs[StagePrepare] / 150e6, g_stageNs[StageIndices] / 150e6, g_stageNs[StageStreams] / 150e6, g_stageNs[StageIndexUpload] / 150e6);
        for (auto& ns : g_stageNs)
            ns = 0;
    }
    {
        // A frame must not be replaced before the video thread took it: it carries one-time
        // texture uploads (and resolves later frames rely on). Wait for it (bounded, so a stalled
        // window cannot hang the game).
        std::unique_lock lock(g_frameMutex);
        const auto waitStart = std::chrono::steady_clock::now();
        // iOS in the background: rendering is paused (gpu/video.cpp), and the game waits with it.
        if (g_rendererActive)
            while (!g_frameTaken.wait_for(lock, std::chrono::seconds(1), [] { return !g_pendingValid; }) && Video::RenderingPaused())
            {
            }
        const uint32_t waitUs = uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - waitStart).count());
        g_timing.guestWaitUs += waitUs;
        // Game time stands still while a frame stalls (fixed step): log the long ones, with where
        // the time went on this thread and on all threads (file reads, texture decodes, hooks).
        const uint32_t frameUs = uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(presentStart - g_lastHitchCheck).count());
        const HitchStats::Counts thread = HitchStats::TakeThread();
        const HitchStats::Counts all = HitchStats::TakeGlobal();
        const auto [grownUs, grownTo] = FrameBuffer::TakeGrowth();
        uint64_t stageNs[StageCount];
        memcpy(stageNs, g_frameStageNs, sizeof(stageNs));
        memset(g_frameStageNs, 0, sizeof(g_frameStageNs));
        if (frameUs + presentWorkUs + waitUs > 50000 && g_guestFrame > 0)
        {
            static uint32_t logged;
            if (logged++ < 200)
                LOGFN("[hitch] frame {}: {} ms of game work, {} ms at present (texture decode wait {} ms), {} ms waiting for the video thread; "
                      "this thread: file reads {} ms ({} reads, {:.1f} MB), texture decodes {} ms, hooks {} ms; all threads: file reads {} ms "
                      "({:.1f} MB), texture decodes {} ms, hooks {} ms",
                    g_guestFrame, frameUs / 1000, presentWorkUs / 1000, decodeWaitUs / 1000, waitUs / 1000, thread.us[HitchStats::FileRead] / 1000,
                    thread.reads, thread.bytesRead / 1048576.0, thread.us[HitchStats::TextureDecode] / 1000, thread.us[HitchStats::Hooks] / 1000,
                    all.us[HitchStats::FileRead] / 1000, all.bytesRead / 1048576.0, all.us[HitchStats::TextureDecode] / 1000,
                    all.us[HitchStats::Hooks] / 1000);
            if (logged <= 200)
                LOGFN("[hitch]   draw hook stages: prepare {:.1f} ms, indices {:.1f} ms, vertex streams {:.1f} ms, index upload {:.1f} ms; "
                      "frame data {:.1f} MB{}", stageNs[StagePrepare] / 1e6, stageNs[StageIndices] / 1e6, stageNs[StageStreams] / 1e6,
                    stageNs[StageIndexUpload] / 1e6, g_recording.data.size() / 1048576.0,
                    grownTo != 0 ? fmt::format(", grown to {:.0f} MB ({:.1f} ms)", grownTo / 1048576.0, grownUs / 1000.0) : std::string());
            // The profile covers the game's work and the present's (not the wait for the video thread).
            if (frameUs + presentWorkUs > 50000 && logged <= 200)
                CpuSampler::ReportHitch(g_guestFrame, g_lastHitchCheck, presentStart + std::chrono::microseconds(presentWorkUs));
        }
        g_lastHitchCheck = std::chrono::steady_clock::now();
        // The video thread did not take the last frame (stalled over a second): it is dropped, and
        // with it the geometry it was to upload. Everything resident goes back to the per-frame path
        // (and becomes resident again later), so no draw reads a region that was never filled.
        if (g_pendingValid && !g_pending.geometryUploads.empty())
        {
            for (auto& [key, entry] : g_streamCache)
                if (entry.resident != kNotResident)
                {
                    g_geometryHeap.Free(entry.resident, entry.residentSize);
                    g_geometryHeap.Free(entry.residentAux, entry.residentAuxSize);
                    entry.resident = entry.residentAux = kNotResident;
                    entry.valid = false;
                    entry.stableFrames = 0;
                }
            for (auto& [key, entry] : g_indexCache)
                if (entry.resident != kNotResident)
                {
                    g_geometryHeap.Free(entry.resident, entry.residentSize);
                    entry.resident = kNotResident;
                    entry.stableFrames = 0;
                }
            LOGN_WARNING("[renderer] a frame with geometry uploads was dropped: resident geometry reset");
        }
        std::swap(g_recording, g_pending);
        g_pendingValid = true;
    }
    g_recording.Clear();
    g_recording.scale = FrameScale();
    g_vsCache.offset = g_psCache.offset = g_sharedCache.offset = UINT32_MAX;
    g_singleSampleSurfaces.clear();
    g_restoredMsaaSurfaces.clear();
    g_pendingClears.clear();
    g_guestFrame++;

#ifdef _WIN32
    // F12: capture the frame after next (see g_captureFrame).
    static bool f12Down = false;
    const bool f12 = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
    if (f12 && !f12Down)
    {
        g_captureFrame = g_guestFrame + 1;
        g_captureTextures = true;
        LOGFN("[renderer] F12: capturing guest frame {} into dump/", g_guestFrame + 1);
    }
    f12Down = f12;
#endif
    g_recording.capture = g_guestFrame == g_captureFrame.load(std::memory_order_relaxed);
}

// ---- Video thread ---------------------------------------------------------------------------

namespace
{
    RenderDevice* g_device = nullptr;
    bool g_vulkan = false;
    RenderFormat g_backBufferFormat = RenderFormat::UNKNOWN;

    std::unique_ptr<RenderPipelineLayout> g_pipelineLayout;
    std::unique_ptr<RenderDescriptorSet> g_textureSet;
    std::unique_ptr<RenderDescriptorSet> g_samplerSet;
    std::unique_ptr<RenderSampler> g_sampler;
    std::unique_ptr<RenderSampler> g_blitSampler;
    std::unique_ptr<RenderTexture> g_placeholders[3];
    std::unique_ptr<RenderTextureView> g_placeholderViews[3];
    std::unique_ptr<RenderFramebuffer> g_whiteFramebuffer;
    bool g_placeholdersReady = false;

    std::unique_ptr<RenderShader> g_blitVertexShader, g_blitPixelShader, g_depthPixelShader, g_overlayPixelShader;
    std::unique_ptr<RenderShader> g_menuPixelShader;
    std::unique_ptr<RenderPipeline> g_menuPipeline; // options menu
    std::unique_ptr<RenderTexture> g_menuTexture;
    RenderBuffer* g_menuUpload = nullptr; // the current slot's (Render)
    uint32_t g_menuDescriptor = 0, g_menuVersion = UINT32_MAX;
    std::unique_ptr<RenderPipeline> g_overlayPipeline; // touch controls
    std::unique_ptr<RenderShader> g_encodePixelShader;
    std::unique_ptr<RenderPipeline> g_encodePipeline;  // 7e3 encode for resolves from the fixed-point view
    std::unique_ptr<RenderPipeline> g_blitPipeline, g_depthPipeline;

    // Depth resolves: host depth -> staging buffer -> R32F scratch texture -> conversion pass.
    std::unique_ptr<RenderBuffer> g_depthStaging;
    uint64_t g_depthStagingSize = 0;
    std::unique_ptr<RenderTexture> g_depthScratch;
    uint32_t g_depthScratchWidth = 0, g_depthScratchHeight = 0, g_depthScratchDescriptor = 0;
    // From MSAA depth (not copyable to a buffer): sample 0 of each pixel -> R32F target -> conversion.
    std::unique_ptr<RenderShader> g_depthMsaaPixelShader;
    std::unique_ptr<RenderPipeline> g_depthMsaaPipeline;
    std::unique_ptr<RenderTexture> g_depthMsaaScratch;
    std::unique_ptr<RenderFramebuffer> g_depthMsaaFramebuffer;
    uint32_t g_depthMsaaWidth = 0, g_depthMsaaHeight = 0, g_depthMsaaDescriptor = 0;
    uint32_t g_constantSlot = 0; // small per-frame constants (depth resolves), after the frame data

    // Two frames in flight (video.cpp): the video thread records frame N while the GPU still runs
    // N - 1. Everything the CPU writes for a frame, or frees after it, belongs to its slot, which
    // is reused only once the frame two back has finished (Renderer::Render is told the slot).
    constexpr uint32_t kFrameSlots = 2;
    uint32_t g_frameSlot = 0;
    struct UploadSlot
    {
        std::unique_ptr<RenderBuffer> buffer;
        uint8_t* memory = nullptr;
        uint64_t address = 0;
        size_t size = 0;
    };
    UploadSlot g_uploadSlots[kFrameSlots];
    RenderBuffer* g_uploadBuffer = nullptr; // the current slot's (frame data, constants, blit constants)
    uint8_t* g_uploadMemory = nullptr;
    uint64_t g_uploadAddress = 0;
    size_t g_uploadSize = 0;
    std::unique_ptr<RenderBuffer> g_geometryBuffer; // resident vertex streams and index lists (GeometryHeap)

    void CreateUploadSlot(UploadSlot& slot, size_t size)
    {
        slot.buffer.reset(); // the old one goes first: both at once would need the memory twice
        slot.size = size;
        slot.buffer = g_device->createBuffer(RenderBufferDesc::UploadBuffer(slot.size,
            RenderBufferFlag::CONSTANT | RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX));
        slot.memory = static_cast<uint8_t*>(slot.buffer->map());
        slot.address = slot.buffer->getDeviceAddress();
    }

    // A frame's data (with the textures it uploads) goes through its slot's upload buffer. Made
    // bigger in game, it stalled that frame while the memory was made and zeroed: 16-37 ms in a 4K
    // test, 100-130 ms in the user's 4K session at area loads (2026-11-25). Renderer::Init makes
    // both at the size area loads were seen to need (frame data up to 109 MB), so that is rare.
    void UseUploadSlot(size_t needed)
    {
        UploadSlot& slot = g_uploadSlots[g_frameSlot];
        if (slot.size < needed)
        {
            CreateUploadSlot(slot, std::max<size_t>(needed * 3 / 2, 16 << 20));
            static uint32_t logged;
            if (logged++ < 50)
                LOGFN("[renderer] upload buffer of slot {} grown to {} MB in game (frame data {} MB); [memory] {}", g_frameSlot, slot.size >> 20,
                    needed >> 20, MemoryInfo::Describe());
        }
        g_uploadBuffer = slot.buffer.get();
        g_uploadMemory = slot.memory;
        g_uploadAddress = slot.address;
        g_uploadSize = slot.size;
    }

    // The texture descriptors are one set shared by both frames in flight, and the CPU writes them
    // at once: pointing a descriptor at a different texture first waits for the frame in the other
    // slot, which may still read it (video.cpp's callback). Rare: textures made again at a new size
    // or format, targets that grow, resolve textures made. Freed descriptors instead wait for their
    // own slot to come round (SlotState) before they become white and can be handed out again.
    void (*g_waitOtherSlot)() = nullptr;
    bool g_otherSlotWaited = false;
    std::atomic<uint32_t> g_descriptorWaits{ 0 };

    void ReplaceDescriptor(uint32_t descriptor, const RenderTexture* texture, const RenderTextureView* view = nullptr)
    {
        if (!g_otherSlotWaited && g_waitOtherSlot != nullptr)
        {
            HitchStats::Timer timer(HitchStats::OtherFrameWaits);
            g_waitOtherSlot();
            g_otherSlotWaited = true;
            g_descriptorWaits++;
        }
        g_textureSet->setTexture(descriptor, texture, RenderTextureLayout::SHADER_READ, view);
    }
    std::vector<uint32_t> g_releasedThisFrame; // descriptors freed by this frame (to its slot)

    // EDRAM, modelled the way the game uses it: colour buffers by (EDRAM base, format) - the game
    // switches formats from pass to pass (HDR scene, 8888 post-processing) and draws and resolves
    // through different surface objects over the same EDRAM - and depth buffers by EDRAM base.
    struct DepthBuffer
    {
        std::unique_ptr<RenderTexture> texture;
        std::unique_ptr<RenderTextureView> view; // MSAA: the depth plane, read by depth resolves
        uint32_t width = 0, height = 0, samples = 1, descriptor = 0;
        RenderTextureLayout layout = RenderTextureLayout::UNKNOWN;
    };
    ankerl::unordered_dense::map<uint32_t, std::unique_ptr<DepthBuffer>> g_depthBuffers; // depth key | samples << 16

    // MSAA targets (the console's 4X scene pass) keep a single-sample copy: resolves average the
    // samples of the rectangle they copy into it first, and copy or sample from there.
    struct HostTarget
    {
        std::unique_ptr<RenderTexture> color;
        std::unique_ptr<RenderTexture> single; // MSAA only: averaged samples
        DepthBuffer* depth = nullptr;
        std::unique_ptr<RenderFramebuffer> framebuffer;
        const RenderTexture* framebufferDepth = nullptr; // depth texture the framebuffer was built with
        uint32_t width = 0, height = 0, samples = 1;
        RenderFormat format = RenderFormat::UNKNOWN;
        RenderTextureLayout colorLayout = RenderTextureLayout::UNKNOWN;
        RenderTextureLayout singleLayout = RenderTextureLayout::UNKNOWN;
        uint32_t descriptor = 0; // single-sample image: 'color', or 'single' for MSAA
        uint32_t draws = 0;
    };
    ankerl::unordered_dense::map<uint64_t, std::unique_ptr<HostTarget>> g_targets;
    HostTarget* g_shownTarget = nullptr;

    // Host copies of the textures the game resolves into (by guest address).
    struct ResolveTexture
    {
        std::unique_ptr<RenderTexture> texture;
        std::unique_ptr<RenderFramebuffer> framebuffer; // depth resolves render into it
        std::unique_ptr<RenderTextureView> view;        // every mip level (cube: the six faces)
        uint32_t width = 0, height = 0, descriptor = 0, address = 0, lastFrame = UINT32_MAX;
        uint32_t levels = 1;
        bool cube = false;
        RenderFormat format = RenderFormat::UNKNOWN;
    };
    ankerl::unordered_dense::map<uint32_t, std::unique_ptr<ResolveTexture>> g_resolveTextures;
    ResolveTexture* g_shownResolve = nullptr; // the last full-screen resolve of the frame
    ResolveTexture g_sceneSnapshot;        // the 3D scene as the tiled resolves copied it out
    bool g_showFinal = true;               // the game's final image; DR_SHOW=scene shows the HDR scene snapshot
    uint32_t g_frameIndex = 0;
    float g_replayScale = 1.0f;            // resolution scale of the frame being replayed (Frame::scale)
    bool g_dumpingFrame = false; // DR_DUMP: this frame dumps every resolve and render target
    uint32_t g_resolveIndex = 0;
    void QueueDump(RenderCommandList* list, RenderTexture* texture, RenderFormat format, uint32_t width, uint32_t height, std::string path,
        uint32_t level = 0, uint32_t slice = 0);
    void QueueWriteback(RenderCommandList* list, RenderTexture* texture, const Command& cmd, uint32_t width, uint32_t height, float scale);

    // Textures replaced while a frame is recorded stay alive until that frame has finished on the
    // GPU (the video thread waits for each frame before starting the next).
    std::vector<std::unique_ptr<RenderTexture>> g_retiredTextures;
    std::vector<std::unique_ptr<RenderFramebuffer>> g_retiredFramebuffers;
    std::vector<std::unique_ptr<RenderTextureView>> g_retiredViews;

    // Textures decoded from guest memory, by descriptor index.
    struct HostTexture
    {
        std::unique_ptr<RenderTexture> texture;
        std::unique_ptr<RenderTextureView> view;
        uint32_t width = 0, height = 0, layers = 0, levels = 0;
        RenderFormat format = RenderFormat::UNKNOWN;
        RenderSwizzle swizzle[4]{};
    };
    ankerl::unordered_dense::map<uint32_t, HostTexture> g_hostTextures;
    std::vector<std::unique_ptr<RenderSampler>> g_samplers;

    RenderTextureAddressMode ConvertClamp(uint32_t clamp)
    {
        switch (clamp)
        {
        case 0: return RenderTextureAddressMode::WRAP;
        case 1: return RenderTextureAddressMode::MIRROR;
        case 3: case 5: case 7: return RenderTextureAddressMode::MIRROR_ONCE;
        case 6: return RenderTextureAddressMode::BORDER;
        default: return RenderTextureAddressMode::CLAMP; // clamp to last / halfway
        }
    }

    // Creates the samplers the guest registered (state packed as in SamplerIndex).
    void CreateNewSamplers()
    {
        std::lock_guard lock(g_samplerMutex);
        for (auto [state, index] : g_newSamplers)
        {
            RenderSamplerDesc desc;
            desc.addressU = ConvertClamp(state & 7);
            desc.addressV = ConvertClamp((state >> 3) & 7);
            desc.addressW = ConvertClamp((state >> 6) & 7);
            desc.magFilter = ((state >> 9) & 3) == 0 ? RenderFilter::NEAREST : RenderFilter::LINEAR;
            desc.minFilter = ((state >> 11) & 3) == 0 ? RenderFilter::NEAREST : RenderFilter::LINEAR;
            const uint32_t mip = (state >> 13) & 3;
            desc.mipmapMode = mip == 0 ? RenderMipmapMode::NEAREST : RenderMipmapMode::LINEAR;
            if (mip == 2)
                desc.maxLOD = 0.0f; // base map only
            desc.mipLODBias = float(int32_t(uint32_t(state) << 4) >> 22) / 32.0f;
            const uint32_t minLevel = uint32_t(state >> 32) & 0xF, maxLevel = uint32_t(state >> 36) & 0xF;
            static const bool noMipClamp = getenv("DR_NO_MIP_CLAMP") != nullptr; // debugging
            if (!noMipClamp)
            {
                desc.minLOD = float(minLevel);
                desc.maxLOD = std::min(desc.maxLOD, float(std::max(minLevel, maxLevel)));
            }
            const uint32_t aniso = (state >> 15) & 7;
            if (aniso >= 2 && aniso <= 5)
            {
                desc.anisotropyEnabled = true;
                desc.maxAnisotropy = 1u << (aniso - 1);
            }
            if (g_samplers.size() <= index)
                g_samplers.resize(index + 1);
            g_samplers[index] = g_device->createSampler(desc);
            g_samplerSet->setSampler(index, g_samplers[index].get());
        }
        g_newSamplers.clear();
    }

    void ReplayUpload(RenderCommandList* list, const Frame& frame, const Command& cmd)
    {
        HitchStats::Timer timer(HitchStats::Uploads);
        auto& t = g_hostTextures[cmd.uploadDescriptor];
        const uint32_t levels = std::max(cmd.uploadLevels, 1u);
        const bool recreate = t.texture == nullptr || t.width != cmd.width || t.height != cmd.height || t.format != cmd.uploadFormat ||
            t.layers != cmd.uploadLayers || t.levels != levels;
        const bool replacing = t.view != nullptr; // the descriptor already shows a texture
        bool descriptorChanged = false;
        if (recreate)
        {
            if (t.texture != nullptr)
                g_retiredTextures.push_back(std::move(t.texture));
            RenderTextureDesc desc = RenderTextureDesc::Texture2D(cmd.width, cmd.height, levels, cmd.uploadFormat);
            if (cmd.uploadCube)
            {
                desc.arraySize = 6;
                desc.flags = RenderTextureFlag::CUBE;
            }
            t.texture = g_device->createTexture(desc);
            t.width = cmd.width;
            t.height = cmd.height;
            t.format = cmd.uploadFormat;
            t.layers = cmd.uploadLayers;
            t.levels = levels;
        }
        if (recreate || memcmp(t.swizzle, cmd.uploadSwizzle, sizeof(t.swizzle)) != 0)
        {
            if (t.view != nullptr)
                g_retiredViews.push_back(std::move(t.view));
            RenderTextureViewDesc view;
            view.format = cmd.uploadFormat;
            view.dimension = cmd.uploadCube ? RenderTextureViewDimension::TEXTURE_CUBE : RenderTextureViewDimension::TEXTURE_2D;
            view.mipLevels = levels;
            view.componentMapping = RenderComponentMapping(cmd.uploadSwizzle[0], cmd.uploadSwizzle[1], cmd.uploadSwizzle[2], cmd.uploadSwizzle[3]);
            t.view = t.texture->createTextureView(view);
            memcpy(t.swizzle, cmd.uploadSwizzle, sizeof(t.swizzle));
            descriptorChanged = true;
        }

        list->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(t.texture.get(), RenderTextureLayout::COPY_DEST));
        const uint64_t uploadOffset = uint64_t(cmd.uploadOffset) + (cmd.uploadTaken ? TextureUploadBase(frame) : 0);
        if (cmd.uploadLevels == 0)
        {
            for (uint32_t layer = 0; layer < cmd.uploadLayers; layer++)
                list->copyTextureRegion(RenderTextureCopyLocation::Subresource(t.texture.get(), 0, layer),
                    RenderTextureCopyLocation::PlacedFootprint(g_uploadBuffer, cmd.uploadFormat, cmd.width, cmd.height, 1,
                        cmd.uploadRowWidth, uploadOffset + uint64_t(layer) * cmd.uploadLayerPitch));
        }
        else
        {
            TextureDecode::Decoded::Level table[TextureDecode::kMaxLevels];
            memcpy(table, frame.data.data() + cmd.uploadLevelTable, sizeof(table[0]) * levels);
            for (uint32_t level = 0; level < levels; level++)
                for (uint32_t layer = 0; layer < cmd.uploadLayers; layer++)
                    list->copyTextureRegion(RenderTextureCopyLocation::Subresource(t.texture.get(), level, layer),
                        RenderTextureCopyLocation::PlacedFootprint(g_uploadBuffer, cmd.uploadFormat, table[level].width, table[level].height, 1,
                            table[level].rowWidth, uploadOffset + table[level].offset + uint64_t(layer) * table[level].layerPitch));
        }
        if (g_dumpingFrame && cmd.uploadFormat == RenderFormat::R32G32B32A32_FLOAT)
            QueueDump(list, t.texture.get(), cmd.uploadFormat, cmd.width, cmd.height, fmt::format("vtex{}", cmd.uploadDescriptor));
        list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(t.texture.get(), RenderTextureLayout::SHADER_READ));
        if (descriptorChanged)
        {
            // A new texture's descriptor is fresh (no frame in flight reads it); a replaced one waits.
            if (replacing)
                ReplaceDescriptor(cmd.uploadDescriptor, t.texture.get(), t.view.get());
            else
                g_textureSet->setTexture(cmd.uploadDescriptor, t.texture.get(), RenderTextureLayout::SHADER_READ, t.view.get());
        }
    }

    // Pipelines by key hash. A frame's new pipelines are made in parallel on worker threads, asked
    // for when the guest presents it (RequestPipelines); replay waits for any not done yet. Made one
    // by one at replay they cost about 1.3 ms each: 40 ms when an area loaded (2026-11-25).
    struct PipelineSlot
    {
        std::unique_ptr<RenderPipeline> pipeline;
        bool ready = false; // under g_pipelineMutex
    };
    std::mutex g_pipelineMutex;
    std::condition_variable g_pipelineReady;
    ankerl::unordered_dense::map<uint64_t, std::unique_ptr<PipelineSlot>> g_pipelines;
    std::atomic<uint32_t> g_newPipelines{ 0 }; // made since the current replay began
    std::atomic<uint32_t> g_pipelinesFailed{ 0 };

    class WorkerPool
    {
    public:
        explicit WorkerPool(const char* name) : name(name) {}

        void Submit(std::function<void()> job)
        {
            std::call_once(started, [this] {
                const uint32_t threads = std::clamp(std::thread::hardware_concurrency() / 2, 2u, 8u);
                for (uint32_t i = 0; i < threads; i++)
                    std::thread([this] { Run(); }).detach();
                LOGFN("[renderer] {} {} threads", threads, this->name);
            });
            {
                std::lock_guard lock(mutex);
                queue.push_back(std::move(job));
            }
            work.notify_one();
        }

    private:
        void Run()
        {
            while (true)
            {
                std::function<void()> job;
                {
                    std::unique_lock lock(mutex);
                    work.wait(lock, [this] { return !queue.empty(); });
                    job = std::move(queue.front());
                    queue.pop_front();
                }
                job();
            }
        }

        const char* name;
        std::once_flag started;
        std::mutex mutex;
        std::condition_variable work;
        std::deque<std::function<void()>> queue;
    } g_pipelinePool("pipeline");

    struct ReplayStats { uint32_t draws = 0, skipped = 0; } g_replayStats;

    // Fullscreen blit of a host target into the window (compiled at startup with DXC for D3D12; the
    // same source is in blit.hlsl, compiled to SPIR-V ahead of time for Vulkan - keep them in sync).
    const char* kBlitShader = R"(
#ifdef __spirv__
struct PushConstants { uint64_t VertexShaderConstants; uint64_t PixelShaderConstants; uint64_t SharedConstants; };
[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;
#define TEXTURE_INDEX vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants)
#define FLIP vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 4)
#define OFFSET_X vk::RawBufferLoad<int>(g_PushConstants.SharedConstants + 8)
#define OFFSET_Y vk::RawBufferLoad<int>(g_PushConstants.SharedConstants + 12)
#define CIRCLE_COUNT vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 16)
#define CIRCLE(i) vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 32 + (i) * 16)
#define CIRCLE_COLOR(i) vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 32 + 32 * 16 + (i) * 16)
#else
cbuffer SharedConstants : register(b2, space4) { uint g_TextureIndex; uint g_Flip; int g_OffsetX; int g_OffsetY;
    uint g_CircleCount; uint3 g_Pad; float4 g_Circles[32]; float4 g_CircleColors[32]; };
#define CIRCLE_COUNT g_CircleCount
#define CIRCLE(i) g_Circles[i]
#define CIRCLE_COLOR(i) g_CircleColors[i]
#define TEXTURE_INDEX g_TextureIndex
#define FLIP g_Flip
#define OFFSET_X g_OffsetX
#define OFFSET_Y g_OffsetY
#endif
Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);

void mainVS(uint id : SV_VertexID, out float4 position : SV_Position, out float2 uv : TEXCOORD0)
{
    uv = float2((id << 1) & 2, id & 2);
    position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

#define BLIT_SAMPLER 63 // linear, clamped (kBlitSampler)
#define FXAA_ENABLED FLIP // the final blit reuses the depth resolve's flag slot
float FxaaLuma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
// Final image to the window, with FXAA ("console" FXAA 3.11): the console renders with 4X MSAA.
float4 mainPS(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    Texture2D<float4> image = g_Texture2DDescriptorHeap[TEXTURE_INDEX];
    SamplerState linearClamp = g_SamplerDescriptorHeap[BLIT_SAMPLER];
    // A picture larger than the window (resolution scale 2x / 3x): average what lies under the
    // pixel (area filter); one bilinear tap per pixel skipped texels and looked pixelated.
    float2 footprint = float2(abs(ddx(uv.x)), abs(ddy(uv.y)));
    float2 imageSize;
    image.GetDimensions(imageSize.x, imageSize.y);
    float2 ratio = footprint * imageSize;
    if (max(ratio.x, ratio.y) > 1.01)
    {
        int2 taps = int2(clamp(ceil(ratio), 1.0, 4.0));
        float3 sum = 0.0;
        for (int ty = 0; ty < taps.y; ty++)
            for (int tx = 0; tx < taps.x; tx++)
                sum += image.SampleLevel(linearClamp, uv + ((float2(tx, ty) + 0.5) / float2(taps) - 0.5) * footprint, 0).rgb;
        return float4(sum / float(taps.x * taps.y), 1.0);
    }
    float3 rgbM = image.SampleLevel(linearClamp, uv, 0).rgb;
    if (FXAA_ENABLED == 0)
        return float4(rgbM, 1.0);
    float2 size;
    image.GetDimensions(size.x, size.y);
    float2 texel = 1.0 / size;
    float lumaNw = FxaaLuma(image.SampleLevel(linearClamp, uv + float2(-0.5, -0.5) * texel, 0).rgb);
    float lumaSw = FxaaLuma(image.SampleLevel(linearClamp, uv + float2(-0.5, 0.5) * texel, 0).rgb);
    float lumaNe = FxaaLuma(image.SampleLevel(linearClamp, uv + float2(0.5, -0.5) * texel, 0).rgb) + 1.0 / 384.0;
    float lumaSe = FxaaLuma(image.SampleLevel(linearClamp, uv + float2(0.5, 0.5) * texel, 0).rgb);
    float lumaM = FxaaLuma(rgbM);
    float lumaMax = max(max(lumaNw, lumaSw), max(lumaNe, lumaSe));
    float lumaMin = min(min(lumaNw, lumaSw), min(lumaNe, lumaSe));
    if (max(lumaMax, lumaM) - min(lumaMin, lumaM) < max(0.05, lumaMax * 0.125))
        return float4(rgbM, 1.0);
    float dirSwMinusNe = lumaSw - lumaNe;
    float dirSeMinusNw = lumaSe - lumaNw;
    float2 dir1 = normalize(float2(dirSwMinusNe + dirSeMinusNw, dirSwMinusNe - dirSeMinusNw));
    float3 rgbN1 = image.SampleLevel(linearClamp, uv - dir1 * 0.5 * texel, 0).rgb;
    float3 rgbP1 = image.SampleLevel(linearClamp, uv + dir1 * 0.5 * texel, 0).rgb;
    float dirAbsMinTimesC = max(min(abs(dir1.x), abs(dir1.y)) * 8.0, 1e-6);
    float2 dir2 = clamp(dir1 / dirAbsMinTimesC, -2.0, 2.0);
    float3 rgbN2 = image.SampleLevel(linearClamp, uv - dir2 * 2.0 * texel, 0).rgb;
    float3 rgbP2 = image.SampleLevel(linearClamp, uv + dir2 * 2.0 * texel, 0).rgb;
    float3 rgbA = rgbN1 + rgbP1;
    float3 rgbB = (rgbN2 + rgbP2) * 0.25 + rgbA * 0.25;
    float lumaB = FxaaLuma(rgbB);
    if (lumaB < lumaMin || lumaB > lumaMax)
        rgbB = rgbA * 0.5;
    return float4(rgbB, 1.0);
}

// Depth resolve: the copied host depth (R32F) into the destination texture, with the host's
// reversed-Z mirror undone (FLIP), the destination position subtracted (OFFSET).
float4 mainDepthPS(float4 position : SV_Position) : SV_Target
{
    float depth = g_Texture2DDescriptorHeap[TEXTURE_INDEX].Load(int3(int2(position.xy) - int2(OFFSET_X, OFFSET_Y), 0)).r;
    if (FLIP != 0)
        depth = 1.0 - depth;
    return float4(depth, depth, depth, 1.0);
}

// MSAA depth (the 4X scene pass): sample 0 of each pixel, as the console resolves depth. Set 1 is
// bound to the same descriptor heap as set 0.
Texture2DMS<float4> g_Texture2DMSDescriptorHeap[] : register(t0, space1);
float4 mainDepthMsaaPS(float4 position : SV_Position) : SV_Target
{
    float depth = g_Texture2DMSDescriptorHeap[TEXTURE_INDEX].Load(int2(position.xy) - int2(OFFSET_X, OFFSET_Y), 0).r;
    return float4(depth, depth, depth, 1.0);
}

// Resolve from the 10:10:10:2 fixed-point view of the HDR buffer: the console copies the raw 7e3
// bits, which the game decodes in its shaders. The host buffer holds values: encode them (unorm).
float encode7e3(float f)
{
    f = clamp(f, 0.0, 31.875);
    if (f < 0.25)
        return round(f * 512.0) / 1023.0;
    float e = floor(log2(f));
    if (f < exp2(e))
        e -= 1.0;
    else if (f >= exp2(e + 1.0))
        e += 1.0;
    float m = round((f / exp2(e) - 1.0) * 128.0);
    if (m >= 128.0)
    {
        m = 0.0;
        e += 1.0;
    }
    return min((e + 3.0) * 128.0 + m, 1023.0) / 1023.0;
}

float4 mainEncode7e3PS(float4 position : SV_Position) : SV_Target
{
    float4 v = g_Texture2DDescriptorHeap[TEXTURE_INDEX].Load(int3(int2(position.xy) - int2(OFFSET_X, OFFSET_Y), 0));
    return float4(encode7e3(v.r), encode7e3(v.g), encode7e3(v.b), round(saturate(v.a) * 3.0) / 3.0);
}

// Touch controls (hid/touch_controls.cpp): circles (centre, radius in pixels, filled > 0.5) with
// straight-alpha colours, blended over the final image.
float4 mainOverlayPS(float4 position : SV_Position) : SV_Target
{
    float4 result = 0.0;
    for (uint i = 0; i < CIRCLE_COUNT; i++)
    {
        float4 circle = CIRCLE(i);
        float4 color = CIRCLE_COLOR(i);
        float distance = length(position.xy - circle.xy);
        float coverage = circle.w > 0.5 ? saturate(circle.z - distance) : saturate(1.5 - abs(distance - circle.z));
        float alpha = coverage * color.a;
        result.rgb = color.rgb * alpha + result.rgb * (1.0 - alpha);
        result.a = alpha + result.a * (1.0 - alpha);
    }
    return float4(result.rgb / max(result.a, 1e-4), result.a);
}

// Options menu (ui/options_menu.cpp): a straight-alpha RGBA image over the final picture.
float4 mainMenuPS(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    return g_Texture2DDescriptorHeap[TEXTURE_INDEX].SampleLevel(g_SamplerDescriptorHeap[BLIT_SAMPLER], uv, 0);
}
)";

#ifdef _WIN32
    std::unique_ptr<RenderShader> CompileShader(const char* source, const wchar_t* entry, const wchar_t* profile, bool vertex)
    {
        ComPtr<IDxcCompiler3> compiler;
        if (FAILED(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(compiler.GetAddressOf()))))
            return nullptr;

        std::vector<const wchar_t*> args = { L"-E", entry, L"-T", profile, L"-HV", L"2021" };
        if (g_vulkan)
        {
            args.insert(args.end(), { L"-spirv", L"-fvk-use-dx-layout" });
            if (vertex)
                args.push_back(L"-fvk-invert-y");
        }

        DxcBuffer buffer{ source, strlen(source), 0 };
        ComPtr<IDxcResult> result;
        if (FAILED(compiler->Compile(&buffer, args.data(), uint32_t(args.size()), nullptr, IID_PPV_ARGS(result.GetAddressOf()))))
            return nullptr;

        HRESULT status = E_FAIL;
        result->GetStatus(&status);
        if (FAILED(status))
        {
            ComPtr<IDxcBlobUtf8> errors;
            if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(errors.GetAddressOf()), nullptr)) && errors != nullptr)
                LOGFN_ERROR("[renderer] blit shader: {}", std::string_view(errors->GetStringPointer(), errors->GetStringLength()));
            return nullptr;
        }

        ComPtr<IDxcBlob> blob;
        result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(blob.GetAddressOf()), nullptr);
        if (blob == nullptr)
            return nullptr;

        char entryName[32];
        snprintf(entryName, sizeof(entryName), "%ls", entry);
        return g_device->createShader(blob->GetBufferPointer(), blob->GetBufferSize(), entryName,
            g_vulkan ? RenderShaderFormat::SPIRV : RenderShaderFormat::DXIL);
    }
#endif

    RenderBlend ConvertBlend(uint32_t value)
    {
        switch (value)
        {
        case 0: return RenderBlend::ZERO;
        case 1: return RenderBlend::ONE;
        case 4: return RenderBlend::SRC_COLOR;
        case 5: return RenderBlend::INV_SRC_COLOR;
        case 6: return RenderBlend::SRC_ALPHA;
        case 7: return RenderBlend::INV_SRC_ALPHA;
        case 8: return RenderBlend::DEST_COLOR;
        case 9: return RenderBlend::INV_DEST_COLOR;
        case 10: return RenderBlend::DEST_ALPHA;
        case 11: return RenderBlend::INV_DEST_ALPHA;
        case 12: case 14: return RenderBlend::BLEND_FACTOR;
        case 13: case 15: return RenderBlend::INV_BLEND_FACTOR;
        case 16: return RenderBlend::SRC_ALPHA_SAT;
        default: return RenderBlend::ONE;
        }
    }

    RenderBlendOperation ConvertBlendOp(uint32_t value)
    {
        switch (value)
        {
        case 1: return RenderBlendOperation::SUBTRACT;
        case 2: return RenderBlendOperation::MIN;
        case 3: return RenderBlendOperation::MAX;
        case 4: return RenderBlendOperation::REV_SUBTRACT;
        default: return RenderBlendOperation::ADD;
        }
    }

    std::unique_ptr<RenderPipeline> CreatePipeline(const PipelineKey& key);

    // Replay (video thread): the key's pipeline, made now if nobody asked for it, waited for if a
    // worker is making it.
    RenderPipeline* GetPipeline(const PipelineKey& key)
    {
        const uint64_t hash = XXH3_64bits(&key, sizeof(key));
        PipelineSlot* slot;
        {
            std::unique_lock lock(g_pipelineMutex);
            auto& entry = g_pipelines[hash];
            if (entry != nullptr)
            {
                slot = entry.get();
                if (!slot->ready)
                {
                    HitchStats::Timer timer(HitchStats::Pipelines);
                    g_pipelineReady.wait(lock, [slot] { return slot->ready; });
                }
                return slot->pipeline.get();
            }
            entry = std::make_unique<PipelineSlot>();
            slot = entry.get();
        }
        HitchStats::Timer timer(HitchStats::Pipelines);
        std::unique_ptr<RenderPipeline> pipeline = CreatePipeline(key);
        {
            std::lock_guard lock(g_pipelineMutex);
            slot->pipeline = std::move(pipeline);
            slot->ready = true;
        }
        g_pipelineReady.notify_all();
        return slot->pipeline.get();
    }

    // Guest thread, at present: the pipelines of the frame's draws that do not exist yet, made in
    // parallel while the video thread gets to them.
    void RequestPipelines(const Frame& frame)
    {
        static ankerl::unordered_dense::set<uint64_t> known; // guest thread only
        for (const Command& cmd : frame.commands)
        {
            if (cmd.type != Command::Draw || (cmd.colorKey == 0 && cmd.depthKey == 0) || cmd.key.vertexShader == nullptr)
                continue;
            const uint64_t hash = XXH3_64bits(&cmd.key, sizeof(cmd.key));
            if (!known.insert(hash).second)
                continue;
            PipelineSlot* slot;
            {
                std::lock_guard lock(g_pipelineMutex);
                auto& entry = g_pipelines[hash];
                if (entry != nullptr)
                    continue;
                entry = std::make_unique<PipelineSlot>();
                slot = entry.get();
            }
            const PipelineKey key = cmd.key;
            g_pipelinePool.Submit([key, slot] {
                std::unique_ptr<RenderPipeline> pipeline = CreatePipeline(key);
                {
                    std::lock_guard lock(g_pipelineMutex);
                    slot->pipeline = std::move(pipeline);
                    slot->ready = true;
                }
                g_pipelineReady.notify_all();
            });
        }
    }

    std::unique_ptr<RenderPipeline> CreatePipeline(const PipelineKey& key)
    {
        std::unique_ptr<RenderPipeline> pipeline;
        g_newPipelines++;

        RenderGraphicsPipelineDesc desc;
        desc.pipelineLayout = g_pipelineLayout.get();
        desc.vertexShader = Shaders::GetHostShader(const_cast<ShaderCacheEntry*>(key.vertexShader), key.specConstants);
        desc.pixelShader = key.pixelShader != nullptr ? Shaders::GetHostShader(const_cast<ShaderCacheEntry*>(key.pixelShader), key.specConstants) : nullptr;
        if (desc.vertexShader == nullptr || (key.pixelShader != nullptr && desc.pixelShader == nullptr))
        {
            g_pipelinesFailed++;
            return nullptr;
        }

        static constexpr RenderComparisonFunction kCompare[] = { RenderComparisonFunction::NEVER, RenderComparisonFunction::LESS,
            RenderComparisonFunction::EQUAL, RenderComparisonFunction::LESS_EQUAL, RenderComparisonFunction::GREATER,
            RenderComparisonFunction::NOT_EQUAL, RenderComparisonFunction::GREATER_EQUAL, RenderComparisonFunction::ALWAYS };
        desc.depthEnabled = (key.depthControl & 2) != 0;
        desc.depthWriteEnabled = (key.depthControl & 4) != 0;
        desc.depthFunction = kCompare[(key.depthControl >> 4) & 7];
        static const bool noDepthClip = getenv("DR_NO_DEPTH_CLIP") != nullptr; // debugging
        desc.depthClipEnabled = !noDepthClip;
        desc.depthTargetFormat = kDepthFormat;
        desc.multisampling.sampleCount = std::max(key.samples, 1u);
        desc.alphaToCoverageEnabled = (key.flags & kPipelineAlphaToCoverage) != 0;
        if (key.depthControl & 1)
        {
            // Xenos stencil ops: keep, zero, replace, incr clamp, decr clamp, invert, incr wrap, decr wrap.
            static constexpr RenderStencilOp kStencilOp[] = { RenderStencilOp::KEEP, RenderStencilOp::ZERO, RenderStencilOp::REPLACE,
                RenderStencilOp::INCREMENT_AND_CLAMP, RenderStencilOp::DECREMENT_AND_CLAMP, RenderStencilOp::INVERT,
                RenderStencilOp::INCREMENT_AND_WRAP, RenderStencilOp::DECREMENT_AND_WRAP };
            const uint32_t d = key.depthControl;
            desc.stencilEnabled = true;
            desc.stencilReference = key.stencilRefMask & 0xFF;
            desc.stencilReadMask = (key.stencilRefMask >> 8) & 0xFF;
            desc.stencilWriteMask = (key.stencilRefMask >> 16) & 0xFF;
            auto face = [&](uint32_t shift)
            {
                RenderStencilFaceDesc f;
                f.compareFunction = kCompare[(d >> shift) & 7];
                f.failOp = kStencilOp[(d >> (shift + 3)) & 7];
                f.passOp = kStencilOp[(d >> (shift + 6)) & 7];
                f.depthFailOp = kStencilOp[(d >> (shift + 9)) & 7];
                return f;
            };
            desc.stencilFrontFace = face(8);
            desc.stencilBackFace = (d & 0x80) ? face(20) : desc.stencilFrontFace;

            // The velocity pass redraws characters with another vertex shader over the depth the
            // scene left (restored by a depth-export pass) and marks them in stencil where the
            // depth test passes with "nearer or equal". The console's 24-bit depth rounds away the
            // last-bit differences between the two shaders; 32-bit float depth keeps them, the test
            // fails and nothing is marked. A few units of bias toward the viewer restore the
            // console's tolerance for draws that mark stencil on a depth pass.
            static const bool noMarkBias = getenv("DR_NO_STENCIL_BIAS") != nullptr; // debugging
            const uint32_t zfunc = (d >> 4) & 7;
            const bool marks = desc.depthEnabled && ((d >> 14) & 7) == 2; // z pass: replace
            if (marks && !noMarkBias && (zfunc == 6 || zfunc == 4 || zfunc == 3 || zfunc == 1))
            {
                const bool reversed = zfunc == 6 || zfunc == 4; // GEQUAL / GREATER: nearer is larger
                desc.depthBias = reversed ? 64 : -64;
                desc.slopeScaledDepthBias = reversed ? 0.5f : -0.5f;
            }

            // 4X draws after a per-pixel depth restore (PrepareDraw): toward the viewer by the depth
            // change over one pixel, so samples up to 3/8 pixel off the centre pass "nearer or equal".
            if ((key.flags & kPipelineMsaaRestoreBias) && desc.depthEnabled && (zfunc == 1 || zfunc == 3 || zfunc == 4 || zfunc == 6))
            {
                const bool reversed = zfunc == 6 || zfunc == 4;
                desc.depthBias = reversed ? 16 : -16;
                desc.slopeScaledDepthBias = reversed ? 1.0f : -1.0f;
            }
        }
        desc.primitiveTopology = key.topology;
        // PA_SU_SC_MODE_CNTL: bit 0 culls front faces, bit 1 back faces; bit 2 set makes clockwise
        // the front (window space, as on the host: the viewport keeps D3D's y direction).
        static const bool noCull = getenv("DR_NO_CULL") != nullptr; // debugging
        static const bool flipWinding = getenv("DR_FLIP_WINDING") != nullptr; // debugging
        switch (noCull ? 0 : key.cull & 3)
        {
        case 1: desc.cullMode = RenderCullMode::FRONT; break;
        case 2: desc.cullMode = RenderCullMode::BACK; break;
        default: desc.cullMode = RenderCullMode::NONE; break; // 3 (both) is not drawn by the game
        }
        desc.frontFace = ((key.cull & 4) != 0) != flipWinding ? RenderFrontFace::CLOCKWISE : RenderFrontFace::COUNTER_CLOCKWISE;

        if (key.colorFormat != RenderFormat::UNKNOWN)
        {
            desc.renderTargetCount = 1;
            desc.renderTargetFormat[0] = key.colorFormat;
            auto& blend = desc.renderTargetBlend[0];
            const uint32_t b = key.blendControl;
            blend.srcBlend = ConvertBlend(b & 0x1F);
            blend.blendOp = ConvertBlendOp((b >> 5) & 7);
            blend.dstBlend = ConvertBlend((b >> 8) & 0x1F);
            blend.srcBlendAlpha = ConvertBlend((b >> 16) & 0x1F);
            blend.blendOpAlpha = ConvertBlendOp((b >> 21) & 7);
            blend.dstBlendAlpha = ConvertBlend((b >> 24) & 0x1F);
            // Xenos has no blend enable: ONE * src + ZERO * dst is "off".
            blend.blendEnabled = (b & 0x1FFF1FFF) != 0x00010001;
            blend.renderTargetWriteMask = uint8_t(key.colorMask);
        }

        desc.inputElements = key.declaration->inputElements.data();
        desc.inputElementsCount = uint32_t(key.declaration->inputElements.size());

        RenderInputSlot slots[kMaxStreams + 1];
        uint32_t slotCount = 0;
        for (uint32_t n = 0; n < kMaxStreams; n++)
            if ((key.declaration->streamMask | key.declaration->auxMask) & (1u << n))
                slots[slotCount++] = RenderInputSlot(n, key.strides[n]);
        slots[slotCount++] = RenderInputSlot(kDummySlot, 0);
        desc.inputSlots = slots;
        desc.inputSlotsCount = slotCount;

        RenderSpecConstant spec(0, key.specConstants);
        if (key.specConstants != 0)
        {
            desc.specConstants = &spec;
            desc.specConstantsCount = 1;
        }

        pipeline = g_device->createGraphicsPipeline(desc);
        if (pipeline == nullptr)
        {
            // Its draws are skipped. On iOS MoltenVK says why just before ("[stderr]" lines, os/stderr_capture.cpp).
            g_pipelinesFailed++;
            static std::atomic<uint32_t> logged{ 0 };
            if (logged++ < 50)
                LOGFN_WARNING("[renderer] pipeline not made (draws skipped): VS {:016X} PS {:016X}, colour format {}, {} samples, topology {}, spec constants {:X}",
                    key.vertexShader ? key.vertexShader->hash : 0, key.pixelShader ? key.pixelShader->hash : 0, int(key.colorFormat), key.samples,
                    int(key.topology), key.specConstants);
        }
        return pipeline;
    }

    // The host colour buffer (colorKey 0: none) and depth buffer (depthKey 0: none) of a command,
    // bound together. Buffers grow when needed; 'recreated' then says the framebuffer must be bound
    // again.
    HostTarget& GetTarget(uint32_t colorKey, uint32_t depthKey, uint32_t width, uint32_t height, RenderFormat format, uint32_t samples, bool& recreated)
    {
        HitchStats::Timer timer(HitchStats::Targets);
        recreated = false;
        if (colorKey == 0)
            format = RenderFormat::UNKNOWN;
        samples = std::max(samples, 1u);

        // Depth-only passes get a target per depth buffer, colour passes one per colour buffer;
        // each per sample count (the console's EDRAM layout differs between them too, and the game
        // copies between passes of different counts through resolves and restores).
        const uint64_t key = colorKey != 0 ? (uint64_t(colorKey) << 32) | uint32_t(format) | (samples << 16)
                                           : 0xFFFFFFFF00000000ull | depthKey | (samples << 16);
        auto& slot = g_targets[key];
        if (slot == nullptr)
            slot = std::make_unique<HostTarget>();
        auto& target = *slot;
        target.samples = samples;

        DepthBuffer* depth = nullptr;
        if (depthKey != 0)
        {
            auto& depthSlot = g_depthBuffers[depthKey | (samples << 16)];
            if (depthSlot == nullptr)
                depthSlot = std::make_unique<DepthBuffer>();
            depth = depthSlot.get();
            const uint32_t depthWidth = std::max({ depth->width, width, target.width });
            const uint32_t depthHeight = std::max({ depth->height, height, target.height });
            if (depth->texture == nullptr || depth->width < depthWidth || depth->height < depthHeight)
            {
                if (depth->texture != nullptr)
                    g_retiredTextures.push_back(std::move(depth->texture));
                if (depth->view != nullptr)
                    g_retiredViews.push_back(std::move(depth->view));
                depth->width = depthWidth;
                depth->height = depthHeight;
                depth->samples = samples;
                depth->texture = g_device->createTexture(RenderTextureDesc::DepthTarget(depthWidth, depthHeight, kDepthFormat, RenderMultisampling(samples)));
                depth->layout = RenderTextureLayout::UNKNOWN;
                if (samples > 1)
                {
                    depth->view = depth->texture->createTextureView(RenderTextureViewDesc::Texture2D(kDepthFormat));
                    if (depth->descriptor == 0)
                        depth->descriptor = g_descriptorCounter++;
                    ReplaceDescriptor(depth->descriptor, depth->texture.get(), depth->view.get());
                }
            }
        }

        if (format != RenderFormat::UNKNOWN && (target.color == nullptr || target.width < width || target.height < height))
        {
            if (target.color != nullptr)
                g_retiredTextures.push_back(std::move(target.color));
            if (target.single != nullptr)
                g_retiredTextures.push_back(std::move(target.single));
            target.width = std::max(target.width, width);
            target.height = std::max(target.height, height);
            static uint32_t loggedGrowth;
            if (loggedGrowth++ < 40)
                LOGFN("[renderer] colour target {:X}: {}x{} (asked {}x{}), {} samples, format {}, depth key {:X}", key, target.width, target.height,
                    width, height, samples, uint32_t(format), depthKey);
            target.format = format;
            target.color = g_device->createTexture(RenderTextureDesc::ColorTarget(target.width, target.height, format, RenderMultisampling(samples)));
            target.colorLayout = RenderTextureLayout::UNKNOWN;
            if (samples > 1)
            {
                target.single = g_device->createTexture(RenderTextureDesc::ColorTarget(target.width, target.height, format));
                target.singleLayout = RenderTextureLayout::UNKNOWN;
            }
            if (target.descriptor == 0)
                target.descriptor = g_descriptorCounter++;
            ReplaceDescriptor(target.descriptor, samples > 1 ? target.single.get() : target.color.get());
            target.framebufferDepth = nullptr;
            if (target.framebuffer != nullptr)
                g_retiredFramebuffers.push_back(std::move(target.framebuffer));
        }
        if (format == RenderFormat::UNKNOWN && depth != nullptr)
        {
            target.width = depth->width;
            target.height = depth->height;
        }

        const RenderTexture* depthTexture = depth != nullptr ? depth->texture.get() : nullptr;
        if (target.framebuffer == nullptr || target.framebufferDepth != depthTexture || target.depth != depth)
        {
            if (target.framebuffer != nullptr)
                g_retiredFramebuffers.push_back(std::move(target.framebuffer));
            RenderFramebufferDesc fb;
            const RenderTexture* color = target.color.get();
            if (color != nullptr)
            {
                fb.colorAttachments = &color;
                fb.colorAttachmentsCount = 1;
            }
            fb.depthAttachment = depthTexture;
            target.framebuffer = g_device->createFramebuffer(fb);
            target.framebufferDepth = depthTexture;
            target.depth = depth;
            recreated = true;
        }
        return target;
    }

    void SetRootDescriptor(RenderCommandList* list, uint32_t offset, uint32_t index)
    {
        if (g_vulkan)
        {
            uint64_t address = g_uploadAddress + offset;
            list->setGraphicsPushConstants(0, &address, 8 * index, 8);
        }
        else
        {
            list->setGraphicsRootDescriptor(g_uploadBuffer->at(offset), index);
        }
    }

    void BindLayout(RenderCommandList* list)
    {
        list->setGraphicsPipelineLayout(g_pipelineLayout.get());
        list->setGraphicsDescriptorSet(g_textureSet.get(), 0);
        list->setGraphicsDescriptorSet(g_textureSet.get(), 1);
        list->setGraphicsDescriptorSet(g_textureSet.get(), 2);
        list->setGraphicsDescriptorSet(g_samplerSet.get(), 3);
    }

    void TransitionForDrawing(RenderCommandList* list, HostTarget& target)
    {
        if (target.color != nullptr && target.colorLayout != RenderTextureLayout::COLOR_WRITE)
        {
            list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(target.color.get(), RenderTextureLayout::COLOR_WRITE));
            target.colorLayout = RenderTextureLayout::COLOR_WRITE;
        }
        if (target.depth != nullptr && target.depth->layout != RenderTextureLayout::DEPTH_WRITE)
        {
            list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(target.depth->texture.get(), RenderTextureLayout::DEPTH_WRITE));
            target.depth->layout = RenderTextureLayout::DEPTH_WRITE;
        }
    }

    // A target's single-sample image in 'layout': the colour texture itself, or for MSAA targets
    // the single-sample copy with the samples of [x0,x1)x[y0,y1) averaged into it first (as the
    // console's resolve does).
    RenderTexture* SingleSampled(RenderCommandList* list, HostTarget& target, int32_t x0, int32_t y0, int32_t x1, int32_t y1, RenderTextureLayout layout)
    {
        const RenderBarrierStages stage = layout == RenderTextureLayout::COPY_SOURCE ? RenderBarrierStage::COPY : RenderBarrierStage::GRAPHICS;
        if (target.samples <= 1 || target.single == nullptr)
        {
            if (target.colorLayout != layout)
            {
                list->barriers(stage, RenderTextureBarrier(target.color.get(), layout));
                target.colorLayout = layout;
            }
            return target.color.get();
        }
        x1 = std::min(x1, int32_t(target.width));
        y1 = std::min(y1, int32_t(target.height));
        if (x1 > x0 && y1 > y0)
        {
            RenderTextureBarrier before[] = {
                RenderTextureBarrier(target.color.get(), RenderTextureLayout::RESOLVE_SOURCE),
                RenderTextureBarrier(target.single.get(), RenderTextureLayout::RESOLVE_DEST) };
            list->barriers(RenderBarrierStage::COPY, before, 2);
            target.colorLayout = RenderTextureLayout::RESOLVE_SOURCE;
            target.singleLayout = RenderTextureLayout::RESOLVE_DEST;
            RenderRect rect(x0, y0, x1, y1);
            list->resolveTextureRegion(target.single.get(), uint32_t(x0), uint32_t(y0), target.color.get(), &rect);
        }
        if (target.singleLayout != layout)
        {
            list->barriers(stage, RenderTextureBarrier(target.single.get(), layout));
            target.singleLayout = layout;
        }
        return target.single.get();
    }

    // After something else (a dump) moved a target's single-sample image to another layout.
    void SetSingleSampledLayout(HostTarget& target, RenderTextureLayout layout)
    {
        (target.samples > 1 && target.single != nullptr ? target.singleLayout : target.colorLayout) = layout;
    }

    std::vector<std::unique_ptr<RenderBuffer>> g_retiredBuffers;

    // Depth EDRAM -> texture (shadow map, scene depth): host depth -> staging buffer -> R32F scratch
    // texture -> conversion pass into the destination (an R32F render target), which undoes the
    // host's reversed-Z mirror so the game's shaders read the depth values they expect.
    void ReplayDepthResolve(RenderCommandList* list, const Command& cmd, HostTarget& target, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
    {
        if (g_depthPipeline == nullptr) // not made (Renderer::Init logs it)
            return;
        DepthBuffer& depth = *target.depth;
        x1 = std::min(x1, int32_t(depth.width));
        y1 = std::min(y1, int32_t(depth.height));
        const int32_t width = std::min(x1 - x0, int32_t(cmd.destWidth) - cmd.destX);
        const int32_t height = std::min(y1 - y0, int32_t(cmd.destHeight) - cmd.destY);
        if (width <= 0 || height <= 0 || cmd.destX < 0 || cmd.destY < 0)
            return;

        auto& slot = g_resolveTextures[cmd.destKey];
        if (slot == nullptr)
            slot = std::make_unique<ResolveTexture>();
        auto& resolved = *slot;
        if (resolved.texture == nullptr || resolved.framebuffer == nullptr || resolved.width != cmd.destWidth ||
            resolved.height != cmd.destHeight || resolved.format != RenderFormat::R32_FLOAT)
        {
            if (resolved.texture != nullptr)
                g_retiredTextures.push_back(std::move(resolved.texture));
            if (resolved.framebuffer != nullptr)
                g_retiredFramebuffers.push_back(std::move(resolved.framebuffer));
            resolved.texture = g_device->createTexture(RenderTextureDesc::ColorTarget(cmd.destWidth, cmd.destHeight, RenderFormat::R32_FLOAT));
            resolved.width = cmd.destWidth;
            resolved.height = cmd.destHeight;
            resolved.format = RenderFormat::R32_FLOAT;
            resolved.address = cmd.destAddress;
            resolved.descriptor = cmd.destDescriptor;
            const RenderTexture* color = resolved.texture.get();
            RenderFramebufferDesc fb;
            fb.colorAttachments = &color;
            fb.colorAttachmentsCount = 1;
            resolved.framebuffer = g_device->createFramebuffer(fb);
            ReplaceDescriptor(resolved.descriptor, resolved.texture.get());
        }

        // MSAA depth (the 4X scene pass): sample 0 of each pixel, as the console resolves depth, through
        // a pass into an R32F target; it then takes the place of the scratch texture below.
        uint32_t sourceDescriptor = g_depthScratchDescriptor;
        if (depth.samples > 1)
        {
            if (g_depthMsaaPipeline == nullptr || depth.view == nullptr)
                return;
            if (g_depthMsaaScratch == nullptr || g_depthMsaaWidth < uint32_t(width) || g_depthMsaaHeight < uint32_t(height))
            {
                if (g_depthMsaaScratch != nullptr)
                    g_retiredTextures.push_back(std::move(g_depthMsaaScratch));
                if (g_depthMsaaFramebuffer != nullptr)
                    g_retiredFramebuffers.push_back(std::move(g_depthMsaaFramebuffer));
                g_depthMsaaWidth = std::max<uint32_t>(g_depthMsaaWidth, width);
                g_depthMsaaHeight = std::max<uint32_t>(g_depthMsaaHeight, height);
                g_depthMsaaScratch = g_device->createTexture(RenderTextureDesc::ColorTarget(g_depthMsaaWidth, g_depthMsaaHeight, RenderFormat::R32_FLOAT));
                const RenderTexture* color = g_depthMsaaScratch.get();
                RenderFramebufferDesc fb;
                fb.colorAttachments = &color;
                fb.colorAttachmentsCount = 1;
                g_depthMsaaFramebuffer = g_device->createFramebuffer(fb);
                if (g_depthMsaaDescriptor == 0)
                    g_depthMsaaDescriptor = g_descriptorCounter++;
                ReplaceDescriptor(g_depthMsaaDescriptor, g_depthMsaaScratch.get());
            }
            list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(depth.texture.get(), RenderTextureLayout::SHADER_READ));
            depth.layout = RenderTextureLayout::SHADER_READ;
            list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(g_depthMsaaScratch.get(), RenderTextureLayout::COLOR_WRITE));
            list->setFramebuffer(g_depthMsaaFramebuffer.get());
            RenderViewport viewport(0.0f, 0.0f, float(width), float(height));
            list->setViewports(&viewport, 1);
            RenderRect scissor(0, 0, width, height);
            list->setScissors(&scissor, 1);
            list->setPipeline(g_depthMsaaPipeline.get());
            BindLayout(list);
            const size_t offset = ((g_rendering.data.size() + 255) & ~size_t(255)) + 512 + 256 * (g_constantSlot++ % 64);
            uint32_t* constants = reinterpret_cast<uint32_t*>(g_uploadMemory + offset);
            constants[0] = depth.descriptor;
            constants[1] = 0;
            constants[2] = uint32_t(-x0); // the shader loads at position - offset
            constants[3] = uint32_t(-y0);
            SetRootDescriptor(list, uint32_t(offset), 2);
            list->drawInstanced(3, 1, 0, 0);
            list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(g_depthMsaaScratch.get(), RenderTextureLayout::SHADER_READ));
            sourceDescriptor = g_depthMsaaDescriptor;
        }
        else
        {
        const uint32_t rowWidth = (uint32_t(width) + 63) & ~63u; // 256-byte rows
        const uint64_t size = uint64_t(rowWidth) * uint32_t(height) * 4;
        if (g_depthStagingSize < size)
        {
            if (g_depthStaging != nullptr)
                g_retiredBuffers.push_back(std::move(g_depthStaging));
            g_depthStagingSize = std::max<uint64_t>(size, 4 << 20);
            g_depthStaging = g_device->createBuffer(RenderBufferDesc::DefaultBuffer(g_depthStagingSize));
        }
        if (g_depthScratch == nullptr || g_depthScratchWidth < uint32_t(width) || g_depthScratchHeight < uint32_t(height))
        {
            if (g_depthScratch != nullptr)
                g_retiredTextures.push_back(std::move(g_depthScratch));
            g_depthScratchWidth = std::max<uint32_t>(g_depthScratchWidth, width);
            g_depthScratchHeight = std::max<uint32_t>(g_depthScratchHeight, height);
            g_depthScratch = g_device->createTexture(RenderTextureDesc::Texture2D(g_depthScratchWidth, g_depthScratchHeight, 1, RenderFormat::R32_FLOAT));
            if (g_depthScratchDescriptor == 0)
                g_depthScratchDescriptor = g_descriptorCounter++;
            ReplaceDescriptor(g_depthScratchDescriptor, g_depthScratch.get());
        }

        // 1. depth -> staging buffer, 2. staging buffer -> scratch texture.
        list->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(depth.texture.get(), RenderTextureLayout::COPY_SOURCE));
        depth.layout = RenderTextureLayout::COPY_SOURCE;
        list->barriers(RenderBarrierStage::COPY, RenderBufferBarrier(g_depthStaging.get(), RenderBufferAccess::WRITE));
        RenderBox box(x0, y0, x0 + width, y0 + height);
        list->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(g_depthStaging.get(), kDepthCopyFormat, width, height, 1, rowWidth),
            RenderTextureCopyLocation::Subresource(depth.texture.get()), 0, 0, 0, &box);
        list->barriers(RenderBarrierStage::COPY, RenderBufferBarrier(g_depthStaging.get(), RenderBufferAccess::READ));
        list->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(g_depthScratch.get(), RenderTextureLayout::COPY_DEST));
        list->copyTextureRegion(RenderTextureCopyLocation::Subresource(g_depthScratch.get()),
            RenderTextureCopyLocation::PlacedFootprint(g_depthStaging.get(), RenderFormat::R32_FLOAT, width, height, 1, rowWidth));
        list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(g_depthScratch.get(), RenderTextureLayout::SHADER_READ));
        }

        // 3. conversion pass into the destination at the resolve's destination point.
        list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(resolved.texture.get(), RenderTextureLayout::COLOR_WRITE));
        list->setFramebuffer(resolved.framebuffer.get());
        RenderViewport viewport(float(cmd.destX), float(cmd.destY), float(width), float(height));
        list->setViewports(&viewport, 1);
        RenderRect scissor(cmd.destX, cmd.destY, cmd.destX + width, cmd.destY + height);
        list->setScissors(&scissor, 1);
        list->setPipeline(g_depthPipeline.get());
        BindLayout(list);
        const size_t offset = ((g_rendering.data.size() + 255) & ~size_t(255)) + 512 + 256 * (g_constantSlot++ % 64);
        uint32_t* constants = reinterpret_cast<uint32_t*>(g_uploadMemory + offset);
        constants[0] = sourceDescriptor;
        constants[1] = cmd.resolveDepthFlipped ? 1 : 0;
        constants[2] = uint32_t(cmd.destX);
        constants[3] = uint32_t(cmd.destY);
        SetRootDescriptor(list, uint32_t(offset), 2);
        list->drawInstanced(3, 1, 0, 0);
        list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(resolved.texture.get(), RenderTextureLayout::SHADER_READ));
        resolved.lastFrame = g_frameIndex;
        if (g_dumpingFrame)
        {
            QueueDump(list, resolved.texture.get(), RenderFormat::R32_FLOAT, resolved.width, resolved.height,
                fmt::format("dump/frame{:05}_depthresolve{:02}_{:08X}_{}x{}_at{}.bmp", g_frameIndex, g_resolveIndex++, cmd.destAddress,
                    resolved.width, resolved.height, cmd.destY));
            list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(resolved.texture.get(), RenderTextureLayout::SHADER_READ));
        }
    }

    // Resolve through the fixed-point view of the HDR buffer: the destination gets the 7e3 bits the
    // console would copy (as 16-bit unorm), which the game's shaders decode themselves.
    void ReplayEncodeResolve(RenderCommandList* list, const Command& cmd, HostTarget& target, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
    {
        if (g_encodePipeline == nullptr) // not made (Renderer::Init logs it)
            return;
        const int32_t width = std::min(x1 - x0, int32_t(cmd.destWidth) - cmd.destX);
        const int32_t height = std::min(y1 - y0, int32_t(cmd.destHeight) - cmd.destY);
        if (width <= 0 || height <= 0 || cmd.destX < 0 || cmd.destY < 0)
            return;

        auto& slot = g_resolveTextures[cmd.destKey];
        if (slot == nullptr)
            slot = std::make_unique<ResolveTexture>();
        auto& resolved = *slot;
        if (resolved.texture == nullptr || resolved.framebuffer == nullptr || resolved.width != cmd.destWidth ||
            resolved.height != cmd.destHeight || resolved.format != RenderFormat::R16G16B16A16_UNORM)
        {
            if (resolved.texture != nullptr)
                g_retiredTextures.push_back(std::move(resolved.texture));
            if (resolved.framebuffer != nullptr)
                g_retiredFramebuffers.push_back(std::move(resolved.framebuffer));
            resolved.texture = g_device->createTexture(RenderTextureDesc::ColorTarget(cmd.destWidth, cmd.destHeight, RenderFormat::R16G16B16A16_UNORM));
            resolved.width = cmd.destWidth;
            resolved.height = cmd.destHeight;
            resolved.format = RenderFormat::R16G16B16A16_UNORM;
            resolved.address = cmd.destAddress;
            resolved.descriptor = cmd.destDescriptor;
            const RenderTexture* color = resolved.texture.get();
            RenderFramebufferDesc fb;
            fb.colorAttachments = &color;
            fb.colorAttachmentsCount = 1;
            resolved.framebuffer = g_device->createFramebuffer(fb);
            ReplaceDescriptor(resolved.descriptor, resolved.texture.get());
        }

        // The pass reads the target's single-sample image (target.descriptor): MSAA samples averaged.
        RenderTexture* source = SingleSampled(list, target, x0, y0, x0 + width, y0 + height, RenderTextureLayout::SHADER_READ);
        if (g_dumpingFrame)
        {
            QueueDump(list, source, target.format, target.width, target.height,
                fmt::format("dump/frame{:05}_resolve{:02}_{:08X}_encoded_source_{}x{}.bmp", g_frameIndex, g_resolveIndex,
                    cmd.destAddress, target.width, target.height));
            list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(source, RenderTextureLayout::SHADER_READ));
            SetSingleSampledLayout(target, RenderTextureLayout::SHADER_READ);
        }
        list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(resolved.texture.get(), RenderTextureLayout::COLOR_WRITE));
        list->setFramebuffer(resolved.framebuffer.get());
        RenderViewport viewport(float(cmd.destX), float(cmd.destY), float(width), float(height));
        list->setViewports(&viewport, 1);
        RenderRect scissor(cmd.destX, cmd.destY, cmd.destX + width, cmd.destY + height);
        list->setScissors(&scissor, 1);
        list->setPipeline(g_encodePipeline.get());
        BindLayout(list);
        const size_t offset = ((g_rendering.data.size() + 255) & ~size_t(255)) + 512 + 256 * (g_constantSlot++ % 64);
        uint32_t* constants = reinterpret_cast<uint32_t*>(g_uploadMemory + offset);
        constants[0] = target.descriptor;
        constants[1] = 0;
        constants[2] = uint32_t(cmd.destX - x0);
        constants[3] = uint32_t(cmd.destY - y0);
        SetRootDescriptor(list, uint32_t(offset), 2);
        list->drawInstanced(3, 1, 0, 0);
        list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(resolved.texture.get(), RenderTextureLayout::SHADER_READ));
        resolved.lastFrame = g_frameIndex;
        g_resolveIndex++;
    }

    // EDRAM -> texture: copies the source rectangle of the host target into the host copy of the
    // destination texture, then clears the rectangle if the resolve asks for it.
    void ReplayResolve(RenderCommandList* list, const Command& guestCmd, HostTarget& target)
    {
        // Predicated tiling: the EDRAM surface holds one tile, the source rectangle is in tile space
        // and the destination point is where the tile lies on screen. The host renders the whole
        // screen at once, so a resolve from a rectangle inside the (tile-sized) surface into a bigger
        // texture reads the screen area at the destination point.
        int32_t offsetX = 0, offsetY = 0;
        const bool tiled = guestCmd.sourceRect[2] <= int32_t(guestCmd.surfaceWidth) && guestCmd.sourceRect[3] <= int32_t(guestCmd.surfaceHeight) &&
            (guestCmd.destHeight > guestCmd.surfaceHeight || guestCmd.destWidth > guestCmd.surfaceWidth);
        if (tiled)
        {
            offsetX = guestCmd.destX - guestCmd.sourceRect[0];
            offsetY = guestCmd.destY - guestCmd.sourceRect[1];
        }
        // Host pixels: the pass's resolution scale (PassScale) for the source rectangle and the
        // destination (its texture is created at the same scale; draws learn it from LatestResolve).
        const float scale = PassScale(guestCmd.width, guestCmd.height, g_replayScale);
        Command cmd = guestCmd;
        cmd.destX = ScaleEdge(guestCmd.destX, scale);
        cmd.destY = ScaleEdge(guestCmd.destY, scale);
        cmd.destWidth = uint32_t(ScaleEdge(int32_t(guestCmd.destWidth), scale));
        cmd.destHeight = uint32_t(ScaleEdge(int32_t(guestCmd.destHeight), scale));
        const int32_t x0 = std::clamp(ScaleEdge(cmd.sourceRect[0] + offsetX, scale), 0, int32_t(target.width));
        const int32_t y0 = std::clamp(ScaleEdge(cmd.sourceRect[1] + offsetY, scale), 0, int32_t(target.height));
        const int32_t x1 = std::clamp(ScaleEdge(cmd.sourceRect[2] + offsetX, scale), x0, int32_t(target.width));
        const int32_t y1 = std::clamp(ScaleEdge(cmd.sourceRect[3] + offsetY, scale), y0, int32_t(target.height));
        const bool fromDepth = (cmd.resolveFlags & 7) == 4;

        // On by default (the EDRAM restore and the shadow maps read them); DR_NO_DEPTH_RESOLVE=1 skips them.
        static const bool noDepthResolves = getenv("DR_NO_DEPTH_RESOLVE") != nullptr;
        if (fromDepth && !noDepthResolves && target.depth != nullptr && g_depthPipeline != nullptr && cmd.destAddress != 0 && cmd.destWidth != 0 && cmd.destHeight != 0)
            ReplayDepthResolve(list, cmd, target, x0, y0, x1, y1);

        if (!fromDepth && cmd.encode7e3 && g_encodePipeline != nullptr && target.color != nullptr && cmd.destAddress != 0 &&
            cmd.destWidth != 0 && cmd.destHeight != 0)
            ReplayEncodeResolve(list, cmd, target, x0, y0, x1, y1);
        else if (!fromDepth && target.color != nullptr && cmd.destAddress != 0 && cmd.destWidth != 0 && cmd.destHeight != 0)
        {
            auto& slot = g_resolveTextures[cmd.destKey];
            if (slot == nullptr)
                slot = std::make_unique<ResolveTexture>();
            auto& resolved = *slot;
            const uint32_t levels = std::max(cmd.destLevels, 1u);
            if (resolved.texture == nullptr || resolved.width != cmd.destWidth || resolved.height != cmd.destHeight || resolved.format != target.format ||
                resolved.levels != levels || resolved.cube != cmd.destCube)
            {
                if (resolved.texture != nullptr)
                    g_retiredTextures.push_back(std::move(resolved.texture));
                if (resolved.view != nullptr)
                    g_retiredViews.push_back(std::move(resolved.view));
                RenderTextureDesc desc = RenderTextureDesc::Texture2D(cmd.destWidth, cmd.destHeight, levels, target.format);
                if (cmd.destCube)
                {
                    desc.arraySize = 6;
                    desc.flags = RenderTextureFlag::CUBE;
                }
                resolved.texture = g_device->createTexture(desc);
                RenderTextureViewDesc view;
                view.format = target.format;
                view.dimension = cmd.destCube ? RenderTextureViewDimension::TEXTURE_CUBE : RenderTextureViewDimension::TEXTURE_2D;
                view.mipLevels = levels;
                resolved.view = resolved.texture->createTextureView(view);
                resolved.width = cmd.destWidth;
                resolved.height = cmd.destHeight;
                resolved.levels = levels;
                resolved.cube = cmd.destCube;
                resolved.format = target.format;
                resolved.address = cmd.destAddress;
                resolved.descriptor = cmd.destDescriptor;
                ReplaceDescriptor(resolved.descriptor, resolved.texture.get(), resolved.view.get());
            }

            const int32_t levelWidth = int32_t(std::max(resolved.width >> cmd.destLevel, 1u));
            const int32_t levelHeight = int32_t(std::max(resolved.height >> cmd.destLevel, 1u));
            const int32_t width = std::min(x1 - x0, levelWidth - cmd.destX);
            const int32_t height = std::min(y1 - y0, levelHeight - cmd.destY);
            if (width > 0 && height > 0 && cmd.destX >= 0 && cmd.destY >= 0)
            {
                // MSAA targets: the samples averaged into the single-sample copy, copied from there.
                RenderTexture* source = SingleSampled(list, target, x0, y0, x0 + width, y0 + height, RenderTextureLayout::COPY_SOURCE);
                if (g_dumpingFrame && tiled)
                {
                    QueueDump(list, source, target.format, target.width, target.height,
                        fmt::format("dump/frame{:05}_resolve{:02}_source_{}x{}_box{}-{}.bmp", g_frameIndex, g_resolveIndex,
                            target.width, target.height, y0, y0 + height));
                    list->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(source, RenderTextureLayout::COPY_SOURCE));
                    SetSingleSampledLayout(target, RenderTextureLayout::COPY_SOURCE);
                }
                list->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(resolved.texture.get(), RenderTextureLayout::COPY_DEST));
                RenderBox box(x0, y0, x0 + width, y0 + height);
                list->copyTextureRegion(RenderTextureCopyLocation::Subresource(resolved.texture.get(), cmd.destLevel, cmd.destSlice),
                    RenderTextureCopyLocation::Subresource(source), uint32_t(cmd.destX), uint32_t(cmd.destY), 0, &box);
                list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(resolved.texture.get(), RenderTextureLayout::SHADER_READ));
                resolved.lastFrame = g_frameIndex;
                // Write-back in guest texels (a scaled copy is averaged down to them).
                const int32_t guestWidth = scale == 1.0f ? width
                    : std::min(guestCmd.sourceRect[2] - guestCmd.sourceRect[0], int32_t(guestCmd.destWidth) - guestCmd.destX);
                const int32_t guestHeight = scale == 1.0f ? height
                    : std::min(guestCmd.sourceRect[3] - guestCmd.sourceRect[1], int32_t(guestCmd.destHeight) - guestCmd.destY);
                if (cmd.destLevel == 0 && cmd.destSlice == 0 && !resolved.cube &&
                    resolved.format == RenderFormat::R8G8B8A8_UNORM && (cmd.destFetch[1] & 0x3F) == 6 &&
                    guestCmd.destWidth <= 256 && guestCmd.destHeight <= 256 && guestWidth > 0 && guestHeight > 0 &&
                    guestWidth <= 256 && guestHeight <= 256)
                    QueueWriteback(list, resolved.texture.get(), guestCmd, uint32_t(guestWidth), uint32_t(guestHeight), scale);
                if (g_dumpingFrame)
                {
                    // The level and face this resolve wrote (environment cubes are filled level by level).
                    QueueDump(list, resolved.texture.get(), resolved.format, uint32_t(levelWidth), uint32_t(levelHeight),
                        fmt::format("dump/frame{:05}_resolve{:02}_{:08X}_{}x{}_L{}_F{}_flags{:X}.bmp", g_frameIndex, g_resolveIndex, cmd.destAddress,
                            resolved.width, resolved.height, cmd.destLevel, cmd.destSlice, cmd.resolveFlags), cmd.destLevel, cmd.destSlice);
                    list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(resolved.texture.get(), RenderTextureLayout::SHADER_READ));
                }
                g_resolveIndex++;
                if (guestCmd.destWidth >= 1280 && guestCmd.destHeight >= 720)
                    g_shownResolve = &resolved;

                // The tiled resolves copy the finished 3D scene out of EDRAM (before post-processing,
                // which writes into the same texture afterwards): keep a snapshot of them to show. Only the HDR
                // scene: a later 8-bit resolve has the same shape.
                if (tiled && target.format == RenderFormat::R16G16B16A16_FLOAT && guestCmd.destWidth >= 1280 && guestCmd.destHeight >= 720)
                {
                    auto& snapshot = g_sceneSnapshot;
                    if (snapshot.texture == nullptr || snapshot.width != resolved.width || snapshot.height != resolved.height || snapshot.format != target.format)
                    {
                        if (snapshot.texture != nullptr)
                            g_retiredTextures.push_back(std::move(snapshot.texture));
                        snapshot.texture = g_device->createTexture(RenderTextureDesc::Texture2D(resolved.width, resolved.height, 1, target.format));
                        snapshot.width = resolved.width;
                        snapshot.height = resolved.height;
                        snapshot.format = target.format;
                        if (snapshot.descriptor == 0)
                            snapshot.descriptor = g_descriptorCounter++;
                        ReplaceDescriptor(snapshot.descriptor, snapshot.texture.get());
                    }
                    list->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(snapshot.texture.get(), RenderTextureLayout::COPY_DEST));
                    list->copyTextureRegion(RenderTextureCopyLocation::Subresource(snapshot.texture.get()),
                        RenderTextureCopyLocation::Subresource(source), uint32_t(cmd.destX), uint32_t(cmd.destY), 0, &box);
                    list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(snapshot.texture.get(), RenderTextureLayout::SHADER_READ));
                    snapshot.lastFrame = g_frameIndex;
                }
            }
        }

        // Debugging: DR_NO_RESOLVE_CLEAR=color|depth|all skips those clears.
        static const char* skipClears = getenv("DR_NO_RESOLVE_CLEAR");
        static const bool skipColor = skipClears != nullptr && (strcmp(skipClears, "depth") != 0);
        static const bool skipDepth = skipClears != nullptr && (strcmp(skipClears, "color") != 0);
        if ((cmd.resolveFlags & 0x300) != 0 && x1 > x0 && y1 > y0)
        {
            TransitionForDrawing(list, target);
            list->setFramebuffer(target.framebuffer.get());
            RenderRect rect(x0, y0, x1, y1);
            if ((cmd.resolveFlags & 0x100) && target.color != nullptr && !skipColor)
                list->clearColor(0, cmd.clearColor, &rect, 1);
            if ((cmd.resolveFlags & 0x200) && target.depth != nullptr && !skipDepth)
                list->clearDepthStencil(true, true, cmd.clearZ, cmd.clearStencil, &rect, 1);
        }
    }

    // ---- GPU profile (DR_GPU_PROFILE=1): a timestamp between runs of commands of one kind (draws
    // into one render target, a resolve, clears, uploads, the final blit), summed per kind over 300
    // frames and logged largest first. Read back when the next frame starts (the video thread
    // waits for each frame on the GPU).
    struct GpuProfiler
    {
        bool enabled = getenv("DR_GPU_PROFILE") != nullptr;
        static constexpr uint32_t kQueries = 4096;
        struct Slot
        {
            std::unique_ptr<RenderQueryPool> pool;
            uint32_t used = 0;
            std::vector<std::string> labels; // labels[i]: the run that ended at timestamp i
        } slots[kFrameSlots]; // one per frame slot: read when the slot comes round again
        std::string current;
        std::map<std::string, std::pair<double, uint32_t>> totals; // ms, runs
        double frameMs = 0.0;
        uint32_t frames = 0;

        void Write(RenderCommandList* list)
        {
            Slot& s = slots[g_frameSlot];
            if (s.used >= kQueries)
                return;
            list->writeTimestamp(s.pool.get(), s.used++);
            s.labels.push_back(current);
        }

        void Begin(RenderCommandList* list)
        {
            if (!enabled)
                return;
            Slot& s = slots[g_frameSlot];
            if (s.pool == nullptr)
                s.pool = g_device->createQueryPool(kQueries);
            s.used = 0;
            s.labels.clear();
            current = "frame start";
            list->resetQueryPool(s.pool.get(), 0, kQueries);
            Write(list);
        }

        void Mark(RenderCommandList* list, const std::string& label)
        {
            if (!enabled || slots[g_frameSlot].pool == nullptr || label == current)
                return;
            Write(list);
            current = label;
        }

        void End(RenderCommandList* list)
        {
            if (enabled && slots[g_frameSlot].pool != nullptr)
                Write(list);
        }

        // Called when a slot comes round again: its last frame has finished on the GPU.
        void Collect(uint32_t slot)
        {
            Slot& s = slots[slot];
            if (!enabled || s.pool == nullptr || s.used < 2)
                return;
            s.pool->queryResults();
            const uint64_t* ns = s.pool->getResults();
            const uint32_t used = s.used;
            for (uint32_t i = 1; i < used; i++)
            {
                auto& total = totals[s.labels[i]];
                total.first += double(ns[i] - ns[i - 1]) / 1e6;
                total.second++;
            }
            frameMs += double(ns[used - 1] - ns[0]) / 1e6;
            s.used = 0;
            if (++frames < 300)
                return;
            std::vector<std::pair<std::string, std::pair<double, uint32_t>>> sorted(totals.begin(), totals.end());
            std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
            std::string text;
            for (size_t i = 0; i < std::min<size_t>(sorted.size(), 16); i++)
                text += fmt::format("\n    {:6.2f} ms  {} (x{:.1f})", sorted[i].second.first / frames, sorted[i].first,
                    double(sorted[i].second.second) / frames);
            LOGFN("[gpu profile] {:.2f} ms per frame on the GPU (scale {}), by kind:{}", frameMs / frames, g_replayScale, text);
            totals.clear();
            frameMs = 0.0;
            frames = 0;
        }
    } g_gpuProfile;

    const char* FormatName(RenderFormat format)
    {
        switch (format)
        {
        case RenderFormat::R8G8B8A8_UNORM: return "RGBA8";
        case RenderFormat::R16G16B16A16_FLOAT: return "RGBA16F";
        case RenderFormat::R16G16B16A16_UNORM: return "RGBA16";
        case RenderFormat::R32_FLOAT: return "R32F";
        case RenderFormat::R32G32_FLOAT: return "RG32F";
        case RenderFormat::R16G16_FLOAT: return "RG16F";
        case RenderFormat::UNKNOWN: return "depth only";
        default: return "other";
        }
    }

    void Replay(RenderCommandList* list, Frame& frame)
    {
        // Frame data, plus 16 zero bytes for the dummy vertex slot and the blit constants.
        const size_t dummyOffset = (frame.data.size() + 255) & ~size_t(255);
        const size_t textureBase = TextureUploadBase(frame);
        UseUploadSlot(textureBase + frame.texturesSize); // FixedUploadEnd: the regions after the data
        memcpy(g_uploadMemory, frame.data.data(), frame.data.size());
        memset(g_uploadMemory + dummyOffset, 0, 512);
        for (size_t i = 0; i < frame.textures.size(); i++)
            memcpy(g_uploadMemory + textureBase + frame.textureOffsets[i], frame.textures[i].data(), frame.textures[i].size());
        frame.ReleaseTextures(); // in the upload buffer now; the commands keep their offsets

        // Vertex streams and index lists that became resident this frame: into the geometry buffer
        // before any draw (their regions are not in use by frames still on the GPU).
        if (!frame.geometryUploads.empty())
        {
            if (g_geometryBuffer == nullptr)
            {
                g_geometryBuffer = g_device->createBuffer(RenderBufferDesc::DefaultBuffer(kGeometryHeapSize, RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX));
                LOGFN("[memory] geometry buffer made ({} MB): {}", kGeometryHeapSize >> 20, MemoryInfo::Describe());
            }
            list->barriers(RenderBarrierStage::COPY, RenderBufferBarrier(g_geometryBuffer.get(), RenderBufferAccess::WRITE));
            for (const GeometryUpload& upload : frame.geometryUploads)
                list->copyBufferRegion(g_geometryBuffer->at(upload.destination), g_uploadBuffer->at(upload.source), upload.size);
            list->barriers(RenderBarrierStage::GRAPHICS, RenderBufferBarrier(g_geometryBuffer.get(), RenderBufferAccess::READ));
        }
        g_constantSlot = 0;

        CreateNewSamplers();
        for (auto& [key, target] : g_targets)
            target->draws = 0;
        g_frameIndex++;
        g_shownResolve = nullptr;
        g_resolveIndex = 0;
        g_replayScale = frame.scale;

        // A new resolution scale: the EDRAM model's buffers, which otherwise only grow, are made
        // again at the sizes the passes ask for (back at 1x after 3x they held about 500 MB). They
        // keep their descriptors; the textures go when this frame has finished.
        static float lastScale = 1.0f;
        if (frame.scale != lastScale)
        {
            lastScale = frame.scale;
            for (auto& [key, target] : g_targets)
            {
                if (target->color != nullptr)
                    g_retiredTextures.push_back(std::move(target->color));
                if (target->single != nullptr)
                    g_retiredTextures.push_back(std::move(target->single));
                if (target->framebuffer != nullptr)
                    g_retiredFramebuffers.push_back(std::move(target->framebuffer));
                target->width = target->height = 0;
                target->framebufferDepth = nullptr;
                target->depth = nullptr;
                target->colorLayout = target->singleLayout = RenderTextureLayout::UNKNOWN;
            }
            for (auto& [key, depth] : g_depthBuffers)
            {
                if (depth->texture != nullptr)
                    g_retiredTextures.push_back(std::move(depth->texture));
                if (depth->view != nullptr)
                    g_retiredViews.push_back(std::move(depth->view));
                depth->width = depth->height = 0;
                depth->layout = RenderTextureLayout::UNKNOWN;
            }
            g_shownTarget = nullptr;
            LOGFN("[renderer] resolution scale {}: render targets made again", frame.scale);
        }

        {
            // Resolve textures the guest registered: white until a resolve fills them.
            std::lock_guard lock(g_resolveMutex);
            for (uint32_t descriptor : g_newResolveDescriptors)
                g_textureSet->setTexture(descriptor, g_placeholders[0].get(), RenderTextureLayout::SHADER_READ, g_placeholderViews[0].get());
            g_newResolveDescriptors.clear();

            // Evicted textures: free the host copies; the descriptors go back to the guest once this
            // frame's slot comes round (the frame in flight may still read them).
            for (uint32_t descriptor : g_releasedDescriptors)
            {
                if (auto it = g_hostTextures.find(descriptor); it != g_hostTextures.end())
                {
                    if (it->second.texture != nullptr)
                        g_retiredTextures.push_back(std::move(it->second.texture));
                    if (it->second.view != nullptr)
                        g_retiredViews.push_back(std::move(it->second.view));
                    g_hostTextures.erase(it);
                }
                g_releasedThisFrame.push_back(descriptor);
            }
            g_releasedDescriptors.clear();
        }

        BindLayout(list);

        g_gpuProfile.Begin(list);
        HostTarget* current = nullptr;
        for (auto& cmd : frame.commands)
        {
            if (cmd.type == Command::Upload)
            {
                g_gpuProfile.Mark(list, "texture uploads");
                ReplayUpload(list, frame, cmd);
                current = nullptr; // copies end the render pass
                continue;
            }
            if (cmd.colorKey == 0 && cmd.depthKey == 0)
                continue;
            bool recreated = false;
            // Host buffers are storage for every pass that uses them: each pass takes its own scale
            // of the guest size (PassScale), the buffer grows to the largest.
            const float passScale = PassScale(cmd.width, cmd.height, frame.scale);
            HostTarget& target = GetTarget(cmd.colorKey, cmd.depthKey, uint32_t(ScaleEdge(int32_t(cmd.width), passScale)),
                uint32_t(ScaleEdge(int32_t(cmd.height), passScale)), cmd.colorFormat, cmd.samples, recreated);

            if (g_gpuProfile.enabled)
            {
                if (cmd.type == Command::Resolve)
                    g_gpuProfile.Mark(list, fmt::format("resolve {} {}x{} from {}x{} {}x", (cmd.resolveFlags & 7) == 4 ? "depth" : cmd.encode7e3 ? "7e3" : "colour",
                        cmd.destWidth, cmd.destHeight, cmd.width, cmd.height, cmd.samples));
                else if (cmd.type == Command::Clear)
                    g_gpuProfile.Mark(list, "clears");
                else
                    g_gpuProfile.Mark(list, fmt::format("draws into {}x{} {} {}x (key {:X})", ScaleEdge(int32_t(cmd.width), passScale),
                        ScaleEdge(int32_t(cmd.height), passScale), FormatName(cmd.colorFormat), cmd.samples, cmd.colorKey));
            }

            if (cmd.type == Command::Resolve)
            {
                ReplayResolve(list, cmd, target);
                current = nullptr; // layouts changed; bind the framebuffer again before drawing
                continue;
            }

            if (&target != current || recreated)
            {
                TransitionForDrawing(list, target);
                list->setFramebuffer(target.framebuffer.get());
                current = &target;
            }

            if (cmd.type == Command::Clear)
            {
                // Xbox 360 D3DCLEAR flags (Clear, sub_82458B10): bits 0-3 render targets 0-3,
                // 0x10 depth, 0x20 stencil (not PC D3D9's 1 / 2 / 4).
                if ((cmd.clearFlags & 0xF) && target.color != nullptr)
                    list->clearColor(0, cmd.clearColor);
                if ((cmd.clearFlags & 0x30) && target.depth != nullptr)
                    list->clearDepthStencil((cmd.clearFlags & 0x10) != 0, (cmd.clearFlags & 0x20) != 0, cmd.clearZ, cmd.clearStencil);
                continue;
            }

            // A target bound before may have been resolved from in between (copy layouts).
            TransitionForDrawing(list, target);

            // Debugging: DR_DRAW_RANGE=<first>-<last> replays only those draws (command indices).
            static const std::pair<size_t, size_t> drawRange = []
            {
                std::pair<size_t, size_t> range{ 0, SIZE_MAX };
                if (const char* text = getenv("DR_DRAW_RANGE"))
                {
                    char* end;
                    range.first = strtoull(text, &end, 10);
                    range.second = *end == '-' ? strtoull(end + 1, nullptr, 10) : range.first;
                }
                return range;
            }();
            // DR_DRAW_SKIP=<first>-<last> leaves those out.
            static const std::pair<size_t, size_t> skipRange = []
            {
                std::pair<size_t, size_t> range{ SIZE_MAX, 0 };
                if (const char* text = getenv("DR_DRAW_SKIP"))
                {
                    char* end;
                    range.first = strtoull(text, &end, 10);
                    range.second = *end == '-' ? strtoull(end + 1, nullptr, 10) : range.first;
                }
                return range;
            }();
            const size_t commandIndex = size_t(&cmd - frame.commands.data()) - frame.leadingUploads; // as DR_DRAW_LOG numbers them
            if (commandIndex < drawRange.first || commandIndex > drawRange.second ||
                (commandIndex >= skipRange.first && commandIndex <= skipRange.second))
                continue;

            RenderPipeline* pipeline = GetPipeline(cmd.key);
            if (pipeline == nullptr)
            {
                g_replayStats.skipped++;
                continue;
            }

            list->setPipeline(pipeline);
            BindLayout(list);
            SetRootDescriptor(list, cmd.vertexConstants, 0);
            SetRootDescriptor(list, cmd.pixelConstants, 1);
            SetRootDescriptor(list, cmd.sharedConstants, 2);

            RenderViewport viewport = cmd.viewport;
            if (viewport.width <= 0.0f || viewport.height <= 0.0f)
                viewport = RenderViewport(0.0f, 0.0f, float(target.width), float(target.height));
            else if (passScale != 1.0f)
            {
                viewport.x *= passScale;
                viewport.y *= passScale;
                viewport.width *= passScale;
                viewport.height *= passScale;
            }
            list->setViewports(&viewport, 1);
            RenderRect scissor(0, 0, int32_t(target.width), int32_t(target.height));
            list->setScissors(&scissor, 1);

            for (uint32_t n = 0; n < kMaxStreams; n++)
                if (cmd.streamMask & (1u << n))
                {
                    RenderBuffer* source = (cmd.residentStreams & (1u << n)) != 0 ? g_geometryBuffer.get() : g_uploadBuffer;
                    RenderVertexBufferView view(source->at(cmd.streamOffset[n]), cmd.streamSize[n]);
                    RenderInputSlot slot(n, cmd.key.strides[n]);
                    list->setVertexBuffers(n, &view, 1, &slot);
                }
            RenderVertexBufferView dummy(g_uploadBuffer->at(dummyOffset), 16);
            RenderInputSlot dummySlot(kDummySlot, 0);
            list->setVertexBuffers(kDummySlot, &dummy, 1, &dummySlot);

            if (cmd.indexed)
            {
                RenderIndexBufferView view((cmd.residentIndices ? g_geometryBuffer.get() : g_uploadBuffer)->at(cmd.indexOffset), cmd.indexSize, RenderFormat::R32_UINT);
                list->setIndexBuffer(&view);
                list->drawIndexedInstanced(cmd.count, 1, 0, 0, 0);
            }
            else
            {
                list->drawInstanced(cmd.count, 1, 0, 0);
            }

            target.draws++;
            g_replayStats.draws++;

            // Debugging: DR_DUMP_DRAWS=<first>-<last> (command indices as in the draw log) dumps the
            // colour target after each of those draws in the dump frame.
            static const std::pair<size_t, size_t> dumpDraws = []
            {
                std::pair<size_t, size_t> range{ SIZE_MAX, 0 };
                if (const char* text = getenv("DR_DUMP_DRAWS"))
                {
                    char* end;
                    range.first = strtoull(text, &end, 10);
                    range.second = *end == '-' ? strtoull(end + 1, nullptr, 10) : range.first;
                }
                return range;
            }();
            if (g_dumpingFrame && commandIndex >= dumpDraws.first && commandIndex <= dumpDraws.second && target.color != nullptr)
            {
                QueueDump(list, SingleSampled(list, target, 0, 0, int32_t(target.width), int32_t(target.height), RenderTextureLayout::COPY_SOURCE),
                    target.format, target.width, target.height,
                    fmt::format("dump/frame{:05}_draw{:04}_{}x{}.bmp", g_frameIndex, commandIndex, target.width, target.height));
                SetSingleSampledLayout(target, RenderTextureLayout::COPY_SOURCE);
                current = nullptr; // back to drawing: transition and set the framebuffer again
            }
        }

        // Show the colour target the frame drew into most (the scene); resolves are not followed yet.
        g_shownTarget = nullptr;
        for (auto& [key, target] : g_targets)
            if (target->color != nullptr && target->width >= 640 && target->draws > 0 &&
                (g_shownTarget == nullptr || target->draws > g_shownTarget->draws))
                g_shownTarget = target.get();
    }

    // ---- Resolve write-back: the console's CPU reads resolved textures from memory (the game copies
    // its luminance downsample to textures it samples for auto exposure). Small 8-bit resolves are
    // read back and written into guest memory in the texture's tiled layout and byte order, a frame
    // or two late (enough for exposure adaptation).

    struct PendingWriteback
    {
        std::unique_ptr<RenderBuffer> buffer;
        uint32_t address = 0, x = 0, y = 0, width = 0, height = 0, rowWidth = 0; // guest texels
        float scale = 1.0f;                                                     // host texels per guest texel
        uint64_t bufferSize = 0;
        uint32_t hostX = 0, hostY = 0;                                          // where the copy starts
        TextureDecode::FetchConstant fetch{};
    };
    std::vector<PendingWriteback> g_pendingWritebacks;
    std::vector<std::unique_ptr<RenderBuffer>> g_writebackPool;
    uint64_t g_writebackBufferSize = 0; // every pooled buffer's size

    void QueueWriteback(RenderCommandList* list, RenderTexture* texture, const Command& cmd, uint32_t width, uint32_t height, float scale)
    {
        static const bool disabled = getenv("DR_NO_WRITEBACK") != nullptr;
        if (disabled)
            return;
        PendingWriteback wb;
        wb.address = cmd.destAddress;
        wb.x = uint32_t(cmd.destX);
        wb.y = uint32_t(cmd.destY);
        wb.width = width;
        wb.height = height;
        wb.scale = scale;
        // The texture holds the rectangle at the resolution scale: copy all of it (averaged later).
        wb.hostX = uint32_t(ScaleEdge(int32_t(wb.x), scale));
        wb.hostY = uint32_t(ScaleEdge(int32_t(wb.y), scale));
        const uint32_t hostWidth = uint32_t(ScaleEdge(int32_t(wb.x + width), scale)) - wb.hostX;
        const uint32_t hostHeight = uint32_t(ScaleEdge(int32_t(wb.y + height), scale)) - wb.hostY;
        wb.rowWidth = (hostWidth + 63) & ~63u;
        memcpy(wb.fetch.dword, cmd.destFetch, sizeof(wb.fetch.dword));
        // Pooled buffers hold the largest rectangle written back (256x256 guest texels) at the
        // current scale; a larger scale replaces them.
        const uint64_t needed = uint64_t(wb.rowWidth) * hostHeight * 4;
        const uint64_t poolSize = std::max<uint64_t>(uint64_t((ScaleEdge(256, scale) + 63) & ~63) * uint64_t(ScaleEdge(256, scale) + 1) * 4, needed);
        if (poolSize > g_writebackBufferSize)
        {
            g_writebackPool.clear();
            g_writebackBufferSize = poolSize;
        }
        if (!g_writebackPool.empty())
        {
            wb.buffer = std::move(g_writebackPool.back());
            g_writebackPool.pop_back();
        }
        else
            wb.buffer = g_device->createBuffer(RenderBufferDesc::ReadbackBuffer(g_writebackBufferSize));
        wb.bufferSize = g_writebackBufferSize;

        list->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(texture, RenderTextureLayout::COPY_SOURCE));
        RenderBox box(int32_t(wb.hostX), int32_t(wb.hostY), int32_t(wb.hostX + hostWidth), int32_t(wb.hostY + hostHeight));
        list->copyTextureRegion(
            RenderTextureCopyLocation::PlacedFootprint(wb.buffer.get(), RenderFormat::R8G8B8A8_UNORM, hostWidth, hostHeight, 1, wb.rowWidth),
            RenderTextureCopyLocation::Subresource(texture), 0, 0, 0, &box);
        list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(texture, RenderTextureLayout::SHADER_READ));
        g_pendingWritebacks.push_back(std::move(wb));
    }

    // Called once the frame that queued them has finished on the GPU.
    void WriteBackResolves(std::vector<PendingWriteback>& pending)
    {
        for (auto& wb : pending)
        {
            const uint8_t* src = static_cast<const uint8_t*>(wb.buffer->map());
            if (src == nullptr)
            {
                // Mapping can fail (low memory): skip this write-back, the next frame's follows.
                static uint32_t logged;
                if (logged++ < 4)
                    LOGFN_WARNING("[renderer] resolve write-back: readback buffer could not be mapped, skipped");
                if (wb.bufferSize == g_writebackBufferSize)
                    g_writebackPool.push_back(std::move(wb.buffer));
                continue;
            }
            const uint32_t endian = wb.fetch.Endian();
            Renderer::CheckGuestWrite("resolve write-back", wb.address, TextureDecode::GuestSize(wb.fetch));
            for (uint32_t y = 0; y < wb.height; y++)
                for (uint32_t x = 0; x < wb.width; x++)
                {
                    uint32_t texel;
                    if (wb.scale == 1.0f)
                        memcpy(&texel, src + (size_t(y) * wb.rowWidth + x) * 4, 4);
                    else
                    {
                        // The guest texel's block of host texels, averaged (the resolution scale's
                        // supersampling, as the CPU would see the console's texel).
                        const uint32_t hx0 = uint32_t(ScaleEdge(int32_t(wb.x + x), wb.scale)) - wb.hostX;
                        const uint32_t hx1 = std::max(uint32_t(ScaleEdge(int32_t(wb.x + x + 1), wb.scale)) - wb.hostX, hx0 + 1);
                        const uint32_t hy0 = uint32_t(ScaleEdge(int32_t(wb.y + y), wb.scale)) - wb.hostY;
                        const uint32_t hy1 = std::max(uint32_t(ScaleEdge(int32_t(wb.y + y + 1), wb.scale)) - wb.hostY, hy0 + 1);
                        uint32_t sum[4] = {};
                        for (uint32_t hy = hy0; hy < hy1; hy++)
                            for (uint32_t hx = hx0; hx < hx1; hx++)
                                for (uint32_t c = 0; c < 4; c++)
                                    sum[c] += src[(size_t(hy) * wb.rowWidth + hx) * 4 + c];
                        const uint32_t count = (hx1 - hx0) * (hy1 - hy0);
                        texel = 0;
                        for (uint32_t c = 0; c < 4; c++)
                            texel |= ((sum[c] + count / 2) / count) << (8 * c);
                    }
                    // Host RGBA8 (R in the low byte) -> the guest word the GPU swaps back into it.
                    if (endian == 2)
                        texel = __builtin_bswap32(texel);
                    else if (endian == 1)
                        texel = ((texel & 0x00FF00FFu) << 8) | ((texel >> 8) & 0x00FF00FFu);
                    else if (endian == 3)
                        texel = (texel >> 16) | (texel << 16);
                    const uint32_t offset = TextureDecode::TexelOffset(wb.fetch, wb.x + x, wb.y + y, 4);
                    memcpy(g_memory.Translate(wb.address + offset), &texel, 4);
                }
            wb.buffer->unmap();
            if (wb.bufferSize == g_writebackBufferSize) // older, smaller buffers are dropped
                g_writebackPool.push_back(std::move(wb.buffer));
        }
        pending.clear();
    }

    // ---- Frame dumps (DR_DUMP=<every N frames>): every colour target the frame drew into, as BMP
    // files in dump/ (the run folder), to inspect the replay without a screen.

    struct PendingDump
    {
        std::unique_ptr<RenderBuffer> buffer;
        uint32_t width = 0, height = 0, rowWidth = 0, bytesPerPixel = 0;
        RenderFormat format = RenderFormat::UNKNOWN;
        std::string path;
    };
    std::vector<PendingDump> g_pendingDumps;
    uint32_t g_dumpInterval = 0;
    uint32_t g_replayedFrames = 0;

    uint32_t BytesPerPixel(RenderFormat format)
    {
        switch (format)
        {
        case RenderFormat::R16G16B16A16_FLOAT: case RenderFormat::R16G16B16A16_UNORM: case RenderFormat::R32G32_FLOAT: return 8;
        case RenderFormat::R32G32B32A32_FLOAT: return 16;
        default: return 4;
        }
    }

    float HalfToFloat(uint16_t h)
    {
        const uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
        float value = exponent == 0 ? ldexpf(float(mantissa), -24) : exponent == 31 ? 65504.0f : ldexpf(float(mantissa | 0x400), int(exponent) - 25);
        return sign ? -value : value;
    }

    void QueueDump(RenderCommandList* list, RenderTexture* texture, RenderFormat format, uint32_t width, uint32_t height, std::string path,
        uint32_t level, uint32_t slice)
    {
        PendingDump dump;
        dump.width = width;
        dump.height = height;
        dump.format = format;
        dump.bytesPerPixel = BytesPerPixel(format);
        const uint32_t align = 256 / dump.bytesPerPixel;
        dump.rowWidth = (dump.width + align - 1) / align * align;
        dump.buffer = g_device->createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(dump.rowWidth) * dump.height * dump.bytesPerPixel));
        dump.path = std::move(path);

        list->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(texture, RenderTextureLayout::COPY_SOURCE));
        list->copyTextureRegion(
            RenderTextureCopyLocation::PlacedFootprint(dump.buffer.get(), dump.format, dump.width, dump.height, 1, dump.rowWidth),
            RenderTextureCopyLocation::Subresource(texture, level, slice));
        g_pendingDumps.push_back(std::move(dump));
    }

    void QueueDumps(RenderCommandList* list)
    {
        if (g_sceneSnapshot.texture != nullptr && g_sceneSnapshot.lastFrame == g_frameIndex)
        {
            QueueDump(list, g_sceneSnapshot.texture.get(), g_sceneSnapshot.format, g_sceneSnapshot.width, g_sceneSnapshot.height,
                fmt::format("dump/frame{:05}_scene_{}x{}.bmp", g_frameIndex, g_sceneSnapshot.width, g_sceneSnapshot.height));
            list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(g_sceneSnapshot.texture.get(), RenderTextureLayout::SHADER_READ));
        }

        for (auto& [key, target] : g_targets)
        {
            if (target->color == nullptr || target->draws == 0)
                continue;
            QueueDump(list, SingleSampled(list, *target, 0, 0, int32_t(target->width), int32_t(target->height), RenderTextureLayout::COPY_SOURCE),
                target->format, target->width, target->height,
                fmt::format("dump/frame{:05}_target_{:08X}_{}_{}x{}_{}draws.bmp", g_replayedFrames, uint32_t(key >> 32), uint32_t(key), target->width, target->height, target->draws));
            SetSingleSampledLayout(*target, RenderTextureLayout::COPY_SOURCE);
        }
    }

    // Called once the frame with the copies has finished on the GPU.
    void WriteDumps(std::vector<PendingDump>& pending)
    {
        if (pending.empty())
            return;
        std::filesystem::create_directories("dump");

        for (auto& dump : pending)
        {
            const uint8_t* src = static_cast<const uint8_t*>(dump.buffer->map());
            if (src == nullptr)
                continue; // could not be mapped (low memory): no file for this one

            // Vertex textures (bone matrices): a few texels of rows 1 and 2, as the GPU has them.
            if (dump.format == RenderFormat::R32G32B32A32_FLOAT)
            {
                for (uint32_t row : { 1u, 2u })
                    for (uint32_t bone : { 0u, 44u, 45u })
                    {
                        std::string text;
                        for (uint32_t t = 0; t < 3 && row < dump.height; t++)
                        {
                            const float* v = reinterpret_cast<const float*>(src + (size_t(row) * dump.rowWidth + bone * 3 + t) * 16);
                            text += fmt::format(" ({:.4g} {:.4g} {:.4g} {:.4g})", v[0], v[1], v[2], v[3]);
                        }
                        LOGFN("[renderer] dump {}: row {} bone {}:{}", dump.path, row, bone, text);
                    }
                dump.buffer->unmap();
                continue;
            }

            // Single-channel float (depth): stretched to its range, logged.
            float rangeMin = 0.0f, rangeScale = 1.0f;
            if (dump.format == RenderFormat::R32_FLOAT)
            {
                float lo = FLT_MAX, hi = -FLT_MAX;
                for (uint32_t y = 0; y < dump.height; y++)
                    for (uint32_t x = 0; x < dump.width; x++)
                    {
                        const float v = reinterpret_cast<const float*>(src + size_t(y) * dump.rowWidth * 4)[x];
                        if (std::isfinite(v)) { lo = std::min(lo, v); hi = std::max(hi, v); }
                    }
                if (hi > lo)
                {
                    rangeMin = lo;
                    rangeScale = 1.0f / (hi - lo);
                }
                LOGFN("[renderer] dump {}: range {} .. {}", dump.path, lo, hi);
            }
            // Half-float images: count infinities / NaNs and report the largest finite value.
            if (dump.format == RenderFormat::R16G16B16A16_FLOAT)
            {
                uint32_t nonFinite = 0, firstX = 0, firstY = 0;
                float largest = 0.0f;
                for (uint32_t y = 0; y < dump.height; y++)
                    for (uint32_t x = 0; x < dump.width; x++)
                        for (uint32_t c = 0; c < 4; c++)
                        {
                            const uint16_t h = reinterpret_cast<const uint16_t*>(src + (size_t(y) * dump.rowWidth + x) * 8)[c];
                            if (((h >> 10) & 0x1F) == 0x1F)
                            {
                                if (nonFinite++ == 0) { firstX = x; firstY = y; }
                            }
                            else
                                largest = std::max(largest, HalfToFloat(h));
                        }
                LOGFN("[renderer] dump {}: {} non-finite (first at {},{}), largest {}", dump.path, nonFinite, firstX, firstY, largest);
            }
            const uint32_t rowBytes = (dump.width * 3 + 3) & ~3u;
            std::vector<uint8_t> bmp(54 + size_t(rowBytes) * dump.height);
            auto put32 = [&](size_t at, uint32_t v) { memcpy(&bmp[at], &v, 4); };
            bmp[0] = 'B'; bmp[1] = 'M';
            put32(2, uint32_t(bmp.size()));
            put32(10, 54);
            put32(14, 40);
            put32(18, dump.width);
            put32(22, uint32_t(-int32_t(dump.height))); // top-down
            bmp[26] = 1; bmp[28] = 24;

            for (uint32_t y = 0; y < dump.height; y++)
            {
                const uint8_t* row = src + size_t(y) * dump.rowWidth * dump.bytesPerPixel;
                uint8_t* out = &bmp[54 + size_t(y) * rowBytes];
                for (uint32_t x = 0; x < dump.width; x++)
                {
                    float rgb[3] = {};
                    const uint8_t* p = row + size_t(x) * dump.bytesPerPixel;
                    switch (dump.format)
                    {
                    case RenderFormat::R8G8B8A8_UNORM: for (int c = 0; c < 3; c++) rgb[c] = p[c] / 255.0f; break;
                    case RenderFormat::B8G8R8A8_UNORM: for (int c = 0; c < 3; c++) rgb[c] = p[2 - c] / 255.0f; break;
                    case RenderFormat::R16G16B16A16_FLOAT: for (int c = 0; c < 3; c++) rgb[c] = HalfToFloat(reinterpret_cast<const uint16_t*>(p)[c]); break;
                    case RenderFormat::R16G16B16A16_UNORM: for (int c = 0; c < 3; c++) rgb[c] = reinterpret_cast<const uint16_t*>(p)[c] / 65535.0f; break;
                    case RenderFormat::R16G16_FLOAT: for (int c = 0; c < 2; c++) rgb[c] = HalfToFloat(reinterpret_cast<const uint16_t*>(p)[c]); break;
                    case RenderFormat::R32_FLOAT: rgb[0] = rgb[1] = rgb[2] = (reinterpret_cast<const float*>(p)[0] - rangeMin) * rangeScale; break;
                    case RenderFormat::R32G32_FLOAT: for (int c = 0; c < 2; c++) rgb[c] = reinterpret_cast<const float*>(p)[c]; break;
                    default: break;
                    }
                    for (int c = 0; c < 3; c++)
                        out[x * 3 + 2 - c] = uint8_t(std::clamp(rgb[c], 0.0f, 1.0f) * 255.0f + 0.5f);
                }
            }
            dump.buffer->unmap();
            std::ofstream(dump.path, std::ios::binary).write(reinterpret_cast<const char*>(bmp.data()), bmp.size());
            LOGFN("[renderer] dumped {}", dump.path);
        }
        pending.clear();
    }

    void PreparePlaceholders(RenderCommandList* list)
    {
        if (g_placeholdersReady)
            return;

        // White 2D texture (cleared once), zero 3D and cube textures (as Unleashed's null textures).
        const RenderTexture* white = g_placeholders[0].get();
        RenderFramebufferDesc fb;
        fb.colorAttachments = &white;
        fb.colorAttachmentsCount = 1;
        g_whiteFramebuffer = g_device->createFramebuffer(fb);
        list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(g_placeholders[0].get(), RenderTextureLayout::COLOR_WRITE));
        list->setFramebuffer(g_whiteFramebuffer.get());
        list->clearColor(0, RenderColor(1.0f, 1.0f, 1.0f, 1.0f));
        list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(g_placeholders[0].get(), RenderTextureLayout::SHADER_READ));
        g_placeholdersReady = true;
    }
}

bool Renderer::Init(RenderDevice* device, bool vulkan, RenderFormat backBufferFormat)
{
    g_device = device;
    g_vulkan = vulkan;
    g_backBufferFormat = backBufferFormat;

    // Same layout as UnleashedRecomp: texture heap in spaces 0-2 (2D, 3D, cube), samplers in
    // space 3; constants as root CBVs b0-b2 (D3D12) or as buffer addresses in push constants (Vulkan).
    RenderPipelineLayoutBuilder layoutBuilder;
    layoutBuilder.begin(false, true);

    RenderDescriptorSetBuilder setBuilder;
    setBuilder.begin();
    setBuilder.addTexture(0, kTextureDescriptorCount);
    setBuilder.end(true, kTextureDescriptorCount);
    g_textureSet = setBuilder.create(device);
    layoutBuilder.addDescriptorSet(setBuilder);
    layoutBuilder.addDescriptorSet(setBuilder);
    layoutBuilder.addDescriptorSet(setBuilder);

    setBuilder.begin();
    setBuilder.addSampler(0, kSamplerDescriptorCount);
    setBuilder.end(true, kSamplerDescriptorCount);
    g_samplerSet = setBuilder.create(device);
    layoutBuilder.addDescriptorSet(setBuilder);

    if (vulkan)
    {
        layoutBuilder.addPushConstant(0, 4, 24, RenderShaderStageFlag::VERTEX | RenderShaderStageFlag::PIXEL);
    }
    else
    {
        layoutBuilder.addRootDescriptor(0, 4, RenderRootDescriptorType::CONSTANT_BUFFER);
        layoutBuilder.addRootDescriptor(1, 4, RenderRootDescriptorType::CONSTANT_BUFFER);
        layoutBuilder.addRootDescriptor(2, 4, RenderRootDescriptorType::CONSTANT_BUFFER);
    }
    layoutBuilder.end();
    g_pipelineLayout = layoutBuilder.create(device);

    g_sampler = device->createSampler(RenderSamplerDesc());
    g_samplerSet->setSampler(0, g_sampler.get());
    RenderSamplerDesc clampDesc;
    clampDesc.addressU = clampDesc.addressV = clampDesc.addressW = RenderTextureAddressMode::CLAMP;
    g_blitSampler = device->createSampler(clampDesc);
    g_samplerSet->setSampler(kBlitSampler, g_blitSampler.get());

    for (int i = 0; i < 3; i++)
    {
        RenderTextureDesc desc;
        desc.width = 1;
        desc.height = 1;
        desc.depth = 1;
        desc.mipLevels = 1;
        desc.arraySize = 1;
        RenderTextureViewDesc view;
        view.mipLevels = 1;
        if (i == 0)
        {
            desc = RenderTextureDesc::ColorTarget(1, 1, RenderFormat::R8G8B8A8_UNORM);
            view.format = RenderFormat::R8G8B8A8_UNORM;
            view.dimension = RenderTextureViewDimension::TEXTURE_2D;
        }
        else
        {
            desc.format = RenderFormat::R8_UNORM;
            view.format = RenderFormat::R8_UNORM;
            view.componentMapping = RenderComponentMapping(RenderSwizzle::ZERO, RenderSwizzle::ZERO, RenderSwizzle::ZERO, RenderSwizzle::ZERO);
            if (i == 1)
            {
                desc.dimension = RenderTextureDimension::TEXTURE_3D;
                view.dimension = RenderTextureViewDimension::TEXTURE_3D;
            }
            else
            {
                desc.dimension = RenderTextureDimension::TEXTURE_2D;
                desc.arraySize = 6;
                desc.flags = RenderTextureFlag::CUBE;
                view.dimension = RenderTextureViewDimension::TEXTURE_CUBE;
            }
        }
        g_placeholders[i] = device->createTexture(desc);
        g_placeholderViews[i] = g_placeholders[i]->createTextureView(view);
        g_textureSet->setTexture(i, g_placeholders[i].get(), RenderTextureLayout::SHADER_READ, g_placeholderViews[i].get());
    }

    // Vulkan (Windows, and iOS through MoltenVK): SPIR-V compiled ahead of time from blit.hlsl
    // (scripts/build_blit_spirv.ps1). D3D12: compiled from kBlitShader at startup.
    if (g_vulkan)
    {
        g_blitVertexShader = device->createShader(kBlitSpirv_mainVS, sizeof(kBlitSpirv_mainVS), "mainVS", RenderShaderFormat::SPIRV);
        g_blitPixelShader = device->createShader(kBlitSpirv_mainPS, sizeof(kBlitSpirv_mainPS), "mainPS", RenderShaderFormat::SPIRV);
        g_depthPixelShader = device->createShader(kBlitSpirv_mainDepthPS, sizeof(kBlitSpirv_mainDepthPS), "mainDepthPS", RenderShaderFormat::SPIRV);
        g_depthMsaaPixelShader = device->createShader(kBlitSpirv_mainDepthMsaaPS, sizeof(kBlitSpirv_mainDepthMsaaPS), "mainDepthMsaaPS", RenderShaderFormat::SPIRV);
        g_overlayPixelShader = device->createShader(kBlitSpirv_mainOverlayPS, sizeof(kBlitSpirv_mainOverlayPS), "mainOverlayPS", RenderShaderFormat::SPIRV);
        g_menuPixelShader = device->createShader(kBlitSpirv_mainMenuPS, sizeof(kBlitSpirv_mainMenuPS), "mainMenuPS", RenderShaderFormat::SPIRV);
        g_encodePixelShader = device->createShader(kBlitSpirv_mainEncode7e3PS, sizeof(kBlitSpirv_mainEncode7e3PS), "mainEncode7e3PS", RenderShaderFormat::SPIRV);
    }
#ifdef _WIN32
    else
    {
        g_blitVertexShader = CompileShader(kBlitShader, L"mainVS", L"vs_6_0", true);
        g_blitPixelShader = CompileShader(kBlitShader, L"mainPS", L"ps_6_0", false);
        g_depthPixelShader = CompileShader(kBlitShader, L"mainDepthPS", L"ps_6_0", false);
        g_depthMsaaPixelShader = CompileShader(kBlitShader, L"mainDepthMsaaPS", L"ps_6_0", false);
        g_overlayPixelShader = CompileShader(kBlitShader, L"mainOverlayPS", L"ps_6_0", false);
        g_menuPixelShader = CompileShader(kBlitShader, L"mainMenuPS", L"ps_6_0", false);
        g_encodePixelShader = CompileShader(kBlitShader, L"mainEncode7e3PS", L"ps_6_0", false);
    }
#endif
    if (g_blitVertexShader == nullptr || g_blitPixelShader == nullptr)
    {
        LOGN_ERROR("[renderer] blit shaders could not be compiled");
        return false;
    }

    RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = g_pipelineLayout.get();
    desc.vertexShader = g_blitVertexShader.get();
    desc.pixelShader = g_blitPixelShader.get();
    desc.renderTargetCount = 1;
    desc.renderTargetFormat[0] = backBufferFormat;
    desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
    desc.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
    g_blitPipeline = device->createGraphicsPipeline(desc);
    if (g_depthPixelShader != nullptr)
    {
        desc.pixelShader = g_depthPixelShader.get();
        desc.renderTargetFormat[0] = RenderFormat::R32_FLOAT;
        g_depthPipeline = device->createGraphicsPipeline(desc);
        if (g_depthMsaaPixelShader != nullptr)
        {
            desc.pixelShader = g_depthMsaaPixelShader.get();
            g_depthMsaaPipeline = device->createGraphicsPipeline(desc);
        }
    }
    if (g_encodePixelShader != nullptr)
    {
        desc.pixelShader = g_encodePixelShader.get();
        desc.renderTargetFormat[0] = RenderFormat::R16G16B16A16_UNORM;
        desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
        g_encodePipeline = device->createGraphicsPipeline(desc);
    }
    if (g_overlayPixelShader != nullptr)
    {
        desc.pixelShader = g_overlayPixelShader.get();
        desc.renderTargetFormat[0] = backBufferFormat;
        desc.renderTargetBlend[0] = RenderBlendDesc::AlphaBlend();
        g_overlayPipeline = device->createGraphicsPipeline(desc);
    }
    if (g_menuPixelShader != nullptr)
    {
        desc.pixelShader = g_menuPixelShader.get();
        desc.renderTargetFormat[0] = backBufferFormat;
        desc.renderTargetBlend[0] = RenderBlendDesc::AlphaBlend();
        g_menuPipeline = device->createGraphicsPipeline(desc);
    }
    if ((g_depthPixelShader != nullptr && g_depthPipeline == nullptr) || (g_depthMsaaPixelShader != nullptr && g_depthMsaaPipeline == nullptr) ||
        (g_encodePixelShader != nullptr && g_encodePipeline == nullptr) || (g_overlayPixelShader != nullptr && g_overlayPipeline == nullptr) ||
        (g_menuPixelShader != nullptr && g_menuPipeline == nullptr))
        LOGFN_WARNING("[renderer] pipelines not made: depth {}, depth MSAA {}, encode {}, overlay {}, menu {} (those passes are skipped)",
            g_depthPipeline == nullptr, g_depthMsaaPipeline == nullptr, g_encodePipeline == nullptr, g_overlayPipeline == nullptr, g_menuPipeline == nullptr);
    if (g_blitPipeline == nullptr)
    {
        LOGN_ERROR("[renderer] blit pipeline could not be created");
        return false;
    }

    if (const char* dump = getenv("DR_DUMP"))
        g_dumpInterval = std::max(1, atoi(dump));
    if (const char* show = getenv("DR_SHOW"))
        g_showFinal = strcmp(show, "scene") != 0 && strcmp(show, "Scene") != 0;

    // Both upload buffers at their working size now, not at the first area load (UseUploadSlot).
    // DR_UPLOAD_MB sets the size (0: start at 16 MB and grow in game, as before).
    {
        const char* value = getenv("DR_UPLOAD_MB");
        #if defined(__APPLE__) && TARGET_OS_IPHONE
        constexpr size_t kDefaultMB = 48; // the app's own memory on iOS (kGeometryHeapSize); grows when a frame needs more
#else
        constexpr size_t kDefaultMB = 160;
#endif
        const size_t mb = value != nullptr ? size_t(std::max(0, atoi(value))) : kDefaultMB;
        if (mb >= 16)
        {
            const auto start = std::chrono::steady_clock::now();
            for (UploadSlot& slot : g_uploadSlots)
                CreateUploadSlot(slot, mb << 20);
            LOGFN("[renderer] upload buffers: {} x {} MB ({} ms)", kFrameSlots, mb,
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
        }
    }

    g_rendererActive = true;
    LOGFN("[renderer] ready ({}){}", vulkan ? "Vulkan" : "D3D12",
        g_dumpInterval != 0 ? fmt::format(", dumping render targets every {} frames", g_dumpInterval) : std::string());
    return true;
}

namespace
{
    // What a frame leaves for later, kept with its slot until the slot comes round again (the
    // frame has then finished on the GPU): resources it replaced, write-backs and dumps to read.
    struct SlotState
    {
        std::vector<std::unique_ptr<RenderTexture>> textures;
        std::vector<std::unique_ptr<RenderFramebuffer>> framebuffers;
        std::vector<std::unique_ptr<RenderTextureView>> views;
        std::vector<std::unique_ptr<RenderBuffer>> buffers;
        std::vector<PendingWriteback> writebacks;
        std::vector<PendingDump> dumps;
        std::vector<uint32_t> releasedDescriptors;
    };
    SlotState g_slotStates[kFrameSlots];

    template <typename T>
    void MoveAppend(std::vector<T>& to, std::vector<T>& from)
    {
        to.insert(to.end(), std::make_move_iterator(from.begin()), std::make_move_iterator(from.end()));
        from.clear();
    }

    // When Render returns: this frame's leftovers go to its slot.
    struct SlotHandOver
    {
        ~SlotHandOver()
        {
            SlotState& slot = g_slotStates[g_frameSlot];
            MoveAppend(slot.textures, g_retiredTextures);
            MoveAppend(slot.framebuffers, g_retiredFramebuffers);
            MoveAppend(slot.views, g_retiredViews);
            MoveAppend(slot.buffers, g_retiredBuffers);
            MoveAppend(slot.writebacks, g_pendingWritebacks);
            MoveAppend(slot.dumps, g_pendingDumps);
            MoveAppend(slot.releasedDescriptors, g_releasedThisFrame);
        }
    };

    size_t UploadSizeFor(const Frame& frame)
    {
        // Frame data, the dummy vertex slot and blit constants, 64 constant slots (depth resolves),
        // the touch overlay and the options menu (Replay, Render), then the taken textures.
        return TextureUploadBase(frame) + frame.texturesSize;
    }
}

bool Renderer::Render(RenderCommandList* list, RenderFramebuffer* backBuffer, uint32_t width, uint32_t height, uint32_t slot,
    void (*waitOtherSlot)())
{
    // video.cpp waited for this slot's last frame on the GPU: what it retired goes, its
    // write-backs, dumps and GPU timestamps are read, the descriptors it freed are handed back.
    g_frameSlot = slot % kFrameSlots;
    g_waitOtherSlot = waitOtherSlot;
    g_otherSlotWaited = false;
    // The work around the replay, timed: the video thread's own log (video.cpp) showed replay
    // stages of 120-135 ms at area loads at 4K where the replay itself took 25-60 ms.
    using Clock = std::chrono::steady_clock;
    const auto usSince = [](Clock::time_point start) { return uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count()); };
    const auto renderStart = Clock::now();
    uint32_t freeUs = 0, writeBackUs = 0, placeholderUs = 0, uploadUs = 0, freedTextures = 0, freedBuffers = 0;
    {
        SlotState& done = g_slotStates[g_frameSlot];
        if (!done.releasedDescriptors.empty() && g_textureSet != nullptr)
        {
            // The frame in the other slot was recorded after these were freed: it cannot read them.
            for (uint32_t descriptor : done.releasedDescriptors)
                g_textureSet->setTexture(descriptor, g_placeholders[0].get(), RenderTextureLayout::SHADER_READ, g_placeholderViews[0].get());
            std::lock_guard lock(g_resolveMutex);
            g_freeDescriptors.insert(g_freeDescriptors.end(), done.releasedDescriptors.begin(), done.releasedDescriptors.end());
            done.releasedDescriptors.clear();
        }
        freedTextures = uint32_t(done.textures.size());
        freedBuffers = uint32_t(done.buffers.size());
        const auto freeStart = Clock::now();
        done.textures.clear();
        done.framebuffers.clear();
        done.views.clear();
        done.buffers.clear();
        freeUs = usSince(freeStart);
        const auto writeBackStart = Clock::now();
        WriteDumps(done.dumps);
        WriteBackResolves(done.writebacks);
        g_gpuProfile.Collect(g_frameSlot);
        writeBackUs = usSince(writeBackStart);
    }
    SlotHandOver handOver;

    const auto placeholderStart = Clock::now();
    PreparePlaceholders(list);
    placeholderUs = usSince(placeholderStart);

    bool haveFrame = false;
    {
        std::lock_guard lock(g_frameMutex);
        if (g_pendingValid)
        {
            std::swap(g_pending, g_rendering);
            g_pendingValid = false;
            g_frameTaken.notify_all();
            haveFrame = true;
        }
    }
    const size_t uploadSizeBefore = g_uploadSlots[g_frameSlot].size;
    const auto uploadStart = Clock::now();
    UseUploadSlot(UploadSizeFor(g_rendering));
    uploadUs = usSince(uploadStart);
    {
        const uint32_t beforeUs = usSince(renderStart);
        static uint32_t logged;
        if (beforeUs > 15000 && logged++ < 200)
            LOGFN("[hitch] before the replay of frame {}: {} ms; freeing {} textures and {} buffers {} ms, write-backs {} ms, placeholders {} ms, "
                  "upload buffer {} ms ({} -> {} MB)",
                g_frameIndex + 1, beforeUs / 1000, freedTextures, freedBuffers, freeUs / 1000, writeBackUs / 1000, placeholderUs / 1000, uploadUs / 1000,
                uploadSizeBefore >> 20, g_uploadSlots[g_frameSlot].size >> 20);
    }

    if (haveFrame)
    {
        g_dumpingFrame = (g_dumpInterval != 0 && (g_replayedFrames + 1) % g_dumpInterval == 0) || g_rendering.capture;
        const auto replayStart = std::chrono::steady_clock::now();
        HitchStats::TakeThread();
        g_newPipelines = 0;
        Replay(list, g_rendering);
        const uint64_t replayUs = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - replayStart).count());
        g_timing.replayUs += replayUs;
        if (replayUs > 25000)
        {
            // A long replay holds the game at its next present: what it spent the time on.
            const HitchStats::Counts counts = HitchStats::TakeThread();
            static uint32_t logged;
            if (logged++ < 200)
                LOGFN("[hitch] replay of frame {}: {} ms; {} new pipelines {} ms, texture uploads {} ms, waiting for the other frame {} ms, render targets {} ms",
                    g_frameIndex, replayUs / 1000, g_newPipelines.load(), counts.us[HitchStats::Pipelines] / 1000, counts.us[HitchStats::Uploads] / 1000,
                    counts.us[HitchStats::OtherFrameWaits] / 1000, counts.us[HitchStats::Targets] / 1000);
        }
        g_timing.replayedFrames++;
        g_replayedFrames++;
        if (g_dumpingFrame)
            QueueDumps(list);
    }

    // Minimized window (video.cpp, RenderOffscreen): replayed for its uploads and resolves only.
    if (backBuffer == nullptr)
        return haveFrame;

    // Show the frame's last full-screen resolve (what the game copies out for display); before the
    // game resolves anything, the render target it drew into most.
    uint32_t shownDescriptor = 0, shownWidth = 0, shownHeight = 0;
    if (!g_showFinal && g_sceneSnapshot.texture != nullptr)
    {
        shownDescriptor = g_sceneSnapshot.descriptor;
        shownWidth = g_sceneSnapshot.width;
        shownHeight = g_sceneSnapshot.height;
    }
    else if (g_shownResolve != nullptr)
    {
        shownDescriptor = g_shownResolve->descriptor;
        shownWidth = g_shownResolve->width;
        shownHeight = g_shownResolve->height;
    }
    else if (g_shownTarget != nullptr)
    {
        SingleSampled(list, *g_shownTarget, 0, 0, int32_t(g_shownTarget->width), int32_t(g_shownTarget->height), RenderTextureLayout::SHADER_READ);
        shownDescriptor = g_shownTarget->descriptor;
        shownWidth = g_shownTarget->width;
        shownHeight = g_shownTarget->height;
    }
    bool havePicture = true;
    if (shownDescriptor == 0 && g_shownTarget == nullptr && g_shownResolve == nullptr &&
        (g_showFinal || g_sceneSnapshot.texture == nullptr))
    {
        // No picture from the game yet: black, with the touch controls and the options menu or
        // the status panel (ui/options_menu.cpp) over it, so the screen is never just black.
        if (!OptionsMenu::Visible())
            return false;
        havePicture = false;
        shownWidth = 1280;
        shownHeight = 720;
    }

    // The blit reads its texture index (and the FXAA switch) from the "shared constants" slot.
    // DR_AA=0 turns FXAA off.
    static const bool noFxaa = getenv("DR_AA") && strcmp(getenv("DR_AA"), "0") == 0;
    const bool fxaa = Config::FXAA && !noFxaa;
    const size_t blitOffset = ((g_rendering.data.size() + 255) & ~size_t(255)) + 256;
    const uint32_t blitConstants[4] = { shownDescriptor, fxaa ? 1u : 0u, 0, 0 };
    memcpy(g_uploadMemory + blitOffset, blitConstants, sizeof(blitConstants));

    // Options menu (ui/options_menu.cpp): its image is uploaded when it changed (outside the back
    // buffer's pass) and blended over the picture after the touch overlay.
    const bool menu = g_menuPipeline != nullptr && OptionsMenu::Visible();
    const size_t menuOffset = ((g_rendering.data.size() + 255) & ~size_t(255)) + 512 + 256 * 64 + 1280;
    if (menu)
    {
        uint32_t menuWidth = 0, menuHeight = 0, version = 0;
        const uint8_t* pixels = OptionsMenu::Image(menuWidth, menuHeight, version);
        if (g_menuTexture == nullptr)
        {
            g_menuTexture = g_device->createTexture(RenderTextureDesc::Texture2D(menuWidth, menuHeight, 1, RenderFormat::R8G8B8A8_UNORM));
            g_menuDescriptor = AllocateDescriptor();
        }
        if (version != g_menuVersion)
        {
            // One upload buffer per frame slot: the other may still be copied from by the GPU.
            static std::unique_ptr<RenderBuffer> uploads[kFrameSlots];
            static uint8_t* memories[kFrameSlots] = {};
            if (uploads[g_frameSlot] == nullptr)
            {
                uploads[g_frameSlot] = g_device->createBuffer(RenderBufferDesc::UploadBuffer(size_t(menuWidth) * menuHeight * 4));
                memories[g_frameSlot] = static_cast<uint8_t*>(uploads[g_frameSlot]->map());
            }
            g_menuUpload = uploads[g_frameSlot].get();
            memcpy(memories[g_frameSlot], pixels, size_t(menuWidth) * menuHeight * 4);
            list->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(g_menuTexture.get(), RenderTextureLayout::COPY_DEST));
            list->copyTextureRegion(RenderTextureCopyLocation::Subresource(g_menuTexture.get()),
                RenderTextureCopyLocation::PlacedFootprint(g_menuUpload, RenderFormat::R8G8B8A8_UNORM, menuWidth, menuHeight, 1, menuWidth));
            list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(g_menuTexture.get(), RenderTextureLayout::SHADER_READ));
            if (g_menuVersion == UINT32_MAX)
                g_textureSet->setTexture(g_menuDescriptor, g_menuTexture.get(), RenderTextureLayout::SHADER_READ);
            g_menuVersion = version;
        }
        const uint32_t menuConstants[4] = { g_menuDescriptor, 0, 0, 0 };
        memcpy(g_uploadMemory + menuOffset, menuConstants, sizeof(menuConstants));
    }

    // Keep the game's aspect ratio (16:9): black bars on wider or taller windows (phones).
    list->setFramebuffer(backBuffer);
    RenderViewport viewport(0.0f, 0.0f, float(width), float(height));
    RenderRect scissor(0, 0, int32_t(width), int32_t(height));
    list->setScissors(&scissor, 1);
    const float scale = std::min(float(width) / float(shownWidth), float(height) / float(shownHeight));
    const float shownW = std::round(float(shownWidth) * scale), shownH = std::round(float(shownHeight) * scale);
    g_gpuProfile.Mark(list, "final blit to the window");
    if (!havePicture)
    {
        list->setViewports(&viewport, 1);
        list->clearColor(0, RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
    }
    else if (shownW < float(width) - 0.5f || shownH < float(height) - 0.5f)
    {
        list->clearColor(0, RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
        RenderViewport image(std::floor((float(width) - shownW) * 0.5f), std::floor((float(height) - shownH) * 0.5f), shownW, shownH);
        list->setViewports(&image, 1);
    }
    else
        list->setViewports(&viewport, 1);
    if (havePicture)
    {
        list->setPipeline(g_blitPipeline.get());
        BindLayout(list);
        SetRootDescriptor(list, uint32_t(blitOffset), 2);
        list->drawInstanced(3, 1, 0, 0);
    }

    // Dumps: the image as shown (FXAA included), from the same blit into an offscreen copy.
    // DR_SHOT=<step>[:<first>-<last>] writes only this image, every <step> frames.
    static uint32_t shotStep = 0, shotFirst = 0, shotLast = UINT32_MAX;
    static const bool shotParsed = [] {
        if (const char* shot = getenv("DR_SHOT"))
        {
            shotStep = std::max(1, atoi(shot));
            if (const char* range = strchr(shot, ':'))
            {
                shotFirst = uint32_t(strtoul(range + 1, nullptr, 10));
                if (const char* dash = strchr(range, '-'))
                    shotLast = uint32_t(strtoul(dash + 1, nullptr, 10));
            }
        }
        return true;
    }();
    (void)shotParsed;
    const bool shot = shotStep != 0 && g_frameIndex >= shotFirst && g_frameIndex <= shotLast && (g_frameIndex - shotFirst) % shotStep == 0;
    if ((g_dumpingFrame || shot) && havePicture)
    {
        static std::unique_ptr<RenderTexture> shownTexture;
        static std::unique_ptr<RenderFramebuffer> shownFramebuffer;
        if (shownTexture != nullptr)
            g_retiredTextures.push_back(std::move(shownTexture));
        if (shownFramebuffer != nullptr)
            g_retiredFramebuffers.push_back(std::move(shownFramebuffer));
        shownTexture = g_device->createTexture(RenderTextureDesc::ColorTarget(shownWidth, shownHeight, g_backBufferFormat));
        const RenderTexture* color = shownTexture.get();
        RenderFramebufferDesc fb;
        fb.colorAttachments = &color;
        fb.colorAttachmentsCount = 1;
        shownFramebuffer = g_device->createFramebuffer(fb);
        list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(shownTexture.get(), RenderTextureLayout::COLOR_WRITE));
        list->setFramebuffer(shownFramebuffer.get());
        RenderViewport shownViewport(0.0f, 0.0f, float(shownWidth), float(shownHeight));
        list->setViewports(&shownViewport, 1);
        RenderRect shownScissor(0, 0, int32_t(shownWidth), int32_t(shownHeight));
        list->setScissors(&shownScissor, 1);
        list->setPipeline(g_blitPipeline.get());
        BindLayout(list);
        SetRootDescriptor(list, uint32_t(blitOffset), 2);
        list->drawInstanced(3, 1, 0, 0);
        if (menu)
        {
            list->setPipeline(g_menuPipeline.get());
            BindLayout(list);
            SetRootDescriptor(list, uint32_t(menuOffset), 2);
            list->drawInstanced(3, 1, 0, 0);
        }
        QueueDump(list, shownTexture.get(), g_backBufferFormat, shownWidth, shownHeight,
            fmt::format("dump/frame{:05}_shown_{}x{}.bmp", g_frameIndex, shownWidth, shownHeight));
        LOGFN("[sync] frame {} audio sample {}", g_frameIndex, Audio::SamplesSubmitted());
        list->setFramebuffer(backBuffer);
        list->setScissors(&scissor, 1);
    }
    list->setViewports(&viewport, 1); // the overlay covers the whole window

    // On-screen touch controls (iOS; DR_TOUCH=1 on the desktop) over the game's image.
    if (g_overlayPipeline != nullptr && TouchControls::Visible(Input::ControllerConnected()))
    {
        static std::vector<TouchControls::Circle> circles;
        circles.clear();
        TouchControls::GetOverlay(width, height, circles);
        const size_t overlayOffset = ((g_rendering.data.size() + 255) & ~size_t(255)) + 512 + 256 * 64;
        uint8_t* constants = g_uploadMemory + overlayOffset;
        memset(constants, 0, 32);
        const uint32_t count = uint32_t(std::min<size_t>(circles.size(), 32));
        memcpy(constants + 16, &count, 4);
        for (uint32_t i = 0; i < count; i++)
        {
            const float circle[4] = { circles[i].x, circles[i].y, circles[i].radius, circles[i].filled ? 1.0f : 0.0f };
            memcpy(constants + 32 + i * 16, circle, 16);
            memcpy(constants + 32 + 32 * 16 + i * 16, circles[i].color, 16);
        }
        list->setPipeline(g_overlayPipeline.get());
        BindLayout(list);
        SetRootDescriptor(list, uint32_t(overlayOffset), 2);
        list->drawInstanced(3, 1, 0, 0);
    }
    if (menu)
    {
        RenderViewport menuViewport(std::floor((float(width) - shownW) * 0.5f), std::floor((float(height) - shownH) * 0.5f), shownW, shownH);
        list->setViewports(&menuViewport, 1);
        list->setPipeline(g_menuPipeline.get());
        BindLayout(list);
        SetRootDescriptor(list, uint32_t(menuOffset), 2);
        list->drawInstanced(3, 1, 0, 0);
    }

    g_gpuProfile.End(list);

    static auto lastLog = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (now - lastLog > std::chrono::seconds(5))
    {
        lastLog = now;
        LOGFN("[renderer] frame: {} commands, {:.1f} MB data; replayed {} draws, {} skipped, {} pipelines ({} failed); "
              "recording skips: no shader {}, no declaration {}, no buffers {}, primitive {}, too large {}; {} stencil draws, {} 2X and {} 4X MSAA draws; showing {} {}x{}",
            g_rendering.commands.size(), g_rendering.data.size() / 1048576.0, g_replayStats.draws, g_replayStats.skipped,
            [] { std::lock_guard lock(g_pipelineMutex); return g_pipelines.size(); }(), g_pipelinesFailed.load(), g_stats.noShader.load(), g_stats.noDeclaration.load(),
            g_stats.noBuffers.load(), g_stats.unsupportedPrimitive.load(), g_stats.tooLarge.load(), g_stats.stencil.load(), g_stats.samples2.load(), g_stats.samples4.load(),
            (!g_showFinal && g_sceneSnapshot.texture != nullptr) ? "scene" : g_shownResolve != nullptr ? "final resolve" : "target", shownWidth, shownHeight);
        LOGFN("[renderer] textures: {} decoded ({:.1f} MB), {} re-uploaded, {} unsupported; {} samplers; descriptors {} of {}",
            g_textureStats.decoded.load(), g_textureStats.bytes.load() / 1048576.0, g_textureStats.reuploaded.load(),
            g_textureStats.failed.load(), g_samplers.size(), g_descriptorCounter.load(), kTextureDescriptorCount);
        g_replayStats.draws = g_replayStats.skipped = 0;
        const uint32_t guestFrames = std::max(g_timing.guestFrames.exchange(0), 1u);
        const uint32_t replayed = std::max(g_timing.replayedFrames.exchange(0), 1u);
        LOGFN("[renderer] timing per frame: guest {:.1f} ms (of which waiting for the video thread {:.1f} ms), renderer hooks {:.1f} ms, replay {:.1f} ms, waiting for the GPU {:.1f} ms; "
              "{} texture cache entries, {} free descriptors, {} waits for the other frame (descriptors)",
            g_timing.guestFrameUs.exchange(0) / 1000.0 / guestFrames, g_timing.guestWaitUs.exchange(0) / 1000.0 / guestFrames, g_timing.hookUs.exchange(0) / 1000.0 / guestFrames,
            g_timing.replayUs.exchange(0) / 1000.0 / replayed, g_timing.gpuWaitUs.exchange(0) / 1000.0 / replayed,
            g_textureCacheSize.load(), [] { std::lock_guard lock(g_resolveMutex); return g_freeDescriptors.size(); }(), g_descriptorWaits.exchange(0));

        // GPU memory of the EDRAM model and the render target copies (the resolution scale multiplies it).
        auto bytesPerPixel = [](RenderFormat format) -> double {
            switch (format)
            {
            case RenderFormat::R16G16B16A16_FLOAT: case RenderFormat::R16G16B16A16_UNORM: case RenderFormat::R32G32_FLOAT: return 8;
            default: return 4;
            }
        };
        double targetBytes = 0, depthBytes = 0, resolveBytes = 0;
        uint32_t largestWidth = 0, largestHeight = 0;
        for (auto& [key, target] : g_targets)
            if (target->color != nullptr)
            {
                targetBytes += double(target->width) * target->height * bytesPerPixel(target->format) * (target->samples + (target->samples > 1 ? 1 : 0));
                if (uint64_t(target->width) * target->height > uint64_t(largestWidth) * largestHeight)
                    largestWidth = target->width, largestHeight = target->height;
            }
        for (auto& [key, depth] : g_depthBuffers)
            if (depth->texture != nullptr)
                depthBytes += double(depth->width) * depth->height * 8.0 * depth->samples;
        for (auto& [key, resolved] : g_resolveTextures)
            if (resolved->texture != nullptr)
                resolveBytes += double(resolved->width) * resolved->height * bytesPerPixel(resolved->format) * (resolved->cube ? 6 : 1) * (resolved->levels > 1 ? 1.34 : 1.0);
        LOGFN("[renderer] memory at scale {}: {} colour targets {:.0f} MB (largest {}x{}), {} depth buffers {:.0f} MB, {} target copies {:.0f} MB",
            g_replayScale, g_targets.size(), targetBytes / 1048576.0, largestWidth, largestHeight, g_depthBuffers.size(), depthBytes / 1048576.0,
            g_resolveTextures.size(), resolveBytes / 1048576.0);
    }
    return true;
}

uint32_t Renderer::PipelineCount()
{
    std::lock_guard lock(g_pipelineMutex);
    return uint32_t(g_pipelines.size());
}
