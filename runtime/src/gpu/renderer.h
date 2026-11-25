#pragma once

#include <cstdint>

namespace plume
{
    struct RenderDevice;
    struct RenderCommandList;
    struct RenderFramebuffer;
    struct RenderTexture;
    enum class RenderFormat;
}

// Host rendering of Dead Rising's draws (stage 1: the original D3D still runs; see docs/renderer.md).
//
// Guest thread: the D3D hooks record every draw with the state it needs - shaders, vertex
// declaration, render state from the device's Xenos register shadows, constants, and copies of
// the vertex and index data (byte-swapped) - into a frame. Present hands the frame over.
// Video thread: replays the frame with plume into host render targets (one per guest surface)
// and shows the main one in the window.
namespace Renderer
{
    // Video thread, after the device and the shader cache exist.
    bool Init(plume::RenderDevice* device, bool vulkan, plume::RenderFormat backBufferFormat);

    // Video thread: draws the newest guest frame (if any) and presents it into the back buffer
    // framebuffer. Returns false if there is nothing to show yet (the caller clears instead).
    // slot: 0 or 1, alternating; the caller has waited for the GPU to finish the last frame in it.
    // waitOtherSlot: waits for the frame in the other slot (still on the GPU); called before a
    // texture descriptor that frame may read is pointed at another texture.
    bool Render(plume::RenderCommandList* list, plume::RenderFramebuffer* backBuffer, uint32_t width, uint32_t height, uint32_t slot,
        void (*waitOtherSlot)());

    // Guest thread hooks (gpu/d3d_hooks.cpp). Addresses are guest addresses.
    void OnCreateVertexDeclaration(uint32_t elements, uint32_t declaration);
    void OnDrawIndexed(uint32_t device, uint32_t primitive, int32_t baseVertex, uint32_t startIndex, uint32_t indexCount);
    void OnBeginVertices(uint32_t device, uint32_t primitive, uint32_t vertexCount, uint32_t stride, uint32_t data);
    void OnBeginIndexedVertices(uint32_t device, uint32_t primitive, int32_t baseVertex, uint32_t vertexCount, uint32_t indexCount,
        bool indices32, uint32_t stride, uint32_t vertexData, uint32_t indexData);
    void OnEndVertices(uint32_t device);
    void OnClear(uint32_t device, uint32_t flags, uint32_t color, float z, uint32_t stencil);
    // Resolve(device, flags, srcRect, destTexture, destPoint, level, slice, clearColor, clearZ, clearStencil)
    void OnResolve(uint32_t device, uint32_t flags, uint32_t sourceRect, uint32_t destTexture, uint32_t destPoint,
        uint32_t destLevel, uint32_t destSlice, uint32_t clearColor, float clearZ, uint32_t clearStencil);
    void OnPresent(uint32_t device);
    void AddGpuWait(uint32_t microseconds); // video thread: time waiting for the GPU to finish a frame
    uint32_t PipelineCount();               // pipelines made (or being made) so far

    // Debugging (DR_TEX_WATCH=1): the runtime's own writes into guest memory (XMA output, resolve
    // write-back) are checked against the memory of cached textures; overlaps are logged. Any
    // thread. 'who' is a static string.
    void CheckGuestWrite(const char* who, uint32_t address, uint32_t size);
}
