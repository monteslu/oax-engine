/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/
/*
tb_ulight_stencil.c: unified lighting, stencil shadow volumes (the A/B
prototype of DESIGN 3.6; shadow maps stay the default).

Adapted from DOOM-3 neo/renderer/tr_stencilshadow.cpp (R_AddSilEdges: an
edge is a silhouette when exactly one of its faces casts, with unmatched
edges counting as silhouettes; PointsOrdered: a consistent split of each
silhouette quad) and neo/renderer/draw_common.cpp (RB_StencilShadowPass:
depth-fail counting, back faces increment and front faces decrement).
Changes: C on renderergl2 data; world casters are the BSP's brushes (convex
and closed, so their silhouettes are exact); entity casters are MD3
surfaces with CPU silhouettes from welded edge adjacency; volumes extrude a
finite distance past the light and the vertex stage clamps them to the far
plane (ES 3.0 has no depth clamp) instead of projecting to infinity; no
turbo shadows, no clipping to the light frustum; the quads' winding is
fixed against the caster's centroid instead of id's index order.
*/

#include "tr_local.h"

#ifndef GL_INCR_WRAP
#define GL_INCR_WRAP 0x8507
#define GL_DECR_WRAP 0x8508
#endif

extern const char *fallbackShader_interaction_vp;
extern const char *fallbackShader_interaction_fp;

typedef struct {
	vec3_t normal;
	float  dist;
	int    firstVert, numVerts;
} sFace_t;

typedef struct {
	int    firstFace, numFaces;
	vec3_t mins, maxs;
} sBrush_t;

typedef struct {
	int firstBrush, numBrushes;
} sModel_t;

static sBrush_t *sBrushes;
static int       numSBrushes;
static sFace_t  *sFaces;
static int       numSFaces;
static vec3_t   *sVerts;
static int       numSVerts;
static sModel_t *sModels;
static int       numSModels;

// cached world volumes of the static map lights
typedef struct {
	vao_t *vao;
	int    numIndexes;
	int    stamp;       // light derivedCount the volume was built for
} sLightVolume_t;

static sLightVolume_t *sLightVolumes;
static int             numSLightVolumes;

// silhouette adjacency of MD3 surfaces, built on first use
typedef struct sMeshAdj_s {
	const mdvSurface_t *surf;
	int   numEdges;
	int  *edges;        // v1, v2 (welded), t1, t2 (-1: unmatched)
	int  *weld;         // vertex -> welded vertex
	struct sMeshAdj_s *next;
} sMeshAdj_t;

static sMeshAdj_t *meshAdj;

static shaderProgram_t volumeProgram;
static qboolean        volumeProgramValid;

// volume triangles being built (positions, three per triangle)
#define MAX_VOLUME_VERTS 196608
static vec3_t *vbuf;
static int     vbufCount;

/*
=====================================================================

LOADING: brush polyhedra

=====================================================================
*/

static int ClipWinding( vec3_t *in, int n, vec3_t *out, const vec3_t normal, float dist, int max ) {
	int i, o = 0;

	// keep the back side (inside the brush): normal . p - dist <= 0
	for ( i = 0; i < n && o < max - 1; i++ ) {
		float *a = in[i], *b = in[( i + 1 ) % n];
		float da = DotProduct( a, normal ) - dist, db = DotProduct( b, normal ) - dist;

		if ( da <= 0.01f ) {
			VectorCopy( a, out[o] );
			o++;
		}
		if ( ( da < -0.01f && db > 0.01f ) || ( da > 0.01f && db < -0.01f ) ) {
			float t = da / ( da - db );

			out[o][0] = a[0] + t * ( b[0] - a[0] );
			out[o][1] = a[1] + t * ( b[1] - a[1] );
			out[o][2] = a[2] + t * ( b[2] - a[2] );
			o++;
		}
	}
	return o;
}

// a big quad on the plane, wound counter-clockwise seen from the normal side
static int BaseWinding( const vec3_t normal, float dist, vec3_t *w ) {
	vec3_t org, u, v;
	float size = 65536;

	VectorScale( normal, dist, org );
	PerpendicularVector( u, normal );
	CrossProduct( normal, u, v );
	VectorScale( u, size, u );
	VectorScale( v, size, v );
	VectorSubtract( org, u, w[0] ); VectorSubtract( w[0], v, w[0] );
	VectorAdd( org, u, w[1] );      VectorSubtract( w[1], v, w[1] );
	VectorAdd( org, u, w[2] );      VectorAdd( w[2], v, w[2] );
	VectorSubtract( org, u, w[3] ); VectorAdd( w[3], v, w[3] );
	return 4;
}

