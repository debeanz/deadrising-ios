# Frame rate (30 fps and the 60 fps work)

## How the game paces itself

* Present (`sub_8245BA18`) reads the presentation interval from the D3D device (+0x33C0,
  D3DPRESENT_INTERVAL_*). The game uses TWO: each swap waits for two vblanks (`sub_8245B818`,
  countdown at device +0x3B1C, vblank count +0x3B14), so 60 Hz vblanks give 30 fps.
* The game's clock is MT Framework's `sMain` (object at 0x40100010, pointer at 0x828AE8F8; property
  table `sub_82735598`): +64 `mFps` (float, 30), +68 `mFrameWait` (byte), +72 `mActualFps`, +104 /
  +108 pause, +112 `mDeltaTime` (float, in frames of 1/`mFps`).
* `sub_82734808` (called by the game's frame function `sub_8218ECF8`) sets `mDeltaTime`. With
  `mFrameWait` set (the console build), it is 1.0 every frame whatever the frame took. With it
  clear, it is the measured time × `mFps` (at most 16): 0.5 per frame at 60 fps.
* `sub_82752FE0` copies `mDeltaTime` into every unit (+16) before calling its move().
* `sub_821748A8(unit manager [0x828AECC4], "uPlayer" 0x82088D98)` returns Frank's object.

## Experiments (debug switches in `runtime/src/gpu/d3d_hooks.cpp`)

* `DR_PRESENT_INTERVAL=1` or `DR_VBLANK_HZ=120`: 60 fps, and the whole game runs twice as fast
  (boot logos in 3.8 s instead of 8 s). The fixed step counts frames, not time.
* `DR_VARIABLE_STEP=1` (clears `mFrameWait`) with `DR_PRESENT_INTERVAL=1`: 60 fps, `mDeltaTime`
  0.49-0.52. Movement speed and animation clocks are right (animation frames advance 0.5 a frame),
  but code that counts frames is not:
  * menus and HUD animations run at double speed;
  * jumps look sped up;
  * Frank gets stuck mid-stride while the stick is held (also with a steady 0.5:
    `DR_FIXED_DELTA=0.5`) until the stick is released or he jumps. While stuck his move() still
    runs, but position, bones and velocity stop. An integer at Frank's object +0x4F494 counts down
    one per frame (37 ... 0) and reaches 0 exactly when he stops; a velocity at +0x5B5E0 / +0x5B5E8
    drops to 0 at the same time.
* `DR_PLAYER_SNAP=<step>` (+ `DR_PLAYER_SNAP_SIZE`, `DR_PLAYER_SNAP_COUNT`) writes Frank's object
  to `snap/` every 5 steps, for diffing walking against stuck.

## Fixed for 60 fps (all inactive at 30 fps, where the step is exactly 1.0)

* **Frank stuck mid-stride** (`sub_822C21D0` wrapper, d3d_hooks.cpp): his base motion layer (+240:
  flags +248, frame +260, end +264, speed +268) is held at end - 1.0 while the motion system's
  "next frame reaches the end" flag (frame + speed >= end, `sub_826E7F08`) is set, so the next
  advance lands on the end. With 0.5 steps it went 20 -> 20.5 -> 20 for ever. Now held at
  end - speed x step. Found with `DR_WATCH_OFFSET` (write watch) on the frame.
* **Animation cross-fades** (`sub_826E7F08` wrapper): each motion layer (+252, 272 bytes, count at
  +224) blends over an int16 frame count (remaining / total); a cross-fade that just started gets
  both scaled by 1 / step.
* **Zombies and survivors stuck in place** (2026-11-25, after the mall invasion: "stuck", "struggle
  to chase"): the same hold as Frank's stride, in about 40 places of per-unit code (frame set to
  end - 1.0 while the layer's bit 0 is set). The `sub_826E7F08` wrapper now releases it for every
  unit and layer: a frame moved back to exactly end - 1.0 since the layer's last update (each
  layer's last frame is kept in a lossy lock-free table) is put one advance (speed x step) before
  the end. A frame that advanced to end - 1.0 is left alone. User-confirmed after the invasion
  ("they move fine now"); that play's log: 2,000+ holds released in uEm00 / uEm01 / uEm08
  (zombies) over 5 minutes, no crash.
* **Animation events fired two or three times** (2026-11-25, crashes: drinking coffee, X with a
  stick, X with nothing): each layer has two event tracks (+176 / +180: +64 count, +68 entries of
  bits and length in frames); the update sets the bits (+184 / +188, the update before at +192 /
  +196) of the segments overlapping frames int(frame) .. int(frame + speed - 0.00001), one 30 fps
  frame ahead whatever the step. At 60 fps a one-frame segment was set on three updates. Frank's
  drink action uses the held item on the "use" bit (Frank +0x1A8 bit 2) and clears it, so the
  second update called through null. The wrapper sets the bits again at 60 fps: the window looks
  speed x step ahead (longer segments last as long as at 30 fps), and a one-frame segment is set
  only on the update whose advance reached its frame (previous < frame <= current; a wrapped loop
  counts its end and its start; a motion clamped at its end reaches nothing), so once, as at 30 fps.
  `[motion]` log lines (every 1800 steps, when any fired) count both fixes, with sampled classes.
  Checked: Security Room save, walking, the door, punches; 1385 repeats dropped in 3600 steps.
  A layer whose frame did not advance (paused, or clamped at its end) keeps the game's bits: see
  the zombie entry below (dead zombies got up while only "reached" segments were set).
* **Jump height and air time** (mid-asm hooks `PlayerGravityMidAsmHook`, `PlayerFallMidAsmHook` in
  `sub_821FF738`, runtime/src/patches/frame_rate_hooks.cpp): fall speed (+4208) += gravity
  (1.96, 0x820DB570) and height -= fall speed, once per frame. Scaled by the step, with
  g x (1 - step) / 2 added so the half steps sum to the console's per-frame arc: peak 1467.87 at
  both rates, air time 17 frames.
* **Jump distance** (`PlayerJumpMoveMidAsmHook`, `sub_822C4AC8`): position += facing x jump speed
  (+5604, 13.0) per frame, scaled by the step: 221 units at 30 fps, 227 at 60 (was 416).

Mid-asm hooks: `[[midasm_hook]]` in config/DR.toml, then
`tools\XenonRecomp\build\XenonRecomp\XenonRecomp.exe config\DR.toml tools\XenonRecomp\XenonUtils\ppc_context.h`
(3 s; only the files with hooked functions change) and build_runtime.bat.

Test switches: `DR_WALK_TEST=<steps>` (hold forward that long after Frank exists),
`DR_JUMP_TEST=1` (press jump 60 frames in, log height / position for 60 frames),
`DR_PAUSE_TEST=1`, `DR_WATCH_JUMP=1` (arm the write watch at the jump), `DR_SNAP_CLASS=<class>[:<offset>]`
(snapshot / map / watch another unit), `DR_UNIT_LIST=<n>` (classes in the 32 move lines),
`DR_PLAYER_SNAP_EVERY=<n>`.

* **UI, menus, HUD** (`runtime/src/patches/frame_rate.h`): they count frames and keep no clock of
  their own (cSprAnm, vtable 0x82061740, holds no per-frame state). Above 30 fps they run at
  30 Hz: `FrameRate::OnStep` (from the sMain step) marks a UI frame whenever the steps add up to
  one 30 fps frame. The move pass queues every unit as a job (all 32 move lines have the
  top flag bit set), and the job function `sub_8242DCA0` (wrapped) tail-calls move(); it skips the
  move of uCockpit, uCockpitModel, uMessage, uTimeMessage,
  uSchedulerMessage, uFade, uPPDisp and uLoadingEffect on the other frames and
  gives them the accumulated step on theirs. Not uOption (the pause menu's Options page): it
  draws what its last move built, so at 30 Hz the whole page showed on every other frame only;
  moved every frame its fades take as long (they run on uFade). Checked frame by frame: the HUD,
  area labels, kill count, Status and Notebook pages draw on every frame. (`UnitMoveSkipMidAsmHook` covers the direct-call path
  at 0x82753168, which no line uses.) `DR_UI_LOG=1` counts their moves run / skipped. XamInputGetState only takes new buttons and triggers on UI frames (sticks every
  frame, or turning and the camera moved in 30 Hz steps): the game sees a press for the one frame its state changes, which must be one the UI
  runs on (and gameplay must not see it twice). Order per frame: sMain step, one input read,
  unit moves.
* Game tasks (cGametaskInit, Warning, Title, Main) run under sTaskManager (sMain + 0x10208,
  `sub_8219A2A0`). Its move calls the current task's move (vtable slot 24, skipped by the game
  itself while paused: sMain +104 / +108), then slots 32 and 36 every frame, then bookkeeping
  (sub_82199E70, a 30-entry loop, sub_8219A990). Above 30 fps only the slot 24 call of the boot and
  title tasks waits for UI frames (`TaskMoveSkipMidAsmHook` at 0x8219A344, jumping to 0x8219A354
  like the pause check; config/DR.toml); cGametaskMain (gameplay, cutscenes) moves every frame.
  The first version skipped the whole manager every other frame; with any 30 Hz task move, world
  markers queued by cGametaskMain (the roof's "Security Room" label) had their dark background
  drawn twice on every other frame: a 30 Hz blink at 60 fps (background luminance 10 / 5
  alternating, 10 steady once Main moved every frame). Measured with cGametaskMain at 60 Hz: the
  date screen before the intro, the intro cutscene and the pause menu animate exactly as with
  it throttled. `DR_TASK_EVERY_FRAME=1` or `=<class>,...` moves more tasks every frame
  (debugging); `[task]` log lines name each task as it starts. uGametaskPrim must move every
  frame: it rebuilds the tasks' draw list each move, and skipping it made the title menu flash.
  `DR_UI_KEEP=<class>,...` takes classes off the 30 Hz unit list (debugging).

* The game clock (2026-11-25): `sub_822DB980` keeps the 72-hour clock, game time at +336 in 30ths
  of a game second (+364..+367: day, hour, minute, second). When r4 is 0 it adds the rate at +332
  (12: the game's 12x clock) to it and to the play-time counter, then checks the case deadlines,
  once per frame. Measured at 60 fps in the Security Room (`[clock]` lines, every 20 s): 24.0
  game seconds per second, so the 72 hours and every deadline went twice as fast. The
  `sub_822DB980` wrapper (frame_rate_hooks.cpp) sets the rate to rate x step for the call, carrying
  the remainder to the next frame, and restores it afterwards: 12.0 game seconds per second at 60
  fps. `DR_CLOCK_FIX=0` turns it off.
* Zombies knocked back (2026-11-25, user report: a bat hit sent them further at 60 fps). The
  "blown" action `sub_8236BA40` (enemy action 0x0E, motions 0x204-0x207) adds the velocity at +3248
  to the position once per update and multiplies it by 0.9 (contact flags +3692 & 3) or 0.2 (& 0xC)
  each update; at 60 fps the zombie flew about twice as far (14.4 units added on each half step).
  Its wrapper scales friction F to F^step and the move to step x c x velocity, with
  c = F^(1-step) (1 - F^step) / (step (1 - F)), which matches the 30 fps distance for any step.
  Built, not yet confirmed in game.
* Zombies "stand up instantly" at 60 fps (same report). Enemy state bytes: +20, +21 (sub-state),
  +22 (action), +23 (group). The per-update action functions are in the table at 0x828A6000
  (vtable slot 64), the groups at 0x828A5820 (slot 61); when an action returns 1 (+4064 set),
  slot 59 (`sub_8236CB30`) picks the next one on the following update. Action 6 = knocked down,
  0x0E = blown away, 3 = walking, 7 with group 2 = dead (lies there until Frank is 5000 away).
  The cause was the event fix above, not damage: `[em hit]` (zombie damage function, uEm00 slot 34,
  `sub_82362F38`, HP at +3876 minus the hit info's +120) showed the bat doing 100 at both rates and
  HP reaching 0. The knocked-down and blown actions pause their landing motion on its last frame
  when it ends (status bit 0: frame = end - 1, mode |= 0x10, return 1), and `sub_8236D508` turns a
  zombie with HP 0 into action 7 (dead) only if that motion's event bit 1 (+424) is set. The game's
  motion update sets a paused layer's window bits on every update, but the 60 fps event fix set a
  one-frame segment only on an update that advanced onto it, so a paused or clamped layer had none:
  the dead zombie (group 2, 0x0E) went straight to walking (03, group 0), and zombies killed by the
  survivors in the prologue came back too. Fixed in the `sub_826E7F08` wrapper: a layer whose frame
  did not advance keeps the game's own bits (the console's, every update). Checked in the user's
  next session: all 43 zombies killed (36 at 60 fps, bat and the prologue survivors) went to action
  7 / group 2 with `events 2` and never moved again; one left at 50 HP got up through its own
  motions (7 / group 0, 8, 3) as at 30 fps. `[em detail]` lines end with `events` (+424).
* Knockback (2026-11-25 session log, after the fix above): a 30 fps bat hit's landing motion
  (0x205) moved 354 units, fastest 37.6 per 30 fps frame; at 60 fps 230-400 units, fastest about 40
  (was about twice as far).
* Climbing "faster at 60 fps" (user report, 2026-11-25; the user then said they could not tell):
  measured the same. `[pl]` lines (enemy trace, Frank's base layer) give, for each of his motions,
  the frames it played per 30 fps frame (1.00 when it follows speed x step), the updates that moved
  its frame by something else (game code setting it), and the distance and height moved. A climb is
  0x35 (crouch, speed 0.7) -> 0x36 -> 0x54 (one update each) -> 0x58 (climb up, speed 1.3); in the
  user's play 0x58 ran 1.26-1.28 frames per 30 fps frame at both rates, no update off speed x step,
  72.0 units forward every time, and +225 in 37.0 frames of time at 30 fps against +222-228 in
  37.0-37.6 at 60. Not measured: the camera's follow during the climb.
* Seen in passing, not changed: `sub_823619C0` (zombie slot 54) takes 1 HP per update while the
  zombie's status at +2820 has bit 1 or 3: twice as fast at 60 fps.

## Switching in game (options menu, `Config::FrameRate`)

* The `sub_82734808` wrapper clears `mFrameWait` while 60 is chosen and sets it again (once) when
  it goes back to 30.
* The Present hook (`sub_8245BA18`) writes interval 1 to device +0x33C0 while at 60, and 2 (the
  game's D3DPRESENT_INTERVAL_TWO) once when leaving 60. Do not restore a captured value instead:
  with `FrameRate = 60` already in recomp_config.toml the first capture ran before the game had set
  TWO, so going back to 30 kept interval 1: a fixed step at 60 fps, the game at twice its speed.
  Checked: started at 60, switched in the menu, 30 fps steady at normal speed (user-confirmed).

## What 60 fps still needs

* Anything else that counts frames (found by playing).
