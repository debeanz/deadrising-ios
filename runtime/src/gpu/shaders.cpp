// Dead Rising shader lookup and host shader creation (see shaders.h and docs/shaders.md).
// Host shader creation follows UnleashedRecomp (GPL-3.0): SPIR-V is stored smol-v encoded and
// specialised at pipeline creation; DXIL shaders that use specialization constants are DXIL
// libraries, linked at runtime with a one-function library that returns the constant values.

#include <stdafx.h>
#include "shaders.h"
#include "shader_cache.h"
#include "shader_keys.h"
#include <kernel/memory.h>
#include <os/logger.h>

#include <condition_variable>
#include <plume_render_interface.h>
#include <smolv.h>
#include <zstd.h>

#ifdef _WIN32
#include <wrl/client.h>
#include <dxcapi.h>
using Microsoft::WRL::ComPtr;
#endif

using namespace plume;

struct HostShader
{
    bool isPixel = false;
    std::mutex mutex;
    std::unique_ptr<RenderShader> shader;                                 // SPIR-V, or DXIL without spec constants
    ankerl::unordered_dense::map<uint32_t, std::unique_ptr<RenderShader>> linked; // DXIL per spec constant value
    bool failed = false;
};

namespace
{
    // Offsets of the shader container behind D3D's in-place shader objects (sub_82452620 clears
    // 0x250 bytes for a vertex shader, sub_82452498 0x38 bytes for a pixel shader).
    constexpr uint32_t kVertexShaderObjectSize = 0x250;
    constexpr uint32_t kPixelShaderObjectSize = 0x38;

    RenderDevice* g_device = nullptr;
    bool g_spirv = false;
    std::unique_ptr<uint8_t[]> g_cache; // decompressed DXIL or smol-v SPIR-V

    std::mutex g_mutex;
    ankerl::unordered_dense::map<uint32_t, ShaderCacheEntry*> g_objects; // guest object -> entry
    std::vector<ShaderCacheEntry*> g_pending;                             // to build (worker)
    std::condition_variable g_pendingCv;

    struct Stats
    {
        std::atomic<uint32_t> vertexShaders{ 0 }, pixelShaders{ 0 };
        std::atomic<uint32_t> unknownKey{ 0 }, badContainer{ 0 };
        std::atomic<uint32_t> built{ 0 }, buildFailed{ 0 };
        std::atomic<uint32_t> binds{ 0 }, unknownBinds{ 0 }, nullBinds{ 0 };
    } g_stats;

