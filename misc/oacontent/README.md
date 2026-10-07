# oacontent: tooling for enhancing OpenArena's content

Phase 0 of internal-oa/OA_CONTENT_ENHANCEMENT_PLAN.md. Dependency-free
Node ESM (ImageMagick for pixel work). Run everything through the driver:

    node misc/oacontent/oacontent.mjs <command> [options]

| command | tool | what it does |
| --- | --- | --- |
| `audit` | T1 | inventory of maps, shaders, textures and models of an install |
| `skyexposure` | T10 | how much of each map's floor sees the sky, and the sun direction |
| `lights` | T3 | recovers light sources for maps and writes `.oaxmap` sidecars |
| `materials` | T4 | classifies shaders and writes the material overlay shader file |
| `textures` | T5 | generated normal/specular maps, upscale hook, texture manifest |
| `smooth` | T7 | smoothing and tessellation settings, faceted-curve report |
| `tour` | T2 | camera tours, renders, metrics, contact sheets (the baseline) |
| `pack` | T8 | builds and validates the overlay pk3, load tests, licence manifest |
| `safety` | T9 | gameplay identity, bot smoke and performance checks |
| `fxpreview` | T6 | effect preview map and per-effect frames |

Libraries are in `lib/` (zip, bsp, shader, md3, image, entities, packs).
Tests: `node --test misc/oacontent/test/`.

The engine side (the `maps/<name>.oaxmap` sidecar, `com_oaxEnhanced`) is
documented in docs/map-format.md ("Map overlay").
