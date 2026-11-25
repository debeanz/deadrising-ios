// Minimal Xenos command processor (no rendering). Packet semantics follow Xenia's
// CommandProcessor (src/xenia/gpu/command_processor.cc).

#include <stdafx.h>
#include "command_processor.h"
#include <cpu/guest_thread.h>
#include <cpu/ppc_context.h>
#include <kernel/memory.h>
#include <os/logger.h>
#include <profiler/cpu_sampler.h>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define DR_SPIN_PAUSE() _mm_pause()
#else
#define DR_SPIN_PAUSE() __asm__ __volatile__("yield")
#endif

namespace
{
    // GPU MMIO lives at 0x7FC80000 on the console; D3D writes the ring write pointer there.
    constexpr uint32_t kGpuRegisterBase = 0x7FC80000;
    constexpr uint32_t kRegCpRbWptr = 0x01C5;
    constexpr uint32_t kRegScratchUmsk = 0x01DC;
    constexpr uint32_t kRegScratchAddr = 0x01DD;
    constexpr uint32_t kRegScratch0 = 0x0578;

    std::atomic<uint32_t> g_ringBase;
    std::atomic<uint32_t> g_ringSizeDwords;
    std::atomic<uint32_t> g_rptrWriteBack;
    std::atomic<uint32_t> g_interruptCallback;
    std::atomic<uint32_t> g_interruptData;
    std::atomic<uint32_t> g_swapCounter;
    std::atomic<bool> g_started;

    std::vector<uint32_t> g_registers(0x10000);

    // Predicated-tiling bin state (CP thread only); all bins selected until D3D says otherwise.
    uint64_t g_binMask = ~0ull;
    uint64_t g_binSelect = ~0ull;
    std::set<uint32_t> g_seenOpcodes;

    std::mutex g_interruptMutex;

    // The ring write pointer wakes the command processor thread (OnRegisterWrite). Polling every
    // 100 us slept about a millisecond each time on Windows, and the game spins in D3D until the
    // GPU frees ring space (sub_8244B050): in the busy mall that was half the game thread's time.
    std::mutex g_kickMutex;
    std::condition_variable g_kick;
    bool g_kickPending = false;

    uint32_t LoadBE(uint32_t guest)
    {
        return ByteSwap(*(uint32_t*)g_memory.Translate(guest));
    }

    // Xenos endian modes in the low bits of a GPU address.
    uint32_t GpuSwap(uint32_t value, uint32_t endian)
    {
        switch (endian & 3)
        {
            case 1: return ((value & 0x00FF00FF) << 8) | ((value & 0xFF00FF00) >> 8); // 8in16
            case 2: return ByteSwap(value);                                           // 8in32
            case 3: return (value << 16) | (value >> 16);                             // 16in32
            default: return value;
        }
    }

    // Store as the GPU would: host order after the endian swap.
    void GpuStore(uint32_t address, uint32_t value)
    {
        uint32_t guest = CommandProcessor::GpuToGuest(address & ~3u);
        *(uint32_t*)g_memory.Translate(guest) = GpuSwap(value, address & 3);
    }

    uint32_t GpuLoad(uint32_t address)
    {
        uint32_t guest = CommandProcessor::GpuToGuest(address & ~3u);
        return GpuSwap(*(uint32_t*)g_memory.Translate(guest), address & 3);
    }

    // Runs the game's graphics interrupt handler (source 0 = vblank, 1 = command processor).
    void DispatchInterrupt(uint32_t source, uint32_t cpu)
    {
        uint32_t callback = g_interruptCallback;
        if (!callback)
            return;

        thread_local std::unique_ptr<GuestThreadContext> context;
        if (!context)
            context = std::make_unique<GuestThreadContext>(cpu);

        std::lock_guard lock(g_interruptMutex);
        auto& ctx = context->ppcContext;

        // The handler reads the current CPU from the processor control block (r13 + 0x10C) and
        // acknowledges that CPU's bit, so it must match the CPU this interrupt is delivered to.
        *(uint8_t*)g_memory.Translate(ctx.r13.u32 + 0x10C) = uint8_t(cpu);

        if (source == 1)
        {
            static std::atomic<uint32_t> logged;
            if (logged++ < 6)
            {
                uint32_t shared = LoadBE(g_interruptData + 0x2A14);
                LOGFN("[cp] CP interrupt cpu {}: shared block 0x{:08X} = [{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}]", cpu, shared,
                    LoadBE(shared), LoadBE(shared + 4), LoadBE(shared + 8), LoadBE(shared + 12), LoadBE(shared + 16), LoadBE(shared + 20));
            }
        }

        ctx.r3.u64 = source;
        ctx.r4.u64 = g_interruptData;
        g_memory.FindFunction(callback)(ctx, g_memory.base);
    }

