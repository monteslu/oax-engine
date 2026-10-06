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
tr_ulight.c: unified lighting, renderer frontend (phase 5).

Adapted from DOOM-3 neo/game/Light.cpp (idGameEdit::ParseSpawnArgsToRenderLight:
light entity keys), neo/renderer/tr_lightrun.cpp (R_SetLightProject,
R_SetLightFrustum, R_DeriveLightData), neo/renderer/tr_light.cpp
(R_CalcLightScissorRectangle / R_ClippedLightScissorRectangle),
neo/renderer/tr_polytope.cpp (R_PolytopeSurface: the light volume's
faces) and neo/renderer/Image_init.cpp (R_QuadraticImage,
R_CreateNoFalloffImage, R_FlatNormalImage, R_WhiteImage, R_BlackImage).
Changes: ported to C on renderergl2's data; idPlane becomes vec4_t with
the same a*x+b*y+c*z+d convention; light-to-surface interactions come from
the BSP (light PVS + volume) instead of area references; culling uses Q3
areas and the snapshot areamask; Q3 `light` keys convert to D3 light parms;
`_pointlight` and `_spotlight` are new generated images (our own curves).
*/

#include "tr_local.h"

uLightWorld_t ulw;

cvar_t *r_ulight;
cvar_t *r_ulightShadows;
cvar_t *r_ulightUE1Floor;
cvar_t *r_ulightShadowMode;
cvar_t *r_ulightScissor;
cvar_t *r_ulightAreaCull;
cvar_t *r_ulightSpecular;
cvar_t *r_ulightShadowBias;
cvar_t *r_ulightDebug;
cvar_t *r_ulightCasterDump;
cvar_t *r_dlightShadows;
cvar_t *r_shadowMapSizeU;
static cvar_t *r_ulightStencil;     // registered again at each renderer start (latched)

image_t *ulightImages_quadratic;
image_t *ulightImages_noFalloff;
image_t *ulightImages_pointLight;
image_t *ulightImages_spotLight;
image_t *ulightImages_flat;
image_t *ulightImages_white;
image_t *ulightImages_black;

// a big converted map's view: 60+ lights with hundreds of casters each, plus
// the ambient list and each light's lit surfaces; a full pool leaves the
// later lights with nothing to light (statSurfsFull counts the views)
#define MAX_ULIGHT_SURFS 262144

// per-frame pools (the frontend runs every view of a frame before the backend)
static drawSurf_t ulSurfs[MAX_ULIGHT_SURFS];
static int        ulNumSurfs;
static uView_t    ulViews[MAX_ULIGHT_VIEWS];
static int        ulNumViews;
static int        ulPoolFrame = -1;

// world surface lookup: srf pointer -> world surface index
static int        *surfHashIdx;
static void      **surfHashKey;
static int         surfHashSize;

// per light: bitsets over the world surfaces (in volume, facing the light)
static byte      **lightSurfBits;
static byte      **lightFacingBits;
static int         surfBitBytes;

// entity-lump ordinal -> light index (-1 none)
static int        *ordinalToLight;
static int         numOrdinals;

static int         numMapLightsAlloc;

/*
=====================================================================

GENERATED LIGHT IMAGES

=====================================================================
*/

#define QUADRATIC_WIDTH  32
#define QUADRATIC_HEIGHT 4
#define FALLOFF_TEXTURE_SIZE 64

// Image_init.cpp R_QuadraticImage
static void R_QuadraticImage( byte data[QUADRATIC_HEIGHT][QUADRATIC_WIDTH][4] ) {
	int x, y, b;

	for ( x = 0; x < QUADRATIC_WIDTH; x++ ) {
		for ( y = 0; y < QUADRATIC_HEIGHT; y++ ) {
			float d;

			d = x - ( QUADRATIC_WIDTH / 2 - 0.5f );
			d = fabs( d );
			d -= 0.5f;
			d /= QUADRATIC_WIDTH / 2;

			d = 1.0f - d;
			d = d * d;

			b = (int)( d * 255 );
			if ( b <= 0 ) {
				b = 0;
			} else if ( b > 255 ) {
				b = 255;
			}
			data[y][x][0] = data[y][x][1] = data[y][x][2] = b;
			data[y][x][3] = 255;
		}
	}
}

static void R_ULightCreateImages( void ) {
	static byte quad[QUADRATIC_HEIGHT][QUADRATIC_WIDTH][4];
	static byte nofall[16][FALLOFF_TEXTURE_SIZE][4];
	static byte point[64][64][4];
	static byte spot[64][64][4];
	byte flat[2][2][4], white[2][2][4], black[2][2][4];
	imgFlags_t flags = IMGFLAG_CLAMPTOEDGE | IMGFLAG_NO_COMPRESSION | IMGFLAG_NOLIGHTSCALE;
	int x, y;

	R_QuadraticImage( quad );
	ulightImages_quadratic = R_CreateImage( "_quadratic", (byte *)quad, QUADRATIC_WIDTH, QUADRATIC_HEIGHT, IMGTYPE_COLORALPHA, flags, 0 );

	// Image_init.cpp R_CreateNoFalloffImage: white, zero clamped
	Com_Memset( nofall, 0, sizeof( nofall ) );
	for ( x = 1; x < FALLOFF_TEXTURE_SIZE - 1; x++ ) {
		for ( y = 1; y < 15; y++ ) {
			nofall[y][x][0] = nofall[y][x][1] = nofall[y][x][2] = nofall[y][x][3] = 255;
		}
	}
	ulightImages_noFalloff = R_CreateImage( "_noFalloff", (byte *)nofall, FALLOFF_TEXTURE_SIZE, 16, IMGTYPE_COLORALPHA, flags, 0 );

	// our default point light projection: (1 - r)^2 over the unit disc,
	// r = distance from the center in units of the light radius
	for ( y = 0; y < 64; y++ ) {
		for ( x = 0; x < 64; x++ ) {
			float u = ( x + 0.5f ) / 64.0f * 2.0f - 1.0f;
			float v = ( y + 0.5f ) / 64.0f * 2.0f - 1.0f;
			float r = sqrt( u * u + v * v );
			float d = r < 1.0f ? ( 1.0f - r ) * ( 1.0f - r ) : 0.0f;
			int b = (int)( d * 255.0f + 0.5f );

			point[y][x][0] = point[y][x][1] = point[y][x][2] = b;
			point[y][x][3] = 255;
		}
	}
	ulightImages_pointLight = R_CreateImage( "_pointlight", (byte *)point, 64, 64, IMGTYPE_COLORALPHA, flags, 0 );

	// our default projected light image: a disc with a soft edge
	for ( y = 0; y < 64; y++ ) {
		for ( x = 0; x < 64; x++ ) {
			float u = ( x + 0.5f ) / 64.0f * 2.0f - 1.0f;
			float v = ( y + 0.5f ) / 64.0f * 2.0f - 1.0f;
			float r = sqrt( u * u + v * v );
			float d = r < 0.75f ? 1.0f : r < 1.0f ? ( 1.0f - r ) / 0.25f : 0.0f;
			int b = (int)( d * 255.0f + 0.5f );

			spot[y][x][0] = spot[y][x][1] = spot[y][x][2] = b;
			spot[y][x][3] = 255;
		}
	}
	ulightImages_spotLight = R_CreateImage( "_spotlight", (byte *)spot, 64, 64, IMGTYPE_COLORALPHA, flags, 0 );

	// Image_init.cpp R_FlatNormalImage, R_WhiteImage, R_BlackImage
	for ( y = 0; y < 2; y++ ) {
		for ( x = 0; x < 2; x++ ) {
			flat[y][x][0] = 128; flat[y][x][1] = 128; flat[y][x][2] = 255; flat[y][x][3] = 255;
			white[y][x][0] = white[y][x][1] = white[y][x][2] = white[y][x][3] = 255;
			black[y][x][0] = black[y][x][1] = black[y][x][2] = 0; black[y][x][3] = 255;
		}
	}
	ulightImages_flat = R_CreateImage( "_flat", (byte *)flat, 2, 2, IMGTYPE_NORMAL, IMGFLAG_NO_COMPRESSION | IMGFLAG_NOLIGHTSCALE, 0 );
	ulightImages_white = R_CreateImage( "_white", (byte *)white, 2, 2, IMGTYPE_COLORALPHA, flags, 0 );
	ulightImages_black = R_CreateImage( "_black", (byte *)black, 2, 2, IMGTYPE_COLORALPHA, flags, 0 );
}

/*
=====================================================================

INIT

=====================================================================
*/

void R_ULightInit( void ) {
	r_ulight = ri.Cvar_Get( "r_ulight", "1", CVAR_CHEAT );
	ri.Cvar_SetDescription( r_ulight, "Unified lighting for maps that ask for it (oax_lighting unified|hybrid). 0 renders them like stock maps." );
	r_ulightShadows = ri.Cvar_Get( "r_ulightShadows", "1", CVAR_CHEAT );
	ri.Cvar_SetDescription( r_ulightShadows, "Shadows of unified lights. 0 is a test control only: shadows never turn off on a host, only their resolution scales." );
	r_ulightShadowMode = ri.Cvar_Get( "r_ulightShadowMode", "0", CVAR_CHEAT );
	ri.Cvar_SetDescription( r_ulightShadowMode, "0 = the map's oax_shadowmode, 1 = shadow maps, 2 = stencil volumes." );
	r_ulightScissor = ri.Cvar_Get( "r_ulightScissor", "1", CVAR_CHEAT );
	r_ulightAreaCull = ri.Cvar_Get( "r_ulightAreaCull", "1", CVAR_CHEAT );
	r_ulightSpecular = ri.Cvar_Get( "r_ulightSpecular", "1", CVAR_ARCHIVE );
	r_ulightUE1Floor = ri.Cvar_Get( "r_ulightUE1Floor", "0.0075", CVAR_TEMP );
	ri.Cvar_SetDescription( r_ulightUE1Floor, "UE1 lights: light units taken off each lamp's own contribution (UE1 loses about one display unit per lamp; 0.0075 fitted with the gain, docs/lights.md), read when the map loads." );
	r_ulightShadowBias = ri.Cvar_Get( "r_ulightShadowBias", "0.004", CVAR_CHEAT );
	r_ulightDebug = ri.Cvar_Get( "r_ulightDebug", "0", CVAR_CHEAT );
	r_ulightCasterDump = ri.Cvar_Get( "r_ulightCasterDump", "-1", CVAR_CHEAT | CVAR_TEMP );
	ri.Cvar_SetDescription( r_ulightCasterDump, "Print the shadow casters of the map light with this entity-lump ordinal once (the next view that lights with it), then reset to -1." );
	r_dlightShadows = ri.Cvar_Get( "r_dlightShadows", "0", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_dlightShadows, "Unified lighting: dynamic lights (rockets, muzzle flashes) cast shadows." );
	r_shadowMapSizeU = ri.Cvar_Get( "r_ulightShadowMapSize", "512", CVAR_ARCHIVE | CVAR_LATCH );
	ri.Cvar_CheckRange( r_shadowMapSizeU, 128, 2048, qtrue );
	ri.Cvar_SetDescription( r_shadowMapSizeU, "Unified lighting shadow map size (host tier: 256, 512, 1024). Goldens use 512." );

	Com_Memset( &ulw, 0, sizeof( ulw ) );
	ulPoolFrame = -1;
	R_ULightCreateImages();
	R_MatExprInit();
	RB_ULightInit();
}

