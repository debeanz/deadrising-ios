#include <stdafx.h>
#include "call_profiler.h"
#include <kernel/memory.h>
#include <os/logger.h>

namespace
{
    constexpr int kSamples = 6;

    struct Sample
    {
        uint32_t lr;
        uint32_t args[8];          // r3..r10
        double f1;
        uint32_t deref[3][4];      // data behind r3..r5 when they look like pointers
        uint32_t ret;              // r3 after the call
        uint32_t retDeref[4];
        std::atomic<bool> complete;
    };

    struct Entry
    {
        std::atomic<uint64_t> calls{ 0 };
        std::atomic<uint64_t> deviceCalls{ 0 };
        std::atomic<uint32_t> firstCaller{ 0 };
        std::atomic<uint32_t> callers[4]{};
        std::atomic<int> sampleCount{ 0 };
        Sample samples[kSamples];
    };

    std::unique_ptr<Entry[]> g_entries;
    std::atomic<uint32_t> g_device{ 0 };

    // D3D lives here (plus the 0x8254 draw helpers); sample those, count everything.
    bool IsSampled(uint32_t address)
    {
        return (address >= 0x82440000 && address < 0x82460000) || (address >= 0x82540000 && address < 0x82550000);
    }

    bool LooksLikePointer(uint32_t v)
    {
        // 16 bytes are read behind it, so keep clear of the end of the 4 GB guest space.
        return v >= 0x00010000 && (v < 0x82000000 || v >= 0xA0000000) && v < 0xFFFF0000;
    }

    void ReadDwords(uint32_t address, uint32_t (&out)[4])
    {
        for (int i = 0; i < 4; i++)
            out[i] = LooksLikePointer(address) ? ByteSwap(*(uint32_t*)g_memory.Translate(address + i * 4)) : 0;
    }

    void Dump()
    {
        std::ofstream csv("call_profile.csv", std::ios::trunc);
        csv << "address,calls,device_calls,first_caller,callers\n";

        std::ofstream samples("call_samples.txt", std::ios::trunc);

        for (uint32_t i = 0; i < CallProfiler::kFunctionCount; i++)
        {
            auto& e = g_entries[i];
            uint64_t calls = e.calls.load(std::memory_order_relaxed);
            if (!calls)
                continue;

            std::string callers;
            for (auto& c : e.callers)
            {
                if (uint32_t v = c.load(std::memory_order_relaxed))
                    callers += fmt::format("{:08X} ", v);
            }

            csv << fmt::format("{:08X},{},{},{:08X},{}\n", CallProfiler::kFunctionAddresses[i], calls,
                e.deviceCalls.load(), e.firstCaller.load(), callers);

            int count = std::min(e.sampleCount.load(), kSamples);
            if (count == 0)
                continue;

            samples << fmt::format("sub_{:08X}  calls={} device_calls={}\n", CallProfiler::kFunctionAddresses[i], calls, e.deviceCalls.load());
            for (int s = 0; s < count; s++)
            {
                auto& sm = e.samples[s];
                if (!sm.complete.load())
                    continue;

                samples << fmt::format("  lr={:08X} r3={:08X} r4={:08X} r5={:08X} r6={:08X} r7={:08X} r8={:08X} r9={:08X} r10={:08X} f1={} -> {:08X}\n",
                    sm.lr, sm.args[0], sm.args[1], sm.args[2], sm.args[3], sm.args[4], sm.args[5], sm.args[6], sm.args[7], sm.f1, sm.ret);
                for (int a = 0; a < 3; a++)
                {
                    if (LooksLikePointer(sm.args[a]) && sm.args[a] != g_device)
                        samples << fmt::format("      *r{} = {:08X} {:08X} {:08X} {:08X}\n", a + 3, sm.deref[a][0], sm.deref[a][1], sm.deref[a][2], sm.deref[a][3]);
                }
                if (LooksLikePointer(sm.ret))
                    samples << fmt::format("      *ret = {:08X} {:08X} {:08X} {:08X}\n", sm.retDeref[0], sm.retDeref[1], sm.retDeref[2], sm.retDeref[3]);
            }
        }

        if (uint32_t device = g_device)
        {
            std::ofstream bin("d3d_device.bin", std::ios::binary | std::ios::trunc);
            bin.write((const char*)g_memory.Translate(device), 0x8000);
        }
    }
}

int CallProfiler::Record(uint32_t index, PPCContext& ctx)
{
    auto& e = g_entries[index];
    e.calls.fetch_add(1, std::memory_order_relaxed);

    uint32_t lr = uint32_t(ctx.lr);
    uint32_t expected = 0;
    e.firstCaller.compare_exchange_strong(expected, lr, std::memory_order_relaxed);

    for (auto& c : e.callers)
    {
        uint32_t v = c.load(std::memory_order_relaxed);
        if (v == lr)
            break;
        if (v == 0)
        {
            uint32_t zero = 0;
            if (c.compare_exchange_strong(zero, lr, std::memory_order_relaxed) || zero == lr)
                break;
        }
    }

    uint32_t device = g_device.load(std::memory_order_relaxed);
    if (device != 0 && ctx.r3.u32 == device)
        e.deviceCalls.fetch_add(1, std::memory_order_relaxed);

    if (!IsSampled(kFunctionAddresses[index]) || e.sampleCount.load(std::memory_order_relaxed) >= kSamples)
        return -1;

    // Only keep calls that differ from the ones already sampled (call site or first two arguments).
    int existing = std::min(e.sampleCount.load(), kSamples);
    for (int s = 0; s < existing; s++)
    {
        auto& sm = e.samples[s];
        if (sm.lr == lr && sm.args[1] == ctx.r4.u32 && sm.args[0] == ctx.r3.u32)
            return -1;
    }

    int slot = e.sampleCount.fetch_add(1);
    if (slot >= kSamples)
        return -1;

    auto& sm = e.samples[slot];
    sm.lr = lr;
    PPCRegister* regs[] = { &ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10 };
    for (int a = 0; a < 8; a++)
        sm.args[a] = regs[a]->u32;
    sm.f1 = ctx.f1.f64;
    for (int a = 0; a < 3; a++)
        ReadDwords(sm.args[a], sm.deref[a]);
    return slot;
}

void CallProfiler::Return(uint32_t index, int slot, PPCContext& ctx)
{
    if (slot < 0)
        return;

    auto& sm = g_entries[index].samples[slot];
    sm.ret = ctx.r3.u32;
    ReadDwords(sm.ret, sm.retDeref);
    sm.complete = true;
}

void CallProfiler::SetDevice(uint32_t device)
{
    g_device = device;
    LOGFN("[profiler] D3D device = 0x{:08X}", device);
}

void CallProfiler::Start()
{
    g_entries = std::make_unique<Entry[]>(kFunctionCount);
    std::thread([]
    {
        while (true)
        {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            Dump();
        }
    }).detach();
    LOGFN("[profiler] call profiler on: {} functions wrapped", kFunctionCount);
}
