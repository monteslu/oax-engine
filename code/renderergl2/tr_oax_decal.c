/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
/*
tr_oax_decal.c: projected decals (phase 6).

Q3 marks (tr_marks.c R_MarkFragments) clip a quad's prism against world
surfaces and leave the rest to the cgame: world only, the cgame keeps the
polygons and the fade. A projected decal is a box (origin, axis, half
sizes) owned by the renderer:

- every world surface and every brush-model surface inside the box is
  clipped against the box's six planes (R_ChopPolyBehindPlane, as in
  tr_marks.c), and texture coordinates come from the box projection;
- brush-model decals are kept in the model's own space and drawn with
  the entity, so they move with doors and movers (the model's transform
  is the last one the renderer drew it with);
- vertices fade with the angle between the surface and the projection
  and toward the box's near and far faces, and the decal fades out over
  the end of its life by the refdef clock;
- at most OAX_MAX_DECALS live at once: a new decal replaces the oldest.

Everything depends only on the requests and the map, in order, so a
replay makes the same decals on every build.
===========================================================================
*/

#include "tr_local.h"

#define MAX_CLIP_VERTS	64

typedef struct {
	qboolean		inUse;
	int				owner;			// 0 world, else the inline model index
	oaxDecal_t		d;				// as requested (world space)
	int				numVerts, numPolys;
	vec3_t			xyz[OAX_DECAL_MAX_VERTS];	// owner space
	vec2_t			st[OAX_DECAL_MAX_VERTS];
	float			strength[OAX_DECAL_MAX_VERTS];
	unsigned char	polyFirst[OAX_DECAL_MAX_POLYS], polyNum[OAX_DECAL_MAX_POLYS];
	vec3_t			bounds[2];		// owner space
	srfOaxDecal_t	surf;
} oaxDecalSlot_t;

typedef struct {
	qboolean	seen;
	vec3_t		origin;
	vec3_t		axis[3];
} bmodelXform_t;

#define MAX_DECAL_BMODELS 256

static oaxDecalSlot_t	decals[OAX_MAX_DECALS];
static int				nextDecal;
static int				decalClock;
static bmodelXform_t	bmodelXf[MAX_DECAL_BMODELS];

static oaxDecalSlot_t	scratch;	// a decal is built here, then kept only if it has polygons

void R_OAXDecalsWorldLoaded( void ) {
	memset( decals, 0, sizeof( decals ) );
	memset( bmodelXf, 0, sizeof( bmodelXf ) );
	nextDecal = 0;
	decalClock = 0;
}

void RE_OAXClearDecals( void ) {
	int i;

	for ( i = 0; i < OAX_MAX_DECALS; i++ ) {
		decals[i].inUse = qfalse;
	}
	nextDecal = 0;
}

/*
=============
ChopPolyBehindPlane

tr_marks.c's R_ChopPolyBehindPlane: keeps the part in front of the plane.
=============
*/
#define	SIDE_FRONT	0
#define	SIDE_BACK	1
#define	SIDE_ON		2
static void ChopPolyBehindPlane( int numInPoints, vec3_t inPoints[MAX_CLIP_VERTS],
		int *numOutPoints, vec3_t outPoints[MAX_CLIP_VERTS], const vec3_t normal, vec_t dist, vec_t epsilon ) {
	float	dists[MAX_CLIP_VERTS + 4];
	int		sides[MAX_CLIP_VERTS + 4];
	int		counts[3];
	float	dot;
	int		i, j;
	float	*p1, *p2, *clip;
	float	d;

	if ( numInPoints >= MAX_CLIP_VERTS - 2 ) {
		*numOutPoints = 0;
		return;
	}
	counts[0] = counts[1] = counts[2] = 0;
	for ( i = 0; i < numInPoints; i++ ) {
		dot = DotProduct( inPoints[i], normal ) - dist;
		dists[i] = dot;
		if ( dot > epsilon ) {
			sides[i] = SIDE_FRONT;
		} else if ( dot < -epsilon ) {
			sides[i] = SIDE_BACK;
		} else {
			sides[i] = SIDE_ON;
		}
		counts[sides[i]]++;
	}
	sides[i] = sides[0];
	dists[i] = dists[0];

	*numOutPoints = 0;
	if ( !counts[0] ) {
		return;
	}
	if ( !counts[1] ) {
		*numOutPoints = numInPoints;
		Com_Memcpy( outPoints, inPoints, numInPoints * sizeof( vec3_t ) );
		return;
	}
	for ( i = 0; i < numInPoints; i++ ) {
		p1 = inPoints[i];
		clip = outPoints[*numOutPoints];
		if ( sides[i] == SIDE_ON ) {
			VectorCopy( p1, clip );
			( *numOutPoints )++;
			continue;
		}
		if ( sides[i] == SIDE_FRONT ) {
			VectorCopy( p1, clip );
			( *numOutPoints )++;
			clip = outPoints[*numOutPoints];
		}
		if ( sides[i + 1] == SIDE_ON || sides[i + 1] == sides[i] ) {
			continue;
		}
		p2 = inPoints[( i + 1 ) % numInPoints];
		d = dists[i] - dists[i + 1];
		dot = d == 0 ? 0 : dists[i] / d;
		for ( j = 0; j < 3; j++ ) {
			clip[j] = p1[j] + dot * ( p2[j] - p1[j] );
		}
		( *numOutPoints )++;
	}
}

