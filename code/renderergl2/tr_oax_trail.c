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
tr_oax_trail.c: ribbon trails (phase 6).

The cgame keeps each trailed entity's trajectory history and adds the
points (x y z ageMs, newest first) every frame with CG_OAX_R_ADDTRAIL.
The renderer turns them into a strip that faces the viewer of each view
(main, reflections) when the batch is drawn, so a trail looks right from
every camera. Width and colour run from the head (age 0) to lifeMs; the
oldest segment is cut exactly at lifeMs, so the tail does not pop as
history points age out.
===========================================================================
*/

#include "tr_local.h"

static srfOaxTrail_t	trails[OAX_MAX_SCENE_TRAILS];
static int				numTrails, firstTrail;
static float			trailPoints[OAX_MAX_TRAIL_POINTS][4];
static int				numTrailPoints;

void R_OAXTrailsReset( void ) {
	numTrails = firstTrail = numTrailPoints = 0;
}

void R_OAXTrailsInitNextFrame( void ) {
	numTrails = firstTrail = numTrailPoints = 0;
}

void R_OAXTrailsClearScene( void ) {
	firstTrail = numTrails;
}

void R_OAXTrailsEndScene( void ) {
	firstTrail = numTrails;
}

void R_OAXTrailsBeginScene( void ) {
	tr.refdef.oaxFirstTrail = firstTrail;
	tr.refdef.oaxNumTrails = numTrails - firstTrail;
}

/*
=================
RE_OAXAddTrail
=================
*/
void RE_OAXAddTrail( const oaxTrail_t *t, const float *points ) {
	srfOaxTrail_t *surf;
	float maxWidth, life;
	int i, n;

	if ( !tr.registered || !r_oaxTrails->integer || !t || !points || t->numPoints < 2 ) {
		return;
	}
	if ( numTrails >= OAX_MAX_SCENE_TRAILS || numTrailPoints + t->numPoints > OAX_MAX_TRAIL_POINTS ) {
		ri.Printf( PRINT_DEVELOPER, "WARNING: RE_OAXAddTrail: trail lists are full\n" );
		return;
	}
	life = t->lifeMs > 0 ? t->lifeMs : 1;
	surf = &trails[numTrails];
	surf->surfaceType = SF_OAX_TRAIL;
	surf->t = *t;
	surf->firstPoint = numTrailPoints;
	n = 0;
	for ( i = 0; i < t->numPoints; i++ ) {
		const float *p = points + i * 4;
		float *out = trailPoints[numTrailPoints + n];

		if ( p[3] < 0 ) {
			continue;
		}
		// a resting entity repeats its position: no zero-length segments
		if ( n > 0 && p[3] < life ) {
			const float *prev = trailPoints[numTrailPoints + n - 1];
			vec3_t d;

			VectorSubtract( p, prev, d );
			if ( DotProduct( d, d ) < 0.25f ) {
				continue;
			}
		}
		if ( p[3] >= life ) {
			// cut the last segment at exactly lifeMs
			if ( n > 0 ) {
				const float *prev = trailPoints[numTrailPoints + n - 1];
				float f = ( life - prev[3] ) / ( p[3] - prev[3] );

				if ( f > 0 ) {
					out[0] = prev[0] + f * ( p[0] - prev[0] );
					out[1] = prev[1] + f * ( p[1] - prev[1] );
					out[2] = prev[2] + f * ( p[2] - prev[2] );
					out[3] = life;
					n++;
				}
			}
			break;
		}
		Vector4Copy( p, out );
		n++;
	}
	if ( n < 2 ) {
		return;
	}
	surf->t.numPoints = n;
	numTrailPoints += n;
	numTrails++;

	maxWidth = MAX( fabs( t->width[0] ), fabs( t->width[1] ) ) * 0.5f + 1.0f;
	ClearBounds( surf->bounds[0], surf->bounds[1] );
	for ( i = 0; i < n; i++ ) {
		AddPointToBounds( trailPoints[surf->firstPoint + i], surf->bounds[0], surf->bounds[1] );
	}
	for ( i = 0; i < 3; i++ ) {
		surf->bounds[0][i] -= maxWidth;
		surf->bounds[1][i] += maxWidth;
	}
	oaxFxStats.trailsAdded++;
}