void R_ULightStencilLoadWorld( const void *header ) {
	const dheader_t *h = header;
	const byte *base = header;
	const dbrush_t *brushes;
	const dbrushside_t *sides;
	const dmodel_t *models;
	const dshader_t *shaders;
	int numBrushes, numSides, numModels, numShaders, i, j, k;
	int maxFaces, maxVerts;

	R_ULightStencilFreeWorld();
	if ( !h || !tr.world ) {
		return;
	}
	brushes = (const dbrush_t *)( base + LittleLong( h->lumps[LUMP_BRUSHES].fileofs ) );
	numBrushes = LittleLong( h->lumps[LUMP_BRUSHES].filelen ) / sizeof( dbrush_t );
	sides = (const dbrushside_t *)( base + LittleLong( h->lumps[LUMP_BRUSHSIDES].fileofs ) );
	numSides = LittleLong( h->lumps[LUMP_BRUSHSIDES].filelen ) / sizeof( dbrushside_t );
	models = (const dmodel_t *)( base + LittleLong( h->lumps[LUMP_MODELS].fileofs ) );
	numModels = LittleLong( h->lumps[LUMP_MODELS].filelen ) / sizeof( dmodel_t );
	shaders = (const dshader_t *)( base + LittleLong( h->lumps[LUMP_SHADERS].fileofs ) );
	numShaders = LittleLong( h->lumps[LUMP_SHADERS].filelen ) / sizeof( dshader_t );

	maxFaces = numSides;
	maxVerts = numSides * 24;
	sBrushes = ri.Malloc( sizeof( sBrush_t ) * ( numBrushes + 1 ) );
	sFaces = ri.Malloc( sizeof( sFace_t ) * ( maxFaces + 1 ) );
	sVerts = ri.Malloc( sizeof( vec3_t ) * ( maxVerts + 1 ) );
	sModels = ri.Malloc( sizeof( sModel_t ) * ( numModels + 1 ) );
	numSBrushes = numSFaces = numSVerts = 0;
	numSModels = numModels;

	for ( k = 0; k < numModels; k++ ) {
		int first = LittleLong( models[k].firstBrush ), num = LittleLong( models[k].numBrushes );

		sModels[k].firstBrush = numSBrushes;
		for ( i = first; i < first + num && i < numBrushes; i++ ) {
			const dbrush_t *b = &brushes[i];
			int sh = LittleLong( b->shaderNum ), fs = LittleLong( b->firstSide ), ns = LittleLong( b->numSides );
			sBrush_t *sb;

			if ( sh < 0 || sh >= numShaders || !( LittleLong( shaders[sh].contentFlags ) & CONTENTS_SOLID )
				|| ( LittleLong( shaders[sh].contentFlags ) & CONTENTS_TRANSLUCENT ) ) {
				continue;
			}
			sb = &sBrushes[numSBrushes];
			sb->firstFace = numSFaces;
			sb->numFaces = 0;
			ClearBounds( sb->mins, sb->maxs );
			for ( j = 0; j < ns; j++ ) {
				vec3_t wa[64], wb[64];
				int n, m, pn = LittleLong( sides[fs + j].planeNum );
				cplane_t *p;

				if ( pn < 0 || pn >= tr.world->numplanes ) {
					continue;
				}
				p = &tr.world->planes[pn];
				n = BaseWinding( p->normal, p->dist, wa );
				for ( m = 0; m < ns && n >= 3; m++ ) {
					int qn = LittleLong( sides[fs + m].planeNum );
					cplane_t *q;

					if ( m == j || qn < 0 || qn >= tr.world->numplanes ) {
						continue;
					}
					q = &tr.world->planes[qn];
					n = ClipWinding( wa, n, wb, q->normal, q->dist, 64 );
					Com_Memcpy( wa, wb, sizeof( vec3_t ) * n );
				}
				if ( n < 3 || numSVerts + n > maxVerts || numSFaces >= maxFaces ) {
					continue;
				}
				VectorCopy( p->normal, sFaces[numSFaces].normal );
				sFaces[numSFaces].dist = p->dist;
				sFaces[numSFaces].firstVert = numSVerts;
				sFaces[numSFaces].numVerts = n;
				for ( m = 0; m < n; m++ ) {
					VectorCopy( wa[m], sVerts[numSVerts + m] );
					AddPointToBounds( wa[m], sb->mins, sb->maxs );
				}
				numSVerts += n;
				numSFaces++;
				sb->numFaces++;
			}
			if ( sb->numFaces >= 4 ) {
				numSBrushes++;
			} else {
				numSFaces = sb->firstFace;
			}
		}
		sModels[k].numBrushes = numSBrushes - sModels[k].firstBrush;
	}
	ri.Printf( PRINT_DEVELOPER, "unified lighting: %d shadow brushes, %d faces\n", numSBrushes, numSFaces );
}

