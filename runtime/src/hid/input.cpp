// See input.h.

#include <stdafx.h>
#include "input.h"
#include "touch_controls.h"
#include <ui/options_menu.h>

#include <SDL.h>
#include <os/logger.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    enum : uint16_t
    {
        DPAD_UP = 0x0001, DPAD_DOWN = 0x0002, DPAD_LEFT = 0x0004, DPAD_RIGHT = 0x0008,
        START = 0x0010, BACK = 0x0020, LEFT_THUMB = 0x0040, RIGHT_THUMB = 0x0080,
        LEFT_SHOULDER = 0x0100, RIGHT_SHOULDER = 0x0200, A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000,
    };

    std::mutex g_mutex;
    Input::Pad g_pad, g_touchPad;
    Input::Pad g_rawPad;          // what the player holds (the options menu reads it)
    std::atomic<bool> g_blocked{ false }; // the game sees nothing pressed (options menu open)
    uint32_t g_packet = 0;
    std::vector<SDL_GameController*> g_controllers;
    std::atomic<uint32_t> g_vibration{ 0 }; // left motor << 16 | right motor (XamInputSetState)

    struct AutoPress { uint32_t frame; uint16_t buttons; bool leftTrigger, rightTrigger; uint32_t duration = 6; int16_t lx = 0, ly = 0, rx = 0, ry = 0; };
    std::vector<AutoPress> g_autoPresses;

    int16_t Axis(SDL_GameController* controller, SDL_GameControllerAxis axis, bool invert)
    {
        int32_t value = SDL_GameControllerGetAxis(controller, axis);
        if (invert)
            value = -value - 1; // SDL: +Y is down; XInput: +Y is up
        return int16_t(std::clamp(value, -32768, 32767));
    }

    void ParseAutoInput()
    {
        const char* text = getenv("DR_AUTO_INPUT");
        if (text == nullptr)
            return;
        std::string spec(text);
        size_t start = 0;
        while (start < spec.size())
        {
            size_t end = spec.find(',', start);
            if (end == std::string::npos)
                end = spec.size();
            const std::string item = spec.substr(start, end - start);
            const size_t colon = item.find(':');
            if (colon != std::string::npos)
            {
                AutoPress press{ uint32_t(strtoul(item.c_str(), nullptr, 10)), 0, false, false };
                // "frame:name" or "frame:name*frames" (held that many video frames, default 6).
                std::string name = item.substr(colon + 1);
                if (const size_t star = name.find('*'); star != std::string::npos)
                {
                    press.duration = uint32_t(strtoul(name.c_str() + star + 1, nullptr, 10));
                    name.resize(star);
                }
                if (name == "lup") press.ly = 32767;
                else if (name == "ldown") press.ly = -32768;
                else if (name == "lleft") press.lx = -32768;
                else if (name == "lright") press.lx = 32767;
                // Diagonals (45 degrees): lupleft, lupright, ldownleft, ldownright.
                else if (name == "lupleft") { press.lx = -23170; press.ly = 23170; }
                else if (name == "lupright") { press.lx = 23170; press.ly = 23170; }
                else if (name == "ldownleft") { press.lx = -23170; press.ly = -23170; }
                else if (name == "ldownright") { press.lx = 23170; press.ly = -23170; }
                else if (name == "rup") press.ry = 32767;
                else if (name == "rdown") press.ry = -32768;
                else if (name == "rleft") press.rx = -32768;
                else if (name == "rright") press.rx = 32767;
                if (name == "a") press.buttons = A;
                else if (name == "b") press.buttons = B;
                else if (name == "x") press.buttons = X;
                else if (name == "y") press.buttons = Y;
                else if (name == "start") press.buttons = START;
                else if (name == "back") press.buttons = BACK;
                else if (name == "lb") press.buttons = LEFT_SHOULDER;
                else if (name == "l3") press.buttons = LEFT_THUMB;
                else if (name == "r3") press.buttons = RIGHT_THUMB;
                else if (name == "rb") press.buttons = RIGHT_SHOULDER;
                else if (name == "up") press.buttons = DPAD_UP;
                else if (name == "down") press.buttons = DPAD_DOWN;
                else if (name == "left") press.buttons = DPAD_LEFT;
                else if (name == "right") press.buttons = DPAD_RIGHT;
                else if (name == "lt") press.leftTrigger = true;
                else if (name == "rt") press.rightTrigger = true;
                g_autoPresses.push_back(press);
            }
            start = end + 1;
        }
        LOGFN("[input] {} scripted presses (DR_AUTO_INPUT)", g_autoPresses.size());
    }
}

