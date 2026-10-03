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

Navigation from intent (step 7.5): OAXNav_BuildEx also takes off-mesh links
(teleporters, jump pads, ladders, routes) and cost volumes (hazards). Area
ids: 63 plain walkable, 62 teleporter links (nearly free: a teleport costs
no walking), 1-61 hazard volumes grouped by cost. Polygon flags say what a
filter may cross (nav_oax.h OAXNAV_*).
===========================================================================
*/

#include <string.h>
#include <stdio.h>
#include <math.h>

#include "Recast.h"
#include "RecastAlloc.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourNavMeshQuery.h"
#include "nav_oax.h"

#define NAV_MAX_POLYS_PATH	512
#define NAV_QUERY_NODES		32768	// paths across a large tiled map

static dtNavMesh		*navMesh;
static dtNavMeshQuery	*navQuery;
static int				navDataSize;
static int				navPolys;
static unsigned			navHash;
static dtQueryFilter	navFilter;
static int				navLinks;		// links connected at both ends
#define NAV_MAX_LINK_STATE	1024
static unsigned char	navLinkOk[NAV_MAX_LINK_STATE];	// by link index (userId): connected at both ends

#define NAV_AREA_WALK		RC_WALKABLE_AREA	// 63
#define NAV_AREA_TELEPORT	62
#define NAV_AREA_HAZARD_MAX	61
#define NAV_TELEPORT_COST	0.05f

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
		dtFreeNavMesh( navMesh );	// frees the tiles (DT_TILE_FREE_DATA)
		navMesh = NULL;
	}
	navDataSize = 0;
	navPolys = 0;
	navHash = 0;
	navLinks = 0;
}

// links whose off-mesh polygon reached the mesh at both ends (every tile)
static int CountConnectedLinks( void ) {
	const dtNavMesh *nm = navMesh;
	int t, i, n = 0;

	memset( navLinkOk, 0, sizeof( navLinkOk ) );
	if ( !nm ) {
		return 0;
	}
	for ( t = 0; t < nm->getMaxTiles(); t++ ) {
		const dtMeshTile *tile = nm->getTile( t );
		if ( !tile || !tile->header ) {
			continue;
		}
		for ( i = 0; i < tile->header->offMeshConCount; i++ ) {
			const dtPoly *poly = &tile->polys[tile->offMeshCons[i].poly];
			int ends = 0;
			for ( unsigned int k = poly->firstLink; k != DT_NULL_LINK; k = tile->links[k].next ) {
				ends |= 1 << ( tile->links[k].edge & 1 );
			}
			if ( ends == 3 ) {
				n++;
				if ( tile->offMeshCons[i].userId < NAV_MAX_LINK_STATE ) {
					navLinkOk[tile->offMeshCons[i].userId] = 1;
				}
			}
		}
	}
	return n;
}

extern "C" int Sys_Milliseconds( void );
static int navMsRaster, navMsFill, navMsRest;	// wall-clock profile of the last build (logged only)
static float navBounds[6];
static int navStatTiles, navStatGridTiles, navStatSolidColumns, navStatOpaqueTops, navStatForbidTris;

void OAXNav_BuildStats( int *tiles, int *gridTiles, int *solidColumns, int *opaqueTops ) {
	*tiles = navStatTiles;
	*gridTiles = navStatGridTiles;
	*solidColumns = navStatSolidColumns;
	*opaqueTops = navStatOpaqueTops;
}

int OAXNav_ForbiddenTris( void ) {
	return navStatForbidTris;
}

void OAXNav_BuildProfile( int *msRaster, int *msFill, int *msRest, float *bounds ) {
	*msRaster = navMsRaster;
	*msFill = navMsFill;
	*msRest = navMsRest;
	memcpy( bounds, navBounds, sizeof( navBounds ) );
}

int OAXNav_Build( const float *verts, int numVerts, const int *tris, const unsigned char *walkable,
		int numTris, const oaxNavParams_t *p, char *err, int errSize ) {
	return OAXNav_BuildEx( verts, numVerts, tris, walkable, numTris, p, NULL, 0, NULL, 0, NULL, err, errSize );
}

/*
A span the way rcAddSpan stores one, but merged so that the span keeps the
area of whatever caps it: the contributor with the highest top (several
within flagMergeThr of it: the highest area id). rcAddSpan instead takes
the larger area whenever the existing span is the higher one, so a buried
walkable face would make the solid above it walkable.
*/
static rcSpan *NavAllocSpan( rcHeightfield &hf ) {
	rcSpan *s;

	if ( !hf.freelist || !hf.freelist->next ) {
		rcSpanPool *pool = (rcSpanPool *)rcAlloc( sizeof( rcSpanPool ), RC_ALLOC_PERM );
		rcSpan *freeList, *head, *it;
		if ( !pool ) {
			return NULL;
		}
		pool->next = hf.pools;
		hf.pools = pool;
		freeList = hf.freelist;
		head = &pool->items[0];
		it = &pool->items[RC_SPANS_PER_POOL];
		do {
			--it;
			it->next = freeList;
			freeList = it;
		} while ( it != head );
		hf.freelist = it;
	}
	s = hf.freelist;
	hf.freelist = hf.freelist->next;
	return s;
}

