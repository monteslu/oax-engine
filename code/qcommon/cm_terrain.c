/*
===========================================================================
cm_terrain.c: heightmap terrain collision (see cm_terrain.h).

Each terrain triangle collides as a convex brush: the triangle on top, a
flat bottom at the terrain's `bottom`, three vertical sides, and every
supporting plane Q3 needs to sweep a box through a brush correctly (the six
axial planes and the bevels of each top edge with the x and y axes). The
brush is built on the fly and handed to CM_TraceThroughBrush /
CM_TestBoxInBrush, so terrain obeys exactly the brush rules (epsilons,
startsolid/allsolid, the plane reported to pmove).

A min/max height pyramid over the cells culls the cells a trace cannot
touch: a block is visited only when the swept box overlaps its bounds.

All arithmetic is plain float +, -, *, / and sqrtf (correctly rounded on
every build) under -ffp-contract=off, so native and wasm agree bit for bit.
===========================================================================
*/

#include "cm_local.h"
#include "cm_terrain.h"
#include "oax.h"

#define MAX_PRISM_PLANES	32
#define MAX_PYRAMID			12

typedef struct {
	oaxTerrainInfo_t	info;
	float		*z;						// samplesX * samplesY
	int			cellsX, cellsY;
	int			levels;
	int			lw[MAX_PYRAMID], lh[MAX_PYRAMID];
	float		*lmax[MAX_PYRAMID];		// per block surface max
	float		*lmin[MAX_PYRAMID];		// per block surface min
	vec3_t		mins, maxs;
} cmTerrain_t;

typedef struct {
	vec3_t		bounds[2];
} cmTrunk_t;

static cmTerrain_t	cmTerrains[OAX_TERRAIN_MAX];
static int			cmNumTerrains;
static cmTrunk_t	*cmTrunks;
static int			cmNumTrunks;
static vec3_t		cmTerrainMins, cmTerrainMaxs;	// all terrains and trunks

static cvar_t		*cm_noTerrain;

void CM_OAXTerrainInitCommands( void );

// the brush a triangle or trunk is turned into
static cplane_t		prismPlanes[MAX_PRISM_PLANES];
static cbrushside_t	prismSides[MAX_PRISM_PLANES];
static cbrush_t		prismBrush;

int c_terrain_prisms;

/*
=================
CM_OAXTerrainClear
=================
*/
void CM_OAXTerrainClear( void ) {
	cmNumTerrains = 0;
	cmNumTrunks = 0;
	cmTrunks = NULL;
	Com_Memset( cmTerrains, 0, sizeof( cmTerrains ) );
}

static void BuildPyramid( cmTerrain_t *t ) {
	int l, i, j, w = t->cellsX, h = t->cellsY;

	t->lw[0] = w;
	t->lh[0] = h;
	t->lmax[0] = Hunk_Alloc( w * h * sizeof( float ), h_high );
	t->lmin[0] = Hunk_Alloc( w * h * sizeof( float ), h_high );
	for ( j = 0; j < h; j++ ) {
		for ( i = 0; i < w; i++ ) {
			const float *z = t->z + j * t->info.samplesX + i;
			float a = z[0], b = z[1], c = z[t->info.samplesX], d = z[t->info.samplesX + 1];
			float hi = a, lo = a;
			hi = b > hi ? b : hi;
			hi = c > hi ? c : hi;
			hi = d > hi ? d : hi;
			lo = b < lo ? b : lo;
			lo = c < lo ? c : lo;
			lo = d < lo ? d : lo;
			t->lmax[0][j * w + i] = hi;
			t->lmin[0][j * w + i] = lo;
		}
	}
	for ( l = 1; l < MAX_PYRAMID && ( t->lw[l - 1] > 1 || t->lh[l - 1] > 1 ); l++ ) {
		int pw = t->lw[l - 1], ph = t->lh[l - 1];
		w = ( pw + 1 ) / 2;
		h = ( ph + 1 ) / 2;
		t->lw[l] = w;
		t->lh[l] = h;
		t->lmax[l] = Hunk_Alloc( w * h * sizeof( float ), h_high );
		t->lmin[l] = Hunk_Alloc( w * h * sizeof( float ), h_high );
		for ( j = 0; j < h; j++ ) {
			for ( i = 0; i < w; i++ ) {
				float hi = -MAX_WORLD_COORD * 4.0f, lo = MAX_WORLD_COORD * 4.0f;
				int di, dj;
				for ( dj = 0; dj < 2; dj++ ) {
					for ( di = 0; di < 2; di++ ) {
						int ci = i * 2 + di, cj = j * 2 + dj;
						if ( ci >= pw || cj >= ph ) {
							continue;
						}
						if ( t->lmax[l - 1][cj * pw + ci] > hi ) hi = t->lmax[l - 1][cj * pw + ci];
						if ( t->lmin[l - 1][cj * pw + ci] < lo ) lo = t->lmin[l - 1][cj * pw + ci];
					}
				}
				t->lmax[l][j * w + i] = hi;
				t->lmin[l][j * w + i] = lo;
			}
		}
	}
	t->levels = l;
}

