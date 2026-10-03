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
cm_oaxsurf.c: the surface world in the collision model (step 7.5).

OAX_SURFACES (docs/map-format.md) carries render surfaces only: collision,
PVS and areas come from the map's hull (simple brushes compiled into the
BSP), so traces, Box3D and the navmesh need nothing new for them. This
file adds two things:

- Load-time validation: every visible surface is checked against the
  hull, and surfaces buried in it or floating away from it are reported
  as debug values (cm_surf_*), so an importer's mistakes show up as
  numbers instead of as invisible walls or holes.

- OAX_COLLISION: collision meshes for sources whose solid space is not a
  set of convex brushes. Each triangle collides as a thin slab (the
  triangle and a copy `thickness` behind it) built as a brush on the fly
  and swept by CM_TraceThroughBrush / CM_TestBoxInBrush, so meshes obey
  the brush rules exactly (epsilons, startsolid, the plane pmove sees). A
  static bounding-volume tree over the triangles culls them. Navigation
  (cm_navgeom.c) and physics (phys_bsp.c) read the same triangles.

Plain float +, -, *, /, sqrtf under -ffp-contract=off: native and wasm
agree bit for bit.
===========================================================================
*/

#include "cm_local.h"
#include "oax.h"
#include "oax_surfaces.h"

#define MAX_SLAB_PLANES		48
#define BVH_LEAF_TRIS		4

typedef struct {
	float		v[3][3];
	float		normal[3];
	float		thickness;
	int			contents;
	int			surfaceFlags;
	vec3_t		mins, maxs;		// of the slab
} cmCollTri_t;

typedef struct {
	vec3_t		mins, maxs;
	int			first, count;	// leaf: triangles [first, first + count)
	int			child;			// node: children child and child + 1; -1 for a leaf
} cmCollNode_t;

static cmCollTri_t		*collTris;
static int				numCollTris;
static int				numCollMeshes;
static cmCollNode_t		*collNodes;
static int				numCollNodes;
static unsigned			collHash;
static cvar_t			*cm_noCollisionMeshes;
static cvar_t			*cm_surfGap;

// the slab brush
static cplane_t		slabPlanes[MAX_SLAB_PLANES];
static cbrushside_t	slabSides[MAX_SLAB_PLANES];
static cbrush_t		slabBrush;

// OAX_SURFACES copied out of the file until the hull is ready to check it
static byte			*pendingSurfaces;
static int			pendingSurfacesLen;

void CM_OAXSurfClear( void ) {
	collTris = NULL;
	numCollTris = 0;
	numCollMeshes = 0;
	collNodes = NULL;
	numCollNodes = 0;
	collHash = 0;
	if ( pendingSurfaces ) {
		Z_Free( pendingSurfaces );
		pendingSurfaces = NULL;
	}
	pendingSurfacesLen = 0;
}

// ---- slabs ---------------------------------------------------------------------

static void SlabPlane( float nx, float ny, float nz, vec3_t *pts, int numPts ) {
	cplane_t *p;
	float l, d, best;
	int i;

	if ( slabBrush.numsides >= MAX_SLAB_PLANES ) {
		return;
	}
	l = sqrtf( nx * nx + ny * ny + nz * nz );
	if ( !( l > 1e-6f ) ) {
		return;
	}
	nx /= l; ny /= l; nz /= l;
	for ( i = 0; i < slabBrush.numsides; i++ ) {
		p = &slabPlanes[i];
		if ( p->normal[0] * nx + p->normal[1] * ny + p->normal[2] * nz > 0.99999f ) {
			return;
		}
	}
	best = pts[0][0] * nx + pts[0][1] * ny + pts[0][2] * nz;
	for ( i = 1; i < numPts; i++ ) {
		d = pts[i][0] * nx + pts[i][1] * ny + pts[i][2] * nz;
		if ( d > best ) {
			best = d;
		}
	}
	p = &slabPlanes[slabBrush.numsides++];
	VectorSet( p->normal, nx, ny, nz );
	p->dist = best;
	p->type = PlaneTypeForNormal( p->normal );
	SetPlaneSignbits( p );
}

