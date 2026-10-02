/*
===========================================================================
nav_oax.cpp: build and query the map's navigation mesh with Recast/Detour
(code/thirdparty/recastnavigation, zlib license). See nav_oax.h.

Q3 is z up, Recast is y up: a Q3 point (x, y, z) is (x, z, y) here. That
mirrors the space, which Recast does not mind: triangle winding only
matters to rcMarkWalkableTriangles, and the engine marks walkable
triangles itself (cm_navgeom.c), so host cosf never decides anything.

Built with -ffp-contract=off and the stable-sort patch (README.oax.md), so
native and wasm produce the same navmesh bytes; OAXNav_Hash proves it.
===========================================================================
*/

#include <string.h>
#include <stdio.h>
#include <math.h>

#include "Recast.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourNavMeshQuery.h"
#include "nav_oax.h"

#define NAV_MAX_POLYS_PATH	512
#define NAV_QUERY_NODES		4096

static dtNavMesh		*navMesh;
static dtNavMeshQuery	*navQuery;
static unsigned char	*navData;		// owned by navMesh (DT_TILE_FREE_DATA)
static int				navDataSize;
static int				navPolys;
static unsigned			navHash;
static dtQueryFilter	navFilter;

static inline void ToRc( const float *q, float *r ) {
	r[0] = q[0];
	r[1] = q[2];
	r[2] = q[1];
}

static inline void ToQ3( const float *r, float *q ) {
	q[0] = r[0];
	q[1] = r[2];
	q[2] = r[1];
}

static void Fail( char *err, int errSize, const char *msg ) {
	if ( err && errSize > 0 ) {
		snprintf( err, errSize, "%s", msg );
	}
}

void OAXNav_Free( void ) {
	if ( navQuery ) {
		dtFreeNavMeshQuery( navQuery );
		navQuery = NULL;
	}
	if ( navMesh ) {
		dtFreeNavMesh( navMesh );	// frees navData
		navMesh = NULL;
	}
	navData = NULL;
	navDataSize = 0;
	navPolys = 0;
	navHash = 0;
}

