#pragma once

#include <cstdint>

// XMA decoding (the console's hardware audio decoder). The game's XAudio library programs 64-byte
// contexts in physical memory with input buffers of 2 KB XMA packets and an output ring buffer of
// 16-bit PCM, and drives them through the APU's registers: the context array's address (0x600,
// read once at start), kick (0x650+, start decoding), lock (0x690+, stop) and clear (0x6A0+,
// reset), one bit per context index. A worker thread services kicked contexts like the hardware.
// Decoding uses FFmpeg's XMA2 decoder (DR_HAVE_FFMPEG); without it contexts stay idle.
namespace Xma
{
    void Init();                                   // context array and its register, before the game starts
    uint32_t CreateContext();                      // XMACreateContext: guest address, 0 when all are taken
    void ReleaseContext(uint32_t guestContext);    // XMAReleaseContext
    void WriteRegister(uint32_t index, uint32_t value); // APU register (address 0x7FEA0000 + index * 4)
    bool Available();
}
