// Enemy trace (enemy_trace.h). Units move as parallel jobs (sub_8242DCA0), so the trace's state is
// under one mutex; only enemies (class uEm*) and Frank take it.

#include <stdafx.h>
#include "enemy_trace.h"
#include <cpu/ppc_context.h>
#include <gpu/guest_device.h>
#include <os/logger.h>
#include <kernel/memory.h>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <algorithm>
#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
    // Unit layout (MT Framework uCoord / uModel): position at +48 (x), +52 (height), +56 (z); the
    // step at +16; motion layers of 272 bytes from +240 (+4 motion id, +8 status, +10 mode, +20
    // frame, +24 end, +28 speed).
    struct Motion
    {
        uint16_t id = 0xFFFF, mode = 0, status = 0;
        float frame = 0, end = 0, speed = 0;
    };

    Motion ReadMotion(uint32_t unit)
    {
        const uint32_t layer = unit + 240;
        Motion m;
        m.id = GuestDevice::Load16(layer + 4);
        m.status = GuestDevice::Load16(layer + 8);
        m.mode = GuestDevice::Load16(layer + 10);
        m.frame = GuestDevice::LoadFloat(layer + 20);
        m.end = GuestDevice::LoadFloat(layer + 24);
        m.speed = GuestDevice::LoadFloat(layer + 28);
        return m;
    }

    struct EnemyState
    {
        Motion motion;     // layer 0 as the current motion started
        uint32_t steps = 0;
        float lastFrame = 0;
        double distance = 0, rise = 0, fastest = 0, time = 0; // time: sum of steps (30 fps frames)
        uint32_t releases = 0;
        uint32_t watchMoves = 0; // moves left with the write watch on
        uint32_t detail = 0;     // updates left with the detailed log (knocked back: motions 0x204-0x207)
        uint64_t lastKey = 0;
        uint32_t sinceLine = 0;
    };

    std::mutex g_mutex;
    std::unordered_map<uint32_t, int8_t> g_kinds; // vtable -> 1 enemy, 2 Frank, 0 other
    std::unordered_map<uint32_t, EnemyState> g_enemies;
    float g_player[3] = {};
    bool g_havePlayer = false;
    uint32_t g_lines = 0, g_watches = 0, g_detailLines = 0, g_detailed = 0;
    constexpr uint32_t kMaxDetailLines = 12000, kMaxDetailed = 40;
    constexpr uint32_t kMaxLines = 8000, kMaxWatches = 24;
    constexpr float kNear = 1500.0f;     // logged within this distance of Frank (about 15 m)
    constexpr float kHit = 400.0f;       // the watch only for enemies this close to Frank (the ones he hits)
    constexpr float kFast = 15.0f;       // a move faster than this per 30 fps frame arms the watch (Frank walks at up to ~6)

    thread_local uint32_t t_unit = 0;
    thread_local float t_before[3];
    thread_local float t_frameBefore = 0.0f;
    thread_local uint16_t t_motionBefore = 0xFFFF;

    // Frank's motions ("[pl]", 2026-11-25: the user saw climbing play faster at 60 fps): for each
    // motion of his base layer, how many frames it played per 30 fps frame and how many updates
    // advanced it by something else than speed x step (game code setting the frame), with how far
    // he moved; one line when it ends.
    struct PlayerState
    {
        Motion motion;
        uint32_t steps = 0, offAdvance = 0;
        double time = 0, advanced = 0, offExtra = 0, distance = 0, rise = 0;
        float minSpeed = 1e9f, maxSpeed = -1e9f;
    } g_playerState;
    uint32_t g_playerLines = 0;
    constexpr uint32_t kMaxPlayerLines = 4000;

    bool Enabled()
    {
        static const bool enabled = [] {
            // Off unless asked for (2026-11-25): it takes one lock on every traced unit's move, and the game moves its
            // units on 5 threads at once; on the iPhone's roof and in the mall they queued on it.
            const char* value = getenv("DR_ENEMY_TRACE");
            return value != nullptr && strcmp(value, "0") != 0;
        }();
        return enabled;
    }

    // The position write watch (it found sub_8236BA40, the knockback): DR_ENEMY_WATCH=1.
    bool WatchEnabled()
    {
        static const bool enabled = getenv("DR_ENEMY_WATCH") != nullptr;
        return enabled;
    }

    int8_t Kind(uint32_t unit)
    {
        if (unit < 0x40000000 || unit >= 0xC0000000)
            return 0;
        const uint32_t vtable = GuestDevice::Load32(unit);
        {
            std::lock_guard lock(g_mutex);
            if (auto it = g_kinds.find(vtable); it != g_kinds.end())
                return it->second;
        }
        // DR_ENEMY_TRACE=<class prefix> traces other classes (uPlayer: Frank, to test the trace).
        static const std::string prefix = [] {
            const char* value = getenv("DR_ENEMY_TRACE");
            return value != nullptr && strcmp(value, "1") != 0 ? std::string(value) : std::string("uEm");
        }();
        const std::string name = GuestDevice::ClassName(unit);
        const int8_t kind = name.rfind(prefix, 0) == 0 ? 1 : name == "uPlayer" ? 2 : 0;
        std::lock_guard lock(g_mutex);
        g_kinds[vtable] = kind;
        return kind;
    }

    void ReadPosition(uint32_t unit, float* out)
    {
        out[0] = GuestDevice::LoadFloat(unit + 48);
        out[1] = GuestDevice::LoadFloat(unit + 52);
        out[2] = GuestDevice::LoadFloat(unit + 56);
    }

