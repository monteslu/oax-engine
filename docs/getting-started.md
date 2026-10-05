# Building and playing oax

oax is three pieces, built separately:

| Piece | Repo | What it gives you |
| --- | --- | --- |
| the engine | [oax-engine](https://github.com/monteslu/oax-engine) (branch `next`) | the client and dedicated server binaries |
| the game code | [oax-gamecode](https://github.com/monteslu/oax-gamecode) (branch `oax`) | the game modules (QVMs) with the oax features |
| game data | [OpenArena 0.8.8](https://openarena.ws) | maps, models, sounds, textures |

The engine runs stock OpenArena as it is. The oax features (navmesh bots,
vehicles, physics effects, map scripts, GUIs and so on) need the oax game
code, and the oax test maps show them off.

## 1. Build the engine

Linux (Debian/Ubuntu names; other distros have the same packages):

    sudo apt install build-essential cmake libsdl2-dev
    git clone -b next https://github.com/monteslu/oax-engine.git
    cd oax-engine
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j

The client is `build/Release/ioquake3` (the binary keeps ioquake3's name for
now) and the dedicated server `build/Release/ioq3ded`.

macOS (11 or later, Apple Silicon or Intel): install the Xcode command line
tools (`xcode-select --install`) and CMake (`brew install cmake`), then the
same `git clone` and `cmake` commands. SDL comes with the source tree. The
client is the app bundle `build/Release/ioquake3.app`; the commands below run
`build/Release/ioquake3.app/Contents/MacOS/ioquake3`.

The build needs a C and a C++ compiler (parts of the engine are C++).

## 2. Build the game code

    git clone -b oax https://github.com/monteslu/oax-gamecode.git
    cd oax-gamecode
    make
    make pk3

See the oax-gamecode README for details. The result is
`build/release-<os>-<arch>/oax/zzz-oax-game.pk3`: the QVMs plus the data the
oax game modules ship with (scripts, particle definitions, a model).

## 3. Get OpenArena's data

Linux: `sudo apt install openarena-data` puts it in
`/usr/share/games/openarena/baseoa`. Anywhere else (and on macOS): download
OpenArena 0.8.8 from openarena.ws and unpack it; you need its `baseoa`
folder.

Below, `OA` is the folder that CONTAINS `baseoa` (for the Debian package,
`/usr/share/games/openarena`) and `HOME_DIR` is a folder of your choice for
settings, logs and the oax files, for example `~/oax-home`.

## 4. Install the game code and run

    mkdir -p HOME_DIR/baseoa
    cp oax-gamecode/build/release-*/oax/zzz-oax-game.pk3 HOME_DIR/baseoa/

    ./build/Release/ioquake3 \
      +set fs_basepath OA +set com_basegame baseoa +set fs_homepath HOME_DIR \
      +set sv_pure 0

`com_basegame baseoa` makes the engine run OpenArena (its built-in default is
Quake 3's `baseq3`). `sv_pure 0` lets the loose pk3 in your home folder load.
The game code runs as QVMs, compiled to native code where the platform has a
compiler (`vm_game`, `vm_cgame`, `vm_ui` 2; 1 runs the slower interpreter).
The pk3's name starts with `zzz` so it loads after OpenArena's own game code
and replaces it.

To check the oax game code is the one running: the console log shows
`File "vm/qagame.qvm" found in ".../zzz-oax-game.pk3"`, and once a map is
loaded `debugvalues` lists `g_oax 1` and `cg_oax 1`, which only the oax game
modules publish. `oax_features` lists what the engine itself supports.

The log also warns that `zzz-oax-game.pk3` and OpenArena's own pk3s "supply
different bytes" for the QVMs: that is the engine's content-isolation report
noting the replacement, and it is expected.

## 5. The oax test maps

The engine repo builds a set of small maps that exercise each feature
(`tests/maps/src/*.mjs`, written with the engine's own JavaScript map
tooling). Worth playing:

| Map | What is in it | Settings |
| --- | --- | --- |
| `oax_canyon` | a large canyon: heightmap terrain with rock walls, trees and grass that move in the wind, shallow pools with reflections and caustics, height fog, cloud shadows, colour grading; an armoured carrier and hover tanks | `g_gametype 4`, `g_oaxVehicles 1` |
| `oax_showcase` | every map feature in one place: surfaces, lights, zones, teleporters, navigation links, GUIs, scripts | |
| `oax_outdoor_ctf` | a heightmap valley with two forts, foliage, sun shadows; navmesh bots play CTF | `g_gametype 4` |
| `oax_outdoor_vctf` | the same valley with buggies and hover craft | `g_gametype 4`, `g_oaxVehicles 1` |
| `oax_assault` | Assault: one team attacks a walled fortress against the clock (destroy the gate generator, take the keep controls, kill the golden cow), then the teams swap; bots attack and defend | `g_gametype 14`, optionally `g_oaxVehicles 1` |
| `oax_scripted` | movers, triggers and an in-world GUI driven by a map script | |
| `oax_unified` | unified dynamic lighting and shadows | |
| `oax_fx` | particles, soft particles, decals, trails, water, bloom | |
| `oax_phys` | physics debris and ragdolls (fire a rocket) | |
| `oax_gui`, `oax_movers`, `oax_portal`, `oax_zones`, `oax_skyportal`, `oax_warp`, `oax_terrain` | one feature each | |

**Prebuilt:** download the latest `oax-testmaps-*.zip` from the
[releases](https://github.com/monteslu/oax-engine/releases) (tagged
`testmaps-<commit>`), unzip it and copy `oax-testmaps/baseoa/.` into
`HOME_DIR/baseoa/`. Then skip to starting a map below.

**From source:** building them needs Node.js 20 or later and two map compilers on your `PATH`
(or named by the `Q3MAP2` and `BSPC` environment variables):

- `q3map2` from [NetRadiant](https://gitlab.com/xonotic/netradiant) (the maps
  are built with 2.5.17), built with `-DBUILD_RADIANT=OFF` and
  `make q3map2`;
- `bspc` for bot navigation: `mbspc` from
  [netradiant-custom](https://github.com/Garux/netradiant-custom)
  (`tools/mbspc`). Other bspc builds can write navigation files the engine
  rejects as out of date.

Then, with OpenArena's data installed (or `OA_BASEOA` pointing at `baseoa`):

    node tests/maps/build.mjs
    cp -R tests/maps/out/baseoa/. HOME_DIR/baseoa/

and start a map with bots, for example:

    ./build/Release/ioquake3 \
      +set fs_basepath OA +set com_basegame baseoa +set fs_homepath HOME_DIR \
      +set sv_pure 0 +set g_gametype 4 +set g_oaxVehicles 1 +set bot_enable 1 \
      +map oax_canyon

then `addbot Sarge 3 blue` and so on in the console. Team games start you as
a spectator: `team red` (or ESC and the JOIN menu) puts you in, and on the
vehicle maps the use-item key (`+button2`: E or Enter on the keyboard, X on
a gamepad) gets into a vehicle with a tap and out with a short hold.

### Controls

Keyboard and mouse work as in OpenArena. A gamepad works out of the box with
an Xbox 360 style layout (SDL maps most pads to it):

| Control | Action |
| --- | --- |
| left stick / right stick | move / look |
| RT / LT | fire / zoom |
| A / B | jump / crouch |
| X | use: holdable items; tap to get into a vehicle, hold to get out |
| Y, RB, d-pad up or right | next weapon |
| LB, d-pad down or left | previous weapon |
| Back / Start | scores / menu |
| left stick click / right stick click | walk / first or third person |

In menus the d-pad or left stick moves, A selects and B goes back.

In a vehicle the buttons follow Halo and Battlefield:

| Control | Action |
| --- | --- |
| left stick | drive: forward throttle, back brakes then reverses, sideways steers |
| right stick | aim the gun, or look around from a seat without one |
| RT / LT | fire the mounted gun / zoom |
| A | switch to the next free seat |
| B | handbrake (on a hover craft, the brake) |
| X | hold to get out |
| right stick click | first or third person |

On the keyboard: WASD drive, the mouse aims, Space switches seats, 1 and 2
pick the driver or gunner seat, crouch is the handbrake, hold E to get out
and V switches the view. A line of these hints shows for a few seconds after
taking a seat (`cg_oaxVehHints 0` hides it).

Each vehicle has a mounted gun fired with RT: the hover tank's
turret cannon and the hover craft's plasma guns belong to the driver, the
heavy machine guns on the buggy and the carrier to the gunner (the second
seat, on the bed or the roof). Whoever has the gun aims it with the right
stick; the camera follows the aim while the vehicle drives on its own, and
the reticle shows where the gun's line meets the world. The machine guns
overheat: the bar above the vehicle panel is the heat (or, for the cannon,
the reload). The driver of a vehicle without the gun looks around with the
right stick and the view swings back behind the vehicle when it is let go.

The right stick click (V) switches between first person and the chase
camera, in any seat, cockpits included. The carrier and the hover tank
have cockpits: their driver sits inside and starts in first person, the
drivers of open vehicles start with the chase camera, and gunners in first
person. On foot it toggles a third-person view. Driving with only one
player, stop, switch to the gunner seat with A and fire, then switch back.

Stick speed and response are cvars (`j_yaw`, `j_pitch`, `j_lookCurve`; see
[cvars.md](cvars.md)).

### Settings and performance

The graphics menu (Setup, System) lists your display's resolutions; the
console command `ui_graphics` opens it directly. OpenArena's configs cap the
frame rate at 85 (`com_maxfps`): set it to your display's refresh rate, or 0
for no cap. `cl_oaxPerfHud 1` shows frame time and CPU and GPU totals on
screen, `cl_oaxPerfHud 2` the GPU time of every pass, and `oaxprof` prints the
same in the console. Antialiasing is `r_ext_framebuffer_multisample` (4 by
default); foliage edges use it too.

The cvars of the engine are listed in [cvars.md](cvars.md); the game code's
in the oax-gamecode repo (`docs/cvars.md`).

## 6. Optional: the cart build and the test suite

The engine also builds as a [wasmcart](https://github.com/wasmcart) cart
(`-DWASMCART=ON` with Emscripten). The romdev test suite
(`tests/romdev/run.mjs`, `misc/ci/gate.sh`) drives that cart and the native
client side by side; it needs Emscripten, a romdev server and the
[oax-engine-testdata](https://github.com/monteslu/oax-engine-testdata)
references next to the engine checkout. None of this is needed to build and
play.