int OAXNav_Build( const float *verts, int numVerts, const int *tris, const unsigned char *walkable,
		int numTris, const oaxNavParams_t *p, char *err, int errSize ) {
	rcContext ctx( false );
	rcConfig cfg;
	rcHeightfield *hf = NULL;
	rcCompactHeightfield *chf = NULL;
	rcContourSet *cset = NULL;
	rcPolyMesh *pmesh = NULL;
	rcPolyMeshDetail *dmesh = NULL;
	float *rv = NULL;
	unsigned char *areas = NULL;
	unsigned char *data = NULL;
	int dataSize = 0, i, ok = 0;
	dtNavMeshCreateParams cp;

	OAXNav_Free();

	memset( &cfg, 0, sizeof( cfg ) );
	cfg.cs = p->cellSize;
	cfg.ch = p->cellHeight;
	cfg.walkableSlopeAngle = 45.0f;		// unused: areas come marked
	cfg.walkableHeight = (int)ceilf( p->agentHeight / cfg.ch );
	cfg.walkableClimb = (int)floorf( p->agentClimb / cfg.ch );
	cfg.walkableRadius = (int)ceilf( p->agentRadius / cfg.cs );
	cfg.maxEdgeLen = (int)( 96.0f / cfg.cs );
	cfg.maxSimplificationError = 1.3f;
	cfg.minRegionArea = 8 * 8;
	cfg.mergeRegionArea = 20 * 20;
	cfg.maxVertsPerPoly = 6;
	cfg.detailSampleDist = cfg.cs * 6.0f;
	cfg.detailSampleMaxError = cfg.ch;
	ToRc( p->bmin, cfg.bmin );
	ToRc( p->bmax, cfg.bmax );
	// ToRc swaps y and z, which keeps min/max per axis
	rcCalcGridSize( cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height );
	if ( cfg.width <= 0 || cfg.height <= 0 || (long long)cfg.width * cfg.height > 16 * 1024 * 1024 ) {
		Fail( err, errSize, "navmesh grid too large or empty" );
		return 0;
	}

	rv = new float[numVerts * 3];
	areas = new unsigned char[numTris];
	for ( i = 0; i < numVerts; i++ ) {
		ToRc( verts + i * 3, rv + i * 3 );
	}
	for ( i = 0; i < numTris; i++ ) {
		areas[i] = walkable[i] ? RC_WALKABLE_AREA : RC_NULL_AREA;
	}

	hf = rcAllocHeightfield();
	chf = rcAllocCompactHeightfield();
	cset = rcAllocContourSet();
	pmesh = rcAllocPolyMesh();
	dmesh = rcAllocPolyMeshDetail();
	if ( !hf || !chf || !cset || !pmesh || !dmesh ) {
		Fail( err, errSize, "out of memory" );
		goto done;
	}
	if ( !rcCreateHeightfield( &ctx, *hf, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch ) ) {
		Fail( err, errSize, "rcCreateHeightfield failed" );
		goto done;
	}
	if ( !rcRasterizeTriangles( &ctx, rv, numVerts, tris, areas, numTris, *hf, cfg.walkableClimb ) ) {
		Fail( err, errSize, "rcRasterizeTriangles failed" );
		goto done;
	}
	rcFilterLowHangingWalkableObstacles( &ctx, cfg.walkableClimb, *hf );
	rcFilterLedgeSpans( &ctx, cfg.walkableHeight, cfg.walkableClimb, *hf );
	rcFilterWalkableLowHeightSpans( &ctx, cfg.walkableHeight, *hf );
	if ( !rcBuildCompactHeightfield( &ctx, cfg.walkableHeight, cfg.walkableClimb, *hf, *chf ) ) {
		Fail( err, errSize, "rcBuildCompactHeightfield failed" );
		goto done;
	}
	if ( !rcErodeWalkableArea( &ctx, cfg.walkableRadius, *chf ) ) {
		Fail( err, errSize, "rcErodeWalkableArea failed" );
		goto done;
	}
	if ( !rcBuildDistanceField( &ctx, *chf ) || !rcBuildRegions( &ctx, *chf, 0, cfg.minRegionArea, cfg.mergeRegionArea ) ) {
		Fail( err, errSize, "rcBuildRegions failed" );
		goto done;
	}
	if ( !rcBuildContours( &ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset ) ) {
		Fail( err, errSize, "rcBuildContours failed" );
		goto done;
	}
	if ( !rcBuildPolyMesh( &ctx, *cset, cfg.maxVertsPerPoly, *pmesh ) ) {
		Fail( err, errSize, "rcBuildPolyMesh failed" );
		goto done;
	}
	if ( !rcBuildPolyMeshDetail( &ctx, *pmesh, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *dmesh ) ) {
		Fail( err, errSize, "rcBuildPolyMeshDetail failed" );
		goto done;
	}
	if ( pmesh->npolys <= 0 ) {
		Fail( err, errSize, "no walkable polygons" );
		goto done;
	}
	if ( pmesh->nverts >= 0xffff ) {
		Fail( err, errSize, "too many navmesh vertices for one tile" );
		goto done;
	}
	for ( i = 0; i < pmesh->npolys; i++ ) {
		pmesh->flags[i] = pmesh->areas[i] == RC_WALKABLE_AREA ? 1 : 0;
	}

	memset( &cp, 0, sizeof( cp ) );
	cp.verts = pmesh->verts;
	cp.vertCount = pmesh->nverts;
	cp.polys = pmesh->polys;
	cp.polyAreas = pmesh->areas;
	cp.polyFlags = pmesh->flags;
	cp.polyCount = pmesh->npolys;
	cp.nvp = pmesh->nvp;
	cp.detailMeshes = dmesh->meshes;
	cp.detailVerts = dmesh->verts;
	cp.detailVertsCount = dmesh->nverts;
	cp.detailTris = dmesh->tris;
	cp.detailTriCount = dmesh->ntris;
	cp.walkableHeight = p->agentHeight;
	cp.walkableRadius = p->agentRadius;
	cp.walkableClimb = p->agentClimb;
	rcVcopy( cp.bmin, pmesh->bmin );
	rcVcopy( cp.bmax, pmesh->bmax );
	cp.cs = cfg.cs;
	cp.ch = cfg.ch;
	cp.buildBvTree = true;
	if ( !dtCreateNavMeshData( &cp, &data, &dataSize ) ) {
		Fail( err, errSize, "dtCreateNavMeshData failed" );
		goto done;
	}

	navMesh = dtAllocNavMesh();
	if ( !navMesh || dtStatusFailed( navMesh->init( data, dataSize, DT_TILE_FREE_DATA ) ) ) {
		dtFree( data );
		Fail( err, errSize, "dtNavMesh init failed" );
		OAXNav_Free();
		goto done;
	}
	navData = data;
	navDataSize = dataSize;
	navQuery = dtAllocNavMeshQuery();
	if ( !navQuery || dtStatusFailed( navQuery->init( navMesh, NAV_QUERY_NODES ) ) ) {
		Fail( err, errSize, "dtNavMeshQuery init failed" );
		OAXNav_Free();
		goto done;
	}
	navFilter.setIncludeFlags( 1 );
	navFilter.setExcludeFlags( 0 );
	navPolys = pmesh->npolys;
	navHash = 2166136261U;
	for ( i = 0; i < dataSize; i++ ) {
		navHash = ( navHash ^ data[i] ) * 16777619U;
	}
	ok = navPolys;

done:
	rcFreeHeightField( hf );
	rcFreeCompactHeightfield( chf );
	rcFreeContourSet( cset );
	rcFreePolyMesh( pmesh );
	rcFreePolyMeshDetail( dmesh );
	delete[] rv;
	delete[] areas;
	return ok;
}

