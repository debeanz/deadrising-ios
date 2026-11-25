#pragma once

// Above 30 fps (docs/frame_rate.md): the game advances by MT Framework's step (sMain mDeltaTime,
// 1.0 = one 30 fps frame), but its UI counts frames. The UI (menus, HUD, messages, fades) and the
// controller input it reads therefore run at 30 Hz: on "UI frames", every frame at 30 fps and every
// other frame at 60.

#include <cstdint>

namespace FrameRate
{
    void OnStep(float step); // sMain's step for this frame (sub_82734808), before anything moves
    bool Above30();          // this frame's step is below 1 (more than 30 frames a second)
    bool UiFrame();          // UI units and input update this frame
    float UiStep();          // their step this frame (the steps since their last update)
    bool IsUiUnit(uint32_t unit);
    float Step();            // sMain's step for this frame
    uint32_t FrameIndex();   // counts frames (OnStep calls)
}