static bool NavAddCappedSpan( rcHeightfield &hf, int x, int z, unsigned int smin, unsigned int smax,
		unsigned char area, int thr ) {
	rcSpan *s = NavAllocSpan( hf ), *prev = NULL, *cur;
	const int col = x + z * hf.width;
	unsigned int top = smax;
	unsigned char topArea = area;

	if ( !s ) {
		return false;
	}
	cur = hf.spans[col];
	while ( cur ) {
		if ( cur->smin > smax ) {
			break;
		}
		if ( cur->smax < smin ) {
			prev = cur;
			cur = cur->next;
			continue;
		}
		if ( cur->smin < smin ) {
			smin = cur->smin;
		}
		if ( cur->smax > smax ) {
			smax = cur->smax;
		}
		if ( (int)cur->smax > (int)top + thr ) {
			top = cur->smax;
			topArea = cur->area;
		} else if ( rcAbs( (int)cur->smax - (int)top ) <= thr ) {
			if ( cur->smax > top ) {
				top = cur->smax;
			}
			topArea = rcMax( topArea, (unsigned char)cur->area );
		}
		{
			rcSpan *next = cur->next;
			cur->next = hf.freelist;
			hf.freelist = cur;
			if ( prev ) {
				prev->next = next;
			} else {
				hf.spans[col] = next;
			}
			cur = next;
		}
	}
	s->smin = smin;
	s->smax = smax;
	s->area = topArea;
	if ( prev ) {
		s->next = prev->next;
		prev->next = s;
	} else {
		s->next = hf.spans[col];
		hf.spans[col] = s;
	}
	return true;
}

// fill the volumes' column spans (sampled at column centres) into hf
static bool NavFillSolids( rcHeightfield &hf, const oaxNavSolids_t *sol, const float *tileMinQ3,
		const float *tileMaxQ3, int thr ) {
	const float ich = 1.0f / hf.ch, by = hf.bmax[1] - hf.bmin[1];
	int v;

	for ( v = 0; v < sol->numVolumes; v++ ) {
		const float *b = &sol->volBounds[v * 6];
		const float *pl = &sol->planes[sol->volFirstPlane[v] * 4];
		const unsigned char *topFlag = &sol->planeTop[sol->volFirstPlane[v]];
		const int np = sol->volNumPlanes[v];
		int x0, x1, z0, z1, x, z;

		if ( b[3] < tileMinQ3[0] || b[0] > tileMaxQ3[0] || b[4] < tileMinQ3[1] || b[1] > tileMaxQ3[1] ||
			b[5] < hf.bmin[1] || b[2] > hf.bmax[1] ) {
			continue;
		}
		// columns whose centre can lie inside (Q3 x -> rc x, Q3 y -> rc z)
		x0 = (int)floorf( ( b[0] - hf.bmin[0] ) / hf.cs - 0.5f );
		x1 = (int)ceilf( ( b[3] - hf.bmin[0] ) / hf.cs - 0.5f );
		z0 = (int)floorf( ( b[1] - hf.bmin[2] ) / hf.cs - 0.5f );
		z1 = (int)ceilf( ( b[4] - hf.bmin[2] ) / hf.cs - 0.5f );
		x0 = rcMax( x0, 0 );
		z0 = rcMax( z0, 0 );
		x1 = rcMin( x1, hf.width - 1 );
		z1 = rcMin( z1, hf.height - 1 );
		for ( z = z0; z <= z1; z++ ) {
			const float qy = hf.bmin[2] + ( z + 0.5f ) * hf.cs;
			for ( x = x0; x <= x1; x++ ) {
				const float qx = hf.bmin[0] + ( x + 0.5f ) * hf.cs;
				float lo = -1e30f, hi = 1e30f;
				int capTop = 0, inside = 1, i;
				for ( i = 0; i < np && inside; i++ ) {
					const float *q = &pl[i * 4];
					const float rest = q[3] - q[0] * qx - q[1] * qy;
					if ( q[2] > 1e-6f ) {
						const float zz = rest / q[2];
						if ( zz < hi - 1e-4f ) {
							hi = zz;
							capTop = topFlag[i];
						} else if ( zz <= hi + 1e-4f ) {
							capTop |= topFlag[i];
						}
					} else if ( q[2] < -1e-6f ) {
						const float zz = rest / q[2];
						if ( zz > lo ) {
							lo = zz;
						}
					} else if ( rest < 0.0f ) {
						inside = 0;
					}
				}
				if ( !inside || hi <= lo ) {
					continue;
				}
				{
					// Recast's own quantisation (rasterizeTri)
					float smin = lo - hf.bmin[1], smax = hi - hf.bmin[1];
					int ismin, ismax;
					if ( smax < 0.0f || smin > by ) {
						continue;
					}
					if ( smin < 0.0f ) {
						smin = 0.0f;
					}
					if ( smax > by ) {
						smax = by;
					}
					ismin = rcClamp( (int)floorf( smin * ich ), 0, RC_SPAN_MAX_HEIGHT );
					ismax = rcClamp( (int)ceilf( smax * ich ), ismin + 1, RC_SPAN_MAX_HEIGHT );
					if ( !NavAddCappedSpan( hf, x, z, (unsigned int)ismin, (unsigned int)ismax,
						capTop ? RC_WALKABLE_AREA : RC_NULL_AREA, thr ) ) {
						return false;
					}
					navStatSolidColumns++;
				}
			}
		}
	}
	return true;
}