    uint32_t LoadBE32(const uint8_t* p)
    {
        return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(p));
    }

    // Must stay identical to ShaderKey in xfb2xenos/main.cpp.
    uint64_t ShaderKeyOf(const uint8_t* container, size_t virtualSize, const uint8_t* microcode, size_t physicalSize)
    {
        return XXH3_64bits_withSeed(microcode, physicalSize, XXH3_64bits(container, virtualSize));
    }

    ShaderCacheEntry* FindEntryByHash(uint64_t hash)
    {
        auto end = g_shaderCacheEntries + g_shaderCacheEntryCount;
        auto it = std::lower_bound(g_shaderCacheEntries, end, hash,
            [](const ShaderCacheEntry& e, uint64_t h) { return e.hash < h; });
        return (it != end && it->hash == hash) ? it : nullptr;
    }

    ShaderCacheEntry* FindEntryByKey(uint64_t key)
    {
        auto end = g_shaderKeys + g_shaderKeyCount;
        auto it = std::lower_bound(g_shaderKeys, end, key,
            [](const ShaderKey& k, uint64_t v) { return k.key < v; });
        return (it != end && it->key == key) ? FindEntryByHash(it->cacheHash) : nullptr;
    }

    void OnShaderInit(uint32_t object, uint32_t microcode, bool isPixel)
    {
        const uint8_t* container = static_cast<const uint8_t*>(g_memory.Translate(
            object + (isPixel ? kPixelShaderObjectSize : kVertexShaderObjectSize)));
        const uint32_t flags = LoadBE32(container);
        const uint32_t virtualSize = LoadBE32(container + 4);
        const uint32_t physicalSize = LoadBE32(container + 8);

        (isPixel ? g_stats.pixelShaders : g_stats.vertexShaders)++;

        if ((flags & 0xFFFFFF00) != 0x102A0E00 || ((flags & 1) == 0) != isPixel ||
            virtualSize > 0x10000 || physicalSize > 0x40000)
        {
            if (g_stats.badContainer++ < 8)
                LOGFN_WARNING("[shaders] {} object {:08X}: no 2006 shader container behind it (flags {:08X})",
                    isPixel ? "pixel" : "vertex", object, flags);
            return;
        }

        const uint64_t key = ShaderKeyOf(container, virtualSize,
            static_cast<const uint8_t*>(g_memory.Translate(microcode)), physicalSize);
        ShaderCacheEntry* entry = FindEntryByKey(key);
        if (entry == nullptr)
        {
            if (g_stats.unknownKey++ < 8)
                LOGFN_WARNING("[shaders] {} object {:08X}: key {:016X} not in the shader cache (shader not from the XFB packages?)",
                    isPixel ? "pixel" : "vertex", object, key);
        }

        std::lock_guard lock(g_mutex);
        g_objects[object] = entry;
        if (entry != nullptr && entry->hostShader == nullptr)
        {
            entry->hostShader = new HostShader();
            entry->hostShader->isPixel = isPixel;
            g_pending.push_back(entry);
            g_pendingCv.notify_one();
        }
    }

#ifdef _WIN32
    // Links a DXIL library shader with "uint g_SpecConstants() { return value; }" (as UnleashedRecomp).
    std::unique_ptr<RenderShader> LinkDxil(ShaderCacheEntry* entry, uint32_t specConstants)
    {
        static std::mutex s_libraryMutex;
        static ankerl::unordered_dense::map<uint32_t, ComPtr<IDxcBlob>> s_specConstantLibraries;

        thread_local ComPtr<IDxcCompiler3> s_compiler;
        thread_local ComPtr<IDxcLinker> s_linker;
        thread_local ComPtr<IDxcUtils> s_utils;

        if (s_compiler == nullptr && FAILED(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(s_compiler.GetAddressOf()))))
            return nullptr;
        if (s_linker == nullptr && FAILED(DxcCreateInstance(CLSID_DxcLinker, IID_PPV_ARGS(s_linker.GetAddressOf()))))
            return nullptr;
        if (s_utils == nullptr && FAILED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(s_utils.GetAddressOf()))))
            return nullptr;

        ComPtr<IDxcBlob> specLibrary;
        {
            std::lock_guard lock(s_libraryMutex);
            specLibrary = s_specConstantLibraries[specConstants];
        }
        if (specLibrary == nullptr)
        {
            char hlsl[0x100];
            snprintf(hlsl, sizeof(hlsl), "export uint g_SpecConstants() { return %u; }", specConstants);
            DxcBuffer buffer{ hlsl, strlen(hlsl), 0 };
            const wchar_t* args[] = { L"-T lib_6_3" };

            ComPtr<IDxcResult> result;
            if (FAILED(s_compiler->Compile(&buffer, args, 1, nullptr, IID_PPV_ARGS(result.GetAddressOf()))) ||
                FAILED(result->GetResult(specLibrary.GetAddressOf())) || specLibrary == nullptr)
                return nullptr;

            std::lock_guard lock(s_libraryMutex);
            s_specConstantLibraries[specConstants] = specLibrary;
        }

        ComPtr<IDxcBlobEncoding> shaderLibrary;
        if (FAILED(s_utils->CreateBlobFromPinned(g_cache.get() + entry->dxilOffset, entry->dxilSize, DXC_CP_ACP,
            shaderLibrary.GetAddressOf())))
            return nullptr;

        wchar_t specName[64], shaderName[64];
        swprintf_s(specName, L"SpecConstants_%u", specConstants);
        swprintf_s(shaderName, L"Shader_%u", entry->dxilOffset);
        s_linker->RegisterLibrary(specName, specLibrary.Get());
        s_linker->RegisterLibrary(shaderName, shaderLibrary.Get());

        const wchar_t* libraries[] = { specName, shaderName };
        ComPtr<IDxcOperationResult> result;
        HRESULT status = E_FAIL;
        if (FAILED(s_linker->Link(L"main", entry->hostShader->isPixel ? L"ps_6_0" : L"vs_6_0", libraries, 2, nullptr, 0,
            result.GetAddressOf())) || FAILED(result->GetStatus(&status)) || FAILED(status))
        {
            ComPtr<IDxcBlobEncoding> errors;
            if (result != nullptr && SUCCEEDED(result->GetErrorBuffer(errors.GetAddressOf())) && errors != nullptr &&
                errors->GetBufferSize() != 0)
                LOGFN_ERROR("[shaders] linking {:016X} failed: {}", entry->hash,
                    std::string_view(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()));
            return nullptr;
        }

        ComPtr<IDxcBlob> blob;
        if (FAILED(result->GetResult(blob.GetAddressOf())) || blob == nullptr)
            return nullptr;

        return g_device->createShader(blob->GetBufferPointer(), blob->GetBufferSize(), "main", RenderShaderFormat::DXIL);
    }