static void SlabPoints( const cmCollTri_t *t, vec3_t pts[6] ) {
	int i;

	for ( i = 0; i < 3; i++ ) {
		VectorCopy( t->v[i], pts[i] );
		pts[i + 3][0] = t->v[i][0] - t->normal[0] * t->thickness;
		pts[i + 3][1] = t->v[i][1] - t->normal[1] * t->thickness;
		pts[i + 3][2] = t->v[i][2] - t->normal[2] * t->thickness;
	}
}

// the triangle's slab as a brush: axial planes first (CM_TestBoxInBrush
// expects them), the two faces, three sides, and the edge bevels
static void BuildSlab( const cmCollTri_t *t ) {
	vec3_t pts[6];
	int i, k;
	static const float ax[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };

	SlabPoints( t, pts );
	slabBrush.numsides = 6;
	slabBrush.contents = t->contents;
	VectorCopy( t->mins, slabBrush.bounds[0] );
	VectorCopy( t->maxs, slabBrush.bounds[1] );
	for ( i = 0; i < 3; i++ ) {
		cplane_t *p = &slabPlanes[i * 2];
		VectorClear( p->normal );
		p->normal[i] = -1;
		p->dist = -t->mins[i];
		p->type = PlaneTypeForNormal( p->normal );
		SetPlaneSignbits( p );
		p = &slabPlanes[i * 2 + 1];
		VectorClear( p->normal );
		p->normal[i] = 1;
		p->dist = t->maxs[i];
		p->type = i;
		SetPlaneSignbits( p );
	}
	for ( i = 0; i < MAX_SLAB_PLANES; i++ ) {
		slabSides[i].surfaceFlags = t->surfaceFlags;
		slabSides[i].shaderNum = 0;
	}
	SlabPlane( t->normal[0], t->normal[1], t->normal[2], pts, 6 );
	SlabPlane( -t->normal[0], -t->normal[1], -t->normal[2], pts, 6 );
	for ( i = 0; i < 3; i++ ) {
		const float *a = t->v[i], *b = t->v[( i + 1 ) % 3];
		float e[3], s[3];

		VectorSubtract( b, a, e );
		CrossProduct( e, t->normal, s );
		SlabPlane( s[0], s[1], s[2], pts, 6 );
		for ( k = 0; k < 3; k++ ) {
			float c[3];
			CrossProduct( e, ax[k], c );
			SlabPlane( c[0], c[1], c[2], pts, 6 );
			SlabPlane( -c[0], -c[1], -c[2], pts, 6 );
		}
	}
	for ( k = 0; k < 3; k++ ) {
		float c[3];
		CrossProduct( t->normal, ax[k], c );
		SlabPlane( c[0], c[1], c[2], pts, 6 );
		SlabPlane( -c[0], -c[1], -c[2], pts, 6 );
	}
}

// ---- the bounding-volume tree ----------------------------------------------------

static int			*bvhOrder;

static float TriCenter( int t, int axis ) {
	return ( collTris[t].mins[axis] + collTris[t].maxs[axis] ) * 0.5f;
}

// insertion sort of order[first..first+count) by center on axis (stable, deterministic)
static void SortTris( int first, int count, int axis ) {
	int i, j;

	for ( i = first + 1; i < first + count; i++ ) {
		int t = bvhOrder[i];
		float c = TriCenter( t, axis );
		for ( j = i - 1; j >= first && ( TriCenter( bvhOrder[j], axis ) > c || ( TriCenter( bvhOrder[j], axis ) == c && bvhOrder[j] > t ) ); j-- ) {
			bvhOrder[j + 1] = bvhOrder[j];
		}
		bvhOrder[j + 1] = t;
	}
}

static void BuildNode( int n, int first, int count ) {
	cmCollNode_t *node = &collNodes[n];
	int i, axis = 0;
	vec3_t size;

	ClearBounds( node->mins, node->maxs );
	for ( i = first; i < first + count; i++ ) {
		AddPointToBounds( collTris[bvhOrder[i]].mins, node->mins, node->maxs );
		AddPointToBounds( collTris[bvhOrder[i]].maxs, node->mins, node->maxs );
	}
	node->first = first;
	node->count = count;
	node->child = -1;
	if ( count <= BVH_LEAF_TRIS ) {
		return;
	}
	VectorSubtract( node->maxs, node->mins, size );
	if ( size[1] > size[axis] ) axis = 1;
	if ( size[2] > size[axis] ) axis = 2;
	SortTris( first, count, axis );
	node->child = numCollNodes;
	numCollNodes += 2;
	BuildNode( node->child, first, count / 2 );
	BuildNode( node->child + 1, first + count / 2, count - count / 2 );
}

