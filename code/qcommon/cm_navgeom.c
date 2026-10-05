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
cm_navgeom.c: the world's collision geometry as triangles, for the
navigation mesh (server/sv_nav_oax.c).

- Every solid or playerclip brush of the world model (not movers,
  triggers or zones: their brushes belong to inline models) as its face
  polygons, cut from the brush planes with cm_polylib. Sky faces are left
  out.
- Every terrain triangle (cm_terrain.c / oax_terrain.h).
- Every collidable foliage trunk as a box.
- Every OAX_COLLISION triangle (cm_oaxsurf.c), as it faces.

Brush faces buried in terrain (a floor under the terrain, say) are left
out, so the navmesh does not grow walkable area under the ground.

A triangle is walkable when its unit normal has z >= MIN_WALK_NORMAL
(0.7, as pmove decides ground). Exact float math only; the same on every
build.
===========================================================================
*/

#include "cm_local.h"
#include "cm_terrain.h"
#include "cm_navgeom.h"
#include <stdlib.h>

#define NAV_MIN_WALK_NORMAL 0.7f

typedef struct {
	oaxNavGeometry_t	*g;
	int					maxVerts, maxTris;
	int					maxVolumes, maxPlanes;
	int					closedFaces;
	int					buriedBrushes;
} navBuild_t;

static void Grow( navBuild_t *b, int verts, int tris ) {
	oaxNavGeometry_t *g = b->g;

	// plain malloc: a large map's geometry outgrows the zone
	if ( g->numVerts + verts > b->maxVerts ) {
		b->maxVerts = ( g->numVerts + verts ) * 2;
		g->verts = realloc( g->verts, b->maxVerts * 3 * sizeof( float ) );
	}
	if ( g->numTris + tris > b->maxTris ) {
		b->maxTris = ( g->numTris + tris ) * 2;
		g->tris = realloc( g->tris, b->maxTris * 3 * sizeof( int ) );
		g->walkable = realloc( g->walkable, b->maxTris );
	}
	if ( ( b->maxVerts && !g->verts ) || ( b->maxTris && ( !g->tris || !g->walkable ) ) ) {
		Com_Error( ERR_DROP, "CM_OAXNavGeometry: out of memory" );
	}
}

static int AddVert( navBuild_t *b, const float *p ) {
	oaxNavGeometry_t *g = b->g;

	Grow( b, 1, 0 );
	g->verts[g->numVerts * 3 + 0] = p[0];
	g->verts[g->numVerts * 3 + 1] = p[1];
	g->verts[g->numVerts * 3 + 2] = p[2];
	return g->numVerts++;
}

static void AddTri( navBuild_t *b, int a, int c, int d, qboolean walkable ) {
	oaxNavGeometry_t *g = b->g;

	Grow( b, 0, 1 );
	g->tris[g->numTris * 3 + 0] = a;
	g->tris[g->numTris * 3 + 1] = c;
	g->tris[g->numTris * 3 + 2] = d;
	g->walkable[g->numTris] = walkable ? 1 : 0;
	g->numTris++;
}

// a convex polygon as a fan; normal z decides walkability
static void AddPolygon( navBuild_t *b, int numPoints, vec3_t *p, float normalZ ) {
	int base, i;

	if ( numPoints < 3 ) {
		return;
	}
	base = b->g->numVerts;
	for ( i = 0; i < numPoints; i++ ) {
		AddVert( b, p[i] );
	}
	for ( i = 2; i < numPoints; i++ ) {
		AddTri( b, base, base + i - 1, base + i, normalZ >= NAV_MIN_WALK_NORMAL );
	}
}

// is a face buried below some terrain's surface?
static qboolean UnderTerrain( int numPoints, vec3_t *p ) {
	vec3_t c;
	int i;

	if ( !CM_OAXNumTerrains() ) {
		return qfalse;
	}
	VectorClear( c );
	for ( i = 0; i < numPoints; i++ ) {
		VectorAdd( c, p[i], c );
	}
	VectorScale( c, 1.0f / numPoints, c );
	return ( CM_OAXTerrainPointContents( c ) & CONTENTS_SOLID ) != 0;
}

