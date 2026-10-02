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
cm_guisurf.c: the GUI surfaces of the map, for the server's and the
client's GUI traces (oax in-world GUIs, qcommon/oax.h).

Adapted from DOOM-3 neo/renderer/tr_guisurf.cpp (R_SurfaceToTextureAxis)
and neo/renderer/RenderWorld.cpp (idRenderWorldLocal::GuiTrace).
Changes: C over the Q3 BSP. At map load only the draw surfaces whose
shader carries SURF_OAX_GUI (surfaceparm gui, from custinfoparms.txt) are
kept, with each surface's texture axes computed once; GuiTrace intersects
a ray with them in the entity's frame (origin and angles) instead of a
render model's local trace. The same code answers the server (focus,
clicks) and the client (the cosmetic cursor).
===========================================================================
*/

#include "cm_local.h"
#include "oax.h"

typedef struct {
	int			model;			// inline model: 0 world, 1+ bmodels
	int			numVerts;
	int			numIndexes;
	vec3_t		*xyz;
	int			*indexes;
	vec3_t		origin;			// texture space: a point dotted with the
	vec3_t		axis[3];		// axes gives 0..1 in S and T on the panel
	vec3_t		mins, maxs;
} cmGuiSurf_t;

#define MAX_GUI_SURFS	256

static cmGuiSurf_t	guiSurfs[MAX_GUI_SURFS];
static int			numGuiSurfs;

void RotatePoint( vec3_t point, vec3_t matrix[3] );
void CreateRotationMatrix( const vec3_t angles, vec3_t matrix[3] );

/*
================
CM_SurfaceToTextureAxis

DOOM-3's R_SurfaceToTextureAxis: two axes for the surface such that a point
dotted against them gives a 0.0 to 1.0 range in S and T inside the panel.
================
*/
static void CM_SurfaceToTextureAxis( const drawVert_t *verts, int numVerts, const int *indexes, vec3_t origin, vec3_t axis[3] ) {
	float		area, inva;
	float		d0[5], d1[5];
	const drawVert_t *a, *b, *c;
	float		bounds[2][2];
	float		boundsOrg[2];
	int			i, j;
	float		v;
	vec3_t		e0, e1, n;

	// find the bounds of the texture
	bounds[0][0] = bounds[0][1] = 999999;
	bounds[1][0] = bounds[1][1] = -999999;
	for ( i = 0 ; i < numVerts ; i++ ) {
		for ( j = 0 ; j < 2 ; j++ ) {
			v = verts[i].st[j];
			if ( v < bounds[0][j] ) {
				bounds[0][j] = v;
			}
			if ( v > bounds[1][j] ) {
				bounds[1][j] = v;
			}
		}
	}

	// use the floor of the midpoint as the origin of the
	// surface, which will prevent a slight misalignment
	// from throwing it an entire cycle off
	boundsOrg[0] = floor( ( bounds[0][0] + bounds[1][0] ) * 0.5 );
	boundsOrg[1] = floor( ( bounds[0][1] + bounds[1][1] ) * 0.5 );

	// determine the world S and T vectors from the first drawSurf triangle
	a = verts + indexes[0];
	b = verts + indexes[1];
	c = verts + indexes[2];

	VectorSubtract( b->xyz, a->xyz, d0 );
	d0[3] = b->st[0] - a->st[0];
	d0[4] = b->st[1] - a->st[1];
	VectorSubtract( c->xyz, a->xyz, d1 );
	d1[3] = c->st[0] - a->st[0];
	d1[4] = c->st[1] - a->st[1];

	area = d0[3] * d1[4] - d0[4] * d1[3];
	if ( area == 0.0 ) {
		VectorClear( axis[0] );
		VectorClear( axis[1] );
		VectorClear( axis[2] );
		VectorClear( origin );
		return;	// degenerate
	}
	inva = 1.0 / area;

	axis[0][0] = (d0[0] * d1[4] - d0[4] * d1[0]) * inva;
	axis[0][1] = (d0[1] * d1[4] - d0[4] * d1[1]) * inva;
	axis[0][2] = (d0[2] * d1[4] - d0[4] * d1[2]) * inva;

	axis[1][0] = (d0[3] * d1[0] - d0[0] * d1[3]) * inva;
	axis[1][1] = (d0[3] * d1[1] - d0[1] * d1[3]) * inva;
	axis[1][2] = (d0[3] * d1[2] - d0[2] * d1[3]) * inva;

	// the plane normal (idPlane::FromPoints)
	VectorSubtract( a->xyz, b->xyz, e0 );
	VectorSubtract( c->xyz, b->xyz, e1 );
	CrossProduct( e0, e1, n );
	VectorNormalize( n );
	VectorCopy( n, axis[2] );

	// take point 0 and project the vectors to the texture origin
	VectorMA( a->xyz, boundsOrg[0] - a->st[0], axis[0], origin );
	VectorMA( origin, boundsOrg[1] - a->st[1], axis[1], origin );
}