int OAXNav_PolyCount( void ) {
	return navPolys;
}

unsigned OAXNav_Hash( void ) {
	return navHash;
}

int OAXNav_DataSize( void ) {
	return navDataSize;
}

static dtPolyRef Nearest( const float *q3, const float *halfExtentsQ3, float *outRc ) {
	float p[3], ext[3];
	dtPolyRef ref = 0;

	ToRc( q3, p );
	ToRc( halfExtentsQ3, ext );
	if ( dtStatusFailed( navQuery->findNearestPoly( p, ext, &navFilter, &ref, outRc ) ) ) {
		return 0;
	}
	return ref;
}

int OAXNav_Nearest( const float p[3], const float halfExtents[3], float out[3] ) {
	float r[3];

	if ( !navQuery || !Nearest( p, halfExtents, r ) ) {
		return 0;
	}
	ToQ3( r, out );
	return 1;
}

int OAXNav_FindPath( const float start[3], const float goal[3], const float halfExtents[3],
		float *points, int maxPoints, int *flags ) {
	dtPolyRef sref, gref, path[NAV_MAX_POLYS_PATH];
	float s[3], g[3], straight[64 * 3];
	int npath = 0, nstraight = 0, i, n;
	dtStatus st;

	if ( flags ) {
		*flags = 0;
	}
	if ( !navQuery || maxPoints <= 0 ) {
		return 0;
	}
	sref = Nearest( start, halfExtents, s );
	gref = Nearest( goal, halfExtents, g );
	if ( !sref || !gref ) {
		return 0;
	}
	st = navQuery->findPath( sref, gref, s, g, &navFilter, path, &npath, NAV_MAX_POLYS_PATH );
	if ( dtStatusFailed( st ) || npath <= 0 ) {
		return 0;
	}
	if ( path[npath - 1] != gref ) {
		// unreachable goal: the path ends at the nearest point of its last poly
		float cl[3];
		if ( flags ) {
			*flags |= OAXNAV_PATH_PARTIAL;
		}
		if ( dtStatusSucceed( navQuery->closestPointOnPoly( path[npath - 1], g, cl, NULL ) ) ) {
			rcVcopy( g, cl );
		}
	}
	if ( st & DT_PARTIAL_RESULT && flags ) {
		*flags |= OAXNAV_PATH_PARTIAL;
	}
	n = maxPoints < 64 ? maxPoints : 64;
	if ( dtStatusFailed( navQuery->findStraightPath( s, g, path, npath, straight, NULL, NULL, &nstraight, n, 0 ) ) ) {
		return 0;
	}
	for ( i = 0; i < nstraight; i++ ) {
		ToQ3( straight + i * 3, points + i * 3 );
	}
	return nstraight;
}

static unsigned navRandState;

static float NavRand( void ) {
	// LCG; deterministic on every build
	navRandState = navRandState * 1664525U + 1013904223U;
	return (float)( navRandState >> 8 ) * ( 1.0f / 16777216.0f );
}

int OAXNav_RandomPoint( unsigned seed, float out[3] ) {
	dtPolyRef ref;
	float r[3];

	if ( !navQuery ) {
		return 0;
	}
	navRandState = seed ^ 0x5bd1e995U;
	if ( dtStatusFailed( navQuery->findRandomPoint( &navFilter, NavRand, &ref, r ) ) ) {
		return 0;
	}
	ToQ3( r, out );
	return 1;
}