// the box, in the space being clipped (world or a model's)
typedef struct {
	vec3_t	origin;
	vec3_t	axis[3];
	vec3_t	halfSize;
	vec3_t	normals[6];
	float	dists[6];
	vec3_t	mins, maxs;		// axis-aligned bounds of the box
} decalBox_t;

static void BoxPlanes( decalBox_t *b ) {
	int i, j;

	ClearBounds( b->mins, b->maxs );
	for ( i = 0; i < 8; i++ ) {
		vec3_t corner;

		VectorCopy( b->origin, corner );
		for ( j = 0; j < 3; j++ ) {
			VectorMA( corner, ( ( i >> j ) & 1 ) ? b->halfSize[j] : -b->halfSize[j], b->axis[j], corner );
		}
		AddPointToBounds( corner, b->mins, b->maxs );
	}

	for ( i = 0; i < 3; i++ ) {
		// keep dot(p, n) >= dist: the inside of each face
		VectorScale( b->axis[i], -1.0f, b->normals[i * 2] );
		b->dists[i * 2] = DotProduct( b->normals[i * 2], b->origin ) - b->halfSize[i];
		VectorCopy( b->axis[i], b->normals[i * 2 + 1] );
		b->dists[i * 2 + 1] = DotProduct( b->normals[i * 2 + 1], b->origin ) - b->halfSize[i];
	}
}

static float Smooth( float e0, float e1, float x ) {
	float t = ( x - e0 ) / ( e1 - e0 );

	if ( t <= 0 ) {
		return 0;
	}
	if ( t >= 1 ) {
		return 1;
	}
	return t * t * ( 3 - 2 * t );
}

/*
=============
AddTriangle

Clips one triangle by the box and appends the polygon to the decal.
=============
*/
static void AddTriangle( oaxDecalSlot_t *slot, const decalBox_t *b, const vec3_t v0, const vec3_t v1, const vec3_t v2, const vec3_t front ) {
	vec3_t	clip[2][MAX_CLIP_VERTS];
	vec3_t	e1, e2, normal;
	float	facing;
	int		n = 3, pingPong = 0, i;

	// outside the box's bounds: nothing to clip
	for ( i = 0; i < 3; i++ ) {
		if ( ( v0[i] < b->mins[i] && v1[i] < b->mins[i] && v2[i] < b->mins[i] )
			|| ( v0[i] > b->maxs[i] && v1[i] > b->maxs[i] && v2[i] > b->maxs[i] ) ) {
			return;
		}
	}

	VectorSubtract( v1, v0, e1 );
	VectorSubtract( v2, v0, e2 );
	CrossProduct( e1, e2, normal );
	if ( VectorNormalize( normal ) == 0 ) {
		return;
	}
	// the side the surface faces: the face plane or the vertex normal
	if ( DotProduct( normal, front ) < 0 ) {
		VectorScale( normal, -1.0f, normal );
	}
	facing = DotProduct( normal, b->axis[0] );
	if ( facing < 0.05f ) {
		return;
	}

	VectorCopy( v0, clip[0][0] );
	VectorCopy( v1, clip[0][1] );
	VectorCopy( v2, clip[0][2] );
	for ( i = 0; i < 6; i++ ) {
		ChopPolyBehindPlane( n, clip[pingPong], &n, clip[!pingPong], b->normals[i], b->dists[i], 0.01f );
		pingPong ^= 1;
		if ( !n ) {
			return;
		}
	}
	if ( n < 3 || slot->numPolys >= OAX_DECAL_MAX_POLYS || slot->numVerts + n > OAX_DECAL_MAX_VERTS || n > 255 ) {
		return;
	}

	slot->polyFirst[slot->numPolys] = slot->numVerts;
	slot->polyNum[slot->numPolys] = n;
	slot->numPolys++;
	for ( i = 0; i < n; i++ ) {
		float *p = clip[pingPong][i];
		vec3_t delta;
		float depth;
		int v = slot->numVerts++;

		VectorCopy( p, slot->xyz[v] );
		VectorSubtract( p, b->origin, delta );
		slot->st[v][0] = 0.5f + DotProduct( delta, b->axis[1] ) * 0.5f / b->halfSize[1];
		slot->st[v][1] = 0.5f + DotProduct( delta, b->axis[2] ) * 0.5f / b->halfSize[2];
		depth = fabs( DotProduct( delta, b->axis[0] ) ) / b->halfSize[0];
		slot->strength[v] = Smooth( 0.05f, 0.35f, facing ) * ( 1.0f - Smooth( 0.6f, 1.0f, depth ) );
		AddPointToBounds( p, slot->bounds[0], slot->bounds[1] );
	}
}