void R_ULightStencilFreeWorld( void ) {
	sMeshAdj_t *a, *next;

	if ( sBrushes ) ri.Free( sBrushes );
	if ( sFaces ) ri.Free( sFaces );
	if ( sVerts ) ri.Free( sVerts );
	if ( sModels ) ri.Free( sModels );
	if ( sLightVolumes ) ri.Free( sLightVolumes );
	if ( vbuf ) ri.Free( vbuf );
	for ( a = meshAdj; a; a = next ) {
		next = a->next;
		ri.Free( a->edges );
		ri.Free( a->weld );
		ri.Free( a );
	}
	meshAdj = NULL;
	sBrushes = NULL; sFaces = NULL; sVerts = NULL; sModels = NULL; sLightVolumes = NULL; vbuf = NULL;
	numSBrushes = numSFaces = numSVerts = numSModels = numSLightVolumes = 0;
	if ( volumeProgramValid ) {
		GLSL_DeleteGPUShader( &volumeProgram );
		volumeProgramValid = qfalse;
	}
}

/*
=====================================================================

VOLUME BUILDING

=====================================================================
*/

static void EmitTri( const vec3_t a, const vec3_t b, const vec3_t c ) {
	if ( vbufCount + 3 > MAX_VOLUME_VERTS ) {
		return;
	}
	VectorCopy( a, vbuf[vbufCount] );
	VectorCopy( b, vbuf[vbufCount + 1] );
	VectorCopy( c, vbuf[vbufCount + 2] );
	vbufCount += 3;
}

// a triangle wound so that its right-hand normal points away from `inside`
static void EmitTriOut( const vec3_t a, const vec3_t b, const vec3_t c, const vec3_t inside ) {
	vec3_t e1, e2, n, d;

	VectorSubtract( b, a, e1 );
	VectorSubtract( c, a, e2 );
	CrossProduct( e1, e2, n );
	VectorSubtract( inside, a, d );
	if ( DotProduct( n, d ) > 0 ) {
		EmitTri( a, c, b );
	} else {
		EmitTri( a, b, c );
	}
}

// tr_stencilshadow.cpp PointsOrdered
static qboolean PointsOrdered( const vec3_t a, const vec3_t b ) {
	float i = a[0] + a[1] * 127 + a[2] * 1023;
	float j = b[0] + b[1] * 127 + b[2] * 1023;

	return i < j;
}

static void Extrude( const vec3_t p, const vec3_t light, float dist, vec3_t out ) {
	vec3_t d;

	VectorSubtract( p, light, d );
	VectorNormalize( d );
	VectorMA( p, dist, d, out );
}

// the side quad of a silhouette edge a-b, split the same way whichever
// order the edge comes in
static void EmitSilQuad( const vec3_t a, const vec3_t b, const vec3_t light, float dist, const vec3_t inside ) {
	vec3_t a2, b2;

	if ( !PointsOrdered( a, b ) ) {
		const float *t = a;
		a = b;
		b = t;
	}
	Extrude( a, light, dist, a2 );
	Extrude( b, light, dist, b2 );
	EmitTriOut( a, a2, b, inside );
	EmitTriOut( b, a2, b2, inside );
}

static qboolean SamePoint( const vec3_t a, const vec3_t b ) {
	return fabs( a[0] - b[0] ) < 0.1f && fabs( a[1] - b[1] ) < 0.1f && fabs( a[2] - b[2] ) < 0.1f;
}

