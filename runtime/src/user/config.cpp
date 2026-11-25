#include <stdafx.h>
#include "config.h"

#include <dr_shims.h>
#include <os/logger.h>

#include <fstream>
#include <sstream>
#include <string>

namespace
{
    std::string Trim(std::string s)
    {
        const size_t first = s.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return {};
        const size_t last = s.find_last_not_of(" \t\r\n");
        return s.substr(first, last - first + 1);
    }

    bool ParseBool(const std::string& value, bool fallback)
    {
        if (value == "true" || value == "1")
            return true;
        if (value == "false" || value == "0")
            return false;
        return fallback;
    }
}

std::filesystem::path Config::Path()
{
    return GetGamePath() / "recomp_config.toml";
}

void Config::Load()
{
    std::ifstream file(Path());
    if (!file)
    {
        LOGFN("[config] {} not found: defaults", (const char*)Path().u8string().c_str());
        Save();
        return;
    }

    // A small TOML subset: [sections], key = value, # comments.
    std::string line;
    while (std::getline(file, line))
    {
        if (const size_t hash = line.find('#'); hash != std::string::npos)
            line.resize(hash);
        const size_t equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        const std::string key = Trim(line.substr(0, equals));
        const std::string value = Trim(line.substr(equals + 1));
        if (key == "FrameRate")
            FrameRate = atoi(value.c_str()) >= 60 ? 60 : 30;
        else if (key == "VSync")
            VSync = ParseBool(value, VSync);
        else if (key == "Fullscreen")
            Fullscreen = ParseBool(value, Fullscreen);
        else if (key == "MSAA")
            MSAA = ParseBool(value, MSAA);
        else if (key == "FXAA")
            FXAA = ParseBool(value, FXAA);
        else if (key == "MotionBlur")
            MotionBlur = ParseBool(value, MotionBlur);
        else if (key == "ResolutionScale")
            ResolutionScale = std::clamp(std::round(float(atof(value.c_str())) * 2.0f) / 2.0f, 1.0f, kMaxResolutionScale);
        else if (key == "ShowFps")
            ShowFps = ParseBool(value, ShowFps);
        else if (key == "ScreenScaling")
            ScreenScaling = std::clamp(atoi(value.c_str()), 0, 2);
    }
    // Test windows: DR_SETTINGS="FrameRate=30;MotionBlur=true;..." overrides settings for this run
    // only, and DR_NO_SAVE=1 (implied by DR_SETTINGS) keeps the menu from writing the player's file.
    if (const char* overrides = getenv("DR_SETTINGS"))
    {
        std::string list = overrides;
        size_t start = 0;
        while (start < list.size())
        {
            size_t end = list.find(';', start);
            if (end == std::string::npos)
                end = list.size();
            const std::string item = list.substr(start, end - start);
            start = end + 1;
            const size_t equals = item.find('=');
            if (equals == std::string::npos)
                continue;
            const std::string key = Trim(item.substr(0, equals)), value = Trim(item.substr(equals + 1));
            if (key == "FrameRate") FrameRate = atoi(value.c_str()) >= 60 ? 60 : 30;
            else if (key == "VSync") VSync = ParseBool(value, VSync);
            else if (key == "MSAA") MSAA = ParseBool(value, MSAA);
            else if (key == "FXAA") FXAA = ParseBool(value, FXAA);
            else if (key == "MotionBlur") MotionBlur = ParseBool(value, MotionBlur);
            else if (key == "ResolutionScale") ResolutionScale = std::clamp(std::round(float(atof(value.c_str())) * 2.0f) / 2.0f, 1.0f, kMaxResolutionScale);
            else if (key == "ShowFps") ShowFps = ParseBool(value, ShowFps);
            else if (key == "ScreenScaling") ScreenScaling = std::clamp(atoi(value.c_str()), 0, 2);
        }
        LOGFN("[config] DR_SETTINGS overrides: {} (not saved)", overrides);
    }
    LOGFN("[config] loaded: {} fps, vsync {}, fullscreen {}, MSAA {}, FXAA {}, motion blur {}, resolution scale {}, show fps {}, screen scaling {}",
        FrameRate.load(), VSync.load(), Fullscreen.load(), MSAA.load(), FXAA.load(), MotionBlur.load(), ResolutionScale.load(), ShowFps.load(), ScreenScaling.load());
}

void Config::Save()
{
    if (getenv("DR_SETTINGS") != nullptr || getenv("DR_NO_SAVE") != nullptr)
    {
        LOGN("[config] test run (DR_SETTINGS / DR_NO_SAVE): settings not saved");
        return;
    }
    std::ostringstream out;
    out << "# Dead Rising (recompiled) settings. Also in game: Options (F1 on a keyboard, Start + Back on a controller).\n"
        << "[Video]\n"
        << "FrameRate = " << FrameRate.load() << "           # 30 (as on the console) or 60\n"
        << "VSync = " << (VSync ? "true" : "false") << "\n"
        << "Fullscreen = " << (Fullscreen ? "true" : "false") << "\n"
        << "MSAA = " << (MSAA ? "true" : "false") << "                # the console's 4X multisample anti-aliasing\n"
        << "FXAA = " << (FXAA ? "true" : "false") << "                # smoothing filter on the final image\n"
        << "MotionBlur = " << (MotionBlur ? "true" : "false") << "\n"
        << "ResolutionScale = " << ResolutionScale.load() << "     # internal resolution: 1 = 720p (the console's), 1.5 = 1080p, 2 = 1440p, 3 = 2160p\n"
        << "ShowFps = " << (ShowFps ? "true" : "false") << "            # frame counter in the top-left corner\n"
        << "ScreenScaling = " << ScreenScaling.load() << "         # 0 fit (black bars), 1 fill (stretched), 2 fill 1.943:1 (bars beside the Dynamic Island)\n";
    std::ofstream file(Path(), std::ios::binary | std::ios::trunc);
    file << out.str();
    if (!file)
        LOGFN_WARNING("[config] could not write {}", (const char*)Path().u8string().c_str());
}