// walkable tops whose air is opaque (inside solid, the void) are dropped
static void NavDropOpaqueTops( rcHeightfield &hf, const oaxNavSolids_t *sol ) {
	int x, z;

	for ( z = 0; z < hf.height; z++ ) {
		for ( x = 0; x < hf.width; x++ ) {
			for ( rcSpan *s = hf.spans[x + z * hf.width]; s; s = s->next ) {
				float q[3];
				if ( s->area == RC_NULL_AREA ) {
					continue;
				}
				q[0] = hf.bmin[0] + ( x + 0.5f ) * hf.cs;
				q[1] = hf.bmin[2] + ( z + 0.5f ) * hf.cs;
				q[2] = hf.bmin[1] + s->smax * hf.ch + 1.0f;
				if ( !sol->openAt( q ) ) {
					s->area = RC_NULL_AREA;
					navStatOpaqueTops++;
				}
			}
		}
	}
}

/*
=================
OAXNav_BuildEx

Tiles of NAV_TILE_CELLS cells across, over the walkable geometry's bounds
(plus a margin) only: a map's sky or void can be huge, and nothing outside
the floors matters to a walker. Every tile is a full Recast build with a
border (Recast's tiled pipeline); tiles with no walkable triangle are not
built. Off-mesh connections are given to every tile: Detour keeps each in
the tile its start lies in, links landings in that tile and its neighbours,
and dtNavMesh::connectFarOffMeshLinks (oax patch) the rest.
=================
*/
#define NAV_TILE_CELLS	256
#define NAV_MAX_TILES	4096

