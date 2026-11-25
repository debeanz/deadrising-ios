#include <stdafx.h>
#include "file_system_dr.h"
#include <kernel/xam.h>
#include <os/logger.h>

namespace
{
    struct DeviceRoot
    {
        std::filesystem::path root;
        bool writable = false;

        // lower-case relative path ("arc/rom/x.arc") -> host path
        std::unordered_map<std::string, std::filesystem::path> exact;
        // lower-case relative path without separators ("arcromx.arc") -> host path
        std::unordered_map<std::string, std::filesystem::path> flat;
        bool indexed = false;
    };

    std::mutex g_mutex;
    DeviceRoot g_game;
    DeviceRoot g_save;
    std::unordered_map<std::string, std::string> g_symlinks; // lower-case link -> target

    std::string ToLower(std::string_view s)
    {
        std::string r(s);
        std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return r;
    }

    std::string Flatten(std::string_view s)
    {
        std::string r;
        r.reserve(s.size());
        for (char c : s)
        {
            if (c != '/' && c != '\\')
                r += (char)std::tolower((unsigned char)c);
        }
        return r;
    }

    void Index(DeviceRoot& device)
    {
        device.exact.clear();
        device.flat.clear();

        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(device.root, ec);
             it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        {
            if (ec)
                break;

            // Skip our own working folders inside the game directory.
            if (it.depth() == 0 && it->is_directory(ec))
            {
                auto name = it->path().filename().u8string();
                if (!name.empty() && name[0] == u8'_')
                {
                    it.disable_recursion_pending();
                    continue;
                }
            }

            auto rel = std::filesystem::relative(it->path(), device.root, ec).generic_u8string();
            std::string relStr((const char*)rel.c_str());
            device.exact.emplace(ToLower(relStr), it->path());
            device.flat.emplace(Flatten(relStr), it->path());
        }

        device.indexed = true;
        LOGFN("Indexed {} entries under {}", device.exact.size(), (const char*)device.root.u8string().c_str());
    }

    bool Lookup(DeviceRoot& device, std::string_view relative, FileSystemDR::Resolved& out)
    {
        if (!device.indexed)
            Index(device);

        std::string rel(relative);
        std::replace(rel.begin(), rel.end(), '\\', '/');
        while (!rel.empty() && rel.front() == '/')
            rel.erase(rel.begin());
        while (!rel.empty() && rel.back() == '/')
            rel.pop_back();

        out.writable = device.writable;

        if (rel.empty())
        {
            out.hostPath = device.root;
            out.exists = true;
            out.isDirectory = true;
            return true;
        }

        std::filesystem::path found;
        if (auto it = device.exact.find(ToLower(rel)); it != device.exact.end())
            found = it->second;
        else if (auto it = device.flat.find(Flatten(rel)); it != device.flat.end())
            found = it->second;

        std::error_code ec;
        if (!found.empty())
        {
            out.hostPath = found;
            out.exists = true;
            out.isDirectory = std::filesystem::is_directory(found, ec);
            return true;
        }

        // Not found: where it would be created (writable devices only matter here).
        out.hostPath = device.root / std::filesystem::path(std::u8string_view((const char8_t*)rel.data(), rel.size()));
        out.exists = false;
        out.isDirectory = false;
        return true;
    }
}

void FileSystemDR::Init(const std::filesystem::path& gameRoot, const std::filesystem::path& saveRoot)
{
    std::lock_guard lock(g_mutex);

    g_game.root = gameRoot;
    g_game.writable = false;
    g_save.root = saveRoot;
    g_save.writable = true;

    std::error_code ec;
    std::filesystem::create_directories(saveRoot, ec);
}

void FileSystemDR::AddSymbolicLink(std::string_view link, std::string_view target)
{
    std::lock_guard lock(g_mutex);
    std::string key = ToLower(link);
    if (key.starts_with("\\??\\"))
        key.erase(0, 4);
    g_symlinks[key] = std::string(target);
}

void FileSystemDR::RemoveSymbolicLink(std::string_view link)
{
    std::lock_guard lock(g_mutex);
    std::string key = ToLower(link);
    if (key.starts_with("\\??\\"))
        key.erase(0, 4);
    g_symlinks.erase(key);
}

bool FileSystemDR::Resolve(std::string_view guestPath, Resolved& out)
{
    std::lock_guard lock(g_mutex);

    std::string path(guestPath);
    std::replace(path.begin(), path.end(), '/', '\\');

    if (path.starts_with("\\??\\"))
        path.erase(0, 4);

    // Expand symbolic links ("game:" -> "\Device\Cdrom0"), longest prefix first.
    for (int depth = 0; depth < 4; depth++)
    {
        std::string lower = ToLower(path);
        size_t best = 0;
        const std::string* target = nullptr;
        for (auto& [link, dest] : g_symlinks)
        {
            if (link.size() > best && lower.starts_with(link) &&
                (lower.size() == link.size() || lower[link.size()] == '\\'))
            {
                best = link.size();
                target = &dest;
            }
        }

        if (!target)
            break;

        path = *target + path.substr(best);
    }

    std::string lower = ToLower(path);

    auto stripPrefix = [&](std::string_view prefix, std::string& rest) -> bool
    {
        if (lower.starts_with(prefix) && (lower.size() == prefix.size() || lower[prefix.size()] == '\\'))
        {
            rest = path.substr(prefix.size());
            return true;
        }
        return false;
    };

    std::string rest;
    if (stripPrefix("\\device\\cdrom0", rest) || stripPrefix("game:", rest) || stripPrefix("d:", rest))
        return Lookup(g_game, rest, out);

    if (stripPrefix("\\device\\harddisk0\\partition1", rest) || stripPrefix("\\device\\harddisk0\\partition0", rest) ||
        stripPrefix("\\device\\harddisk0\\systempartition", rest) || stripPrefix("cache:", rest) ||
        stripPrefix("e:", rest))
    {
        return Lookup(g_save, rest, out);
    }

    // Roots registered through XamContentCreateEx / XamRootCreate (e.g. a save container "save:").
    size_t colon = path.find(':');
    if (colon != std::string::npos && path.find('\\') > colon)
    {
        std::string root = path.substr(0, colon);
        std::string_view mapped = XamGetRootPath(root);
        if (!mapped.empty())
        {
            DeviceRoot device;
            device.root = std::filesystem::path(std::u8string_view((const char8_t*)mapped.data(), mapped.size()));
            device.writable = true;
            Index(device);
            return Lookup(device, path.substr(colon + 1), out);
        }
    }

    // Bare relative path: treat as game-relative.
    if (colon == std::string::npos && !path.starts_with("\\device\\"))
        return Lookup(g_game, path, out);

    return false;
}

void FileSystemDR::NotifyCreated(const std::filesystem::path& hostPath)
{
    std::lock_guard lock(g_mutex);

    for (DeviceRoot* device : { &g_game, &g_save })
    {
        std::error_code ec;
        auto rel = std::filesystem::relative(hostPath, device->root, ec);
        if (ec || rel.empty() || rel.native().starts_with(std::filesystem::path("..").native()))
            continue;

        auto relU8 = rel.generic_u8string();
        std::string relStr((const char*)relU8.c_str());
        device->exact[ToLower(relStr)] = hostPath;
        device->flat[Flatten(relStr)] = hostPath;
    }
}
