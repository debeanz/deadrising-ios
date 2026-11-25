#include <stdafx.h>
#include "cpu_sampler.h"
#include <os/logger.h>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")

namespace
{
    constexpr int kMaxDepth = 48;

    struct Thread
    {
        HANDLE handle = nullptr;
        DWORD id = 0;
        std::string name;
        std::vector<uint64_t> stacks; // frames of every sample, innermost first
        std::vector<uint8_t> depths;
    };

    bool Enabled()
    {
        static const bool enabled = [] {
            const char* value = getenv("DR_CPU_PROFILE");
            return value != nullptr && atoi(value) != 0;
        }();
        return enabled;
    }

    // Symbols: the sampler thread and crash reports share one DbgHelp session.
    void InitSymbols()
    {
        static std::once_flag once;
        std::call_once(once, [] {
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
            if (!SymInitialize(GetCurrentProcess(), nullptr, TRUE))
                LOGN_WARNING("[cpu profile] SymInitialize failed: addresses only");
        });
    }

    std::mutex g_threadsMutex;
    std::vector<std::unique_ptr<Thread>> g_threads;

    // Reads a suspended thread's stack: frame 0 is the instruction pointer, the others return
    // addresses (x64 unwind data; a frame without it is a leaf whose return address is at rsp).
    int WalkStack(CONTEXT ctx, uint64_t* frames)
    {
        int depth = 0;
        while (depth < kMaxDepth && ctx.Rip > 0x10000)
        {
            frames[depth++] = ctx.Rip;
            uint64_t imageBase = 0;
            PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
            if (function == nullptr)
            {
                if (ctx.Rsp == 0 || (ctx.Rsp & 7) != 0)
                    break;
                ctx.Rip = *reinterpret_cast<const uint64_t*>(ctx.Rsp);
                ctx.Rsp += 8;
                continue;
            }
            void* handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, function, &ctx, &handlerData, &establisher, nullptr);
        }
        return depth;
    }

    struct Names
    {
        std::unordered_map<uint64_t, uint32_t> byAddress; // pc -> name index
        std::vector<std::string> names;
        std::vector<bool> guest, waiting;
        std::unordered_map<std::string, uint32_t> indices;

        uint32_t Get(uint64_t pc)
        {
            if (auto it = byAddress.find(pc); it != byAddress.end())
                return it->second;
            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 511;
            DWORD64 displacement = 0;
            std::string name;
            if (SymFromAddr(GetCurrentProcess(), pc, &displacement, symbol))
                name.assign(symbol->Name, symbol->NameLen);
            else
            {
                IMAGEHLP_MODULE64 module{};
                module.SizeOfStruct = sizeof(module);
                name = SymGetModuleInfo64(GetCurrentProcess(), pc, &module) ? fmt::format("{}!?", module.ModuleName) : fmt::format("{:X}", pc);
            }
            // Recompiled functions appear twice (__imp__sub_X, the body, and sub_X, the hookable
            // wrapper): one name.
            if (name.rfind("__imp__", 0) == 0)
                name.erase(0, 7);
            auto [it, inserted] = indices.try_emplace(name, uint32_t(names.size()));
            if (inserted)
            {
                names.push_back(name);
                guest.push_back(name.rfind("sub_", 0) == 0);
                waiting.push_back(name.find("WaitFor") != std::string::npos || name.find("WaitOnAddress") != std::string::npos ||
                    name.find("ZwDelayExecution") != std::string::npos || name.find("NtDelayExecution") != std::string::npos ||
                    name.find("ZwRemoveIoCompletion") != std::string::npos);
            }
            byAddress.emplace(pc, it->second);
            return it->second;
        }
    };

    void Report(Names& names, Thread& thread, double seconds, std::string& out)
    {
        const size_t samples = thread.depths.size();
        if (samples == 0)
            return;
        std::unordered_map<uint32_t, uint32_t> self, inclusive;
        std::map<std::vector<uint32_t>, uint32_t> paths;
        std::vector<uint32_t> seen, path;
        uint32_t waits = 0;
        size_t offset = 0;
        for (uint8_t depth : thread.depths)
        {
            seen.clear();
            path.clear();
            for (int i = 0; i < depth; i++)
            {
                const uint32_t index = names.Get(thread.stacks[offset + i]);
                if (i == 0)
                {
                    self[index]++;
                    if (names.waiting[index])
                        waits++;
                }
                if (std::find(seen.begin(), seen.end(), index) == seen.end())
                {
                    seen.push_back(index);
                    inclusive[index]++;
                }
                // Call path: the innermost frame, then the innermost five recompiled functions.
                if ((i == 0 || names.guest[index]) && path.size() < 6 && (path.empty() || path.back() != index))
                    path.push_back(index);
            }
            paths[path]++;
            offset += depth;
        }
        const double total = double(samples);
        auto sorted = [](const auto& counts, size_t count) {
            std::vector<std::pair<uint32_t, uint32_t>> order(counts.begin(), counts.end());
            std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            if (order.size() > count)
                order.resize(count);
            return order;
        };
        out += fmt::format("[cpu profile] {}: {} samples in {:.1f} s, {:.1f}% waiting\n", thread.name, samples, seconds, 100.0 * waits / total);
        for (auto [index, count] : sorted(self, 12))
            out += fmt::format("[cpu profile]   self {:5.1f}%  incl {:5.1f}%  {}\n", 100.0 * count / total, 100.0 * inclusive[index] / total, names.names[index]);
        uint32_t shown = 0;
        for (auto [index, count] : sorted(inclusive, 60))
        {
            if (count > samples * 95 / 100 || !names.guest[index] || shown >= 16)
                continue;
            shown++;
            out += fmt::format("[cpu profile]   incl {:5.1f}%  {}\n", 100.0 * count / total, names.names[index]);
        }
        std::vector<std::pair<std::vector<uint32_t>, uint32_t>> pathOrder(paths.begin(), paths.end());
        std::sort(pathOrder.begin(), pathOrder.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        for (size_t i = 0; i < pathOrder.size() && i < 10; i++)
        {
            std::string text;
            for (uint32_t index : pathOrder[i].first)
                text += (text.empty() ? "" : " < ") + names.names[index];
            out += fmt::format("[cpu profile]   path {:5.1f}%  {}\n", 100.0 * pathOrder[i].second / total, text);
        }
    }

    void SamplerThread()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        timeBeginPeriod(1);
        InitSymbols();

        Names names;
        uint64_t frames[kMaxDepth];
        auto start = std::chrono::steady_clock::now();
        while (true)
        {
            Sleep(1);
            {
                std::lock_guard lock(g_threadsMutex);
                for (auto& thread : g_threads)
                {
                    int depth = 0;
                    if (SuspendThread(thread->handle) == DWORD(-1))
                        continue;
                    CONTEXT context{};
                    context.ContextFlags = CONTEXT_FULL;
                    if (GetThreadContext(thread->handle, &context))
                        depth = WalkStack(context, frames);
                    ResumeThread(thread->handle);
                    // Nothing above allocates: the thread may have been holding the heap lock.
                    if (depth > 0)
                    {
                        thread->stacks.insert(thread->stacks.end(), frames, frames + depth);
                        thread->depths.push_back(uint8_t(depth));
                    }
                }
            }
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            if (seconds >= 10.0)
            {
                // One log call for the whole report: line by line the logger's lock was held long
                // enough to stall the game thread's own log lines (a 50-100 ms hitch every 10 s).
                std::string out = fmt::format("[cpu profile] ---- {} threads over {:.1f} s ----\n", g_threads.size(), seconds);
                {
                    std::lock_guard lock(g_threadsMutex);
                    for (auto& thread : g_threads)
                    {
                        Report(names, *thread, seconds, out);
                        thread->stacks.clear();
                        thread->depths.clear();
                    }
                }
                if (!out.empty() && out.back() == '\n')
                    out.pop_back();
                LOGFN("{}", out);
                start = std::chrono::steady_clock::now();
            }
        }
    }
}

