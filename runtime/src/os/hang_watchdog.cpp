// Hang watchdog (hang_watchdog.h).

#include <stdafx.h>
#include "hang_watchdog.h"
#include <cpu/ppc_context.h>
#include <os/logger.h>
#include <gpu/video.h>
#include <kernel/memory.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <dlfcn.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <pthread.h>
#endif

namespace
{
    constexpr int kSlots = 64;
    constexpr int64_t kStallMs = 15000;

    struct Slot
    {
        std::atomic<const char*> lastImport{ nullptr };
        std::atomic<uint32_t> lastLr{ 0 };
        std::atomic<uint64_t> imports{ 0 };
        char name[64] = {};
        bool used = false;
#if defined(__APPLE__)
        mach_port_t thread = MACH_PORT_NULL;
        uintptr_t stackLow = 0, stackHigh = 0;
#endif
    };

    Slot g_slots[kSlots];
    std::atomic<int> g_slotCount{ 0 };
    std::mutex g_slotMutex;
    thread_local Slot* t_slot = nullptr;
    std::atomic<int64_t> g_lastPresentMs{ 0 };
    std::atomic<uint64_t> g_presents{ 0 };

    int64_t NowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    bool Enabled()
    {
        static const bool enabled = getenv("DR_WATCHDOG") == nullptr || atoi(getenv("DR_WATCHDOG")) != 0;
        return enabled;
    }

    // Host addresses of the recompiled functions, sorted: a return address inside one names it.
    std::vector<std::pair<uintptr_t, uint32_t>> g_functions;

    std::string Describe(uintptr_t address)
    {
        auto it = std::upper_bound(g_functions.begin(), g_functions.end(), std::make_pair(address, UINT32_MAX));
        if (it == g_functions.begin())
            return fmt::format("{:X}", address);
        return fmt::format("sub_{:08X}", std::prev(it)->second);
    }

#if defined(__APPLE__)
    // The thread's frames (pc, lr, then the frame-pointer chain; Apple's arm64 ABI keeps frame
    // pointers), as recompiled functions.
    std::string Stack(const Slot& slot)
    {
        if (slot.thread == MACH_PORT_NULL)
            return {};
        std::vector<uintptr_t> frames;
        if (thread_suspend(slot.thread) != KERN_SUCCESS)
            return "(could not suspend)";
#if defined(__arm64__) || defined(__aarch64__)
        arm_thread_state64_t state{};
        mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
        if (thread_get_state(slot.thread, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state), &count) == KERN_SUCCESS)
        {
            frames.push_back(uintptr_t(arm_thread_state64_get_pc(state)));
            frames.push_back(uintptr_t(arm_thread_state64_get_lr(state)));
            uintptr_t fp = uintptr_t(arm_thread_state64_get_fp(state));
            while (frames.size() < 48 && fp >= slot.stackLow && fp + 16 <= slot.stackHigh && (fp & 7) == 0)
            {
                const uintptr_t next = *reinterpret_cast<const uintptr_t*>(fp);
                const uintptr_t ret = *reinterpret_cast<const uintptr_t*>(fp + 8);
                if (ret == 0)
                    break;
                frames.push_back(ret);
                if (next <= fp)
                    break;
                fp = next;
            }
        }
#endif
        thread_resume(slot.thread);
        std::string text;
        std::string previous;
        for (uintptr_t frame : frames)
        {
            std::string name = Describe(frame);
            if (name == previous)
                continue;
            text += (text.empty() ? "" : " < ") + name;
            previous = name;
        }
        return text;
    }
#endif

    void Report(int64_t stalledMs, const std::vector<uint64_t>& importsBefore)
    {
        const int count = g_slotCount.load();
        LOGFN_WARNING("[watchdog] no frame presented for {:.0f} s ({} presented so far); game threads:", stalledMs / 1000.0, g_presents.load());
        for (int i = 0; i < count; i++)
        {
            Slot& slot = g_slots[i];
            const char* import = slot.lastImport.load();
            std::string importName = import ? import : "none";
            if (importName.rfind("__imp__", 0) == 0)
                importName.erase(0, 7);
            const uint64_t made = slot.imports.load() - (i < int(importsBefore.size()) ? importsBefore[i] : 0);
            std::string line = fmt::format("[watchdog]   {}: last import {} (lr {:08X}), {} imports in the last {:.0f} s", slot.name, importName,
                slot.lastLr.load(), made, kStallMs / 1000.0);
#if defined(__APPLE__)
            const std::string stack = Stack(slot);
            if (!stack.empty())
                line += fmt::format("; stack {}", stack);
#endif
            LOGFN_WARNING("{}", line);
        }
    }

    void LoadFunctions()
    {
        if (!g_functions.empty())
            return;
        for (size_t i = 0; PPCFuncMappings[i].host != nullptr; i++)
            g_functions.emplace_back(uintptr_t(PPCFuncMappings[i].host), uint32_t(PPCFuncMappings[i].guest));
        std::sort(g_functions.begin(), g_functions.end());
    }