void Input::Init()
{
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
        LOGFN_WARNING("[input] game controllers unavailable: {}", SDL_GetError());
    ParseAutoInput();
    TouchControls::Init();
}

void Input::HandleEvent(const SDL_Event& event)
{
    if (TouchControls::HandleEvent(event))
        return;
    if (event.type == SDL_CONTROLLERDEVICEADDED)
    {
        if (SDL_GameController* controller = SDL_GameControllerOpen(event.cdevice.which))
        {
            g_controllers.push_back(controller);
            LOGFN("[input] controller connected: {}", SDL_GameControllerName(controller) ? SDL_GameControllerName(controller) : "?");
        }
    }
    else if (event.type == SDL_CONTROLLERDEVICEREMOVED)
    {
        for (auto it = g_controllers.begin(); it != g_controllers.end(); ++it)
            if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(*it)) == event.cdevice.which)
            {
                SDL_GameControllerClose(*it);
                g_controllers.erase(it);
                LOGN("[input] controller disconnected");
                break;
            }
    }
}

void Input::Update(uint32_t videoFrame)
{
    Pad pad;

    // Rumble as the game sets it (left: low-frequency motor, right: high-frequency), renewed while on:
    // SDL stops an effect after its duration. DR_NO_RUMBLE=1 turns it off.
    {
        static const bool noRumble = getenv("DR_NO_RUMBLE") != nullptr;
        static uint32_t applied = 0, appliedFrame = 0;
        const uint32_t vibration = noRumble ? 0 : g_vibration.load();
        if (vibration != applied || (vibration != 0 && videoFrame - appliedFrame >= 60))
        {
            // Timed: a slow controller write would hold up the video thread, and the game with it.
            const auto start = std::chrono::steady_clock::now();
            for (SDL_GameController* controller : g_controllers)
                SDL_GameControllerRumble(controller, uint16_t(vibration >> 16), uint16_t(vibration), vibration != 0 ? 3000 : 0);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            static uint32_t logged;
            if (ms >= 10 && logged++ < 50)
                LOGFN("[input] rumble {:08X} took {} ms", vibration, ms);
            applied = vibration;
            appliedFrame = videoFrame;
        }
    }

    // Controllers: the first one with any input wins; buttons of all are combined.
    for (SDL_GameController* controller : g_controllers)
    {
        static constexpr std::pair<SDL_GameControllerButton, uint16_t> kButtons[] = {
            { SDL_CONTROLLER_BUTTON_DPAD_UP, DPAD_UP }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, DPAD_DOWN },
            { SDL_CONTROLLER_BUTTON_DPAD_LEFT, DPAD_LEFT }, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, DPAD_RIGHT },
            { SDL_CONTROLLER_BUTTON_START, START }, { SDL_CONTROLLER_BUTTON_BACK, BACK },
            { SDL_CONTROLLER_BUTTON_LEFTSTICK, LEFT_THUMB }, { SDL_CONTROLLER_BUTTON_RIGHTSTICK, RIGHT_THUMB },
            { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, LEFT_SHOULDER }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, RIGHT_SHOULDER },
            { SDL_CONTROLLER_BUTTON_A, A }, { SDL_CONTROLLER_BUTTON_B, B }, { SDL_CONTROLLER_BUTTON_X, X }, { SDL_CONTROLLER_BUTTON_Y, Y },
        };
        for (auto [button, bit] : kButtons)
            if (SDL_GameControllerGetButton(controller, button))
                pad.buttons |= bit;
        pad.leftTrigger = std::max<uint8_t>(pad.leftTrigger, uint8_t(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7));
        pad.rightTrigger = std::max<uint8_t>(pad.rightTrigger, uint8_t(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7));
        if (pad.thumbLX == 0 && pad.thumbLY == 0 && pad.thumbRX == 0 && pad.thumbRY == 0)
        {
            pad.thumbLX = Axis(controller, SDL_CONTROLLER_AXIS_LEFTX, false);
            pad.thumbLY = Axis(controller, SDL_CONTROLLER_AXIS_LEFTY, true);
            pad.thumbRX = Axis(controller, SDL_CONTROLLER_AXIS_RIGHTX, false);
            pad.thumbRY = Axis(controller, SDL_CONTROLLER_AXIS_RIGHTY, true);
        }
    }

    // Keyboard.
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    if (keys != nullptr)
    {
        static constexpr std::pair<SDL_Scancode, uint16_t> kKeys[] = {
            { SDL_SCANCODE_UP, DPAD_UP }, { SDL_SCANCODE_DOWN, DPAD_DOWN }, { SDL_SCANCODE_LEFT, DPAD_LEFT }, { SDL_SCANCODE_RIGHT, DPAD_RIGHT },
            { SDL_SCANCODE_RETURN, START }, { SDL_SCANCODE_BACKSPACE, BACK }, { SDL_SCANCODE_TAB, BACK },
            { SDL_SCANCODE_LCTRL, LEFT_THUMB }, { SDL_SCANCODE_RCTRL, RIGHT_THUMB },
            { SDL_SCANCODE_1, LEFT_SHOULDER }, { SDL_SCANCODE_3, RIGHT_SHOULDER },
            { SDL_SCANCODE_SPACE, A }, { SDL_SCANCODE_E, B }, { SDL_SCANCODE_Q, X }, { SDL_SCANCODE_R, Y },
        };
        for (auto [key, bit] : kKeys)
            if (keys[key])
                pad.buttons |= bit;
        if (keys[SDL_SCANCODE_LSHIFT]) pad.leftTrigger = 255;
        if (keys[SDL_SCANCODE_F]) pad.rightTrigger = 255;
        auto stick = [&](SDL_Scancode negative, SDL_Scancode positive, int16_t& axis)
        {
            if (keys[negative] != keys[positive])
                axis = keys[positive] ? 32767 : -32768;
        };
        stick(SDL_SCANCODE_A, SDL_SCANCODE_D, pad.thumbLX);
        stick(SDL_SCANCODE_S, SDL_SCANCODE_W, pad.thumbLY);
        stick(SDL_SCANCODE_J, SDL_SCANCODE_L, pad.thumbRX);
        stick(SDL_SCANCODE_K, SDL_SCANCODE_I, pad.thumbRY);
    }

    // DR_NO_PAD=1 (test windows): real controllers and the keyboard are ignored, so a player's controller
    // (which reaches every window) cannot change a scripted run.
    static const bool noPad = getenv("DR_NO_PAD") != nullptr;
    if (noPad)
        pad = Pad{};

    // Scripted presses: held for their duration (video frames).
    for (const AutoPress& press : g_autoPresses)
        if (videoFrame >= press.frame && videoFrame < press.frame + press.duration)
        {
            pad.buttons |= press.buttons;
            if (press.leftTrigger) pad.leftTrigger = 255;
            if (press.rightTrigger) pad.rightTrigger = 255;
            if (press.lx || press.ly) { pad.thumbLX = press.lx; pad.thumbLY = press.ly; }
            if (press.rx || press.ry) { pad.thumbRX = press.rx; pad.thumbRY = press.ry; }
            static uint32_t lastLogged = UINT32_MAX;
            if (lastLogged != press.frame)
            {
                lastLogged = press.frame;
                LOGFN("[input] scripted press {:04X} at video frame {}", press.buttons, videoFrame);
            }
        }

    std::lock_guard lock(g_mutex);
    pad.buttons |= g_touchPad.buttons;
    pad.leftTrigger = std::max(pad.leftTrigger, g_touchPad.leftTrigger);
    pad.rightTrigger = std::max(pad.rightTrigger, g_touchPad.rightTrigger);
    if (g_touchPad.thumbLX || g_touchPad.thumbLY) { pad.thumbLX = g_touchPad.thumbLX; pad.thumbLY = g_touchPad.thumbLY; }
    if (g_touchPad.thumbRX || g_touchPad.thumbRY) { pad.thumbRX = g_touchPad.thumbRX; pad.thumbRY = g_touchPad.thumbRY; }
    g_rawPad = pad;
    Pad game = g_blocked ? Pad{} : pad;
    if (!g_blocked)
        game.buttons = OptionsMenu::FilterButtons(pad.buttons); // Start + Back opens the options menu
    if (memcmp(&game, &g_pad, sizeof(game)) != 0)
    {
        g_pad = game;
        g_packet++;
    }
}

bool Input::ControllerConnected()
{
    return !g_controllers.empty();
}

Input::Pad Input::GetPad(uint32_t& packetNumber)
{
    std::lock_guard lock(g_mutex);
    packetNumber = g_packet;
    return g_pad;
}

void Input::SetTouchPad(const Pad& pad)
{
    std::lock_guard lock(g_mutex);
    g_touchPad = pad;
}

void Input::SetVibration(uint16_t left, uint16_t right)
{
    g_vibration = (uint32_t(left) << 16) | right;
}

Input::Pad Input::GetRawPad()
{
    std::lock_guard lock(g_mutex);
    return g_rawPad;
}

void Input::SetBlocked(bool blocked)
{
    g_blocked = blocked;
}
