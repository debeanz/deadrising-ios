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
    void WalkFrames(CONTEXT& ctx, uint64_t* frames, int& depth)
    {
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
    }

    // A thread stopped where a frame is not set up yet (its frame register still holding something else) unwinds
    // into memory that is not there (2026-11-25: a crash in the hitch profiler, fault at 0x8218FA18): the sample
    // ends at that frame.
    int WalkStack(CONTEXT ctx, uint64_t* frames)
    {
        int depth = 0;
        __try
        {
            WalkFrames(ctx, frames, depth);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
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
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <dlfcn.h>
#include <cxxabi.h>
#include <unistd.h>
#include <sys/resource.h>
#include <kernel/park.h>

// libproc (not in the iOS SDK's headers; in its libraries since iOS 7).
extern "C" int proc_pid_rusage(int pid, int flavor, rusage_info_t* buffer);

// Apple (iPhone, iPad): the CPU profile of the threads the frame waits for. A sampler thread stops each watched thread
// every 2 ms, reads its call stack (frame-pointer chain) and lets it go on; every 10 s it logs, per thread,
// the functions with the most samples ("[cpu]": self = where the thread was, total = anywhere on its stack), runtime and
// recompiled game functions (sub_XXXXXXXX) alike. 2026-11-25: the iPhone's draw hooks took 5 times a PC's; this says
// where. DR_HITCH_PROFILE=0 turns it off. Nothing is allocated or locked while a thread is stopped.
namespace
{
    constexpr int kThreads = 8, kMaxDepth = 40;
    constexpr uintptr_t kAddressMask = 0x0000000FFFFFFFFFull; // no pointer authentication bits

    struct Watched
    {
        char name[48] = {};
        mach_port_t thread = MACH_PORT_NULL;
        uintptr_t low = 0, high = 0;
    };
    Watched g_watched[kThreads];
    const mach_port_t g_mainThread = pthread_mach_thread_np(pthread_self()); // static initialisation runs on the main thread
    std::atomic<int> g_watchedCount{ 0 };
    std::mutex g_watchMutex;

    bool Enabled()
    {
        static const bool enabled = getenv("DR_HITCH_PROFILE") == nullptr || atoi(getenv("DR_HITCH_PROFILE")) != 0;
        return enabled;
    }

    struct Symbol
    {
        uintptr_t start = 0;
        std::string name;
    };

    // The function an address is in (dladdr: the executable keeps its symbols), demangled and without its parameters.
    const Symbol& Lookup(uintptr_t pc, std::unordered_map<uintptr_t, Symbol>& cache)
    {
        auto it = cache.find(pc);
        if (it != cache.end())
            return it->second;
        Symbol symbol;
        Dl_info info{};
        if (dladdr(reinterpret_cast<void*>(pc), &info) != 0 && info.dli_sname != nullptr)
        {
            symbol.start = uintptr_t(info.dli_saddr);
            int status = 0;
            char* demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
            std::string name = status == 0 && demangled != nullptr ? demangled : info.dli_sname;
            free(demangled);
            // Without the parameter list (but not cutting "(anonymous namespace)").
            for (size_t i = 1; i < name.size(); i++)
                if (name[i] == '(' && name.compare(i, 21, "(anonymous namespace)") != 0)
                {
                    name.resize(i);
                    break;
                }
            const std::string anonymous = "(anonymous namespace)::";
            for (size_t at; (at = name.find(anonymous)) != std::string::npos;)
                name.erase(at, anonymous.size());
            symbol.name = name.size() > 70 ? name.substr(0, 70) : name;
        }
        else
        {
            symbol.start = pc;
            const char* file = info.dli_fname ? strrchr(info.dli_fname, '/') : nullptr;
            symbol.name = fmt::format("{}+{:X}", file ? file + 1 : "?", info.dli_fbase ? pc - uintptr_t(info.dli_fbase) : pc);
        }
        return cache.emplace(pc, std::move(symbol)).first->second;
    }

    struct Profile
    {
        uint32_t samples = 0;
        std::unordered_map<uintptr_t, uint32_t> self;  // innermost pc -> samples
        std::vector<uintptr_t> stacks;                  // every sample's frames, 0-terminated (symbolized at report)
    };

    void Report(const Watched& watched, Profile& profile, std::unordered_map<uintptr_t, Symbol>& cache)
    {
        if (profile.samples == 0)
            return;
        std::unordered_map<std::string, uint32_t> self, total;
        for (const auto& [pc, count] : profile.self)
            self[Lookup(pc, cache).name] += count;
        std::vector<std::string_view> seen;
        for (size_t i = 0; i < profile.stacks.size(); i++)
        {
            if (profile.stacks[i] == 0)
            {
                seen.clear();
                continue;
            }
            const std::string& name = Lookup(profile.stacks[i], cache).name;
            if (std::find(seen.begin(), seen.end(), std::string_view(name)) == seen.end()) // once per sample (recursion)
            {
                seen.push_back(name);
                total[name]++;
            }
        }
        auto top = [&](const std::unordered_map<std::string, uint32_t>& counts, size_t n) {
            std::vector<std::pair<uint32_t, const std::string*>> sorted;
            for (const auto& [name, count] : counts)
                sorted.emplace_back(count, &name);
            std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            std::string text;
            for (size_t i = 0; i < sorted.size() && i < n; i++)
                text += fmt::format("{}{:.0f}% {}", i ? ", " : "", 100.0 * sorted[i].first / profile.samples, *sorted[i].second);
            return text;
        };
        LOGFN("[cpu] {}: {} samples; self {}", watched.name, profile.samples, top(self, 14));
        LOGFN("[cpu] {}: total {}", watched.name, top(total, 22));
    }

    // Every thread of the app, every 10 s: its share of one core over the period ("[cpu] threads"), by name (the
    // watched threads' names, else the thread's own: os/thread_qos.h NameThisThread). With the whole app's: CPU time
    // in all, the part of it on the performance cores, the GPU's busy time, wakeups and the energy counter. 2026-11-25:
    // the iPhone got hot at 30-60 fps; this says which thread spends the power, and whether it is CPU or GPU.
    void ReportThreads(std::unordered_map<uint64_t, uint64_t>& last, double seconds)
    {
        thread_act_array_t list = nullptr;
        mach_msg_type_number_t count = 0;
        if (task_threads(mach_task_self(), &list, &count) != KERN_SUCCESS)
            return;
        const bool first = last.empty();
        const mach_port_t mainThread = g_mainThread;
        std::unordered_map<uint64_t, uint64_t> now;
        std::unordered_map<std::string, std::pair<uint64_t, uint32_t>> byName; // ns, threads
        std::unordered_map<std::string, int> priorityByName; // the highest scheduling priority (47: QoS user-interactive)
        uint64_t allNs = 0;
        for (mach_msg_type_number_t i = 0; i < count; i++)
        {
            thread_identifier_info_data_t id{};
            mach_msg_type_number_t idCount = THREAD_IDENTIFIER_INFO_COUNT;
            thread_extended_info_data_t info{};
            mach_msg_type_number_t infoCount = THREAD_EXTENDED_INFO_COUNT;
            if (thread_info(list[i], THREAD_IDENTIFIER_INFO, reinterpret_cast<thread_info_t>(&id), &idCount) == KERN_SUCCESS &&
                thread_info(list[i], THREAD_EXTENDED_INFO, reinterpret_cast<thread_info_t>(&info), &infoCount) == KERN_SUCCESS)
            {
                thread_basic_info_data_t basic{};
                mach_msg_type_number_t basicCount = THREAD_BASIC_INFO_COUNT;
                if (thread_info(list[i], THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&basic), &basicCount) != KERN_SUCCESS)
                    basic = {};
                const uint64_t ns = (uint64_t(basic.user_time.seconds) + uint64_t(basic.system_time.seconds)) * 1000000000ull +
                    (uint64_t(basic.user_time.microseconds) + uint64_t(basic.system_time.microseconds)) * 1000ull;
                now[id.thread_id] = ns;
                auto it = last.find(id.thread_id);
                const uint64_t delta = it != last.end() ? (ns > it->second ? ns - it->second : 0) : ns; // new: since it started
                std::string name;
                const int watchedCount = g_watchedCount.load(std::memory_order_acquire);
                for (int w = 0; w < watchedCount; w++)
                    if (g_watched[w].thread == list[i])
                        name = g_watched[w].name;
                if (name.empty() && info.pth_name[0] != 0)
                    name.assign(info.pth_name, strnlen(info.pth_name, sizeof(info.pth_name)));
                if (name.empty())
                    name = list[i] == mainThread ? "main (UIKit, SDL events)" : "unnamed";
                auto& entry = byName[name];
                entry.first += delta;
                entry.second++;
                int& priority = priorityByName[name];
                priority = std::max(priority, int(info.pth_curpri));
                allNs += delta;
            }
            mach_port_deallocate(mach_task_self(), list[i]);
        }
        vm_deallocate(mach_task_self(), vm_address_t(list), vm_size_t(count * sizeof(thread_act_t)));
        last = std::move(now);
        if (first)
            return; // the first look: the times so far, the base for the next

        // The app as a whole: performance-core time, GPU time, wakeups, energy (task_power_info_v2).
        static uint64_t lastPtime = 0, lastGpuNs = 0, lastWakeups = 0, lastEnergy = 0, lastTaskCpu = 0;
        std::string task;
        task_power_info_v2_data_t power{};
        mach_msg_type_number_t powerCount = TASK_POWER_INFO_V2_COUNT;
        if (task_info(mach_task_self(), TASK_POWER_INFO_V2, reinterpret_cast<task_info_t>(&power), &powerCount) == KERN_SUCCESS)
        {
            const uint64_t taskCpu = power.cpu_energy.total_user + power.cpu_energy.total_system;
            const uint64_t wakeups = power.cpu_energy.task_interrupt_wakeups + power.cpu_energy.task_platform_idle_wakeups;
            const uint64_t gpuNs = power.gpu_energy.task_gpu_utilisation;
            uint64_t energy = 0;
#if defined(__arm64__) || defined(__aarch64__)
            energy = power.task_energy;
#endif
            if (lastTaskCpu != 0)
            {
                const double cpuNs = double(taskCpu - lastTaskCpu);
                task = fmt::format("; performance cores {:.0f}% of the CPU time, GPU busy {:.0f}%{}, {:.0f} wakeups/s, energy {:.2f} a second "
                                   "(task_energy: watts if nanojoules)",
                    cpuNs > 0 ? 100.0 * double(power.task_ptime - lastPtime) / cpuNs : 0.0, 100.0 * double(gpuNs - lastGpuNs) / (seconds * 1e9),
                    gpuNs == 0 ? " (not reported)" : "", double(wakeups - lastWakeups) / seconds, double(energy - lastEnergy) / 1e9 / seconds);
            }
            lastPtime = power.task_ptime;
            lastGpuNs = gpuNs;
            lastWakeups = wakeups;
            lastEnergy = energy;
            lastTaskCpu = taskCpu;
        }

#ifdef RUSAGE_INFO_V6
        // The same from the process's resource usage: cycles and instructions on the performance cores, and the CPU's
        // energy in nanojoules (all cores, performance cores).
        {
            static rusage_info_v6 lastUsage{};
            rusage_info_v6 usage{};
            if (proc_pid_rusage(getpid(), RUSAGE_INFO_V6, reinterpret_cast<rusage_info_t*>(&usage)) == 0)
            {
                static bool haveLast = false;
                if (haveLast)
                {
                    // ri_runnable_time and the QoS times are in Mach time units.
                    mach_timebase_info_data_t timebase{};
                    mach_timebase_info(&timebase);
                    const double tickNs = timebase.denom != 0 ? double(timebase.numer) / double(timebase.denom) : 1.0;
                    const double qosAll = double(
                        (usage.ri_cpu_time_qos_default - lastUsage.ri_cpu_time_qos_default) +
                        (usage.ri_cpu_time_qos_maintenance - lastUsage.ri_cpu_time_qos_maintenance) +
                        (usage.ri_cpu_time_qos_background - lastUsage.ri_cpu_time_qos_background) +
                        (usage.ri_cpu_time_qos_utility - lastUsage.ri_cpu_time_qos_utility) +
                        (usage.ri_cpu_time_qos_legacy - lastUsage.ri_cpu_time_qos_legacy) +
                        (usage.ri_cpu_time_qos_user_initiated - lastUsage.ri_cpu_time_qos_user_initiated) +
                        (usage.ri_cpu_time_qos_user_interactive - lastUsage.ri_cpu_time_qos_user_interactive));
                    auto qos = [&](uint64_t now, uint64_t before) { return qosAll > 0 ? 100.0 * double(now - before) / qosAll : 0.0; };
                    task += fmt::format("; CPU time by QoS: user-interactive {:.0f}%, user-initiated {:.0f}%, default {:.0f}%, legacy {:.0f}%, "
                                        "utility and below {:.0f}%",
                        qos(usage.ri_cpu_time_qos_user_interactive, lastUsage.ri_cpu_time_qos_user_interactive),
                        qos(usage.ri_cpu_time_qos_user_initiated, lastUsage.ri_cpu_time_qos_user_initiated),
                        qos(usage.ri_cpu_time_qos_default, lastUsage.ri_cpu_time_qos_default),
                        qos(usage.ri_cpu_time_qos_legacy, lastUsage.ri_cpu_time_qos_legacy),
                        qos(usage.ri_cpu_time_qos_utility + usage.ri_cpu_time_qos_background + usage.ri_cpu_time_qos_maintenance,
                            lastUsage.ri_cpu_time_qos_utility + lastUsage.ri_cpu_time_qos_background + lastUsage.ri_cpu_time_qos_maintenance));
                    const double cycles = double(usage.ri_cycles - lastUsage.ri_cycles);
                    const double instructions = double(usage.ri_instructions - lastUsage.ri_instructions);
                    task += fmt::format("; rusage: performance cores {:.0f}% of the cycles and {:.0f}% of the instructions, "
                                        "CPU energy {:.2f} W ({:.2f} W on the performance cores), threads runnable but waiting for a core {:.0f}% of one core",
                        cycles > 0 ? 100.0 * double(usage.ri_pcycles - lastUsage.ri_pcycles) / cycles : 0.0,
                        instructions > 0 ? 100.0 * double(usage.ri_pinstructions - lastUsage.ri_pinstructions) / instructions : 0.0,
                        double(usage.ri_energy_nj - lastUsage.ri_energy_nj) / 1e9 / seconds,
                        double(usage.ri_penergy_nj - lastUsage.ri_penergy_nj) / 1e9 / seconds,
                        100.0 * double(usage.ri_runnable_time - lastUsage.ri_runnable_time) * tickNs / (seconds * 1e9));
                }
                lastUsage = usage;
                haveLast = true;
            }
        }
#endif
        task += fmt::format("; missed wakes {}", Park::MissedWakes().load());

        std::vector<std::pair<uint64_t, std::string>> sorted;
        for (const auto& [name, entry] : byName)
            sorted.emplace_back(entry.first, fmt::format("{}{} (pri {})", name, entry.second > 1 ? fmt::format(" x{}", entry.second) : std::string(), priorityByName[name]));
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::string text;
        for (size_t i = 0; i < sorted.size() && i < 16; i++)
        {
            const double percent = 100.0 * double(sorted[i].first) / (seconds * 1e9);
            if (percent < 1.0)
                break;
            text += fmt::format("{}{} {:.0f}%", i ? ", " : "", sorted[i].second, percent);
        }
        LOGFN("[cpu] threads, last {:.0f} s: the app {:.0f}% of one core ({} threads, {} cores){}; {}", seconds,
            100.0 * double(allNs) / (seconds * 1e9), count, std::thread::hardware_concurrency(), task, text);
    }

    void SamplerThread()
    {
        std::unordered_map<uint64_t, uint64_t> threadTimes;
        ReportThreads(threadTimes, 10.0); // the base
        pthread_setname_np("cpu sampler");
        std::unordered_map<uintptr_t, Symbol> cache;
        Profile profiles[kThreads];
        auto lastReport = std::chrono::steady_clock::now();
        while (true)
        {
            usleep(2000);
            const int count = g_watchedCount.load(std::memory_order_acquire);
            for (int i = 0; i < count; i++)
            {
                const Watched& watched = g_watched[i];
                uintptr_t frames[kMaxDepth];
                int depth = 0;
                if (thread_suspend(watched.thread) != KERN_SUCCESS)
                    continue;
#if defined(__arm64__) || defined(__aarch64__)
                arm_thread_state64_t state{};
                mach_msg_type_number_t stateCount = ARM_THREAD_STATE64_COUNT;
                if (thread_get_state(watched.thread, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state), &stateCount) == KERN_SUCCESS)
                {
                    frames[depth++] = uintptr_t(arm_thread_state64_get_pc(state)) & kAddressMask;
                    frames[depth++] = uintptr_t(arm_thread_state64_get_lr(state)) & kAddressMask;
                    uintptr_t fp = uintptr_t(arm_thread_state64_get_fp(state));
                    while (depth < kMaxDepth && fp >= watched.low && fp + 16 <= watched.high && (fp & 7) == 0)
                    {
                        const uintptr_t next = *reinterpret_cast<const uintptr_t*>(fp);
                        const uintptr_t ret = *reinterpret_cast<const uintptr_t*>(fp + 8) & kAddressMask;
                        if (ret == 0)
                            break;
                        frames[depth++] = ret;
                        if (next <= fp)
                            break;
                        fp = next;
                    }
                }
#endif
                thread_resume(watched.thread);
                if (depth == 0)
                    continue;
                Profile& profile = profiles[i];
                profile.samples++;
                profile.self[frames[0]]++;
                profile.stacks.insert(profile.stacks.end(), frames, frames + depth);
                profile.stacks.push_back(0);
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - lastReport >= std::chrono::seconds(10))
            {
                ReportThreads(threadTimes, std::chrono::duration<double>(now - lastReport).count());
                lastReport = now;
                for (int i = 0; i < count; i++)
                {
                    Report(g_watched[i], profiles[i], cache);
                    profiles[i] = Profile{};
                }
            }
        }
    }
}

