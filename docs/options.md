# Options menu and settings

`recomp_config.toml` in the game folder (on iOS: Documents/game, visible in the Files app) holds
the settings; it is written with defaults on the first start and whenever the menu closes.

| Setting | Values | What it does |
|-|-|-|
| `FrameRate` | 30, 60 | 60: the engine's variable step plus one vblank per swap; menus, HUD and buttons stay at 30 Hz (docs/frame_rate.md). Switchable in game both ways: back at 30 the fixed step and two vblanks per swap return |
| `MotionBlur` | true / false | off: uMotionBlurFilter's full-scene and stretch blur scales (+64, +80) are held at 0 |
| `MSAA` | true / false | the console's 4X MSAA (off: every pass single-sampled); see renderer.md, "Real 4X MSAA", for the depth-restore bias that keeps it on in gameplay |
| `FXAA` | true / false | the smoothing filter in the final blit |
| `VSync` | true / false | swap chain vsync |
| `Fullscreen` | true / false | borderless desktop fullscreen (not on iOS) |
| `ResolutionScale` | 1, 1.5, 2, 3 | internal resolution ("Resolution" in the menu): 720p (the console's), 1080p, 1440p, 2160p; switchable while playing (renderer.md, "Resolution scale") |
| `ShowFps` | true / false | frames per second (video loop, every half second) in the top-left corner, drawn into the menu overlay; the overlay is shown while the menu is open or this is on |

## In game

F1 (keyboard, anywhere) or Start + Back (controller) opens and closes the menu; each open is logged (`[options] F1 pressed`, `Start + Back pressed`), so a key that never arrives (window not focused, a laptop F1 that needs Fn) shows in the log. Start or Back alone is held back from the game for up to 4 frames (`OptionsMenu::FilterButtons`, called from `Input::Update`): if the other follows, the menu opens and the game sees neither; otherwise the game gets the press late, and a shorter tap is replayed. Up / down chooses,
left / right, A or Start changes, B, Back, Escape, F1 or Start + Back again closes (and saves). While it is open the game
gets no input (`Input::SetBlocked`), and after it closes the game gets input back once every
button is released, so the closing press never reaches the game. The game keeps running behind
it: pause first in gameplay.

Code: `runtime/src/user/config.{h,cpp}` (values are atomics, read live by the renderer, the frame
step hook and the video loop), `runtime/src/ui/options_menu.{h,cpp}` (logic and drawing into a
1280x720 RGBA image with the 8x16 VGA font, `ui/font_vga16.h`), renderer: `mainMenuPS` in
blit.hlsl / kBlitShader, `g_menuPipeline`, uploaded when its version changes, blended over the
picture after the touch overlay and drawn into DR_SHOT dumps too. Scripted input knows `l3` and `r3` too.

Not done yet: a way to open it from the game's own pause / title menus, and a touch button for it
on iOS without a controller.
