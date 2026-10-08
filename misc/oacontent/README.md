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
| `fxpreview` | T6 | every particle decl rendered on the oax_fx map at fixed frames, with an empty-room control and a contact sheet |
| `fires` | T11 | flame sprites in the maps get a looping flame emitter and a flickering light (sidecar entities) |
| `sky` | T12 | per-map environment keys from the exposure table: bloom, sun disc, and for outdoor maps haze, cloud shadows, grade |
| `pilot` | | every tool in order on three maps, then pack, safety, and a before/after tour |

## Quality gates

- `tour bless --tag t` saves a tour's metrics as a golden (`data/golden/tour.json` is the
  Phase 1 five-map set, `materials.json` the Phase 3 twelve-map set); `tour check --tag t
  --golden file` fails on brightness drift over 12%, draw calls over 1.5x + 50, or a lost frame.
- Lights are calibrated closed-loop against the engine (`lights --calibrate`): a map's
  relit brightness matches its stock brightness, and a shortfall is reported, not hidden.
- The textures tool writes normal and specular companions for the most used textures
  first, within `--budget-mb` (100 MB covers 89% of the (texture, map) uses); the shortlist
  of small, widely used textures for upscaling is `textures/upscale-candidates.json`, and
  only names added to `data/texture-approvals.json` are ever upscaled.
- The material classifier's review items are decided in `data/materials.json` (every
  shader is applied, skipped with a reason, or an override with its why). The sunsky rule
  (q3gl2_sun for outdoor skies, so sun shadows work when `r_sunlightMode` is on) is on; `--no-sunsky` skips it.

## Typical run

    oacontent lights && oacontent smooth && oacontent materials && oacontent textures
    oacontent pack                      # build/pack/zzz-oax-enhanced.pk3 + manifest.json
    oacontent pack --check some.pk3     # validate any pack
    oacontent safety                    # identity, bot smoke, draw budget: stock vs stock + pack
    oacontent tour --tag after --pack build-oacontent/pack/zzz-oax-enhanced.pk3
    oacontent tour compare --a baseline --b after

The game modules come from the sibling oa-gamecode build (`OA_QVM_DIR` to
override); output goes under `build-oacontent/` (`OACONTENT_OUT`).

`pack` merges the sidecar parts of each map (lights, smoothing, and hand edits
in `data/sidecars/<map>.oaxmap`) into one file, and refuses anything outside
`maps/`, `scripts/` and `textures/`. `safety` fails when the pack changes the
navmesh hash, polygon count or item count, when a bot run logs an error, or
when the spawn view exceeds the draw budget; it also proves its identity
check can fail by comparing two different maps.

Libraries are in `lib/` (zip, bsp, shader, md3, image, entities, packs).
Tests: `node --test misc/oacontent/test/lib.test.mjs`.

The engine side (the `maps/<name>.oaxmap` sidecar, `com_oaxEnhanced`) is
documented in docs/map-format.md ("Map overlay").