void CpuSampler::WatchHitchThread(const char* name)
{
    if (!Enabled())
        return;
    std::lock_guard lock(g_watchMutex);
    const mach_port_t self = pthread_mach_thread_np(pthread_self());
    const int count = g_watchedCount.load();
    for (int i = 0; i < count; i++)
        if (g_watched[i].thread == self)
            return;
    if (count >= kThreads)
        return;
    Watched& watched = g_watched[count];
    strncpy(watched.name, name, sizeof(watched.name) - 1);
    watched.thread = self;
    watched.high = uintptr_t(pthread_get_stackaddr_np(pthread_self()));
    watched.low = watched.high - pthread_get_stacksize_np(pthread_self());
    g_watchedCount.store(count + 1, std::memory_order_release);
    if (count == 0)
    {
        std::thread(SamplerThread).detach();
        LOGN("[cpu] CPU profile of the frame's threads every 10 s (DR_HITCH_PROFILE=0 off)");
    }
}

void CpuSampler::WatchHitches()
{
    WatchHitchThread("game thread (presents)");
}

void CpuSampler::ReportHitch(uint32_t, std::chrono::steady_clock::time_point, std::chrono::steady_clock::time_point) {}
void CpuSampler::RegisterCurrentThread(const char*) {}
void CpuSampler::LogStack(const void*) {}
#else
void CpuSampler::WatchHitches() {}
void CpuSampler::WatchHitchThread(const char*) {}
void CpuSampler::ReportHitch(uint32_t, std::chrono::steady_clock::time_point, std::chrono::steady_clock::time_point) {}
void CpuSampler::RegisterCurrentThread(const char*) {}
void CpuSampler::LogStack(const void*) {}
#endif
