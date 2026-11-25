#pragma once

// Small stand-ins for UnleashedRecomp systems the kernel layer references
// (user config, paths). Real versions come later (settings UI, iOS sandbox paths).

#include <filesystem>
#include <cstdint>

enum class ELanguage : uint32_t
{
    English = 1,
    Japanese = 2,
    German = 3,
    French = 4,
    Spanish = 5,
    Italian = 6,
};

struct LanguageSetting
{
    ELanguage Value = ELanguage::English;
    bool operator==(ELanguage other) const { return Value == other; }
};

namespace Config
{
    inline LanguageSetting Language{};
}

// Folder that holds default.xex and the game data (set once at startup).
inline std::filesystem::path g_gamePath;

inline std::filesystem::path GetGamePath()
{
    return g_gamePath;
}

inline std::filesystem::path GetSavePath(bool /*checkForMods*/)
{
    return g_gamePath / "_recomp_save";
}
