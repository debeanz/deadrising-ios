// Mid-function hooks for running above 30 fps (docs/frame_rate.md). XenonRecomp calls them from
// inside the recompiled functions (config/DR.toml, [[midasm_hook]]). Every unit carries the frame's
// step at +16 (MT Framework's mDeltaTime, copied in by sub_82752FE0): 1.0 at 30 fps, 0.5 at 60.

#include <stdafx.h>
#include <cpu/ppc_context.h>
#include <gpu/guest_device.h>
#include <os/logger.h>

namespace
{
    double UnitStep(uint32_t unit)
    {
        const float step = GuestDevice::LoadFloat(unit + 16);
        return step > 0.0f && step < 1.0f ? double(step) : 1.0;
    }
}

// sub_821FF738, airborne: fall speed (+4208) += gravity (f0), once per frame.
void PlayerGravityMidAsmHook(PPCRegister& f0, PPCRegister& r31)
{
    f0.f64 *= UnitStep(r31.u32);
}

// sub_821FF738, airborne: height (+52) -= fall speed (f0), once per frame. The console's arc is
// v += g, y -= v per 30 fps frame; k steps of dt = 1/k reproduce it exactly when each moves by
// dt * (v + g * (1 - dt) / 2) (without the term the peak came out 6% higher at 60 fps).
// Gravity: 0x820DB570, maximum fall speed (no more gravity once reached): 0x820DB4FC.
void PlayerFallMidAsmHook(PPCRegister& f0, PPCRegister& r31)
{
    const double step = UnitStep(r31.u32);
    if (step >= 1.0)
        return;
    const double gravity = GuestDevice::LoadFloat(0x820DB570);
    const double maximum = GuestDevice::LoadFloat(0x820DB4FC);
    const double correction = f0.f64 < maximum ? gravity * (1.0 - step) * 0.5 : 0.0;
    f0.f64 = (f0.f64 + correction) * step;
}

// sub_822C4AC8, jump: position += facing x jump speed (f0, from +5604) once per frame.
void PlayerJumpMoveMidAsmHook(PPCRegister& f0, PPCRegister& r31)
{
    f0.f64 *= UnitStep(r31.u32);
}

// Enemy knocked back (sub_8236BA40, the "blown" action of the enemy vtables, motions 0x204-0x207;
// docs/frame_rate.md): every call the velocity (+3248) is multiplied by 0.9 while the contact flags
// (+3692) have bit 0 or 1 and by 0.2 while they have bit 2 or 3 (0x820DB5F0, 0x820DB4C0), then the
// position (+48) moves by the velocity (sub-state byte +21: 0 takes the velocity from +4544 first, 1-3
// fly and slide). Once per frame, so at 60 fps a zombie hit with a bat flew about twice as far
// (2026-11-25, enemy trace: 14.4 units added on every half step). Above 30 fps the friction F becomes
// F^step per update and the position moves by step x c x velocity, c = F^(1-step) (1 - F^step) /
// (step (1 - F)), so the updates of one 30 fps frame add up to the console's frame.
PPC_FUNC_IMPL(__imp__sub_8236BA40);
PPC_FUNC(sub_8236BA40)
{
    const uint32_t unit = ctx.r3.u32;
    const double step = UnitStep(unit);
    const uint8_t subState = GuestDevice::Load8(unit + 21);
    if (step >= 1.0 || subState > 3)
    {
        __imp__sub_8236BA40(ctx, base);
        return;
    }
    const uint32_t flags = GuestDevice::Load32(unit + 3692);
    float position[3], velocity[3];
    for (int i = 0; i < 3; i++)
    {
        position[i] = GuestDevice::LoadFloat(unit + 48 + i * 4);
        velocity[i] = GuestDevice::LoadFloat(unit + 3248 + i * 4);
    }

    __imp__sub_8236BA40(ctx, base);

    // Every sub-state adds the velocity once; anything else (a position set elsewhere in the call)
    // is left alone.
    float moved[3], newVelocity[3];
    for (int i = 0; i < 3; i++)
    {
        newVelocity[i] = GuestDevice::LoadFloat(unit + 3248 + i * 4);
        moved[i] = GuestDevice::LoadFloat(unit + 48 + i * 4) - position[i];
        if (std::fabs(moved[i] - newVelocity[i]) > 0.01f + std::fabs(newVelocity[i]) * 1e-4f)
            return;
    }
    double friction = 1.0;
    if (subState != 0) // sub-state 0 starts from +4544 after the friction
    {
        if ((flags & 3) != 0)
            friction *= GuestDevice::LoadFloat(0x820DB5F0);
        if ((flags & 0xC) != 0)
            friction *= GuestDevice::LoadFloat(0x820DB4C0);
    }
    const double perUpdate = friction < 1.0 ? std::pow(friction, step) : 1.0;
    const double scale = friction < 1.0 && friction > 0.0
        ? std::pow(friction, 1.0 - step) * (1.0 - perUpdate) / (step * (1.0 - friction)) * step
        : step;
    for (int i = 0; i < 3; i++)
    {
        const double v = friction < 1.0 ? velocity[i] * perUpdate : newVelocity[i];
        GuestDevice::StoreFloat(unit + 3248 + i * 4, float(v));
        GuestDevice::StoreFloat(unit + 48 + i * 4, float(position[i] + v * scale));
    }
}