#if defined(__APPLE__)
    // ---- Crashes on Apple platforms. Nothing caught them before (2026-11-25: the iPhone build ended at
    // frame 8 with nothing in the log). The report gives the signal, the address, the thread (and, for a
    // game thread, its last import), then the stack: each frame as a recompiled function (sub_XXXXXXXX)
    // when it is one, else the runtime's own symbol (dladdr) and its offset in the executable, which the
    // build's symbol list (DeadRising-N.symbols.gz in the release) names.
    struct sigaction g_previous[NSIG];
    std::atomic<int> g_crashing{ 0 };

    const char* SignalName(int signal)
    {
        switch (signal)
        {
        case SIGSEGV: return "SIGSEGV (bad memory access)";
        case SIGBUS: return "SIGBUS (bad memory access)";
        case SIGILL: return "SIGILL (illegal instruction)";
        case SIGFPE: return "SIGFPE (arithmetic)";
        case SIGTRAP: return "SIGTRAP (trap)";
        case SIGABRT: return "SIGABRT (abort)";
        default: return "signal";
        }
    }

    std::string HostFrame(uintptr_t address)
    {
        const uintptr_t first = g_functions.empty() ? 0 : g_functions.front().first;
        const uintptr_t last = g_functions.empty() ? 0 : g_functions.back().first + 0x100000;
        if (address >= first && address < last)
        {
            Dl_info info{};
            // Inside the recompiled code: the nearest recompiled function by host address.
            if (dladdr(reinterpret_cast<void*>(address), &info) == 0 || info.dli_sname == nullptr || strncmp(info.dli_sname, "sub_", 4) == 0 ||
                strncmp(info.dli_sname, "_sub_", 5) == 0)
                return Describe(address);
        }
        Dl_info info{};
        if (dladdr(reinterpret_cast<void*>(address), &info) != 0 && info.dli_fbase != nullptr)
        {
            const uintptr_t offset = address - uintptr_t(info.dli_fbase);
            const char* file = info.dli_fname ? strrchr(info.dli_fname, '/') : nullptr;
            file = file ? file + 1 : (info.dli_fname ? info.dli_fname : "?");
            if (info.dli_sname != nullptr)
                return fmt::format("{}+{:X} ({} +{:X})", info.dli_sname, address - uintptr_t(info.dli_saddr), file, offset);
            return fmt::format("{} +{:X}", file, offset);
        }
        return fmt::format("{:X}", address);
    }

    // A write through a null game pointer: the game's page 0 is read-only zeros (kernel/memory.cpp), so the store faults;
    // it is skipped and the game goes on, as reads there read zero (2026-11-25, the mall: a zombie wrote into the object
    // it was holding, which another game thread had just let go: stb to 0x2C33 in sub_822AA158; an Xbox 360 would crash
    // too, but its timing never got there). The first 20 are logged. DR_NULL_WRITES=crash turns this off.
    bool SkipNullWrite(int signal, siginfo_t* info, void* context)
    {
#if defined(__arm64__) || defined(__aarch64__)
        static const bool enabled = getenv("DR_NULL_WRITES") == nullptr || strcmp(getenv("DR_NULL_WRITES"), "crash") != 0;
        if (!enabled || (signal != SIGBUS && signal != SIGSEGV) || info == nullptr || context == nullptr || g_memory.base == nullptr)
            return false;
        const uintptr_t fault = uintptr_t(info->si_addr);
        const uintptr_t base = uintptr_t(g_memory.base);
        if (fault < base || fault >= base + uintptr_t(getpagesize()))
            return false;
        ucontext_t* uc = static_cast<ucontext_t*>(context);
        if (uc->uc_mcontext == nullptr)
            return false;
        auto& ss = uc->uc_mcontext->__ss;
        const uintptr_t pc = uintptr_t(arm_thread_state64_get_pc(ss));
        static std::atomic<uint32_t> skipped{ 0 };
        if (skipped.fetch_add(1) < 20)
        {
            Dl_info where{};
            dladdr(reinterpret_cast<void*>(pc), &where);
            LOGFN_WARNING("[null] write to game address {:04X} at {}+{:X} (game lr {:08X}): skipped", fault - base,
                where.dli_sname ? where.dli_sname : "?", where.dli_saddr ? pc - uintptr_t(where.dli_saddr) : 0,
                t_slot != nullptr ? t_slot->lastLr.load() : 0);
        }
        arm_thread_state64_set_pc_fptr(ss, reinterpret_cast<void*>(pc + 4)); // every AArch64 instruction is 4 bytes
        return true;
#else
        (void)signal; (void)info; (void)context;
        return false;
#endif
    }

    void CrashHandler(int signal, siginfo_t* info, void* context)
    {
        if (SkipNullWrite(signal, info, context))
            return;
        if (g_crashing.fetch_add(1) == 0)
        {
            char thread[64] = {};
            pthread_getname_np(pthread_self(), thread, sizeof(thread));
            const uintptr_t fault = uintptr_t(info != nullptr ? info->si_addr : nullptr);
            std::string text = fmt::format("[crash] {} at {:X}", SignalName(signal), fault);
            if (g_memory.base != nullptr && g_memory.IsInMemoryRange(reinterpret_cast<void*>(fault)))
                text += fmt::format(" (game address {:08X})", g_memory.MapVirtual(reinterpret_cast<void*>(fault)));
            text += fmt::format(", thread \"{}\"", thread[0] != 0 ? thread : (t_slot != nullptr ? t_slot->name : "?"));
            if (t_slot != nullptr)
            {
                const char* import = t_slot->lastImport.load();
                text += fmt::format(" (game thread {}, last import {} lr {:08X})", t_slot->name, import ? import : "none", t_slot->lastLr.load());
            }
#if defined(__arm64__) || defined(__aarch64__)
            const ucontext_t* uc = static_cast<const ucontext_t*>(context);
            if (uc != nullptr && uc->uc_mcontext != nullptr)
            {
                const auto& ss = uc->uc_mcontext->__ss;
                std::vector<uintptr_t> frames;
                frames.push_back(uintptr_t(arm_thread_state64_get_pc(ss)));
                frames.push_back(uintptr_t(arm_thread_state64_get_lr(ss)));
                const uintptr_t high = uintptr_t(pthread_get_stackaddr_np(pthread_self()));
                const uintptr_t low = high - pthread_get_stacksize_np(pthread_self());
                const uintptr_t sp = uintptr_t(arm_thread_state64_get_sp(ss));
                uintptr_t fp = uintptr_t(arm_thread_state64_get_fp(ss));
                while (frames.size() < 40 && fp >= low && fp + 16 <= high && (fp & 7) == 0)
                {
                    const uintptr_t next = *reinterpret_cast<const uintptr_t*>(fp);
                    const uintptr_t ret = *reinterpret_cast<const uintptr_t*>(fp + 8) & 0x0000000FFFFFFFFFull; // pointer authentication bits off
                    if (ret == 0)
                        break;
                    frames.push_back(ret);
                    if (next <= fp)
                        break;
                    fp = next;
                }
                text += fmt::format("; sp {:X} (stack {:X}-{:X}{})", sp, low, high, sp < low + 16384 ? ", nearly full: stack overflow?" : "");
                std::string stack, previous;
                for (uintptr_t frame : frames)
                {
                    const std::string name = HostFrame(frame & 0x0000000FFFFFFFFFull);
                    if (name == previous)
                        continue;
                    stack += (stack.empty() ? "" : " < ") + name;
                    previous = name;
                }
                text += "; stack " + stack;
                text += fmt::format("; x0 {:X} x1 {:X} x2 {:X} x3 {:X}", ss.__x[0], ss.__x[1], ss.__x[2], ss.__x[3]);
            }
#endif
            os::logger::LogCrash(text);
        }
        // Back to the system's handler: the instruction runs again and the app ends as before.
        sigaction(signal, &g_previous[signal], nullptr);
        if (signal == SIGABRT || signal == SIGTRAP)
            raise(signal);
    }

    void InstallCrashHandler()
    {
        static bool installed = false;
        if (installed || (getenv("DR_CRASH_HANDLER") != nullptr && atoi(getenv("DR_CRASH_HANDLER")) == 0))
            return;
        installed = true;
        LoadFunctions();
        HangWatchdog::PrepareThread(); // this (the main) thread
        struct sigaction action{};
        action.sa_sigaction = CrashHandler;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&action.sa_mask);
        for (int signal : { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT })
            sigaction(signal, &action, &g_previous[signal]);
        // An exception nothing catches: its message, then abort (whose report gives the stack where it was thrown).
        std::set_terminate([] {
            std::string what = "(not a std::exception)";
            if (std::exception_ptr exception = std::current_exception())
            {
                try
                {
                    std::rethrow_exception(exception);
                }
                catch (const std::exception& e)
                {
                    what = e.what();
                }
                catch (...)
                {
                }
            }
            else
                what = "(none: std::terminate called directly)";
            os::logger::LogCrash(fmt::format("[crash] uncaught C++ exception: {}", what));
            abort();
        });
        LOGN("[crash] crash reports on (DR_CRASH_HANDLER=0 off)");
    }