// Hitch profile: the game's presenting thread, its drawing thread and the GPU command processor
// sampled every 4 ms into one ring each (the last ~2 s); a frame that took too long is reported from
// the rings on the sampler thread (symbols load there, not in the game).
namespace
{
    namespace HitchProfile
    {
        constexpr int kRing = 512;
        constexpr int kThreads = 4;
        struct Sample
        {
            int64_t time = 0;
            int depth = 0;
            uint64_t frames[kMaxDepth];
        };
        struct Watched
        {
            HANDLE handle = nullptr;
            DWORD id = 0;
            char name[48] = {};
            Sample ring[kRing];
            uint32_t next = 0;
        };
        Watched g_watched[kThreads];
        std::atomic<int> g_count{ 0 };
        std::mutex g_watchMutex;
        struct Request
        {
            uint32_t frame;
            int64_t from, to;
        };
        std::mutex g_requestMutex;
        std::vector<Request> g_requests;

        bool Enabled()
        {
            static const bool enabled = [] {
                const char* value = getenv("DR_HITCH_PROFILE");
                return value == nullptr || atoi(value) != 0;
            }();
            return enabled;
        }

        int64_t Now()
        {
            return std::chrono::steady_clock::now().time_since_epoch().count();
        }

        // One thread's samples in the request's window: "<name>: N samples, X% waiting", then (when it
        // was not just waiting) self time, guest functions and call paths.
        void Report(Names& names, const Watched& watched, const Request& request, std::string& out)
        {
            std::unordered_map<uint32_t, uint32_t> self, inclusive;
            std::map<std::vector<uint32_t>, uint32_t> paths;
            std::vector<uint32_t> seen, path;
            uint32_t samples = 0, waits = 0;
            for (const Sample& sample : watched.ring)
            {
                if (sample.depth == 0 || sample.time < request.from || sample.time >= request.to)
                    continue;
                samples++;
                seen.clear();
                path.clear();
                for (int i = 0; i < sample.depth; i++)
                {
                    const uint32_t index = names.Get(sample.frames[i]);
                    if (i == 0)
                    {
                        self[index]++;
                        if (names.waiting[index])
                            waits++;
                    }
                    if (std::find(seen.begin(), seen.end(), index) == seen.end())
                    {
                        seen.push_back(index);
                        if (names.guest[index])
                            inclusive[index]++;
                    }
                    if ((i == 0 || names.guest[index]) && path.size() < 7 && (path.empty() || path.back() != index))
                        path.push_back(index);
                }
                paths[path]++;
            }
            out += fmt::format("\n[hitch profile]  {}: {} samples, {:.0f}% waiting", watched.name, samples, samples ? 100.0 * waits / samples : 0.0);
            if (samples == 0 || waits * 10 >= samples * 9)
                return; // idle: nothing to show
            auto sorted = [](const auto& counts, size_t count) {
                std::vector<std::pair<uint32_t, uint32_t>> order(counts.begin(), counts.end());
                std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
                if (order.size() > count)
                    order.resize(count);
                return order;
            };
            out += "\n[hitch profile]   self:";
            for (auto [index, count] : sorted(self, 8))
                out += fmt::format(" {:.0f}% {},", 100.0 * count / samples, names.names[index]);
            out += "\n[hitch profile]   guest functions (inclusive):";
            uint32_t shown = 0;
            for (auto [index, count] : sorted(inclusive, 48))
            {
                if (count == samples && inclusive.size() > 16)
                    continue; // the thread's loop itself
                if (shown++ >= 16)
                    break;
                out += fmt::format(" {:.0f}% {},", 100.0 * count / samples, names.names[index]);
            }
            std::vector<std::pair<std::vector<uint32_t>, uint32_t>> order(paths.begin(), paths.end());
            std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            for (size_t i = 0; i < order.size() && i < 6; i++)
            {
                std::string text;
                for (uint32_t index : order[i].first)
                    text += (text.empty() ? "" : " < ") + names.names[index];
                out += fmt::format("\n[hitch profile]   path {:.0f}%: {}", 100.0 * order[i].second / samples, text);
            }
        }