/*
================
CM_LoadGuiSurfaces

Called by CM_LoadMap with the BSP still in memory.
================
*/
void CM_LoadGuiSurfaces( const byte *base, const dheader_t *header ) {
	const dshader_t		*shaders;
	const dsurface_t	*surfs;
	const drawVert_t	*verts;
	const int			*indexes;
	const dmodel_t		*models;
	int					numShaders, numSurfs, numVerts, numIndexes, numModels;
	int					i, j, m;

	numGuiSurfs = 0;

	shaders = (const dshader_t *)( base + header->lumps[LUMP_SHADERS].fileofs );
	numShaders = header->lumps[LUMP_SHADERS].filelen / sizeof( *shaders );
	surfs = (const dsurface_t *)( base + header->lumps[LUMP_SURFACES].fileofs );
	numSurfs = header->lumps[LUMP_SURFACES].filelen / sizeof( *surfs );
	verts = (const drawVert_t *)( base + header->lumps[LUMP_DRAWVERTS].fileofs );
	numVerts = header->lumps[LUMP_DRAWVERTS].filelen / sizeof( *verts );
	indexes = (const int *)( base + header->lumps[LUMP_DRAWINDEXES].fileofs );
	numIndexes = header->lumps[LUMP_DRAWINDEXES].filelen / sizeof( *indexes );
	models = (const dmodel_t *)( base + header->lumps[LUMP_MODELS].fileofs );
	numModels = header->lumps[LUMP_MODELS].filelen / sizeof( *models );

	for ( m = 0; m < numModels; m++ ) {
		int first = LittleLong( models[m].firstSurface );
		int count = LittleLong( models[m].numSurfaces );

		for ( i = first; i < first + count && i < numSurfs; i++ ) {
			const dsurface_t *in = &surfs[i];
			int shaderNum = LittleLong( in->shaderNum );
			int type = LittleLong( in->surfaceType );
			int fv = LittleLong( in->firstVert ), nv = LittleLong( in->numVerts );
			int fi = LittleLong( in->firstIndex ), ni = LittleLong( in->numIndexes );
			cmGuiSurf_t *out;
			drawVert_t *local;
			int *localIndexes;

			if ( shaderNum < 0 || shaderNum >= numShaders ) {
				continue;
			}
			if ( !( LittleLong( shaders[shaderNum].surfaceFlags ) & SURF_OAX_GUI ) ) {
				continue;
			}
			if ( type != MST_PLANAR && type != MST_TRIANGLE_SOUP ) {
				continue;
			}
			if ( nv < 3 || ni < 3 || fv < 0 || fv + nv > numVerts || fi < 0 || fi + ni > numIndexes ) {
				continue;
			}
			if ( numGuiSurfs == MAX_GUI_SURFS ) {
				Com_Printf( S_COLOR_YELLOW "WARNING: more than %d gui surfaces\n", MAX_GUI_SURFS );
				return;
			}

			out = &guiSurfs[numGuiSurfs];
			Com_Memset( out, 0, sizeof( *out ) );
			out->model = m;
			out->numVerts = nv;
			out->numIndexes = ni;
			out->xyz = Hunk_Alloc( nv * sizeof( vec3_t ), h_high );
			out->indexes = Hunk_Alloc( ni * sizeof( int ), h_high );

			// byte-swapped copies to work from
			local = Hunk_AllocateTempMemory( nv * sizeof( drawVert_t ) );
			localIndexes = out->indexes;
			ClearBounds( out->mins, out->maxs );
			for ( j = 0; j < nv; j++ ) {
				local[j].xyz[0] = LittleFloat( verts[fv + j].xyz[0] );
				local[j].xyz[1] = LittleFloat( verts[fv + j].xyz[1] );
				local[j].xyz[2] = LittleFloat( verts[fv + j].xyz[2] );
				local[j].st[0] = LittleFloat( verts[fv + j].st[0] );
				local[j].st[1] = LittleFloat( verts[fv + j].st[1] );
				VectorCopy( local[j].xyz, out->xyz[j] );
				AddPointToBounds( local[j].xyz, out->mins, out->maxs );
			}
			for ( j = 0; j < ni; j++ ) {
				localIndexes[j] = LittleLong( indexes[fi + j] );
				if ( localIndexes[j] < 0 || localIndexes[j] >= nv ) {
					localIndexes[j] = 0;
				}
			}
			CM_SurfaceToTextureAxis( local, nv, localIndexes, out->origin, out->axis );
			Hunk_FreeTempMemory( local );

			if ( VectorLength( out->axis[0] ) == 0.0f || VectorLength( out->axis[1] ) == 0.0f ) {
				continue;	// degenerate texture mapping
			}
			numGuiSurfs++;
		}
	}
	if ( numGuiSurfs ) {
		Com_DPrintf( "%d gui surfaces\n", numGuiSurfs );
	}
}