// ---- loading ----------------------------------------------------------------------

static void CM_OAXCollisionLoad( const void *bsp, int bspLen ) {
	const void *lump;
	oaxCollLump_t L;
	char err[160];
	int len, m, t, n = 0;
	cmCollTri_t *sorted;

	lump = OAX_FindBspxLump( bsp, bspLen, OAX_COLLISION_LUMP, &len );
	if ( !lump ) {
		return;
	}
	if ( !OAXColl_Parse( lump, len, &L, err, sizeof( err ) ) ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: OAX_COLLISION ignored: %s\n", err );
		return;
	}
	collHash = OAX_Fnv1a( 2166136261U, lump, len );
	for ( m = 0; m < L.numMeshes; m++ ) {
		oaxCollMesh_t mesh;
		OAXColl_Mesh( &L, m, &mesh );
		n += mesh.numIndexes / 3;
	}
	collTris = Hunk_Alloc( sizeof( cmCollTri_t ) * ( n + 1 ), h_high );
	for ( m = 0; m < L.numMeshes; m++ ) {
		oaxCollMesh_t mesh;

		OAXColl_Mesh( &L, m, &mesh );
		for ( t = 0; t < mesh.numIndexes; t += 3 ) {
			cmCollTri_t *ct = &collTris[numCollTris];
			float e1[3], e2[3], l;
			vec3_t pts[6];
			int k;

			for ( k = 0; k < 3; k++ ) {
				OAXColl_Vert( &L, mesh.firstVert + OAXColl_Index( &L, mesh.firstIndex + t + k ), ct->v[k] );
			}
			VectorSubtract( ct->v[1], ct->v[0], e1 );
			VectorSubtract( ct->v[2], ct->v[0], e2 );
			CrossProduct( e1, e2, ct->normal );
			l = sqrtf( DotProduct( ct->normal, ct->normal ) );
			if ( !( l > 1e-6f ) ) {
				continue;	// degenerate
			}
			ct->normal[0] /= l;
			ct->normal[1] /= l;
			ct->normal[2] /= l;
			ct->thickness = mesh.thickness;
			ct->contents = mesh.contents;
			ct->surfaceFlags = mesh.surfaceFlags;
			SlabPoints( ct, pts );
			ClearBounds( ct->mins, ct->maxs );
			for ( k = 0; k < 6; k++ ) {
				AddPointToBounds( pts[k], ct->mins, ct->maxs );
			}
			numCollTris++;
		}
	}
	numCollMeshes = L.numMeshes;

	// the tree, over a triangle order, then the triangles stored in tree order
	if ( numCollTris ) {
		bvhOrder = Z_Malloc( sizeof( int ) * numCollTris );
		for ( t = 0; t < numCollTris; t++ ) {
			bvhOrder[t] = t;
		}
		collNodes = Hunk_Alloc( sizeof( cmCollNode_t ) * ( numCollTris * 2 + 1 ), h_high );
		numCollNodes = 1;
		BuildNode( 0, 0, numCollTris );
		sorted = Z_Malloc( sizeof( cmCollTri_t ) * numCollTris );
		for ( t = 0; t < numCollTris; t++ ) {
			sorted[t] = collTris[bvhOrder[t]];
		}
		Com_Memcpy( collTris, sorted, sizeof( cmCollTri_t ) * numCollTris );
		Z_Free( sorted );
		Z_Free( bvhOrder );
		bvhOrder = NULL;
	}
	for ( t = 0; t < MAX_SLAB_PLANES; t++ ) {
		slabSides[t].plane = &slabPlanes[t];
	}
	slabBrush.sides = slabSides;
	Com_Printf( "OAX_COLLISION: %i meshes, %i triangles, %i tree nodes, hash %08x\n", numCollMeshes, numCollTris, numCollNodes, collHash );
}