/*
=================
CM_OAXTerrainLoad

Called from CM_LoadMap with the whole BSP file. A map without the lump has
no terrain, and everything below is skipped.
=================
*/
void CM_OAXTerrainLoad( const void *bsp, int bspLen ) {
	oaxTerrainInfo_t infos[OAX_TERRAIN_MAX];
	const void *lump;
	int len, n, k, f, i, j, trunks = 0;
	char err[128];

	CM_OAXTerrainClear();
	if ( !cm_noTerrain ) {
		cm_noTerrain = Cvar_Get( "cm_noTerrain", "0", CVAR_CHEAT );
		Cvar_SetDescription( cm_noTerrain, "Debug: the collision model ignores oax heightmap terrain." );
		CM_OAXTerrainInitCommands();
	}
	lump = OAXTerrain_FindLump( bsp, bspLen, &len );
	if ( !lump ) {
		return;
	}
	n = OAXTerrain_Parse( lump, len, infos, OAX_TERRAIN_MAX, err, sizeof( err ) );
	if ( !n ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: OAX_TERRAIN ignored: %s\n", err );
		return;
	}
	ClearBounds( cmTerrainMins, cmTerrainMaxs );
	for ( k = 0; k < n; k++ ) {
		cmTerrain_t *t = &cmTerrains[k];
		int sx = infos[k].samplesX, sy = infos[k].samplesY;
		byte *copy;

		t->info = infos[k];
		// the lump lives in the BSP file buffer, which is freed after loading
		copy = Hunk_Alloc( sx * sy * 10, h_high );
		Com_Memcpy( copy, infos[k].heights, sx * sy * 2 );
		Com_Memcpy( copy + sx * sy * 2, infos[k].splat, sx * sy * 4 );
		Com_Memcpy( copy + sx * sy * 6, infos[k].density, sx * sy * 4 );
		t->info.heights = copy;
		t->info.splat = copy + sx * sy * 2;
		t->info.density = copy + sx * sy * 6;
		t->z = Hunk_Alloc( sx * sy * sizeof( float ), h_high );
		for ( j = 0; j < sy; j++ ) {
			for ( i = 0; i < sx; i++ ) {
				t->z[j * sx + i] = OAXTerrain_SampleZ( &t->info, i, j );
			}
		}
		t->cellsX = sx - 1;
		t->cellsY = sy - 1;
		BuildPyramid( t );
		OAXTerrain_Bounds( &t->info, t->mins, t->maxs );
		AddPointToBounds( t->mins, cmTerrainMins, cmTerrainMaxs );
		AddPointToBounds( t->maxs, cmTerrainMins, cmTerrainMaxs );
		for ( f = 0; f < t->info.numFoliage; f++ ) {
			oaxFoliageInstance_t inst[OAX_FOLIAGE_MAX_PER_CELL];
			if ( !( t->info.foliage[f].collideRadius > 0.0f ) ) {
				continue;
			}
			for ( j = 0; j < t->cellsY; j++ ) {
				for ( i = 0; i < t->cellsX; i++ ) {
					trunks += OAXTerrain_CellFoliage( &t->info, f, i, j, inst );
				}
			}
		}
	}
	cmNumTerrains = n;

	// collidable foliage trunks
	if ( trunks ) {
		cmTrunks = Hunk_Alloc( trunks * sizeof( cmTrunk_t ), h_high );
		for ( k = 0; k < n; k++ ) {
			cmTerrain_t *t = &cmTerrains[k];
			for ( f = 0; f < t->info.numFoliage; f++ ) {
				const oaxFoliageDisk_t *fd = &t->info.foliage[f];
				oaxFoliageInstance_t inst[OAX_FOLIAGE_MAX_PER_CELL];
				if ( !( fd->collideRadius > 0.0f ) ) {
					continue;
				}
				for ( j = 0; j < t->cellsY; j++ ) {
					for ( i = 0; i < t->cellsX; i++ ) {
						int c = OAXTerrain_CellFoliage( &t->info, f, i, j, inst ), m;
						for ( m = 0; m < c && cmNumTrunks < trunks; m++ ) {
							cmTrunk_t *tr = &cmTrunks[cmNumTrunks++];
							float r = fd->collideRadius;
							tr->bounds[0][0] = inst[m].origin[0] - r;
							tr->bounds[0][1] = inst[m].origin[1] - r;
							tr->bounds[0][2] = inst[m].origin[2] - 16.0f;
							tr->bounds[1][0] = inst[m].origin[0] + r;
							tr->bounds[1][1] = inst[m].origin[1] + r;
							tr->bounds[1][2] = inst[m].origin[2] + fd->collideHeight;	// absolute (scale is the tree's render size)
							AddPointToBounds( tr->bounds[0], cmTerrainMins, cmTerrainMaxs );
							AddPointToBounds( tr->bounds[1], cmTerrainMins, cmTerrainMaxs );
						}
					}
				}
			}
		}
	}

	for ( k = 0; k < MAX_PRISM_PLANES; k++ ) {
		prismSides[k].plane = &prismPlanes[k];
	}
	prismBrush.sides = prismSides;

	Com_Printf( "OAX_TERRAIN: %i terrain(s), %i trunks, hash %08x\n", n, cmNumTrunks, CM_OAXTerrainHash() );
	Com_DebugSetInt( "cm_terrains", n );
	Com_DebugSetInt( "cm_terrain_trunks", cmNumTrunks );
	Com_DebugSet( "cm_terrain_hash", va( "%08x", CM_OAXTerrainHash() ) );
}

