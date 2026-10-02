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

// Builds the navmesh; returns its polygon count (0 on failure, with a reason
// in err). walkable[i] marks triangle i walkable.
int		OAXNav_Build( const float *verts, int numVerts, const int *tris, const unsigned char *walkable,
			int numTris, const oaxNavParams_t *params, char *err, int errSize );
void	OAXNav_Free( void );
int		OAXNav_PolyCount( void );
unsigned OAXNav_Hash( void );		// FNV-1a of the Detour navmesh data
int		OAXNav_DataSize( void );

// Straight path (corner points) from start to goal; returns the number of
// points written (start first, goal or the nearest reachable point last),
// 0 if either end is off the mesh. *flags gets OAXNAV_PATH_*.
int		OAXNav_FindPath( const float start[3], const float goal[3], const float halfExtents[3],
			float *points, int maxPoints, int *flags );
// Nearest point on the mesh within halfExtents; returns 1 if found.
int		OAXNav_Nearest( const float p[3], const float halfExtents[3], float out[3] );
// A deterministic random point on the mesh (area weighted) from seed.
int		OAXNav_RandomPoint( unsigned seed, float out[3] );

#ifdef __cplusplus
}
#endif

#endif
