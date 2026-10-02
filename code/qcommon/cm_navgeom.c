/*
===========================================================================
cm_navgeom.c: the world's collision geometry as triangles, for the
navigation mesh (server/sv_nav_oax.c).

- Every solid or playerclip brush of the world model (not movers: their
  brushes belong to inline models) as its face polygons, cut from the
  brush planes with cm_polylib. Sky faces are left out.
- Every terrain triangle (cm_terrain.c / oax_terrain.h).
- Every collidable foliage trunk as a box.

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

static void AddBrush( navBuild_t *b, cbrush_t *brush ) {
	int i, j;

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
			AddPolygon( b, w->numpoints, w->p, side->plane->normal[2] );
		}
		FreeWinding( w );
	}
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
	Com_Memset( g, 0, sizeof( *g ) );
}
