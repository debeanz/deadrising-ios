#pragma once

#include <cstdint>
#include <chrono>

// Minimal Xenos command processor: consumes D3D's PM4 ring buffer so the game's GPU
// synchronisation works (fences, interrupts, read pointer), but does not render anything.
// It is the stand-in until the real renderer (D3D hooks or a PM4 translator) exists.
namespace CommandProcessor
{
    void InitializeRingBuffer(uint32_t base, uint32_t sizeLog2);
    void EnableReadPointerWriteBack(uint32_t address, uint32_t blockLog2);
    void SetInterruptCallback(uint32_t callback, uint32_t userData);
    void OnSwap();

    // The game wrote a GPU register through an MMIO store (ppc_detail.h): a new ring write pointer
    // wakes the command processor at once instead of at its next poll.
    void OnRegisterWrite(uint32_t address);

    // The "[perf]" report: since the last call.
    struct Stats
    {
        uint64_t waits = 0, waitNs = 0, waitMaxNs = 0, waitsAfterVblank = 0; // WAIT_REG_MEM that had to wait
        uint64_t vblanks = 0, vblanksLate = 0, vblankLateMaxNs = 0;    // the 60 Hz vblank timer (late: over 0.5 ms)
        uint64_t swaps = 0;                                              // VdSwap
        // The time between the game's swaps (its frame times): up to 20 ms, 20-36, 36-52, over 52 ms (a stutter), longest.
        uint64_t swapIntervals[4] = {}, swapIntervalMaxNs = 0;
    };
    Stats TakeStats();

    // D3D's spin-waits for the GPU (gpu/d3d_hooks.cpp, sub_8244ED10) sleep here: until the command processor moves on
    // (a batch done, a fence or memory write, a WAIT_REG_MEM reached, a vblank) or 'timeout' passes. 'seen' is the
    // caller's own count: when it moved on since the caller last looked, this returns at once.
    void WaitForProgress(uint64_t& seen, std::chrono::microseconds timeout);

    // GPU addresses are physical (0x0xxxxxxx); this runtime keeps physical memory at 0xA0000000.
    uint32_t GpuToGuest(uint32_t address);
}