void R_ULightShutdown( void ) {
	r_ulightStencil = NULL;     // the next R_Init takes a latched value
	RB_ULightShutdown();
	R_ULightStencilFreeWorld();
	R_ULightZonesFree();
	if ( ulw.lights ) {
		int i;

		for ( i = 0; i < numMapLightsAlloc; i++ ) {
			if ( ulw.lights[i].worldSurfs ) {
				ri.Free( ulw.lights[i].worldSurfs );
			}
		}
		ri.Free( ulw.lights );
	}
	if ( lightSurfBits ) {
		int i;

		for ( i = 0; i < numMapLightsAlloc; i++ ) {
			if ( lightSurfBits[i] ) {
				ri.Free( lightSurfBits[i] );
			}
			if ( lightFacingBits[i] ) {
				ri.Free( lightFacingBits[i] );
			}
		}
		ri.Free( lightSurfBits );
		ri.Free( lightFacingBits );
	}
	if ( surfHashIdx ) {
		ri.Free( surfHashIdx );
		ri.Free( surfHashKey );
	}
	if ( ordinalToLight ) {
		ri.Free( ordinalToLight );
	}
	surfHashIdx = NULL;
	surfHashKey = NULL;
	lightSurfBits = lightFacingBits = NULL;
	ordinalToLight = NULL;
	numOrdinals = 0;
	numMapLightsAlloc = 0;
	Com_Memset( &ulw, 0, sizeof( ulw ) );
}

/*
=================
R_ULightDepthFormat

The render FBO's depth format. Stencil shadow volumes need a packed
depth/stencil buffer; it is only allocated when r_ulightStencil asks for it
(latched: the FBOs are made before the map is known), so stock maps keep
the plain 24-bit depth buffer.
=================
*/

int R_ULightDepthFormat( void ) {
	if ( !r_ulightStencil ) {
		r_ulightStencil = ri.Cvar_Get( "r_ulightStencil", "0", CVAR_ARCHIVE | CVAR_LATCH );
		ri.Cvar_SetDescription( r_ulightStencil, "Unified lighting: give the render target a stencil buffer, for stencil shadow volumes." );
	}
	if ( r_ulightStencil->integer && glRefConfig.framebufferObject ) {
		return GL_DEPTH24_STENCIL8;
	}
	return GL_DEPTH_COMPONENT24;
}

qboolean R_ULightHasStencil( void ) {
	return R_ULightDepthFormat() == GL_DEPTH24_STENCIL8 && tr.renderFbo != NULL;
}

int R_ULightLightingModel( void ) {
	if ( !tr.world || !ulw.loaded || !r_ulight->integer ) {
		return ULIGHT_LIGHTMAP;
	}
	return ulw.lightingModel;
}

/*
=====================================================================

LIGHT DERIVATION (tr_lightrun.cpp)

=====================================================================
*/

static float PlaneDist( const vec4_t p, const vec3_t v ) {
	return p[0] * v[0] + p[1] * v[1] + p[2] * v[2] + p[3];
}

static float Vec4Dot3( const vec4_t a, const vec4_t b ) {
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
}

/*
=====================
R_SetLightProject

All values are relative to the origin. Assumes that right and up are not
normalized. (tr_lightrun.cpp)
=====================
*/
static void R_SetLightProject( vec4_t lightProject[4], const vec3_t origin, const vec3_t target,
	const vec3_t rightVector, const vec3_t upVector, const vec3_t start, const vec3_t stop ) {
	float dist, scale, rLen, uLen, ofs;
	vec3_t normal, right, up, startGlobal;
	vec4_t targetGlobal;
	int i;

	VectorCopy( rightVector, right );
	rLen = VectorNormalize( right );
	VectorCopy( upVector, up );
	uLen = VectorNormalize( up );
	CrossProduct( up, right, normal );
	VectorNormalize( normal );

	dist = DotProduct( target, normal );
	if ( dist < 0 ) {
		dist = -dist;
		VectorNegate( normal, normal );
	}

	scale = ( 0.5f * dist ) / rLen;
	VectorScale( right, scale, right );
	scale = -( 0.5f * dist ) / uLen;
	VectorScale( up, scale, up );

	VectorCopy( normal, lightProject[2] );
	lightProject[2][3] = -DotProduct( origin, lightProject[2] );

	VectorCopy( right, lightProject[0] );
	lightProject[0][3] = -DotProduct( origin, lightProject[0] );

	VectorCopy( up, lightProject[1] );
	lightProject[1][3] = -DotProduct( origin, lightProject[1] );

	// now offset to center
	VectorAdd( target, origin, targetGlobal );
	targetGlobal[3] = 1;
	ofs = 0.5f - Vec4Dot3( targetGlobal, lightProject[0] ) / Vec4Dot3( targetGlobal, lightProject[2] );
	for ( i = 0; i < 4; i++ ) {
		lightProject[0][i] += ofs * lightProject[2][i];
	}
	ofs = 0.5f - Vec4Dot3( targetGlobal, lightProject[1] ) / Vec4Dot3( targetGlobal, lightProject[2] );
	for ( i = 0; i < 4; i++ ) {
		lightProject[1][i] += ofs * lightProject[2][i];
	}

	// set the falloff vector
	VectorSubtract( stop, start, normal );
	dist = VectorNormalize( normal );
	if ( dist <= 0 ) {
		dist = 1;
	}
	VectorScale( normal, 1.0f / dist, lightProject[3] );
	VectorAdd( start, origin, startGlobal );
	lightProject[3][3] = -DotProduct( startGlobal, lightProject[3] );
}

/*
===================
R_SetLightFrustum

Creates plane equations from the light projection, positive sides face out
of the light. (tr_lightrun.cpp)
===================
*/
static void R_SetLightFrustum( vec4_t lightProject[4], vec4_t frustum[6] ) {
	int i, j;

	// we want the planes of s=0, s=q, t=0, and t=q
	Vector4Copy( lightProject[0], frustum[0] );
	Vector4Copy( lightProject[1], frustum[1] );
	for ( j = 0; j < 4; j++ ) {
		frustum[2][j] = lightProject[2][j] - lightProject[0][j];
		frustum[3][j] = lightProject[2][j] - lightProject[1][j];
	}

	// we want the planes of s=0 and s=1 for front and rear clipping planes
	Vector4Copy( lightProject[3], frustum[4] );
	Vector4Copy( lightProject[3], frustum[5] );
	frustum[5][3] -= 1.0f;
	for ( j = 0; j < 4; j++ ) {
		frustum[5][j] = -frustum[5][j];
	}

	for ( i = 0; i < 6; i++ ) {
		float l;

		for ( j = 0; j < 4; j++ ) {
			frustum[i][j] = -frustum[i][j];
		}
		l = VectorLength( frustum[i] );
		if ( l > 0 ) {
			for ( j = 0; j < 4; j++ ) {
				frustum[i][j] /= l;
			}
		}
	}
}

// R_LocalPlaneToGlobal for a model matrix made of axis + origin
static void LocalPlaneToGlobal( const vec3_t axis[3], const vec3_t origin, const vec4_t in, vec4_t out ) {
	vec3_t n;
	int i;

	for ( i = 0; i < 3; i++ ) {
		n[i] = in[0] * axis[0][i] + in[1] * axis[1][i] + in[2] * axis[2][i];
	}
	VectorCopy( n, out );
	out[3] = in[3] - DotProduct( origin, n );
}

// the vertices of the convex volume bounded by the six frustum planes
// (what R_PolytopeSurface produces)
static void R_LightPolytope( uLight_t *l ) {
	int i, j, k, m;

	l->numFrustumVerts = 0;
	ClearBounds( l->bounds[0], l->bounds[1] );
	for ( i = 0; i < 6; i++ ) {
		for ( j = i + 1; j < 6; j++ ) {
			for ( k = j + 1; k < 6; k++ ) {
				const float *a = l->frustum[i], *b = l->frustum[j], *c = l->frustum[k];
				vec3_t bc, ca, ab, p;
				float det;
				qboolean inside = qtrue;

				CrossProduct( b, c, bc );
				CrossProduct( c, a, ca );
				CrossProduct( a, b, ab );
				det = DotProduct( a, bc );
				if ( fabs( det ) < 1e-6f ) {
					continue;
				}
				for ( m = 0; m < 3; m++ ) {
					p[m] = -( a[3] * bc[m] + b[3] * ca[m] + c[3] * ab[m] ) / det;
				}
				for ( m = 0; m < 6; m++ ) {
					if ( PlaneDist( l->frustum[m], p ) > 0.1f ) {
						inside = qfalse;
						break;
					}
				}
				if ( inside && l->numFrustumVerts < (int)ARRAY_LEN( l->frustumVerts ) ) {
					VectorCopy( p, l->frustumVerts[l->numFrustumVerts] );
					l->numFrustumVerts++;
					AddPointToBounds( p, l->bounds[0], l->bounds[1] );
				}
			}
		}
	}
}

static void R_WorldInteractions( int lightNum );

/*
=================
R_DeriveULightData

Fills everything in based on light->parms. (tr_lightrun.cpp R_DeriveLightData)
=================
*/
static void R_DeriveULightData( uLight_t *light, int lightNum ) {
	shader_t *sh = light->parms.shader;
	int i;

	light->projImage = NULL;
	light->falloffImage = NULL;
	light->lightFlags = 0;

	if ( sh ) {
		light->lightFlags = sh->oaxLightFlags;
		light->falloffImage = sh->oaxLightFalloff;
		for ( i = 0; i < MAX_SHADER_STAGES; i++ ) {
			if ( sh->stages[i] && sh->stages[i]->active && sh->stages[i]->bundle[0].image[0] ) {
				light->projImage = sh->stages[i]->bundle[0].image[0];
				break;
			}
		}
	}
	if ( !light->projImage ) {
		light->projImage = light->parms.pointLight ? ulightImages_pointLight : ulightImages_spotLight;
	}
	if ( !light->falloffImage ) {
		// projected lights by default don't diminish with distance
		light->falloffImage = light->parms.pointLight ? ulightImages_quadratic : ulightImages_noFalloff;
	}

	// set the projection
	if ( !light->parms.pointLight ) {
		R_SetLightProject( light->lightProject, vec3_origin, light->parms.target,
			light->parms.right, light->parms.up, light->parms.start, light->parms.end );
	} else {
		// point light
		Com_Memset( light->lightProject, 0, sizeof( light->lightProject ) );
		light->lightProject[0][0] = 0.5f / light->parms.lightRadius[0];
		light->lightProject[1][1] = 0.5f / light->parms.lightRadius[1];
		light->lightProject[3][2] = 0.5f / light->parms.lightRadius[2];
		light->lightProject[0][3] = 0.5f;
		light->lightProject[1][3] = 0.5f;
		light->lightProject[2][3] = 1.0f;
		light->lightProject[3][3] = 0.5f;
	}

	// set the frustum planes
	R_SetLightFrustum( light->lightProject, light->frustum );

	// rotate the light planes and projections by the axis
	for ( i = 0; i < 6; i++ ) {
		vec4_t temp;

		Vector4Copy( light->frustum[i], temp );
		LocalPlaneToGlobal( (const vec3_t *)light->parms.axis, light->parms.origin, temp, light->frustum[i] );
	}
	for ( i = 0; i < 4; i++ ) {
		vec4_t temp;

		Vector4Copy( light->lightProject[i], temp );
		LocalPlaneToGlobal( (const vec3_t *)light->parms.axis, light->parms.origin, temp, light->lightProject[i] );
	}

	// adjust global light origin for off center projections and parallel projections
	// we are just faking parallel by making it a very far off center for now
	if ( light->parms.parallel ) {
		vec3_t dir;

		VectorCopy( light->parms.lightCenter, dir );
		if ( !VectorNormalize( dir ) ) {
			// make point straight up if not specified
			dir[2] = 1;
		}
		VectorMA( light->parms.origin, 100000, dir, light->globalLightOrigin );
	} else {
		VectorCopy( light->parms.origin, light->globalLightOrigin );
		for ( i = 0; i < 3; i++ ) {
			VectorMA( light->globalLightOrigin, light->parms.lightCenter[i], light->parms.axis[i], light->globalLightOrigin );
		}
	}

	R_LightPolytope( light );
	light->derivedCount++;

	if ( lightNum >= 0 && lightNum < ulw.numMapLights ) {
		R_WorldInteractions( lightNum );
	}
}