/*
=================
CM_OAXSurfLoad

Called from CM_LoadMap with the whole BSP file, before it is freed.
=================
*/
void CM_OAXSurfLoad( const void *bsp, int bspLen ) {
	const void *lump;
	int len;

	CM_OAXSurfClear();
	if ( !cm_noCollisionMeshes ) {
		cm_noCollisionMeshes = Cvar_Get( "cm_noCollisionMeshes", "0", CVAR_CHEAT );
		Cvar_SetDescription( cm_noCollisionMeshes, "Debug: the collision model ignores OAX_COLLISION meshes." );
		cm_surfGap = Cvar_Get( "cm_surfGap", "8", 0 );
		Cvar_SetDescription( cm_surfGap, "Surface-world validation: a surface with no hull solid within this many units behind it is reported as floating." );
	}
	CM_OAXCollisionLoad( bsp, bspLen );
	Com_DebugSetInt( "cm_coll_meshes", numCollMeshes );
	Com_DebugSetInt( "cm_coll_tris", numCollTris );
	Com_DebugSet( "cm_coll_hash", va( "%08x", collHash ) );

	lump = OAX_FindBspxLump( bsp, bspLen, OAX_SURFACES_LUMP, &len );
	Com_DebugSetInt( "cm_surf_count", 0 );
	if ( lump && len > 0 ) {
		pendingSurfaces = Z_Malloc( len );
		Com_Memcpy( pendingSurfaces, lump, len );
		pendingSurfacesLen = len;
	}
}

// ---- tracing ------------------------------------------------------------------------

typedef struct {
	traceWork_t	*tw;
	vec3_t		ext;
	vec3_t		dir;
	qboolean	position;
} collWork_t;

static qboolean SweepTouchesBox( const collWork_t *w, const vec3_t lo, const vec3_t hi ) {
	const traceWork_t *tw = w->tw;
	float t0 = 0.0f, t1 = 1.0f;
	int i;

	if ( tw->bounds[0][0] > hi[0] + 1.0f || tw->bounds[0][1] > hi[1] + 1.0f || tw->bounds[0][2] > hi[2] + 1.0f ||
		tw->bounds[1][0] < lo[0] - 1.0f || tw->bounds[1][1] < lo[1] - 1.0f || tw->bounds[1][2] < lo[2] - 1.0f ) {
		return qfalse;
	}
	if ( w->position ) {
		return qtrue;
	}
	for ( i = 0; i < 3; i++ ) {
		float a = lo[i] - w->ext[i] - 1.0f, b = hi[i] + w->ext[i] + 1.0f;
		float s = tw->start[i], d = w->dir[i];
		if ( d == 0.0f ) {
			if ( s < a || s > b ) {
				return qfalse;
			}
			continue;
		}
		{
			float ta = ( a - s ) / d, tb = ( b - s ) / d;
			if ( ta > tb ) {
				float x = ta; ta = tb; tb = x;
			}
			if ( ta > t0 ) t0 = ta;
			if ( tb < t1 ) t1 = tb;
			if ( t0 > t1 ) {
				return qfalse;
			}
		}
	}
	return qtrue;
}

// returns qtrue when the trace is fully blocked (stop walking)
static qboolean CollNode( collWork_t *w, int n ) {
	const cmCollNode_t *node = &collNodes[n];
	traceWork_t *tw = w->tw;
	int i;

	if ( !SweepTouchesBox( w, node->mins, node->maxs ) ) {
		return qfalse;
	}
	if ( node->child >= 0 ) {
		return CollNode( w, node->child ) || CollNode( w, node->child + 1 );
	}
	for ( i = node->first; i < node->first + node->count; i++ ) {
		const cmCollTri_t *t = &collTris[i];
		if ( !( t->contents & tw->contents ) || !CM_BoundsIntersect( tw->bounds[0], tw->bounds[1], t->mins, t->maxs ) ) {
			continue;
		}
		BuildSlab( t );
		if ( w->position ) {
			CM_TestBoxInBrush( tw, &slabBrush );
			if ( tw->trace.allsolid ) {
				return qtrue;
			}
		} else {
			CM_TraceThroughBrush( tw, &slabBrush );
			if ( !tw->trace.fraction ) {
				return qtrue;
			}
		}
	}
	return qfalse;
}

