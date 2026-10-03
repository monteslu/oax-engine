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
