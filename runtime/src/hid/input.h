#pragma once

// Controller input for the game's XamInputGetState: SDL game controllers (Xbox, PlayStation, MFi)
// plus a keyboard mapping, polled on the video thread (which owns SDL's event loop).
//
// Keyboard: WASD left stick, IJKL right stick, arrow keys D-pad, Enter Start, Backspace / Tab Back,
// Space A, E B, Q X, R Y, 1 LB, 3 RB, Left Shift LT, F RT, left / right stick
// click = Left Ctrl / Right Ctrl.
//
// DR_AUTO_INPUT="<frame>:<button>[,<frame>:<button>...]" presses buttons on the given video frames
// (for unattended tests), e.g. "700:start,760:a,820:a". Buttons: a b x y start back lb rb lt rt
// up down left right.

#include <cstdint>

union SDL_Event;

namespace Input
{
    struct Pad
    {
        uint16_t buttons = 0; // XAMINPUT_GAMEPAD_* bits
        uint8_t leftTrigger = 0, rightTrigger = 0;
        int16_t thumbLX = 0, thumbLY = 0, thumbRX = 0, thumbRY = 0;
    };

    void Init();                         // video thread, after SDL_Init
    void HandleEvent(const SDL_Event& event);
    void Update(uint32_t videoFrame);    // video thread, once per loop
    Pad GetPad(uint32_t& packetNumber);  // any thread
    bool ControllerConnected();          // video thread
    void SetTouchPad(const Pad& pad);    // touch controls (iOS) merged into the state
    void SetVibration(uint16_t left, uint16_t right); // any thread (XamInputSetState): controller rumble
    Pad GetRawPad();                     // video thread: what the player holds, also while blocked
    void SetBlocked(bool blocked);       // the game sees nothing pressed (the options menu is open)
}
