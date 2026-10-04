# The oax map format

An oax map is a Quake III BSP (`IBSP`, version 46) plus BSPX extension
lumps. A stock OpenArena map is a valid oax map with no extension lumps,
and it loads exactly as it always has. The extension lumps add what the
Q3 format cannot express: render surfaces that need no volume, heightmap
terrain, collision meshes, and a manifest.

This file is the format's specification. The engine reads it
(`code/qcommon/oax_surfaces.h`, `oax_terrain.h`, `bspx.c`); the JS
writers in `misc/tools` write it, and importers (the separate
`oax-mapgen` repo) use those writers, pinned to a version of this file.

All values are little-endian. All coordinates are Quake III world space:
right-handed, z up, units of one Q3 unit. Importers from other engines
convert handedness and scale before writing (UE1 is left-handed: mirror
one axis, and reverse every polygon's winding with it).

## Contents

1. [The BSPX block](#the-bspx-block)
2. [OAX_MANIFEST](#oax_manifest)
3. [OAX_TERRAIN](#oax_terrain)
4. [OAX_SURFACES (version 1)](#oax_surfaces-version-1)
5. [OAX_COLLISION (version 1)](#oax_collision-version-1)
6. [The hull](#the-hull)
7. [Building a map](#building-a-map)
8. [Versioning](#versioning)

## The BSPX block

After the last standard lump, aligned to 4 bytes, the file holds the
magic `BSPX`, an `int32` count, then `count` directory entries:

| Field | Type | Meaning |
| --- | --- | --- |
| name | `char[24]` | lump name, NUL padded |
| ofs | `int32` | offset of the lump data from the start of the file |
| len | `int32` | lump length in bytes |

Lump data follows the directory, each lump padded to 4 bytes. Loaders that
read the 17 standard lumps by offset (the engine, q3map2, bspc) never see
the block. `q3map2 -light` rewrites the file and drops it, so lumps are
added after the last compile stage. Writer: `misc/tools/bspx.mjs`.

## OAX_MANIFEST

UTF-8 JSON, one object: `{ "oax": 1, "map": "<name>", "features": [...] }`
plus any keys the map's build adds. Its presence marks a map as an oax map
(sound occlusion, for one, turns on for oax maps only).

## OAX_TERRAIN

Heightmap terrain: placement, layers, foliage and the sample arrays. The
layout is documented in `code/qcommon/oax_terrain.h`; the writer is
`misc/tools/oax-terrain.mjs`.

The terrain's `flags` turn on optional shading, each from a key on the
terrain entity: `triplanar` (steep faces take side projections of the
layers), `macro` (large patches a little lighter or darker and warmer or
cooler, so a layer's repeat does not read as a grid from afar) and
`detail` (close up, the layers again at a finer repeat modulate the
colour). Projected decals land on the terrain's collision triangles.

## OAX_SURFACES (version 1)

The surface world: render surfaces that need no volume. A surface is a
convex polygon or an indexed triangle mesh with a material, texture
coordinates, an optional tint and flags. Surfaces are drawn by the
renderer as ordinary world surfaces: they sit in the BSP leaves they
occupy, so PVS clusters, areas and area portals (doors) cull them; they
are lit by the unified lighting model (light interactions, shadows) like
any other world surface; and they coexist with ordinary brush faces in
the same map.

Surfaces carry no collision. Collision, visibility and areas come from
the map's hull (see [The hull](#the-hull)), optionally refined by an
[OAX_COLLISION](#oax_collision-version-1) mesh.

Writer: `misc/tools/oax-surfaces.mjs`. Engine: `code/qcommon/oax_surfaces.h`
(parsing, shared by the collision model and the renderer),
`code/renderergl2/tr_surfworld.c` (drawing), `code/qcommon/cm_oaxsurf.c`
(load-time validation).

### Layout

```
header                        80 bytes
materials[numMaterials]       materialSize bytes each (72 in version 1)
surfaces[numSurfaces]         surfaceSize bytes each (112 in version 1)
verts[numVerts]               vertSize bytes each (52 in version 1)
indexes[numIndexes]           int32 each
leafRefs[numLeafRefs]         int32 each
```

Every table starts at the offset the header gives, relative to the start
of the lump, 4-byte aligned. Records are read with the header's stride, so
a later version can append fields to a record without breaking older
readers (see [Versioning](#versioning)).

### Header

| Offset | Field | Type | Meaning |
| --- | --- | --- | --- |
| 0 | ident | `char[4]` | `OSRF` |
| 4 | version | `int32` | 1 |
| 8 | headerSize | `int32` | 80 |
| 12 | flags | `int32` | 0 (reserved) |
| 16 | numMaterials, ofsMaterials, materialSize | 3 x `int32` | material table |
| 28 | numSurfaces, ofsSurfaces, surfaceSize | 3 x `int32` | surface table |
| 40 | numVerts, ofsVerts, vertSize | 3 x `int32` | vertex table |
| 52 | numIndexes, ofsIndexes, indexSize | 3 x `int32` | index table (indexSize 4) |
| 64 | numLeafRefs, ofsLeafRefs, leafRefSize | 3 x `int32` | leaf references (leafRefSize 4) |
| 76 | reserved | `int32` | 0 |

### Material (72 bytes)

| Offset | Field | Type | Meaning |
| --- | --- | --- | --- |
| 0 | name | `char[64]` | shader name as a BSP shader lump names it, NUL terminated (`textures/base_floor/clang_floor`) |
| 64 | flags | `int32` | 0 (reserved) |
| 68 | reserved | `int32` | 0 |

A material resolves at load exactly like a brush face's shader: a shader
script of that name, else an image of that name. The engine resolves
materials at load time; no compiler pass (q3map2's `shaderlist.txt`) gates
them. A material that resolves to neither is drawn with the default shader
and counted in the `r_surfworld_unknown` debug value; the map build fails
on it first (see [Building a map](#building-a-map)).

### Surface (112 bytes)

| Offset | Field | Type | Meaning |
| --- | --- | --- | --- |
| 0 | material | `int32` | index into the material table |
| 4 | flags | `uint32` | `OSF_*`, below |
| 8 | lightMask | `uint32` | light-mask groups this surface is in; 0 means the default group (bit 0) |
| 12 | model | `int32` | 0: the world; n > 0: inline model `*n` (a mover: the surface moves with it) |
| 16 | tint | `float[4]` | r, g, b multiply the material's colour; a is the opacity of a translucent surface |
| 32 | uv | `float[2][4]` | with `OSF_UVMATRIX`: s = uv[0][0]x + uv[0][1]y + uv[0][2]z + uv[0][3], t likewise from uv[1] |
| 64 | plane | `float[4]` | a, b, c, d of the surface plane (ax + by + cz = d) for a polygon; 0 for a mesh |
| 80 | firstVert, numVerts | 2 x `int32` | vertex range |
| 88 | firstIndex, numIndexes | 2 x `int32` | index range; numIndexes 0: the vertices are one convex polygon |
| 96 | firstLeaf, numLeafs | 2 x `int32` | leaf reference range; firstLeaf -1: not bound, the engine finds the leaves |
| 104 | area | `int32` | the area the surface is in; -1 when it spans several or is not bound |
| 108 | sourceId | `int32` | the importer's id for the surface (UE1 surface index, glTF primitive), for reports; -1 none |

A polygon (`numIndexes` 0) is convex and wound counter-clockwise seen from
its front (the side the plane normal points to); the renderer draws it as
a fan from its first vertex. Anything else (concave polygons, curved or
open meshes) is a mesh: triangles of three indexes each, counter-clockwise
seen from the front, indexes relative to `firstVert`.

Flags:

| Bit | Name | Meaning |
| --- | --- | --- |
| 0x001 | `OSF_TWOSIDED` | drawn from both sides (no back-face culling) |
| 0x002 | `OSF_MASKED` | alpha tested: texels with alpha below 0.5 are holes |
| 0x004 | `OSF_TRANSLUCENT` | alpha blended: tint[3] x the texture's alpha of (vertex light x texel x tint) over what is behind (in a unified map the vertex light is the ambient); drawn after opaque surfaces, not lit by light interactions |
| 0x008 | `OSF_ADDITIVE` | adds tint x texel to what is behind it, unlit (glows, flames) |
| 0x010 | `OSF_INVISIBLE` | never drawn (UE1 invisible surfaces, editor-only polygons); kept for tools and validation |
| 0x020 | `OSF_UVMATRIX` | texture coordinates come from the surface's `uv` matrix; otherwise from each vertex's `st` |
| 0x040 | `OSF_NORMALS` | each vertex's `normal` is used; otherwise the plane's normal (polygon) or the triangle normals averaged per vertex (mesh) |
| 0x080 | `OSF_TANGENTS` | each vertex's `tangent` is used; otherwise tangents are computed from the texture coordinates |
| 0x100 | `OSF_COLORS` | each vertex's `color` multiplies the tint |
| 0x200 | `OSF_DETAIL` | free-standing detail: not expected to lie on the hull (validation skips the floating check) |
| 0x400 | `OSF_NOSHADOW` | receives light but casts no unified-lighting shadow (UE1 non-solid and PF_NoShadows surfaces; the material keyword `oaxNoShadow` does the same per material) |

Other bits are reserved and must be 0. Material properties the flags set
(two-sided, masked, translucent, additive, no shadow, tint) apply on top of the
material's own shader, per surface: the same material can be opaque on one
surface and translucent on another.

### Vertex (52 bytes)

| Offset | Field | Type | Meaning |
| --- | --- | --- | --- |
| 0 | xyz | `float[3]` | position |
| 12 | st | `float[2]` | texture coordinates (ignored with `OSF_UVMATRIX`) |
| 20 | normal | `float[3]` | unit normal (used with `OSF_NORMALS`) |
| 32 | tangent | `float[4]` | unit tangent xyz and the bitangent sign w (used with `OSF_TANGENTS`) |
| 48 | color | `uint8[4]` | rgba (used with `OSF_COLORS`) |

### Leaf references

`int32` indexes into the BSP's leaf lump. A surface lists every leaf its
polygon or triangles pass through that is not solid (cluster != -1). The
writer computes them by clipping each polygon down the compiled BSP tree;
a surface lying on a node's plane goes to the side its normal faces (both
sides when two-sided). A surface with `firstLeaf` -1 is not bound: the
engine assigns it every non-solid leaf its bounds touch. A bound surface
with no leaves is buried in solid and is never drawn. Surfaces of an
inline model (`model` > 0) have no leaves; their model is culled as an
entity.

### Drawing (renderer contract)

- Surfaces become world surfaces: planar polygons of up to 64 vertices as
  faces (plane culled), everything else as triangle soups. They are in the
  world's surface list, so every world path applies: PVS and area culling,
  fog, decals (marks), dynamic lights, the unified lighting interactions
  and shadows, the world vertex cache.
- Light-mask groups: a light lights a surface when the light's mask
  (`light_mask` key, default 1) and the surface's `lightMask` (0 is 1)
  share a bit. Brush faces are in group 1.
- A tint is the material's `oaxTint` (docs/materials.md): it multiplies
  every stage that carries the surface colour, in the stock stages and in
  the light interactions alike, so a tinted surface's diffuse colour is
  exactly the tint times the untinted one's (specular light is not
  tinted). An additive surface takes its tint as its colour.
- A fullbright (unlit) material, one whose first stage is opaque with an
  explicit `rgbGen identity` and that has no lightmap stage, takes no
  light interactions: in a unified-lighting map it shows its texture colour
  whatever lights reach it, none included (UE1 Unlit surfaces). A stage
  without `rgbGen` stays lit.
- In a lightmapped map (no `oax_lighting` key) surfaces are vertex lit from
  the map's light grid (the same samples models use), times the tint. In a
  unified-lighting map they take the ambient floor and the light
  interactions like every world surface.
- The `r_oaxSurfaces 0` cvar (cheat, latched to map load) loads the map
  without its surfaces, for comparisons.

## OAX_COLLISION (version 1)

Collision meshes, for sources whose solid space is not a set of convex
brushes (the hull) or whose detail the hull approximates too coarsely.
Each triangle collides as a thin solid slab: the triangle, a copy of it
`thickness` units behind it along its normal, and the side, axial and edge
bevel planes Q3's box sweep needs, swept by the collision model's own
brush code (the same epsilons and rules as brushes). Physics (Box3D) and
the navmesh take the same triangles.

### Layout

```
header                        48 bytes
meshes[numMeshes]             meshSize bytes each (48 in version 1)
verts[numVerts]               vertSize bytes each (12 in version 1)
indexes[numIndexes]           int32 each
```

### Header

| Offset | Field | Type | Meaning |
| --- | --- | --- | --- |
| 0 | ident | `char[4]` | `OCOL` |
| 4 | version | `int32` | 1 |
| 8 | headerSize | `int32` | 48 |
| 12 | flags | `int32` | 0 (reserved) |
| 16 | numMeshes, ofsMeshes, meshSize | 3 x `int32` | mesh table |
| 28 | numVerts, ofsVerts, vertSize | 3 x `int32` | vertex table (`float[3]` positions) |
| 40 | numIndexes, ofsIndexes | 2 x `int32` | index table (`int32`, three per triangle) |

### Mesh (48 bytes)

| Offset | Field | Type | Meaning |
| --- | --- | --- | --- |
| 0 | contents | `int32` | Q3 `CONTENTS_*` (1 solid, 0x10000 player clip, ...) |
| 4 | surfaceFlags | `int32` | Q3 `SURF_*` reported by traces (footsteps, slick, ...) |
| 8 | thickness | `float` | slab depth behind each triangle, > 0 (default 4) |
| 12 | model | `int32` | 0 (world); other values are reserved |
| 16 | firstVert, numVerts | 2 x `int32` | vertex range |
| 24 | firstIndex, numIndexes | 2 x `int32` | triangles, counter-clockwise seen from the outside, indexes relative to `firstVert` |
| 32 | sourceId | `int32` | importer's id, -1 none |
| 36 | reserved | 3 x `int32` | 0 |

The engine reports the meshes it loaded as debug values (`cm_coll_meshes`,
`cm_coll_tris`, `cm_coll_hash`); `cm_noCollisionMeshes 1` (cheat) ignores
the lump.

## The hull

A surface-world map gets collision, PVS, areas and navigation from simple
solids in the BSP itself: brushes textured `common/caulk` (solid,
structural, never drawn) and `common/clip` / `common/playerclip` for
player-only blocking, compiled by q3map2 as usual. The collision model,
Box3D (`PHYS_WORLD_ADD_BSP`), bspc and the navmesh builder read those
brushes unchanged. Area portals and doors work as in any Q3 map: an
`common/areaportal` brush in the doorway and a `func_door` whose own
brushes are caulk; the door's visible surfaces are OAX_SURFACES surfaces
with `model` set to the door's inline model.

The hull and the surfaces are independent: a surface may lie on a hull
face (a floor polygon on top of a caulk slab), float in air (a flame
sheet, `OSF_DETAIL`), or have no hull at all under it (then nothing
collides there). Zones (`func_oax_zone`) are separate trigger volumes:
a hull solid may sit inside a zone (a source's semisolid volumes that
block movement but still carry a zone), and nothing here assumes hull
solids are outside every zone.

### Load-time validation

When the collision model loads a map with OAX_SURFACES it checks every
visible, opaque, one-sided surface against the hull and publishes the
result as debug values:

| Value | Meaning |
| --- | --- |
| `cm_surf_count` | surfaces in the lump |
| `cm_surf_checked` | surfaces checked (visible, opaque, one-sided) |
| `cm_surf_buried` | surfaces whose front side is in solid at the centroid (inside the hull) |
| `cm_surf_floating` | surfaces with no solid within `cm_surfGap` units (default 8) behind the centroid, not `OSF_DETAIL` |
| `cm_surf_buried_ids`, `cm_surf_floating_ids` | the first 16 offenders as `index:sourceId` pairs |
| `cm_surf_hash` | FNV-1a hash of the lump bytes |

## Building a map

`tests/maps/build.mjs` (and importers that follow it) compile with
q3map2 (`-meta`, `-vis`, `-light` unless unified lighting), bspc, then add
the BSPX lumps. The build is strict and fails, rather than exiting 0, on:

- brushes q3map2 dropped (it silently drops brushes past 65536);
- unknown materials: a brush shader or OAX_SURFACES material that is
  neither a shader script nor an image in the game data or the map's own
  files;
- empty meta output: no drawable surface from q3map2 and no OAX_SURFACES
  lump;
- an OAX_SURFACES surface that binds to no leaf (buried in the hull);
- q3map2 errors and leaks (as before).

## Versioning

Each lump starts with an ident and a version. A reader accepts the
versions it knows and rejects others with a warning (the map still loads
without the lump's content). Within a major version, records only grow:
new fields are appended, the header's record size says how large a record
is, and readers use that stride. Changing the meaning of an existing field
needs a new version number. The engine accepts OAX_SURFACES version 1 and
OAX_COLLISION version 1.
