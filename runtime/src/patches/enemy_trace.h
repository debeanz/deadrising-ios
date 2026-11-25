#pragma once

// Enemy trace (docs/frame_rate.md, 2026-11-25): zombies hit with a bat at 60 fps flew further than
// at 30 and got straight back up. Logs each enemy's motions near Frank ("[em]": how many steps each
// ran, its frames, how far the enemy moved in it) and finds the code that moves an enemy that is
// knocked back (a write watch on its position: "[em watch]"). DR_ENEMY_TRACE=0 turns it off.

#include <cstdint>

namespace EnemyTrace
{
    void BeforeMove(uint32_t unit);     // sub_8242DCA0, before the unit's move()
    void AfterMove(uint32_t unit);      // after it
    void OnHoldReleased(uint32_t model); // the motion wrapper released a hold at end - 1.0

    // A zombie's damage function ran for a hit slot ("[em hit]"): enemy_trace.cpp, sub_82362F38.
    struct Hit
    {
        uint32_t slot = 0, attacker = 0, extra = 0, statesBefore = 0;
        uint64_t flags = 0;
        int32_t damage = 0, hpBefore = 0;
    };
    void OnDamage(uint32_t unit, const Hit& hit);
}