/*
=============
AddSurface

The triangles of one BSP surface (world or brush model), as tr_marks.c
walks them.
=============
*/
static void AddSurface( oaxDecalSlot_t *slot, const decalBox_t *b, msurface_t *surf ) {
	srfBspSurface_t *cv;
	vec3_t front;
	int i, m, n;

	if ( ( surf->shader->surfaceFlags & ( SURF_NOIMPACT | SURF_NOMARKS ) ) || ( surf->shader->contentFlags & CONTENTS_FOG ) ) {
		return;
	}
	switch ( *surf->data ) {
	case SF_FACE:
	case SF_TRIANGLES:
		// triangle soups too (q3map2 -meta writes most map faces as
		// soups); r_marksOnTriangleMeshes is for Q3 marks only
		cv = (srfBspSurface_t *)surf->data;
		if ( *surf->data == SF_FACE && DotProduct( cv->cullPlane.normal, b->axis[0] ) < 0.05f ) {
			return;
		}
		for ( i = 0; i + 2 < cv->numIndexes; i += 3 ) {
			if ( *surf->data == SF_FACE ) {
				VectorCopy( cv->cullPlane.normal, front );
			} else {
				R_VaoUnpackNormal( front, cv->verts[cv->indexes[i]].normal );
			}
			AddTriangle( slot, b, cv->verts[cv->indexes[i]].xyz, cv->verts[cv->indexes[i + 1]].xyz, cv->verts[cv->indexes[i + 2]].xyz, front );
		}
		break;
	case SF_GRID:
		cv = (srfBspSurface_t *)surf->data;
		for ( m = 0; m < cv->height - 1; m++ ) {
			for ( n = 0; n < cv->width - 1; n++ ) {
				srfVert_t *dv = cv->verts + m * cv->width + n;

				R_VaoUnpackNormal( front, dv[0].normal );
				AddTriangle( slot, b, dv[0].xyz, dv[cv->width].xyz, dv[1].xyz, front );
				R_VaoUnpackNormal( front, dv[cv->width + 1].normal );
				AddTriangle( slot, b, dv[1].xyz, dv[cv->width].xyz, dv[cv->width + 1].xyz, front );
			}
		}
		break;
	default:
		break;
	}
}

static oaxDecalSlot_t *BeginSlot( const oaxDecal_t *d, int owner ) {
	oaxDecalSlot_t *slot = &scratch;

	memset( slot, 0, sizeof( *slot ) );
	slot->d = *d;
	slot->owner = owner;
	slot->surf.surfaceType = SF_OAX_DECAL;
	ClearBounds( slot->bounds[0], slot->bounds[1] );
	return slot;
}

/*
=============
KeepSlot

A decal with polygons takes the next ring slot (replacing the oldest).
=============
*/
static int KeepSlot( void ) {
	oaxDecalSlot_t *slot;

	if ( !scratch.numPolys ) {
		return 0;
	}
	slot = &decals[nextDecal];
	*slot = scratch;
	slot->inUse = qtrue;
	slot->surf.index = nextDecal;
	nextDecal = ( nextDecal + 1 ) % OAX_MAX_DECALS;
	return slot->numPolys;
}

/*
=============
BoxSurfaces

The world surfaces in the leaves the box touches (R_BoxSurfaces_r's walk,
without its per-face filters: AddSurface does those).
=============
*/
static void BoxSurfaces( mnode_t *node, const vec3_t mins, const vec3_t maxs, msurface_t **list, int listSize, int *listLen ) {
	int *mark, c, s;

	while ( node->contents == -1 ) {
		s = BoxOnPlaneSide( (vec_t *)mins, (vec_t *)maxs, node->plane );
		if ( s == 1 ) {
			node = node->children[0];
		} else if ( s == 2 ) {
			node = node->children[1];
		} else {
			BoxSurfaces( node->children[0], mins, maxs, list, listSize, listLen );
			node = node->children[1];
		}
	}
	mark = tr.world->marksurfaces + node->firstmarksurface;
	for ( c = node->nummarksurfaces; c > 0 && *listLen < listSize; c--, mark++ ) {
		if ( tr.world->surfacesViewCount[*mark] == tr.viewCount ) {
			continue;
		}
		tr.world->surfacesViewCount[*mark] = tr.viewCount;
		list[( *listLen )++] = tr.world->surfaces + *mark;
	}
}

