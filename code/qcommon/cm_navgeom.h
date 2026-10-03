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
cm_navgeom.h: collision geometry as triangles for the navmesh builder
(cm_navgeom.c). Coordinates are Q3 world coordinates (z up).
===========================================================================
*/
#ifndef CM_NAVGEOM_H
#define CM_NAVGEOM_H

typedef struct {
	float			*verts;		// numVerts * 3
	int				numVerts;
	int				*tris;		// numTris * 3 vertex indexes
	unsigned char	*walkable;	// numTris: 1 if the normal z >= 0.7
	int				numTris;
	vec3_t			mins, maxs;

	// the world brushes as convex volumes (nav_oax.h oaxNavSolids_t)
	int				numVolumes;
	int				*volFirstPlane;
	int				*volNumPlanes;
	float			*volBounds;		// 6 per volume
	int				numPlanes;
	float			*planes;		// 4 per plane: normal, dist
	unsigned char	*planeTop;		// a walkable cap: normal z >= 0.7, not sky, not under terrain, open
	int				closedFaces;	// upward faces with no open air in front (not walkable)
	int				buriedBrushes;	// brushes with no face in open air (no volume)
} oaxNavGeometry_t;

int		CM_OAXNavGeometry( oaxNavGeometry_t *g );
// 0 when p is in opaque space (a leaf with no cluster: structural solid or the void)
int		CM_OAXNavOpenAt( const float p[3] );
void	CM_OAXNavGeometryFree( oaxNavGeometry_t *g );

#endif
