// Hooks on Dead Rising's Direct3D (addresses: docs/d3d_map.md).
//
// Stage 1 ("tap"): every hook records the call and then runs the original D3D function, so the
// game's own GPU synchronisation (fences, callbacks, command processor) keeps working while the
// host renderer is built up. Once the host renderer can draw everything, the hooks stop calling
// the originals and D3D is replaced the way UnleashedRecomp replaces Sonic's.

#include <stdafx.h>
#include "video.h"
#include "shaders.h"
#include "renderer.h"
#include "guest_device.h"
#include <kernel/function.h>
#include <os/logger.h>
#include <cpu/ppc_context.h>
#include <hid/input.h>
#include <patches/frame_rate.h>
#include <patches/enemy_trace.h>
#include <user/config.h>
#include <kernel/heap.h>

namespace
{
    struct Counters
    {
        std::atomic<uint32_t> draws{ 0 };
        std::atomic<uint32_t> vertexBatches{ 0 };
        std::atomic<uint32_t> clears{ 0 };
        std::atomic<uint32_t> resolves{ 0 };
        std::atomic<uint32_t> tiles{ 0 };
        std::atomic<uint32_t> textureBinds{ 0 };
        std::atomic<uint32_t> streamBinds{ 0 };
        std::atomic<uint32_t> shaderBinds{ 0 };
        std::atomic<uint32_t> renderTargetBinds{ 0 };
    };

    Counters g_counters;
    std::atomic<uint32_t> g_frame{ 0 };

    void EndFrame()
    {
        Video::FrameStats stats;
        stats.frame = ++g_frame;
        stats.draws = g_counters.draws.exchange(0);
        stats.vertexBatches = g_counters.vertexBatches.exchange(0);
        stats.clears = g_counters.clears.exchange(0);
        stats.resolves = g_counters.resolves.exchange(0);
        stats.tiles = g_counters.tiles.exchange(0);
        stats.textureBinds = g_counters.textureBinds.exchange(0);
        stats.streamBinds = g_counters.streamBinds.exchange(0);
        stats.shaderBinds = g_counters.shaderBinds.exchange(0);
        stats.renderTargetBinds = g_counters.renderTargetBinds.exchange(0);
        Video::SubmitFrame(stats);
    }
}

