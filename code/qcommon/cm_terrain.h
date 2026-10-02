/*
===========================================================================
cm_terrain.h: heightmap terrain in the collision model (phase 7).

The world model (clip handle 0) collides with every terrain of the map's
OAX_TERRAIN lump as if each terrain triangle were a Q3 brush: a prism from
the triangle down to the terrain's bottom, with axial and edge bevels, swept
by CM_TraceThroughBrush itself. Traces, position tests and point contents
therefore follow the same rules (and epsilons) as brushes, on every build.

Collidable foliage (trees with a collide radius) adds an axial box per
trunk.

Consumers outside collision (navigation, physics) read the same data
through the accessors below. A Box3D height field takes the heights
directly: CM_OAXTerrainHeights() is samplesX * samplesY world z values, row
major (index j * samplesX + i), sample (i, j) at origin + (i, j) * cellSize,
with the triangle split documented in oax_terrain.h.
===========================================================================
*/
#ifndef CM_TERRAIN_H
#define CM_TERRAIN_H

#include "oax_terrain.h"

void	CM_OAXTerrainLoad( const void *bsp, int bspLen );
void	CM_OAXTerrainClear( void );

int		CM_OAXNumTerrains( void );
const oaxTerrainInfo_t *CM_OAXTerrainInfo( int n );
const float *CM_OAXTerrainHeights( int n );
void	CM_OAXTerrainBounds( int n, vec3_t mins, vec3_t maxs );

// collidable foliage trunks (axial boxes)
int		CM_OAXNumTrunks( void );
void	CM_OAXTrunkBounds( int n, vec3_t mins, vec3_t maxs );

// hash of every terrain height and trunk box (tests compare it across builds)
unsigned CM_OAXTerrainHash( void );

#endif