/*
=====================================================================

LIGHT ENTITIES (Light.cpp ParseSpawnArgsToRenderLight)

=====================================================================
*/

const char *R_ULightArg( const spawnArgs_t *a, const char *key ) {
	int i;

	for ( i = 0; i < a->numKeys; i++ ) {
		if ( !Q_stricmp( a->keys[i], key ) ) {
			return a->values[i];
		}
	}
	return NULL;
}

#define Arg R_ULightArg

static qboolean ArgVector( const spawnArgs_t *a, const char *key, const char *def, vec3_t out ) {
	const char *v = Arg( a, key );

	VectorClear( out );
	if ( !v ) {
		if ( def ) {
			sscanf( def, "%f %f %f", &out[0], &out[1], &out[2] );
		}
		return qfalse;
	}
	sscanf( v, "%f %f %f", &out[0], &out[1], &out[2] );
	return qtrue;
}

static qboolean ArgFloat( const spawnArgs_t *a, const char *key, float def, float *out ) {
	const char *v = Arg( a, key );

	*out = v ? atof( v ) : def;
	return v != NULL;
}

static qboolean ArgBool( const spawnArgs_t *a, const char *key ) {
	const char *v = Arg( a, key );

	return v && atoi( v ) != 0;
}

// angles to a rotation matrix, as idAngles::ToMat3 (yaw only here)
static void YawToAxis( float yaw, vec3_t axis[3] ) {
	vec3_t angles;

	VectorSet( angles, 0, yaw, 0 );
	AnglesToAxis( angles, axis );
}

/*
=================
ParseLight

idGameEdit::ParseSpawnArgsToRenderLight plus the Q3 `light` keys:
`light` (radius), `_color`, `target` (a spot light toward the target
entity), `radius` (spot cone radius at the target).
=================
*/
static qboolean ParseLight( const spawnArgs_t *a, const spawnArgs_t *ents, int numEnts, uLightParms_t *p ) {
	qboolean gotTarget, gotUp, gotRight;
	const char *texture, *v;
	vec3_t color;
	float f;
	int i;

	Com_Memset( p, 0, sizeof( *p ) );

	if ( !ArgVector( a, "light_origin", NULL, p->origin ) ) {
		ArgVector( a, "origin", "0 0 0", p->origin );
	}

	gotTarget = ArgVector( a, "light_target", NULL, p->target );
	gotUp = ArgVector( a, "light_up", NULL, p->up );
	gotRight = ArgVector( a, "light_right", NULL, p->right );
	ArgVector( a, "light_start", "0 0 0", p->start );
	if ( !ArgVector( a, "light_end", NULL, p->end ) ) {
		VectorCopy( p->target, p->end );
	}

	// Q3 spot light: `target` names an entity the light points at
	if ( !gotTarget && ( v = Arg( a, "target" ) ) != NULL ) {
		for ( i = 0; i < numEnts; i++ ) {
			const char *tn = Arg( &ents[i], "targetname" );
			vec3_t dest, dir, side;
			float radius;

			if ( !tn || Q_stricmp( tn, v ) ) {
				continue;
			}
			ArgVector( &ents[i], "origin", "0 0 0", dest );
			VectorSubtract( dest, p->origin, p->target );
			VectorCopy( p->target, dir );
			if ( !VectorNormalize( dir ) ) {
				break;
			}
			ArgFloat( a, "radius", 64, &radius );
			PerpendicularVector( side, dir );
			VectorScale( side, radius, p->right );
			CrossProduct( dir, side, p->up );
			VectorScale( p->up, radius, p->up );
			VectorCopy( p->target, p->end );
			gotTarget = gotUp = gotRight = qtrue;
			break;
		}
	}

	// we should have all of the target/right/up or none of them
	if ( ( gotTarget || gotUp || gotRight ) != ( gotTarget && gotUp && gotRight ) ) {
		ri.Printf( PRINT_WARNING, "Light at (%f,%f,%f) has bad target info\n", p->origin[0], p->origin[1], p->origin[2] );
		return qfalse;
	}

	if ( !gotTarget ) {
		p->pointLight = qtrue;

		// allow an optional relative center of light and shadow offset
		ArgVector( a, "light_center", "0 0 0", p->lightCenter );

		// create a point light
		if ( !ArgVector( a, "light_radius", "300 300 300", p->lightRadius ) ) {
			ArgFloat( a, "light", 300, &f );
			p->lightRadius[0] = p->lightRadius[1] = p->lightRadius[2] = f;
		}
		for ( i = 0; i < 3; i++ ) {
			if ( p->lightRadius[i] < 1 ) {
				p->lightRadius[i] = 1;
			}
		}
	}

	// get the rotation matrix in either full form, or single angle form
	if ( ( v = Arg( a, "light_rotation" ) ) != NULL || ( v = Arg( a, "rotation" ) ) != NULL ) {
		float m[9];

		if ( sscanf( v, "%f %f %f %f %f %f %f %f %f", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5], &m[6], &m[7], &m[8] ) == 9 ) {
			for ( i = 0; i < 3; i++ ) {
				VectorSet( p->axis[i], m[i * 3], m[i * 3 + 1], m[i * 3 + 2] );
			}
		} else {
			AxisClear( p->axis );
		}
	} else {
		ArgFloat( a, "angle", 0, &f );
		YawToAxis( AngleNormalize360( f ), p->axis );
	}

	// check for other attributes
	if ( !ArgVector( a, "_color", "1 1 1", color ) ) {
		ArgVector( a, "color", "1 1 1", color );
	}
	p->shaderParms[0] = color[0];
	p->shaderParms[1] = color[1];
	p->shaderParms[2] = color[2];
	ArgFloat( a, "shaderParm3", 1, &p->shaderParms[3] );
	ArgFloat( a, "shaderParm4", 0, &p->shaderParms[4] );
	ArgFloat( a, "shaderParm5", 0, &p->shaderParms[5] );
	ArgFloat( a, "shaderParm6", 0, &p->shaderParms[6] );
	ArgFloat( a, "shaderParm7", 0, &p->shaderParms[7] );
	// light-mask groups: the light reaches surfaces whose mask shares a bit
	// (surface-world surfaces carry one, docs/map-format.md; everything
	// else is in group 1)
	p->lightMask = 1;
	if ( ( v = Arg( a, "light_mask" ) ) != NULL ) {
		p->lightMask = (unsigned)strtoul( v, NULL, 0 );
	}
	p->noShadows = ArgBool( a, "noshadows" );
	p->noSpecular = ArgBool( a, "nospecular" );
	p->parallel = ArgBool( a, "parallel" );

	texture = Arg( a, "texture" );
	if ( texture && texture[0] ) {
		shader_t *sh = R_FindShader( texture, LIGHTMAP_NONE, qtrue );

		p->shader = ( sh && !sh->defaultShader ) ? sh : NULL;
		if ( !p->shader ) {
			ri.Printf( PRINT_WARNING, "light texture '%s' not found, using the default\n", texture );
		}
	}

	// step 7.5 B: the physical light description (tr_ulight_phys.c)
	if ( R_ULightPhysHasKeys( a ) ) {
		R_ULightPhysParse( a, p );
		// one light mask: a physical light's groups (oax_mask, ue1_bSpecialLit)
		// unless light_mask names them outright
		if ( !Arg( a, "light_mask" ) ) {
			p->lightMask = (unsigned)p->phys.mask;
		}
	}
	p->phys.mask = (int)( p->lightMask & ULIGHT_MASK_ALL );
	return qtrue;
}

/*
=================
R_ParseEntityLump

Splits the world's entity string into key/value sets (one per entity).
=================
*/
static spawnArgs_t *R_ParseEntityLump( int *numEnts ) {
	char *p = tr.world->entityString;
	spawnArgs_t *ents;
	int n = 0, max = 0;
	char *s;

	// count entities
	for ( s = p; *s; s++ ) {
		if ( *s == '{' ) {
			max++;
		}
	}
	ents = ri.Malloc( sizeof( spawnArgs_t ) * ( max + 1 ) );
	Com_Memset( ents, 0, sizeof( spawnArgs_t ) * ( max + 1 ) );

	while ( 1 ) {
		char *token = COM_ParseExt( &p, qtrue );
		spawnArgs_t *e;

		if ( !token[0] ) {
			break;
		}
		if ( token[0] != '{' || n >= max ) {
			break;
		}
		e = &ents[n++];
		while ( 1 ) {
			char key[64];

			token = COM_ParseExt( &p, qtrue );
			if ( !token[0] || token[0] == '}' ) {
				break;
			}
			Q_strncpyz( key, token, sizeof( key ) );
			token = COM_ParseExt( &p, qtrue );
			if ( !token[0] || token[0] == '}' ) {
				break;
			}
			if ( e->numKeys < MAX_LIGHT_KEYS ) {
				Q_strncpyz( e->keys[e->numKeys], key, sizeof( e->keys[0] ) );
				Q_strncpyz( e->values[e->numKeys], token, sizeof( e->values[0] ) );
				e->numKeys++;
			}
		}
	}
	*numEnts = n;
	return ents;
}

/*
=====================================================================

WORLD INTERACTIONS

=====================================================================
*/

static mnode_t *PointInLeaf( const vec3_t p ) {
	mnode_t *node = tr.world->nodes;

	while ( node->contents == -1 ) {
		float d = DotProduct( p, node->plane->normal ) - node->plane->dist;

		node = d > 0 ? node->children[0] : node->children[1];
	}
	return node;
}

static int SurfHashSlot( const void *key ) {
	uintptr_t k = (uintptr_t)key;

	k ^= k >> 17;
	k *= 0x9E3779B1u;
	return (int)( k & ( surfHashSize - 1 ) );
}

