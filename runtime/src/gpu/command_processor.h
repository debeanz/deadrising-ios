#pragma once

#include <cstdint>

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

    // GPU addresses are physical (0x0xxxxxxx); this runtime keeps physical memory at 0xA0000000.
    uint32_t GpuToGuest(uint32_t address);
}