static void CollWork( traceWork_t *tw, qboolean position ) {
	collWork_t w;
	int i;

	if ( !numCollTris || cm_noCollisionMeshes->integer ) {
		return;
	}
	w.tw = tw;
	w.position = position;
	VectorSubtract( tw->end, tw->start, w.dir );
	for ( i = 0; i < 3; i++ ) {
		if ( tw->sphere.use ) {
			w.ext[i] = tw->sphere.radius + fabs( tw->sphere.offset[i] );
		} else {
			w.ext[i] = -tw->size[0][i] > tw->size[1][i] ? -tw->size[0][i] : tw->size[1][i];
		}
	}
	CollNode( &w, 0 );
}

void CM_OAXCollisionTrace( traceWork_t *tw ) {
	CollWork( tw, qfalse );
}

void CM_OAXCollisionPositionTest( traceWork_t *tw ) {
	if ( !tw->trace.allsolid ) {
		CollWork( tw, qtrue );
	}
}

static int CollPointNode( int n, const vec3_t p ) {
	const cmCollNode_t *node = &collNodes[n];
	int i, k, contents = 0;

	if ( !CM_BoundsIntersectPoint( node->mins, node->maxs, p ) ) {
		return 0;
	}
	if ( node->child >= 0 ) {
		return CollPointNode( node->child, p ) | CollPointNode( node->child + 1, p );
	}
	for ( i = node->first; i < node->first + node->count; i++ ) {
		const cmCollTri_t *t = &collTris[i];
		if ( !CM_BoundsIntersectPoint( t->mins, t->maxs, p ) ) {
			continue;
		}
		BuildSlab( t );
		for ( k = 0; k < slabBrush.numsides; k++ ) {
			if ( DotProduct( p, slabPlanes[k].normal ) > slabPlanes[k].dist ) {
				break;
			}
		}
		if ( k == slabBrush.numsides ) {
			contents |= t->contents;
		}
	}
	return contents;
}

int CM_OAXCollisionPointContents( const vec3_t p ) {
	if ( !numCollTris || cm_noCollisionMeshes->integer ) {
		return 0;
	}
	return CollPointNode( 0, p );
}

// ---- consumers outside collision ------------------------------------------------------

int CM_OAXNumCollisionTris( void ) {
	return cm_noCollisionMeshes && cm_noCollisionMeshes->integer ? 0 : numCollTris;
}

// triangle n: corners (counter-clockwise seen from the outside) and contents
int CM_OAXCollisionTri( int n, float v[3][3] ) {
	if ( n < 0 || n >= numCollTris ) {
		return 0;
	}
	Com_Memcpy( v, collTris[n].v, sizeof( collTris[n].v ) );
	return collTris[n].contents;
}

// ---- validation -------------------------------------------------------------------------