static void BuildSurfHash( void ) {
	int i;

	surfHashSize = 1;
	while ( surfHashSize < tr.world->numsurfaces * 2 + 16 ) {
		surfHashSize <<= 1;
	}
	surfHashIdx = ri.Malloc( sizeof( int ) * surfHashSize );
	surfHashKey = ri.Malloc( sizeof( void * ) * surfHashSize );
	Com_Memset( surfHashKey, 0, sizeof( void * ) * surfHashSize );
	for ( i = 0; i < tr.world->numWorldSurfaces; i++ ) {
		void *key = tr.world->surfaces[i].data;
		int h = SurfHashSlot( key );

		while ( surfHashKey[h] ) {
			h = ( h + 1 ) & ( surfHashSize - 1 );
		}
		surfHashKey[h] = key;
		surfHashIdx[h] = i;
	}
}

static int WorldSurfIndex( const void *key ) {
	int h;

	if ( !surfHashKey ) {
		return -1;
	}
	h = SurfHashSlot( key );
	while ( surfHashKey[h] ) {
		if ( surfHashKey[h] == key ) {
			return surfHashIdx[h];
		}
		h = ( h + 1 ) & ( surfHashSize - 1 );
	}
	return -1;
}

// box against the light volume (planes face out)
static qboolean BoxInLight( const uLight_t *l, const vec3_t mins, const vec3_t maxs ) {
	int i;

	if ( mins[0] > l->bounds[1][0] || mins[1] > l->bounds[1][1] || mins[2] > l->bounds[1][2]
		|| maxs[0] < l->bounds[0][0] || maxs[1] < l->bounds[0][1] || maxs[2] < l->bounds[0][2] ) {
		return qfalse;
	}
	for ( i = 0; i < 6; i++ ) {
		const float *p = l->frustum[i];
		vec3_t nearest;

		// the corner furthest inside (most negative distance)
		nearest[0] = p[0] > 0 ? mins[0] : maxs[0];
		nearest[1] = p[1] > 0 ? mins[1] : maxs[1];
		nearest[2] = p[2] > 0 ? mins[2] : maxs[2];
		if ( PlaneDist( p, nearest ) > 0 ) {
			return qfalse;
		}
	}
	return qtrue;
}

typedef struct {
	uLight_t   *light;
	int         lightNum;
	const byte *pvs;
	int        *stamp;
	int         stampValue;
} interactionWalk_t;

static int *surfStamp;
static int  surfStampValue;

static void AddLightArea( uLight_t *l, int area ) {
	int i;

	if ( area < 0 || l->numAreas < 0 ) {
		return;
	}
	for ( i = 0; i < l->numAreas; i++ ) {
		if ( l->areas[i] == area ) {
			return;
		}
	}
	if ( l->numAreas == MAX_ULIGHT_AREAS ) {
		l->numAreas = -1;       // too many: never cull by area
		return;
	}
	l->areas[l->numAreas++] = area;
}

static void WalkLightNodes( interactionWalk_t *w, mnode_t *node ) {
	uLight_t *l = w->light;

	while ( 1 ) {
		if ( node->mins[0] > l->bounds[1][0] || node->mins[1] > l->bounds[1][1] || node->mins[2] > l->bounds[1][2]
			|| node->maxs[0] < l->bounds[0][0] || node->maxs[1] < l->bounds[0][1] || node->maxs[2] < l->bounds[0][2] ) {
			return;
		}
		if ( node->contents != -1 ) {
			break;
		}
		WalkLightNodes( w, node->children[0] );
		node = node->children[1];
	}

	// leaf
	if ( w->pvs && node->cluster >= 0 && !( w->pvs[node->cluster >> 3] & ( 1 << ( node->cluster & 7 ) ) ) ) {
		return;
	}
	if ( node->cluster < 0 ) {
		return;     // solid
	}
	if ( !BoxInLight( l, node->mins, node->maxs ) ) {
		return;
	}
	AddLightArea( l, node->area );

	{
		int c = node->nummarksurfaces;
		int *mark = tr.world->marksurfaces + node->firstmarksurface;

		while ( c-- ) {
			int s = *mark++;
			msurface_t *surf;
			cullinfo_t *ci;

			if ( s < 0 || s >= tr.world->numWorldSurfaces || surfStamp[s] == surfStampValue ) {
				continue;
			}
			surfStamp[s] = surfStampValue;
			surf = &tr.world->surfaces[s];
			ci = &surf->cullinfo;
			if ( *surf->data == SF_SKIP || *surf->data == SF_BAD || *surf->data == SF_FLARE ) {
				continue;
			}
			if ( surf->shader->isSky || ( surf->shader->surfaceFlags & ( SURF_SKY | SURF_NODRAW ) ) ) {
				continue;
			}
			if ( ( ci->type & CULLINFO_BOX ) && !BoxInLight( l, ci->bounds[0], ci->bounds[1] ) ) {
				continue;
			}
			if ( !( (unsigned)R_ULightSurfaceMask( surf->shader, s ) & l->parms.lightMask ) ) {
				continue;	// not in a group this light lights
			}
			if ( l->numWorldSurfs == l->maxWorldSurfs ) {
				int *n;

				l->maxWorldSurfs = l->maxWorldSurfs ? l->maxWorldSurfs * 2 : 64;
				n = ri.Malloc( sizeof( int ) * l->maxWorldSurfs );
				if ( l->worldSurfs ) {
					Com_Memcpy( n, l->worldSurfs, sizeof( int ) * l->numWorldSurfs );
					ri.Free( l->worldSurfs );
				}
				l->worldSurfs = n;
			}
			l->worldSurfs[l->numWorldSurfs++] = s;
			lightSurfBits[w->lightNum][s >> 3] |= 1 << ( s & 7 );

			// does the light reach its front side?
			if ( ( ci->type & CULLINFO_PLANE ) && surf->shader->cullType != CT_TWO_SIDED ) {
				float d;

				if ( l->parms.parallel ) {
					vec3_t dir;

					VectorSubtract( l->globalLightOrigin, l->parms.origin, dir );
					d = DotProduct( dir, ci->plane.normal );
				} else {
					d = DotProduct( l->globalLightOrigin, ci->plane.normal ) - ci->plane.dist;
				}
				if ( surf->shader->cullType == CT_BACK_SIDED ) {
					d = -d;
				}
				if ( d <= 0 ) {
					continue;
				}
			}
			lightFacingBits[w->lightNum][s >> 3] |= 1 << ( s & 7 );
		}
	}
}

/*
=================
R_WorldInteractions

The world surfaces a light can reach: in the light volume and in the PVS
of the light's cluster. Also the areas the light touches.
=================
*/
static void R_WorldInteractions( int lightNum ) {
	uLight_t *l = &ulw.lights[lightNum];
	interactionWalk_t w;
	mnode_t *leaf;

	l->numWorldSurfs = 0;
	l->numAreas = 0;
	Com_Memset( lightSurfBits[lightNum], 0, surfBitBytes );
	Com_Memset( lightFacingBits[lightNum], 0, surfBitBytes );

	Com_Memset( &w, 0, sizeof( w ) );
	w.light = l;
	w.lightNum = lightNum;
	if ( !l->parms.parallel && tr.world->vis ) {
		leaf = PointInLeaf( l->globalLightOrigin );
		if ( leaf->cluster >= 0 ) {
			w.pvs = tr.world->vis + leaf->cluster * tr.world->clusterBytes;
		} else {
			// light center in solid: the volume decides alone
			l->numAreas = -1;
		}
	}
	surfStampValue++;
	WalkLightNodes( &w, tr.world->nodes );
	if ( l->numAreas == 0 ) {
		l->numAreas = -1;
	}
}

/*
=====================================================================

LOADING

=====================================================================
*/

static void R_ULightLoadLights( const void *header ) {
	spawnArgs_t *ents;
	int numEnts, i, n;
	const char *v;

	ents = R_ParseEntityLump( &numEnts );
	if ( !numEnts ) {
		ri.Free( ents );
		return;
	}

	// worldspawn keys
	v = Arg( &ents[0], "oax_lighting" );
	ulw.lightingModel = ULIGHT_LIGHTMAP;
	if ( v && !Q_stricmp( v, "unified" ) ) {
		ulw.lightingModel = ULIGHT_UNIFIED;
	} else if ( v && !Q_stricmp( v, "hybrid" ) ) {
		ulw.lightingModel = ULIGHT_HYBRID;
	}
	if ( ulw.lightingModel == ULIGHT_LIGHTMAP ) {
		ri.Free( ents );
		return;
	}
	ArgVector( &ents[0], "oax_ambient", ulw.lightingModel == ULIGHT_UNIFIED ? "0.1 0.1 0.1" : "0 0 0", ulw.ambient );
	v = Arg( &ents[0], "oax_shadowmode" );
	ulw.shadowMode = ULIGHT_SHADOW_AUTO;
	if ( v && !Q_stricmp( v, "maps" ) ) {
		ulw.shadowMode = ULIGHT_SHADOW_MAPS;
	} else if ( v && !Q_stricmp( v, "stencil" ) ) {
		ulw.shadowMode = ULIGHT_SHADOW_STENCIL;
	}
	R_ULightPhysWorldspawn( &ents[0] );
	R_ULightZonesLoad( header, ents, numEnts );

	// count lights: every `light` in unified maps (kept by _keepLights 1),
	// `rtlight` in both
	n = 0;
	for ( i = 1; i < numEnts; i++ ) {
		const char *cn = Arg( &ents[i], "classname" );

		if ( cn && ( !Q_stricmp( cn, "rtlight" ) || ( ulw.lightingModel == ULIGHT_UNIFIED && !Q_stricmp( cn, "light" ) ) ) ) {
			n++;
		}
	}

	numMapLightsAlloc = n + MAX_DLIGHTS;
	ulw.lights = ri.Malloc( sizeof( uLight_t ) * numMapLightsAlloc );
	Com_Memset( ulw.lights, 0, sizeof( uLight_t ) * numMapLightsAlloc );
	numOrdinals = numEnts;
	ordinalToLight = ri.Malloc( sizeof( int ) * numEnts );
	for ( i = 0; i < numEnts; i++ ) {
		ordinalToLight[i] = -1;
	}

	surfBitBytes = ( tr.world->numsurfaces + 7 ) >> 3;
	lightSurfBits = ri.Malloc( sizeof( byte * ) * numMapLightsAlloc );
	lightFacingBits = ri.Malloc( sizeof( byte * ) * numMapLightsAlloc );
	Com_Memset( lightSurfBits, 0, sizeof( byte * ) * numMapLightsAlloc );
	Com_Memset( lightFacingBits, 0, sizeof( byte * ) * numMapLightsAlloc );
	surfStamp = ri.Hunk_Alloc( sizeof( int ) * ( tr.world->numsurfaces + 1 ), h_low );
	surfStampValue = 0;

	ulw.numMapLights = 0;
	for ( i = 1; i < numEnts && ulw.numMapLights < n; i++ ) {
		const char *cn = Arg( &ents[i], "classname" );
		uLight_t *l;

		if ( !cn || !( !Q_stricmp( cn, "rtlight" ) || ( ulw.lightingModel == ULIGHT_UNIFIED && !Q_stricmp( cn, "light" ) ) ) ) {
			continue;
		}
		l = &ulw.lights[ulw.numMapLights];
		if ( !ParseLight( &ents[i], ents, numEnts, &l->parms ) ) {
			continue;
		}
		l->baseParms = l->parms;
		l->entityNum = i;
		l->rtlight = !Q_stricmp( cn, "rtlight" );
		l->on = !l->parms.phys.startOff;
		l->shadowSlot = -1;
		if ( l->parms.phys.physical ) {
			ulw.numPhysical++;
		}
		v = Arg( &ents[i], "targetname" );
		if ( v ) {
			Q_strncpyz( l->targetname, v, sizeof( l->targetname ) );
		}
		l->gameControlled = v != NULL || Arg( &ents[i], "bind" ) != NULL;
		lightSurfBits[ulw.numMapLights] = ri.Malloc( surfBitBytes + 1 );
		lightFacingBits[ulw.numMapLights] = ri.Malloc( surfBitBytes + 1 );
		ordinalToLight[i] = ulw.numMapLights;
		ulw.numMapLights++;
		R_DeriveULightData( l, ulw.numMapLights - 1 );
	}
	ulw.numLights = ulw.numMapLights;
	ri.Free( ents );

	ri.Printf( PRINT_ALL, "unified lighting: %s, %d lights, ambient %.3f %.3f %.3f\n",
		ulw.lightingModel == ULIGHT_UNIFIED ? "unified" : "hybrid", ulw.numMapLights,
		ulw.ambient[0], ulw.ambient[1], ulw.ambient[2] );
}

