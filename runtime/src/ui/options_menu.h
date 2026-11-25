#pragma once

// In-game options (settings in user/config.h), drawn over the game by the renderer.
//
// Open / close: F1 on a keyboard (anywhere), Start + Back (Select) on a controller. While it is open
// the game sees no input. Up / down picks a setting, left / right, A or Start changes it; B, Back,
// Escape, F1 or Start + Back again closes it (and saves recomp_config.toml).

#include <cstdint>
#include <string>

union SDL_Event;

namespace OptionsMenu
{
    bool HandleEvent(const SDL_Event& event); // video thread; true if the menu used the event
    void Update();                            // video thread, once per loop (after Input::Update)
    bool Visible();                           // the menu is open or the frame counter is on (the overlay is drawn)
    void SetFps(double fps);                  // video thread: frames per second of the last half second
    // Input::Update: the buttons the game gets (Start + Back opens the menu without reaching it).
    uint16_t FilterButtons(uint16_t raw);
    // The menu image (RGBA8, straight alpha, width x height); version changes whenever it does.
    const uint8_t* Image(uint32_t& width, uint32_t& height, uint32_t& version);
    // Video thread: the status panel shown while the game gives no picture (headline and the latest
    // log lines); an empty headline hides it.
    void SetStatus(const std::string& headline);
}
