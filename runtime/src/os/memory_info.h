#pragma once

#include <cstdint>
#include <string>

// How much memory the app uses. On iPhone / iPad the system ends an app that goes over its limit without
// any message (2026-11-25: the build stopped at frame 8 with nothing in the log), so the log shows where
// it stands: "[memory]" lines.
namespace MemoryInfo
{
    uint64_t FootprintMB();  // what the system counts against the app (Apple: phys_footprint; Windows: private bytes)
    int64_t AvailableMB();   // iOS: how much more before the system ends the app; -1 elsewhere
    std::string Describe();  // "footprint N MB, M MB left"
}
