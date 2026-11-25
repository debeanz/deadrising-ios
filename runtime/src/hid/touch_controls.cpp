// See touch_controls.h.

#include <stdafx.h>
#include "touch_controls.h"
#include "input.h"

#include <SDL.h>
#include <os/logger.h>

#include <cmath>
#include <unordered_map>

namespace
{
    enum : uint16_t
    {
        DPAD_UP = 0x0001, DPAD_DOWN = 0x0002, DPAD_LEFT = 0x0004, DPAD_RIGHT = 0x0008,
        START = 0x0010, BACK = 0x0020, LEFT_SHOULDER = 0x0100, RIGHT_SHOULDER = 0x0200,
        A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000,
    };

    // Layout in "height units": y in [0, 1] top to bottom, x in [0, aspect].
    struct Button
    {
        float x, y, radius;
        uint16_t bit;
        uint8_t trigger; // 1 LT, 2 RT
        float color[3];
    };

    constexpr float kStickX = 0.30f, kStickY = 0.72f, kStickRadius = 0.17f, kKnobRadius = 0.075f;
    constexpr float kCameraRange = 0.15f; // drag distance for full right-stick deflection

    std::vector<Button> LayoutButtons(float aspect)
    {
        const float fx = aspect - 0.30f, fy = 0.70f, step = 0.115f;
        const float cx = aspect * 0.5f;
        return {
            { fx, fy + step, 0.058f, A, 0, { 0.30f, 0.85f, 0.35f } },
            { fx + step, fy, 0.058f, B, 0, { 0.90f, 0.30f, 0.30f } },
            { fx - step, fy, 0.058f, X, 0, { 0.30f, 0.50f, 0.95f } },
            { fx, fy - step, 0.058f, Y, 0, { 0.95f, 0.85f, 0.30f } },
            { 0.12f, 0.12f, 0.05f, 0, 1, { 0.85f, 0.85f, 0.85f } },               // LT
            { 0.27f, 0.12f, 0.05f, LEFT_SHOULDER, 0, { 0.85f, 0.85f, 0.85f } },
            { aspect - 0.12f, 0.12f, 0.05f, 0, 2, { 0.85f, 0.85f, 0.85f } },      // RT
            { aspect - 0.27f, 0.12f, 0.05f, RIGHT_SHOULDER, 0, { 0.85f, 0.85f, 0.85f } },
            { cx - 0.08f, 0.07f, 0.035f, BACK, 0, { 0.85f, 0.85f, 0.85f } },
            { cx + 0.08f, 0.07f, 0.035f, START, 0, { 0.85f, 0.85f, 0.85f } },
            { 0.62f, 0.77f, 0.035f, DPAD_UP, 0, { 0.75f, 0.75f, 0.75f } },
            { 0.62f, 0.91f, 0.035f, DPAD_DOWN, 0, { 0.75f, 0.75f, 0.75f } },
            { 0.55f, 0.84f, 0.035f, DPAD_LEFT, 0, { 0.75f, 0.75f, 0.75f } },
            { 0.69f, 0.84f, 0.035f, DPAD_RIGHT, 0, { 0.75f, 0.75f, 0.75f } },
        };
    }

    enum class Owner : uint8_t { Stick, Camera, Button };
    struct Finger
    {
        Owner owner;
        int button = -1;
        float downX = 0, downY = 0, x = 0, y = 0;
    };

    std::mutex g_mutex;
    std::unordered_map<SDL_FingerID, Finger> g_fingers;
    float g_aspect = 16.0f / 9.0f;
    bool g_enabled = false;
    uint32_t g_lastTouchTicks = 0;

    float WindowAspect(uint32_t windowID)
    {
        if (SDL_Window* window = SDL_GetWindowFromID(windowID))
        {
            int w = 0, h = 0;
            SDL_GetWindowSize(window, &w, &h);
            if (w > 0 && h > 0)
                return float(w) / float(h);
        }
        return g_aspect;
    }

    void Clamp(float& x, float& y)
    {
        const float length = std::sqrt(x * x + y * y);
        if (length > 1.0f)
        {
            x /= length;
            y /= length;
        }
    }

    // The virtual pad from the fingers down now.
    Input::Pad ComputePad()
    {
        Input::Pad pad;
        const auto buttons = LayoutButtons(g_aspect);
        for (auto& [id, finger] : g_fingers)
        {
            if (finger.owner == Owner::Button && finger.button >= 0)
            {
                const Button& b = buttons[size_t(finger.button)];
                pad.buttons |= b.bit;
                if (b.trigger == 1) pad.leftTrigger = 255;
                if (b.trigger == 2) pad.rightTrigger = 255;
            }
            else if (finger.owner == Owner::Stick)
            {
                float x = (finger.x - kStickX) / kStickRadius, y = (finger.y - kStickY) / kStickRadius;
                Clamp(x, y);
                pad.thumbLX = int16_t(x * 32767.0f);
                pad.thumbLY = int16_t(-y * 32767.0f);
            }
            else if (finger.owner == Owner::Camera)
            {
                float x = (finger.x - finger.downX) / kCameraRange, y = (finger.y - finger.downY) / kCameraRange;
                Clamp(x, y);
                pad.thumbRX = int16_t(x * 32767.0f);
                pad.thumbRY = int16_t(-y * 32767.0f);
            }
        }
        return pad;
    }
}

