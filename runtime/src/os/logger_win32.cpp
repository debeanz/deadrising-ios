#include <stdafx.h>
#include <os/logger.h>

// Console + file logger. Always on: while bringing the game up, every log line matters.

static std::mutex g_logMutex;
static FILE* g_logFile;
constexpr size_t kRecentLines = 300;
static std::string g_recent[kRecentLines]; // ring of the last lines (RecentLines)
static size_t g_recentCount = 0;

void os::logger::Init()
{
    g_logFile = fopen("DRRecomp.log", "w");
}

void os::logger::Log(const std::string_view str, ELogType type, const char* func)
{
    static constexpr const char* prefixes[] = { "", "[util] ", "[warn] ", "[error] " };
    const char* prefix = prefixes[size_t(type)];

    std::lock_guard lock(g_logMutex);
    std::string line = func ? fmt::format("{}[{}] {}\n", prefix, func, str) : fmt::format("{}{}\n", prefix, str);

    fputs(line.c_str(), stdout);
    fflush(stdout);

    if (g_logFile)
    {
        fputs(line.c_str(), g_logFile);
        fflush(g_logFile);
    }

    line.pop_back(); // the newline
    g_recent[g_recentCount++ % kRecentLines] = std::move(line);
}

std::vector<std::string> os::logger::RecentLines(size_t count)
{
    std::lock_guard lock(g_logMutex);
    count = std::min({ count, g_recentCount, kRecentLines });
    std::vector<std::string> lines;
    for (size_t i = g_recentCount - count; i < g_recentCount; i++)
        lines.push_back(g_recent[i % kRecentLines]);
    return lines;
}

void os::logger::LogCrash(const std::string_view str)
{
    // From a crash handler: the thread that crashed may hold the log's lock, so wait for it only briefly
    // and write anyway.
    std::unique_lock lock(g_logMutex, std::defer_lock);
    for (int attempt = 0; attempt < 100 && !lock.try_lock(); attempt++)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const std::string line = fmt::format("[error] {}\n", str);
    fputs(line.c_str(), stdout);
    fflush(stdout);
    if (g_logFile)
    {
        fputs(line.c_str(), g_logFile);
        fflush(g_logFile);
    }
}
