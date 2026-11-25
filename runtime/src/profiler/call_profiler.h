#pragma once

// Diagnostic build only (CMake option DR_CALL_PROFILER): counts calls to every recompiled
// function, and calls made with the D3D device as first argument, to map Dead Rising's
// statically linked Direct3D. Output (in the run folder):
//   call_profile.csv   per-function counts
//   call_samples.txt   up to 6 distinct calls per D3D-range function: arguments, the data behind
//                      pointer arguments, and the return value
//   d3d_device.bin     snapshot of the D3D device

#include <cstdint>
#include <ppc/ppc_recomp_shared.h>

namespace CallProfiler
{
    extern const uint32_t kFunctionCount;
    extern const uint32_t kFunctionAddresses[];

    // Returns a sample slot (or -1) that must be passed to Return() after the call.
    int Record(uint32_t index, PPCContext& ctx);
    void Return(uint32_t index, int slot, PPCContext& ctx);

    // The D3D device pointer (D3D passes it as the graphics interrupt callback's data).
    void SetDevice(uint32_t device);

    // Starts the thread that writes the output files every few seconds.
    void Start();
}
