#pragma once

#include <cstdint>

// The "[perf]" report (gpu/video.cpp, every 5 s): numbers each part keeps, taken (and reset) by the report. Together
// they say where a frame's time goes on a device nobody can attach a profiler to (iPhone).
namespace Perf
{
    // The game waiting for the GPU (D3D's spin until ring space or a fence frees up, sub_8244B050): gpu/d3d_hooks.cpp.
    struct GameWait
    {
        uint64_t calls = 0, ns = 0, maxNs = 0;
    };
    GameWait TakeGameGpuWait();

    // The game's audio mixer thread (apu/audio.cpp): runs, late wakes (over 2 ms), the device queue at each run.
    struct Audio
    {
        uint64_t runs = 0, late = 0, lateMaxNs = 0, nearlyEmpty = 0, dropped = 0;
        double queuedMsSum = 0, queuedMsMin = 1e9;
    };
    Audio TakeAudio();

    // XMA decoding (apu/xma.cpp): packets decoded and the time it took.
    struct Xma
    {
        uint64_t decodes = 0, ns = 0, maxNs = 0;
    };
    Xma TakeXma();
}
