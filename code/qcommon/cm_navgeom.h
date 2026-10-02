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
} oaxNavGeometry_t;

int		CM_OAXNavGeometry( oaxNavGeometry_t *g );
void	CM_OAXNavGeometryFree( oaxNavGeometry_t *g );

#endif
