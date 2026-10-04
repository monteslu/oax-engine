# oax engine console variables

The console variables the oax engine adds to ioquake3, plus the ioquake3 cvars whose default it changes.
Game-side cvars (`g_oax*`, `cg_*`, `bot_oaxNav`) are documented in the oax-gamecode repo's `docs/cvars.md`.
Cvars flagged **cheat** only take a non-default value under `devmap` or with `sv_cheats 1`.
**latch** means the new value takes effect at the next `vid_restart` (renderer) or restart of that subsystem.

## Changed defaults (ioquake3 -> oax)

| Cvar | Default | Flags | What changed |
|---|---|---|---|
| `r_picmip` | 1 -> **0** | archive, latch | Full-resolution textures by default, in both renderers (see [test-hooks.md](test-hooks.md#capture-profile-r_picmip-0-fixed-resolution)). |
| `r_ext_framebuffer_multisample` | 0 -> **4** | archive, latch | 4x MSAA on the opengl2 renderer's framebuffer by default. |
| `r_dlightMode` | 0 -> **1** | archive, latch | Dynamic lights are lit per pixel in the shaders (1) instead of the old additive pass (0); 2 adds shadows, as in ioquake3. |
| `r_vaoCache` | 0 -> **1** | archive | Surfaces without CPU deforms (not sky, not portal) are drawn through the cached vertex array object path instead of being streamed every frame. |
| `pmove_fixed` | 0 -> **1** | systeminfo | Player movement runs in fixed `pmove_msec` steps at any frame rate, so movement is identical across builds and frame rates. The engine now creates it before the game module does. |
| `pmove_msec` | 8 (unchanged) | systeminfo | Step length in ms for `pmove_fixed`; now created by the engine (was game-only). |
| `vm_game`, `vm_cgame`, `vm_ui` | 2 (1 on wasmcart) | archive | QVMs run compiled (2, the JIT) where the build has one; a wasmcart runs the interpreter (1). The tests pin 1 on every build so native and wasm share one execution path. 0 loads a native library. |

## Rendering

All are opengl2-renderer cvars except `r_guiSize` (client).

| Cvar | Default | Flags | What it does |
|---|---|---|---|
| `r_oaxSkyPortal` | 1 | archive | Draw oax sky portals; 0 draws the map's fallback skybox. |
| `r_oaxSkyModelLight` | 1 | cheat | Light sky-portal models that have no light-grid light from worldspawn `oaxSkyAmbient` / `oaxSkyLight` ([materials.md](materials.md#map-keys-for-models-in-sky-areas-worldspawn)); 0 leaves them to the light grid, as stock maps do. |
| `r_oaxParticles` | 1 | archive | Draw oax particle systems (`particles/*.prt`) the cgame adds. |
| `r_oaxSoftParticles` | 12 | archive | Soft particles fade over this many units in front of the geometry behind them; 0 = off. |
| `r_oaxDecals` | 1 | archive | Projected decals from the cgame; 0 = off (the cgame then makes stock marks). |
| `r_oaxTrails` | 1 | archive | Draw ribbon trails the cgame adds. |
| `r_oaxWater` | 1 | archive | `oaxWater` shaders: 1 reflection and refraction, 2 refraction only, 0 the shader's own stages. |
| `r_oaxEnv` | 1 | archive | The outdoor environment a map's worldspawn asks for (wind, foliage alpha to coverage, atmosphere, cloud shadows, colour grading; [materials.md](materials.md#outdoor-environment-worldspawn)); 0 turns all of it off. |
| `r_oaxBloom` | 0 | archive | Bloom in the HDR post-process chain, before tone mapping. |
| `r_oaxBloomThreshold` | 1.0 | archive | Bloom: scene light above this blooms. |
| `r_oaxBloomKnee` | 0.5 | archive | Bloom: soft knee below the threshold, as a fraction of it. |
| `r_oaxBloomIntensity` | 0.6 | archive | Bloom: how much of the blurred light is added back. |
| `r_oaxBloomLevels` | 5 | archive | Bloom pyramid levels, clamped to 1-6; more is wider. |
| `r_displayCurve` | 0 | archive | 1 puts the finished 3D scene through the UE1 brightness + gamma display curve; 0 leaves frames linear ([lights.md](lights.md#display-curve-opt-in-r_displaycurve)). |
| `r_displayCurveBrightness` | 1.0 | archive | Display curve brightness (0.5 is neutral). |
| `r_displayCurveGammaOffset` | 0.1 | archive | Display curve gamma offset (0 is neutral). |
| `r_oaxTerrain` | 1 | cheat | Draw oax heightmap terrain; 0 hides it (collision unaffected). |
| `r_oaxTerrainLodDist` | 1200 | archive | Distance at which terrain chunks drop to the next level of detail (doubling per level); 0 = always full detail. |
| `r_oaxTerrainDebug` | 0 | cheat | Terrain test modes: 1 flat green (crack test), 2 flat green without edge stitching (must crack), 3 terrain and foliage cast no sun shadows, 4 terrain shows the sun shadow mask, 5 = 3 and 4. |
| `r_oaxOcclusion` | 1 | archive | Occlusion queries for terrain chunks and foliage: 0 off, 1 conservative, 2 first results frozen (a deliberately broken test control). |
| `r_oaxOcclusionMargin` | 96 | archive | Units occlusion query boxes are inflated by; results are dropped once the camera moves a quarter of it. |
| `r_oaxShadowOffset` | 2 | cheat | Polygon offset factor (units are twice it) for terrain drawn into sun shadow maps. |
| `r_oaxFoliage` | 1 | archive | Draw instanced terrain foliage. |
| `r_oaxSurfaces` | 1 | cheat, latch | Load a map's surface world (`OAX_SURFACES`, [map-format.md](map-format.md#oax_surfaces-version-1)); 0 loads the map without it for comparisons, from the next map load. |
| `r_ulight` | 1 | cheat | Unified lighting for maps that ask for it (`oax_lighting unified` or `hybrid`, [lights.md](lights.md)); 0 renders them like stock maps. |
| `r_ulightShadows` | 1 | cheat | Shadows of unified lights; 0 is a test control only. |
| `r_ulightShadowMode` | 0 | cheat | 0 = the map's `oax_shadowmode`, 1 = shadow maps, 2 = stencil volumes (falls back to shadow maps without `r_ulightStencil 1`). |
| `r_ulightShadowMapSize` | 512 | archive, latch | Unified-light shadow map size, clamped to 128-2048 (goldens use 512). |
| `r_ulightShadowBias` | 0.004 | cheat | Depth bias of the unified-light shadow map comparison. |
| `r_ulightStencil` | 0 | archive, latch | Give the render target a stencil buffer, needed for stencil shadow volumes. |
| `r_ulightScissor` | 1 | cheat | Scissor each unified light to its screen rectangle; 0 draws it over the whole viewport. |
| `r_ulightAreaCull` | 1 | cheat | Skip static unified lights whose areas are not in the visible area mask; 0 draws them all. |
| `r_ulightSpecular` | 1 | archive | Specular from unified lights; 0 turns it off for every light. |
| `r_ulightDebug` | 0 | cheat | Registered but currently read by nothing (no effect). |
| `r_dlightShadows` | 0 | archive | Under unified lighting, dynamic lights (rockets, muzzle flashes) cast shadows. |
| `r_guiSize` | 512 | archive | Pixel size (clamped 64-2048) of the square texture each in-world GUI draws into; its 640x480 screen is scaled to fit. |

## Sound

| Cvar | Default | Flags | What it does |
|---|---|---|---|
| `snd_occlusion` | -1 | archive | Sounds behind closed area portals play quieter: -1 = on for oax maps only (those with an `OAX_MANIFEST` lump), 0 off, 1 on. DMA mixer only (not OpenAL). |
| `snd_occlusionScale` | 0.2 | archive | Volume factor for occluded sounds. |

## Physics

| Cvar | Default | Flags | What it does |
|---|---|---|---|
| `phys_workers` | 0 | archive | Box3D worker threads for every physics world: 0 = as many as the gamecode asks, otherwise this many (clamped 1-8; wasm builds without thread support use 1). |

## Collision and navigation

| Cvar | Default | Flags | What it does |
|---|---|---|---|
| `cm_noCollisionMeshes` | 0 | cheat | Debug: collision ignores `OAX_COLLISION` meshes. |
| `cm_noTerrain` | 0 | cheat | Debug: collision ignores oax heightmap terrain. |
| `cm_surfGap` | 8 | none | Surface-world load validation: a surface with no hull solid within this many units behind it is reported as floating (values <= 0 use 8). |
| `sv_navmesh` | -1 | none | Bot navigation mesh: -1 = build for maps with terrain or without an `.aas` file, 0 never, 1 always ([navigation.md](navigation.md)). Read at map load. |
| `sv_navCellSize` | 8 | none | Navmesh voxel size across, in world units (values <= 1 use 8). |
| `sv_navLinks` | 1 | none | Build the navmesh with the map's authored links and hazard costs (teleporters, jump pads, ladders, routes); 0 = plain walkable mesh (a test control). |

## Scripting

| Cvar | Default | Flags | What it does |
|---|---|---|---|
| `script_maxInstructions` | 100000 | cheat | Script instructions one thread may run per frame before it is killed as a runaway. |
| `g_debugScript` | 0 | cheat | 1 prints script thread creation and end (with level time and thread name) to the console, and warns when a script waits on a thread that is not running. |
| `g_disasm` | 0 | cheat | 1 prints a disassembly of every compiled script function to the console. |

## Server and simulation

| Cvar | Default | Flags | What it does |
|---|---|---|---|
| `sv_gameSeed` | -1 | none | Fixed random seed passed to the game module at init; -1 = from the clock. For replayable test matches. |

See also `pmove_fixed`, `pmove_msec`, and `vm_*` under Changed defaults.

## Input (gamepad)

Both builds read a gamepad by default with modern twin-stick controls
(`code/client/cl_gamepad.c`):

| Cvar | Default | Flags | What it does |
|---|---|---|---|
| `in_joystick` | 0 -> **1** | archive, latch | The gamepad is read by default. |
| `in_joystickUseAnalog` | 0 -> **1** | archive | Sticks drive analog movement and look instead of key presses. |
| `j_forward` / `j_side` | -0.25 / 0.25 -> **-0.0045 / 0.0045** | archive | Movement in proportion to the left stick (the old values reached full speed at a touch). |
| `j_yaw` / `j_pitch` | -0.022 / 0.022 -> **-0.008 / 0.0055** | archive | Look speed at full tilt: about 260 degrees a second across, 180 up and down (was 720). |
| `j_lookCurve` | **2** (new) | archive | Look response: the stick's deflection to this power, so small movements aim finely; 1 = linear. |
| `in_gamepadBinds` | **1** (new) | archive | At startup, bind the default layout to every pad button the player has not bound; 0 leaves the pad alone. |
| `in_gamepadVersion` | (new) | archive | Configs older than the modern stick values get them once. |

The default layout (Xbox 360 names): left stick move, right stick look,
RT fire, LT zoom, A jump, B crouch, X use (vehicles and holdable items),
Y / RB / d-pad up and right next weapon, LB / d-pad down and left previous
weapon, Back scores, Start menu, left stick click walk, right stick click
centre view. In menus the d-pad and left stick move, A accepts and B goes
back. Weapons have no alternate fire.

`joy_threshold` (0.15) and `in_mouse` (1) keep their ioquake3 defaults. A wasmcart also forces `r_mode -1` with `r_customwidth`/`r_customheight` set to the cart's display size.

## Performance

| Cvar / command | Default | Flags | What it does |
|---|---|---|---|
| `cl_oaxPerfHud` | 0 | archive | Overlay: 1 one line (fps, frame time, CPU stages, GPU total), 2 the full breakdown (GPU time per pass, draw counts). |
| `oaxprof` | | command | Print the breakdown to the console (averages over the last 60 frames). |
| `oaxprof csv <file> [frames]` | | command | One row a frame for the next frames (default 600) into the home's game folder. |
| `r_oaxDirectPost` | 1 | archive | Tone map (with the map's grading and atmosphere) straight to the screen when nothing after needs the image in a texture: saves several full-screen copies. 0 keeps the copy chain (for comparisons). |
| `r_oaxProfile` | 0 | | The renderer's profiler; the overlay and `oaxprof` turn it on while they need it. Debug values `r_prof_*` every 30 frames for tests. |

CPU stages: server, cgame, renderer front end (culling, sorting), back end
(issuing GL calls), and the wait at the end of the frame (glFinish and the
swap). GPU time per pass comes from timer queries (desktop GL 3.3; not on
GLES or the wasmcart): shadow maps, water reflection, depth prepass, sun
shadow mask, terrain, foliage, surfaces, each post-process step (MSAA
resolve, fog and atmosphere, bloom, tone map, grading and the final copy),
2D, and the present. `com_maxfps` caps the frame rate (OpenArena's default
is 85).

## Test hooks (test-only)

Mostly cheat/temp cvars for tests; see [test-hooks.md](test-hooks.md). Not meant for play.

| Cvar | Default | Flags | What it does |
|---|---|---|---|
| `cl_overrideView` | "" | cheat, temp | `"x y z pitch yaw roll"` sets the rendered main view without moving the player (sky portals turn with it). |
| `cl_oaxFreezeTime` | -1 | cheat, temp | Freeze everything animated the client draws at this scene time in ms; -1 = off. |
| `r_fixedShaderTime` | -1 | cheat | Pin the shader clock to this time in seconds; negative = off. |
| `r_oaxViewFog` | "" | cheat | Override the oax view fog with `"r g b density start end"`; empty = the map's fog. |
| `r_oaxSurfaceIdAt` | "" | cheat, temp | `"x y [x y ...]"` window pixels (from the top left): every frame, publish the surface and material under each as debug values `r_surfid<i>`. |
| `r_oaxSurfaceIdDump` | 0 | cheat, temp | Write a whole-frame surface and material id map for the next N frames (`surfids/frame<n>.txt` and the debug blob); counts down to 0. |
| `r_oaxSurfaceIdOpaque` | 0 | cheat, temp | Surface ids: only opaque surfaces take pixels. |
| `s_meter` | 0 | temp | 1 meters the final sound mix and publishes debug value `s_meter` = `peak tail_ms peak_ms`; setting it again restarts the measurement. |
| `com_errorQuit` | (unset) | user-created | 1 makes any error except a plain disconnect end the process (native exits with code 3) instead of returning to the menu. Set with `+set`. |
| `padscript_minstart` | (unset) | user-created | Earliest server level time (ms) at which a pad script may start, so builds that reach the map at different times start it at the same level time. |
| `r_imageprogram_result` | (unset) | written by engine | Output of the `imageprogram` command: `WxH hash` (FNV-1a of the RGBA bytes) or `failed`. |

## Feature probes

| Cvar | Default | Flags | What it does |
|---|---|---|---|
| `oax_version` | 1 | rom | The oax extension API version. |
| `oax_features` | (built at startup) | rom | Space-separated list of the oax extensions this build provides; gamecode probes it before using one. |

Tokens `oax_features` can contain, and who adds them:

| Token(s) | Added by |
|---|---|
| `debug`, `bspx`, `detmath` | engine core (always) |
| `script`, `movers`, `portal`, `zones`, `warp`, `gui`, `ulight`, `physics`, `physics_vehicle`, `physics_vehicle_state`, `ent_obb`, `nav` | server |
| `reverb`, `freeze`, `ulight`, `physics`, `physics_vehicle` | client |
| `skyportal`, `lightstyle`, `proc`, `viewfog`, `physics_skel`, `particles`, `decals`, `trails`, `water`, `bloom` | opengl2 renderer (on load; the opengl1 renderer adds none) |