        void Report(Names& names, const Request& request)
        {
            const double ms = double(request.to - request.from) * std::chrono::steady_clock::period::num * 1000.0 /
                std::chrono::steady_clock::period::den;
            std::string out = fmt::format("[hitch profile] frame {}: {:.0f} ms", request.frame, ms);
            const int count = g_count.load(std::memory_order_acquire);
            for (int i = 0; i < count; i++)
                Report(names, g_watched[i], request, out);
            LOGFN("{}", out);
        }

        void Thread()
        {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            timeBeginPeriod(1);
            Names names;
            bool symbols = false;
            uint64_t frames[kMaxDepth];
            std::vector<Request> requests;
            while (true)
            {
                Sleep(4);
                const int count = g_count.load(std::memory_order_acquire);
                for (int t = 0; t < count; t++)
                {
                    Watched& watched = g_watched[t];
                    int depth = 0;
                    if (SuspendThread(watched.handle) != DWORD(-1))
                    {
                        CONTEXT context{};
                        context.ContextFlags = CONTEXT_FULL;
                        if (GetThreadContext(watched.handle, &context))
                            depth = WalkStack(context, frames);
                        ResumeThread(watched.handle);
                    }
                    // Nothing above allocates (the sampled thread may have held the heap lock).
                    Sample& sample = watched.ring[watched.next++ % kRing];
                    sample.time = Now();
                    sample.depth = depth;
                    memcpy(sample.frames, frames, sizeof(uint64_t) * depth);
                }
                {
                    std::lock_guard lock(g_requestMutex);
                    requests.swap(g_requests);
                }
                for (const Request& request : requests)
                {
                    if (!symbols)
                    {
                        InitSymbols();
                        symbols = true;
                    }
                    Report(names, request);
                }
                requests.clear();
            }
        }
    }
}

