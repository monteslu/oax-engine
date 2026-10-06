# oax lights: the physical light description

Unified-lighting maps (`oax_lighting unified` or
`hybrid`, see `code/renderergl2/tr_ulight.h`) can describe a light by
measurable properties instead of a projection image and a falloff image.
The terms are evaluated per pixel in the interaction shader
(`interaction_fp.glsl`, `ULIGHT_PHYSICAL`); nothing is baked. Profiles
translate another engine's own light keys into these terms, so an importer
writes the source values and the engine does the conversion.

Stock maps and lights without any of the keys below are untouched: they
take the unified-lighting path they always had, bit for bit.

## The model

For a surface point at distance `d` from the light, with `x = d / radius`:

```
v     = softcap( intensity * falloff(x) )         0 where x >= 1
light = min( v * angular * color, ceiling )       per channel
pixel = texture * light  (+ specular * v * color where the profile has it)
```

- `intensity 1` lights a texture at 1x where the falloff is 1, N.L is 1 and
  the color is 1.
- `color` is NOT normalised: `0.5 0.5 0.5` is half as bright as `1 1 1`,
  and a channel may exceed 1.
- `softcap` is a ceiling with a quadratic knee of half width `k` around the
  cap `c`: `v` below `c - k`, `c` above `c + k`, and
  `v - (v - c + k)^2 / (4k)` between (continuous, with a continuous slope).
  `k = 0` is a hard `min(v, c)`. No cap: `c = 0`.