#endif

    // Builds the host shaders of newly created guest shaders off the video and guest threads.
    void BuildWorker()
    {
        while (true)
        {
            std::vector<ShaderCacheEntry*> batch;
            {
                std::unique_lock lock(g_mutex);
                g_pendingCv.wait(lock, [] { return !g_pending.empty(); });
                batch.swap(g_pending);
            }

            for (ShaderCacheEntry* entry : batch)
            {
                bool ok = Shaders::GetHostShader(entry, 0) != nullptr;
                if (entry->hostShader->isPixel && (entry->specConstantsMask & Shaders::SPEC_ALPHA_TEST))
                    ok &= Shaders::GetHostShader(entry, Shaders::SPEC_ALPHA_TEST) != nullptr;
                if (!entry->hostShader->isPixel && (entry->specConstantsMask & Shaders::SPEC_FLIP_Z))
                    ok &= Shaders::GetHostShader(entry, Shaders::SPEC_FLIP_Z) != nullptr;
                (ok ? g_stats.built : g_stats.buildFailed)++;
            }
        }
    }
}

bool Shaders::Init(RenderDevice* device, bool spirv)
{
    g_device = device;
    g_spirv = spirv;

#ifdef _WIN32
    const uint8_t* compressed = spirv ? g_compressedSpirvCache : g_compressedDxilCache;
    const size_t compressedSize = spirv ? g_spirvCacheCompressedSize : g_dxilCacheCompressedSize;
    const size_t size = spirv ? g_spirvCacheDecompressedSize : g_dxilCacheDecompressedSize;
#else
    // XenosRecomp builds the DXIL cache on Windows only: elsewhere the cache is SPIR-V (Vulkan,
    // MoltenVK on iOS).
    const uint8_t* compressed = g_compressedSpirvCache;
    const size_t compressedSize = g_spirvCacheCompressedSize;
    const size_t size = g_spirvCacheDecompressedSize;
#endif

    g_cache = std::make_unique<uint8_t[]>(size);
    const size_t result = ZSTD_decompress(g_cache.get(), size, compressed, compressedSize);
    if (ZSTD_isError(result) || result != size)
    {
        LOGFN_ERROR("[shaders] could not unpack the {} shader cache", spirv ? "SPIR-V" : "DXIL");
        return false;
    }

    LOGFN("[shaders] {} shader cache ready: {} shaders, {} game-side keys, {:.1f} MB",
        spirv ? "SPIR-V" : "DXIL", g_shaderCacheEntryCount, g_shaderKeyCount, size / 1048576.0);

    std::thread(BuildWorker).detach();
    return true;
}