/*
Every world brush is also handed over as a convex volume: the navmesh
builder fills each column's span through it, so faces buried inside other
brushes (overlapping brushes, stacked convex cells of an imported hull)
merge into solid instead of standing as floors in mid-air. A plane caps
walkably when its face is walkable (normal z >= 0.7, not sky, not under
terrain).
*/
static void AddVolume( navBuild_t *b, cbrush_t *brush, const unsigned char *top ) {
	oaxNavGeometry_t *g = b->g;
	int i;

	if ( g->numVolumes + 1 > b->maxVolumes ) {
		b->maxVolumes = ( g->numVolumes + 1 ) * 2;
		g->volFirstPlane = realloc( g->volFirstPlane, b->maxVolumes * sizeof( int ) );
		g->volNumPlanes = realloc( g->volNumPlanes, b->maxVolumes * sizeof( int ) );
		g->volBounds = realloc( g->volBounds, b->maxVolumes * 6 * sizeof( float ) );
	}
	if ( g->numPlanes + brush->numsides > b->maxPlanes ) {
		b->maxPlanes = ( g->numPlanes + brush->numsides ) * 2;
		g->planes = realloc( g->planes, b->maxPlanes * 4 * sizeof( float ) );
		g->planeTop = realloc( g->planeTop, b->maxPlanes );
	}
	if ( !g->volFirstPlane || !g->volNumPlanes || !g->volBounds || !g->planes || !g->planeTop ) {
		Com_Error( ERR_DROP, "CM_OAXNavGeometry: out of memory" );
	}
	g->volFirstPlane[g->numVolumes] = g->numPlanes;
	g->volNumPlanes[g->numVolumes] = brush->numsides;
	for ( i = 0; i < 3; i++ ) {
		g->volBounds[g->numVolumes * 6 + i] = brush->bounds[0][i];
		g->volBounds[g->numVolumes * 6 + 3 + i] = brush->bounds[1][i];
	}
	for ( i = 0; i < brush->numsides; i++ ) {
		const cplane_t *pl = brush->sides[i].plane;
		float *q = &g->planes[g->numPlanes * 4];
		q[0] = pl->normal[0];
		q[1] = pl->normal[1];
		q[2] = pl->normal[2];
		q[3] = pl->dist;
		g->planeTop[g->numPlanes] = top[i];
		g->numPlanes++;
	}
	g->numVolumes++;
}

#define NAV_OPEN_GRID_MAX	64	// samples per axis over a large floor face

/* the same for an upward face, sampled on a grid over it (in xy): an
   imported hull's floor brush can run under walls and out into the void
   with its centre and every corner in opaque space, while a room stands
   on one part of it (UT99 AS-Overlord: 6112 x 640 under one barracks room) */
static qboolean FloorFaceOpen( const winding_t *w, const vec3_t normal, float dist ) {
	vec3_t mins, maxs, q;
	float step[2];
	int i, a, sx, sy, n[2];

	ClearBounds( mins, maxs );
	for ( i = 0; i < w->numpoints; i++ ) {
		AddPointToBounds( w->p[i], mins, maxs );
	}
	for ( a = 0; a < 2; a++ ) {
		step[a] = ( maxs[a] - mins[a] ) / NAV_OPEN_GRID_MAX;
		if ( step[a] < 16.0f ) {
			step[a] = 16.0f;
		}
		n[a] = (int)( ( maxs[a] - mins[a] ) / step[a] ) + 1;
	}
	for ( sy = 0; sy < n[1]; sy++ ) {
		for ( sx = 0; sx < n[0]; sx++ ) {
			const float x = mins[0] + ( sx + 0.5f ) * step[0], y = mins[1] + ( sy + 0.5f ) * step[1];
			int inside = 1;
			// inside the convex winding, in xy (either winding order)
			float sign = 0.0f;
			for ( i = 0; i < w->numpoints && inside; i++ ) {
				const float *p0 = w->p[i], *p1 = w->p[( i + 1 ) % w->numpoints];
				const float cr = ( p1[0] - p0[0] ) * ( y - p0[1] ) - ( p1[1] - p0[1] ) * ( x - p0[0] );
				if ( cr != 0.0f ) {
					if ( sign == 0.0f ) {
						sign = cr;
					} else if ( ( cr > 0.0f ) != ( sign > 0.0f ) ) {
						inside = 0;
					}
				}
			}
			if ( !inside ) {
				continue;
			}
			q[0] = x;
			q[1] = y;
			q[2] = ( dist - normal[0] * x - normal[1] * y ) / normal[2];
			VectorMA( q, 1.0f, normal, q );
			if ( cm.leafs[CM_PointLeafnum( q )].cluster >= 0 ) {
				return qtrue;
			}
		}
	}
	return qfalse;
}

/* is there open air (a leaf with a cluster) just in front of the face: at
   its centre or near any corner, a unit out along its normal? A walkable
   face is also sampled across (FloorFaceOpen). */
