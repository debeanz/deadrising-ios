#pragma once

#include <cstdint>

// Host window + GPU device (plume: D3D12 or Vulkan on Windows, Metal on Apple).
// The guest's D3D Present hands each finished frame to the video thread.
namespace Video
{
    // Per-frame counts of the D3D calls the game made (filled by gpu/d3d_hooks.cpp).
    struct FrameStats
    {
        uint32_t frame = 0;
        uint32_t draws = 0;          // indexed draws
        uint32_t vertexBatches = 0;  // BeginVertices / BeginIndexedVertices
        uint32_t clears = 0;
        uint32_t resolves = 0;
        uint32_t tiles = 0;          // BeginTiling calls
        uint32_t textureBinds = 0;
        uint32_t streamBinds = 0;
        uint32_t shaderBinds = 0;
        uint32_t renderTargetBinds = 0;
    };

    // Starts the video thread: creates the window and the GPU device. Returns false on failure.
    bool Init();

    // Apple (UIKit needs SDL on the main thread): creates the window and the GPU device on the
    // calling thread, calls startGame once they exist, then runs the video loop (never returns).
    // iPhone / iPad: the game is rendered on a thread of its own (which calls startGame), and the
    // main thread keeps UIKit, SDL's events and the controllers.
    void RunOnMainThread(void (*startGame)());

    // Called from the guest's Present with the stats of the frame that just ended.
    void SubmitFrame(const FrameStats& stats);

    // For the native overlay (iOS, os/ios_overlay.mm) and the watchdog; any thread.
    uint32_t GameFrames();          // pictures the game has shown
    double SecondsSinceGameFrame(); // since the last one (or since the window opened, or the app came back)
    double Fps();                   // frames presented per second over the last half second
    void SetAppActive(bool active); // iOS: no rendering while the app is in the background (the game waits)
    bool RenderingPaused();
}