// ---------------------------------------------------------------------------------------------
// UI at 30 Hz above 30 fps (frame_rate.h)
// ---------------------------------------------------------------------------------------------

#include "frame_rate.h"
#include "enemy_trace.h"
#include <user/config.h>
#include <mutex>
#include <unordered_map>

namespace
{
    float g_uiAccumulated = 0.0f;
    float g_uiStep = 1.0f;
    bool g_uiFrame = true;
    bool g_above30 = false;
    float g_step = 1.0f;
    uint32_t g_frameIndex = 0;

    // The units that draw menus, the HUD, messages and fades; they count frames. (Not uGametaskPrim: it
    // rebuilds the game tasks' draw list every move, so skipping it made the title menu flash; the tasks
    // themselves run at 30 Hz through sTaskManager below.)
    bool IsUiClass(const std::string& name)
    {
        // Debugging: DR_UI_KEEP=<class>,<class> moves those every frame (off the 30 Hz list).
        static const std::string keep = getenv("DR_UI_KEEP") ? std::string(",") + getenv("DR_UI_KEEP") + "," : std::string();
        if (!keep.empty() && keep.find("," + name + ",") != std::string::npos)
            return false;
        // Not uOption (the pause menu's Options page): it draws what its last move built and then
        // clears it, so at 30 Hz the whole page showed on every other frame only (2026-11-25, F12
        // capture). Its fades take the same time moved every frame (they run on uFade).
        static const char* const kClasses[] = { "uCockpit", "uCockpitModel", "uMessage", "uTimeMessage",
            "uSchedulerMessage", "uFade", "uPPDisp", "uLoadingEffect" };
        for (const char* c : kClasses)
            if (name == c)
                return true;
        return false;
    }
}

void FrameRate::OnStep(float step)
{
    g_step = step;
    g_frameIndex++;
    g_above30 = step > 0.0f && step < 0.99f;
    if (!g_above30)
    {
        g_uiFrame = true;
        g_uiStep = step;
        g_uiAccumulated = 0.0f;
        return;
    }
    // A UI frame once the steps add up to (nearly) one 30 fps frame: every other frame at 60 fps.
    g_uiAccumulated += step;
    g_uiFrame = g_uiAccumulated >= 1.0f - step * 0.5f;
    if (g_uiFrame)
    {
        g_uiStep = g_uiAccumulated;
        g_uiAccumulated = 0.0f;
    }
}

bool FrameRate::Above30() { return g_above30; }
bool FrameRate::UiFrame() { return g_uiFrame; }
float FrameRate::UiStep() { return g_uiStep; }
float FrameRate::Step() { return g_step; }
uint32_t FrameRate::FrameIndex() { return g_frameIndex; }

bool FrameRate::IsUiUnit(uint32_t unit)
{
    thread_local std::unordered_map<uint32_t, bool> byVtable; // unit moves run as jobs, possibly on several threads
    const uint32_t vtable = GuestDevice::Load32(unit);
    auto it = byVtable.find(vtable);
    if (it == byVtable.end())
        it = byVtable.emplace(vtable, IsUiClass(GuestDevice::ClassName(unit))).first;
    return it->second;
}

// sub_82752FE0 (the move pass), before a unit's move() (vtable +24): UI units move on UI frames
// only, with the steps since their last move. Returns true to skip the call.
bool UnitMoveSkipMidAsmHook(PPCRegister& r31)
{
    // Debugging: DR_UI_LOG=1 counts UI moves run and skipped, per class, every 300 frames.
    static const bool log = getenv("DR_UI_LOG") != nullptr;
    if (log)
    {
        static std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> counts;
        static uint32_t calls;
        if (FrameRate::IsUiUnit(r31.u32))
        {
            auto& c = counts[GuestDevice::ClassName(r31.u32)];
            (FrameRate::Above30() && !FrameRate::UiFrame() ? c.second : c.first)++;
        }
        if (++calls % 20000 == 0 || calls <= 3)
        {
            std::string text;
            for (auto& [name, c] : counts)
                text += fmt::format(" {} {}/{}", name, c.first, c.second);
            LOGFN("[ui] above30 {} moves run/skipped:{}", FrameRate::Above30(), text);
        }
    }
    if (!FrameRate::Above30() || !FrameRate::IsUiUnit(r31.u32))
        return false;
    if (!FrameRate::UiFrame())
        return true;
    GuestDevice::StoreFloat(r31.u32 + 16, FrameRate::UiStep());
    return false;
}

