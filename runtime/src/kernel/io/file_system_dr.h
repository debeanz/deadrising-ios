#pragma once

#include <filesystem>
#include <string>
#include <string_view>

// Maps Xbox 360 paths ("game:\arc\rom\x.arc", "\Device\Cdrom0\...", "d:\...") to host files.
//
// Dead Rising dumps sometimes come with their folder structure flattened
// ("arc\rom\om\om0001.arc" stored as "arcromomom0001.arc"). Lookups first try the path as
// written (case-insensitive), then the same path with its separators removed, so both layouts work.
namespace FileSystemDR
{
    void Init(const std::filesystem::path& gameRoot, const std::filesystem::path& saveRoot);

    // Records ObCreateSymbolicLink("\??\game:", "\Device\Cdrom0").
    void AddSymbolicLink(std::string_view link, std::string_view target);
    void RemoveSymbolicLink(std::string_view link);

    struct Resolved
    {
        std::filesystem::path hostPath;
        bool exists = false;
        bool isDirectory = false;
        bool writable = false; // true for save/cache devices
    };

    // Returns false if the path does not belong to any known device.
    bool Resolve(std::string_view guestPath, Resolved& out);

    // Called after a file is created so later lookups find it.
    void NotifyCreated(const std::filesystem::path& hostPath);
}