#ifdef _WIN32
    // Write watch on enemy positions: the page is made read-only; a write faults, the handler notes
    // what is written there, runs the instruction single-stepped and protects the page again, then
    // logs the value and the recompiled function that wrote it.
    namespace Watch
    {
        std::mutex mutex;
        std::vector<uint32_t> addresses;
        std::vector<uint8_t*> pages;
        std::vector<uint8_t*> everWatched; // pages watched before: a write that faulted just before Disarm is retried
        std::vector<std::pair<uintptr_t, uint32_t>> functions; // host start, guest address
        thread_local uint8_t* t_page = nullptr;
        thread_local uint32_t t_address = 0, t_old = 0, t_function = 0, t_lr = 0;
        std::unordered_map<uint64_t, uint32_t> logged; // (function, lr) -> times
        bool installed = false;

        uint32_t GuestFunction(uintptr_t host)
        {
            auto it = std::upper_bound(functions.begin(), functions.end(), std::make_pair(host, UINT32_MAX));
            return it == functions.begin() ? 0 : std::prev(it)->second;
        }

        LONG CALLBACK Handler(EXCEPTION_POINTERS* info)
        {
            const DWORD code = info->ExceptionRecord->ExceptionCode;
            if (code == EXCEPTION_SINGLE_STEP && t_page != nullptr)
            {
                DWORD old;
                {
                    std::lock_guard lock(mutex);
                    if (std::find(pages.begin(), pages.end(), t_page) != pages.end())
                        VirtualProtect(t_page, 4096, PAGE_READONLY, &old);
                }
                t_page = nullptr;
                if (t_address != 0)
                {
                    const uint32_t now = GuestDevice::Load32(t_address);
                    float before, after;
                    memcpy(&before, &t_old, 4);
                    memcpy(&after, &now, 4);
                    std::lock_guard lock(mutex);
                    uint32_t& times = logged[(uint64_t(t_function) << 32) | t_lr];
                    if (times++ < 6)
                        LOGFN("[em watch] {:08X} {:.2f} -> {:.2f} ({:+.2f}) by sub_{:08X}, lr {:08X}", t_address, before, after, after - before, t_function, t_lr);
                    t_address = 0;
                }
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (code != EXCEPTION_ACCESS_VIOLATION || info->ExceptionRecord->ExceptionInformation[0] != 1)
                return EXCEPTION_CONTINUE_SEARCH;
            auto* fault = reinterpret_cast<uint8_t*>(info->ExceptionRecord->ExceptionInformation[1]);
            uint8_t* page = reinterpret_cast<uint8_t*>(uintptr_t(fault) & ~uintptr_t(4095));
            {
                std::lock_guard lock(mutex);
                if (std::find(pages.begin(), pages.end(), page) == pages.end())
                {
                    // Disarm made the page writable while this write was faulting (2026-11-25: that
                    // write went on to the crash handler): run it again.
                    if (std::find(everWatched.begin(), everWatched.end(), page) == everWatched.end())
                        return EXCEPTION_CONTINUE_SEARCH;
                    MEMORY_BASIC_INFORMATION mbi{};
                    if (VirtualQuery(page, &mbi, sizeof(mbi)) != 0 && mbi.Protect != PAGE_READWRITE)
                    {
                        DWORD old;
                        VirtualProtect(page, 4096, PAGE_READWRITE, &old);
                    }
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
                DWORD old;
                VirtualProtect(page, 4096, PAGE_READWRITE, &old);
                const uint32_t guest = uint32_t(fault - g_memory.base);
                t_address = 0;
                for (uint32_t address : addresses)
                    if (guest >= address && guest < address + 4)
                    {
                        t_address = address;
                        t_old = GuestDevice::Load32(address);
                        t_function = GuestFunction(uintptr_t(info->ContextRecord->Rip));
                        auto* ppc = GetPPCContext();
                        t_lr = ppc ? uint32_t(ppc->lr) : 0;
                    }
            }
            t_page = page;
            info->ContextRecord->EFlags |= 0x100;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        void Arm(uint32_t unit)
        {
            std::lock_guard lock(mutex);
            if (!installed)
            {
                for (size_t i = 0; PPCFuncMappings[i].host != nullptr; i++)
                    functions.emplace_back(uintptr_t(PPCFuncMappings[i].host), uint32_t(PPCFuncMappings[i].guest));
                std::sort(functions.begin(), functions.end());
                AddVectoredExceptionHandler(1, Handler);
                installed = true;
            }
            addresses = { unit + 48, unit + 52, unit + 56 };
            logged.clear();
            uint8_t* page = reinterpret_cast<uint8_t*>(uintptr_t(g_memory.Translate(unit + 48)) & ~uintptr_t(4095));
            pages = { page };
            if (std::find(everWatched.begin(), everWatched.end(), page) == everWatched.end())
                everWatched.push_back(page);
            DWORD old;
            VirtualProtect(page, 4096, PAGE_READONLY, &old);
        }

        void Disarm()
        {
            std::lock_guard lock(mutex);
            for (uint8_t* page : pages)
            {
                DWORD old;
                VirtualProtect(page, 4096, PAGE_READWRITE, &old);
            }
            pages.clear();
            addresses.clear();
        }
    }
#endif

    std::string Describe(const Motion& m)
    {
        return m.id == 0xFFFF ? std::string("none")
            : fmt::format("{:04X} (end {:.0f}, speed {:.2f}, mode {:02X}, status {:02X})", m.id, m.end, m.speed, m.mode, m.status);
    }
}

void EnemyTrace::BeforeMove(uint32_t unit)
{
    t_unit = 0;
    if (!Enabled() || Kind(unit) == 0)
        return;
    t_unit = unit;
    ReadPosition(unit, t_before);
    t_frameBefore = GuestDevice::LoadFloat(unit + 240 + 20);
    t_motionBefore = GuestDevice::Load16(unit + 240 + 4);
}

void EnemyTrace::AfterMove(uint32_t unit)
{
    if (t_unit != unit || unit == 0)
        return;
    t_unit = 0;
    float now[3];
    ReadPosition(unit, now);
    const int8_t kind = Kind(unit);
    std::lock_guard lock(g_mutex);
    if (kind == 2)
    {
        memcpy(g_player, now, sizeof(now));
        g_havePlayer = true;
        const float step = GuestDevice::LoadFloat(unit + 16);
        const Motion motion = ReadMotion(unit);
        PlayerState& p = g_playerState;
        if (motion.id != p.motion.id)
        {
            if (p.steps >= 2 && p.motion.id != 0xFFFF && g_playerLines < kMaxPlayerLines)
            {
                g_playerLines++;
                LOGFN("[pl] step {:.2f}: motion {:04X} (end {:.0f}, speed {:.2f}-{:.2f}) ran {} steps ({:.1f} frames of 30 fps), advanced {:.1f} frames "
                      "({:.2f} per 30 fps frame), {} updates off speed x step ({:+.1f} frames), moved {:.1f}, height {:+.1f}; now {:04X}",
                    step, p.motion.id, p.motion.end, p.minSpeed, p.maxSpeed, p.steps, p.time, p.advanced, p.time > 0 ? p.advanced / p.time : 0.0,
                    p.offAdvance, p.offExtra, p.distance, p.rise, motion.id);
            }
            p = PlayerState{};
            p.motion = motion;
        }
        else if (t_motionBefore == motion.id)
        {
            // This update's advance (a wrap or a jump back is not counted).
            const float advance = motion.frame - t_frameBefore;
            const float expected = (motion.mode & 0x10) != 0 ? 0.0f : motion.speed * step;
            if (advance >= 0.0f)
            {
                p.advanced += advance;
                if (std::fabs(advance - expected) > 0.01f && motion.frame < motion.end - 0.01f)
                {
                    p.offAdvance++;
                    p.offExtra += advance - expected;
                }
            }
        }
        p.steps++;
        p.time += step;
        p.minSpeed = std::min(p.minSpeed, motion.speed);
        p.maxSpeed = std::max(p.maxSpeed, motion.speed);
        p.distance += std::hypot(now[0] - t_before[0], now[2] - t_before[2]);
        p.rise += now[1] - t_before[1];
        return;
    }

    const float step = GuestDevice::LoadFloat(unit + 16);
    const double dx = now[0] - t_before[0], dz = now[2] - t_before[2];
    const double moved = std::sqrt(dx * dx + dz * dz);
    const double toPlayer = g_havePlayer ? std::hypot(now[0] - g_player[0], now[2] - g_player[2]) : 0.0;
    const Motion motion = ReadMotion(unit);

    auto [it, added] = g_enemies.try_emplace(unit);
    EnemyState& s = it->second;
    if (added || motion.id != s.motion.id)
    {
        if (!added && toPlayer < kNear && g_lines < kMaxLines && s.steps > 0)
        {
            g_lines++;
            LOGFN("[em] {:08X} {} step {:.2f}: motion {} ran {} steps ({:.1f} frames of 30 fps), frames {:.1f} -> {:.1f}, moved {:.1f} "
                  "(fastest {:.1f} per 30 fps frame, height {:+.1f}), holds released {}; now {} at frame {:.1f}, {:.0f} from Frank",
                unit, GuestDevice::ClassName(unit), step, Describe(s.motion), s.steps, s.time, s.motion.frame, s.lastFrame, s.distance,
                s.fastest, s.rise, s.releases, Describe(motion), motion.frame, toPlayer);
        }
        const EnemyState kept = s;
        s = EnemyState{};
        s.motion = motion;
        s.watchMoves = kept.watchMoves;
        s.detail = kept.detail;
        s.lastKey = kept.lastKey;
    }
    // Knocked back (the blown action's motions): every change of its state for 300 updates.
    if (motion.id >= 0x204 && motion.id <= 0x207 && s.detail == 0 && toPlayer < kNear && g_detailed < kMaxDetailed)
    {
        g_detailed++;
        s.detail = 300;
    }
    if (s.detail > 0)
    {
        s.detail--;
        const uint32_t states = GuestDevice::Load32(unit + 20); // bytes +20..+23 (action, sub-state ...)
        const uint32_t flags = GuestDevice::Load32(unit + 3692);
        const uint16_t layer1 = GuestDevice::Load32(unit + 224) > 1 ? GuestDevice::Load16(unit + 240 + 272 + 4) : 0xFFFF;
        const uint64_t key = (uint64_t(states) << 32) ^ (uint64_t(flags) << 16) ^ (uint64_t(motion.id) << 8) ^ (motion.status & 1) ^
            (uint64_t(motion.mode) << 1) ^ (uint64_t(layer1) << 48);
        if ((key != s.lastKey || ++s.sinceLine >= 10) && g_detailLines < kMaxDetailLines)
        {
            g_detailLines++;
            s.lastKey = key;
            s.sinceLine = 0;
            LOGFN("[em detail] {:08X} step {:.2f}: states {:08X} flags {:X} motion {:04X} frame {:.1f}/{:.0f} mode {:02X} status {:02X} "
                  "layer1 {:04X} at {:.1f},{:.1f},{:.1f} moved {:.1f} velocity {:.2f},{:.2f},{:.2f} hp {}/{} events {:X}",
                unit, step, states, flags, motion.id, motion.frame, motion.end, motion.mode, motion.status, layer1, now[0], now[1], now[2], moved,
                GuestDevice::LoadFloat(unit + 3248), GuestDevice::LoadFloat(unit + 3252), GuestDevice::LoadFloat(unit + 3256),
                int32_t(GuestDevice::Load32(unit + 3876)), int32_t(GuestDevice::Load32(unit + 3872)), GuestDevice::Load32(unit + 424));
        }
    }

    s.steps++;
    s.time += step;
    s.lastFrame = motion.frame;
    s.distance += moved;
    s.rise += now[1] - t_before[1];
    if (step > 0.0f)
        s.fastest = std::max(s.fastest, moved / step);

#ifdef _WIN32
    if (s.watchMoves > 0 && --s.watchMoves == 0)
    {
        Watch::Disarm();
        LOGFN("[em watch] {:08X} done", unit);
    }
    else if (WatchEnabled() && step > 0.0f && moved / step > kFast && toPlayer < kHit && g_watches < kMaxWatches && s.watchMoves == 0)
    {
        bool another = false;
        for (auto& [u, e] : g_enemies)
            another |= e.watchMoves > 0;
        if (!another)
        {
            g_watches++;
            s.watchMoves = 12;
            LOGFN("[em watch] {:08X} {} moved {:.1f} in one step of {:.2f} (motion {} at frame {:.1f}): watching its position for 12 moves",
                unit, GuestDevice::ClassName(unit), moved, step, Describe(motion), motion.frame);
            Watch::Arm(unit);
        }
    }
#endif

    // Units that are gone (their memory reused): dropped now and then.
    if (g_enemies.size() > 512)
        g_enemies.clear();
}

// The zombie's damage function (uEm00 vtable slot 34; the base character's is sub_82212908) for
// hit slot r4 (0-7) of the unit's collision at +2772: when the slot's status (+28) is 2 its hit info
// (+32) carries flags (+96, 64-bit; 0x780000000 = a hit to apply), the attacker (+108) and the
// damage (+120); HP (mVitalNew, +3876; mVitalMax +3872) -= damage, and at 0 the zombie dies
// (sub_82364B98). 2026-11-25: every zombie hit with the bat at 30 fps died and lay down (action 7),
// every one hit at 60 fps got straight back up (action 3), so this logs each hit's damage.
PPC_FUNC_IMPL(__imp__sub_82362F38);
PPC_FUNC(sub_82362F38)
{
    const uint32_t unit = ctx.r3.u32, slot = ctx.r4.u32;
    if (!Enabled())
    {
        __imp__sub_82362F38(ctx, base);
        return;
    }
    uint32_t info = 0;
    if (slot < 8)
    {
        const uint32_t hit = GuestDevice::Load32(unit + 2772 + (slot + 1) * 4);
        if (hit != 0 && GuestDevice::Load32(hit + 28) == 2)
            info = GuestDevice::Load32(hit + 32);
    }
    EnemyTrace::Hit h;
    h.slot = slot;
    h.hpBefore = int32_t(GuestDevice::Load32(unit + 3876));
    h.statesBefore = GuestDevice::Load32(unit + 20);
    if (info != 0)
    {
        h.flags = (uint64_t(GuestDevice::Load32(info + 96)) << 32) | GuestDevice::Load32(info + 100);
        h.attacker = GuestDevice::Load32(info + 108);
        h.damage = int32_t(GuestDevice::Load32(info + 120));
        h.extra = GuestDevice::Load32(info + 124);
    }
    __imp__sub_82362F38(ctx, base);
    if (info != 0 || int32_t(GuestDevice::Load32(unit + 3876)) != h.hpBefore)
        EnemyTrace::OnDamage(unit, h);
}

void EnemyTrace::OnDamage(uint32_t unit, const Hit& h)
{
    const int32_t hp = int32_t(GuestDevice::Load32(unit + 3876));
    if ((h.flags & 0x780000000ull) == 0 && hp == h.hpBefore)
        return;
    const float step = GuestDevice::LoadFloat(unit + 16);
    std::string attacker = "none";
    if (h.attacker >= 0x40000000 && h.attacker < 0xC0000000)
    {
        const Motion m = ReadMotion(h.attacker);
        attacker = fmt::format("{:08X} {} motion {:04X} frame {:.1f}/{:.0f} step {:.2f}", h.attacker, GuestDevice::ClassName(h.attacker), m.id, m.frame,
            m.end, GuestDevice::LoadFloat(h.attacker + 16));
    }
    std::lock_guard lock(g_mutex);
    static uint32_t lines = 0;
    if (lines++ < 3000)
        LOGFN("[em hit] {:08X} {} step {:.2f}: slot {} damage {} (+124 {:08X}, flags {:016X}), hp {} -> {} of {}, states {:08X} -> {:08X}, "
              "+4746 {}; attacker {}",
            unit, GuestDevice::ClassName(unit), step, h.slot, h.damage, h.extra, h.flags, h.hpBefore, hp, int32_t(GuestDevice::Load32(unit + 3872)),
            h.statesBefore, GuestDevice::Load32(unit + 20), GuestDevice::Load8(unit + 4746), attacker);
    // Follow the zombie that was hit (its states, motions and HP) for 300 updates.
    EnemyState& s = g_enemies[unit];
    if (s.detail == 0 && g_detailed < kMaxDetailed)
    {
        g_detailed++;
        s.detail = 300;
    }
}

void EnemyTrace::OnHoldReleased(uint32_t model)
{
    if (!Enabled())
        return;
    std::lock_guard lock(g_mutex);
    if (auto it = g_enemies.find(model); it != g_enemies.end())
        it->second.releases++;
}
