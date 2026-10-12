# fakexr — a fake OpenXR runtime for testing without a headset

A small OpenXR runtime (OpenXR 1.0 + `XR_KHR_opengl_enable`, the subset the engine uses) that the OpenXR loader
picks up through `XR_RUNTIME_JSON`, so the engine runs its normal, unchanged VR path. Head and controller poses,
movements and buttons come from a control FIFO; on request the runtime composites the submitted layers (the
projection layer plus every quad: menu/HUD panel, hand panels, weapon grid, beams, cinema screen) like a compositor
and writes a PNG plus a JSON description of every layer.

Not simulated: reprojection, lens distortion, SteamVR's own binding remaps (the Sense → Touch emulation).
Haptic pulses are logged, not felt.

## Build

    make            # build/fakexr32 (i386, for the 32-bit engine) and build/fakexr64

Needs `bubblewrap` (write guard), `xorg-server-xvfb`, `python-pillow` (crops/zooms) and, for GPU rendering,
`virtualgl lib32-virtualgl`.

## Run

    ./fakexr-run live scenario.txt [xash args...]          # play a scenario against the installed game, then quit
    ./fakexr-run --serve live [xash args...]               # keep the game running ...
    ./fakexr-ctl <directive>                               # ... and drive it one directive at a time
    ./fakexr-ctl -f more.txt                               # or a file of them
    ./fakexr-ctl stop

`live` = the installed game (`~/Games/lambda1vr-pc`, or `$FAKEXR_LIVE`), used **read-only**. A thin root in
`$FAKEXR_OUT/root` holds symlinks to the installed binaries and takes every write (configs, saves); the engine reads
the game data from the install through `-rodir`. The game runs in a bubblewrap sandbox in which `$HOME` is read-only
except the output and cache directories, so a test cannot touch the install, its settings or its saves (a write
fails with "Read-only file system"); after each run the runner also reports anything in the install newer than the
run's start. The settings in effect are the installed ones (read from the install until the test root has saved
its own copy). Instead of `live` a full copy of the game tree still works (the real install itself is refused).
To test freshly built libraries without installing them, put them in a directory laid out like the game root
(`libxash.so`, `valve/cl_dlls/client.so`, ...) and pass it as `FAKEXR_OVERLAY`.

The engine looks for `<game>/custom/` and `<game>_hd/_addon/_lv` only in the writable root, not in `-rodir`; the
runner links those in (`cstrike/custom/extras.pk3` holds the CS VR models and the bot profiles).

`-game` is taken from the arguments (`fakexr-run live s.txt -game cstrike`), else `$FAKEXR_GAME`, else `valve`.
Output (PNGs, JSON, `game.log`) goes to `$FAKEXR_OUT` (default `./fakexr-out`).

Rendering: with VirtualGL installed the game's GL runs on the GPU at ~90 fps and 2048 px per eye; without it
llvmpipe manages 7–17 fps at 960 px, too slow for anything that depends on motion over time. `FAKEXR_GPU=0/1`
forces either. VirtualGL uses its GLX back end through the desktop's X server (`$DISPLAY`, hidden pbuffers: the
game window stays in Xvfb and nothing appears on the monitors); without a reachable X server the runner falls back
to llvmpipe. Not the EGL back end (`-d egl`): with VirtualGL 3.1.5 it hands the runtime the first image of each
swapchain forever, so every shot is stale (found 2026-10-12: head turns did not change the shots). Audio goes to SDL's dummy
driver (silent) unless `FAKEXR_AUDIO=1`.

### Scenario directives

One per line, `#` comments, `$OUT` = the output directory:

| Directive | Meaning |
|---|---|
| `xr <command>` | runtime control command (below) |
| `con <command>` | engine console command |
| `sleep <s>` | wait |
| `shot <name> [both\|left\|right] [layers] [world] [rect=x0,y0,x1,y1] [px=N]` | composited view of the next frame → `<name>.png` + `<name>.json`; `layers` also writes each quad layer's image to `<name>.layerN.png`; `world` leaves the panels out; `rect` renders only that part of the eye (pixels of a normal shot) with `px` as its longer side |
| `zoom <name> <shot> <layer> [left\|right] [px] [margin %]` | re-render the area of a panel seen in an earlier shot, sharp (default 1024 px) |
| `panel <shot> <layer> [left\|right] [margin px]` | crop a panel out of a shot as it is (no re-render) |
| `crop <shot> x0 y0 x1 y1 [name]` | crop a pixel box out of a shot |
| `mark` | later `wait-log`/`expect-log`/`log` only see game output after this point |
| `wait-log "<regex>" [s]` | wait until the game log matches (default 60 s) |
| `expect-log "<regex>"` | fail unless the log already matches |
| `log ["<regex>"]` | print the game log since the mark |
| `status` | print poses, held inputs, session state, measured fps, projection check counters |
| `check-projection [left\|right] [degrees]` | rotation consistency test: 3D-only shots before and after turning the head about that eye (pitch, yaw, roll; default 12°), fitted by `fakexr-projcheck.py`; fails unless the image uses the declared field of view (scale 1.000 ± 2 %). Needs a still scene (not paused: the paused game fades the world to black) |
| `include <file>` | run another scenario (relative to this one, or `scenarios/`) |

