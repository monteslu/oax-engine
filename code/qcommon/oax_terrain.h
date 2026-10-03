/*
===========================================================================
oax engine
Copyright (C) 2026 Luis Montes

This file is part of the oax engine, a fork of ioquake3.
It is free software; you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version. The combined engine is distributed under GPLv3 (see
COPYING-GPLv3.txt).

This program is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
more details.
===========================================================================
*/

/*
===========================================================================
oax_terrain.h: heightmap terrain shared by collision, rendering and
navigation (phase 7, outdoor maps).

A map places terrain with a `misc_oax_terrain` entity; the map build step
(tests/maps/build.mjs, misc/tools/oax-terrain.mjs) bakes the entity's
heightmap, splat map and foliage density map into the BSPX lump
OAX_TERRAIN. The engine reads only the lump, so collision (cm_terrain.c),
the renderer (renderergl2/tr_terrain.c) and the navmesh builder all see
exactly the same samples.

Lump layout (little-endian, every offset 4-byte aligned):

  oaxTerrainLumpHeader_t   "OTRN", version 1, numTerrains
  numTerrains x {
    oaxTerrainDisk_t       placement, layers, foliage, array offsets
    uint16 heights[samplesY][samplesX]
    byte   splat[samplesY][samplesX][4]     layer weights 0..255
    byte   density[samplesY][samplesX][4]   foliage density channels
  }

Sample (i, j) sits at world ( origin[0] + i * cellSize, origin[1] + j *
cellSize, origin[2] + height * heightScale ). Cell (i, j) is split along its
(i,j)-(i+1,j+1) diagonal into triangles {(i,j),(i+1,j),(i+1,j+1)} and
{(i,j),(i+1,j+1),(i,j+1)}, counter-clockwise seen from above. The terrain is
solid from that surface down to `bottom`.

Foliage is placed deterministically from the density map and the seed
(OAXTerrain_CellFoliage), so the renderer, collision (tree trunks) and the
navmesh agree on every instance without storing them.

This file is compiled into the engine and into the renderer module: no
engine allocation, no host libm, only exact float operations
(-ffp-contract=off), so every build computes the same values.
===========================================================================
*/
#ifndef OAX_TERRAIN_H
#define OAX_TERRAIN_H

#define OAX_TERRAIN_LUMP			"OAX_TERRAIN"
#define OAX_TERRAIN_IDENT			"OTRN"
#define OAX_TERRAIN_VERSION			1
#define OAX_TERRAIN_MAX				4
#define OAX_TERRAIN_MAX_LAYERS		4
#define OAX_TERRAIN_MAX_FOLIAGE		4
#define OAX_TERRAIN_MAX_SAMPLES		1025	// per side
#define OAX_TERRAIN_NAME			64

// foliage kinds
#define OAX_FOLIAGE_GRASS			0		// crossed alpha-tested blades, no collision
#define OAX_FOLIAGE_TREE			1		// trunk + canopy mesh; may collide (collideRadius)

// at most this many instances of one foliage type in one cell
#define OAX_FOLIAGE_MAX_PER_CELL	16

typedef struct {
	char	ident[4];
	int		version;
	int		numTerrains;
	int		reserved;
} oaxTerrainLumpHeader_t;

typedef struct {
	int		kind;					// OAX_FOLIAGE_*
	char	shader[OAX_TERRAIN_NAME];	// texture / shader for the instances
	int		channel;				// density map channel 0..3
	float	density;				// expected instances per cell at channel 255
	float	sizeMin, sizeMax;		// instance scale (grass: blade height; tree: total height)
	float	fadeStart, fadeEnd;		// distance fade (renderer)
	float	collideRadius;			// > 0: a solid square trunk of this half-width
	float	collideHeight;			// trunk height above the ground
	float	maxSlope;				// no instance where the surface normal z is below this
	float	reserved[3];
} oaxFoliageDisk_t;

typedef struct {
	int		size;					// bytes of this record, arrays included
	float	origin[3];
	float	cellSize;
	int		samplesX, samplesY;
	float	heightScale;
	float	bottom;
	int		contents;				// collision contents (CONTENTS_SOLID)
	int		surfaceFlags;			// trace surfaceFlags (footsteps)
	int		numLayers;
	char	layerShader[OAX_TERRAIN_MAX_LAYERS][OAX_TERRAIN_NAME];
	float	layerTexScale[OAX_TERRAIN_MAX_LAYERS];	// world units per texture repeat
	int		foliageSeed;
	int		numFoliage;
	oaxFoliageDisk_t foliage[OAX_TERRAIN_MAX_FOLIAGE];
	int		heightsOfs, splatOfs, densityOfs;	// from the start of this record
	int		reserved[4];
} oaxTerrainDisk_t;

// a parsed terrain: the header fields plus pointers into the lump
typedef struct {
	float	origin[3];
	float	cellSize;
	int		samplesX, samplesY;
	float	heightScale;
	float	bottom;
	int		contents, surfaceFlags;
	int		numLayers;
	char	layerShader[OAX_TERRAIN_MAX_LAYERS][OAX_TERRAIN_NAME];
	float	layerTexScale[OAX_TERRAIN_MAX_LAYERS];
	int		foliageSeed;
	int		numFoliage;
	oaxFoliageDisk_t foliage[OAX_TERRAIN_MAX_FOLIAGE];
	const unsigned char	*heights;	// samplesX * samplesY little-endian uint16
	const unsigned char	*splat;		// samplesX * samplesY * 4
	const unsigned char	*density;	// samplesX * samplesY * 4
} oaxTerrainInfo_t;

// one foliage instance
typedef struct {
	float	origin[3];		// on the collision surface
	float	yaw;			// degrees
	float	scale;			// sizeMin..sizeMax
	float	normal[3];		// surface normal under it
} oaxFoliageInstance_t;

// Finds the OAX_TERRAIN lump in a whole BSP file in memory; NULL if absent.
const void *OAXTerrain_FindLump( const void *bsp, int bspLen, int *outLen );

// Parses a lump; returns the number of terrains (0 on any error, with a
// reason in err when given).
int OAXTerrain_Parse( const void *lump, int len, oaxTerrainInfo_t *out, int maxOut, char *err, int errSize );

// world z of sample (i, j), clamped to the grid
float OAXTerrain_SampleZ( const oaxTerrainInfo_t *t, int i, int j );

// Surface height and normal at a world xy inside the grid; returns 0 outside.
// Uses the same triangles as collision.
int OAXTerrain_HeightAt( const oaxTerrainInfo_t *t, float x, float y, float *z, float normal[3] );

// The world corners of a cell's two triangles: tri[0] and tri[1], each three
// points, counter-clockwise seen from above.
void OAXTerrain_CellTriangles( const oaxTerrainInfo_t *t, int i, int j, float tri[2][3][3] );

// The deterministic foliage instances of type f in cell (i, j); returns the
// count (<= OAX_FOLIAGE_MAX_PER_CELL).
int OAXTerrain_CellFoliage( const oaxTerrainInfo_t *t, int f, int i, int j, oaxFoliageInstance_t *out );

// world bounds of a terrain (surface max to bottom)
void OAXTerrain_Bounds( const oaxTerrainInfo_t *t, float mins[3], float maxs[3] );

#endif
