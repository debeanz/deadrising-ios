#pragma once

// Settings (recomp_config.toml in the game folder; on iOS the Documents/game folder the Files app
// shows). Changed in game in the options menu (ui/options_menu.h). Read from any thread.

#include <atomic>
#include <filesystem>

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

    std::filesystem::path Path();
    void Load();
    void Save();
}