static qboolean BoxBoundsOverlap( const vec3_t amins, const vec3_t amaxs, const vec3_t bmins, const vec3_t bmaxs ) {
	int i;

	for ( i = 0; i < 3; i++ ) {
		if ( amaxs[i] < bmins[i] || amins[i] > bmaxs[i] ) {
			return qfalse;
		}
	}
	return qtrue;
}

/*
=============
RE_OAXAddDecal
=============
*/
int RE_OAXAddDecal( const oaxDecal_t *d ) {
	decalBox_t		box;
	msurface_t		*list[256];
	vec3_t			mins, maxs;
	oaxDecalSlot_t	*slot;
	int				listLen = 0, i, j, k, total = 0;

	if ( !tr.registered || !tr.world || !d || !r_oaxDecals->integer ) {
		return 0;
	}
	if ( d->halfSize[0] <= 0 || d->halfSize[1] <= 0 || d->halfSize[2] <= 0 ) {
		return 0;
	}

	VectorCopy( d->origin, box.origin );
	for ( i = 0; i < 3; i++ ) {
		VectorCopy( d->axis[i], box.axis[i] );
		VectorNormalize( box.axis[i] );
		box.halfSize[i] = d->halfSize[i];
	}
	BoxPlanes( &box );

	// the box's world bounds
	VectorCopy( box.mins, mins );
	VectorCopy( box.maxs, maxs );

	// world surfaces
	tr.viewCount++;
	BoxSurfaces( tr.world->nodes, mins, maxs, list, ARRAY_LEN( list ), &listLen );
	slot = BeginSlot( d, 0 );
	for ( i = 0; i < listLen; i++ ) {
		AddSurface( slot, &box, list[i] );
	}
	total += KeepSlot();

	// brush models the renderer has drawn, in their own space
	for ( i = 1; i < tr.world->numBModels && i < MAX_DECAL_BMODELS; i++ ) {
		bmodelXform_t *xf = &bmodelXf[i];
		bmodel_t *bm = &tr.world->bmodels[i];
		decalBox_t lbox;
		vec3_t delta;

		if ( !xf->seen || !bm->numSurfaces ) {
			continue;
		}
		// box into model space
		VectorSubtract( box.origin, xf->origin, delta );
		for ( j = 0; j < 3; j++ ) {
			lbox.origin[j] = DotProduct( delta, xf->axis[j] );
		}
		for ( k = 0; k < 3; k++ ) {
			for ( j = 0; j < 3; j++ ) {
				lbox.axis[k][j] = DotProduct( box.axis[k], xf->axis[j] );
			}
		}
		VectorCopy( box.halfSize, lbox.halfSize );
		BoxPlanes( &lbox );
		if ( !BoxBoundsOverlap( lbox.mins, lbox.maxs, bm->bounds[0], bm->bounds[1] ) ) {
			continue;
		}
		slot = BeginSlot( d, i );
		VectorCopy( lbox.origin, slot->d.origin );
		for ( k = 0; k < bm->numSurfaces; k++ ) {
			AddSurface( slot, &lbox, tr.world->surfaces + bm->firstSurface + k );
		}
		total += KeepSlot();
	}
	return total;
}

static qboolean DecalExpired( const oaxDecalSlot_t *slot, int time ) {
	return slot->d.lifeMs > 0 && time >= slot->d.startTime + slot->d.lifeMs;
}

int R_OAXDecalsLive( void ) {
	int i, live = 0;

	for ( i = 0; i < OAX_MAX_DECALS; i++ ) {
		if ( decals[i].inUse && !DecalExpired( &decals[i], decalClock ) ) {
			live++;
		}
	}
	return live;
}