/*
=================
R_ULightLoadWorld

Called at the end of RE_LoadWorldMap.
=================
*/
void R_ULightLoadWorld( const void *header ) {
	ulw.loaded = qfalse;
	ulw.numLights = ulw.numMapLights = 0;
	ulw.lightingModel = ULIGHT_LIGHTMAP;
	ulw.numPhysical = 0;
	ulw.overbright = 1;
	ulw.ue1LevelBrightness = 1;
	R_ULightZonesFree();
	if ( !tr.world || !tr.world->entityString ) {
		return;
	}

	BuildSurfHash();
	R_ULightLoadLights( header );
	if ( ulw.lightingModel == ULIGHT_LIGHTMAP ) {
		return;
	}

	// unified maps have no lightmaps: the vertex color every stock stage
	// multiplies by is the ambient floor
	if ( ulw.lightingModel == ULIGHT_UNIFIED ) {
		int i, j;
		uint16_t c[4];

		for ( j = 0; j < 3; j++ ) {
			float f = ulw.ambient[j] * tr.identityLight;

			c[j] = (uint16_t)( 65535.0f * ( f < 0 ? 0 : f > 1 ? 1 : f ) );
		}
		c[3] = 65535;
		for ( i = 0; i < tr.world->numWorldSurfaces; i++ ) {
			srfBspSurface_t *srf = (srfBspSurface_t *)tr.world->surfaces[i].data;

			if ( srf->surfaceType == SF_FACE || srf->surfaceType == SF_GRID || srf->surfaceType == SF_TRIANGLES ) {
				uint16_t sc[3];

				sc[0] = c[0];
				sc[1] = c[1];
				sc[2] = c[2];
				if ( ulw.zoneAmbient && srf->numVerts ) {
					// step 7.5 B: a surface takes the ambient of the zone its
					// centre faces into (a step off it along its normal);
					// surfaces are split at zone boundaries (UE1's are)
					vec3_t centre, normal, n, p, za;
					int k;

					VectorClear( centre );
					VectorClear( normal );
					for ( j = 0; j < srf->numVerts; j++ ) {
						R_VaoUnpackNormal( n, srf->verts[j].normal );
						VectorAdd( centre, srf->verts[j].xyz, centre );
						VectorAdd( normal, n, normal );
					}
					VectorScale( centre, 1.0f / srf->numVerts, centre );
					VectorNormalize( normal );
					VectorMA( centre, 2.0f, normal, p );
					if ( R_ULightZoneAmbientAt( p, za ) ) {
						for ( k = 0; k < 3; k++ ) {
							float f = za[k] * tr.identityLight;

							sc[k] = (uint16_t)( 65535.0f * ( f < 0 ? 0 : f > 1 ? 1 : f ) );
						}
					}
				}
				for ( j = 0; j < srf->numVerts; j++ ) {
					srf->verts[j].color[0] = sc[0];
					srf->verts[j].color[1] = sc[1];
					srf->verts[j].color[2] = sc[2];
				}
			}
		}
	}

	R_ULightStencilLoadWorld( header );
	R_ULightStencilBuildStatic();
	ulw.loaded = qtrue;
}

/*
=====================================================================

GAME UPDATES

=====================================================================
*/

/*
=================
R_ULightUpdateDef

The cgame drives lights it controls (ET_OAX_LIGHT): index is the light's
ordinal in the entity lump. flags: 1 = on, 2 = axis given, 4 = parms given.
=================
*/
void R_ULightUpdateDef( int index, const vec3_t origin, const vec3_t axis[3], const vec3_t rgb, const float *parms, int flags ) {
	uLight_t *l;
	qboolean moved;
	int li;

	if ( !ulw.loaded || index < 0 || index >= numOrdinals || ( li = ordinalToLight[index] ) < 0 ) {
		return;
	}
	l = &ulw.lights[li];
	l->on = ( flags & 1 ) != 0;
	l->lastUpdateFrame = tr.frameCount;
	moved = !VectorCompare( origin, l->parms.origin );
	if ( flags & 2 ) {
		int i;

		for ( i = 0; i < 3; i++ ) {
			if ( !VectorCompare( axis[i], l->parms.axis[i] ) ) {
				moved = qtrue;
			}
		}
		if ( moved ) {
			AxisCopy( (vec3_t *)axis, l->parms.axis );
		}
	}
	VectorCopy( origin, l->parms.origin );
	l->parms.shaderParms[0] = rgb[0];
	l->parms.shaderParms[1] = rgb[1];
	l->parms.shaderParms[2] = rgb[2];
	if ( ( flags & 4 ) && parms ) {
		int i;

		for ( i = 3; i < 12; i++ ) {
			l->parms.shaderParms[i] = parms[i];
		}
	}
	if ( moved ) {
		R_DeriveULightData( l, li );
	}
}

/*
=====================================================================

PER SCENE / PER VIEW

=====================================================================
*/

static void ResetPools( void ) {
	if ( ulPoolFrame != tr.frameCount ) {
		ulPoolFrame = tr.frameCount;
		ulNumSurfs = 0;
		ulNumViews = 0;
		ulw.statVisible = 0;
		ulw.statVisibleIds[0] = 0;
	}
}

/*
=================
R_ULightBeginScene

Per scene: evaluate material expressions; in unified and hybrid maps the
scene's dynamic lights become unified lights without shadows.
=================
*/
void R_ULightBeginScene( void ) {
	int i;

	R_MatExprUpdateFrame();
	ResetPools();

	if ( R_ULightLightingModel() == ULIGHT_LIGHTMAP || ( tr.refdef.rdflags & RDF_NOWORLDMODEL ) ) {
		return;
	}

	ulw.numLights = ulw.numMapLights;
	for ( i = 0; i < tr.refdef.num_dlights && ulw.numLights < numMapLightsAlloc; i++ ) {
		dlight_t *dl = &tr.refdef.dlights[i];
		uLight_t *l = &ulw.lights[ulw.numLights];

		Com_Memset( l, 0, sizeof( *l ) );
		l->entityNum = -1;
		l->dynamic = qtrue;
		l->on = qtrue;
		l->shadowSlot = -1;
		l->parms.pointLight = qtrue;
		l->parms.lightMask = ~0u;	// scene dlights light every group
		AxisClear( l->parms.axis );
		VectorCopy( dl->origin, l->parms.origin );
		VectorSet( l->parms.lightRadius, dl->radius, dl->radius, dl->radius );
		VectorCopy( dl->color, l->parms.shaderParms );
		l->parms.shaderParms[3] = 1;
		l->parms.noShadows = !r_dlightShadows->integer;
		R_DeriveULightData( l, -1 );
		ulw.numLights++;
	}
	tr.refdef.num_dlights = 0;
}

/*
=================
R_ULightInteractionStage

The stage of an opaque shader that is its interaction material: the stage
collapsed to lightall (diffuse + normal + specular), else the first opaque
color stage that isn't a lightmap. -1 if none.
=================
*/
/*
=================
R_ULightShaderIsFullbright

A fullbright (unlit) material: no lightmap (stage, bundle or lightmap
index), and its first stage is
opaque with an explicit `rgbGen identity`. Light interactions skip it, so
it draws at its texture colour wherever it is, lights or none (an unlit
sky-zone wall, a screen). A stage without rgbGen gets identityLighting
and stays lit.
=================
*/
qboolean R_ULightShaderIsFullbright( const shader_t *sh ) {
	const shaderStage_t *first = NULL;
	int i, b;

	if ( sh->lightmapIndex >= 0 ) {
		return qfalse;		// built on a lightmap (collapsed stages keep it in a second bundle)
	}
	for ( i = 0; i < MAX_SHADER_STAGES; i++ ) {
		const shaderStage_t *st = sh->stages[i];

		if ( !st || !st->active ) {
			break;
		}
		for ( b = 0; b < NUM_TEXTURE_BUNDLES; b++ ) {
			if ( st->bundle[b].isLightmap || ( b == 0 && st->bundle[0].tcGen == TCGEN_LIGHTMAP ) ) {
				return qfalse;
			}
		}
		if ( !first ) {
			first = st;
		}
	}
	return first && first->rgbGen == CGEN_IDENTITY && !( first->stateBits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) );
}

int R_ULightInteractionStage( shader_t *sh ) {
	int i;

	if ( sh->oaxInteraction ) {
		return sh->oaxInteraction > 0 ? sh->oaxInteraction - 1 : -1;
	}
	sh->oaxInteraction = -1;   // none, unless a stage qualifies below
	if ( sh->sort > SS_OPAQUE || sh->isSky || ( sh->surfaceFlags & ( SURF_SKY | SURF_NODRAW ) ) || sh->oaxLightFlags & ULSF_LIGHTSHADER ) {
		return -1;
	}
	if ( R_ULightShaderIsFullbright( sh ) ) {
		return -1;		// drawn at its texture colour by the stock stages
	}
	for ( i = 0; i < MAX_SHADER_STAGES; i++ ) {
		shaderStage_t *st = sh->stages[i];

		if ( !st || !st->active ) {
			break;
		}
		if ( st->glslShaderGroup == tr.lightallShader ) {
			sh->oaxInteraction = i + 1;
			return i;
		}
	}
	for ( i = 0; i < MAX_SHADER_STAGES; i++ ) {
		shaderStage_t *st = sh->stages[i];
		int blend;

		if ( !st || !st->active ) {
			break;
		}
		blend = st->stateBits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS );
		if ( st->bundle[0].isLightmap || st->bundle[0].tcGen == TCGEN_LIGHTMAP ) {
			continue;
		}
		if ( blend == 0 || blend == ( GLS_DSTBLEND_SRC_COLOR | GLS_SRCBLEND_ZERO ) || blend == ( GLS_DSTBLEND_ZERO | GLS_SRCBLEND_DST_COLOR ) ) {
			if ( st->bundle[0].image[0] ) {
				sh->oaxInteraction = i + 1;
				return i;
			}
		}
	}
	return -1;
}