Ready-made includes in `scenarios/`: `hl-start.txt` (c1a0, cheats, god, all weapons) and `cs-spawn.txt` (de_dust2
with bots, spawned as CT; sets `maxplayers 16` first — a console `map` keeps 1 slot, so bots cannot join and the
player stays dead after `jointeam`). Wait for map loads on `Custom resource propagation complete` after a `mark`
(`client connected` needs `developer`; `first stereo frame submitted` is logged once per process).

### Runtime control commands

LOCAL space, metres and degrees; +X right, +Y up, -Z forward; pitch + = up, yaw + = left, roll + = tilt left; the
head starts at the origin:

| Command | |
|---|---|
| `head x y z pitch yaw roll` | head pose (jump) |
| `aim left\|right x y z pitch yaw roll` | controller aim pose (jump); the grip pose follows (`FAKEXR_GRIP_PITCH`) |
| `grip left\|right x y z pitch yaw roll` | set the grip pose explicitly |
| `look pitch yaw roll [left\|right]` | turn the head about that eye (the eye stays put: no parallax) |
| `fov left\|right\|both l r u d` | field of view handed out (degrees, left/down negative) |
| `move head\|left\|right x y z pitch yaw roll <s> [smooth\|linear]` | move there over time; the matching linear and angular velocity is reported meanwhile, so throws and swing gestures work like with a real arm |
| `vel left\|right vx vy vz` | extra controller linear velocity (m/s, kept until set again), added to a move's |
| `track left\|right 0\|1` | controller tracked |
| `input left\|right <component> <v> [<y>]` | input under `/user/hand/<hand>/input/`, e.g. `trigger/value 1`, `thumbstick 0 1`, `a/click 1` |
| `state focused\|visible\|synchronized` | session state: visible = dashboard open (inputs inactive), synchronized = headset asleep (no rendering) |
| `status` | log poses, inputs, state, fps |
| `shot <file.png> ...` | as above (the runner adds the path) |
| `exit` | ask the application to end the session |

The runtime presents Touch controllers (`FAKEXR_PROFILE`), like SteamVR does for the PS VR2 Sense controllers.
On the Touch layout Triangle = left `y/click`, Square = left `x/click`, Cross/Circle = right `a/click`/`b/click`,
Options = left `menu/click`.

### Projection checks

Two checks catch an app that renders with another field of view or aspect ratio than it declares (Black Mesa VR
squashed the world ~29 % vertically that way; here it would read "vertical 0.710"):

1. **Every frame** (runtime): the fov and eye pose each projection view says it used are compared with what
   `xrLocateViews` handed out, and the image's pixel aspect with that fov. Mismatches are logged as
   `fakexr: projection mismatch ...`; `status` prints the counters and worst values; the shot JSON has them per eye.
2. **`check-projection`** (image based): without parallax, a turned view must line up with the base view under the
   declared projection; the fit reports the real horizontal/vertical scale. Self-test: `fakexr-projcheck.py
   --declared-scale 1.1 0.8 <shots>` must report 0.909 / 1.250.

Default field of view: 52° out, 44° in, 48° up and 48.6° down per eye, so the vertical tan extent equals the
horizontal one and the recommended square image has square pixels (until 2026-10-12 it was 52° down, which made
every image's pixels 6 % taller than wide).

### The shot JSON

`frame`, `session`, `eye_size`, `shot_size`, `rect`, `world_only`, `eye_left`/`eye_right` (pose, fov in degrees,
and `check`: fov/pose/pixel-aspect difference of the submitted view), the head and both hands' aim/grip poses, and
per layer: type,
space (`local`/`view`/`stage` or `<hand> aim|grip <action>`), `tracked`, `eye`, `size` (m), swapchain image size,
`pose` (LOCAL), `from_head` (pose in the head's frame) and `png_left`/`png_right` = the panel's pixel box in this
PNG (may reach outside it; `null` when behind the eye). Poses are `{"pos": [x, y, z], "ang": [pitch, yaw, roll]}`.

Other environment variables: `FAKEXR_EYE_SIZE` (960, 2048 on the GPU), `FAKEXR_FPS` (90; 0 = unthrottled),
`FAKEXR_IPD` (0.064), `FAKEXR_VERBOSE`, `FAKEXR_DISPLAY`, `FAKEXR_SCREEN`, `FAKEXR_CACHE`.

Example (a grenade throw by swinging the arm):

    include hl-start.txt
    con weapon_handgrenade
    sleep 1.5
    xr aim right 0.2 -0.2 0.1 30 0 0
    xr input right trigger/value 1
    sleep 1.2
    xr move right 0.2 0.1 -0.6 30 0 0 0.25 linear
    sleep 0.15
    xr input right trigger/value 0
    sleep 0.4
    shot throw left
    zoom throw-hud throw 2 left