- `angular` is N.L (Lambert, with the material's normal map) or, for
  `none`, 1 on the side of the surface that faces the light.
- `ceiling` is the worldspawn `oax_overbright` (below). Each light is
  clamped on its own; lights add in the HDR render target.
- Shadows, scissoring, area culling and light shaders (`texture`, whose
  color expressions still multiply the color) work as for any unified
  light. A physical light is always a sphere: `light_target` and friends
  are ignored. Sky faces cast shadows for point and spot lights, not for
  parallel ones (docs/materials.md, "Sky faces as shadow casters").

## Light entity keys (`light`, `rtlight`)

A light is physical when it has any `oax_*` key below or any `ue1_*` key.

| key | value | default |
| --- | --- | --- |
| `oax_profile` | `physical`, `ue1`, `q3`, `doom3` | `ue1` if a `ue1_*` key is present, else `physical` |
| `oax_radius` | radius in world units | the profile's; `physical`: `light_radius` (largest axis), else `light`, else 300 |
| `oax_intensity` | scalar peak (see above) | the profile's; `physical`: 1 |
| `oax_color` | `r g b`, unnormalised | the profile's; `physical`: `1 1 1` |
| `oax_falloff` | `linear` (1 - x), `quadratic` ((1 - x)^2, 16 points), `smooth` (1 - smoothstep(x)), `invsq <units>` ((m / max(x, m))^2 with m = units / radius), `table x y x y ...` (up to 16 points, x ascending, clamped at both ends), `image <path>` (red channel at (x, 0.5), bilinear, clamped) | the profile's; `physical`: `linear` |
| `oax_cap` | ceiling `c` on `v`, 0 = none | the profile's; `physical`: 0 |
| `oax_capKnee` | knee half width `k` | 0 |
| `oax_angular` | `lambert` or `none` | `lambert` (UE1: `none` for `LE_NonIncidence`) |
| `oax_mask` | light-mask groups (16 bits, decimal or 0x hex) | 1 (UE1: 2 for `bSpecialLit`) |
| `oax_effect` | `<table> [rate] [phase] [base] [amp]`: the color is multiplied by `base + amp * table(time * rate + phase)` (time in seconds, rate in table cycles per second); `none` | none (UE1: from `LightType`) |
| `oax_specular` | 0 or 1 | 1; `ue1` and `q3`: 0 (their lightmaps have no specular) |

`_color` still multiplies the color (it is the light's shaderParm 0-2, which
game scripts change with `setColor`). Any `oax_*` key overrides what the
profile derived.

### Profile `ue1` (UE1)

Keys are the Light actor's properties as the UE1 editor writes them, prefixed
`ue1_`: `ue1_LightBrightness` (64), `ue1_LightHue` (0), `ue1_LightSaturation`
(255), `ue1_LightRadius` (64), `ue1_LightType` (`LT_Steady`; a name or its
number), `ue1_LightEffect` (`LE_None`), `ue1_LightPeriod` (32),
`ue1_LightPhase` (0), `ue1_bSpecialLit` (`False`). Defaults are UE1's own
(Engine/Classes/Light.uc).

| term | value | source |
| --- | --- | --- |
| radius | `25 * (LightRadius + 1)` | `Actor::WorldLightRadius()`, UE1 SDK AActor.h |
| falloff | `1 - smoothstep(x)` (`3x^2 - 2x^3`), zero at the radius, no ceiling | measured in linear space, below |
| intensity | `0.01265 * LightBrightness` | measured; fitted with the floor, below |
| color | FGetHSV hue and saturation, unnormalised: hue wheel linear (red 0, green 85, blue 170), saturation blends toward white (255 white); hue 0 is RED | UE1 |
| angular | Lambert; `LE_NonIncidence`: none | |
| mask | 1; `bSpecialLit`: 2 | |
| off | `LT_None`, or brightness 0 | |
| level brightness | worldspawn `ue1_LevelBrightness` b: intensity x b | measured, below |
| per-lamp floor | each lamp's own light less 0.0075 (about one display unit), not below 0, before the lamps add (`r_ulightUE1Floor`) | measured, below |

Measured in linear space (2026-10-02). UE1 test maps were baked with
the UE1 editor's lighting build (driven by an external converter's
calibration shim) and shot by the UE1 reference renderer with its display
chain neutral (Brightness 0.5, GammaOffset 0), so a pixel is texel x light.
Six lamps: four calibration corridors (LightRadius 12 to 64, 325 to
1625 units, brightness 50 and 96) and two halls with a reference map's
deck-lamp radii (LightRadius 200 and 255, 5025 and 6400 units). Every lamp fits
`max(gain * LightBrightness * (1 - smoothstep(d / R)) * N.L - floor, 0)` with R =
WorldLightRadius() exactly (fitted zeros 0.93 to 1.03 R), gain 0.01265 and
floor 0.0075: rms 1.14 grey levels, mean |log| 0.027 over 433 cells
(`misc/tools/ue1-light-calib.mjs`, `tests/romdev/reference/ue1-light-calib.json`;
the gain fitted alone, 0.0123, gave rms 1.29 and |log| 0.030).
A line with a ceiling fits no better (rms 1.27 to 4.6 per lamp against 1.04
to 4.5) and needs two more numbers per lamp.