    // Last packets executed, dumped when something goes wrong (CP thread only). Kept raw and only
    // formatted by DumpHistory: formatting a string for every packet and register write was most
    // of the command processor's time, and the game waits for it to free ring space.
    struct HistoryEntry
    {
        enum Kind : uint8_t { None, Register, Packet, Skipped } kind = None;
        uint32_t a = 0, b = 0; // register index and value / opcode and dword count
        uint32_t data[6] = {};
        int depth = 0;
    };
    std::array<HistoryEntry, 24> g_history;
    size_t g_historyNext = 0;

    void RecordRegister(uint32_t index, uint32_t value)
    {
        HistoryEntry& entry = g_history[g_historyNext++ % g_history.size()];
        entry.kind = HistoryEntry::Register;
        entry.a = index;
        entry.b = value;
    }

    void RecordPacket(HistoryEntry::Kind kind, uint32_t opcode, uint32_t count, const uint32_t* data, int depth)
    {
        HistoryEntry& entry = g_history[g_historyNext++ % g_history.size()];
        entry.kind = kind;
        entry.a = opcode;
        entry.b = count;
        for (uint32_t i = 0; i < 6; i++)
            entry.data[i] = i < count ? data[i] : 0;
        entry.depth = depth;
    }

    void DumpHistory()
    {
        LOGFN("[cp] SCRATCH_UMSK reg={:08X} mmio={:08X}, SCRATCH_ADDR reg={:08X} mmio={:08X}",
            g_registers[kRegScratchUmsk], LoadBE(kGpuRegisterBase + kRegScratchUmsk * 4),
            g_registers[kRegScratchAddr], LoadBE(kGpuRegisterBase + kRegScratchAddr * 4));
        LOGN("[cp] last packets (oldest first):");
        for (size_t i = 0; i < g_history.size(); i++)
        {
            const auto& entry = g_history[(g_historyNext + i) % g_history.size()];
            if (entry.kind == HistoryEntry::Register)
                LOGFN("[cp]   reg 0x{:04X} = {:08X}", entry.a, entry.b);
            else if (entry.kind == HistoryEntry::Skipped)
                LOGFN("[cp]   type3 op=0x{:02X} skipped (predicated)", entry.a);
            else if (entry.kind == HistoryEntry::Packet)
                LOGFN("[cp]   type3 op=0x{:02X} n={} [{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}] depth {}", entry.a, entry.b,
                    entry.data[0], entry.data[1], entry.data[2], entry.data[3], entry.data[4], entry.data[5], entry.depth);
        }
    }

    void WriteRegister(uint32_t index, uint32_t value)
    {
        index &= 0xFFFF;
        g_registers[index] = value;

        if (index < 0x2000 || (index >= 0x0578 && index < 0x0580))
            RecordRegister(index, value);

        // Scratch registers mirror to memory when enabled: D3D uses them as fences.
        if (index >= kRegScratch0 && index < kRegScratch0 + 8)
        {
            uint32_t n = index - kRegScratch0;
            if (g_registers[kRegScratchUmsk] & (1u << n))
            {
                uint32_t address = CommandProcessor::GpuToGuest(g_registers[kRegScratchAddr] + n * 4);
                *(be<uint32_t>*)g_memory.Translate(address) = value;
            }
        }
    }

    bool CompareFunction(uint32_t function, uint32_t value, uint32_t reference)
    {
        switch (function & 7)
        {
            case 0: return false;
            case 1: return value < reference;
            case 2: return value <= reference;
            case 3: return value == reference;
            case 4: return value != reference;
            case 5: return value >= reference;
            case 6: return value > reference;
            default: return true;
        }
    }