/*
=================
BrushVolume

A convex brush's shadow volume: the faces toward the light (near cap),
the same faces pushed past the light's reach (far cap), and the edges
between a face toward the light and one away from it (sides). The light
inside the brush casts nothing.
=================
*/
static void BrushVolume( const sBrush_t *b, const vec3_t light, float dist, const vec3_t offset ) {
	qboolean facing[64];
	vec3_t center;
	int i, j, k, n = b->numFaces;

	if ( n > 64 ) {
		return;
	}
	for ( i = 0; i < n; i++ ) {
		const sFace_t *f = &sFaces[b->firstFace + i];
		vec3_t lp;

		VectorSubtract( light, offset, lp );
		facing[i] = DotProduct( f->normal, lp ) - f->dist > 0;
	}
	for ( i = 0; i < n && !facing[i]; i++ ) {
	}
	if ( i == n ) {
		return;     // the light is inside
	}
	VectorAdd( b->mins, b->maxs, center );
	VectorScale( center, 0.5f, center );
	VectorAdd( center, offset, center );

	for ( i = 0; i < n; i++ ) {
		const sFace_t *f = &sFaces[b->firstFace + i];
		vec3_t p0;

		if ( !facing[i] ) {
			continue;
		}
		VectorAdd( sVerts[f->firstVert], offset, p0 );
		// caps
		for ( j = 1; j + 1 < f->numVerts; j++ ) {
			vec3_t p1, p2, e0, e1, e2;

			VectorAdd( sVerts[f->firstVert + j], offset, p1 );
			VectorAdd( sVerts[f->firstVert + j + 1], offset, p2 );
			EmitTriOut( p0, p1, p2, center );   // near cap: out toward the light
			Extrude( p0, light, dist, e0 );
			Extrude( p1, light, dist, e1 );
			Extrude( p2, light, dist, e2 );
			{
				// far cap: out away from the light; the inside is between the caps
				vec3_t mid;

				VectorAdd( p0, e0, mid );
				VectorScale( mid, 0.5f, mid );
				EmitTriOut( e0, e1, e2, mid );
			}
		}
		// silhouette edges: shared with a face away from the light
		for ( j = 0; j < f->numVerts; j++ ) {
			const float *a = sVerts[f->firstVert + j], *c = sVerts[f->firstVert + ( j + 1 ) % f->numVerts];
			qboolean sil = qtrue;

			for ( k = 0; k < n; k++ ) {
				const sFace_t *g = &sFaces[b->firstFace + k];
				int m, hits = 0;

				if ( k == i ) {
					continue;
				}
				for ( m = 0; m < g->numVerts; m++ ) {
					if ( SamePoint( sVerts[g->firstVert + m], a ) || SamePoint( sVerts[g->firstVert + m], c ) ) {
						hits++;
					}
				}
				if ( hits >= 2 ) {
					sil = !facing[k];
					break;
				}
			}
			if ( sil ) {
				vec3_t pa, pc, mid, inside;

				VectorAdd( a, offset, pa );
				VectorAdd( c, offset, pc );
				// the volume's inside: just behind the face, toward the far cap
				VectorAdd( pa, pc, mid );
				VectorScale( mid, 0.5f, mid );
				VectorSubtract( center, mid, inside );
				VectorMA( mid, 0.01f, inside, inside );
				{
					vec3_t away;

					VectorSubtract( mid, light, away );
					VectorNormalize( away );
					VectorMA( inside, 4.0f, away, inside );
				}
				EmitSilQuad( pa, pc, light, dist, inside );
			}
		}
	}
}

static qboolean BrushTouchesLight( const sBrush_t *b, const uLight_t *l, const vec3_t offset ) {
	vec3_t mins, maxs;

	VectorAdd( b->mins, offset, mins );
	VectorAdd( b->maxs, offset, maxs );
	return !( mins[0] > l->bounds[1][0] || mins[1] > l->bounds[1][1] || mins[2] > l->bounds[1][2]
		|| maxs[0] < l->bounds[0][0] || maxs[1] < l->bounds[0][1] || maxs[2] < l->bounds[0][2] );
}

static float ExtrudeDistance( const uLight_t *l ) {
	float far = 1;
	int i;

	for ( i = 0; i < l->numFrustumVerts; i++ ) {
		vec3_t d;

		VectorSubtract( l->frustumVerts[i], l->globalLightOrigin, d );
		far = MAX( far, VectorLength( d ) );
	}
	return far * 2.0f;
}

