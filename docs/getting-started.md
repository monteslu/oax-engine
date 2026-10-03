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
      +set sv_pure 0 +set vm_game 1 +set vm_cgame 1 +set vm_ui 1

`com_basegame baseoa` makes the engine run OpenArena (its built-in default is
Quake 3's `baseq3`). `sv_pure 0` lets the loose pk3 in your home folder load,
and the `vm_*` settings run the game code as QVMs. The pk3's name starts with
`zzz` so it loads after OpenArena's own game code and replaces it.

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
| `oax_showcase` | every map feature in one place: surfaces, lights, zones, teleporters, navigation links, GUIs, scripts | |
| `oax_outdoor_ctf` | a heightmap valley with two forts, foliage, sun shadows; navmesh bots play CTF | `g_gametype 4` |
| `oax_outdoor_vctf` | the same valley with buggies and hover craft | `g_gametype 4`, `g_oaxVehicles 1` |
| `oax_scripted` | movers, triggers and an in-world GUI driven by a map script | |
| `oax_unified` | unified dynamic lighting and shadows | |
| `oax_fx` | particles, soft particles, decals, trails, water, bloom | |
| `oax_phys` | physics debris and ragdolls (fire a rocket) | |
| `oax_gui`, `oax_movers`, `oax_portal`, `oax_zones`, `oax_skyportal`, `oax_warp`, `oax_terrain` | one feature each | |

Building them needs Node.js 20 or later and two map compilers on your `PATH`
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
      +set sv_pure 0 +set vm_game 1 +set vm_cgame 1 +set vm_ui 1 \
      +set g_gametype 4 +set g_oaxVehicles 1 +set bot_enable 1 +map oax_outdoor_vctf

then `addbot Sarge 3 blue` and so on in the console. Team games start you as
a spectator: `team red` (or ESC and the JOIN menu) puts you in, and on the
vehicle map the use-item key (`+button2`, Enter by default) enters and leaves
a vehicle.

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