The per-lamp floor (measured 2026-10-05, an external converter's
calibration maps at a reference Assault map's lamp settings, shot straight
down at a floor 200 below each lamp): far out on its falloff a UE1 lamp
gives about one display unit less than the curve, reaching zero at 0.80 to
0.90 R where the curve still has 1 to 1.4. The loss is per lamp, not on the
sum: two overlapping lamps sum to their two single-lamp profiles in UE1, as
in oax (a floor on the sum would leave the pair a unit above).
The loss is the same at every radius (LightRadius 64, 128 and 255) and
brightness (32 to 255, coloured too), and with LevelInfo ambient. Each
lamp's contribution loses `r_ulightUE1Floor` light units (read when the map
loads), floored at 0, before the lamps add. The gain fitted without the
floor came out low, absorbing the tails' excess: near a lamp oax sat about
4% under UE1 (a near-constant ratio, the reference halls' mid range).
Fitted together on the 433 cells the optimum is gain 0.0126, floor 0.0065
(rms 1.13); the engine takes 0.01265 and 0.0075 (rms 1.14, halls rms 1.04
to 0.67), the floor that also fits the dimmer lamps' tails on the
calibration rig (LightBrightness 64 at LightRadius 64, 120 at 32: tail rms
0.83 to 0.12 display units, paired lamps on UE1's sum).

The profile fitted before (a line to 0.89 R, a ceiling at 115 level / B of
the peak, gain 0.02778, ceiling scaling with LevelInfo Brightness^0.65) was
fitted to shots that had passed the reference OpenGL renderer's display curve (its
default settings: Brightness 1.0 and GammaOffset 0.1, see "Display curve" below): the curve's
compressive top imitated the ceiling. On the linear points it is off by
mean |log| 0.62; `ulight-ue1-calib` keeps it as the control that must
fail. A lamp lights up to `0.01265 * LightBrightness` (1.21 for a 96 lamp),
so a near lamp needs `oax_overbright 2` to show above 1x.

`LightType` time functions, as effects (35 ticks a second): `LT_Pulse`
`0.6 + 0.39 sin`, `LT_SubtlePulse` `0.9 + 0.09 sin`, at
`35 / LightPeriod` cycles a second with `LightPhase / 256` of a cycle
offset; `LT_Blink` dark for the second half of each `35 / (LightPeriod + 1)`
cycle; `LT_Flicker` and `LT_Strobe` from fixed random tables (35 / 64
table cycles a second). These constants are UE1's light manager recalled,
NOT measured: treat them as provisional until a UE1 bake of each type
confirms them. `LT_BackdropLight`, `LT_TexturePalette*` and most spatial
`LightEffect`s (`LE_TorchWaver`, `LE_Spotlight`, ...) have no translation:
the light is steady, and the count of such features shows as `unsupported`
in its `r_ulight_phys_lights` record (below).

Two spatial effects are translated, measured in UE1 (linear-display shots of
UE1 bakes, an external converter's calibration tool, 2026-10-05):

| effect | translation |
| --- | --- |
| `LE_StaticSpot` | a soft cone round the facing direction, on top of the plain falloff: with u = (1 - cos(angle off the axis)) / (1 - cos(edge)), the light is multiplied by `smoothstep(1 - u)`, none past the edge. The edge has 1 - cos(edge) = 0.00342 * `LightCone` (27.2 degrees at `LightCone` 32, 38.1 at 64, about 56 at 128, the shape the same at each). Keys: `ue1_LightCone` (UE1's default 128; 0 reads as 128) and `ue1_direction` "x y z", the facing in map space (the actor's Rotation X axis); without a direction the lamp is lit plain and counted unsupported. |
| `LE_Cylinder` | the lamp's full peak anywhere inside its sphere (radius `25 * (LightRadius + 1)`, the full radius), times Lambert; nothing outside (7.2x and 4.9x a plain lamp at two measured points, against 7.4x and 4.8x predicted). |

### Profile `q3` (q3map2)

Reads `light` / `_light` (300), `_scale`, `_color`, `spawnflags`, `fade`,
with q3map2's constants (light.c, q3map2.h): `photons = light * _scale *
7500`. Lightmap units: 255 is the texture at 1x.

- Inverse square (default): `add = photons * N.L / max(d, 16)^2`; radius
  `sqrt(photons)` (where it falls to 1, q3map2's envelope); falloff
  `invsq 16`; intensity `photons / 16^2 / 255`.
- Linear (`spawnflags 1`): `add = photons / 8000 - max(d, 16) * fade`, no
  angle term (q3map2's Q3 mode); radius `photons / 8000 / fade`.
- `spawnflags 2`: no angle term.
- `_color` is normalised (ColorNormalize: the brightest channel is 1).

### Profile `doom3` (id Tech 4 conventions)

Radius from `light_radius` (largest axis) or `light`; falloff `quadratic`,
the radial counterpart of this engine's default point light (id Tech 4's own
default light images are not used); `_color` as given; Lambert and specular.

### Profile `physical`

No translation: the `oax_*` keys and their defaults above.

## Worldspawn

| key | value | default |
| --- | --- | --- |
| `oax_overbright` | the per-light ceiling of physical lights, 1 to 2 | 1 (off: a light reaches the texture at 1x, like a stock lightmap); 2 for UE1-style 2x lightmaps |
| `ue1_LevelBrightness` | UE1 LevelInfo `Brightness` (1.5 in the reference map): a gain on every ue1 lamp (measured, below) | 1 |

`oax_lighting`, `oax_ambient` and `oax_shadowmode` belong to unified
lighting itself (`tr_ulight.h`).

## Zones (`func_oax_zone`)

| key | value |
| --- | --- |
| `ambient` | `r g b`: the ambient light inside the zone (unified maps), replacing `oax_ambient` |
| `name` | a location name: a player inside reports it (team overlay, `say_team` `#l`) ahead of any `target_location` |
| `color` | 0-7, the name's color (as `target_location`'s `count`) |

A world surface takes the ambient of the zone its centre faces into (a
step off the surface along its normal), so surfaces must be split at zone
boundaries (UE1's are); q3map2 keeps surfaces of different materials apart.
An entity takes the zone at its origin (lighting origin). Where zones
overlap, the highest `priority` wins, then the lowest entity number (the
game's rule). The ambient is static: a zone that starts off (spawnflag 1)
has none, and toggling the zone later does not change it. Zone names use
location configstrings from the top down (`MAX_LOCATIONS - 1 - slot`), so
they never collide with `target_location`s.

## Light-mask groups

A light lights a surface only when their masks share a bit. Bits (16):

| bit | value | meaning |
| --- | --- | --- |
| 0 | 1 | the default group: every surface and light is in it unless it says otherwise |
| 1 | 2 | UE1 SpecialLit: `ue1_bSpecialLit` lights, surfaces with UE1's `PF_SpecialLit` |
| 2-15 | 4 .. 0x8000 | free for maps |

- Material keyword `oaxLightMask <bits>` sets a material's groups (0: in
  no group, lit by no light; the ambient still applies).
- Lights that are not physical are in the default group.
- Per world surface: `R_ULightSetSurfaceMask( surfaceIndex, mask )`
  (`tr_ulight.h`) overrides the material. The surface-world lump
  (`OAX_SURFACES`, docs/map-format.md) carries the same 16 bits per surface; a
  value of 0 there means "the material's groups" (do not call the setter),
  anything else is passed as is.

## Effects and tables

`oax_effect` names any material table (`scripts/*.table`, DOOM-3 syntax,
`tr_matexpr.c`). Built in (a map's table of the same name wins):

| table | values |
| --- | --- |
| `oax_sin` | one cycle of sin, 64 values, interpolated |
| `oax_blink` | `{ 1, 0 }` snapped |
| `oax_flicker` | 64 snapped values: 0 below 0.5, else the value |
| `oax_strobe` | 64 snapped values: 0 or 1 |

Effects run on the shader clock: `r_fixedShaderTime` and
`cl_oaxFreezeTime` pin them.

## Debug values

| value | meaning |
| --- | --- |
| `r_ulight_phys` | `<physical lights> <overbright> <zones with ambient> <ue1_LevelBrightness>` |
| `r_ulight_zones` | `<zones with ambient> <brushes> <planes> <errors>`: zone brushes are read without a limit; a zone, brush or side that cannot be read is a warning and counts in `errors` |
| `r_ulight_phys_lights` | per physical light (the first 12): `ordinal:profile radius intensity cap knee lambert mask r g b effectTable unsupported` (`unsupported`: source features with no translation) |
| `g_location` | the location a team message from the first client would name, or `none` |

## Tests

- `ulight-physical`: every profile, the soft cap, a falloff image, masks,
  zone ambient on and off, an effect at two pinned times and the location
  names, against the JS oracle `tests/romdev/lib/lightoracle.mjs` (written
  from this document), with controls that must fail; cart and native.
- `ulight-ue1-calib`: the UE1 calibration rooms (four small-lamp corridors,
  two big halls) rebuilt with `ue1_*` lamps; the render equals the oracle
  and reproduces the reference linear points (mean |log| at most 0.06 per room set:
  0.041 corridors, 0.021 halls); control: the curve-space profile must
  fail (0.69, 0.55).
- `ulight-ue1-level`: `ue1_LevelBrightness` 1.5 against the oracle; controls
  Brightness 1 and Brightness^0.65 must fail.
- `display-curve`: `r_displayCurve` against the curve and the measured reference shots.

## Measurements: UE1 LevelInfo Brightness

Measured in linear space by re-shooting the calibration corridors baked at
LevelInfo `Brightness` 1, 1.5 and 2, and the big halls at 1 and 1.5, with
the reference renderer's display chain neutral: Brightness is a plain gain. Fitted peaks and
ceilings scale by 1.50 and 2.00; the one profile gain over the corridors
and halls at 1.5 is 1.507x the gain at 1. `ue1_LevelBrightness` multiplies
a ue1 lamp's intensity. (A first measurement through the reference renderer's display curve
read the line at 1.5x but a ceiling rising only by Brightness^0.65; that
was the curve.)

## Frame contract: light 1 is the texture at 1x

In a unified-lighting map, light 1 (ambient 1, or a lamp at its unit)
draws the texture at exactly 1x on screen with the tone map off
(`r_toneMap 0`, every other view cvar at its default). `oax_overbright`
only lets a light exceed 1, up to 2; it does not scale anything.

How: renderergl2 keeps the stock overbright convention. The render target
holds light at `identityLight` (1/2 with the default `r_overBrightBits 1`),
and the final pass multiplies the frame by `2^r_cameraExposure` (2 by
default). Lightmaps and fullbright (`rgbGen identity`) stages already follow
it; the unified ambient and light passes do too, since this step, by
scaling their light by `identityLight` (`tb_ulight.c`). Before that, they
wrote light at 1 and the frame showed 2x with the tone map off.

`ulight-albedo` checks the contract. With no lights and `oax_ambient 1 1 1`,
a brush face, a material tinted with `oaxTint`, an `OAX_SURFACES` quad with
a tint, and a zone with `ambient 0.5` all render at exactly texel x tint
(x ambient), within 0.2 grey levels, with `oax_overbright` 1 or 2 alike, on
the cart and native. Control: with `r_cameraExposure 0` they render at 0.5x.

Settings that change that, outside the light model:
- `r_toneMap 1` (the default, with `r_autoExposure 1`) tone-maps the frame
  with an exposure taken from the scene: the albedo panels render at about
  0.60x. That is not linear and depends on what is in view. UE1 draws
  texture x lightmap with no tone map, so `r_toneMap 0` is UE1's look and
  the like-for-like setting for same-eye comparisons;
- `r_picmip 1` downsizes textures at load with renderergl2's gamma-correct
  mip filter, which brightens high-contrast textures (a noise texture of
  mean 130 rendered at 138, 1.07x). `r_picmip 0`, the engine default, is
  exact.

## Display curve (opt-in): `r_displayCurve`

The reference OpenGL renderer for UE1 puts everything it draws, screenshots included,
through a display curve: with shader brightness B = clamp(2 x Brightness,
0.05, 2.99) and gamma g = 1 / (1 + GammaOffset), on the max channel v,
B > 1 multiplies by (v + (1 - (2v - 1)^2) / 4 (B - 1)) / v (clamped), B < 1
by B, then everything is raised to g. Its default settings (Brightness 1.0,
GammaOffset 0.1) lift mid-tones a lot: texel 41 shows as 84, 180 as 232.

The engine's lighting stays linear. `r_displayCurve 1` applies the curve to
the finished 3D scene (after tone mapping and exposure, before the 2D
overlays), with `r_displayCurveBrightness` (default 1.0) and
`r_displayCurveGammaOffset` (default 0.1), so an imported UE1 map can look
as players of the source engine saw it. Default 0: frames are untouched.
`display-curve` checks it against the curve's oracle (within 0.24 grey) and
the measured reference shots (within 3), with the curve off and the
neutral settings (Brightness 0.5, GammaOffset 0) as controls.
