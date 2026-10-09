# fakexr — a fake OpenXR runtime for testing without a headset

A small OpenXR runtime (OpenXR 1.0 + `XR_KHR_opengl_enable`, the subset the engine uses) that the OpenXR loader
picks up through `XR_RUNTIME_JSON`, so the engine runs its normal, unchanged VR path. Head and controller poses
and buttons come from a control FIFO; on request the runtime composites the submitted layers (the projection
layer plus every quad: menu/HUD panel, hand panels, weapon grid, beams, cinema screen) like a compositor and
writes a PNG with both eyes side by side.

Not simulated: reprojection, lens distortion, timing/performance. Haptics are only counted (and logged with
`FAKEXR_VERBOSE=1`).

## Build

    make            # build/fakexr32 (i386, for the 32-bit engine) and build/fakexr64

## Run

    ./fakexr-run <copy-of-game-root> scenario.txt [xash args...]

Starts `Xvfb` on a free display, the game with `XR_RUNTIME_JSON` pointing here and `-stdincmd`, then plays the
scenario. Use a **copy** of the game tree: the game writes configs and saves into it (the real
`~/Games/lambda1vr-pc` is refused unless `FAKEXR_ALLOW_LIVE=1`). Output (PNGs, `game.log`) goes to
`$FAKEXR_OUT` (default `./fakexr-out`).

Scenario directives, one per line:

| Directive | Meaning |
|---|---|
| `xr <command>` | runtime control command (below) |
| `con <command>` | engine console command |
| `sleep <s>` | wait |
| `shot <name> [both\|left\|right] [layers]` | composited view of the next frame → `<name>.png`; `layers` also writes each overlay layer to `<name>.layerN.png` |
| `wait-log "<regex>" [s]` | wait until the game log matches (default 60 s) |

Runtime control commands (LOCAL space, metres and degrees; +X right, +Y up, -Z forward; pitch + = up,
yaw + = left, roll + = tilt left; the head starts at the origin):

| Command | |
|---|---|
| `head x y z pitch yaw roll` | head pose |
| `aim left\|right x y z pitch yaw roll` | controller aim pose; the grip pose follows (`FAKEXR_GRIP_PITCH`) |
| `grip left\|right x y z pitch yaw roll` | set the grip pose explicitly |
| `track left\|right 0\|1` | controller tracked |
| `input left\|right <component> <v> [<y>]` | input under `/user/hand/<hand>/input/`, e.g. `trigger/value 1`, `thumbstick 0 1`, `a/click 1`, `y/click 1` |
| `shot <file.png> [both\|left\|right]` | as above (the runner adds the path) |
| `exit` | ask the application to end the session |

The runtime presents Touch controllers (`FAKEXR_PROFILE`), like SteamVR does for the PS VR2 Sense controllers.
On the Touch layout Triangle = left `y/click`, Square = left `x/click`, Cross/Circle = right `a/click`/`b/click`,
Options = left `menu/click`.

Other environment variables: `FAKEXR_EYE_SIZE` (960), `FAKEXR_FPS` (90; 0 = unthrottled), `FAKEXR_IPD`
(0.064), `FAKEXR_VERBOSE`.

Example:

    wait-log "OpenXR: initialized" 60
    con sv_cheats 1
    con map c1a0
    wait-log "first stereo frame submitted" 120
    con impulse 101
    xr aim right 0.15 -0.35 -0.4 0 0 0
    xr head 0 0 0 -25 0 0
    sleep 1
    shot hands