/*
=================
CM_OAXSurfValidate

Called from CM_LoadMap once the hull (brushes, patches, terrain, collision
meshes) is loaded. For each visible surface, up to 16 of its triangles are
probed at their centroids, half a unit in front of the surface (in solid:
buried) and along the back normal up to cm_surfGap units (no solid hit:
floating). A surface is buried when most probes are, floating when every
probe is.
=================
*/
void CM_OAXSurfValidate( void ) {
	oaxSurfLump_t L;
	char err[160], buried[256], floating[256];
	int i, nBuried = 0, nFloating = 0, nChecked = 0, nVisible = 0;
	float gap;

	buried[0] = floating[0] = 0;
	Com_DebugSet( "cm_surf_buried_ids", "" );
	Com_DebugSet( "cm_surf_floating_ids", "" );
	if ( !pendingSurfaces ) {
		Com_DebugSetInt( "cm_surf_count", 0 );
		Com_DebugSetInt( "cm_surf_checked", 0 );
		Com_DebugSetInt( "cm_surf_buried", 0 );
		Com_DebugSetInt( "cm_surf_floating", 0 );
		Com_DebugSet( "cm_surf_hash", "none" );
		return;
	}
	Com_DebugSet( "cm_surf_hash", va( "%08x", OAX_Fnv1a( 2166136261U, pendingSurfaces, pendingSurfacesLen ) ) );
	if ( !OAXSurf_Parse( pendingSurfaces, pendingSurfacesLen, &L, err, sizeof( err ) ) ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: OAX_SURFACES not validated: %s\n", err );
		Com_DebugSet( "cm_surf_error", err );
		Z_Free( pendingSurfaces );
		pendingSurfaces = NULL;
		return;
	}
	gap = cm_surfGap->value > 0 ? cm_surfGap->value : 8.0f;
	for ( i = 0; i < L.numSurfaces; i++ ) {
		oaxSurf_t s;
		int tris, step, t, probes = 0, inSolid = 0, open = 0;
		qboolean checkFloat;

		OAXSurf_Surface( &L, i, &s );
		if ( ( s.flags & OSF_INVISIBLE ) || s.model != 0 ) {
			continue;
		}
		nVisible++;
		checkFloat = !( s.flags & ( OSF_TWOSIDED | OSF_TRANSLUCENT | OSF_ADDITIVE | OSF_MASKED | OSF_DETAIL ) );
		tris = OAXSurf_NumTris( &s );
		step = tris > 16 ? tris / 16 : 1;
		for ( t = 0; t < tris && probes < 16; t += step ) {
			oaxSurfVert_t a, b, c;
			int tv[3], k;
			vec3_t ctr, e1, e2, n, front, back;
			float l;
			trace_t tr;

			OAXSurf_Tri( &L, &s, t, tv );
			OAXSurf_Vert( &L, tv[0], &a );
			OAXSurf_Vert( &L, tv[1], &b );
			OAXSurf_Vert( &L, tv[2], &c );
			VectorSubtract( b.xyz, a.xyz, e1 );
			VectorSubtract( c.xyz, a.xyz, e2 );
			CrossProduct( e1, e2, n );
			l = sqrtf( DotProduct( n, n ) );
			if ( !( l > 1e-6f ) ) {
				continue;
			}
			VectorScale( n, 1.0f / l, n );
			for ( k = 0; k < 3; k++ ) {
				ctr[k] = ( a.xyz[k] + b.xyz[k] + c.xyz[k] ) / 3.0f;
			}
			probes++;
			VectorMA( ctr, 0.5f, n, front );
			if ( CM_PointContents( front, 0 ) & CONTENTS_SOLID ) {
				inSolid++;
			}
			if ( checkFloat ) {
				VectorMA( ctr, 0.25f, n, front );
				VectorMA( ctr, -gap, n, back );
				CM_BoxTrace( &tr, front, back, vec3_origin, vec3_origin, 0, CONTENTS_SOLID, qfalse );
				if ( tr.fraction == 1.0f && !tr.startsolid ) {
					open++;
				}
			}
		}
		if ( !probes ) {
			continue;
		}
		if ( checkFloat ) {
			nChecked++;
		}
		if ( inSolid * 2 > probes ) {
			nBuried++;
			if ( nBuried <= 16 ) {
				Q_strcat( buried, sizeof( buried ), va( "%s%d:%d", buried[0] ? " " : "", i, s.sourceId ) );
			}
		} else if ( checkFloat && open == probes ) {
			nFloating++;
			if ( nFloating <= 16 ) {
				Q_strcat( floating, sizeof( floating ), va( "%s%d:%d", floating[0] ? " " : "", i, s.sourceId ) );
			}
		}
	}
	Com_DebugSetInt( "cm_surf_count", L.numSurfaces );
	Com_DebugSetInt( "cm_surf_visible", nVisible );
	Com_DebugSetInt( "cm_surf_checked", nChecked );
	Com_DebugSetInt( "cm_surf_buried", nBuried );
	Com_DebugSetInt( "cm_surf_floating", nFloating );
	Com_DebugSet( "cm_surf_buried_ids", buried );
	Com_DebugSet( "cm_surf_floating_ids", floating );
	Com_Printf( "OAX_SURFACES: %i surfaces, %i checked against the hull: %i buried, %i floating\n", L.numSurfaces, nChecked, nBuried, nFloating );
	Z_Free( pendingSurfaces );
	pendingSurfaces = NULL;
}