qboolean R_ULightStageIsInteraction( shader_t *sh, shaderStage_t *stage ) {
	int i = R_ULightInteractionStage( sh );

	return i >= 0 && sh->stages[i] == stage;
}

// entity bounds in world space
static qboolean EntityBounds( trRefEntity_t *ent, vec3_t mins, vec3_t maxs ) {
	model_t *model;
	vec3_t lmins, lmaxs;
	int i;

	if ( ent->e.reType != RT_MODEL ) {
		return qfalse;
	}
	model = R_GetModelByHandle( ent->e.hModel );
	if ( !model ) {
		return qfalse;
	}
	if ( model->type == MOD_MESH && model->mdv[0] && model->mdv[0]->numFrames ) {
		mdvModel_t *mdv = model->mdv[0];
		int f0 = ent->e.frame % mdv->numFrames, f1 = ent->e.oldframe % mdv->numFrames;

		if ( f0 < 0 ) {
			f0 = 0;
		}
		if ( f1 < 0 ) {
			f1 = 0;
		}
		for ( i = 0; i < 3; i++ ) {
			lmins[i] = MIN( mdv->frames[f0].bounds[0][i], mdv->frames[f1].bounds[0][i] );
			lmaxs[i] = MAX( mdv->frames[f0].bounds[1][i], mdv->frames[f1].bounds[1][i] );
		}
	} else {
		R_ModelBounds( ent->e.hModel, lmins, lmaxs );
	}

	ClearBounds( mins, maxs );
	for ( i = 0; i < 8; i++ ) {
		vec3_t c, w;
		int j;

		c[0] = ( i & 1 ) ? lmaxs[0] : lmins[0];
		c[1] = ( i & 2 ) ? lmaxs[1] : lmins[1];
		c[2] = ( i & 4 ) ? lmaxs[2] : lmins[2];
		VectorCopy( ent->e.origin, w );
		for ( j = 0; j < 3; j++ ) {
			VectorMA( w, c[j], ent->e.axis[j], w );
		}
		AddPointToBounds( w, mins, maxs );
	}
	return qtrue;
}

// a convex polygon clipped to the inside of a plane (normal . p - dist >= 0)
static int ClipPoly( vec3_t *in, int n, vec3_t *out, const vec3_t normal, float dist ) {
	int i, o = 0;

	for ( i = 0; i < n; i++ ) {
		float *a = in[i], *b = in[( i + 1 ) % n];
		float da = DotProduct( a, normal ) - dist, db = DotProduct( b, normal ) - dist;

		if ( da >= 0 ) {
			VectorCopy( a, out[o] );
			o++;
		}
		if ( ( da >= 0 ) != ( db >= 0 ) ) {
			float t = da / ( da - db );

			out[o][0] = a[0] + t * ( b[0] - a[0] );
			out[o][1] = a[1] + t * ( b[1] - a[1] );
			out[o][2] = a[2] + t * ( b[2] - a[2] );
			o++;
		}
	}
	return o;
}

/*
=================
R_LightScissor

R_ClippedLightScissorRectangle: every face of the light volume, clipped to
the view frustum (and the near plane), projected to the window. Returns
qfalse if nothing is on screen.
=================
*/
static qboolean R_LightScissor( uLight_t *l, int scissor[4] ) {
	float x1 = 1e9f, y1 = 1e9f, x2 = -1e9f, y2 = -1e9f;
	vec3_t nearNormal;
	float nearDist;
	int f, i;

	VectorCopy( tr.viewParms.or.axis[0], nearNormal );
	nearDist = DotProduct( tr.viewParms.or.origin, nearNormal ) + r_znear->value;

	for ( f = 0; f < 6; f++ ) {
		vec3_t poly[48], tmp[48], center;
		float ang[48];
		int n = 0, j, k;
		vec3_t u, v;

		// the volume's vertices on this face, ordered around their center
		for ( i = 0; i < l->numFrustumVerts && n < 16; i++ ) {
			if ( fabs( PlaneDist( l->frustum[f], l->frustumVerts[i] ) ) < 0.5f ) {
				VectorCopy( l->frustumVerts[i], poly[n] );
				n++;
			}
		}
		if ( n < 3 ) {
			continue;
		}
		VectorClear( center );
		for ( i = 0; i < n; i++ ) {
			VectorAdd( center, poly[i], center );
		}
		VectorScale( center, 1.0f / n, center );
		PerpendicularVector( u, l->frustum[f] );
		CrossProduct( l->frustum[f], u, v );
		for ( i = 0; i < n; i++ ) {
			vec3_t d;

			VectorSubtract( poly[i], center, d );
			ang[i] = atan2( DotProduct( d, v ), DotProduct( d, u ) );
		}
		for ( i = 1; i < n; i++ ) {
			for ( j = i; j > 0 && ang[j - 1] > ang[j]; j-- ) {
				float t = ang[j];
				vec3_t tv;

				ang[j] = ang[j - 1];
				ang[j - 1] = t;
				VectorCopy( poly[j], tv );
				VectorCopy( poly[j - 1], poly[j] );
				VectorCopy( tv, poly[j - 1] );
			}
		}

		// clip against the view frustum and the near plane
		n = ClipPoly( poly, n, tmp, nearNormal, nearDist );
		for ( k = 0; k < 4 && n >= 3; k++ ) {
			if ( k % 2 == 0 ) {
				n = ClipPoly( tmp, n, poly, tr.viewParms.frustum[k].normal, tr.viewParms.frustum[k].dist );
			} else {
				n = ClipPoly( poly, n, tmp, tr.viewParms.frustum[k].normal, tr.viewParms.frustum[k].dist );
			}
		}
		if ( n < 3 ) {
			continue;
		}
		// after four clips the result is in tmp
		for ( i = 0; i < n; i++ ) {
			vec4_t eye, clip, ndc, win;

			R_TransformModelToClip( tmp[i], tr.viewParms.world.modelMatrix, tr.viewParms.projectionMatrix, eye, clip );
			if ( clip[3] <= 0.01f ) {
				clip[3] = 0.01f;
			}
			R_TransformClipToWindow( clip, &tr.viewParms, ndc, win );
			x1 = MIN( x1, win[0] );
			y1 = MIN( y1, win[1] );
			x2 = MAX( x2, win[0] );
			y2 = MAX( y2, win[1] );
		}
	}

	// the view is inside the volume: the whole view
	if ( BoxInLight( l, tr.viewParms.or.origin, tr.viewParms.or.origin ) ) {
		x1 = y1 = 0;
		x2 = tr.viewParms.viewportWidth;
		y2 = tr.viewParms.viewportHeight;
	}
	if ( x2 < x1 || y2 < y1 ) {
		return qfalse;
	}
	// the fudge boundary
	x1 = floor( x1 ) - 1;
	y1 = floor( y1 ) - 1;
	x2 = ceil( x2 ) + 1;
	y2 = ceil( y2 ) + 1;
	x1 = MAX( x1, 0 );
	y1 = MAX( y1, 0 );
	x2 = MIN( x2, tr.viewParms.viewportWidth );
	y2 = MIN( y2, tr.viewParms.viewportHeight );
	if ( x2 <= x1 || y2 <= y1 ) {
		return qfalse;
	}
	scissor[0] = tr.viewParms.viewportX + (int)x1;
	scissor[1] = tr.viewParms.viewportY + (int)y1;
	scissor[2] = (int)( x2 - x1 );
	scissor[3] = (int)( y2 - y1 );
	return qtrue;
}