static void WorldVolume( const uLight_t *l ) {
	float dist = ExtrudeDistance( l );
	int i;

	if ( !numSModels ) {
		return;
	}
	for ( i = sModels[0].firstBrush; i < sModels[0].firstBrush + sModels[0].numBrushes; i++ ) {
		if ( BrushTouchesLight( &sBrushes[i], l, vec3_origin ) ) {
			BrushVolume( &sBrushes[i], l->globalLightOrigin, dist, vec3_origin );
		}
	}
}

/*
=================
MeshAdjacency

Welded edge list of an MD3 surface (positions of frame 0), each edge with
its one or two triangles.
=================
*/
static sMeshAdj_t *MeshAdjacency( const mdvSurface_t *surf ) {
	sMeshAdj_t *a;
	int i, j, numTris = surf->numIndexes / 3;

	for ( a = meshAdj; a; a = a->next ) {
		if ( a->surf == surf ) {
			return a;
		}
	}
	a = ri.Malloc( sizeof( *a ) );
	a->surf = surf;
	a->weld = ri.Malloc( sizeof( int ) * surf->numVerts );
	a->edges = ri.Malloc( sizeof( int ) * 4 * surf->numIndexes );
	a->numEdges = 0;
	for ( i = 0; i < surf->numVerts; i++ ) {
		a->weld[i] = i;
		for ( j = 0; j < i; j++ ) {
			if ( a->weld[j] == j && VectorCompare( surf->verts[i].xyz, surf->verts[j].xyz ) ) {
				a->weld[i] = j;
				break;
			}
		}
	}
	for ( i = 0; i < numTris; i++ ) {
		for ( j = 0; j < 3; j++ ) {
			int v1 = a->weld[surf->indexes[i * 3 + j]], v2 = a->weld[surf->indexes[i * 3 + ( j + 1 ) % 3]];
			int e;

			for ( e = 0; e < a->numEdges; e++ ) {
				int *ed = &a->edges[e * 4];

				if ( ed[3] < 0 && ed[0] == v2 && ed[1] == v1 ) {
					ed[3] = i;
					break;
				}
			}
			if ( e == a->numEdges ) {
				int *ed = &a->edges[a->numEdges * 4];

				ed[0] = v1; ed[1] = v2; ed[2] = i; ed[3] = -1;
				a->numEdges++;
			}
		}
	}
	a->next = meshAdj;
	meshAdj = a;
	return a;
}

/*
=================
MeshVolume

R_AddSilEdges for an MD3 surface in model space: a triangle casts when it
faces the light; an edge is a silhouette when exactly one of its triangles
casts (an unmatched edge counts as one whose other side never casts).
=================
*/
static void MeshVolume( const mdvSurface_t *surf, int frame, int oldframe, float backlerp, const vec3_t light, float dist ) {
	static vec3_t pos[SHADER_MAX_VERTEXES * 4];
	static byte casts[SHADER_MAX_INDEXES];
	sMeshAdj_t *adj;
	int i, numTris = surf->numIndexes / 3;
	const mdvVertex_t *v0, *v1;

	if ( surf->numVerts > (int)ARRAY_LEN( pos ) || numTris > (int)sizeof( casts ) ) {
		return;
	}
	adj = MeshAdjacency( surf );
	v0 = surf->verts + surf->numVerts * frame;
	v1 = surf->verts + surf->numVerts * oldframe;
	for ( i = 0; i < surf->numVerts; i++ ) {
		pos[i][0] = v0[i].xyz[0] + backlerp * ( v1[i].xyz[0] - v0[i].xyz[0] );
		pos[i][1] = v0[i].xyz[1] + backlerp * ( v1[i].xyz[1] - v0[i].xyz[1] );
		pos[i][2] = v0[i].xyz[2] + backlerp * ( v1[i].xyz[2] - v0[i].xyz[2] );
	}
	for ( i = 0; i < numTris; i++ ) {
		const float *a = pos[surf->indexes[i * 3]], *b = pos[surf->indexes[i * 3 + 1]], *c = pos[surf->indexes[i * 3 + 2]];
		vec3_t e1, e2, n, d;

		// Q3 models wind their visible side clockwise: the outward normal
		// is (c - a) x (b - a)
		VectorSubtract( c, a, e1 );
		VectorSubtract( b, a, e2 );
		CrossProduct( e1, e2, n );
		VectorSubtract( light, a, d );
		casts[i] = DotProduct( n, d ) > 0;
		if ( casts[i] ) {
			vec3_t ea, eb, ec, center, mid;

			VectorAdd( a, b, center );
			VectorAdd( center, c, center );
			VectorScale( center, 1.0f / 3, center );
			Extrude( a, light, dist, ea );
			Extrude( b, light, dist, eb );
			Extrude( c, light, dist, ec );
			// near cap toward the light, far cap away from it
			VectorAdd( center, ea, mid );
			VectorScale( mid, 0.5f, mid );
			EmitTriOut( a, b, c, mid );
			EmitTriOut( ea, eb, ec, center );
		}
	}
	for ( i = 0; i < adj->numEdges; i++ ) {
		const int *ed = &adj->edges[i * 4];
		qboolean c1 = casts[ed[2]], c2 = ed[3] >= 0 ? casts[ed[3]] : qfalse;
		int t;
		vec3_t center, mid, inside, away;
		const float *a, *b;

		if ( !( c1 ^ c2 ) ) {
			continue;
		}
		t = c1 ? ed[2] : ed[3];
		a = pos[ed[0]];
		b = pos[ed[1]];
		VectorAdd( pos[surf->indexes[t * 3]], pos[surf->indexes[t * 3 + 1]], center );
		VectorAdd( center, pos[surf->indexes[t * 3 + 2]], center );
		VectorScale( center, 1.0f / 3, center );
		VectorAdd( a, b, mid );
		VectorScale( mid, 0.5f, mid );
		VectorSubtract( mid, light, away );
		VectorNormalize( away );
		VectorSubtract( center, mid, inside );
		VectorMA( mid, 0.25f, inside, inside );
		VectorMA( inside, 4.0f, away, inside );
		EmitSilQuad( a, b, light, dist, inside );
	}
}

