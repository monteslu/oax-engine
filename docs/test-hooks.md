# oax verification hooks

Hooks for tests and for same-eye comparisons against other engines. All of them are cheats (`devmap`), off by default, and a
stock map renders and plays bit-identically while they are off. They work
the same on the native client and the wasmcart.

## Exact placement: `setviewpos x y z yaw pitch [roll]`

The stock four-number `setviewpos x y z yaw` is unchanged: it is a teleport
(origin lifted 1 unit, a 400 ups push along the view, a knockback hold),
which overshoots by tens of units. Give a pitch and it places exactly
instead (oax game module, `g_oax_place.c`):

- the origin is exactly x y z, velocity zero, no push, no knockback hold, no
  teleport effects and no telefrag;
- the view faces yaw / pitch / roll.

Read-back (debug values, see below):

| value | meaning |
| --- | --- |
| `g_place_request` | `client x y z pitch yaw roll` as asked |
| `g_place` | `client x y z pitch yaw roll ground vz`: the player state, every server frame after the placement (after pmove ran); ground entity (1022 world, 1023 none) and vertical speed |
| `g_place_frames` | server frames since the placement |
| `cl_view` | `x y z pitch yaw roll`: the eye the last frame was actually rendered from (after `cl_overrideView`) |
| `cl_view_time` | the scene time that frame was rendered at (ms) |

The angles come back quantized to the usercmd's 16-bit angles, and pmove
clamps the pitch to about 88 degrees. The origin stays put only on a resting
spot: in the air the player falls, inside a floor pmove pushes out. The
read-back shows both, so a test compares from where the shot really was.
To stand exactly on a floor at height `f`, place at `z = f + 24.125`: the
player's mins (24) plus the 0.125 collision epsilon every trace keeps from
a surface (placed at `f + 24`, pmove settles it at `f + 24.125`). The eye
is then `z + viewheight` (26 standing).

`cl_overrideView "x y z pitch yaw roll"` still sets the rendered view
directly without moving the player; `cl_view` reports it too.

## One freeze: `cl_oaxFreezeTime <ms>`

`cl_oaxFreezeTime 1000` freezes everything animated the client draws at
scene time 1000 ms; `-1` (default) is off.

- Engine (`code/client/cl_oax_hooks.c`): every scene the cgame renders gets
  `refdef.time = <ms>`, which pins the renderer's shader clock (animated
  stages, tcMods, deforms, animMaps, procedural textures, material and
  light expressions, water) and the particle and trail clocks.
- oax cgame (`CG_OAXTime`): sky portal rotation, light styles, unified
  light trajectories, mover interpolation (`ET_MOVER`, so `func_oax_mover`
  splines and Q3 doors/plats), item bob and spin.

The per-feature pins still work and win over the freeze for their feature:
`r_fixedShaderTime <seconds>`, `cg_oaxSkyPortalTime <ms>`,
`cg_oaxLightTime <ms>`. Not frozen: the simulation itself (players, bots,
missiles, cosmetic physics bodies keep moving), player model animation,
and decals spawned while frozen (their start time is after the frozen
scene time, so they wait). Freeze screenshots of a scene without moving
actors in view.

## Surface and material ids: `r_oaxSurfaceIdAt`, `r_oaxSurfaceIdDump`

A debug pass (`code/renderergl2/tr_oax_surfid.c`) draws the main world view
a second time into a private RGBA8 target where every surface writes its
draw id, a 24-bit integer held exactly in three 8-bit channels (portable to
ES 3.0 / WebGL, which have no guaranteed integer or float read-back). The
generic vertex program is used, so deforms, vertex and bone animation and
the alpha test of masked stages match the real draw; blending is off and the
nearest surface wins. Particle systems, trails and flares are left out.

- `r_oaxSurfaceIdAt "x y [x y ...]"` (up to 16 window pixels, from the top
  left like a screenshot): every frame while set, publishes
  `r_surfid<i>` = `x y drawId kind index entity shaderIndex shaderName [model]`
  and `r_surfid_frame` = `frame width height draws`.
- `r_oaxSurfaceIdDump <n>`: the next n frames each write a whole-frame map,
  `surfids/frame<frame>.txt` in the home directory; the last one is also the
  debug blob (wasmcart debug field `debug_blob`), so a cart test can read it.
- `r_oaxSurfaceIdOpaque 1`: only opaque surfaces take pixels, so glass,
  additive beams and decals do not hide the material behind them.

`kind`: `none` (nothing drawn: void), `world` (index = world surface,
the BSP draw surface number), `bmodel` (a brush model entity's surface,
index = world surface), `triangles`, `poly`, `model` (an MD3/IQM entity;
the model name follows), `sprite`, `decal`, `other`. `entity` is the
render entity number (-1 for the world). `shaderIndex` / `shaderName` is
the material. Sky pixels name the sky shader.

Dump format (text):

```
oaxsurfids 1
size <w> <h>
frame <n>
surf <drawId> <kind> <index> <entity> <shaderIndex> <shaderName> [model]   (each id present)
row <y> <id>*<count> <id>*<count> ...                                      (top to bottom)
[truncated]
```

The pass only runs while one of the cvars is set; nothing is allocated
otherwise.

## Capture profile: `r_picmip 0`, fixed resolution

`r_picmip` defaults to 0 (full-resolution textures) in both renderers; the
installed OA default of 1 halved every texture and made imports read as
blurry. `r_capture` (debug value, set at renderer start) is
`width height picmip`.

Fixed resolution: native tests pass `r_mode -1` 1280x720; a deterministic
cart run (a test replay) always renders at its 1280x720 default, whatever
size the host prefers.

The test harness (`tests/romdev/lib/capture.mjs`) boots both builds at
`LEGACY_PICMIP` (1) by default, because every golden captured before this
change was taken at picmip 1; a test passes `picmip: null` for the engine
default (as `oax-hooks` does) or a number. On the cart the boot cvars go
through `console_cmd` written before the first frame: the cart turns
`"set a b;set c d"` into `+set` arguments of its command line, so latched
cvars apply at boot.

## Content isolation

Packages (pk3) are searched in name order, a later package's copy of a path
winning. Two hooks keep a map's assets its own (`code/qcommon/files.c`):

- **Duplicate report.** At every filesystem start, each path two packages
  supply with different bytes (zip CRC-32 or size differ) is printed as a
  warning and published: `fs_conflicts` (count), `fs_conflict<i>` =
  `path winner.pk3 loser.pk3` (first 16). Identical copies stay quiet.
  Conflicts inside the game's own numbered packages (`pak0.pk3`,
  `pak6-patch088.pk3`, ...: the official patch chain) only count, as
  `fs_conflicts_official`. `fs_conflicts [substring]` lists them all.
- **Map packages win.** When a map loads, the package that supplies
  `maps/<name>.bsp` becomes the map package if it is a map's own package
  (exactly one `.bsp`, not an official numbered pak). While that map is
  loaded, every file the package holds is read from it before the normal
  search order. `fs_mappack` = `package.pk3 N` (N: files it now supplies
  over a different winning copy), `-` when the map has no package (a loose
  map, or a multi-map content package like OA's `pak1-maps.pk3`).

On the cart, the OA packages are flattened into loose files by
`pack-cart.mjs` (in load order); a `.pk3` in an `--overlay` directory is
kept as a package, so map packages and the report work there too.