#endif

    void Thread()
    {
        LoadFunctions();

        int reports = 0;
        int64_t reportedForPresent = -1;
        std::vector<uint64_t> importsBefore;
        while (true)
        {
            // The import counts as the window starts, so a report says what each thread did in it.
            importsBefore.assign(kSlots, 0);
            for (int i = 0; i < g_slotCount.load(); i++)
                importsBefore[i] = g_slots[i].imports.load();
            std::this_thread::sleep_for(std::chrono::milliseconds(kStallMs));

            const int64_t last = g_lastPresentMs.load();
            const int64_t stalled = NowMs() - last;
            if (stalled < kStallMs || Video::RenderingPaused()) // iOS in the background: no stall
            {
                reports = 0;
                continue;
            }
            if (reportedForPresent != last)
            {
                reportedForPresent = last;
                reports = 0;
            }
            if (reports++ < 3)
                Report(stalled, importsBefore);
        }
    }
}

void HangWatchdog::Start()
{
#if defined(__APPLE__)
    InstallCrashHandler();
#endif
    if (!Enabled())
        return;
    g_lastPresentMs = NowMs();
    std::thread(Thread).detach();
    LOGFN("[watchdog] on: a stall of {:.0f} s without a frame is reported (DR_WATCHDOG=0 off)", kStallMs / 1000.0);
}