    void ExecuteBuffer(uint32_t guestStart, uint32_t dwords, int depth);

    // Reads packet dwords from a buffer that may wrap (ring) or not (indirect buffers).
    struct Reader
    {
        uint32_t base;  // guest address
        uint32_t size;  // dwords (for wrapping); 0 = linear
        uint32_t index; // dwords from base

        uint32_t Read()
        {
            uint32_t value = LoadBE(base + index * 4);
            index++;
            if (size && index >= size)
                index = 0;
            return value;
        }
    };

    // Executes one packet; returns false on an unknown packet type.
    void ExecutePacket(Reader& r, int depth)
    {
        uint32_t header = r.Read();
        uint32_t type = header >> 30;

        if (type == 0)
        {
            uint32_t count = ((header >> 16) & 0x3FFF) + 1;
            uint32_t baseIndex = header & 0x7FFF;
            bool oneReg = (header >> 15) & 1;
            for (uint32_t i = 0; i < count; i++)
                WriteRegister(oneReg ? baseIndex : baseIndex + i, r.Read());
            return;
        }

        if (type == 1)
        {
            uint32_t reg1 = header & 0x7FF;
            uint32_t reg2 = (header >> 11) & 0x7FF;
            WriteRegister(reg1, r.Read());
            WriteRegister(reg2, r.Read());
            return;
        }

        if (type == 2)
            return; // NOP

        // Type 3
        uint32_t opcode = (header >> 8) & 0x7F;
        uint32_t count = ((header >> 16) & 0x3FFF) + 1;
        // One buffer per nesting depth (INDIRECT_BUFFER recurses), reused: no allocation per packet.
        static std::vector<uint32_t> buffers[6];
        std::vector<uint32_t>& data = buffers[std::min(depth, 5)];
        if (data.size() < count)
            data.resize(count);
        for (uint32_t i = 0; i < count; i++)
            data[i] = r.Read();

        // Predicated tiling: D3D replays one command buffer per screen tile, and packets with the
        // predicate bit only run for the selected bins (Xenia: skip unless bin_select & bin_mask).
        // Without this, D3D's CPU-callback interrupt fires once per tile instead of once.
        if ((header & 1) && (g_binSelect & g_binMask) == 0)
        {
            RecordPacket(HistoryEntry::Skipped, opcode, count, data.data(), depth);
            return;
        }

        if (g_seenOpcodes.insert(opcode).second)
            LOGFN("[cp] first PM4 type-3 opcode 0x{:02X} ({} dwords)", opcode, count);

        RecordPacket(HistoryEntry::Packet, opcode, count, data.data(), depth);

        switch (opcode)
        {
            case 0x3F: // INDIRECT_BUFFER
            case 0x37: // INDIRECT_BUFFER_PFD
                if (depth < 4)
                    ExecuteBuffer(CommandProcessor::GpuToGuest(data[0]), data[1] & 0xFFFFF, depth + 1);
                break;

            case 0x50: // SET_BIN_MASK (hi, lo)
                g_binMask = (uint64_t(data[0]) << 32) | data[1];
                break;
            case 0x51: // SET_BIN_SELECT (hi, lo)
                g_binSelect = (uint64_t(data[0]) << 32) | data[1];
                break;
            case 0x60: // SET_BIN_MASK_LO
                g_binMask = (g_binMask & 0xFFFFFFFF00000000ull) | data[0];
                break;
            case 0x61: // SET_BIN_MASK_HI
                g_binMask = (g_binMask & 0x00000000FFFFFFFFull) | (uint64_t(data[0]) << 32);
                break;
            case 0x62: // SET_BIN_SELECT_LO
                g_binSelect = (g_binSelect & 0xFFFFFFFF00000000ull) | data[0];
                break;
            case 0x63: // SET_BIN_SELECT_HI
                g_binSelect = (g_binSelect & 0x00000000FFFFFFFFull) | (uint64_t(data[0]) << 32);
                break;

            case 0x54: // INTERRUPT
                for (uint32_t cpu = 0; cpu < 6; cpu++)
                {
                    if (data[0] & (1u << cpu))
                        DispatchInterrupt(1, cpu);
                }
                break;

            case 0x3D: // MEM_WRITE
            {
                uint32_t address = data[0] & 0x3FFFFFFF;
                for (uint32_t i = 1; i < count; i++, address += 4)
                    GpuStore(address, data[i]);
                break;
            }

            case 0x3E: // REG_TO_MEM
                GpuStore(data[1], g_registers[data[0] & 0xFFFF]);
                break;

            case 0x21: // REG_RMW
            {
                uint32_t info = data[0];
                uint32_t reg = info & 0x1FFF;
                uint32_t value = g_registers[reg];
                uint32_t andMask = (info >> 31) ? g_registers[data[1] & 0x1FFF] : data[1];
                uint32_t orMask = ((info >> 30) & 1) ? g_registers[data[2] & 0x1FFF] : data[2];
                WriteRegister(reg, (value & andMask) | orMask);
                break;
            }

            case 0x3C: // WAIT_REG_MEM
            {
                uint32_t info = data[0];
                bool isMemory = (info & 0x10) != 0;
                if (isMemory)
                {
                    // Wait for the CPU to write a value (bounded, so a mistake cannot hang the GPU thread):
                    // spinning first, as a sleep lasts about a millisecond on Windows.
                    for (int spin = 0; spin < 4000 && !CompareFunction(info, GpuLoad(data[1]) & data[3], data[2]); spin++)
                        DR_SPIN_PAUSE();
                    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
                    while (!CompareFunction(info, GpuLoad(data[1]) & data[3], data[2]))
                    {
                        if (std::chrono::steady_clock::now() > deadline)
                        {
                            static std::atomic<uint32_t> timeouts;
                            uint32_t n = ++timeouts;
                            if (n <= 3 || (n % 64) == 0)
                            {
                                LOGFN_WARNING("[cp] WAIT_REG_MEM timed out (#{}): mem 0x{:08X} value 0x{:08X} & mask 0x{:08X}, func {} ref 0x{:08X}",
                                    n, data[1], GpuLoad(data[1]), data[3], info & 7, data[2]);
                                if (n <= 3)
                                    DumpHistory();
                            }
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                    }
                }
                break; // register waits (vblank, idle states) are treated as satisfied
            }

            case 0x45: // COND_WRITE
            {
                uint32_t info = data[0];
                uint32_t value = (info & 0x10) ? GpuLoad(data[1]) : g_registers[data[1] & 0xFFFF];
                if (CompareFunction(info, value & data[3], data[2]))
                {
                    if (info & 0x100)
                        GpuStore(data[4], data[5]);
                    else
                        WriteRegister(data[4], data[5]);
                }
                break;
            }

            case 0x58: // EVENT_WRITE_SHD
            {
                uint32_t value = (data[0] >> 31) ? g_swapCounter.load() : data[2];
                GpuStore(data[1], value);
                break;
            }

            case 0x59: // EVENT_WRITE_EXT (screen extents): report the full surface
            {
                uint32_t address = data[1];
                uint16_t extents[6] = { 0, 0x2000, 0, 0x2000, 0, 0x2000 };
                for (uint32_t i = 0; i < 3; i++)
                    GpuStore(address + i * 4, (uint32_t(extents[i * 2]) << 16) | extents[i * 2 + 1]);
                break;
            }

            default:
                break; // state, constants, draws: nothing to do without a renderer
        }
    }

    void ExecuteBuffer(uint32_t guestStart, uint32_t dwords, int depth)
    {
        Reader r{ guestStart, 0, 0 };
        while (r.index < dwords)
            ExecutePacket(r, depth);
    }

    void CommandProcessorThread()
    {
        CpuSampler::RegisterCurrentThread("command processor"); // DR_CPU_PROFILE=1
        CpuSampler::WatchHitchThread("command processor"); // "[hitch profile]"
        uint32_t rptr = 0;
        bool loggedFirst = false;

        while (true)
        {
            uint32_t base = g_ringBase;
            uint32_t size = g_ringSizeDwords;
            uint32_t wptr = LoadBE(kGpuRegisterBase + kRegCpRbWptr * 4);

            if (base && size && wptr != rptr && wptr < size)
            {
                if (!loggedFirst)
                {
                    LOGFN("[cp] ring buffer active (write pointer {})", wptr);
                    loggedFirst = true;
                }

                Reader r{ base, size, rptr };
                while (r.index != wptr)
                    ExecutePacket(r, 0);
                rptr = wptr;

                if (uint32_t writeBack = g_rptrWriteBack)
                    *(be<uint32_t>*)g_memory.Translate(CommandProcessor::GpuToGuest(writeBack)) = rptr;
            }
            else
            {
                // A short spin first (the game usually kicks again within microseconds), then wait for
                // the next write pointer (or 1 ms, for writes that do not come through MMIO stores).
                bool kicked = false;
                for (int spin = 0; spin < 2000 && !kicked; spin++)
                {
                    DR_SPIN_PAUSE();
                    kicked = LoadBE(kGpuRegisterBase + kRegCpRbWptr * 4) != rptr;
                }
                if (!kicked)
                {
                    std::unique_lock lock(g_kickMutex);
                    g_kick.wait_for(lock, std::chrono::milliseconds(1), [] { return g_kickPending; });
                    g_kickPending = false;
                }
            }
        }
    }

    void VblankThread()
    {
        auto next = std::chrono::steady_clock::now();
        // Debugging: DR_VBLANK_HZ=<rate> (60 by default).
        static const int64_t period = getenv("DR_VBLANK_HZ") ? 1000000 / std::max(1, atoi(getenv("DR_VBLANK_HZ"))) : 16667;
        while (true)
        {
            next += std::chrono::microseconds(period);
            std::this_thread::sleep_until(next);
            DispatchInterrupt(0, 2);
        }
    }

    // GPU/display registers D3D reads directly through MMIO. In this runtime MMIO is plain memory,
    // so preset the values the console (and Xenia's GraphicsSystem::ReadRegister) reports.
    void PresetDisplayRegisters()
    {
        auto set = [](uint32_t index, uint32_t value)
        {
            *(be<uint32_t>*)g_memory.Translate(kGpuRegisterBase + index * 4) = value;
        };

        set(0x0F00, 0x08100748); // RB_EDRAM_TIMING
        set(0x0F01, 0x0000200E); // RB_BC_CONTROL
        set(0x194C, 0x000002D0); // D1MODE_V_COUNTER: D3D divides by the screen height derived from 0x1961
        set(0x1951, 0x00000001); // vblank interrupt status: without bit 0 the handler (0x8244A608) ignores
                                 // vblanks and never clears the swap-pending flag the command stream waits on
        set(0x1961, 0x050002D0); // D1MODE_VIEWPORT_SIZE: 1280x720
    }

    void StartThreads()
    {
        bool expected = false;
        if (g_started.compare_exchange_strong(expected, true))
        {
            PresetDisplayRegisters();
            std::thread(CommandProcessorThread).detach();
            std::thread(VblankThread).detach();
            LOGFN("[cp] command processor and vblank threads started");
        }
    }
}

uint32_t CommandProcessor::GpuToGuest(uint32_t address)
{
    // Physical addresses (< 512 MB) map to this runtime's physical heap at 0xA0000000.
    // Anything above is already a guest virtual address (MmGetPhysicalAddress is the identity here).
    return address < 0x20000000 ? (address | 0xA0000000) : address;
}

void CommandProcessor::InitializeRingBuffer(uint32_t base, uint32_t sizeLog2)
{
    g_ringBase = GpuToGuest(base);
    g_ringSizeDwords = (1u << (sizeLog2 + 3)) / 4; // Xenia: size = 1 << (log2 + 3) bytes
    StartThreads();
}

void CommandProcessor::EnableReadPointerWriteBack(uint32_t address, uint32_t blockLog2)
{
    g_rptrWriteBack = address;
}

void CommandProcessor::SetInterruptCallback(uint32_t callback, uint32_t userData)
{
    g_interruptCallback = callback;
    g_interruptData = userData;
}

void CommandProcessor::OnSwap()
{
    g_swapCounter++;
}

void CommandProcessor::OnRegisterWrite(uint32_t address)
{
    if (address != kGpuRegisterBase + kRegCpRbWptr * 4)
        return;
    {
        std::lock_guard lock(g_kickMutex);
        g_kickPending = true;
    }
    g_kick.notify_one();
}
