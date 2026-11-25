// Hang watchdog (hang_watchdog.h).

#include <stdafx.h>
#include "hang_watchdog.h"
#include <cpu/ppc_context.h>
#include <os/logger.h>
#include <gpu/video.h>

#if defined(__APPLE__)
#include <mach/mach.h>
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

    void Thread()
    {
        for (size_t i = 0; PPCFuncMappings[i].host != nullptr; i++)
            g_functions.emplace_back(uintptr_t(PPCFuncMappings[i].host), uint32_t(PPCFuncMappings[i].guest));
        std::sort(g_functions.begin(), g_functions.end());

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

void HangWatchdog::RegisterGuestThread(const char* name)
{
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