// ---- accessors -------------------------------------------------------------------

int CM_OAXNumTerrains( void ) {
	return cmNumTerrains;
}

const oaxTerrainInfo_t *CM_OAXTerrainInfo( int n ) {
	return n >= 0 && n < cmNumTerrains ? &cmTerrains[n].info : NULL;
}

const float *CM_OAXTerrainHeights( int n ) {
	return n >= 0 && n < cmNumTerrains ? cmTerrains[n].z : NULL;
}

void CM_OAXTerrainBounds( int n, vec3_t mins, vec3_t maxs ) {
	if ( n >= 0 && n < cmNumTerrains ) {
		VectorCopy( cmTerrains[n].mins, mins );
		VectorCopy( cmTerrains[n].maxs, maxs );
	}
}

int CM_OAXNumTrunks( void ) {
	return cmNumTrunks;
}

void CM_OAXTrunkBounds( int n, vec3_t mins, vec3_t maxs ) {
	if ( n >= 0 && n < cmNumTrunks ) {
		VectorCopy( cmTrunks[n].bounds[0], mins );
		VectorCopy( cmTrunks[n].bounds[1], maxs );
	}
}

unsigned CM_OAXTerrainHash( void ) {
	unsigned h = 2166136261U;
	int k, i;

	for ( k = 0; k < cmNumTerrains; k++ ) {
		const byte *p = (const byte *)cmTerrains[k].z;
		int n = cmTerrains[k].info.samplesX * cmTerrains[k].info.samplesY * sizeof( float );
		for ( i = 0; i < n; i++ ) {
			h = ( h ^ p[i] ) * 16777619U;
		}
	}
	for ( k = 0; k < cmNumTrunks; k++ ) {
		const byte *p = (const byte *)&cmTrunks[k];
		for ( i = 0; i < (int)sizeof( cmTrunk_t ); i++ ) {
			h = ( h ^ p[i] ) * 16777619U;
		}
	}
	return h;
}

