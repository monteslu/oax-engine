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
nav_oax.h: the C interface of the Recast/Detour navmesh (nav_oax.cpp).

Coordinates are Q3 world coordinates (z up); nav_oax.cpp maps them to
Recast's y-up space as (x, z, y). One navmesh per map, built from the
triangles cm_navgeom.c collects.
===========================================================================
*/
#ifndef NAV_OAX_H
#define NAV_OAX_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	float	cellSize;		// voxel size across
	float	cellHeight;		// voxel size up
	float	agentHeight;	// standing player
	float	agentRadius;
	float	agentClimb;		// step height
	float	bmin[3], bmax[3];	// Q3 bounds of the build
} oaxNavParams_t;

#define OAXNAV_PATH_PARTIAL	1	// the goal was not reachable: the path ends nearest to it

// Polygon and off-mesh link flags (Detour poly flags). Walkable polygons
// carry WALK, those inside a hazard volume WALK | HAZARD. Off-mesh links
// carry their kind; kinds below OAXNAV_RULE_KINDS are always usable, the
// others only when the caller includes them (a server rule allows them).
// The same numbers are documented in qcommon/oax.h (G_OAX_NAV_*).
#define OAXNAV_FLAG_WALK		0x0001
#define OAXNAV_FLAG_HAZARD		0x0002
#define OAXNAV_LINK_TELEPORT	0x0010
#define OAXNAV_LINK_JUMPPAD		0x0020
#define OAXNAV_LINK_LADDER		0x0040
#define OAXNAV_LINK_JUMP		0x0080
#define OAXNAV_LINK_DROP		0x0100
#define OAXNAV_LINK_TRANSLOCATOR	0x1000
#define OAXNAV_RULE_KINDS		0x1000	// this bit and above: rule-gated
#define OAXNAV_DEFAULT_INCLUDE	( OAXNAV_FLAG_WALK | OAXNAV_FLAG_HAZARD | OAXNAV_LINK_TELEPORT | OAXNAV_LINK_JUMPPAD | \
								  OAXNAV_LINK_LADDER | OAXNAV_LINK_JUMP | OAXNAV_LINK_DROP )

// An off-mesh link from authored intent: start and end on the floor (feet).
typedef struct {
	float			start[3], end[3];
	float			radius;		// how far from a polygon an end may be (across)
	int				bidir;
	unsigned short	kind;		// one OAXNAV_LINK_*
} oaxNavLink_t;

// A volume whose walkable surface costs more to cross (hazards). cost < 0
// removes it from the mesh entirely (a deadly volume, like bspc's lava).
typedef struct {
	float	mins[3], maxs[3];
	float	cost;
} oaxNavArea_t;

// Builds the navmesh; returns its polygon count (0 on failure, with a reason
// in err). walkable[i] marks triangle i walkable.
int		OAXNav_Build( const float *verts, int numVerts, const int *tris, const unsigned char *walkable,
			int numTris, const oaxNavParams_t *params, char *err, int errSize );
// Solid volumes (convex brushes) besides the triangles. The triangles alone
// leave every face buried in solid (inside overlapping or stacked brushes)
// as a floor in mid-air; filling each volume's column span merges them
// away, and the span keeps the area of the face that caps it. Plane i of a
// volume: normal planes[i*4..+2], dist planes[i*4+3], inside where
// n.p <= d; planeTop[i] says whether it is a walkable cap.
typedef struct {
	int					numVolumes;
	const int			*volFirstPlane;
	const int			*volNumPlanes;
	const float			*volBounds;		// 6 per volume: Q3 mins, maxs
	const float			*planes;
	const unsigned char	*planeTop;
	// 0 when the Q3 point is in opaque space (structural solid, the void):
	// a walkable top whose air is opaque is unreachable and is dropped
	int					( *openAt )( const float p[3] );
	// Q3 points walkers start from (spawn points): only tiles reachable
	// from these are built. None: every tile with walkable geometry.
	const float			*seeds;
	int					numSeeds;
} oaxNavSolids_t;

// The same with off-mesh links, cost volumes and solid volumes (any may be
// empty / NULL). Large maps are built in tiles; only tiles that hold
// walkable geometry are built.
int		OAXNav_BuildEx( const float *verts, int numVerts, const int *tris, const unsigned char *walkable,
			int numTris, const oaxNavParams_t *params, const oaxNavLink_t *links, int numLinks,
			const oaxNavArea_t *areas, int numAreas, const oaxNavSolids_t *solids, char *err, int errSize );
// Build statistics of the last build: tiles built, tiles in the grid,
// spans merged into solid volumes, walkable tops dropped as opaque
void	OAXNav_BuildStats( int *tiles, int *gridTiles, int *solidColumns, int *opaqueTops );
// walkable triangles the last build dropped as inside a removing (deadly) volume
int		OAXNav_ForbiddenTris( void );
// wall-clock profile of the last build, for the log only (never game state), and its Q3 bounds
void	OAXNav_BuildProfile( int *msRaster, int *msFill, int *msRest, float *bounds );
int		OAXNav_LinkCount( void );		// links that reached the mesh at both ends
int		OAXNav_LinkConnected( int index );	// did link index reach the mesh at both ends
void	OAXNav_Free( void );
int		OAXNav_PolyCount( void );
unsigned OAXNav_Hash( void );		// FNV-1a of the Detour navmesh data
int		OAXNav_DataSize( void );

// Straight path (corner points) from start to goal; returns the number of
// points written (start first, goal or the nearest reachable point last),
// 0 if either end is off the mesh. *flags gets OAXNAV_PATH_*.
int		OAXNav_FindPath( const float start[3], const float goal[3], const float halfExtents[3],
			float *points, int maxPoints, int *flags );
// The same with explicit filter flags; links[i] (when not NULL) gets the
// index of the off-mesh link that starts at point i (the next point is its
// end), or -1.
int		OAXNav_FindPathEx( const float start[3], const float goal[3], const float halfExtents[3],
			float *points, int *links, int maxPoints, int *flags, int include, int exclude );
// Nearest point on the mesh within halfExtents; returns 1 if found.
int		OAXNav_Nearest( const float p[3], const float halfExtents[3], float out[3] );
// A deterministic random point on the mesh (area weighted) from seed.
int		OAXNav_RandomPoint( unsigned seed, float out[3] );

#ifdef __cplusplus
}
#endif

#endif
