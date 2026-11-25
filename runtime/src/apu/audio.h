#pragma once

#include <cstdint>

// XAudio render driver (the console's audio hardware interface): the game's XAudio mixer registers a
// client callback; it is run every 256 samples (48 kHz, 5.33 ms) and submits 6 channels of 256
// big-endian floats, which go to SDL (down-mixed to stereo unless the device has 6 channels).
namespace Audio
{
    void RegisterClient(uint32_t callback, uint32_t param);
    void SubmitFrame(const void* samples);
    float MasterVolume();
    uint64_t SamplesSubmitted(); // per channel, since the mixer started (debugging: aligning dumps)
}