int OAXNav_BuildEx( const float *verts, int numVerts, const int *tris, const unsigned char *walkable,
		int numTris, const oaxNavParams_t *p, const oaxNavLink_t *links, int numLinks,
		const oaxNavArea_t *vols, int numVols, const oaxNavSolids_t *solids, char *err, int errSize ) {
	float areaCost[64];
	int numHazardIds = 0;
	float *conVerts = NULL, *conRad = NULL, *triBounds = NULL;
	unsigned short *conFlags = NULL;
	unsigned char *conAreas = NULL, *conDir = NULL;
	unsigned int *conIds = NULL;
	rcContext ctx( false );
	rcConfig base;
	float *rv = NULL;
	unsigned char *areas = NULL;
	int *tileTris = NULL;
	int i, k, ok = 0, tw, th, tileBits, polys = 0, any = 0;
	unsigned char *tileState = NULL;
	int *tileQueue = NULL, queueHead = 0, queueTail = 0;
	float wmin[3], wmax[3], tileSize;
	dtNavMeshParams nmp;
	unsigned hash = 2166136261U;
	int totalSize = 0;

	OAXNav_Free();
	navStatTiles = navStatGridTiles = navStatSolidColumns = navStatOpaqueTops = navStatForbidTris = 0;
	navMsRaster = navMsFill = navMsRest = 0;

	memset( &base, 0, sizeof( base ) );
	base.cs = p->cellSize;
	base.ch = p->cellHeight;
	base.walkableSlopeAngle = 45.0f;		// unused: areas come marked
	base.walkableHeight = (int)ceilf( p->agentHeight / base.ch );
	base.walkableClimb = (int)floorf( p->agentClimb / base.ch );
	base.walkableRadius = (int)ceilf( p->agentRadius / base.cs );
	base.maxEdgeLen = (int)( 96.0f / base.cs );
	base.maxSimplificationError = 1.3f;
	base.minRegionArea = 8 * 8;
	base.mergeRegionArea = 20 * 20;
	base.maxVertsPerPoly = 6;
	base.detailSampleDist = base.cs * 6.0f;
	base.detailSampleMaxError = base.ch;
	base.borderSize = base.walkableRadius + 3;
	base.tileSize = NAV_TILE_CELLS;

	// walkable triangles wholly inside a removing cost volume (a deadly
	// hazard: an imported map's kill zone under the level, say) are no
	// floor: they neither widen the build nor make tiles
	unsigned char *walk = new unsigned char[numTris > 0 ? numTris : 1];
	for ( i = 0; i < numTris; i++ ) {
		walk[i] = walkable[i];
		for ( k = 0; k < numVols && walk[i]; k++ ) {
			int c, a, in = 1;
			if ( vols[k].cost >= 0.0f ) {
				continue;
			}
			for ( c = 0; c < 3 && in; c++ ) {
				const float *v = verts + tris[i * 3 + c] * 3;
				for ( a = 0; a < 3; a++ ) {
					const float lo = vols[k].mins[a] - ( a == 2 ? p->agentClimb : 0.0f );
					if ( v[a] < lo || v[a] > vols[k].maxs[a] ) {
						in = 0;
						break;
					}
				}
			}
			if ( in ) {
				walk[i] = 0;
				navStatForbidTris++;
			}
		}
	}

	// the walkable geometry's bounds (Q3), within the given bounds
	for ( i = 0; i < numTris; i++ ) {
		if ( !walk[i] ) {
			continue;
		}
		for ( k = 0; k < 3; k++ ) {
			const float *v = verts + tris[i * 3 + k] * 3;
			int a;
			for ( a = 0; a < 3; a++ ) {
				if ( !any || v[a] < wmin[a] ) {
					wmin[a] = v[a];
				}
				if ( !any || v[a] > wmax[a] ) {
					wmax[a] = v[a];
				}
			}
			any = 1;
		}
	}
	if ( !any ) {
		Fail( err, errSize, "no walkable triangles" );
		delete[] walk;
		return 0;
	}
	for ( k = 0; k < 2; k++ ) {
		wmin[k] -= p->agentRadius + base.cs * 2.0f;
		wmax[k] += p->agentRadius + base.cs * 2.0f;
	}
	wmin[2] -= p->agentClimb + base.ch * 2.0f;
	wmax[2] += p->agentHeight + p->agentClimb + base.ch * 2.0f;
	for ( k = 0; k < 3; k++ ) {
		if ( wmin[k] < p->bmin[k] ) {
			wmin[k] = p->bmin[k];
		}
		if ( wmax[k] > p->bmax[k] ) {
			wmax[k] = p->bmax[k];
		}
	}
	for ( k = 0; k < 3; k++ ) {
		navBounds[k] = wmin[k];
		navBounds[3 + k] = wmax[k];
	}
	tileSize = NAV_TILE_CELLS * base.cs;
	tw = (int)ceilf( ( wmax[0] - wmin[0] ) / tileSize );
	th = (int)ceilf( ( wmax[1] - wmin[1] ) / tileSize );
	if ( tw <= 0 || th <= 0 || tw * th > NAV_MAX_TILES ) {
		Fail( err, errSize, "navmesh tile grid empty or too large" );
		delete[] walk;
		return 0;
	}
	navStatGridTiles = tw * th;
	tileBits = 0;
	while ( ( 1 << tileBits ) < tw * th ) {
		tileBits++;
	}
	if ( tileBits > 14 ) {
		Fail( err, errSize, "too many navmesh tiles" );
		delete[] walk;
		return 0;
	}

	memset( &nmp, 0, sizeof( nmp ) );
	ToRc( wmin, nmp.orig );
	nmp.tileWidth = tileSize;
	nmp.tileHeight = tileSize;
	nmp.maxTiles = 1 << tileBits;
	nmp.maxPolys = 1 << ( 22 - tileBits );
	navMesh = dtAllocNavMesh();
	if ( !navMesh || dtStatusFailed( navMesh->init( &nmp ) ) ) {
		Fail( err, errSize, "dtNavMesh init failed" );
		OAXNav_Free();
		delete[] walk;
		return 0;
	}

	// cost volume area ids: equal costs share an id; a negative cost removes the surface
	for ( i = 0; i < 64; i++ ) {
		areaCost[i] = 1.0f;
	}
	unsigned char *volId = new unsigned char[numVols > 0 ? numVols : 1];
	for ( i = 0; i < numVols; i++ ) {
		unsigned char id = RC_NULL_AREA;
		if ( vols[i].cost >= 0.0f ) {
			for ( k = 1; k <= numHazardIds; k++ ) {
				if ( areaCost[k] == vols[i].cost ) {
					break;
				}
			}
			if ( k > numHazardIds ) {
				if ( numHazardIds >= NAV_AREA_HAZARD_MAX ) {
					k = NAV_AREA_HAZARD_MAX;	// out of ids: share the last one
				} else {
					k = ++numHazardIds;
					areaCost[k] = vols[i].cost;
				}
			}
			id = (unsigned char)k;
		}
		volId[i] = id;
	}

	rv = new float[numVerts * 3];
	areas = new unsigned char[numTris];
	triBounds = new float[numTris * 4];		// Q3 xy bounds
	tileTris = new int[numTris * 3 + 1];
	for ( i = 0; i < numVerts; i++ ) {
		ToRc( verts + i * 3, rv + i * 3 );
	}
	for ( i = 0; i < numTris; i++ ) {
		const float *a = verts + tris[i * 3] * 3, *b = verts + tris[i * 3 + 1] * 3, *c = verts + tris[i * 3 + 2] * 3;
		areas[i] = walk[i] ? RC_WALKABLE_AREA : RC_NULL_AREA;
		triBounds[i * 4 + 0] = rcMin( a[0], rcMin( b[0], c[0] ) );
		triBounds[i * 4 + 1] = rcMin( a[1], rcMin( b[1], c[1] ) );
		triBounds[i * 4 + 2] = rcMax( a[0], rcMax( b[0], c[0] ) );
		triBounds[i * 4 + 3] = rcMax( a[1], rcMax( b[1], c[1] ) );
	}
	if ( numLinks > 0 ) {
		conVerts = new float[numLinks * 6];
		conRad = new float[numLinks];
		conFlags = new unsigned short[numLinks];
		conAreas = new unsigned char[numLinks];
		conDir = new unsigned char[numLinks];
		conIds = new unsigned int[numLinks];
		for ( i = 0; i < numLinks; i++ ) {
			ToRc( links[i].start, conVerts + i * 6 );
			ToRc( links[i].end, conVerts + i * 6 + 3 );
			conRad[i] = links[i].radius > 0.0f ? links[i].radius : p->agentRadius * 2.0f;
			conFlags[i] = links[i].kind;
			conAreas[i] = links[i].kind == OAXNAV_LINK_TELEPORT ? NAV_AREA_TELEPORT : NAV_AREA_WALK;
			conDir[i] = links[i].bidir ? DT_OFFMESH_CON_BIDIR : 0;
			conIds[i] = (unsigned int)i;
		}
	}

	/*
	The tiles to build: with seed points (the spawn points), only tiles the
	walkers can reach: the seeds' tiles, then the neighbours a built tile's
	polygons open onto (portal edges) and the tiles its off-mesh links land
	in, breadth first. Floors no one can reach (an imported map's sky room,
	the bottom of its void) cost nothing. Without seeds, every tile.
	*/
	tileState = new unsigned char[tw * th];
	tileQueue = new int[tw * th];
	memset( tileState, 0, tw * th );
	if ( solids && solids->numSeeds > 0 ) {
		for ( i = 0; i < solids->numSeeds; i++ ) {
			const float *q = &solids->seeds[i * 3];
			int sx = (int)floorf( ( q[0] - wmin[0] ) / tileSize ), sy = (int)floorf( ( q[1] - wmin[1] ) / tileSize );
			if ( sx < 0 || sy < 0 || sx >= tw || sy >= th || tileState[sx + sy * tw] ) {
				continue;
			}
			tileState[sx + sy * tw] = 1;
			tileQueue[queueTail++] = sx + sy * tw;
		}
	} else {
		for ( i = 0; i < tw * th; i++ ) {
			tileState[i] = 1;
			tileQueue[queueTail++] = i;
		}
	}
	while ( queueHead < queueTail ) {
		{
			const int tileIndex = tileQueue[queueHead++];
			rcConfig cfg = base;
			float tmin[3], tmax[3], emin[2], emax[2];
			const float pad = base.borderSize * base.cs;
			int n = 0, anyWalk = 0, ntri;
			rcHeightfield *hf = NULL;
			rcCompactHeightfield *chf = NULL;
			rcContourSet *cset = NULL;
			rcPolyMesh *pmesh = NULL;
			rcPolyMeshDetail *dmesh = NULL;
			unsigned char *data = NULL;
			int dataSize = 0, tileOk = 0, restStart = 0, added = 0;
			const int tx = tileIndex % tw, ty = tileIndex / tw;
			dtNavMeshCreateParams cp;

			// Q3 bounds of this tile, and with the border
			tmin[0] = wmin[0] + tx * tileSize;
			tmin[1] = wmin[1] + ty * tileSize;
			tmin[2] = wmin[2];
			tmax[0] = tmin[0] + tileSize;
			tmax[1] = tmin[1] + tileSize;
			tmax[2] = wmax[2];
			emin[0] = tmin[0] - pad;
			emin[1] = tmin[1] - pad;
			emax[0] = tmax[0] + pad;
			emax[1] = tmax[1] + pad;
			for ( i = 0; i < numTris; i++ ) {
				const float *b = &triBounds[i * 4];
				if ( b[2] < emin[0] || b[0] > emax[0] || b[3] < emin[1] || b[1] > emax[1] ) {
					continue;
				}
				tileTris[n * 3 + 0] = tris[i * 3 + 0];
				tileTris[n * 3 + 1] = tris[i * 3 + 1];
				tileTris[n * 3 + 2] = tris[i * 3 + 2];
				anyWalk |= walk[i];
				n++;
			}
			if ( !anyWalk ) {
				continue;
			}
			ntri = n;
			{
				// the areas of the gathered triangles, in the same order
				unsigned char *tAreas = new unsigned char[ntri];
				n = 0;
				for ( i = 0; i < numTris; i++ ) {
					const float *b = &triBounds[i * 4];
					if ( b[2] < emin[0] || b[0] > emax[0] || b[3] < emin[1] || b[1] > emax[1] ) {
						continue;
					}
					tAreas[n++] = areas[i];
				}

				cfg.width = NAV_TILE_CELLS + cfg.borderSize * 2;
				cfg.height = NAV_TILE_CELLS + cfg.borderSize * 2;
				{
					float qmin[3], qmax[3];
					qmin[0] = emin[0];
					qmin[1] = emin[1];
					qmin[2] = tmin[2];
					qmax[0] = emax[0];
					qmax[1] = emax[1];
					qmax[2] = tmax[2];
					ToRc( qmin, cfg.bmin );
					ToRc( qmax, cfg.bmax );
				}
				hf = rcAllocHeightfield();
				chf = rcAllocCompactHeightfield();
				cset = rcAllocContourSet();
				pmesh = rcAllocPolyMesh();
				dmesh = rcAllocPolyMeshDetail();
				if ( !hf || !chf || !cset || !pmesh || !dmesh ) {
					Fail( err, errSize, "out of memory" );
				} else if ( !rcCreateHeightfield( &ctx, *hf, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch ) ) {
					Fail( err, errSize, "rcCreateHeightfield failed" );
				} else {
					int t0 = Sys_Milliseconds(), t1;
					if ( !rcRasterizeTriangles( &ctx, rv, numVerts, tileTris, tAreas, ntri, *hf, cfg.walkableClimb ) ) {
						Fail( err, errSize, "rcRasterizeTriangles failed" );
					} else {
						t1 = Sys_Milliseconds();
						navMsRaster += t1 - t0;
						if ( solids && solids->numVolumes > 0 && !NavFillSolids( *hf, solids, emin, emax, cfg.walkableClimb ) ) {
							Fail( err, errSize, "navmesh solid fill: out of memory" );
						} else {
							tileOk = 1;
						}
						navMsFill += Sys_Milliseconds() - t1;
					}
				}
				delete[] tAreas;
			}
			if ( !tileOk ) {
				goto tileDone;
			}
			tileOk = 0;
			restStart = Sys_Milliseconds();
			if ( solids && solids->openAt ) {
				NavDropOpaqueTops( *hf, solids );
			}
			rcFilterLowHangingWalkableObstacles( &ctx, cfg.walkableClimb, *hf );
			rcFilterLedgeSpans( &ctx, cfg.walkableHeight, cfg.walkableClimb, *hf );
			rcFilterWalkableLowHeightSpans( &ctx, cfg.walkableHeight, *hf );
			if ( !rcBuildCompactHeightfield( &ctx, cfg.walkableHeight, cfg.walkableClimb, *hf, *chf ) ) {
				Fail( err, errSize, "rcBuildCompactHeightfield failed" );
				goto tileDone;
			}
			rcFreeHeightField( hf );
			hf = NULL;
			if ( !rcErodeWalkableArea( &ctx, cfg.walkableRadius, *chf ) ) {
				Fail( err, errSize, "rcErodeWalkableArea failed" );
				goto tileDone;
			}
			// cost volumes: the walkable surface inside (or up to a step below) the box
			for ( i = 0; i < numVols; i++ ) {
				float lo[3], hi[3], q[3];
				rcVcopy( q, vols[i].mins );
				q[2] -= p->agentClimb;
				ToRc( q, lo );
				ToRc( vols[i].maxs, hi );
				rcMarkBoxArea( &ctx, lo, hi, volId[i], *chf );
			}
			if ( !rcBuildDistanceField( &ctx, *chf ) ||
				!rcBuildRegions( &ctx, *chf, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea ) ) {
				Fail( err, errSize, "rcBuildRegions failed" );
				goto tileDone;
			}
			if ( !rcBuildContours( &ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset ) ) {
				Fail( err, errSize, "rcBuildContours failed" );
				goto tileDone;
			}
			if ( !rcBuildPolyMesh( &ctx, *cset, cfg.maxVertsPerPoly, *pmesh ) ) {
				Fail( err, errSize, "rcBuildPolyMesh failed" );
				goto tileDone;
			}
			if ( !rcBuildPolyMeshDetail( &ctx, *pmesh, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *dmesh ) ) {
				Fail( err, errSize, "rcBuildPolyMeshDetail failed" );
				goto tileDone;
			}
			if ( pmesh->npolys <= 0 ) {
				tileOk = 1;		// nothing walkable here after all
				goto tileDone;
			}
			if ( pmesh->nverts >= 0xffff || pmesh->npolys >= ( 1 << ( 22 - tileBits ) ) ) {
				Fail( err, errSize, "too many navmesh vertices or polygons for one tile" );
				goto tileDone;
			}
			for ( i = 0; i < pmesh->npolys; i++ ) {
				unsigned char a = pmesh->areas[i];
				pmesh->flags[i] = a == NAV_AREA_WALK ? OAXNAV_FLAG_WALK :
					( a >= 1 && a <= NAV_AREA_HAZARD_MAX ) ? OAXNAV_FLAG_WALK | OAXNAV_FLAG_HAZARD : 0;
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
			cp.offMeshConVerts = conVerts;
			cp.offMeshConRad = conRad;
			cp.offMeshConFlags = conFlags;
			cp.offMeshConAreas = conAreas;
			cp.offMeshConDir = conDir;
			cp.offMeshConUserID = conIds;
			cp.offMeshConCount = numLinks > 0 ? numLinks : 0;
			cp.walkableHeight = p->agentHeight;
			cp.walkableRadius = p->agentRadius;
			cp.walkableClimb = p->agentClimb;
			cp.tileX = tx;
			cp.tileY = ty;
			cp.tileLayer = 0;
			rcVcopy( cp.bmin, pmesh->bmin );
			rcVcopy( cp.bmax, pmesh->bmax );
			cp.cs = cfg.cs;
			cp.ch = cfg.ch;
			cp.buildBvTree = true;
			if ( !dtCreateNavMeshData( &cp, &data, &dataSize ) ) {
				Fail( err, errSize, "dtCreateNavMeshData failed" );
				goto tileDone;
			}
			if ( dtStatusFailed( navMesh->addTile( data, dataSize, DT_TILE_FREE_DATA, 0, NULL ) ) ) {
				dtFree( data );
				Fail( err, errSize, "dtNavMesh addTile failed" );
				goto tileDone;
			}
			for ( i = 0; i < dataSize; i++ ) {
				hash = ( hash ^ data[i] ) * 16777619U;
			}
			totalSize += dataSize;
			polys += pmesh->npolys;
			navStatTiles++;
			added = 1;
			tileOk = 1;
tileDone:
			if ( restStart ) {
				navMsRest += Sys_Milliseconds() - restStart;
			}
			rcFreeHeightField( hf );
			rcFreeCompactHeightfield( chf );
			rcFreeContourSet( cset );
			rcFreePolyMesh( pmesh );
			rcFreePolyMeshDetail( dmesh );
			if ( !tileOk ) {
				OAXNav_Free();
				goto done;
			}
			if ( added && solids && solids->numSeeds > 0 ) {
				// queue what this tile opens onto
				const dtMeshTile *t = navMesh->getTileAt( tx, ty, 0 );
				static const int dx[8] = { 1, 1, 0, -1, -1, -1, 0, 1 }, dy[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
				int pi, e;
				for ( pi = 0; t && pi < t->header->polyCount; pi++ ) {
					const dtPoly *poly = &t->polys[pi];
					for ( e = 0; e < poly->vertCount; e++ ) {
						if ( poly->neis[e] & DT_EXT_LINK ) {
							const int side = poly->neis[e] & 0xff, nx = tx + dx[side], ny = ty + dy[side];
							if ( nx >= 0 && ny >= 0 && nx < tw && ny < th && !tileState[nx + ny * tw] ) {
								tileState[nx + ny * tw] = 1;
								tileQueue[queueTail++] = nx + ny * tw;
							}
						}
					}
				}
				// off-mesh links: from their start in this tile to the tile they
				// land in, and back along two-way links that land here
				for ( pi = 0; pi < numLinks; pi++ ) {
					const float *from = NULL, *to = NULL;
					int fx, fy, nx, ny;
					fx = (int)floorf( ( links[pi].start[0] - wmin[0] ) / tileSize );
					fy = (int)floorf( ( links[pi].start[1] - wmin[1] ) / tileSize );
					nx = (int)floorf( ( links[pi].end[0] - wmin[0] ) / tileSize );
					ny = (int)floorf( ( links[pi].end[1] - wmin[1] ) / tileSize );
					if ( fx == tx && fy == ty ) {
						from = links[pi].start;
						to = links[pi].end;
					} else if ( links[pi].bidir && nx == tx && ny == ty ) {
						from = links[pi].end;
						to = links[pi].start;
						nx = fx;
						ny = fy;
					}
					if ( !from || !to ) {
						continue;
					}
					if ( nx >= 0 && ny >= 0 && nx < tw && ny < th && !tileState[nx + ny * tw] ) {
						tileState[nx + ny * tw] = 1;
						tileQueue[queueTail++] = nx + ny * tw;
					}
				}
			}
		}
	}
	if ( polys <= 0 ) {
		Fail( err, errSize, "no walkable polygons" );
		OAXNav_Free();
		goto done;
	}
	navMesh->connectFarOffMeshLinks();
	navQuery = dtAllocNavMeshQuery();
	if ( !navQuery || dtStatusFailed( navQuery->init( navMesh, NAV_QUERY_NODES ) ) ) {
		Fail( err, errSize, "dtNavMeshQuery init failed" );
		OAXNav_Free();
		goto done;
	}
	navFilter.setIncludeFlags( OAXNAV_DEFAULT_INCLUDE );
	navFilter.setExcludeFlags( 0 );
	for ( i = 0; i < 64; i++ ) {
		navFilter.setAreaCost( i, areaCost[i] );
	}
	navFilter.setAreaCost( NAV_AREA_WALK, 1.0f );
	navFilter.setAreaCost( NAV_AREA_TELEPORT, NAV_TELEPORT_COST );
	navLinks = CountConnectedLinks();
	navPolys = polys;
	navDataSize = totalSize;
	navHash = hash;
	ok = navPolys;

done:
	delete[] tileState;
	delete[] tileQueue;
	delete[] walk;
	delete[] volId;
	delete[] rv;
	delete[] areas;
	delete[] triBounds;
	delete[] tileTris;
	delete[] conVerts;
	delete[] conRad;
	delete[] conFlags;
	delete[] conAreas;
	delete[] conDir;
	delete[] conIds;
	return ok;
}

int OAXNav_LinkCount( void ) {
	return navLinks;
}

int OAXNav_LinkConnected( int index ) {
	return index >= 0 && index < NAV_MAX_LINK_STATE && navLinkOk[index];
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

static dtPolyRef Nearest( const float *q3, const float *halfExtentsQ3, float *outRc, const dtQueryFilter *filter = &navFilter ) {
	float p[3], ext[3];
	dtPolyRef ref = 0;

	ToRc( q3, p );
	ToRc( halfExtentsQ3, ext );
	if ( dtStatusFailed( navQuery->findNearestPoly( p, ext, filter, &ref, outRc ) ) ) {
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
	return OAXNav_FindPathEx( start, goal, halfExtents, points, NULL, maxPoints, flags, OAXNAV_DEFAULT_INCLUDE, 0 );
}

int OAXNav_FindPathEx( const float start[3], const float goal[3], const float halfExtents[3],
		float *points, int *links, int maxPoints, int *flags, int include, int exclude ) {
	dtPolyRef sref, gref, path[NAV_MAX_POLYS_PATH], straightRefs[64];
	float s[3], g[3], straight[64 * 3];
	unsigned char straightFlags[64];
	int npath = 0, nstraight = 0, i, n;
	dtStatus st;
	dtQueryFilter filter = navFilter;

	if ( flags ) {
		*flags = 0;
	}
	if ( !navQuery || maxPoints <= 0 ) {
		return 0;
	}
	filter.setIncludeFlags( (unsigned short)include );
	filter.setExcludeFlags( (unsigned short)exclude );
	sref = Nearest( start, halfExtents, s, &filter );
	gref = Nearest( goal, halfExtents, g, &filter );
	if ( !sref || !gref ) {
		return 0;
	}
	st = navQuery->findPath( sref, gref, s, g, &filter, path, &npath, NAV_MAX_POLYS_PATH );
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
	if ( dtStatusFailed( navQuery->findStraightPath( s, g, path, npath, straight, straightFlags, straightRefs, &nstraight, n, 0 ) ) ) {
		return 0;
	}
	for ( i = 0; i < nstraight; i++ ) {
		ToQ3( straight + i * 3, points + i * 3 );
		if ( links ) {
			const dtOffMeshConnection *con = NULL;
			if ( straightFlags[i] & DT_STRAIGHTPATH_OFFMESH_CONNECTION ) {
				con = navMesh->getOffMeshConnectionByRef( straightRefs[i] );
			}
			links[i] = con ? (int)con->userId : -1;
		}
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