static qboolean FaceOpen( const winding_t *w, const vec3_t normal, float dist ) {
	vec3_t c, q;
	int i;

	VectorClear( c );
	for ( i = 0; i < w->numpoints; i++ ) {
		VectorAdd( c, w->p[i], c );
	}
	VectorScale( c, 1.0f / w->numpoints, c );
	VectorMA( c, 1.0f, normal, q );
	if ( cm.leafs[CM_PointLeafnum( q )].cluster >= 0 ) {
		return qtrue;
	}
	for ( i = 0; i < w->numpoints; i++ ) {
		vec3_t d;
		float len;
		VectorSubtract( c, w->p[i], d );
		len = VectorNormalize( d );
		VectorMA( w->p[i], len < 4.0f ? len * 0.5f : 2.0f, d, q );
		VectorMA( q, 1.0f, normal, q );
		if ( cm.leafs[CM_PointLeafnum( q )].cluster >= 0 ) {
			return qtrue;
		}
	}
	if ( normal[2] >= NAV_MIN_WALK_NORMAL ) {
		return FloorFaceOpen( w, normal, dist );
	}
	return qfalse;
}

static void AddBrush( navBuild_t *b, cbrush_t *brush ) {
	int i, j;
	qboolean anyOpen = qfalse;
	unsigned char top[1024];

	if ( brush->numsides > (int)sizeof( top ) ) {
		Com_Error( ERR_DROP, "CM_OAXNavGeometry: a brush with %i sides", brush->numsides );
	}
	for ( i = 0; i < brush->numsides; i++ ) {
		top[i] = 0;
	}
	for ( i = 0; i < brush->numsides; i++ ) {
		cbrushside_t *side = &brush->sides[i];
		winding_t *w;

		if ( side->surfaceFlags & SURF_SKY ) {
			continue;
		}
		w = BaseWindingForPlane( side->plane->normal, side->plane->dist );
		for ( j = 0; j < brush->numsides && w; j++ ) {
			cplane_t *p = brush->sides[j].plane;
			vec3_t n;
			if ( j == i ) {
				continue;
			}
			// ignore a plane that duplicates this side (bevels can)
			if ( DotProduct( p->normal, side->plane->normal ) > 0.999f && fabs( p->dist - side->plane->dist ) < 0.01f ) {
				continue;
			}
			VectorNegate( p->normal, n );
			ChopWindingInPlace( &w, n, -p->dist, 0.0f );
		}
		if ( !w ) {
			continue;
		}
		if ( !UnderTerrain( w->numpoints, w->p ) ) {
			// a walkable face with no open air in front of it (it lies in
			// structural solid or faces the void) is no floor: it would only
			// widen the build to the whole hull of an imported map
			float nz = side->plane->normal[2];
			qboolean open = FaceOpen( w, side->plane->normal, side->plane->dist );
			anyOpen |= open;
			if ( nz >= NAV_MIN_WALK_NORMAL && !open ) {
				nz = 0.0f;
				b->closedFaces++;
			}
			AddPolygon( b, w->numpoints, w->p, nz );
			top[i] = nz >= NAV_MIN_WALK_NORMAL;
		}
		FreeWinding( w );
	}
	// a brush with no face in open air lies wholly in opaque space: its
	// columns hold no walkable span to merge away, so it needs no fill
	if ( anyOpen ) {
		AddVolume( b, brush, top );
	} else {
		b->buriedBrushes++;
	}
}

int CM_OAXNavOpenAt( const float p[3] ) {
	vec3_t q;

	VectorCopy( p, q );
	return cm.leafs[CM_PointLeafnum( q )].cluster >= 0;
}

static void AddBox( navBuild_t *b, const vec3_t mins, const vec3_t maxs ) {
	vec3_t q[4];
	int axis;

	// six faces, outward normals; only the top can be walkable
	for ( axis = 0; axis < 3; axis++ ) {
		int u = ( axis + 1 ) % 3, v = ( axis + 2 ) % 3, s;
		for ( s = 0; s < 2; s++ ) {
			float d = s ? maxs[axis] : mins[axis];
			q[0][axis] = q[1][axis] = q[2][axis] = q[3][axis] = d;
			q[0][u] = mins[u]; q[0][v] = mins[v];
			q[1][u] = maxs[u]; q[1][v] = mins[v];
			q[2][u] = maxs[u]; q[2][v] = maxs[v];
			q[3][u] = mins[u]; q[3][v] = maxs[v];
			AddPolygon( b, 4, q, axis == 2 && s ? 1.0f : 0.0f );
		}
	}
}