void TouchControls::Init()
{
#if defined(__APPLE__) && TARGET_OS_IPHONE
    g_enabled = false; // iPhone / iPad: native controls in UIKit (os/ios_overlay.mm), not drawn by the renderer
#else
    g_enabled = getenv("DR_TOUCH") != nullptr;
    if (g_enabled)
        SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "1"); // the mouse acts as a finger (testing)
#endif
    if (g_enabled)
        LOGN("[input] on-screen touch controls enabled");
}

bool TouchControls::HandleEvent(const SDL_Event& event)
{
    if (!g_enabled || (event.type != SDL_FINGERDOWN && event.type != SDL_FINGERMOTION && event.type != SDL_FINGERUP))
        return false;

    std::lock_guard lock(g_mutex);
    g_aspect = WindowAspect(event.tfinger.windowID);
    const float x = event.tfinger.x * g_aspect, y = event.tfinger.y;
    g_lastTouchTicks = SDL_GetTicks();

    if (event.type == SDL_FINGERDOWN)
    {
        Finger finger;
        finger.downX = finger.x = x;
        finger.downY = finger.y = y;
        finger.owner = x < g_aspect * 0.5f ? Owner::Stick : Owner::Camera;

        const auto buttons = LayoutButtons(g_aspect);
        float best = 1e9f;
        for (size_t i = 0; i < buttons.size(); i++)
        {
            const float dx = x - buttons[i].x, dy = y - buttons[i].y;
            const float distance = std::sqrt(dx * dx + dy * dy);
            if (distance < buttons[i].radius * 1.35f && distance < best)
            {
                best = distance;
                finger.owner = Owner::Button;
                finger.button = int(i);
            }
        }
        g_fingers[event.tfinger.fingerId] = finger;
    }
    else if (event.type == SDL_FINGERMOTION)
    {
        auto it = g_fingers.find(event.tfinger.fingerId);
        if (it != g_fingers.end())
        {
            it->second.x = x;
            it->second.y = y;
        }
    }
    else
    {
        g_fingers.erase(event.tfinger.fingerId);
    }

    Input::SetTouchPad(ComputePad());
    return true;
}

void TouchControls::CancelAll()
{
    std::lock_guard lock(g_mutex);
    g_fingers.clear();
    Input::SetTouchPad(Input::Pad{});
}

bool TouchControls::Visible(bool controllerConnected)
{
    if (!g_enabled)
        return false;
    static const bool always = getenv("DR_TOUCH") != nullptr && getenv("DR_TOUCH")[0] == '2'; // testing
    if (always)
        return true;
    // With a controller: hidden until the screen is touched again (for 10 s).
    return !controllerConnected || SDL_GetTicks() - g_lastTouchTicks < 10000;
}

void TouchControls::GetOverlay(uint32_t width, uint32_t height, std::vector<Circle>& circles)
{
    std::lock_guard lock(g_mutex);
    const float scale = float(height);
    const float aspect = float(width) / float(height);
    const auto buttons = LayoutButtons(aspect);

    std::vector<bool> pressed(buttons.size(), false);
    float knobX = kStickX, knobY = kStickY;
    for (auto& [id, finger] : g_fingers)
    {
        if (finger.owner == Owner::Button && finger.button >= 0)
            pressed[size_t(finger.button)] = true;
        else if (finger.owner == Owner::Stick)
        {
            float x = (finger.x - kStickX) / kStickRadius, y = (finger.y - kStickY) / kStickRadius;
            Clamp(x, y);
            knobX = kStickX + x * kStickRadius;
            knobY = kStickY + y * kStickRadius;
        }
    }

    circles.push_back({ kStickX * scale, kStickY * scale, kStickRadius * scale, false, { 1.0f, 1.0f, 1.0f, 0.35f } });
    circles.push_back({ knobX * scale, knobY * scale, kKnobRadius * scale, true, { 1.0f, 1.0f, 1.0f, 0.40f } });
    for (size_t i = 0; i < buttons.size(); i++)
    {
        const Button& b = buttons[i];
        const float alpha = pressed[i] ? 0.65f : 0.30f;
        circles.push_back({ b.x * scale, b.y * scale, b.radius * scale, true, { b.color[0], b.color[1], b.color[2], alpha } });
        circles.push_back({ b.x * scale, b.y * scale, b.radius * scale, false, { 0.08f, 0.08f, 0.08f, 0.45f } }); // outline
    }
}
