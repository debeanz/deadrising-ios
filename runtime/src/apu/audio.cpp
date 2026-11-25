#include <stdafx.h>
#include "audio.h"

#include <cpu/guest_thread.h>
#include <kernel/heap.h>
#include <kernel/memory.h>
#include <os/logger.h>

#include <SDL.h>

// Ported from UnleashedRecomp (apu/driver/sdl2_driver.cpp), with Dead Rising's runtime types.
namespace
{
    constexpr uint32_t kSampleRate = 48000;
    constexpr uint32_t kChannels = 6;
    constexpr uint32_t kSamples = 256; // per frame and channel

    uint32_t g_callback = 0;
    uint32_t g_callbackParam = 0; // guest pointer to the client's parameter
    SDL_AudioDeviceID g_device = 0;
    bool g_stereo = true;
    std::atomic<bool> g_running{ false };

    bool OpenDevice()
    {
#ifdef _WIN32
        SDL_setenv("SDL_AUDIODRIVER", "wasapi", 1);
#endif
        SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_APP_NAME, "Dead Rising");
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
        {
            LOGFN_ERROR("[audio] SDL audio: {}", SDL_GetError());
            return false;
        }

        // DR_AUDIO_SURROUND=1 asks for 6 channels; stereo otherwise.
        const bool surround = getenv("DR_AUDIO_SURROUND") != nullptr;
        SDL_AudioSpec desired{}, obtained{};
        desired.freq = kSampleRate;
        desired.format = AUDIO_F32SYS;
        desired.channels = surround ? kChannels : 2;
        desired.samples = kSamples;
        g_device = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, surround ? SDL_AUDIO_ALLOW_CHANNELS_CHANGE : 0);
        if (g_device != 0 && obtained.channels != 2 && obtained.channels != kChannels)
        {
            SDL_CloseAudioDevice(g_device);
            desired.channels = 2;
            g_device = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
        }
        if (g_device == 0)
        {
            LOGFN_ERROR("[audio] cannot open an audio device: {}", SDL_GetError());
            return false;
        }
        g_stereo = obtained.channels == 2;
        LOGFN("[audio] device open: {} Hz, {} channels", obtained.freq, obtained.channels);
        SDL_PauseAudioDevice(g_device, 0);
        return true;
    }

    // Runs the game's mixer every 5.33 ms, keeping at most ~50 ms queued.
    void AudioThread()
    {
        GuestThreadContext context(0);
        const uint32_t frameBytes = (g_stereo ? 2 : kChannels) * kSamples * sizeof(float);
        const auto interval = std::chrono::nanoseconds(1000000000ull * kSamples / kSampleRate);
        auto next = std::chrono::steady_clock::now();
        auto fullSince = next;
        bool full = false, reported = false;
        while (g_running)
        {
            // The game's audio waits on its mixer (voices start and stop in it), so the mixer must
            // run in real time even when the device does not play (iPhone, 2026-11-25: the audio
            // session not running yet, the queue stayed full and the game hung in its audio set-up
            // after the first frame). A queue full for over 200 ms is dropped and mixing goes on.
            const bool room = SDL_GetQueuedAudioSize(g_device) / frameBytes <= 10;
            const auto start = std::chrono::steady_clock::now();
            if (!room && !full)
            {
                full = true;
                fullSince = start;
            }
            if (room)
                full = false;
            if (room || start - fullSince > std::chrono::milliseconds(200))
            {
                if (!room)
                {
                    SDL_ClearQueuedAudio(g_device);
                    if (!reported)
                    {
                        reported = true;
                        LOGFN_WARNING("[audio] the device is not playing (status {}): mixing goes on, its output is dropped",
                            int(SDL_GetAudioDeviceStatus(g_device)));
                    }
                    full = false;
                }
                context.ppcContext.r3.u32 = g_callbackParam;
                g_memory.FindFunction(g_callback)(context.ppcContext, g_memory.base);
            }
            next += interval;
            const auto now = std::chrono::steady_clock::now();
            if (next < now - std::chrono::milliseconds(50))
                next = now; // fell behind (debugger, stall): do not burst
            std::this_thread::sleep_until(next);
        }
    }
}

namespace Audio
{
    float MasterVolume()
    {
        static const float volume = getenv("DR_VOLUME") ? float(atof(getenv("DR_VOLUME"))) : 1.0f;
        return volume;
    }

    void RegisterClient(uint32_t callback, uint32_t param)
    {
        if (g_running || callback == 0)
            return;
        if (getenv("DR_NO_AUDIO") != nullptr || !OpenDevice())
        {
            LOGFN("[audio] audio disabled");
            return;
        }
        auto* paramStorage = static_cast<be<uint32_t>*>(g_userHeap.Alloc(sizeof(uint32_t)));
        *paramStorage = param;
        g_callbackParam = g_memory.MapVirtual(paramStorage);
        g_callback = callback;
        g_running = true;
        std::thread(AudioThread).detach();
        LOGFN("[audio] render driver client {:08X} (param {:08X}) running", callback, param);
    }

    std::atomic<uint64_t> g_samplesSubmitted{ 0 };

    uint64_t SamplesSubmitted()
    {
        return g_samplesSubmitted.load();
    }

    void SubmitFrame(const void* samples)
    {
        if (g_device == 0 || samples == nullptr)
            return;
        g_samplesSubmitted += kSamples;
        // Channel-major: 6 runs of 256 big-endian floats (L, R, C, LFE, Ls, Rs).
        const auto* in = static_cast<const be<float>*>(samples);
        const float volume = MasterVolume();

        // Level meter in the log (every ~5 s): peak and RMS of what the game mixed.
        static uint32_t frames = 0;
        static double sumSquares = 0.0;
        static float peak = 0.0f;
        for (uint32_t i = 0; i < kChannels * kSamples; i++)
        {
            const float v = in[i];
            sumSquares += double(v) * v;
            peak = std::max(peak, std::fabs(v));
        }
        if (++frames == 940)
        {
            LOGFN("[audio] last 5 s: peak {:.3f}, rms {:.4f}", peak, std::sqrt(sumSquares / (double(frames) * kChannels * kSamples)));
            frames = 0;
            sumSquares = 0.0;
            peak = 0.0f;
        }
        if (g_stereo)
        {
            float out[2 * kSamples];
            for (uint32_t i = 0; i < kSamples; i++)
            {
                const float c = in[2 * kSamples + i];
                // ITU downmix (centre and surrounds at -3 dB), LFE dropped as consoles do for stereo.
                out[i * 2 + 0] = std::clamp((in[0 * kSamples + i] + (c + in[4 * kSamples + i]) * 0.7071f) * volume, -1.0f, 1.0f);
                out[i * 2 + 1] = std::clamp((in[1 * kSamples + i] + (c + in[5 * kSamples + i]) * 0.7071f) * volume, -1.0f, 1.0f);
            }
            SDL_QueueAudio(g_device, out, sizeof(out));
            // DR_AUDIO_DUMP=<file>: the stereo output as raw 48 kHz float (ffmpeg -f f32le -ar 48000 -ac 2).
            static FILE* dump = getenv("DR_AUDIO_DUMP") ? fopen(getenv("DR_AUDIO_DUMP"), "wb") : nullptr;
            if (dump != nullptr)
                fwrite(out, sizeof(out), 1, dump);
        }
        else
        {
            float out[kChannels * kSamples];
            for (uint32_t i = 0; i < kSamples; i++)
                for (uint32_t c = 0; c < kChannels; c++)
                    out[i * kChannels + c] = in[c * kSamples + i] * volume;
            SDL_QueueAudio(g_device, out, sizeof(out));
        }
    }
}