static void AddTriangle( navBuild_t *b, float tri[3][3] ) {
	float e1[3], e2[3], n[3], l;
	int a;

	e1[0] = tri[1][0] - tri[0][0]; e1[1] = tri[1][1] - tri[0][1]; e1[2] = tri[1][2] - tri[0][2];
	e2[0] = tri[2][0] - tri[0][0]; e2[1] = tri[2][1] - tri[0][1]; e2[2] = tri[2][2] - tri[0][2];
	n[0] = e1[1] * e2[2] - e1[2] * e2[1];
	n[1] = e1[2] * e2[0] - e1[0] * e2[2];
	n[2] = e1[0] * e2[1] - e1[1] * e2[0];
	l = sqrtf( n[0] * n[0] + n[1] * n[1] + n[2] * n[2] );
	a = AddVert( b, tri[0] );
	AddVert( b, tri[1] );
	AddVert( b, tri[2] );
	AddTri( b, a, a + 1, a + 2, l > 0.0f && n[2] / l >= NAV_MIN_WALK_NORMAL );
}

/*
=================
CM_OAXNavGeometry

Fills g (free it with CM_OAXNavGeometryFree). Returns the triangle count.
=================
*/
int CM_OAXNavGeometry( oaxNavGeometry_t *g ) {
	navBuild_t b;
	byte *seen;
	int i, k, t, ci, cj;

	Com_Memset( g, 0, sizeof( *g ) );
	Com_Memset( &b, 0, sizeof( b ) );
	b.g = g;
	if ( !cm.numBrushes ) {
		return 0;
	}

	// world brushes: the ones the world tree's leafs reference
	seen = Z_Malloc( cm.numBrushes );
	for ( i = 0; i < cm.numLeafs; i++ ) {
		cLeaf_t *leaf = &cm.leafs[i];
		for ( k = 0; k < leaf->numLeafBrushes; k++ ) {
			int bn = cm.leafbrushes[leaf->firstLeafBrush + k];
			if ( bn >= 0 && bn < cm.numBrushes ) {
				seen[bn] = 1;
			}
		}
	}
	// q3map2 lists brush entities' brushes in the world leafs too: drop the
	// ones an inline model owns (triggers, movers, zones). OA's common/trigger
	// shader has no nonsolid, so trigger brushes carry CONTENTS_SOLID, and
	// every trigger_hurt, teleporter or zone would otherwise cut a hole in
	// the mesh (the items inside a hazard became unreachable).
	for ( i = 1; i < cm.numSubModels; i++ ) {
		cLeaf_t *leaf = &cm.cmodels[i].leaf;
		for ( k = 0; k < leaf->numLeafBrushes; k++ ) {
			int bn = cm.leafbrushes[leaf->firstLeafBrush + k];
			if ( bn >= 0 && bn < cm.numBrushes ) {
				seen[bn] = 0;
			}
		}
	}
	for ( i = 0; i < cm.numBrushes; i++ ) {
		if ( seen[i] && ( cm.brushes[i].contents & ( CONTENTS_SOLID | CONTENTS_PLAYERCLIP ) ) ) {
			AddBrush( &b, &cm.brushes[i] );
		}
	}
	Z_Free( seen );

	for ( t = 0; t < CM_OAXNumTerrains(); t++ ) {
		const oaxTerrainInfo_t *info = CM_OAXTerrainInfo( t );
		if ( !( info->contents & ( CONTENTS_SOLID | CONTENTS_PLAYERCLIP ) ) ) {
			continue;
		}
		for ( cj = 0; cj < info->samplesY - 1; cj++ ) {
			for ( ci = 0; ci < info->samplesX - 1; ci++ ) {
				float tri[2][3][3];
				OAXTerrain_CellTriangles( info, ci, cj, tri );
				AddTriangle( &b, tri[0] );
				AddTriangle( &b, tri[1] );
			}
		}
	}

	for ( t = 0; t < CM_OAXNumTrunks(); t++ ) {
		vec3_t mins, maxs;
		CM_OAXTrunkBounds( t, mins, maxs );
		AddBox( &b, mins, maxs );
	}

	// collision meshes: the triangles as they face (the front is the outside)
	for ( t = 0; t < CM_OAXNumCollisionTris(); t++ ) {
		float tri[3][3];
		if ( CM_OAXCollisionTri( t, tri ) & ( CONTENTS_SOLID | CONTENTS_PLAYERCLIP ) ) {
			AddTriangle( &b, tri );
		}
	}

	g->closedFaces = b.closedFaces;
	g->buriedBrushes = b.buriedBrushes;
	ClearBounds( g->mins, g->maxs );
	for ( i = 0; i < g->numVerts; i++ ) {
		AddPointToBounds( &g->verts[i * 3], g->mins, g->maxs );
	}
	return g->numTris;
}

void CM_OAXNavGeometryFree( oaxNavGeometry_t *g ) {
	free( g->verts );
	free( g->tris );
	free( g->walkable );
	free( g->volFirstPlane );
	free( g->volNumPlanes );
	free( g->volBounds );
	free( g->planes );
	free( g->planeTop );
	Com_Memset( g, 0, sizeof( *g ) );
}

