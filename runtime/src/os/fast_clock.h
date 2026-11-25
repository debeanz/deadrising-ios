#pragma once

#include <chrono>
#include <cstdint>

// A cheap clock for timing hot paths: the draw hooks time every draw, several times over. On ARM64 the CPU's virtual
// counter (one instruction; 24 MHz on Apple chips), elsewhere steady_clock. 2026-11-25: steady_clock::now() was 6%
// of the iPhone's drawing thread on the roof (2500 draws a frame).
namespace FastClock
{
#if defined(__aarch64__) || defined(_M_ARM64)
    inline uint64_t Ticks()
    {
        uint64_t ticks;
        __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(ticks));
        return ticks;
    }

    inline uint64_t ToNs(uint64_t ticks)
    {
        static const uint64_t frequency = []
        {
            uint64_t value;
            __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(value));
            return value != 0 ? value : 24000000ull;
        }();
        return ticks * 1000000000ull / frequency; // durations: no overflow below about 20 minutes
    }
#else
    inline uint64_t Ticks()
    {
        return uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
    }

    inline uint64_t ToNs(uint64_t ticks)
    {
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::duration(ticks)).count());
    }
#endif
}
