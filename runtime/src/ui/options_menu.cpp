// See options_menu.h. The menu is drawn on the CPU into a 1280x720 RGBA image (8x16 VGA font,
// font_vga16.h) that the renderer blends over the final picture, scaled to the window.

#include <stdafx.h>
#include "options_menu.h"
#include "font_vga16.h"

#include <hid/input.h>
#include <os/logger.h>
#include <user/config.h>

#include <SDL.h>

#include <functional>
#include <string>
#include <vector>

namespace
{
    constexpr uint32_t kWidth = 1280, kHeight = 720;

    enum : uint16_t
    {
        DPAD_UP = 0x0001, DPAD_DOWN = 0x0002, DPAD_LEFT = 0x0004, DPAD_RIGHT = 0x0008,
        START = 0x0010, BACK = 0x0020, LEFT_THUMB = 0x0040, RIGHT_THUMB = 0x0080, A = 0x1000, B = 0x2000,
    };

    struct Item
    {
        const char* label;
        const char* help;
        std::function<std::string()> value;
        std::function<void(int)> change; // +1 / -1
    };

    std::vector<uint8_t> g_image(size_t(kWidth) * kHeight * 4);
    uint32_t g_version = 0;
    bool g_open = false;
    std::string g_status; // SetStatus: the panel shown while the game gives no picture
    double g_fps = 0.0;          // last measured by the video loop (SetFps)
    int g_shownFps = -1;
    bool g_waitRelease = false; // closed: the game gets input back once everything is released
    bool g_comboOpen = false;   // Start + Back seen by FilterButtons: open on the next Update
    int g_selected = 0;

    std::string OnOff(bool on) { return on ? "On" : "Off"; }

    const std::vector<Item>& Items()
    {
        static const std::vector<Item> items = [] {
            std::vector<Item> list = {
                { "Frame Rate", "60 FPS is twice as smooth. Menus, the HUD and the game's timing stay as on the console.",
                    [] { return Config::FrameRate == 60 ? std::string("60 FPS") : std::string("30 FPS (console)"); },
                    [](int) { Config::FrameRate = Config::FrameRate == 60 ? 30 : 60; } },
                { "Resolution", "Renders the game above the console's 720p: sharper, but needs a faster graphics card. "
                    "Pick your screen's resolution, or higher for extra smoothing.",
                    [] {
                        const float scale = Config::ResolutionScale;
                        return scale >= 2.75f ? std::string("2160p (3x)") : scale >= 1.75f ? std::string("1440p (2x)")
                            : scale >= 1.25f ? std::string("1080p (1.5x)") : std::string("720p (console)");
                    },
                    [](int direction) {
                        static constexpr float kScales[] = { 1.0f, 1.5f, 2.0f, 3.0f };
                        int index = 0;
                        for (int i = 0; i < 4; i++)
                            if (Config::ResolutionScale >= kScales[i] - 0.25f)
                                index = i;
                        Config::ResolutionScale = kScales[(index + (direction < 0 ? 3 : 1)) % 4];
                    } },
                { "Motion Blur", "Blur from camera and character movement, as on the console.",
                    [] { return OnOff(Config::MotionBlur); }, [](int) { Config::MotionBlur = !Config::MotionBlur; } },
                { "Anti-Aliasing", "Smooths edges the way the console does (4X MSAA).",
                    [] { return Config::MSAA ? std::string("4X MSAA") : std::string("Off"); }, [](int) { Config::MSAA = !Config::MSAA; } },
                { "FXAA", "An extra smoothing filter over the final picture.",
                    [] { return OnOff(Config::FXAA); }, [](int) { Config::FXAA = !Config::FXAA; } },
                { "V-Sync", "Waits for the display's refresh: no tearing.",
                    [] { return OnOff(Config::VSync); }, [](int) { Config::VSync = !Config::VSync; } },
                { "Show FPS", "Frames per second in the top-left corner.",
                    [] { return OnOff(Config::ShowFps); }, [](int) { Config::ShowFps = !Config::ShowFps; } },
            };
#if !(defined(__APPLE__) && TARGET_OS_IPHONE)
            list.push_back({ "Fullscreen", "Fills the screen (also Alt+Enter).",
                [] { return OnOff(Config::Fullscreen); }, [](int) { Config::Fullscreen = !Config::Fullscreen; } });
#endif
            return list;
        }();
        return items;
    }

    // ---- drawing ------------------------------------------------------------------------------

    void Fill(int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        x0 = std::max(x0, 0), y0 = std::max(y0, 0);
        x1 = std::min(x1, int(kWidth)), y1 = std::min(y1, int(kHeight));
        for (int y = y0; y < y1; y++)
        {
            uint8_t* p = g_image.data() + (size_t(y) * kWidth + x0) * 4;
            for (int x = x0; x < x1; x++, p += 4)
                p[0] = r, p[1] = g, p[2] = b, p[3] = a;
        }
    }

    int TextWidth(const std::string& text, int scale)
    {
        return int(text.size()) * 8 * scale;
    }

