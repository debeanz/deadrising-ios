#pragma once

#include <cstdint>

// Hang watchdog (2026-11-25: on the iPhone the game stopped after its first frame, with nothing in
// the log). When no frame has been presented for 15 s it logs, for every game (guest) thread, the
// last kernel / XAM import it called and from where (lr), how many imports it made in the last
// 15 s (still calling: a loop; none: blocked in that import or spinning in game code) and, on Apple
// platforms, its call stack as recompiled functions (sub_XXXXXXXX). "[watchdog]" lines; at most
// 3 reports per stall. DR_WATCHDOG=0 turns it off.
namespace HangWatchdog
{
    void Start();                                   // once, before the game starts
    void OnPresent();                               // the renderer presented a frame
    void RegisterGuestThread(const char* name);     // on each guest thread as it starts
    // Apple: a stack of its own for crash reports on this thread (RegisterGuestThread does it for game
    // threads); Start installs the crash handler ("[crash]" lines, DR_CRASH_HANDLER=0 off).
    void PrepareThread();
    void NoteImport(const char* name, uint32_t lr); // every import call (kernel/imports_dr.cpp)
}