static qboolean LightAreaVisible( const uLight_t *l ) {
	int i;

	if ( !r_ulightAreaCull->integer || l->numAreas < 0 || l->dynamic ) {
		return qtrue;
	}
	for ( i = 0; i < l->numAreas; i++ ) {
		int a = l->areas[i];

		if ( !( tr.refdef.areamask[a >> 3] & ( 1 << ( a & 7 ) ) ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

static void ComposeSort( drawSurf_t *ds, surfaceType_t *surface, shader_t *sh, int entityNum, int fogIndex, int cubemap ) {
	ds->sort = ( sh->sortedIndex << QSORT_SHADERNUM_SHIFT ) | ( entityNum << QSORT_REFENTITYNUM_SHIFT ) | ( fogIndex << QSORT_FOGNUM_SHIFT );
	ds->cubemapIndex = cubemap;
	ds->surface = surface;
}

typedef struct {
	drawSurf_t *ds;
	int         entityNum;
	int         worldIndex;     // world surface index, -1 for entities
	vec3_t      bounds[2];
	qboolean    hasBounds;
	qboolean    interaction;    // has an interaction stage (lit, ambient)
	qboolean    metal;          // oaxMetal: reflects a probe
	qboolean    caster;
	int         mask;           // light-mask groups (step 7.5 B)
	qboolean    maskNonDefault; // mask is not the default group alone
} viewSurf_t;

#define MAX_VIEW_SURFS 8192
static viewSurf_t viewSurfs[MAX_VIEW_SURFS];

/*
=================
R_ULightAddView

After a view's draw surfaces are generated: find the lights it sees and the
surfaces each one lights and shadows. Returns the view index + 1, 0 if the
view has no unified lighting.
=================
*/
/*
=================
R_ULightCastersWanted

Whether a view should add the entities it culled as shadow casters
(R_OAXAddCasterEntities): unified or hybrid lighting with shadows on.
=================
*/
qboolean R_ULightCastersWanted( void ) {
	int model = R_ULightLightingModel();

	return model != ULIGHT_LIGHTMAP && ulw.numLights > 0 && r_ulightShadows->integer;
}

/*
=================
R_ULightDumpCasters

r_ulightCasterDump: one light's shadow casters as the console sees them,
for finding what shadows a surface (shader, entity, bounds). World
surfaces come from the light's own list (every opaque surface in its
volume that is not oaxNoShadow); entities from the view.
=================
*/
static void R_ULightDumpCasters( const uLight_t *l, const uViewLight_t *vl ) {
	int k;

	ri.Printf( PRINT_ALL, "ulight casters: light %i at %.0f %.0f %.0f, %i casters (%i static), shadows %s\n", l->entityNum,
		l->parms.origin[0], l->parms.origin[1], l->parms.origin[2], vl->numCaster, vl->shadowSize, vl->shadows ? "on" : "off" );
	for ( k = vl->firstCaster; k < vl->firstCaster + vl->numCaster; k++ ) {
		const drawSurf_t *ds = &ulSurfs[k];
		int entityNum, fogNum, dlighted, pshadowed;
		shader_t *sh;
		vec3_t lo, hi;

		R_DecomposeSort( ds->sort, &entityNum, &sh, &fogNum, &dlighted, &pshadowed );
		VectorClear( lo );
		VectorClear( hi );
		if ( entityNum == REFENTITYNUM_WORLD && ( *ds->surface == SF_FACE || *ds->surface == SF_GRID || *ds->surface == SF_TRIANGLES ) ) {
			const srfBspSurface_t *srf = (const srfBspSurface_t *)ds->surface;
			VectorCopy( srf->cullBounds[0], lo );
			VectorCopy( srf->cullBounds[1], hi );
		}
		ri.Printf( PRINT_ALL, "  %s %s%s %.0f %.0f %.0f .. %.0f %.0f %.0f\n", sh->name,
			entityNum == REFENTITYNUM_WORLD ? "world" : va( "entity %i", entityNum ), sh->isSky ? " sky" : "",
			lo[0], lo[1], lo[2], hi[0], hi[1], hi[2] );
	}
}

// r_ulightCasterDump: the player's own view only (not a portal, a sky portal
// room or a probe capture, which light with the same list)
static qboolean R_ULightDumpView( void ) {
	return !tr.viewParms.isPortal && !( tr.viewParms.flags & VPF_NOCUBEMAPS ) && !( tr.refdef.rdflags & RDF_OAX_SKYPORTAL );
}

// r_ulightCasterDump: why the dumped light drew nothing in this view
static void R_ULightDumpSkip( const uLight_t *l, const char *why ) {
	if ( r_ulightCasterDump->integer >= 0 && l->entityNum == r_ulightCasterDump->integer && R_ULightDumpView() ) {
		ri.Printf( PRINT_ALL, "ulight casters: light %i at %.0f %.0f %.0f (radius %.0f %.0f %.0f, bounds %.0f %.0f %.0f .. %.0f %.0f %.0f, %i frustum verts, phys %i spot %.2f) skipped: %s\n", l->entityNum,
			l->parms.origin[0], l->parms.origin[1], l->parms.origin[2], l->parms.lightRadius[0], l->parms.lightRadius[1], l->parms.lightRadius[2],
			l->bounds[0][0], l->bounds[0][1], l->bounds[0][2], l->bounds[1][0], l->bounds[1][1], l->bounds[1][2], l->numFrustumVerts, l->parms.phys.physical, l->parms.phys.spotScale, why );
		ri.Cvar_Set( "r_ulightCasterDump", "-1" );
	}
}

int R_ULightAddView( int firstDrawSurf, int numVisible, int numDrawSurfs ) {
	static vec3_t entMins[MAX_REFENTITIES], entMaxs[MAX_REFENTITIES];
	static int entState[MAX_REFENTITIES];
	int model = R_ULightLightingModel();
	uView_t *view;
	int numVS = 0, i, j;
	qboolean shadowsOn;

	if ( ( tr.viewParms.flags & ( VPF_SHADOWMAP | VPF_DEPTHSHADOW ) ) || ( tr.refdef.rdflags & RDF_NOWORLDMODEL ) ) {
		return 0;
	}
	// a lightmapped map gets a view only for oaxMetal's reflection probes
	// (no lights, no ambient pass: the stock pass lights it)
	if ( model == ULIGHT_LIGHTMAP && ( !tr.numCubemaps || r_cubeMapping->integer || ( tr.viewParms.flags & VPF_NOCUBEMAPS ) ) ) {
		return 0;
	}
	ResetPools();
	if ( ulNumViews == MAX_ULIGHT_VIEWS ) {
		return 0;
	}
	view = &ulViews[ulNumViews];
	view->numLights = 0;
	view->firstAmbient = view->numAmbient = 0;
	view->firstMetal = view->numMetal = 0;
	view->lightingModel = model;
	VectorCopy( ulw.ambient, view->ambient );
	view->shadowMode = r_ulightShadowMode->integer ? r_ulightShadowMode->integer : ulw.shadowMode;
	if ( view->shadowMode == ULIGHT_SHADOW_AUTO ) {
		view->shadowMode = ULIGHT_SHADOW_MAPS;      // the measured default (3.6)
	}
	if ( view->shadowMode == ULIGHT_SHADOW_STENCIL && !R_ULightHasStencil() ) {
		view->shadowMode = ULIGHT_SHADOW_MAPS;      // no stencil buffer: shadows never turn off
	}
	shadowsOn = r_ulightShadows->integer != 0;

	// the view's opaque surfaces
	Com_Memset( entState, 0, sizeof( int ) * tr.refdef.num_entities );
	if ( numDrawSurfs > MAX_DRAWSURFS ) {
		numDrawSurfs = MAX_DRAWSURFS;
	}
	for ( i = firstDrawSurf; i < numDrawSurfs && numVS < MAX_VIEW_SURFS; i++ ) {
		drawSurf_t *ds = &tr.refdef.drawSurfs[i & DRAWSURF_MASK];
		int entityNum, fogNum, dlighted, pshadowed;
		shader_t *sh;
		viewSurf_t *vs;

		R_DecomposeSort( ds->sort, &entityNum, &sh, &fogNum, &dlighted, &pshadowed );
		if ( sh->sort > SS_OPAQUE || sh->isSky || ( sh->surfaceFlags & ( SURF_SKY | SURF_NODRAW ) ) ) {
			continue;
		}
		vs = &viewSurfs[numVS];
		vs->ds = ds;
		vs->entityNum = entityNum;
		vs->worldIndex = -1;
		vs->hasBounds = qfalse;
		vs->interaction = R_ULightInteractionStage( sh ) >= 0;
		if ( i >= numVisible ) {
			vs->interaction = qfalse;	// an entity out of view: a shadow caster only
		}
		vs->metal = vs->interaction && sh->oaxMetal;
		vs->caster = !sh->oaxNoShadow;
		if ( entityNum == REFENTITYNUM_WORLD ) {
			vs->worldIndex = WorldSurfIndex( ds->surface );
			vs->mask = R_ULightSurfaceMask( sh, vs->worldIndex );
		} else {
			vs->mask = R_ULightSurfaceMask( sh, -1 );
		}
		vs->maskNonDefault = vs->mask != ULIGHT_MASK_DEFAULT;
		if ( entityNum == REFENTITYNUM_WORLD ) {
			if ( *ds->surface == SF_FACE || *ds->surface == SF_GRID || *ds->surface == SF_TRIANGLES ) {
				srfBspSurface_t *srf = (srfBspSurface_t *)ds->surface;

				VectorCopy( srf->cullBounds[0], vs->bounds[0] );
				VectorCopy( srf->cullBounds[1], vs->bounds[1] );
				vs->hasBounds = qtrue;
			}
		} else if ( entityNum < tr.refdef.num_entities ) {
			trRefEntity_t *ent = &tr.refdef.entities[entityNum];

			if ( !entState[entityNum] ) {
				entState[entityNum] = EntityBounds( ent, entMins[entityNum], entMaxs[entityNum] ) ? 1 : 2;
			}
			if ( entState[entityNum] == 1 ) {
				VectorCopy( entMins[entityNum], vs->bounds[0] );
				VectorCopy( entMaxs[entityNum], vs->bounds[1] );
				vs->hasBounds = qtrue;
			}
			if ( ent->e.renderfx & ( RF_NOSHADOW | RF_FIRST_PERSON | RF_DEPTHHACK ) ) {
				vs->caster = qfalse;
			}
			if ( ent->e.renderfx & RF_DEPTHHACK ) {
				vs->interaction = qfalse;   // the view weapon keeps its stock lighting
			}
		} else {
			continue;
		}
		numVS++;
	}

	// the ambient pass (unified only): every opaque interaction surface
	view->firstAmbient = ulNumSurfs;
	if ( model == ULIGHT_UNIFIED ) {
		for ( i = 0; i < numVS && ulNumSurfs < MAX_ULIGHT_SURFS; i++ ) {
			if ( viewSurfs[i].interaction ) {
				ulSurfs[ulNumSurfs++] = *viewSurfs[i].ds;
			}
		}
	}
	view->numAmbient = ulNumSurfs - view->firstAmbient;

	// oaxMetal surfaces, for the reflection pass (both lighting models)
	view->firstMetal = ulNumSurfs;
	if ( tr.numCubemaps ) {
		for ( i = 0; i < numVS && ulNumSurfs < MAX_ULIGHT_SURFS; i++ ) {
			if ( viewSurfs[i].metal && viewSurfs[i].interaction ) {
				ulSurfs[ulNumSurfs++] = *viewSurfs[i].ds;
			}
		}
	}
	view->numMetal = ulNumSurfs - view->firstMetal;
	if ( model == ULIGHT_LIGHTMAP && !view->numMetal ) {
		return 0;	// nothing for unified lighting to draw in this view
	}

	// the visible lights
	for ( j = 0; model != ULIGHT_LIGHTMAP && j < ulw.numLights && view->numLights < MAX_VIEW_ULIGHTS; j++ ) {
		uLight_t *l = &ulw.lights[j];
		uViewLight_t *vl;
		vec4_t c;
		int k;

		if ( !l->on || l->numFrustumVerts < 4 ) {
			R_ULightDumpSkip( l, l->on ? "no frustum" : "off" );
			continue;
		}
		if ( !LightAreaVisible( l ) ) {
			R_ULightDumpSkip( l, "its areas are not visible from the view (r_ulightAreaCull)" );
			continue;
		}
		if ( R_CullBox( l->bounds ) == CULL_OUT ) {
			R_ULightDumpSkip( l, "its bounds are outside the view frustum" );
			continue;
		}
		vl = &view->lights[view->numLights];
		Com_Memset( vl, 0, sizeof( *vl ) );
		if ( r_ulightScissor->integer ) {
			if ( !R_LightScissor( l, vl->scissor ) ) {
				R_ULightDumpSkip( l, "its scissor is empty" );
				continue;
			}
		} else {
			vl->scissor[0] = tr.viewParms.viewportX;
			vl->scissor[1] = tr.viewParms.viewportY;
			vl->scissor[2] = tr.viewParms.viewportWidth;
			vl->scissor[3] = tr.viewParms.viewportHeight;
		}

		// the light's color for this frame
		R_StageExprColor( l->parms.shader, 0, l->parms.shaderParms, tr.refdef.floatTime, c );
		for ( k = 0; k < 3; k++ ) {
			vl->color[k] = l->parms.shaderParms[k] * c[k];
		}
		if ( l->parms.phys.physical ) {
			// step 7.5 B: the unnormalised color and the effect's multiplier
			float e = R_ULightPhysEffect( &l->parms.phys, tr.refdef.floatTime );

			for ( k = 0; k < 3; k++ ) {
				vl->color[k] *= l->parms.phys.color[k] * e;
			}
		}
		if ( vl->color[0] <= 0 && vl->color[1] <= 0 && vl->color[2] <= 0 ) {
			continue;
		}
		vl->light = l;
		vl->index = j;
		vl->shadows = shadowsOn && !l->parms.noShadows && !( l->lightFlags & ULSF_NOSHADOWS );
		if ( l->lightFlags & ULSF_FORCESHADOWS ) {
			vl->shadows = shadowsOn;
		}

		// lit surfaces
		vl->firstLit = ulNumSurfs;
		for ( i = 0; i < numVS && ulNumSurfs < MAX_ULIGHT_SURFS; i++ ) {
			viewSurf_t *vs = &viewSurfs[i];

			if ( !vs->interaction ) {
				continue;
			}
			if ( l->parms.lightMask != ULIGHT_MASK_DEFAULT || vs->maskNonDefault ) {
				// light-mask groups (the one mask: light_mask / oax_mask)
				if ( !( l->parms.lightMask & (unsigned)vs->mask ) ) {
					continue;
				}
			}
			if ( vs->worldIndex >= 0 && j < ulw.numMapLights ) {
				if ( !( lightFacingBits[j][vs->worldIndex >> 3] & ( 1 << ( vs->worldIndex & 7 ) ) ) ) {
					continue;
				}
			} else if ( vs->hasBounds ) {
				if ( !BoxInLight( l, vs->bounds[0], vs->bounds[1] ) ) {
					continue;
				}
			}
			ulSurfs[ulNumSurfs++] = *vs->ds;
		}
		vl->numLit = ulNumSurfs - vl->firstLit;
		if ( !vl->numLit ) {
			if ( r_ulightCasterDump->integer >= 0 && l->entityNum == r_ulightCasterDump->integer && R_ULightDumpView() ) {
				// the view's world surfaces inside the light's box, and why each is not lit
				int n = 0;
				{
					const mnode_t *ll = PointInLeaf( l->globalLightOrigin );
					ri.Printf( PRINT_ALL, "  light leaf: cluster %i area %i, bounds %.0f %.0f %.0f .. %.0f %.0f %.0f, contents %x; light areas %i\n",
						ll->cluster, ll->area, ll->mins[0], ll->mins[1], ll->mins[2], ll->maxs[0], ll->maxs[1], ll->maxs[2], ll->contents, l->numAreas );
				}
				for ( k = 0; k < numVS && n < 12; k++ ) {
					const viewSurf_t *vs = &viewSurfs[k];
					int entityNum, fogNum, dlighted, pshadowed, s;
					shader_t *sh;
					const srfBspSurface_t *bs = (const srfBspSurface_t *)vs->ds->surface;
					if ( vs->worldIndex < 0 || bs->numVerts <= 0 || !BoxInLight( l, bs->verts[0].xyz, bs->verts[0].xyz ) ) {
						continue;	// only surfaces with a vertex inside the light's box
					}
					s = vs->worldIndex;
					R_DecomposeSort( vs->ds->sort, &entityNum, &sh, &fogNum, &dlighted, &pshadowed );
					{
						// where the surface is, and whether the light's PVS sees its leaf
						const srfBspSurface_t *srf = (const srfBspSurface_t *)vs->ds->surface;
						const mnode_t *sl = srf->numVerts > 0 ? PointInLeaf( srf->verts[0].xyz ) : NULL;
						const mnode_t *ll = PointInLeaf( l->globalLightOrigin );
						const byte *pvs = ( tr.world->vis && ll->cluster >= 0 ) ? tr.world->vis + ll->cluster * tr.world->clusterBytes : NULL;
						ri.Printf( PRINT_ALL, "  surface vertex %.0f %.0f %.0f in leaf cluster %i area %i: %s the light's PVS; %i verts\n",
							srf->numVerts > 0 ? srf->verts[0].xyz[0] : 0, srf->numVerts > 0 ? srf->verts[0].xyz[1] : 0, srf->numVerts > 0 ? srf->verts[0].xyz[2] : 0,
							sl ? sl->cluster : -2, sl ? sl->area : -2,
							!pvs ? "(no PVS)" : ( sl && sl->cluster >= 0 && ( pvs[sl->cluster >> 3] & ( 1 << ( sl->cluster & 7 ) ) ) ) ? "IN" : "NOT in", srf->numVerts );
					}
					ri.Printf( PRINT_ALL, "  view surface: %s%s (entity %i, world %i, type %i) %s%.0f %.0f %.0f .. %.0f %.0f %.0f: %s, %s, mask %x vs light %x%s\n",
						sh->name, vs->interaction ? "" : " (no interaction stage)", vs->entityNum, s, (int)*vs->ds->surface,
						vs->hasBounds ? "" : "(no bounds) ",
						vs->bounds[0][0], vs->bounds[0][1], vs->bounds[0][2], vs->bounds[1][0], vs->bounds[1][1], vs->bounds[1][2],
						s >= 0 && ( lightSurfBits[j][s >> 3] & ( 1 << ( s & 7 ) ) ) ? "in the light's PVS walk" : "NOT reached by the light's PVS walk",
						s >= 0 && ( lightFacingBits[j][s >> 3] & ( 1 << ( s & 7 ) ) ) ? "facing" : "NOT facing (or not reached)",
						vs->mask, l->parms.lightMask, sh->cullType == CT_TWO_SIDED ? ", two-sided" : "" );
					n++;
				}
			}
			R_ULightDumpSkip( l, va( "none of its %i world surfaces (or any entity) is in this view (surface pool %i of %i used%s)",
				l->numWorldSurfs, ulNumSurfs, MAX_ULIGHT_SURFS, ulNumSurfs >= MAX_ULIGHT_SURFS ? ": FULL" : "" ) );
			continue;
		}

		// shadow casters: the light's world surfaces (cached in the static
		// shadow layer), then the entities in the volume (the dynamic layer)
		vl->firstCaster = ulNumSurfs;
		if ( vl->shadows ) {
			if ( j < ulw.numMapLights ) {
				for ( i = 0; i < l->numWorldSurfs && ulNumSurfs < MAX_ULIGHT_SURFS; i++ ) {
					msurface_t *surf = &tr.world->surfaces[l->worldSurfs[i]];

					if ( surf->shader->sort > SS_OPAQUE || surf->shader->oaxNoShadow ) {
						continue;
					}
					ComposeSort( &ulSurfs[ulNumSurfs++], surf->data, surf->shader, REFENTITYNUM_WORLD, 0, 0 );
				}
			} else {
				// scene dlights with shadows: the world surfaces in view
				for ( i = 0; i < numVS && ulNumSurfs < MAX_ULIGHT_SURFS; i++ ) {
					if ( viewSurfs[i].entityNum == REFENTITYNUM_WORLD && viewSurfs[i].hasBounds && viewSurfs[i].caster
						&& BoxInLight( l, viewSurfs[i].bounds[0], viewSurfs[i].bounds[1] ) ) {
						ulSurfs[ulNumSurfs++] = *viewSurfs[i].ds;
					}
				}
			}
			vl->dynamicCasters = qfalse;
			for ( i = 0; i < numVS && ulNumSurfs < MAX_ULIGHT_SURFS; i++ ) {
				viewSurf_t *vs = &viewSurfs[i];

				if ( vs->entityNum == REFENTITYNUM_WORLD || !vs->caster || !vs->hasBounds ) {
					continue;
				}
				if ( !BoxInLight( l, vs->bounds[0], vs->bounds[1] ) ) {
					continue;
				}
				// the first entity caster starts the dynamic layer
				if ( !vl->dynamicCasters ) {
					vl->dynamicCasters = qtrue;
					vl->shadowSize = ulNumSurfs - vl->firstCaster;  // static count
				}
				ulSurfs[ulNumSurfs++] = *vs->ds;
			}
			if ( !vl->dynamicCasters ) {
				vl->shadowSize = ulNumSurfs - vl->firstCaster;
			}
		}
		vl->numCaster = ulNumSurfs - vl->firstCaster;
		if ( r_ulightCasterDump->integer >= 0 && l->entityNum == r_ulightCasterDump->integer && R_ULightDumpView() ) {
			R_ULightDumpCasters( l, vl );
			ri.Cvar_Set( "r_ulightCasterDump", "-1" );
		}

		view->numLights++;
		if ( ulw.statVisible < 48 ) {
			// map lights by entity-lump ordinal, scene dlights as d<n>
			Q_strcat( ulw.statVisibleIds, sizeof( ulw.statVisibleIds ), l->entityNum >= 0
				? va( "%s%d", ulw.statVisibleIds[0] ? "," : "", l->entityNum )
				: va( "%sd%d", ulw.statVisibleIds[0] ? "," : "", j - ulw.numMapLights ) );
		}
		ulw.statVisible++;
	}

	if ( ulNumSurfs > ulw.statSurfs ) {
		ulw.statSurfs = ulNumSurfs;
	}
	if ( ulNumSurfs >= MAX_ULIGHT_SURFS ) {
		ulw.statSurfsFull++;
		if ( !ulw.warnedSurfsFull ) {
			ulw.warnedSurfsFull = qtrue;
			ri.Printf( PRINT_WARNING, "unified lighting: the view's surface pool (%i) is full; lights past it light nothing this frame\n", MAX_ULIGHT_SURFS );
		}
	}
	ulNumViews++;
	return ulNumViews;
}

uView_t *R_ULightGetView( int index ) {
	if ( index <= 0 || index > ulNumViews ) {
		return NULL;
	}
	return &ulViews[index - 1];
}

drawSurf_t *R_ULightSurfList( int first ) {
	return &ulSurfs[first];
}

/*
=================
R_ULightPublishStats

Named debug values for tests (wasmcart debug_values, `debugvalues`).
=================
*/
void R_ULightPublishStats( int frontEndMsec, int backEndMsec ) {
	if ( !ri.DebugSet || !ulw.loaded ) {
		ulw.statDraws = ulw.statShadowPasses = ulw.statShadowCacheHits = ulw.statStencilTris = 0;
		return;
	}
	// renderer time by the engine clock: wall time on native builds; on a
	// deterministic cart the clock only moves between frames, so this reads
	// 0 there and hosts measure the frame from outside
	ri.DebugSet( "r_frame_ms", va( "%d", frontEndMsec + backEndMsec ) );
	ri.DebugSet( "r_ulight_model", va( "%d", R_ULightLightingModel() ) );
	ri.DebugSet( "r_ulights", va( "%d", ulw.numMapLights ) );
	ri.DebugSet( "r_ulights_visible", va( "%d", ulw.statVisible ) );
	ri.DebugSet( "r_ulights_visible_ids", ulw.statVisibleIds[0] ? ulw.statVisibleIds : "-" );
	ri.DebugSet( "r_ulight_draws", va( "%d", ulw.statDraws ) );
	ri.DebugSet( "r_ulight_surfs", va( "%d", ulw.statSurfs ) );
	ri.DebugSet( "r_ulight_surfs_full", va( "%d", ulw.statSurfsFull ) );
	ri.DebugSet( "r_shadow_passes", va( "%d", ulw.statShadowPasses ) );
	ri.DebugSet( "r_shadow_cache_hits", va( "%d", ulw.statShadowCacheHits ) );
	ri.DebugSet( "r_stencil_tris", va( "%d", ulw.statStencilTris ) );
	if ( ulw.numPhysical || ulw.numZoneAmbient ) {
		R_ULightPhysPublish();
	}
	ulw.statDraws = ulw.statShadowPasses = ulw.statShadowCacheHits = ulw.statStencilTris = 0;
}

/*
=================
RE_OAXUpdateLight

refexport entry for the cgame syscall CG_OAX_R_UPDATELIGHTDEF. axis is
nine floats or NULL, parms twelve floats or NULL.
=================
*/
void RE_OAXUpdateLight( int index, const float *origin, const float *axis, const float *rgb, const float *parms, int flags ) {
	vec3_t ax[3];

	if ( axis ) {
		VectorCopy( axis, ax[0] );
		VectorCopy( axis + 3, ax[1] );
		VectorCopy( axis + 6, ax[2] );
		flags |= 2;
	} else {
		flags &= ~2;
	}
	if ( !parms ) {
		flags &= ~4;
	}
	R_ULightUpdateDef( index, origin, (const vec3_t *)ax, rgb, parms, flags );
}