    void Glyphs(int x, int y, int scale, const std::string& text, uint8_t r, uint8_t g, uint8_t b)
    {
        for (char c : text)
        {
            const int index = (unsigned char)c - 32;
            if (index >= 0 && index < 95)
                for (int row = 0; row < 16; row++)
                {
                    const uint8_t bits = kVgaFont16[index][row];
                    for (int col = 0; col < 8; col++)
                        if (bits & (0x80 >> col))
                            Fill(x + col * scale, y + row * scale, x + (col + 1) * scale, y + (row + 1) * scale, r, g, b, 255);
                }
            x += 8 * scale;
        }
    }

    void Text(int x, int y, int scale, const std::string& text, uint8_t r = 235, uint8_t g = 235, uint8_t b = 235)
    {
        Glyphs(x + scale, y + scale, scale, text, 0, 0, 0); // shadow
        Glyphs(x, y, scale, text, r, g, b);
    }

    // While the game gives no picture (starting, or stuck): what is going on, and the log's last
    // lines, so the screen is never just black (2026-11-25, the iPhone build).
    void DrawStatus()
    {
        Fill(0, 0, kWidth, kHeight, 8, 10, 14, 235);
        Text(24, 18, 2, "Dead Rising (recompiled)", 255, 235, 80);
        Text(24, 54, 1, g_status, 255, 170, 90);
#if defined(__APPLE__)
        Text(24, 74, 1, "Full log: Files > On My iPhone > Dead Rising > DRRecomp.log. Options: Start + Back.", 150, 150, 150);
#else
        Text(24, 74, 1, "Full log: DRRecomp.log next to the game. Options: F1 or Start + Back.", 150, 150, 150);
#endif
        int y = 104;
        for (std::string line : os::logger::RecentLines(36))
        {
            if (line.size() > 156)
                line = line.substr(0, 153) + "...";
            Text(24, y, 1, line, 200, 200, 200);
            y += 17;
        }
    }

    void Redraw()
    {
        std::fill(g_image.begin(), g_image.end(), uint8_t(0));
        if (!g_status.empty() && !g_open)
            DrawStatus();
        if (Config::ShowFps && (g_status.empty() || g_open)) // not over the status panel's title
        {
            // Frame counter, top left (also while the menu is open).
            const std::string fps = g_shownFps < 0 ? std::string("-- FPS") : std::to_string(g_shownFps) + " FPS";
            Fill(12, 10, 12 + TextWidth(fps, 2) + 20, 10 + 44, 0, 0, 0, 150);
            Text(22, 16, 2, fps, 255, 235, 80);
        }
        if (g_open)
        {
            const int x0 = 200, x1 = 1080, y0 = 60, y1 = 670;
            Fill(0, 0, kWidth, kHeight, 0, 0, 0, 120);         // dim the game
            Fill(x0, y0, x1, y1, 12, 12, 16, 235);              // panel
            Fill(x0, y0, x1, y0 + 6, 170, 22, 22, 255);         // accent
            const std::string title = "OPTIONS";
            Text((kWidth - TextWidth(title, 3)) / 2, y0 + 26, 3, title);

            const auto& items = Items();
            int y = y0 + 100;
            for (int i = 0; i < int(items.size()); i++, y += 46)
            {
                const bool selected = i == g_selected;
                if (selected)
                    Fill(x0 + 24, y - 8, x1 - 24, y + 40, 150, 20, 20, 230);
                Text(x0 + 44, y, 2, items[i].label);
                const std::string value = selected ? "< " + items[i].value() + " >" : items[i].value();
                Text(x1 - 44 - TextWidth(value, 2), y, 2, value, selected ? 255 : 200, selected ? 255 : 200, selected ? 255 : 200);
            }

            // Help for the selected setting, wrapped to the panel.
            std::string help = items[g_selected].help;
            const size_t perLine = size_t(x1 - x0 - 88) / 8;
            int hy = y1 - 110;
            while (!help.empty())
            {
                size_t cut = help.size() <= perLine ? help.size() : help.rfind(' ', perLine);
                if (cut == std::string::npos || cut == 0)
                    cut = std::min(perLine, help.size());
                Text(x0 + 44, hy, 1, help.substr(0, cut), 200, 200, 200);
                help.erase(0, cut);
                while (!help.empty() && help[0] == ' ')
                    help.erase(0, 1);
                hy += 20;
            }
            const std::string footer = "Up / Down: choose    Left / Right / A: change    B / Esc / F1: close";
            Text((kWidth - TextWidth(footer, 1)) / 2, y1 - 36, 1, footer, 160, 160, 160);
        }
        g_version++;
    }

    void Open()
    {
        g_open = true;
        Input::SetBlocked(true);
        Redraw();
        LOGN("[options] opened");
    }

    void Close()
    {
        g_open = false;
        g_waitRelease = true; // keep the game's input off until the closing button is released
        Config::Save();
        Redraw();
        LOGN("[options] closed, settings saved");
    }
}

