#pragma once

// On-screen controls for phones and tablets: a virtual Xbox controller drawn over the game
// (renderer overlay) and driven by SDL finger events.
//
// Layout (landscape): left stick bottom left, D-pad next to it, A / B / X / Y bottom right,
// LB / LT top left, RB / RT top right, Back / Start top centre; dragging anywhere else on the
// right half moves the camera (right stick).
//
// On by default on iOS while no game controller is connected; on the desktop DR_TOUCH=1 turns them
// on and makes the mouse act as a finger (for testing).

#include <cstdint>
#include <vector>

union SDL_Event;

namespace TouchControls
{
    // Forget every finger and release the virtual pad (ui/options_menu.cpp: three- and four-finger taps).
    void CancelAll();
    struct Circle
    {
        float x, y, radius; // pixels of the window / swap chain
        bool filled;        // filled disc, or a ring
        float color[4];     // straight alpha
    };

    void Init();
    // Returns true if the event was a touch the controls used.
    bool HandleEvent(const SDL_Event& event);
    // Shown at all (iOS without a controller, or DR_TOUCH on the desktop).
    bool Visible(bool controllerConnected);
    // The controls to draw on a width x height image.
    void GetOverlay(uint32_t width, uint32_t height, std::vector<Circle>& circles);
}