/*
=====================================================================

DRAWING (draw_common.cpp RB_StencilShadowPass)

=====================================================================
*/

static qboolean VolumeProgram( void ) {
	if ( volumeProgramValid ) {
		return qtrue;
	}
	if ( !GLSL_InitOAXShader( &volumeProgram, "interaction", ATTR_POSITION | ATTR_NORMAL,
		"#define ULIGHT_DEPTH\n#define ULIGHT_VOLUME\n", fallbackShader_interaction_vp, fallbackShader_interaction_fp ) ) {
		return qfalse;
	}
	volumeProgramValid = qtrue;
	return qtrue;
}

// draws the volume in vbuf through the tess vertex buffer, in chunks
static void DrawVbuf( void ) {
	int first;

	for ( first = 0; first < vbufCount; ) {
		int n = MIN( vbufCount - first, ( SHADER_MAX_VERTEXES - 1 ) / 3 * 3 ), i;

		for ( i = 0; i < n; i++ ) {
			VectorCopy( vbuf[first + i], tess.xyz[i] );
			tess.xyz[i][3] = 1;
			tess.indexes[i] = i;
		}
		tess.numVertexes = n;
		tess.numIndexes = n;
		tess.firstIndex = 0;
		tess.useInternalVao = qtrue;
		tess.useCacheVao = qfalse;
		RB_UpdateTessVao( ATTR_POSITION );
		R_DrawElements( n, 0 );
		ulw.statStencilTris += n / 3;
		first += n;
	}
	tess.numVertexes = tess.numIndexes = 0;
	tess.useInternalVao = qfalse;
}

static vao_t *StaticVolume( uLight_t *l, int lightNum, int *numIndexes ) {
	sLightVolume_t *v;

	if ( lightNum < 0 || lightNum >= numSLightVolumes ) {
		return NULL;
	}
	v = &sLightVolumes[lightNum];
	if ( !v->vao || v->stamp != l->derivedCount ) {
		return NULL;
	}
	*numIndexes = v->numIndexes;
	return v->vao;
}