void CM_ClearGuiSurfaces( void ) {
	numGuiSurfs = 0;
}

int CM_NumGuiSurfaces( int model ) {
	int i, n = 0;

	for ( i = 0; i < numGuiSurfs; i++ ) {
		if ( model < 0 || guiSurfs[i].model == model ) {
			n++;
		}
	}
	return n;
}

/*
================
CM_RayTriangle

Ray start + f * dir against one triangle (both sides); qtrue with f when
it crosses within [0, *best).
================
*/
static qboolean CM_RayTriangle( const vec3_t start, const vec3_t dir, const vec3_t a, const vec3_t b, const vec3_t c, float *best ) {
	vec3_t	e1, e2, p, t, q;
	float	det, inv, u, v, f;

	VectorSubtract( b, a, e1 );
	VectorSubtract( c, a, e2 );
	CrossProduct( dir, e2, p );
	det = DotProduct( e1, p );
	if ( det > -1e-8f && det < 1e-8f ) {
		return qfalse;
	}
	inv = 1.0f / det;
	VectorSubtract( start, a, t );
	u = DotProduct( t, p ) * inv;
	if ( u < 0.0f || u > 1.0f ) {
		return qfalse;
	}
	CrossProduct( t, e1, q );
	v = DotProduct( dir, q ) * inv;
	if ( v < 0.0f || u + v > 1.0f ) {
		return qfalse;
	}
	f = DotProduct( e2, q ) * inv;
	if ( f < 0.0f || f >= *best ) {
		return qfalse;
	}
	*best = f;
	return qtrue;
}

/*
================
CM_GuiTrace

DOOM-3's GuiTrace: where the segment start..end first crosses a GUI
surface of the inline model placed at origin / angles. Returns qtrue with
the panel position (x right, y down, 0..1 inside), the fraction along the
segment and the surface's index. No occlusion test: callers trace the world
first, as DOOM-3 did.
================
*/
qboolean CM_GuiTrace( int model, const vec3_t origin, const vec3_t angles, const vec3_t start, const vec3_t end,
		float *x, float *y, float *fraction ) {
	vec3_t		localStart, localEnd, dir, hit, cursor;
	vec3_t		matrix[3];
	qboolean	rotated;
	float		best = 1.0f;
	int			i, j, bestSurf = -1;

	rotated = angles && ( angles[0] || angles[1] || angles[2] );

	VectorSubtract( start, origin, localStart );
	VectorSubtract( end, origin, localEnd );
	if ( rotated ) {
		CreateRotationMatrix( angles, matrix );
		RotatePoint( localStart, matrix );
		RotatePoint( localEnd, matrix );
	}
	VectorSubtract( localEnd, localStart, dir );

	for ( i = 0; i < numGuiSurfs; i++ ) {
		const cmGuiSurf_t *s = &guiSurfs[i];
		if ( s->model != model ) {
			continue;
		}
		for ( j = 0; j + 2 < s->numIndexes; j += 3 ) {
			if ( CM_RayTriangle( localStart, dir, s->xyz[s->indexes[j]], s->xyz[s->indexes[j + 1]], s->xyz[s->indexes[j + 2]], &best ) ) {
				bestSurf = i;
			}
		}
	}
	if ( bestSurf < 0 ) {
		return qfalse;
	}

	{
		const cmGuiSurf_t *s = &guiSurfs[bestSurf];
		float len0 = DotProduct( s->axis[0], s->axis[0] );
		float len1 = DotProduct( s->axis[1], s->axis[1] );

		VectorMA( localStart, best, dir, hit );
		VectorSubtract( hit, s->origin, cursor );
		if ( x ) {
			*x = DotProduct( cursor, s->axis[0] ) / len0;
		}
		if ( y ) {
			*y = DotProduct( cursor, s->axis[1] ) / len1;
		}
	}
	if ( fraction ) {
		*fraction = best;
	}
	return qtrue;
}