bool OptionsMenu::HandleEvent(const SDL_Event& event)
{
    if (event.type != SDL_KEYDOWN || event.key.repeat)
        return false;
    static const bool noPad = getenv("DR_NO_PAD") != nullptr; // test windows: no keyboard (hid/input.cpp)
    if (noPad)
        return false;
    // F1 opens and closes it anywhere (title, menus, gameplay); Escape also closes it.
    if (event.key.keysym.scancode == SDL_SCANCODE_F1)
    {
        LOGN("[options] F1 pressed");
        g_open ? Close() : Open();
        return true;
    }
    if (g_open && event.key.keysym.scancode == SDL_SCANCODE_ESCAPE)
    {
        Close();
        return true;
    }
    return false;
}

void OptionsMenu::Update()
{
    const Input::Pad pad = Input::GetRawPad();
    static uint16_t previous = 0;
    const uint16_t pressed = pad.buttons & ~previous;
    previous = pad.buttons;

    // Start + Back (FilterButtons, before the game sees either) open it.
    if (g_comboOpen)
    {
        g_comboOpen = false;
        if (!g_open)
        {
            LOGN("[options] Start + Back pressed");
            Open();
            return;
        }
    }

    if (!g_open)
    {
        if (g_waitRelease && pad.buttons == 0 && std::abs(pad.thumbLX) < 8000 && std::abs(pad.thumbLY) < 8000)
        {
            g_waitRelease = false;
            Input::SetBlocked(false);
        }
        return;
    }

    // Directions from the D-pad or the left stick, repeating while held.
    int dx = 0, dy = 0;
    if ((pad.buttons & DPAD_UP) || pad.thumbLY > 16000) dy = -1;
    if ((pad.buttons & DPAD_DOWN) || pad.thumbLY < -16000) dy = 1;
    if ((pad.buttons & DPAD_LEFT) || pad.thumbLX < -16000) dx = -1;
    if ((pad.buttons & DPAD_RIGHT) || pad.thumbLX > 16000) dx = 1;
    static int heldX = 0, heldY = 0, holdFrames = 0;
    bool fire = false;
    if (dx != heldX || dy != heldY)
    {
        heldX = dx, heldY = dy, holdFrames = 0;
        fire = dx != 0 || dy != 0;
    }
    else if (dx != 0 || dy != 0)
        fire = ++holdFrames > 20 && holdFrames % 6 == 0;

    const auto& items = Items();
    bool changed = false;
    if (fire && dy != 0)
    {
        g_selected = (g_selected + dy + int(items.size())) % int(items.size());
        changed = true;
    }
    if (fire && dx != 0)
    {
        items[g_selected].change(dx);
        changed = true;
    }
    if ((pad.buttons & (START | BACK)) == (START | BACK) && (pressed & (START | BACK)) != 0)
    {
        Close(); // Start + Back again
        return;
    }
    if (pressed & (A | START))
    {
        items[g_selected].change(1);
        changed = true;
    }
    if (pressed & (B | BACK))
    {
        Close();
        return;
    }
    if (changed)
        Redraw();
}

void OptionsMenu::SetStatus(const std::string& headline)
{
    if (headline.empty() && g_status.empty())
        return;
    g_status = headline;
    Redraw(); // the log lines change too
}

bool OptionsMenu::Visible()
{
    return g_open || Config::ShowFps || !g_status.empty();
}

const uint8_t* OptionsMenu::Image(uint32_t& width, uint32_t& height, uint32_t& version)
{
    width = kWidth;
    height = kHeight;
    version = g_version;
    return g_image.data();
}

uint16_t OptionsMenu::FilterButtons(uint16_t raw)
{
    // Start or Back alone is held back from the game for a few frames: if the other one follows,
    // the menu opens and the game never sees either; if not, the game gets it late, and a tap
    // shorter than the wait is replayed for a few frames so it is not lost.
    constexpr uint16_t kPair = START | BACK;
    constexpr int kWait = 4;
    static int waited = 0, replay = 0;
    static uint16_t held = 0;
    if (g_open || g_waitRelease)
    {
        waited = replay = 0;
        return raw;
    }
    uint16_t game = raw;
    if ((raw & kPair) == kPair)
    {
        g_comboOpen = true;
        waited = 0;
        held = 0;
        return game & ~kPair;
    }
    if (raw & kPair)
    {
        if (waited == 0)
            held = raw & kPair;
        if (waited < kWait)
        {
            waited++;
            game &= ~kPair;
        }
    }
    else
    {
        if (waited > 0 && waited < kWait)
            replay = 3;
        else
            held = 0;
        waited = 0;
    }
    if (replay > 0)
    {
        game |= held;
        if (--replay == 0)
            held = 0;
    }
    return game;
}

void OptionsMenu::SetFps(double fps)
{
    g_fps = fps;
    const int rounded = int(fps + 0.5);
    if (rounded != g_shownFps)
    {
        g_shownFps = rounded;
        if (Config::ShowFps)
            Redraw();
    }
}