// Records, then runs the original D3D function.
#define DR_D3D_TAP(name, ...)                       \
    PPC_FUNC_IMPL(__imp__##name);                   \
    PPC_FUNC(name)                                  \
    {                                               \
        __VA_ARGS__;                                \
        __imp__##name(ctx, base);                   \
    }

// sMain's per-frame step (MT Framework, property table sub_82735598): +64 mFps (float), +68
// mFrameWait (byte: fixed step, mDeltaTime = 1 every frame), +112 mDeltaTime (in frames of 1/mFps).
// With mFrameWait clear the engine measures the frame (elapsed × mFps, at most 16).
PPC_EXTERN_FUNC(sub_821748A8); // finds a unit by class name (unit manager, name)

#ifdef _WIN32
// Debugging: DR_WATCH_OFFSET=<hex>[,<hex>...] (with DR_PLAYER_SNAP) logs every write to Frank's
// object + offset: the page is made read-only, the write faults, the handler logs the recompiled
// function that wrote (nearest function start below the faulting host address), lets the
// instruction run single-stepped and protects the page again.
namespace WriteWatch
{
    std::vector<uint32_t> g_addresses;           // guest addresses
    std::vector<std::pair<uintptr_t, uint32_t>> g_functions; // host start, guest address
    std::vector<uint8_t*> g_pages;
    thread_local uint8_t* t_stepping = nullptr;
    std::atomic<uint32_t> g_logged{ 0 };

    uint32_t GuestFunction(uintptr_t host)
    {
        auto it = std::upper_bound(g_functions.begin(), g_functions.end(), std::make_pair(host, UINT32_MAX));
        return it == g_functions.begin() ? 0 : std::prev(it)->second;
    }

    LONG CALLBACK Handler(EXCEPTION_POINTERS* info)
    {
        const DWORD code = info->ExceptionRecord->ExceptionCode;
        if (code == EXCEPTION_SINGLE_STEP && t_stepping != nullptr)
        {
            DWORD old;
            VirtualProtect(t_stepping, 4096, PAGE_READONLY, &old);
            t_stepping = nullptr;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (code != EXCEPTION_ACCESS_VIOLATION || info->ExceptionRecord->ExceptionInformation[0] != 1)
            return EXCEPTION_CONTINUE_SEARCH;
        auto* fault = reinterpret_cast<uint8_t*>(info->ExceptionRecord->ExceptionInformation[1]);
        uint8_t* page = reinterpret_cast<uint8_t*>(uintptr_t(fault) & ~uintptr_t(4095));
        if (std::find(g_pages.begin(), g_pages.end(), page) == g_pages.end())
            return EXCEPTION_CONTINUE_SEARCH;
        DWORD old;
        VirtualProtect(page, 4096, PAGE_READWRITE, &old);
        const uint32_t guest = uint32_t(fault - g_memory.base);
        for (uint32_t address : g_addresses)
            if (guest >= address && guest < address + 4 && g_logged++ < 400)
            {
                const uintptr_t rip = uintptr_t(info->ContextRecord->Rip);
                auto* ppc = GetPPCContext();
                LOGFN("[watch] write {:08X} (was {:08X}) by sub_{:08X} (host +0x{:X}), lr {:08X}", guest,
                    GuestDevice::Load32(address), GuestFunction(rip), rip - uintptr_t(GetModuleHandleW(nullptr)),
                    ppc ? uint32_t(ppc->lr) : 0);
            }
        t_stepping = page;
        info->ContextRecord->EFlags |= 0x100; // trap flag: re-protect after this instruction
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    void Arm(uint32_t player)
    {
        const char* spec = getenv("DR_WATCH_OFFSET");
        if (spec == nullptr || !g_addresses.empty())
            return;
        for (const char* p = spec; *p; )
        {
            char* end;
            const uint32_t value = uint32_t(strtoul(p, &end, 16));
            g_addresses.push_back(value >= 0x40000000 ? value : player + value); // absolute or Frank + offset
            p = *end == ',' ? end + 1 : end;
        }
        for (size_t i = 0; PPCFuncMappings[i].host != nullptr; i++)
            g_functions.emplace_back(uintptr_t(PPCFuncMappings[i].host), uint32_t(PPCFuncMappings[i].guest));
        std::sort(g_functions.begin(), g_functions.end());
        AddVectoredExceptionHandler(1, Handler);
        for (uint32_t address : g_addresses)
        {
            uint8_t* page = reinterpret_cast<uint8_t*>(uintptr_t(g_memory.Translate(address)) & ~uintptr_t(4095));
            g_pages.push_back(page);
            DWORD old;
            VirtualProtect(page, 4096, PAGE_READONLY, &old);
            LOGFN("[watch] watching {:08X}", address);
        }
    }
}
#endif

using GuestDevice::ClassName;

// Frank's per-frame update (uPlayer). His base motion layer sits at +240: flags +248 (bit 0: the
// motion system's "next frame reaches the end", set by sub_826E7F08 when frame + speed >= end),
// frame +260, end +264, speed +268; +16 is the unit's step. When the flag is set this function holds
// the frame at end - 1.0, so the next advance (speed x step) lands on the end and the motion moves
// on. With a step below 1 (60 fps) the advance never gets there: the frame went 20 -> 20.5 -> 20 for
// ever and Frank froze mid-stride. Hold one step (speed x step) before the end instead.
// Motion update of a model (all characters): layers of 272 bytes from +240, count at +224. A layer:
// +4 motion id (0xFFFF: empty), +8 status (bit 0: "the next frame reaches the end", set when
// frame + speed >= end), +10 mode (0x04 clamp at the end, 0x10 paused), +12 / +14 cross-fade
// remaining / total (int16), +20 frame, +24 end, +28 speed, +176 / +180 event tracks, +184 / +188
// their event bits for this update, +192 / +196 the bits of the update before. The update advances
// the frame by speed x step (the unit's step, +16: 0.5 at 60 fps), wraps or clamps it at the end,
// then sets the event bits. Three things in it count 30 fps frames; at 60 fps (step below 1):
//
// * Cross-fades count frames (remaining decremented once per call), so every transition took half
//   the time. One that has just started (remaining == total) gets both scaled by 1 / step.
//
// * Holds: when bit 0 is set, the game's per-unit code (about 40 places: Frank, zombies, survivors)
//   holds the frame at end - 1.0 so the next advance (1.0) lands on the end and the motion moves on
//   (a walk cycle wraps, an action ends). A 0.5 advance never got there: end - 1.0 -> end - 0.5 ->
//   held again, for ever. The unit froze mid-stride until something else changed its motion: Frank
//   while the stick was held, and zombies and survivors after the mall invasion (2026-11-25, "stuck
//   in place", "struggle to chase"). When a layer's frame was moved back to exactly end - 1.0 since
//   the last update (so not a frame that got there by advancing), it is put one advance before the
//   end instead, where the next advance ends the motion as at 30 fps.
//
// * Events: each track is a list of segments (+0 bits, +4 length in frames). The bits are the
//   segments overlapping frames int(frame) .. int(frame + speed - 0.00001): one 30 fps frame ahead
//   whatever the step, so at 60 fps every one-frame segment was set on three updates in a row
//   instead of one. Game code acts on some of them once per update: Frank's drink action uses the
//   item when the "use" bit is set and then clears his held item, so the second update called
//   through the cleared item and crashed (2026-11-25, coffee); X-press actions read the held item
//   the same way. At 60 fps the bits are set again here: the window looks speed x step ahead, so a
//   longer segment covers the same time as at 30 fps, and a one-frame segment is set only on the
//   update whose advance reaches its frame (previous frame < its frame <= this frame), once, as at
//   30 fps. A layer whose frame did not advance (paused, clamped at its end) keeps the game's own
//   bits, set on every update as on the console.
namespace MotionFix
{
    // Each layer's frame after its last update (layer address in the high half, frame bits in the
    // low half), so a frame moved back by game code can be told from one that advanced there. Lossy:
    // a slot taken by another layer just means no fix until the next update.
    static std::atomic<uint64_t> g_lastFrames[1 << 16];
    static std::atomic<uint32_t> g_holdsReleased{ 0 }, g_eventsKept{ 0 };
    static std::mutex g_classMutex;
    static std::map<std::string, uint32_t> g_holdClasses;

    static std::atomic<uint64_t>& Slot(uint32_t layer)
    {
        return g_lastFrames[(layer * 2654435761u) >> 16];
    }

    static uint32_t Bits(float value)
    {
        uint32_t bits;
        memcpy(&bits, &value, 4);
        return bits;
    }

    // How far a layer's frame moved in this update: (before, after], or past the end of a loop and
    // round to after, or nowhere (clamped at the end, or set back).
    enum class Moved { Forward, Wrapped, Nowhere };

    // The event bits of one track. Segments longer than a frame: the game's own loop (segments with
    // key > lo, up to and including the first with key > hi; key = end frame of the segment). One-frame
    // segments: only when their frame was reached by this update's advance.
    static uint32_t TrackBits(uint32_t list, int32_t lo, int32_t hi, float before, float after, Moved moved, uint32_t& kept)
    {
        if (list == 0)
            return 0;
        const int32_t count = int32_t(GuestDevice::Load32(list + 64));
        const uint32_t entries = GuestDevice::Load32(list + 68);
        uint32_t bits = 0;
        int32_t key = 0;
        bool windowDone = false;
        for (int32_t j = 0; j < count && j < 4096; j++)
        {
            const uint32_t entry = entries + uint32_t(j) * 8;
            const int32_t length = int32_t(GuestDevice::Load32(entry + 4));
            key += length;
            const bool inWindow = !windowDone && key > lo;
            if (key > hi)
                windowDone = true;
            if (length == 1)
            {
                const float frame = float(key - 1);
                const bool reached = moved == Moved::Forward ? (frame > before && frame <= after)
                    : moved == Moved::Wrapped ? (frame > before || frame <= after)
                    : false;
                const uint32_t segment = GuestDevice::Load32(entry);
                if (reached)
                    bits |= segment;
                else if (inWindow && segment != 0)
                    kept++;
            }
            else if (inWindow)
                bits |= GuestDevice::Load32(entry);
            // Past both the window and the advance (a wrapped advance can reach the list's end).
            if (windowDone && moved != Moved::Wrapped && float(key - length) > after)
                break;
        }
        return bits;
    }
}

PPC_FUNC_IMPL(__imp__sub_826E7F08);
PPC_FUNC(sub_826E7F08)
{
    using namespace MotionFix;
    const uint32_t model = ctx.r3.u32;
    const float step = GuestDevice::LoadFloat(model + 16);
    if (!(step > 0.0f && step < 0.75f))
    {
        __imp__sub_826E7F08(ctx, base);
        return;
    }

    const uint32_t layers = std::min<uint32_t>(GuestDevice::Load32(model + 224), 64);
    const int32_t scale = int32_t(std::lround(1.0f / step));
    float before[64];
    for (uint32_t i = 0; i < layers; i++)
    {
        const uint32_t layer = model + 240 + i * 272;
        const int16_t remaining = int16_t(GuestDevice::Load16(layer + 12));
        const int16_t total = int16_t(GuestDevice::Load16(layer + 14));
        if (total > 0 && remaining == total && total * scale <= 32767)
        {
            GuestDevice::Store16(layer + 12, uint16_t(total * scale));
            GuestDevice::Store16(layer + 14, uint16_t(total * scale));
        }

        float frame = GuestDevice::LoadFloat(layer + 20);
        const float end = GuestDevice::LoadFloat(layer + 24);
        const float advance = GuestDevice::LoadFloat(layer + 28) * step;
        if ((GuestDevice::Load16(layer + 8) & 1) != 0 && (GuestDevice::Load16(layer + 10) & 0x10) == 0 && frame == end - 1.0f &&
            advance > 0.0f && advance < 1.0f)
        {
            const uint64_t last = Slot(layer).load(std::memory_order_relaxed);
            if (uint32_t(last >> 32) == layer && uint32_t(last) != Bits(frame))
            {
                frame = end - advance;
                GuestDevice::StoreFloat(layer + 20, frame);
                EnemyTrace::OnHoldReleased(model);
                if (g_holdsReleased++ % 64 == 0)
                {
                    std::lock_guard lock(g_classMutex);
                    if (g_holdClasses.size() < 64)
                        g_holdClasses[ClassName(model)]++;
                }
            }
        }
        before[i] = frame;
    }

    __imp__sub_826E7F08(ctx, base);

    uint32_t kept = 0;
    for (uint32_t i = 0; i < layers; i++)
    {
        const uint32_t layer = model + 240 + i * 272;
        const float frame = GuestDevice::LoadFloat(layer + 20);
        Slot(layer).store((uint64_t(layer) << 32) | Bits(frame), std::memory_order_relaxed);
        if (GuestDevice::Load16(layer + 4) == 0xFFFF)
            continue;
        const float advance = GuestDevice::LoadFloat(layer + 28) * step;
        if (!(advance > 0.0f))
            continue; // reversed or stopped: left as the game set it
        int32_t lo = int32_t(frame);
        int32_t hi = int32_t(frame + advance - 0.00001f);
        if (hi < lo)
            std::swap(lo, hi);
        // Backwards with a forward speed: a loop wrapped, or a clamped motion was held at its end.
        const Moved moved = frame >= before[i] ? Moved::Forward : (GuestDevice::Load16(layer + 10) & 0x04) == 0 ? Moved::Wrapped : Moved::Nowhere;
        // A layer that did not advance (paused, or clamped back to its end) keeps the game's bits: the
        // console sets the window's segments on every update while a frame stands still, and game code
        // reads them as a state. The zombie's knocked-down and blown actions pause the landing motion on
        // its last frame and the next action is chosen from that motion's event bit 1 (+424, sub_8236D508:
        // HP 0 -> lie dead, action 7); with only "reached" one-frame segments it was never set at 60 fps
        // and dead zombies got straight back up (2026-11-25).
        if (frame == before[i] || moved == Moved::Nowhere)
            continue;
        GuestDevice::Store32(layer + 184, TrackBits(GuestDevice::Load32(layer + 176), lo, hi, before[i], frame, moved, kept));
        GuestDevice::Store32(layer + 188, TrackBits(GuestDevice::Load32(layer + 180), lo, hi, before[i], frame, moved, kept));
    }
    if (kept != 0)
        g_eventsKept += kept;
}

// The game's heaps (MT Framework allocators, sub_82760D18 registers each in the table at 0x828AD778,
// count at 0x828AED08): +4 bytes used, +8 size, +0xC name. Units (zombies, items, survivors) come
// from "Unit" (18 MB, *0x828AED14); an item that cannot be allocated is null, and Frank's actions
// on it crashed (2026-11-25). Logged every 600 steps and when Unit passes 90% ("[heap]").
static std::atomic<uint32_t> g_unitManager{ 0 }; // the unit manager (sUnit), from its move pass

static std::string HeapSummary(bool all)
{
    std::string text;
    const uint32_t count = std::min<uint32_t>(GuestDevice::Load32(0x828AED08), 64);
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t heap = GuestDevice::Load32(0x828AD778 + i * 4);
        if (heap == 0)
            continue;
        const uint32_t size = GuestDevice::Load32(heap + 8);
        if (size == 0 || (!all && heap != GuestDevice::Load32(0x828AED14)))
            continue;
        const uint32_t used = GuestDevice::Load32(heap + 4);
        const uint32_t nameAddress = GuestDevice::Load32(heap + 0xC);
        std::string name = "?";
        if (nameAddress >= 0x82000000 && nameAddress < 0x83000000)
            name = static_cast<const char*>(g_memory.Translate(nameAddress));
        text += fmt::format(" {} {:.1f}/{:.1f} MB", name, used / 1048576.0, size / 1048576.0);
    }
    text += fmt::format(" | physical {:.0f}/512 MB", g_userHeap.physicalInUse / 1048576.0);
    return text;
}

// The Unit heap's size (sub_827614D0 makes each heap: allocator, name, attributes, size; Unit is
// 18 MB at 0x8218E5B4). Every unit (zombies, survivors, items, effects) comes from it, and an item
// it cannot hold is null: in the busy mall Frank's held item came back null and X crashed
// (2026-11-25). It is 36 MB here: the game used at most about 400 of the 512 MB of physical
// memory in the tests, so 18 MB more fits. DR_UNIT_HEAP_MB=<n> sets another size (18: the console's).
PPC_FUNC_IMPL(__imp__sub_827614D0);
PPC_FUNC(sub_827614D0)
{
    if (ctx.r4.u32 == 0x82089B1C) // "Unit"
    {
        static const uint32_t megabytes = getenv("DR_UNIT_HEAP_MB") ? uint32_t(atoi(getenv("DR_UNIT_HEAP_MB"))) : 36;
        if (megabytes != 0)
            ctx.r6.u64 = uint64_t(megabytes) << 20;
        LOGFN("[heap] Unit heap: {:.0f} MB", ctx.r6.u32 / 1048576.0);
    }
    __imp__sub_827614D0(ctx, base);
}

// Units by class in the move lines (the commonest few).
static std::string UnitSummary(uint32_t manager)
{
    std::map<std::string, uint32_t> classes;
    uint32_t total = 0;
    for (uint32_t line = 0; manager != 0 && line < 32; line++)
    {
        uint32_t count = 0;
        for (uint32_t unit = GuestDevice::Load32(manager + 44 + line * 20 + 4); unit != 0 && count < 4000; unit = GuestDevice::Load32(unit + 8), count++)
        {
            classes[GuestDevice::ClassName(unit)]++;
            total++;
        }
    }
    std::vector<std::pair<uint32_t, std::string>> order;
    for (auto& [name, n] : classes)
        order.emplace_back(n, name);
    std::sort(order.rbegin(), order.rend());
    std::string text = fmt::format(" {} units:", total);
    for (size_t i = 0; i < order.size() && i < 8; i++)
        text += fmt::format(" {}x{}", order[i].second.empty() ? "?" : order[i].second, order[i].first);
    return text;
}

// Frank's held item, traced (2026-11-25: X presses in the busy mall crashed with his state saying he
// holds an item while +0xE78, the held item, was empty or had an index with no item table entry).
// Every change of +0xE78 (held item), its item index (+0x2C10), +0xE7C (the bare-hand special
// move entry), +22 (action) and +0x1180 (state), before and after his move, goes into a ring of
// the last 96 changes, which DumpPlayerTrace logs (guards, crash reports).
static std::atomic<uint32_t> g_gameStep{ 0 };
namespace
{
    struct PlayerSnapshot
    {
        uint32_t step = 0, holder = 0, special = 0, index = 0, entry = 0, action = 0, state = 0, buttons = 0;
        const char* when = "";
        bool operator==(const PlayerSnapshot& o) const
        {
            return holder == o.holder && special == o.special && index == o.index && action == o.action && state == o.state;
        }
    };
    std::mutex g_traceMutex;
    std::array<PlayerSnapshot, 96> g_trace;
    size_t g_traceNext = 0;
    PlayerSnapshot g_traceLast;

    PlayerSnapshot SnapshotPlayer(uint32_t player, const char* when)
    {
        PlayerSnapshot s;
        s.step = g_gameStep.load();
        s.holder = GuestDevice::Load32(player + 0xE78);
        s.special = GuestDevice::Load32(player + 0xE7C);
        s.index = s.holder != 0 ? GuestDevice::Load16(s.holder + 0x2C10) : 0xFFFF;
        s.entry = s.index < 0x400 ? GuestDevice::Load32(0x82803270 + s.index * 4) : 0;
        s.action = GuestDevice::Load8(player + 22);
        s.state = GuestDevice::Load16(player + 0x1180);
        s.buttons = (GuestDevice::Load32(player + 0x1030) & 0xFFFF) << 16 | (GuestDevice::Load32(player + 0x1034) & 0xFFFF);
        s.when = when;
        return s;
    }

    void TracePlayer(uint32_t player, const char* when)
    {
        const PlayerSnapshot s = SnapshotPlayer(player, when);
        std::lock_guard lock(g_traceMutex);
        if (s == g_traceLast)
            return;
        // Equipping, dropping and using items (the held item, its index or the item id at +0x1180
        // changed) are logged as they happen: the X crashes had an item id with no held object.
        if (s.holder != g_traceLast.holder || s.index != g_traceLast.index || s.state != g_traceLast.state)
        {
            static uint32_t logged;
            if (logged++ < 600)
                LOGFN("[held] step {} {}: unit {:08X} {} holder {:08X} {} index {} entry {:08X} special {:08X} action {} item id {}",
                    s.step, s.when, player, GuestDevice::ClassName(player), s.holder, s.holder != 0 ? GuestDevice::ClassName(s.holder) : "-",
                    s.index, s.entry, s.special, s.action, s.state);
        }
        g_traceLast = s;
        g_trace[g_traceNext++ % g_trace.size()] = s;
    }
}

void DumpPlayerTrace()
{
    std::unique_lock lock(g_traceMutex, std::try_to_lock); // also called from the crash handler
    if (!lock.owns_lock())
        return;
    LOGN("[held] Frank's held item and state, last changes (oldest first):");
    for (size_t i = 0; i < g_trace.size(); i++)
    {
        const PlayerSnapshot& s = g_trace[(g_traceNext + i) % g_trace.size()];
        if (s.when[0] == 0)
            continue;
        LOGFN("[held]   step {} {}: holder {:08X} {} index {} entry {:08X} special {:08X} action {} state {} buttons {:08X}", s.step, s.when,
            s.holder, s.holder != 0 ? GuestDevice::ClassName(s.holder) : "-", s.index, s.entry, s.special, s.action, s.state, s.buttons);
    }
}

// The held item's object, made for an item id (1-313) out of the Unit heap; null when that fails.
PPC_FUNC_IMPL(__imp__sub_8215FC48);
PPC_FUNC(sub_8215FC48)
{
    const uint32_t id = ctx.r5.u32 & 0xFFFF;
    __imp__sub_8215FC48(ctx, base);
    if (ctx.r3.u32 == 0 && id >= 1 && id <= 313)
    {
        static uint32_t logged;
        if (logged++ < 40)
            LOGFN_WARNING("[item] item {} could not be made (step {}):{}{}", id, g_gameStep.load(), HeapSummary(false), UnitSummary(g_unitManager.load()));
    }
}

// uPlayer::move (vtable +24, run as a job by sub_8242DCA0).
PPC_FUNC_IMPL(__imp__sub_821FE518);
PPC_FUNC(sub_821FE518)
{
    const uint32_t player = ctx.r3.u32;
    TracePlayer(player, "before move");
    __imp__sub_821FE518(ctx, base);
    TracePlayer(player, "after move");
}

// Frank's state 35 sub-handler (table 0x828A3600 by +0x1148, from sub_822A6000): reads the held
// item (+0xE78) or, without one, the special move entry (+0xE7C); with neither it read guest 0xC
// and crashed (2026-11-25). Skipped there and logged until the cause is known.
PPC_FUNC_IMPL(__imp__sub_822A65B0);
PPC_FUNC(sub_822A65B0)
{
    const uint32_t player = ctx.r3.u32;
    // The path that reads them: (+0x112F set or +0x1A8 bit 5) and +0x1154 clear.
    const bool reads = (GuestDevice::Load8(player + 0x112F) != 0 || ((GuestDevice::Load32(player + 0x1A8) >> 5) & 1) != 0) &&
        GuestDevice::Load8(player + 0x1154) == 0;
    // Without a held item it takes the special entry (+0xE7C) chosen by +0x106C (80-byte entries),
    // and reads null when there is none or the chosen one's id (+0) is not the entry's.
    const uint32_t special = GuestDevice::Load32(player + 0xE7C);
    const bool noSpecial = special == 0 ||
        GuestDevice::Load16(special) != GuestDevice::Load16(special + GuestDevice::Load8(player + 0x106C) * 80);
    if (reads && GuestDevice::Load32(player + 0xE78) == 0 && noSpecial)
    {
        static uint32_t logged;
        if (logged++ < 10)
        {
            LOGFN_WARNING("[guard] sub_822A65B0: player {:08X} action {} state {} sub {}: no held item and no special entry; skipped",
                player, GuestDevice::Load8(player + 22), GuestDevice::Load16(player + 0x1180), GuestDevice::Load8(player + 0x1148));
            DumpPlayerTrace();
        }
        return;
    }
    __imp__sub_822A65B0(ctx, base);
}

// Frank's "use item" action (eat / drink; action table 0x828A3538 by +22, from sub_822A6000).
// Its second step (+21 == 1), when the motion's use event fires (+0x1A8 bit 2), calls the held
// item's method (+0xE78, vtable +220) and consumes it; with no held object it called through null
// (2026-11-25, drinking a coffee jug). Until the cause is known the action ends there instead
// (returns 1: done), with nothing consumed, and the trace is logged.
PPC_FUNC_IMPL(__imp__sub_822A9498);
PPC_FUNC(sub_822A9498)
{
    const uint32_t player = ctx.r3.u32;
    if (GuestDevice::Load8(player + 21) == 1 && ((GuestDevice::Load32(player + 0x1A8) >> 2) & 1) != 0 &&
        GuestDevice::Load32(player + 0xE78) == 0)
    {
        static uint32_t logged;
        if (logged++ < 10)
        {
            LOGFN_WARNING("[guard] sub_822A9498: player {:08X} uses item id {} with no held object; action ended",
                player, GuestDevice::Load16(player + 0x1180));
            DumpPlayerTrace();
        }
        ctx.r3.u64 = 1;
        return;
    }
    __imp__sub_822A9498(ctx, base);
}

// One of Frank's action handlers (called through the state table at 0x828A3820 with the input flags
// from sub_822C0700). With flag 0x04 (after an X press) it reads the held item's entry in the static
// item table at 0x82803270 by the index at (player+0xE78)+0x2C10, and the entry's name index at +10.
// The table has gaps (index 0 included): a gap read guest address 0xA and crashed in the busy mall
// (2026-11-25). Until the cause is known the action is skipped there and logged ("[guard]").
PPC_FUNC_IMPL(__imp__sub_822BECE8);
PPC_FUNC(sub_822BECE8)
{
    const uint32_t player = ctx.r3.u32;
    const uint32_t flags = ctx.r4.u32 & 0xFF;
    if ((flags & 0x40) == 0 && (flags & 0x04) != 0)
    {
        const uint32_t holder = GuestDevice::Load32(player + 0xE78);
        const uint32_t index = holder != 0 ? GuestDevice::Load16(holder + 0x2C10) : 0;
        const uint32_t entry = holder != 0 ? GuestDevice::Load32(0x82803270 + index * 4) : 0;
        if (entry == 0)
        {
            static uint32_t logged;
            if (logged++ < 20)
            {
                LOGFN_WARNING("[guard] sub_822BECE8: player {:08X} ({}) state {} flags {:02X}: item index {} has no table entry (holder {:08X} {}); action skipped",
                    player, ClassName(player), GuestDevice::Load16(player + 0x1180), flags, index, holder, holder != 0 ? ClassName(holder) : "");
                DumpPlayerTrace();
            }
            ctx.r3.u64 = 0;
            return;
        }
    }
    __imp__sub_822BECE8(ctx, base);
}

PPC_FUNC_IMPL(__imp__sub_822C21D0);
PPC_FUNC(sub_822C21D0)
{
    const uint32_t self = ctx.r3.u32;
    __imp__sub_822C21D0(ctx, base);
    const float step = GuestDevice::LoadFloat(self + 16);
    if (step < 1.0f && (GuestDevice::Load16(self + 248) & 1) != 0)
    {
        const float end = GuestDevice::LoadFloat(self + 264);
        const float frame = GuestDevice::LoadFloat(self + 260);
        const float advance = GuestDevice::LoadFloat(self + 268) * step;
        if (frame == end - 1.0f && advance > 0.0f && advance < 1.0f)
            GuestDevice::StoreFloat(self + 260, end - advance);
    }
}

// The unit manager's move pass (sUnit, r3): 32 move lines at +44, 20 bytes each ([+0] flags, [+4]
// first unit); units chain through +8. Debugging: DR_UNIT_LIST=<every n steps> logs each line's
// classes (count per class).

// First unit of a class in the move lines (0 if none).
static uint32_t FindUnit(const std::string& className)
{
    const uint32_t manager = g_unitManager.load();
    for (uint32_t line = 0; manager != 0 && line < 32; line++)
    {
        uint32_t count = 0;
        for (uint32_t unit = GuestDevice::Load32(manager + 44 + line * 20 + 4); unit != 0 && count < 4000; unit = GuestDevice::Load32(unit + 8), count++)
            if (ClassName(unit) == className)
                return unit;
    }
    return 0;
}

PPC_FUNC_IMPL(__imp__sub_82752FE0);
PPC_FUNC(sub_82752FE0)
{
    const uint32_t manager = ctx.r3.u32;
    g_unitManager = manager;
    __imp__sub_82752FE0(ctx, base);
    static const uint32_t every = getenv("DR_UNIT_LIST") ? uint32_t(atoi(getenv("DR_UNIT_LIST"))) : 0;
    static uint32_t calls;
    if (every != 0 && (++calls % every) == 0)
    {
        for (uint32_t line = 0; line < 32; line++)
        {
            std::map<std::string, uint32_t> classes;
            uint32_t count = 0;
            for (uint32_t unit = GuestDevice::Load32(manager + 44 + line * 20 + 4); unit != 0 && count < 2000; unit = GuestDevice::Load32(unit + 8), count++)
                classes[ClassName(unit)]++;
            if (classes.empty())
                continue;
            std::string text;
            for (auto& [name, n] : classes)
                text += fmt::format(" {}x{}", name.empty() ? "?" : name, n);
            LOGFN("[units] step {} line {} flags {:08X}:{}", calls, line, GuestDevice::Load32(manager + 44 + line * 20), text);
        }
    }
}

PPC_FUNC_IMPL(__imp__sub_82734808);
PPC_FUNC(sub_82734808)
{
    // 60 fps (Config::FrameRate, or DR_VARIABLE_STEP=1 for tests): the engine measures each frame
    // (mFrameWait clear); back at 30 its fixed step returns (mFrameWait 1, as the game sets it).
    static const bool forceVariable = getenv("DR_VARIABLE_STEP") != nullptr;
    const bool variable = forceVariable || Config::FrameRate == 60;
    static bool wasVariable = false;
    if (variable || wasVariable)
        GuestDevice::Store8(ctx.r3.u32 + 68, variable ? 0 : 1);
    wasVariable = variable;
    const uint32_t main = ctx.r3.u32;
    static bool scanned;
    if (variable && !scanned)
    {
        // Debugging: where the game keeps pointers to sMain.
        scanned = true;
        std::string refs;
        for (uint32_t a = 0x82000000; a < 0x83000000; a += 4)
            if (GuestDevice::Load32(a) == main)
                refs += fmt::format(" {:08X}", a);
        LOGFN("[frame] sMain at {:08X}; pointers to it:{}", main, refs);
    }
    if (static const bool order = getenv("DR_ORDER_LOG") != nullptr; order)
    {
        static uint32_t n;
        if (n++ < 20)
            LOGFN("[order] step");
    }
    __imp__sub_82734808(ctx, base);
    static uint32_t calls, odd;
    g_gameStep++;
    {
        // Every 1800 steps the heaps; the units by class (a walk over every unit, on this thread)
        // only once the Unit heap is over 60% full, and every 120 steps while it is over 90%.
        static uint32_t heapSteps, fullLogged;
        const uint32_t unitHeap = GuestDevice::Load32(0x828AED14);
        const uint32_t unitSize = unitHeap != 0 ? GuestDevice::Load32(unitHeap + 8) : 0;
        const uint32_t unitUsed = unitHeap != 0 ? GuestDevice::Load32(unitHeap + 4) : 0;
        const bool busy = unitSize != 0 && unitUsed > unitSize / 10 * 6;
        const bool nearlyFull = unitSize != 0 && unitUsed > unitSize / 10 * 9;
        if (++heapSteps % 1800 == 0 || (nearlyFull && fullLogged++ % 120 == 0))
            LOGFN("[heap] step {}:{}{}", g_gameStep.load(), HeapSummary(true), busy ? UnitSummary(g_unitManager.load()) : std::string());
        // The 60 fps motion fixes (sub_826E7F08 above), when any were needed in the last 1800 steps.
        if (heapSteps % 1800 == 0 && (MotionFix::g_holdsReleased != 0 || MotionFix::g_eventsKept != 0))
        {
            std::string classes;
            {
                std::lock_guard lock(MotionFix::g_classMutex);
                for (auto& [name, n] : MotionFix::g_holdClasses)
                    classes += fmt::format(" {}x{}", name.empty() ? "?" : name, n);
                MotionFix::g_holdClasses.clear();
            }
            LOGFN("[motion] last 1800 steps: {} held motions released (sampled classes:{}), {} repeats of one-frame events dropped",
                MotionFix::g_holdsReleased.exchange(0), classes, MotionFix::g_eventsKept.exchange(0));
        }
    }
    // Debugging: DR_FIXED_DELTA=<step> replaces the engine's step every frame.
    if (static const char* fixed = getenv("DR_FIXED_DELTA"); fixed != nullptr)
    {
        const float value = float(atof(fixed));
        uint32_t bits;
        memcpy(&bits, &value, 4);
        *static_cast<uint32_t*>(g_memory.Translate(main + 112)) = __builtin_bswap32(bits);
    }
    const float delta = GuestDevice::LoadFloat(main + 112);
    calls++;
    FrameRate::OnStep(delta);
    if (static bool listed = false; !listed && getenv("DR_SYSTEM_LIST") != nullptr && calls > 300)
    {
        listed = true;
        for (uint32_t k = 400; k < 720; k += 4)
        {
            const uint32_t object = GuestDevice::Load32(main + 0x10000 + k);
            const std::string name = ClassName(object);
            LOGFN("[systems] sMain+0x{:X}: {:08X} {} (vtable {:08X})", 0x10000 + k, object, name, object >= 0x40000000 && object < 0xC0000000 ? GuestDevice::Load32(object) : 0);
        }
    }
    if (variable && (delta < 0.1f || delta > 1.5f) && odd++ < 200)
        LOGFN("[frame] step {}: mDeltaTime {:.3f} pause {} {}", calls, delta, GuestDevice::Load32(main + 104), GuestDevice::Load8(main + 108));
    if (variable && (calls % 300) == 0)
        LOGFN("[frame] mFps {} mDeltaTime {:.3f}", GuestDevice::LoadFloat(main + 64), delta);

    // Debugging: DR_SCAN_VTABLE=<hex vtable>,<step>[,<steps>]: at that step finds every object with
    // that vtable (heaps at 0x40000000 and 0xA0000000), then writes each object (+0x100) and the
    // 2560 bytes its +64 points at to scan/<step>.bin every step (cSprAnm: 0x82061740).
    if (static const char* scan = getenv("DR_SCAN_VTABLE"); scan != nullptr)
    {
        static uint32_t vtable, at, steps = 120;
        static std::vector<uint32_t> objects;
        if (vtable == 0)
            sscanf(scan, "%x,%u,%u", &vtable, &at, &steps);
        if (calls == at)
        {
            for (uint64_t a = 0x40000000; a < 0x60000000 && objects.size() < 256; a += 4)
                if (GuestDevice::Load32(uint32_t(a)) == vtable)
                    objects.push_back(uint32_t(a));
            for (uint64_t a = 0xA0000000; a < 0xC0000000 && objects.size() < 256; a += 4)
                if (GuestDevice::Load32(uint32_t(a)) == vtable)
                    objects.push_back(uint32_t(a));
            std::string list;
            for (uint32_t o : objects)
                list += fmt::format(" {:08X}", o);
            LOGFN("[scan] step {}: {} objects with vtable {:08X}:{}", calls, objects.size(), vtable, list);
        }
        if (calls >= at && calls < at + steps && !objects.empty())
        {
            std::filesystem::create_directories("scan");
            std::ofstream out(fmt::format("scan/{:06}.bin", calls), std::ios::binary);
            for (uint32_t o : objects)
            {
                out.write(static_cast<const char*>(g_memory.Translate(o)), 0x100);
                const uint32_t slots = GuestDevice::Load32(o + 64);
                static const std::vector<char> zero(2560, 0);
                out.write(slots >= 0x40000000 && slots < 0xC0000000 ? static_cast<const char*>(g_memory.Translate(slots)) : zero.data(), 2560);
            }
        }
    }

    // Debugging: DR_PLAYER_SNAP=<first step> writes Frank's object (uPlayer, found the way the game
    // finds it: sub_821748A8(unit manager 0x828AECC4, "uPlayer")) to snap/ every 5 steps.
    static const uint32_t snapFirst = getenv("DR_PLAYER_SNAP") ? uint32_t(atoi(getenv("DR_PLAYER_SNAP"))) : 0;
    if (snapFirst != 0 && calls >= snapFirst)
    {
        static uint32_t player;
        if (player == 0 || (calls % 60) == 0)
        {
            PPCContext saved = ctx;
            ctx.r3.u32 = GuestDevice::Load32(0x828AECC4);
            ctx.r4.u32 = 0x82088D98; // "uPlayer"
            if (ctx.r3.u32 != 0)
                sub_821748A8(ctx, base);
            const uint32_t marker = ctx.r3.u32;
            // That is the AI marker (uNpcMarkerHumanEntity); +0x70 is the uPlayer itself.
            uint32_t found = marker != 0 && ClassName(GuestDevice::Load32(marker + 0x70)) == "uPlayer" ? GuestDevice::Load32(marker + 0x70) : marker;
            ctx = saved;
            // DR_SNAP_CLASS=<class>[:<hex offset>]: that unit (or the object it points to there)
            // instead of Frank.
            if (const char* snapClass = getenv("DR_SNAP_CLASS"))
            {
                std::string name(snapClass);
                uint32_t follow = UINT32_MAX;
                if (const size_t colon = name.find(':'); colon != std::string::npos)
                {
                    follow = uint32_t(strtoul(name.c_str() + colon + 1, nullptr, 16));
                    name.resize(colon);
                }
                found = FindUnit(name);
                if (found != 0 && follow != UINT32_MAX)
                    found = GuestDevice::Load32(found + follow);
            }
            if (found != player)
            {
                LOGFN("[frame] uPlayer at {:08X} (vtable {:08X}, class {})", found, found ? GuestDevice::Load32(found) : 0, ClassName(found));
                // The objects Frank points to, by class.
                for (uint32_t offset = 0; found != 0 && offset < 0x8000; offset += 4)
                {
                    const uint32_t pointee = GuestDevice::Load32(found + offset);
                    const std::string name = ClassName(pointee);
                    if (!name.empty())
                        LOGFN("[frame]   +0x{:04X} -> {:08X} {} (vtable {:08X})", offset, pointee, name, GuestDevice::Load32(pointee));
                }
            }
            player = found;
#ifdef _WIN32
            if (player != 0 && getenv("DR_WATCH_JUMP") == nullptr)
                WriteWatch::Arm(player);
#endif
        }
        static const uint32_t snapSize = getenv("DR_PLAYER_SNAP_SIZE") ? uint32_t(strtoul(getenv("DR_PLAYER_SNAP_SIZE"), nullptr, 0)) : 0x20000;
        static const uint32_t snapCount = getenv("DR_PLAYER_SNAP_COUNT") ? uint32_t(atoi(getenv("DR_PLAYER_SNAP_COUNT"))) : 2000;

        // DR_WALK_TEST=<steps>: that many steps after Frank first exists, hold the left stick up for
        // 600 steps (through the touch-pad input) and take the snapshots from 10 steps before it
        // (named by step relative to the walk start + 1000).
        static const uint32_t walkDelay = getenv("DR_WALK_TEST") ? uint32_t(atoi(getenv("DR_WALK_TEST"))) : 0;
        static uint32_t walkStart;
        if (walkDelay != 0 && player != 0 && walkStart == 0)
        {
            walkStart = calls + walkDelay;
            LOGFN("[frame] walk test: holding up from step {}", walkStart);
        }
        if (walkDelay != 0 && walkStart != 0)
        {
            // Time in 30 fps frames (sum of steps) since the walk started, so 30 and 60 fps runs
            // press the same buttons at the same game time. DR_JUMP_TEST: press A (jump) 60 frames
            // in and log Frank's height for the next 60 frames.
            static float elapsed;
            static bool holding, jumping;
            if (calls >= walkStart)
                elapsed += delta;
            static const bool jumpTest = getenv("DR_JUMP_TEST") != nullptr;
            const bool hold = calls >= walkStart && elapsed < 600.0f;
            const bool jump = jumpTest && elapsed >= 60.0f && elapsed < 62.0f;
            static const bool pauseTest = getenv("DR_PAUSE_TEST") != nullptr; // press Start 120 frames in
            const bool pause = pauseTest && elapsed >= 120.0f && elapsed < 122.0f;
            static bool pausing;
            if (hold != holding || jump != jumping || pause != pausing)
            {
                Input::Pad pad;
                pad.thumbLY = hold ? 32767 : 0;
                pad.buttons = (jump ? 0x1000 : 0) | (pause ? 0x0010 : 0); // A, Start
                pausing = pause;
                Input::SetTouchPad(pad);
                holding = hold;
                jumping = jump;
            }
#ifdef _WIN32
            if (jumpTest && elapsed >= 60.0f && getenv("DR_WATCH_JUMP") != nullptr)
                WriteWatch::Arm(player);
#endif
            if (jumpTest && elapsed >= 59.0f && elapsed < 120.0f)
                LOGFN("[jump] t {:.2f} y {:.2f} frame {:.2f} x {:.2f} z {:.2f}", elapsed - 60.0f, GuestDevice::LoadFloat(player + 0x34), GuestDevice::LoadFloat(player + 260), GuestDevice::LoadFloat(player + 0x30), GuestDevice::LoadFloat(player + 0x38));
        }
        const uint32_t snapFrom = walkDelay != 0 ? (walkStart != 0 ? walkStart - 10 : UINT32_MAX) : snapFirst;
        static const uint32_t snapEvery = getenv("DR_PLAYER_SNAP_EVERY") ? uint32_t(atoi(getenv("DR_PLAYER_SNAP_EVERY"))) : 5;
        if (player != 0 && (calls % snapEvery) == 0 && calls >= snapFrom && calls < snapFrom + snapCount)
        {
            std::filesystem::create_directories("snap");
            const uint32_t name = walkDelay != 0 ? calls + 1000 - walkStart : calls;
            std::ofstream(fmt::format("snap/{:06}.bin", name), std::ios::binary)
                .write(static_cast<const char*>(g_memory.Translate(player)), snapSize);
        }
    }
}

// Present. The presentation interval (device + 0x33C0, D3DPRESENT_INTERVAL_*: the game uses TWO,
// 30 fps) decides how many vblanks each swap waits for (sub_8245B818).
DR_D3D_TAP(sub_8245BA18,
    if (static const char* interval = getenv("DR_PRESENT_INTERVAL"); interval != nullptr)
        GuestDevice::Store32(ctx.r3.u32 + 0x33C0, uint32_t(atoi(interval)));
    else
    {
        // 60 fps: one vblank per swap. Back at 30: the game's D3DPRESENT_INTERVAL_TWO (2), written
        // outright: reading it when 60 was turned on gave 1 when 60 was already on at start-up (the
        // game sets TWO later), and switching back then ran the fixed step at 60 fps, twice as fast.
        static bool interval60 = false;
        const bool fps60 = Config::FrameRate == 60;
        if (fps60)
            GuestDevice::Store32(ctx.r3.u32 + 0x33C0, 1);
        else if (interval60)
            GuestDevice::Store32(ctx.r3.u32 + 0x33C0, 2);
        interval60 = fps60;
    }
    Renderer::OnPresent(ctx.r3.u32); EndFrame())

// DrawIndexed(device, primitive, baseVertex, startIndex, indexCount): recorded after the original,
// which computes and flushes the dirty register state (EDRAM mode, colour mask, depth control) the
// renderer reads from the device's register shadows.
PPC_FUNC_IMPL(__imp__sub_82549E20);
PPC_FUNC(sub_82549E20)
{
    const uint32_t device = ctx.r3.u32, primitive = ctx.r4.u32, startIndex = ctx.r6.u32, indexCount = ctx.r7.u32;
    const int32_t baseVertex = ctx.r5.s32;
    g_counters.draws++;
    __imp__sub_82549E20(ctx, base);
    Renderer::OnDrawIndexed(device, primitive, baseVertex, startIndex, indexCount);
}
DR_D3D_TAP(sub_825494B8, Renderer::OnEndVertices(ctx.r3.u32))                         // EndVertices
// Clear(device, count, rects, flags, color, z (f1), stencil)
DR_D3D_TAP(sub_824593C8, g_counters.clears++; Renderer::OnClear(ctx.r3.u32, ctx.r6.u32, ctx.r7.u32, float(ctx.f1.f64), ctx.r8.u32))
// Resolve(device, flags, srcRect, destTexture, destPoint, level, slice, clearColor, clearZ)
DR_D3D_TAP(sub_8245AA30, g_counters.resolves++; Renderer::OnResolve(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32, float(ctx.f1.f64),
    GuestDevice::Load32(ctx.r1.u32 + 92)))  // ClearStencil: the 10th argument, on the stack
DR_D3D_TAP(sub_825483F0, g_counters.tiles++)                                          // BeginTiling
DR_D3D_TAP(sub_82449860, g_counters.textureBinds++)                                   // SetTexture
DR_D3D_TAP(sub_82446EA0, g_counters.streamBinds++)                                    // SetStreamSource
DR_D3D_TAP(sub_82452838, g_counters.shaderBinds++; Shaders::OnBind(ctx.r4.u32))         // SetVertexShader (device, shader)
DR_D3D_TAP(sub_824524F8, g_counters.shaderBinds++; Shaders::OnBind(ctx.r4.u32))         // SetPixelShader (device, shader)
DR_D3D_TAP(sub_82447A00, g_counters.renderTargetBinds++)                              // SetRenderTarget

// Shader creation: D3D initialises each shader object in place (r3 = object, r4 = microcode); the
// game's XFB loader (sub_82770DC8) calls these for all 828 shaders. The hook identifies the shader
// before the original runs.
DR_D3D_TAP(sub_82452620, Shaders::OnVertexShaderInit(ctx.r3.u32, ctx.r4.u32))         // vertex shader init
DR_D3D_TAP(sub_82452498, Shaders::OnPixelShaderInit(ctx.r3.u32, ctx.r4.u32))          // pixel shader init

// BeginVertices(device, primitive, vertexCount, stride) returns the pointer the game writes the
// vertices to (inside the command buffer); EndVertices reads them from there.
PPC_FUNC_IMPL(__imp__sub_82549008);
PPC_FUNC(sub_82549008)
{
    const uint32_t device = ctx.r3.u32, primitive = ctx.r4.u32, count = ctx.r5.u32, stride = ctx.r6.u32;
    g_counters.vertexBatches++;
    __imp__sub_82549008(ctx, base);
    Renderer::OnBeginVertices(device, primitive, count, stride, ctx.r3.u32);
}

// BeginIndexedVertices(device, primitive, baseVertex, vertexCount, indexCount, indexFormat, stride,
// &indexData, &vertexData): the game writes both inline into the command buffer; the device keeps
// the vertex pointer at +0x3328 and the index pointer at +0x332C. Index format bit 2 = 32-bit.
PPC_FUNC_IMPL(__imp__sub_825494D8);
PPC_FUNC(sub_825494D8)
{
    const uint32_t device = ctx.r3.u32, primitive = ctx.r4.u32, vertexCount = ctx.r6.u32, indexCount = ctx.r7.u32;
    const int32_t baseVertex = ctx.r5.s32;
    const bool indices32 = (ctx.r8.u32 & 4) != 0;
    const uint32_t stride = ctx.r9.u32;
    g_counters.vertexBatches++;
    __imp__sub_825494D8(ctx, base);
    if (ctx.r3.u32 == 0)
        Renderer::OnBeginIndexedVertices(device, primitive, baseVertex, vertexCount, indexCount, indices32, stride,
            GuestDevice::Load32(device + 0x3328), GuestDevice::Load32(device + 0x332C));
    else
        Renderer::OnBeginVertices(device, 0, 0, 0, 0);
}

// CreateVertexDeclaration(elements) returns the (cached) declaration object.
PPC_FUNC_IMPL(__imp__sub_82452860);
PPC_FUNC(sub_82452860)
{
    const uint32_t elements = ctx.r3.u32;
    __imp__sub_82452860(ctx, base);
    Renderer::OnCreateVertexDeclaration(elements, ctx.r3.u32);
}
