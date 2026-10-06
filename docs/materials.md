# oax material keywords

Shader-script keywords the oax renderer (renderergl2) adds. Stock Q3/OA
shaders never use them, so stock content renders exactly as before. q3map2
ignores them (it may print a warning).

## `oaxNoShadow` (shader level)

The material receives light (light interactions, ambient) but never casts a
unified-lighting shadow: it is left out of every light's shadow casters, in
shadow maps and stencil volumes alike (world surfaces and entities). For
surfaces a source engine never let cast, such as UE1's non-solid surfaces
and its PF_NoShadows ones (Unlit, Invisible, FakeBackdrop). A surface-world
surface asks for the same per surface with `OSF_NOSHADOW`
(docs/map-format.md). Stencil volumes of the world come from BSP brushes,
so only a brush's own shader can keep a brush from casting there.

## Sky faces as shadow casters

A face with `surfaceparm sky` is never lit, but it blocks every point and
spot light the way UE1's FakeBackdrop faces and Q3's sky brushes block
their lamps: a courtyard lamp does not shine through the courtyard's sky
panel onto the lawn beyond. Only a parallel light (the sun) passes through
sky. The face is in each such light's shadow casters (maps and stencil
volumes); its own pixels keep drawing the sky box. `oaxNoShadow` on the
sky material turns this off.

## `oaxTint r g b` (shader level)

Multiplies the colour of the material's surface stages by `r g b` (floats,
values over 1 brighten; the HDR pipeline keeps them). An importer can tint a
stand-in texture toward a source material's measured colour without
generating a wrapper shader per tint.

Which stages: every active stage except a lightmap-only stage (the lightmap
would otherwise be tinted a second time), a detail stage, and an additive one (`blendfunc add`
/ `GL_ONE GL_ONE`, a glow keeps its own colour). After stage collapsing, a
lightmapped diffuse stage is one stage and is tinted once. The tint applies
on the generic and lightall programs and in unified lighting's ambient and
interaction passes.

```
textures/oax_hooks/tint_red
{
	oaxTint 1 0.5 0.5
	{
		map $lightmap
		rgbGen identity
	}
	{
		map textures/base_wall/basewall01.jpg
		blendfunc filter
		rgbGen identity
	}
}
```

## `detailFade <start> <end> [pixel|vertex]` (stage level)

A detail stage seen only up close, as UE1 draws detail textures: at a view
depth up to `start` the stage draws fully, beyond `end` it has no effect,
and in between it fades linearly. View depth is the eye-space z of each
pixel (`1 / gl_FragCoord.w`), as UE1's OpenGL driver fades on depth, so the
fade is exact per pixel on any polygon however large (a per-vertex distance
interpolated across a big surface-world wall would not be).

`vertex` computes the fade as UE1's OpenGL driver does instead: per
vertex, `clamp((end - depth) / (end - start), 0, 1)` from each vertex's view
depth, interpolated across the polygon with no per-pixel recompute. On a
large polygon whose corners are all beyond `end` (a vertex behind the eye
counts by its distance behind, so it is never "near"), nothing shows even
right under the eye, as the UE1 reference renderer draws its big deck floors. On a small polygon
the two modes agree. `pixel` (the default) is the per-pixel fade. The keyword implies `detail` (so
`r_detailTextures 0` still drops the stage). "No effect" is the stage's
blend's neutral value:

| blend | fades toward |
| --- | --- |
| `GL_DST_COLOR GL_SRC_COLOR` (2x modulate) | 0.5 grey |
| `filter` (`GL_DST_COLOR GL_ZERO` or `GL_ZERO GL_SRC_COLOR`) | white |
| `add` (`GL_ONE GL_ONE`) | black |
| anything else | its alpha fades to 0 |

The stage always draws with the generic program (it is never collapsed into
the lightall program), on every path: brush faces, surface-world surfaces
and their shader variants, and after the light passes under unified
lighting. `oaxTint` (and a surface-world tint) never applies to a detail
stage: a tinted modulate would move its neutral value.

```
	{
		map textures/detail/d_conc.tga
		blendfunc gl_dst_color gl_src_color
		tcMod scale 4 4
		detailFade 128 384
	}
```

## `oaxMetal r g b roughness` (shader level)

A metal that reflects its surroundings. The map places reflection probes
(`misc_cubemap` entities, an `origin` and optionally a `radius`, default 1000);
when the map loads, each probe renders the lit world around it into a cube
map (`r_cubemapSize`, 128 by default). A metal surface reflects the probe
nearest to it (an entity's origin, a world surface's centre), added over its
lit colour by unified lighting:

    reflection = probe(R, mip = roughness * blurriest) * F
    F          = F0 + (max(1 - roughness, F0) - F0) * (1 - N.V)^5   (Schlick)
    F0         = (r g b) * the interaction stage's diffuse texture

