#pragma once

// iPhone / iPad: everything that is not the game, in UIKit above the game's Metal layer (src/os/ios_overlay.mm),
// as in the user's Tunic port: the on-screen controller (Core Animation layers), the Settings panel (the gear, or a
// three-finger tap), the live log (a four-finger tap, and by itself while the game shows no picture) and the frame
// counter. They run on the main thread at display rate, whatever the game's frame rate; the game renders on its own
// thread (gpu/video.cpp).

struct SDL_Window;

namespace Overlay
{
    void Create(SDL_Window* window); // main thread, once the window exists
    void ToggleSettings();           // main thread: Start + Back on a controller, or F1 (ui/options_menu.cpp)
}