void HangWatchdog::OnPresent()
{
    g_lastPresentMs.store(NowMs(), std::memory_order_relaxed);
    g_presents.fetch_add(1, std::memory_order_relaxed);
}

void HangWatchdog::PrepareThread()
{
#if defined(__APPLE__)
    // A stack of its own for the crash report, so a stack overflow is reported too.
    thread_local bool prepared = false;
    if (prepared)
        return;
    prepared = true;
    constexpr size_t kSize = 128 * 1024;
    stack_t stack{};
    stack.ss_sp = malloc(kSize);
    stack.ss_size = kSize;
    stack.ss_flags = 0;
    if (stack.ss_sp != nullptr)
        sigaltstack(&stack, nullptr);
#endif
}

void HangWatchdog::RegisterGuestThread(const char* name)
{
    PrepareThread();
    if (t_slot != nullptr)
        return;
    std::lock_guard lock(g_slotMutex);
    const int index = g_slotCount.load();
    if (index >= kSlots)
        return;
    Slot& slot = g_slots[index];
    strncpy(slot.name, name, sizeof(slot.name) - 1);
#if defined(__APPLE__)
    slot.thread = pthread_mach_thread_np(pthread_self());
    const uintptr_t high = uintptr_t(pthread_get_stackaddr_np(pthread_self()));
    slot.stackHigh = high;
    slot.stackLow = high - pthread_get_stacksize_np(pthread_self());
#endif
    slot.used = true;
    t_slot = &slot;
    g_slotCount.store(index + 1);
}

void HangWatchdog::NoteImport(const char* name, uint32_t lr)
{
    Slot* slot = t_slot;
    if (slot == nullptr)
        return;
    slot->lastImport.store(name, std::memory_order_relaxed);
    slot->lastLr.store(lr, std::memory_order_relaxed);
    slot->imports.fetch_add(1, std::memory_order_relaxed);
}