void CpuSampler::WatchHitchThread(const char* name)
{
    if (!HitchProfile::Enabled())
        return;
    const DWORD id = GetCurrentThreadId();
    std::lock_guard lock(HitchProfile::g_watchMutex);
    const int count = HitchProfile::g_count.load(std::memory_order_relaxed);
    for (int i = 0; i < count; i++)
        if (HitchProfile::g_watched[i].id == id)
            return;
    if (count >= HitchProfile::kThreads)
        return;
    HitchProfile::Watched& watched = HitchProfile::g_watched[count];
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &watched.handle,
            THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
        return;
    watched.id = id;
    strncpy(watched.name, name, sizeof(watched.name) - 1);
    HitchProfile::g_count.store(count + 1, std::memory_order_release);
    LOGFN("[hitch profile] sampling the {} every 4 ms; frames over 50 ms of game work are reported (DR_HITCH_PROFILE=0 off)", name);
    if (count == 0)
        std::thread(HitchProfile::Thread).detach();
}

void CpuSampler::WatchHitches()
{
    WatchHitchThread("game thread (presents)");
}

void CpuSampler::ReportHitch(uint32_t frame, std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to)
{
    if (HitchProfile::g_count.load(std::memory_order_acquire) == 0)
        return;
    std::lock_guard lock(HitchProfile::g_requestMutex);
    if (HitchProfile::g_requests.size() < 8)
        HitchProfile::g_requests.push_back({ frame, from.time_since_epoch().count(), to.time_since_epoch().count() });
}

void CpuSampler::RegisterCurrentThread(const char* name)
{
    if (!Enabled())
        return;
    static std::once_flag started;
    const DWORD id = GetCurrentThreadId();
    {
        std::lock_guard lock(g_threadsMutex);
        for (auto& thread : g_threads)
            if (thread->id == id)
                return;
        auto thread = std::make_unique<Thread>();
        thread->id = id;
        thread->name = name;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread->handle,
            THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
        g_threads.push_back(std::move(thread));
    }
    LOGFN("[cpu profile] sampling {} (DR_CPU_PROFILE)", name);
    std::call_once(started, [] { std::thread(SamplerThread).detach(); });
}
void CpuSampler::LogStack(const void* context)
{
    InitSymbols();
    uint64_t frames[kMaxDepth];
    const int depth = WalkStack(*static_cast<const CONTEXT*>(context), frames);
    const uint64_t exeBase = uint64_t(GetModuleHandleW(nullptr));
    for (int i = 0; i < depth; i++)
    {
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 511;
        DWORD64 displacement = 0;
        const bool named = SymFromAddr(GetCurrentProcess(), frames[i], &displacement, symbol);
        LOGFN_ERROR("       #{:2} {}+0x{:X} (DRRecomp.exe+0x{:X})", i, named ? std::string(symbol->Name, symbol->NameLen) : std::string("?"),
            named ? displacement : 0, frames[i] - exeBase);
    }
}
#else
void CpuSampler::WatchHitches() {}
void CpuSampler::WatchHitchThread(const char*) {}
void CpuSampler::ReportHitch(uint32_t, std::chrono::steady_clock::time_point, std::chrono::steady_clock::time_point) {}
void CpuSampler::RegisterCurrentThread(const char*) {}
void CpuSampler::LogStack(const void*) {}
#endif