void Shaders::OnVertexShaderInit(uint32_t object, uint32_t microcode)
{
    OnShaderInit(object, microcode, false);
}

void Shaders::OnPixelShaderInit(uint32_t object, uint32_t microcode)
{
    OnShaderInit(object, microcode, true);
}

ShaderCacheEntry* Shaders::Find(uint32_t object)
{
    std::lock_guard lock(g_mutex);
    auto it = g_objects.find(object);
    return it != g_objects.end() ? it->second : nullptr;
}

RenderShader* Shaders::GetHostShader(ShaderCacheEntry* entry, uint32_t specConstants)
{
    if (entry == nullptr || entry->hostShader == nullptr || g_cache == nullptr)
        return nullptr;

    HostShader* host = entry->hostShader;
    std::lock_guard lock(host->mutex);

    if (g_spirv || entry->specConstantsMask == 0)
    {
        // SPIR-V: specialization constants are applied when the pipeline is created.
        if (host->shader == nullptr && !host->failed)
        {
            if (g_spirv)
            {
                const uint8_t* encoded = g_cache.get() + entry->spirvOffset;
                std::vector<uint8_t> decoded(smolv::GetDecodedBufferSize(encoded, entry->spirvSize));
                if (smolv::Decode(encoded, entry->spirvSize, decoded.data(), decoded.size()))
                    host->shader = g_device->createShader(decoded.data(), decoded.size(), "main", RenderShaderFormat::SPIRV);
            }
            else
            {
                host->shader = g_device->createShader(g_cache.get() + entry->dxilOffset, entry->dxilSize, "main", RenderShaderFormat::DXIL);
            }
            host->failed = host->shader == nullptr;
        }
        return host->shader.get();
    }

    specConstants &= entry->specConstantsMask;
    auto& linked = host->linked[specConstants];
#ifdef _WIN32
    if (linked == nullptr)
        linked = LinkDxil(entry, specConstants);
#endif
    return linked.get();
}

void Shaders::OnBind(uint32_t object)
{
    g_stats.binds++;
    if (object == 0)
        g_stats.nullBinds++;
    else if (Find(object) == nullptr)
        g_stats.unknownBinds++;
}

void Shaders::Update()
{
    static uint32_t lastBuilt = UINT32_MAX, lastFailed = UINT32_MAX, lastUnknown = UINT32_MAX;
    const uint32_t built = g_stats.built, failed = g_stats.buildFailed, unknown = g_stats.unknownBinds;
    if (built == lastBuilt && failed == lastFailed && unknown == lastUnknown)
        return;

    lastBuilt = built;
    lastFailed = failed;
    lastUnknown = unknown;
    LOGFN("[shaders] game created {} vertex + {} pixel shaders ({} not in the cache, {} without container); "
          "host shaders built for {} cache entries, {} failed; binds {} ({} null, {} of unknown objects)",
        g_stats.vertexShaders.load(), g_stats.pixelShaders.load(), g_stats.unknownKey.load(), g_stats.badContainer.load(),
        built, failed, g_stats.binds.load(), g_stats.nullBinds.load(), unknown);
}

ShaderCacheEntry* Shaders::Prepare(uint64_t hash, bool isPixel)
{
    ShaderCacheEntry* entry = FindEntryByHash(hash);
    if (entry == nullptr || entry->hash != hash)
        return nullptr;
    std::lock_guard lock(g_mutex);
    if (entry->hostShader == nullptr)
    {
        entry->hostShader = new HostShader();
        entry->hostShader->isPixel = isPixel;
    }
    return entry->hostShader->isPixel == isPixel ? entry : nullptr;
}
