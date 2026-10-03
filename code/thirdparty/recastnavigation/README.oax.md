# Recast/Detour (vendored)

Navigation mesh construction (Recast) and path queries (Detour) for oax
bots on maps without AAS coverage (heightmap terrain).

- Upstream: https://github.com/recastnavigation/recastnavigation
- Version: tag v1.6.0, commit 6dc1667f580357e8a2154c28b7867bea7e8ad3a7
- License: zlib (License.txt, kept unmodified). Copyright (c) 2009
  Mikko Mononen. The zlib license is GPLv3-compatible.
- Copied: Recast/Include, Recast/Source, Detour/Include, Detour/Source,
  License.txt. Nothing else (no DetourCrowd, DetourTileCache, demo or tests).

## oax patches

Kept minimal; each patched line is marked "oax patch".

1. `qsort` -> `oaxStableSort` (oax_stablesort.h, a stable merge sort) in
   Recast/Source/RecastContour.cpp (holes, diagonals) and
   Detour/Source/DetourNavMeshBuilder.cpp (BV tree items). qsort is not
   stable, and the native C library and the wasm build's musl order equal
   keys differently, so the two builds would produce different navmeshes.

2. `dtNavMesh::connectFarOffMeshLinks()` (Detour/Include/DetourNavMesh.h,
   Source/DetourNavMesh.cpp): Detour links an off-mesh connection's landing
   only in its start tile and the 8 neighbours; a teleporter across a tiled
   map lands farther. The new method connects those landings (the body of
   connectExtOffMeshLinks for one connection, without the side test); the
   engine calls it once after adding every tile.

Not patched but avoided: rcMarkWalkableTriangles and
rcClearUnwalkableTriangles take the slope as an angle and call host
`cosf`; the engine marks walkable triangles itself with an exact normal-z
threshold (server/sv_nav_oax.cpp). Detour's dtMathCosf/Sinf/Atan2f are not
reached by the queries the engine uses.

Built with -fno-exceptions -fno-rtti -ffp-contract=off (cmake/recast.cmake)
so native and wasm compute the same floats.