static void Pass( int cullFace, int zfailOp, uViewLight_t *vl, vao_t *worldVao, int worldIndexes, qboolean worldDynamic ) {
	static int entityDone[MAX_REFENTITIES], entityStamp;
	drawSurf_t *casters = R_ULightSurfList( vl->firstCaster );
	int i;

	entityStamp++;

	qglCullFace( cullFace );
	qglStencilOp( GL_KEEP, zfailOp, GL_KEEP );

	// the world: cached for lights that have not moved
	backEnd.currentEntity = &tr.worldEntity;
	backEnd.or = backEnd.viewParms.world;
	GL_SetModelviewMatrix( backEnd.viewParms.world.modelMatrix );
	GLSL_SetUniformMat4( &volumeProgram, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection );
	GLSL_SetUniformInt( &volumeProgram, UNIFORM_DEFORMGEN, DGEN_NONE );
	if ( worldVao ) {
		R_BindVao( worldVao );
		qglDrawElements( GL_TRIANGLES, worldIndexes, GL_INDEX_TYPE, BUFFER_OFFSET( 0 ) );
		ulw.statStencilTris += worldIndexes / 3;
	} else if ( worldDynamic ) {
		vbufCount = 0;
		WorldVolume( vl->light );
		DrawVbuf();
	}

	// entities in the light volume
	for ( i = vl->shadowSize; i < vl->numCaster; i++ ) {
		int entityNum, fogNum, dlighted, pshadowed;
		shader_t *sh;
		trRefEntity_t *ent;
		vec3_t light, d;
		float dist = ExtrudeDistance( vl->light );
		int k;

		R_DecomposeSort( casters[i].sort, &entityNum, &sh, &fogNum, &dlighted, &pshadowed );
		if ( entityNum == REFENTITYNUM_WORLD || entityNum >= backEnd.refdef.num_entities ) {
			continue;
		}
		ent = &backEnd.refdef.entities[entityNum];
		R_RotateForEntity( ent, &backEnd.viewParms, &backEnd.or );
		GL_SetModelviewMatrix( backEnd.or.modelMatrix );
		GLSL_SetUniformMat4( &volumeProgram, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection );
		// the light in model space
		VectorSubtract( vl->light->globalLightOrigin, backEnd.or.origin, d );
		for ( k = 0; k < 3; k++ ) {
			light[k] = DotProduct( d, backEnd.or.axis[k] );
		}
		vbufCount = 0;
		if ( *casters[i].surface == SF_VAO_MDVMESH ) {
			srfVaoMdvMesh_t *m = (srfVaoMdvMesh_t *)casters[i].surface;

			if ( m->mdvModel && m->mdvSurface ) {
				int frame = ent->e.frame % m->mdvModel->numFrames;
				int old = ent->e.oldframe % m->mdvModel->numFrames;

				MeshVolume( m->mdvSurface, frame < 0 ? 0 : frame, old < 0 ? 0 : old, ent->e.backlerp, light, dist );
			}
		} else if ( ent->e.reType == RT_MODEL && entityDone[entityNum] != entityStamp ) {
			model_t *model = R_GetModelByHandle( ent->e.hModel );

			// brush models (doors, movers): their own brushes, once per entity
			entityDone[entityNum] = entityStamp;
			if ( model && model->type == MOD_BRUSH && model->bmodel ) {
				int sub = (int)( model->bmodel - tr.world->bmodels ), x;

				if ( sub > 0 && sub < numSModels ) {
					for ( x = sModels[sub].firstBrush; x < sModels[sub].firstBrush + sModels[sub].numBrushes; x++ ) {
						BrushVolume( &sBrushes[x], light, dist, vec3_origin );
					}
				}
			}
		}
		DrawVbuf();
	}
}