/*
=================
R_OAXAddTrailSurfaces
=================
*/
void R_OAXAddTrailSurfaces( void ) {
	int i;

	for ( i = 0; i < tr.refdef.oaxNumTrails; i++ ) {
		srfOaxTrail_t *surf = &trails[tr.refdef.oaxFirstTrail + i];

		if ( R_CullBox( surf->bounds ) == CULL_OUT ) {
			continue;
		}
		R_AddDrawSurf( (surfaceType_t *)surf, R_GetShaderByHandle( surf->t.shader ), 0, qfalse, qfalse, 0 );
	}
}

static unsigned short ColorChannel( float c ) {
	if ( c <= 0 ) {
		return 0;
	}
	if ( c >= 1 ) {
		return 65535;
	}
	return (unsigned short)( c * 65535.0f + 0.5f );
}

/*
=================
RB_SurfaceOAXTrail

The strip, facing this view's origin.
=================
*/
void RB_SurfaceOAXTrail( srfOaxTrail_t *surf ) {
	const oaxTrail_t *t = &surf->t;
	const float (*p)[4] = (const float (*)[4])trailPoints[surf->firstPoint];
	int n = t->numPoints;
	float life = t->lifeMs > 0 ? t->lifeMs : 1;
	float along = 0;
	vec3_t eye;
	int i, base;

	RB_CheckVao( tess.vao );
	RB_CHECKOVERFLOW( n * 2, ( n - 1 ) * 6 );

	VectorCopy( backEnd.viewParms.or.origin, eye );
	base = tess.numVertexes;
	for ( i = 0; i < n; i++ ) {
		vec3_t tangent, toEye, side;
		float f = p[i][3] / life;
		float w = ( t->width[0] + f * ( t->width[1] - t->width[0] ) ) * 0.5f;
		float tc;
		int k, v = tess.numVertexes;

		if ( i == 0 ) {
			VectorSubtract( p[0], p[1], tangent );
		} else if ( i == n - 1 ) {
			VectorSubtract( p[n - 2], p[n - 1], tangent );
		} else {
			VectorSubtract( p[i - 1], p[i + 1], tangent );
		}
		if ( i > 0 ) {
			along += Distance( p[i], p[i - 1] );
		}
		VectorSubtract( eye, p[i], toEye );
		CrossProduct( tangent, toEye, side );
		if ( VectorNormalize( side ) == 0 ) {
			VectorSet( side, 0, 0, 1 );
		}

		VectorMA( p[i], w, side, tess.xyz[v] );
		VectorMA( p[i], -w, side, tess.xyz[v + 1] );
		tc = t->texLength > 0 ? along / t->texLength : f;
		tess.texCoords[v][0] = 0;
		tess.texCoords[v][1] = tc;
		tess.texCoords[v + 1][0] = 1;
		tess.texCoords[v + 1][1] = tc;
		for ( k = 0; k < 4; k++ ) {
			tess.color[v][k] = tess.color[v + 1][k] = ColorChannel( t->rgba[0][k] + f * ( t->rgba[1][k] - t->rgba[0][k] ) );
		}
		tess.numVertexes += 2;
	}
	for ( i = 0; i < n - 1; i++ ) {
		int v = base + i * 2;

		tess.indexes[tess.numIndexes + 0] = v;
		tess.indexes[tess.numIndexes + 1] = v + 1;
		tess.indexes[tess.numIndexes + 2] = v + 2;
		tess.indexes[tess.numIndexes + 3] = v + 2;
		tess.indexes[tess.numIndexes + 4] = v + 1;
		tess.indexes[tess.numIndexes + 5] = v + 3;
		tess.numIndexes += 6;
	}
	oaxFxStats.trailsDrawn++;
	oaxFxStats.trailPointsDrawn += n;
}