`r g b` is the metal's reflectance looking straight on (gold about
`1 0.78 0.34`, silver `0.95 0.93 0.88`, copper `0.95 0.64 0.54`), so a
texture can vary it across the surface. `roughness` (0 to 1) picks a
blurrier mip of the probe: 0 a mirror, around 0.3 polished, 0.6 brushed.
The interaction stage still draws the lit colour (the ambient pass and the
lights, specular included), so give a metal a dark diffuse: a metal's look
is mostly its reflection. A normal map in the stage bends the reflection
too.

The probes see the world as it was lit when the map loaded, without
entities, and the reflection uses the stock renderer's parallax correction
(a box of the probe's `radius`). With no probe in the map, or with
`r_oaxReflect 0`, the metal draws without its reflection. The reflection is
a unified-lighting pass (both lighting models); the stock renderer's own
cube mapping (`r_cubeMapping`, off by default) is separate and unchanged.

```
models/oax/cow/gold_white
{
	oaxMetal 1 0.78 0.34 0.25
	{
		map $whiteimage
		rgbGen const ( 0.12 0.09 0.03 )
	}
}
```

## `oaxWater` (shader level)

Draws the shader's surfaces as water: refraction of the scene behind,
a planar reflection, scrolling wave normals and a depth tint. The shader's
stages stay as the fallback (`r_oaxWater 0`, other renderers). Every
`oaxWaterParm` is listed in `code/renderergl2/tr_oax_water.c`; two are
off unless a shader sets them:

- `oaxWaterParm foam <depth> <strength>`: a broken white band where the
  water is shallower than `depth` units (shorelines, anything standing in
  it).
- `oaxWaterParm caustics <strength> <repeats per unit>`: the bottom seen
  through the surface brightens in moving lines where the waves would
  focus sunlight, fading with depth.

## Outdoor environment (worldspawn)

Worldspawn keys for outdoor maps. Each is off unless set, and `r_oaxEnv 0`
turns them all off (`code/renderergl2/tr_oax_env.c`).

| Key | Value | Effect |
| --- | --- | --- |
| `oax_wind` | `<strength> <speed> <yaw>` | Foliage sways in gusts (`speed` Hz) toward `yaw` degrees; a plant top moves `strength * (20 + 0.06 * height)` units in a full gust (0.4 is a fair breeze). |
| `oax_foliageaa` | `1` | Foliage alpha to coverage: cut-out edges antialias under MSAA (`r_ext_framebuffer_multisample`). |
| `oax_atmosphere` | `<r> <g> <b> <density> <falloff> <baseZ> <sunScatter>` | Height fog thicker low down, integrated along each view ray, brighter toward the sun; distant terrain and sky take its colour (aerial perspective). |
| `oax_clouds` | `<scale> <speedX> <speedY> <coverage> <darkness>` | Cloud shadows: scrolling noise (`scale` units per cell) darkens the sun-lit areas; needs sun shadows (`r_sunShadows 1`). |
| `oax_grade` | `<saturation> <contrast> <r> <g> <b> <vignette>` | Colour grading after tone mapping, and darkening toward the corners. |
| `oax_underwaterfog` | `<r> <g> <b> <density>` or `1` | With the eye in water, the view fogs to the water colour (`1`: a green-grey pond); `cg_oaxUnderwaterFog` overrides it. |
| `oax_groundfx` | `1` | The cgame's ground effects (`cg_oaxGroundFx`): rings on the water where players wade, vehicle dust and splashes, tyre tracks. |

## Map keys for models in sky areas (worldspawn)

A model entity drawn in a sky portal scene (the sky room behind a
`misc_oax_skyportal`) whose light grid sample is empty (the grid often
covers only the play area, and a sky room usually has no lights) is lit:

- inside the grid, from the nearest grid cell with light within 8 cells;
- otherwise by `oaxSkyAmbient "r g b"` and `oaxSkyLight "r g b"` (0-255,
  the directed part along the sky shader's sun direction); unset, ambient
  64 and the sun's colour (or 128) directed.

`r_oaxSkyModelLight 0` (cheat) turns this off (the Q3 behaviour: such a
model gets only the minimum light). Models outside sky portal scenes are
lit as before.

Tests: `oax-hooks` (tests/maps/src/oax_hooks.mjs, and oax_hooks_sw.mjs for
detailFade under unified lighting on surface-world surfaces, a tinted
variant and a brush face, including the linear depth curve).