// ---- brushes from triangles -------------------------------------------------------

// adds the supporting plane of the points with outward normal n (unnormalized)
static void AddSupportPlane( float nx, float ny, float nz, vec3_t *pts, int numPts ) {
	cplane_t *p;
	float l, d, best;
	int i;

	if ( prismBrush.numsides >= MAX_PRISM_PLANES ) {
		return;
	}
	l = sqrtf( nx * nx + ny * ny + nz * nz );
	if ( !( l > 1e-6f ) ) {
		return;
	}
	nx /= l; ny /= l; nz /= l;
	// skip a duplicate of a plane already present
	for ( i = 0; i < prismBrush.numsides; i++ ) {
		p = &prismPlanes[i];
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
	p = &prismPlanes[prismBrush.numsides++];
	VectorSet( p->normal, nx, ny, nz );
	p->dist = best;
	p->type = PlaneTypeForNormal( p->normal );
	SetPlaneSignbits( p );
}

// the six axial planes first, as CM_TestBoxInBrush expects
static void StartBoxBrush( const vec3_t mins, const vec3_t maxs, int contents, int surfaceFlags ) {
	int i;

	prismBrush.numsides = 6;
	prismBrush.contents = contents;
	VectorCopy( mins, prismBrush.bounds[0] );
	VectorCopy( maxs, prismBrush.bounds[1] );
	for ( i = 0; i < 3; i++ ) {
		cplane_t *p = &prismPlanes[i * 2];
		VectorClear( p->normal );
		p->normal[i] = -1;
		p->dist = -mins[i];
		p->type = PlaneTypeForNormal( p->normal );
		SetPlaneSignbits( p );
		p = &prismPlanes[i * 2 + 1];
		VectorClear( p->normal );
		p->normal[i] = 1;
		p->dist = maxs[i];
		p->type = i;
		SetPlaneSignbits( p );
	}
	for ( i = 0; i < MAX_PRISM_PLANES; i++ ) {
		prismSides[i].surfaceFlags = surfaceFlags;
		prismSides[i].shaderNum = 0;
	}
}

// triangle tri (counter-clockwise from above) down to z = bottom
static void BuildPrism( float tri[3][3], float bottom, int contents, int surfaceFlags ) {
	vec3_t pts[6], mins, maxs;
	float e1[3], e2[3], n[3];
	int i;

	for ( i = 0; i < 3; i++ ) {
		VectorCopy( tri[i], pts[i] );
		VectorSet( pts[i + 3], tri[i][0], tri[i][1], bottom );
	}
	ClearBounds( mins, maxs );
	for ( i = 0; i < 6; i++ ) {
		AddPointToBounds( pts[i], mins, maxs );
	}
	StartBoxBrush( mins, maxs, contents, surfaceFlags );

	// top face
	VectorSubtract( tri[1], tri[0], e1 );
	VectorSubtract( tri[2], tri[0], e2 );
	n[0] = e1[1] * e2[2] - e1[2] * e2[1];
	n[1] = e1[2] * e2[0] - e1[0] * e2[2];
	n[2] = e1[0] * e2[1] - e1[1] * e2[0];
	AddSupportPlane( n[0], n[1], n[2], pts, 6 );

	// sides and edge bevels: each top edge crossed with the axes
	for ( i = 0; i < 3; i++ ) {
		float *a = tri[i], *b = tri[( i + 1 ) % 3];
		float ex = b[0] - a[0], ey = b[1] - a[1], ez = b[2] - a[2];
		// e x z (vertical side)
		AddSupportPlane( ey, -ex, 0, pts, 6 );
		// e x x and its opposite
		AddSupportPlane( 0, ez, -ey, pts, 6 );
		AddSupportPlane( 0, -ez, ey, pts, 6 );
		// e x y and its opposite
		AddSupportPlane( -ez, 0, ex, pts, 6 );
		AddSupportPlane( ez, 0, -ex, pts, 6 );
	}
	c_terrain_prisms++;
}

// ---- traversal ---------------------------------------------------------------------

typedef struct {
	traceWork_t	*tw;
	vec3_t		ext;		// half extents of the swept volume around the trace line
	vec3_t		dir;		// end - start
	qboolean	position;	// position test (start == end)
} terrainWork_t;

// can the swept box touch the axial box [lo, hi]?
static qboolean SweepTouchesBox( const terrainWork_t *w, const vec3_t lo, const vec3_t hi ) {
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
	// slab test of the trace line against the box grown by the swept extents
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

static void TerrainCell( terrainWork_t *w, cmTerrain_t *t, int i, int j ) {
	traceWork_t *tw = w->tw;
	float tri[2][3][3];
	int k;

	OAXTerrain_CellTriangles( &t->info, i, j, tri );
	for ( k = 0; k < 2; k++ ) {
		BuildPrism( tri[k], t->info.bottom, t->info.contents, t->info.surfaceFlags );
		if ( !CM_BoundsIntersect( tw->bounds[0], tw->bounds[1], prismBrush.bounds[0], prismBrush.bounds[1] ) ) {
			continue;
		}
		if ( w->position ) {
			CM_TestBoxInBrush( tw, &prismBrush );
			if ( tw->trace.allsolid ) {
				return;
			}
		} else {
			CM_TraceThroughBrush( tw, &prismBrush );
			if ( !tw->trace.fraction ) {
				return;
			}
		}
	}
}

static void TerrainBlock( terrainWork_t *w, cmTerrain_t *t, int level, int bi, int bj ) {
	int span = 1 << level, i0 = bi * span, j0 = bj * span;
	vec3_t lo, hi;

	if ( bi >= t->lw[level] || bj >= t->lh[level] ) {
		return;
	}
	lo[0] = t->info.origin[0] + (float)i0 * t->info.cellSize;
	lo[1] = t->info.origin[1] + (float)j0 * t->info.cellSize;
	lo[2] = t->info.bottom;
	hi[0] = t->info.origin[0] + (float)( i0 + span < t->cellsX ? i0 + span : t->cellsX ) * t->info.cellSize;
	hi[1] = t->info.origin[1] + (float)( j0 + span < t->cellsY ? j0 + span : t->cellsY ) * t->info.cellSize;
	hi[2] = t->lmax[level][bj * t->lw[level] + bi];
	if ( !SweepTouchesBox( w, lo, hi ) ) {
		return;
	}
	if ( level == 0 ) {
		TerrainCell( w, t, bi, bj );
		return;
	}
	TerrainBlock( w, t, level - 1, bi * 2, bj * 2 );
	TerrainBlock( w, t, level - 1, bi * 2 + 1, bj * 2 );
	TerrainBlock( w, t, level - 1, bi * 2, bj * 2 + 1 );
	TerrainBlock( w, t, level - 1, bi * 2 + 1, bj * 2 + 1 );
}

static void TerrainWork( traceWork_t *tw, qboolean position ) {
	terrainWork_t w;
	int k, i;

	if ( ( !cmNumTerrains && !cmNumTrunks ) || cm_noTerrain->integer ) {
		return;
	}
	if ( !CM_BoundsIntersect( tw->bounds[0], tw->bounds[1], cmTerrainMins, cmTerrainMaxs ) ) {
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
	for ( k = 0; k < cmNumTerrains; k++ ) {
		cmTerrain_t *t = &cmTerrains[k];
		if ( !( t->info.contents & tw->contents ) ) {
			continue;
		}
		TerrainBlock( &w, t, t->levels - 1, 0, 0 );
		if ( position ? tw->trace.allsolid : !tw->trace.fraction ) {
			return;
		}
	}
	for ( k = 0; k < cmNumTrunks; k++ ) {
		cmTrunk_t *tr = &cmTrunks[k];
		if ( !( CONTENTS_SOLID & tw->contents ) || !CM_BoundsIntersect( tw->bounds[0], tw->bounds[1], tr->bounds[0], tr->bounds[1] ) ) {
			continue;
		}
		StartBoxBrush( tr->bounds[0], tr->bounds[1], CONTENTS_SOLID, 0 );
		if ( position ) {
			CM_TestBoxInBrush( tw, &prismBrush );
			if ( tw->trace.allsolid ) {
				return;
			}
		} else {
			CM_TraceThroughBrush( tw, &prismBrush );
			if ( !tw->trace.fraction ) {
				return;
			}
		}
	}
}

void CM_OAXTerrainTrace( traceWork_t *tw ) {
	TerrainWork( tw, qfalse );
}

void CM_OAXTerrainPositionTest( traceWork_t *tw ) {
	if ( !tw->trace.allsolid ) {
		TerrainWork( tw, qtrue );
	}
}

/*
=================
CM_OAXTerrainPointContents

Solid below a terrain's surface (down to its bottom) and inside trunks.
=================
*/
int CM_OAXTerrainPointContents( const vec3_t p ) {
	int k, contents = 0;

	if ( ( !cmNumTerrains && !cmNumTrunks ) || cm_noTerrain->integer ) {
		return 0;
	}
	if ( !CM_BoundsIntersectPoint( cmTerrainMins, cmTerrainMaxs, p ) ) {
		return 0;
	}
	for ( k = 0; k < cmNumTerrains; k++ ) {
		cmTerrain_t *t = &cmTerrains[k];
		float z;
		if ( p[2] >= t->info.bottom && OAXTerrain_HeightAt( &t->info, p[0], p[1], &z, NULL ) && p[2] <= z ) {
			contents |= t->info.contents;
		}
	}
	for ( k = 0; k < cmNumTrunks; k++ ) {
		if ( CM_BoundsIntersectPoint( cmTrunks[k].bounds[0], cmTrunks[k].bounds[1], p ) ) {
			contents |= CONTENTS_SOLID;
		}
	}
	return contents;
}

/*
=================
CM_OAXTrace_f

cm_trace sx sy sz ex ey ez [minx miny minz maxx maxy maxz]: a world trace
(every brush plus terrain) printed and published as the debug value
"cm_trace" ("fraction endpos normal startsolid allsolid"), for tests.
=================
*/
static void CM_OAXTrace_f( void ) {
	vec3_t start, end, mins = { 0, 0, 0 }, maxs = { 0, 0, 0 };
	trace_t tr;
	char buf[256];
	int i;

	if ( Cmd_Argc() != 7 && Cmd_Argc() != 13 ) {
		Com_Printf( "usage: cm_trace sx sy sz ex ey ez [minx miny minz maxx maxy maxz]\n" );
		return;
	}
	for ( i = 0; i < 3; i++ ) {
		start[i] = atof( Cmd_Argv( 1 + i ) );
		end[i] = atof( Cmd_Argv( 4 + i ) );
		if ( Cmd_Argc() == 13 ) {
			mins[i] = atof( Cmd_Argv( 7 + i ) );
			maxs[i] = atof( Cmd_Argv( 10 + i ) );
		}
	}
	CM_BoxTrace( &tr, start, end, mins, maxs, 0, CONTENTS_SOLID | CONTENTS_PLAYERCLIP | CONTENTS_BODY, qfalse );
	Com_sprintf( buf, sizeof( buf ), "%.9g %.9g %.9g %.9g %.9g %.9g %.9g %i %i", tr.fraction,
		tr.endpos[0], tr.endpos[1], tr.endpos[2], tr.plane.normal[0], tr.plane.normal[1], tr.plane.normal[2],
		tr.startsolid, tr.allsolid );
	Com_Printf( "cm_trace: %s\n", buf );
	Com_DebugSet( "cm_trace", buf );
}

void CM_OAXTerrainInitCommands( void ) {
	static qboolean done;

	if ( !done ) {
		Cmd_AddCommand( "cm_trace", CM_OAXTrace_f );
		done = qtrue;
	}
}