// Motion blur setting (Config::MotionBlur): uMotionBlurFilter's full-scene blur scale (+64, mFBScale)
// and stretch blur scale (+80, mSBScale; property table in sub_8270DF00, names at 0x820D53CC /
// 0x820D5404) are 0 while it is off; the game's values come back when it is turned on again.
static void MotionBlurAfterMove(uint32_t unit)
{
    thread_local std::unordered_map<uint32_t, bool> byVtable;
    const uint32_t vtable = GuestDevice::Load32(unit);
    auto it = byVtable.find(vtable);
    if (it == byVtable.end())
        it = byVtable.emplace(vtable, GuestDevice::ClassName(unit) == "uMotionBlurFilter").first;
    if (!it->second)
        return;
    static std::mutex mutex;
    static std::unordered_map<uint32_t, std::pair<float, float>> saved; // unit -> the game's scales
    std::lock_guard lock(mutex);
    if (!Config::MotionBlur)
    {
        const float fb = GuestDevice::LoadFloat(unit + 64), sb = GuestDevice::LoadFloat(unit + 80);
        if (fb != 0.0f || sb != 0.0f)
            saved[unit] = { fb, sb };
        GuestDevice::StoreFloat(unit + 64, 0.0f);
        GuestDevice::StoreFloat(unit + 80, 0.0f);
    }
    else if (auto s = saved.find(unit); s != saved.end())
    {
        GuestDevice::StoreFloat(unit + 64, s->second.first);
        GuestDevice::StoreFloat(unit + 80, s->second.second);
        saved.erase(s);
    }
}

// The move pass queues every unit as a job (unit, sub_8242DCA0) when its line's top flag bit is set,
// which all 32 lines have; sub_8242DCA0 tail-calls the unit's move() (vtable +24). UI units move on
// UI frames only (see UnitMoveSkipMidAsmHook, the direct-call path).
PPC_FUNC_IMPL(__imp__sub_8242DCA0);
PPC_FUNC(sub_8242DCA0)
{
    // Debugging: DR_UI_LOG=1 counts UI moves run / skipped per class (log every 20000 jobs).
    static const bool log = getenv("DR_UI_LOG") != nullptr;
    if (log)
    {
        static std::mutex mutex;
        static std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> counts;
        static uint32_t calls;
        std::lock_guard lock(mutex);
        if (FrameRate::IsUiUnit(ctx.r3.u32))
        {
            auto& c = counts[GuestDevice::ClassName(ctx.r3.u32)];
            (FrameRate::Above30() && !FrameRate::UiFrame() ? c.second : c.first)++;
        }
        if (++calls % 20000 == 0)
        {
            std::string text;
            for (auto& [name, c] : counts)
                text += fmt::format(" {} {}/{}", name, c.first, c.second);
            LOGFN("[ui] above30 {} moves run/skipped:{}", FrameRate::Above30(), text);
        }
    }
    if (FrameRate::Above30() && FrameRate::IsUiUnit(ctx.r3.u32))
    {
        if (!FrameRate::UiFrame())
            return;
        GuestDevice::StoreFloat(ctx.r3.u32 + 16, FrameRate::UiStep());
    }
    const uint32_t unit = ctx.r3.u32;
    EnemyTrace::BeforeMove(unit);
    __imp__sub_8242DCA0(ctx, base);
    EnemyTrace::AfterMove(unit);
    MotionBlurAfterMove(unit);
}

