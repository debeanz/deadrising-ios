#pragma once

#include <filesystem>

// The iPhone / iPad start screen (runtime/ios/Launcher.swift, src/os/ios_launcher.mm): settings from user/config.h
// and a check of the game files. Run shows it on the main thread and returns once Play is tapped with the files
// present: the settings are applied and saved, gamePath is the folder the files are in. Returns false when the
// launcher is not in the build (the game then starts as before).
namespace Launcher
{
    bool Run(const std::filesystem::path& documents, std::filesystem::path& gamePath);
}
