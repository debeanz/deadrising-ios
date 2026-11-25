#pragma once

// Settings (recomp_config.toml in the game folder; on iOS the Documents/game folder the Files app
// shows). Changed in game in the options menu (ui/options_menu.h). Read from any thread.

#include <atomic>
#include <filesystem>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace Config
{
    inline std::atomic<int> FrameRate{ 30 };            // 30 (the console's) or 60
    inline std::atomic<bool> VSync{ true };
    inline std::atomic<bool> Fullscreen{ false };
    inline std::atomic<bool> MSAA{ true };              // the console's 4X MSAA
    inline std::atomic<bool> FXAA{ true };              // smoothing filter on the final image
    inline std::atomic<bool> MotionBlur{ true };
    inline std::atomic<float> ResolutionScale{ 1.0f };  // internal resolution (1.0 = 720p, the console's)
    inline std::atomic<bool> ShowFps{ false };          // frame counter in the top-left corner
    // Where the 16:9 picture goes on screens of another shape: 0 Fit (whole picture, black bars), 1 Fill (stretched over
    // the screen), 2 Fill 1.943:1 (stretched over a centred 1.943:1 frame: bars beside an iPhone's Dynamic Island).
    inline std::atomic<int> ScreenScaling{ 0 };

    // Highest internal resolution: 2160p on PCs, 1440p on iPhones and iPads (their GPU spends most of a frame storing
    // and loading render targets; at 2160p the game ran at 9 fps on an iPhone 15).
#if defined(__APPLE__) && TARGET_OS_IPHONE
    constexpr float kMaxResolutionScale = 2.0f;
#else
    constexpr float kMaxResolutionScale = 3.0f;
#endif

    std::filesystem::path Path();
    void Load();
    void Save();
}
