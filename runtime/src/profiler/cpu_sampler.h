#pragma once

#include <chrono>
#include <cstdint>

// Sampling CPU profiler for the game's threads (DR_CPU_PROFILE=1, Windows): guest threads register
// themselves; a sampler thread reads each one's stack about once a millisecond and every 10 s
// logs, per thread, where it was ("[cpu profile]"): time waiting, the functions with the most self
// and inclusive time, and the commonest call paths. Recompiled functions show as sub_XXXXXXXX
// (symbols from DRRecomp.pdb, next to the exe or at its build path).
namespace CpuSampler
{
    // Call on the thread to profile (once per thread; repeats are ignored). Does nothing without
    // DR_CPU_PROFILE. 'name' is copied.
    void RegisterCurrentThread(const char* name);

    // Crash reports (any build, Windows): logs the call stack of the faulting thread, recompiled
    // functions by name (sub_XXXXXXXX: the guest call chain). 'context' is the exception's CONTEXT.
    void LogStack(const void* context);

    // Hitch profile (default on, DR_HITCH_PROFILE=0 off, Windows): WatchHitches on the game's
    // presenting thread starts sampling it every 4 ms; ReportHitch logs where it was between two
    // times ("[hitch profile]": guest functions and call paths), on the sampler thread.
    void WatchHitches();
    // Also sample the calling thread (up to 4 in all; repeats ignored): the game's drawing thread,
    // the command processor. Reports list each thread's samples in the slow frame.
    void WatchHitchThread(const char* name);
    void ReportHitch(uint32_t frame, std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to);
}
