#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

// Where a long frame's time went ("[hitch]" lines, gpu/renderer.cpp): file reads, texture decodes
// and the renderer's hooks, timed on each thread and over all threads since the last present.
namespace HitchStats
{
    // FileRead, TextureDecode, Hooks: guest threads. Pipelines (created at first use), Uploads
    // (texture copies and host textures made for them): the video thread's replay.
    enum Kind { FileRead, TextureDecode, Hooks, Pipelines, Uploads, OtherFrameWaits, Targets, KindCount };

    struct Counts
    {
        uint64_t us[KindCount] = {};
        uint64_t bytesRead = 0, reads = 0;
    };

    inline thread_local Counts t_counts;
    inline std::atomic<uint64_t> g_us[KindCount];
    inline std::atomic<uint64_t> g_bytesRead{ 0 }, g_reads{ 0 };

    inline void Add(Kind kind, uint64_t us, uint64_t bytes = 0)
    {
        t_counts.us[kind] += us;
        g_us[kind] += us;
        if (kind == FileRead)
        {
            t_counts.bytesRead += bytes;
            t_counts.reads++;
            g_bytesRead += bytes;
            g_reads++;
        }
    }

    // This thread's counts since the last call (and resets them).
    inline Counts TakeThread()
    {
        Counts counts = t_counts;
        t_counts = {};
        return counts;
    }

    // All threads' counts since the last call (and resets them).
    inline Counts TakeGlobal()
    {
        Counts counts;
        for (int i = 0; i < KindCount; i++)
            counts.us[i] = g_us[i].exchange(0);
        counts.bytesRead = g_bytesRead.exchange(0);
        counts.reads = g_reads.exchange(0);
        return counts;
    }

    struct Timer
    {
        Kind kind;
        uint64_t bytes = 0;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        explicit Timer(Kind kind) : kind(kind) {}
        ~Timer() { Add(kind, uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count()), bytes); }
    };
}