/*
=============
R_OAXAddWorldDecals

Per view: the world's decals (the world entity is current).
=============
*/
void R_OAXAddWorldDecals( void ) {
	int i;

	decalClock = tr.refdef.time;
	for ( i = 0; i < OAX_MAX_DECALS; i++ ) {
		oaxDecalSlot_t *slot = &decals[i];

		if ( !slot->inUse ) {
			continue;
		}
		if ( DecalExpired( slot, tr.refdef.time ) ) {
			slot->inUse = qfalse;
			continue;
		}
		if ( slot->owner || tr.refdef.time < slot->d.startTime ) {
			continue;
		}
		if ( R_CullBox( slot->bounds ) == CULL_OUT ) {
			continue;
		}
		R_AddDrawSurf( (surfaceType_t *)&slot->surf, R_GetShaderByHandle( slot->d.shader ), 0, qfalse, qfalse, 0 );
	}
}

/*
=============
R_OAXAddBmodelDecals

From R_AddBrushModelSurfaces: remembers where the model is drawn (new
decals are projected into its space with this) and adds its decals.
=============
*/
void R_OAXAddBmodelDecals( int bmodelIndex, const trRefEntity_t *ent ) {
	bmodelXform_t *xf;
	int i;

	if ( bmodelIndex <= 0 || bmodelIndex >= MAX_DECAL_BMODELS ) {
		return;
	}
	if ( tr.refdef.rdflags & ( RDF_NOWORLDMODEL | RDF_OAX_SKYPORTAL ) ) {
		return;
	}
	xf = &bmodelXf[bmodelIndex];
	xf->seen = qtrue;
	VectorCopy( ent->e.origin, xf->origin );
	AxisCopy( (vec3_t *)ent->e.axis, xf->axis );

	if ( !r_oaxDecals->integer || ( tr.viewParms.flags & ( VPF_SHADOWMAP | VPF_DEPTHSHADOW ) ) ) {
		return;
	}
	for ( i = 0; i < OAX_MAX_DECALS; i++ ) {
		oaxDecalSlot_t *slot = &decals[i];

		if ( !slot->inUse || slot->owner != bmodelIndex || DecalExpired( slot, tr.refdef.time )
			|| tr.refdef.time < slot->d.startTime ) {
			continue;
		}
		R_AddDrawSurf( (surfaceType_t *)&slot->surf, R_GetShaderByHandle( slot->d.shader ), 0, qfalse, qfalse, 0 );
	}
}

/*
=============
RB_SurfaceOAXDecal
=============
*/
void RB_SurfaceOAXDecal( srfOaxDecal_t *surf ) {
	oaxDecalSlot_t *slot = &decals[surf->index];
	const oaxDecal_t *d = &slot->d;
	float fade = 1.0f;
	int i, j, time = backEnd.refdef.time;

	if ( !slot->inUse ) {
		return;
	}
	if ( d->lifeMs > 0 && d->fadeMs > 0 && time > d->startTime + d->lifeMs - d->fadeMs ) {
		fade = (float)( d->startTime + d->lifeMs - time ) / d->fadeMs;
		if ( fade < 0 ) {
			fade = 0;
		}
	}

	RB_CheckVao( tess.vao );
	RB_CHECKOVERFLOW( slot->numVerts, slot->numVerts * 3 );

	for ( i = 0; i < slot->numPolys; i++ ) {
		int first = slot->polyFirst[i], n = slot->polyNum[i];
		int base = tess.numVertexes;

		for ( j = 0; j < n; j++ ) {
			int v = first + j, k;
			float s = slot->strength[v] * fade;
			float c[4];

			VectorCopy( slot->xyz[v], tess.xyz[tess.numVertexes] );
			tess.texCoords[tess.numVertexes][0] = slot->st[v][0];
			tess.texCoords[tess.numVertexes][1] = slot->st[v][1];
			if ( d->flags & OAXDECAL_ALPHAFADE ) {
				c[0] = d->rgba[0];
				c[1] = d->rgba[1];
				c[2] = d->rgba[2];
				c[3] = d->rgba[3] * s;
			} else {
				c[0] = d->rgba[0] * s;
				c[1] = d->rgba[1] * s;
				c[2] = d->rgba[2] * s;
				c[3] = d->rgba[3];
			}
			for ( k = 0; k < 4; k++ ) {
				float x = c[k] < 0 ? 0 : ( c[k] > 1 ? 1 : c[k] );
				tess.color[tess.numVertexes][k] = (unsigned short)( x * 65535.0f + 0.5f );
			}
			tess.numVertexes++;
		}
		for ( j = 0; j < n - 2; j++ ) {
			tess.indexes[tess.numIndexes + 0] = base;
			tess.indexes[tess.numIndexes + 1] = base + j + 1;
			tess.indexes[tess.numIndexes + 2] = base + j + 2;
			tess.numIndexes += 3;
		}
	}
	oaxFxStats.decalsDrawn++;
	oaxFxStats.decalPolysDrawn += slot->numPolys;
}