// sTaskManager::move (sMain + 0x10208, vtable 0x8208A4A0 slot 6, sub_8219A2A0), from the frame
// function: runs the current game task (cGametaskTitle, Logo, Demo, Game ...), whose menus count
// frames. Above 30 fps only the task's own move (vtable slot 24, at 0x8219A344, which the game also
// skips while paused) waits for UI frames; its slots 32 / 36 (they prepare what it draws) and the
// manager's bookkeeping run every frame. Skipping the whole manager every other frame left them out
// too: the Security Room marker's background was drawn twice as dark every other frame (a blink).
// DR_TASK_EVERY_FRAME=1 (debugging) runs the task's move every frame too.
bool TaskMoveSkipMidAsmHook(PPCRegister& r3)
{
    static uint32_t lastVtable = 0;
    static bool throttled = true;
    if (const uint32_t vtable = GuestDevice::Load32(r3.u32); vtable != lastVtable)
    {
        lastVtable = vtable;
        const std::string name = GuestDevice::ClassName(r3.u32);
        // cGametaskMain (gameplay, cutscenes, the date screen, the pause menu) moves every frame: it
        // queues world markers (the Security Room label's background) that are drawn every frame,
        // and at 30 Hz they were drawn twice on every other frame (a blink). Its speeds do not depend
        // on it (date screen, intro cutscene and pause menu measured the same both ways). The boot
        // and title tasks count frames in their menus and stay at 30 Hz.
        // Debugging: DR_TASK_EVERY_FRAME=1 (all tasks) or =<class>,<class> moves those every frame too.
        static const std::string every = getenv("DR_TASK_EVERY_FRAME") ? std::string(",") + getenv("DR_TASK_EVERY_FRAME") + "," : std::string();
        throttled = name != "cGametaskMain" && !(every == ",1," || every.find("," + name + ",") != std::string::npos);
        LOGFN("[task] current game task {} (vtable {:08X}), {}", name, vtable, throttled ? "30 Hz above 30 fps" : "every frame");
    }
    return throttled && FrameRate::Above30() && !FrameRate::UiFrame();
}

// ---------------------------------------------------------------------------------------------
// The game clock (docs/frame_rate.md)
// ---------------------------------------------------------------------------------------------

// sub_822DB980 keeps the 72-hour clock: game time at +336 in 30ths of a game second (+364..+367:
// day, hour, minute, second, derived from it at the start of each call), and when r4 is 0 it adds
// the rate (+332: 12, the game's 12x clock) to it and to the play-time counter at +0x829C/+0x8298,
// then checks the case deadlines. Once per frame: at 60 fps the clock ran twice as fast. Above 30
// fps the rate is scaled by the frame's step for the call (the remainder carried to the next
// frame, so a 60 fps clock adds 6 + 6 where the console adds 12) and restored after it.
// DR_CLOCK_FIX=0 turns it off; "[clock]" logs the clock's speed every 20 s.
PPC_FUNC_IMPL(__imp__sub_822DB980);
PPC_FUNC(sub_822DB980)
{
    const uint32_t clock = ctx.r3.u32;
    const bool advances = ctx.r4.u32 == 0;
    static const bool fix = [] {
        const char* value = getenv("DR_CLOCK_FIX");
        return value == nullptr || strcmp(value, "0") != 0;
    }();
    static double carry = 0.0;
    const uint32_t before = GuestDevice::Load32(clock + 336);
    const uint32_t rate = GuestDevice::Load32(clock + 332);
    bool scaled = false;
    if (fix && advances && FrameRate::Above30() && rate > 0 && rate < 100000)
    {
        const double amount = rate * double(FrameRate::Step()) + carry;
        const uint32_t whole = uint32_t(amount);
        carry = amount - whole;
        GuestDevice::Store32(clock + 332, whole);
        scaled = true;
    }
    else if (!FrameRate::Above30())
        carry = 0.0;
    __imp__sub_822DB980(ctx, base);
    if (scaled)
        GuestDevice::Store32(clock + 332, rate);

    // Its speed, in game seconds per real second (12 on the console).
    static auto start = std::chrono::steady_clock::now();
    static uint32_t startTime = before, calls = 0, advancing = 0, frames = 0, lastFrame = UINT32_MAX;
    const uint32_t after = GuestDevice::Load32(clock + 336);
    calls++;
    advancing += after != before;
    if (FrameRate::FrameIndex() != lastFrame)
    {
        lastFrame = FrameRate::FrameIndex();
        frames++;
    }
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - start).count();
    if (seconds >= 20.0)
    {
        LOGFN("[clock] day {} {:02}:{:02}:{:02}: {:.1f} game seconds per second over {:.0f} s ({} calls, {} advanced, {} frames; rate {}, {})",
            GuestDevice::Load8(clock + 364), GuestDevice::Load8(clock + 365), GuestDevice::Load8(clock + 366), GuestDevice::Load8(clock + 367),
            (after - startTime) / 30.0 / seconds, seconds, calls, advancing, frames, rate, fix ? "scaled above 30 fps" : "not scaled");
        start = now;
        startTime = after;
        calls = advancing = frames = 0;
    }
}