/*
=================
RB_ULightStencilShadows

Leaves the stencil test on, passing where the light is not shadowed; the
caller draws the light's interactions and turns the test off.
=================
*/
void RB_ULightStencilShadows( uViewLight_t *vl ) {
	uLight_t *l = vl->light;
	int lightNum = (int)( l - ulw.lights );
	int worldIndexes = 0;
	vao_t *worldVao;
	qboolean dynamicWorld;

	if ( !R_ULightHasStencil() || !VolumeProgram() ) {
		return;
	}
	if ( !vbuf ) {
		vbuf = ri.Malloc( sizeof( vec3_t ) * MAX_VOLUME_VERTS );
	}
	worldVao = StaticVolume( l, lightNum, &worldIndexes );
	dynamicWorld = worldVao == NULL;

	qglClearStencil( 128 );
	qglClear( GL_STENCIL_BUFFER_BIT );

	// depth-fail counting: no color or depth writes, depth test LESS, volumes
	// pushed back so a volume's cap never passes on the surface it lies on
	GL_State( 0 );
	qglDepthFunc( GL_LESS );
	qglColorMask( GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE );
	qglEnable( GL_STENCIL_TEST );
	qglStencilFunc( GL_ALWAYS, 128, 255 );
	qglStencilMask( 255 );
	qglEnable( GL_POLYGON_OFFSET_FILL );
	qglPolygonOffset( 1.0f, 1.0f );
	qglEnable( GL_CULL_FACE );
	GLSL_BindProgram( &volumeProgram );

	// back faces increment, front faces decrement (GL front = counter-clockwise)
	Pass( GL_FRONT, GL_INCR_WRAP, vl, worldVao, worldIndexes, dynamicWorld );
	Pass( GL_BACK, GL_DECR_WRAP, vl, worldVao, worldIndexes, dynamicWorld );

	// restore what GL_State / GL_Cull believe
	qglDisable( GL_POLYGON_OFFSET_FILL );
	qglPolygonOffset( r_offsetFactor->value, r_offsetUnits->value );
	qglDepthFunc( GL_LEQUAL );
	qglColorMask( !backEnd.colorMask[0], !backEnd.colorMask[1], !backEnd.colorMask[2], !backEnd.colorMask[3] );
	qglCullFace( glState.faceCullFront ? GL_FRONT : GL_BACK );
	if ( glState.faceCulling == CT_TWO_SIDED ) {
		qglDisable( GL_CULL_FACE );
	}
	backEnd.currentEntity = &tr.worldEntity;
	backEnd.or = backEnd.viewParms.world;
	GL_SetModelviewMatrix( backEnd.viewParms.world.modelMatrix );

	// the interaction pass draws where the count is back to 128
	qglStencilFunc( GL_EQUAL, 128, 255 );
	qglStencilOp( GL_KEEP, GL_KEEP, GL_KEEP );
}

/*
=================
R_ULightStencilBuildStatic

At load: the world volume of every static map light, in a vertex buffer.
=================
*/
void R_ULightStencilBuildStatic( void ) {
	int i, total = 0;

	if ( !R_ULightHasStencil() || !numSBrushes ) {
		return;
	}
	if ( !vbuf ) {
		vbuf = ri.Malloc( sizeof( vec3_t ) * MAX_VOLUME_VERTS );
	}
	numSLightVolumes = ulw.numMapLights;
	sLightVolumes = ri.Malloc( sizeof( sLightVolume_t ) * ( numSLightVolumes + 1 ) );
	Com_Memset( sLightVolumes, 0, sizeof( sLightVolume_t ) * ( numSLightVolumes + 1 ) );
	for ( i = 0; i < ulw.numMapLights; i++ ) {
		uLight_t *l = &ulw.lights[i];
		vao_t *vao;
		glIndex_t *idx;
		int k;

		if ( l->parms.noShadows ) {
			continue;
		}
		vbufCount = 0;
		WorldVolume( l );
		if ( !vbufCount ) {
			continue;
		}
		idx = ri.Hunk_AllocateTempMemory( sizeof( glIndex_t ) * vbufCount );
		for ( k = 0; k < vbufCount; k++ ) {
			idx[k] = k;
		}
		vao = R_CreateVao( va( "ulightVolume%d", i ), (byte *)vbuf, sizeof( vec3_t ) * vbufCount, (byte *)idx, sizeof( glIndex_t ) * vbufCount, VAO_USAGE_STATIC );
		ri.Hunk_FreeTempMemory( idx );
		vao->attribs[ATTR_INDEX_POSITION].enabled = 1;
		vao->attribs[ATTR_INDEX_POSITION].count = 3;
		vao->attribs[ATTR_INDEX_POSITION].type = GL_FLOAT;
		vao->attribs[ATTR_INDEX_POSITION].normalized = GL_FALSE;
		vao->attribs[ATTR_INDEX_POSITION].stride = sizeof( vec3_t );
		vao->attribs[ATTR_INDEX_POSITION].offset = 0;
		Vao_SetVertexPointers( vao );
		R_BindNullVao();
		sLightVolumes[i].vao = vao;
		sLightVolumes[i].numIndexes = vbufCount;
		sLightVolumes[i].stamp = l->derivedCount;
		total += vbufCount / 3;
	}
	ri.Printf( PRINT_DEVELOPER, "unified lighting: %d static shadow volume triangles\n", total );
}
