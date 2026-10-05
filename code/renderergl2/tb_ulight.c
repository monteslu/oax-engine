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
tb_ulight.c: unified lighting, renderer backend (phase 5).

Light-major drawing for one view, after the view's depth prepass:
  unified: ambient pass (diffuse * oax_ambient, replaces color), then per
           light: shadows, then additive interactions over the light's
           surfaces (depth EQUAL, scissored); then the stock pass draws the
           remaining (non-interaction) stages and the translucent surfaces;
  hybrid:  the stock pass draws the lightmapped opaque surfaces, then the
           realtime lights add, then the translucent surfaces.
Results land in the HDR render FBO, so overbright needs nothing special.

Shadows (DESIGN 3.6): DEPTH_COMPONENT24 cube maps with hardware compare
(samplerCubeShadow) for point lights and 2D maps (sampler2DShadow) for
projected and parallel lights. A light's world casters are rendered once
into a cached slot (the static layer); when entities are inside the light,
the static faces are blitted into a scratch map and the entities drawn over
it (the dynamic overlay). The stencil volume path is in tb_ulight_stencil.c.

The interaction GLSL is written for this engine (interaction_*.glsl); no id
Tech 4 data (interaction.vfp) is used.
===========================================================================
*/

#include "tr_local.h"
#include "tr_fbo.h"

extern const char *fallbackShader_interaction_vp;
extern const char *fallbackShader_interaction_fp;

uLightBackend_t ulb;

// programs: [mode][animation][shadow]
#define UPROG_DEPTH       0
#define UPROG_AMBIENT     1
#define UPROG_INTERACTION 2
#define UPROG_PHYSICAL    3     // step 7.5 B: physical lights (interaction_fp ULIGHT_PHYSICAL)
#define UPROG_AMBIENTZONE 4     // step 7.5 B: ambient from the vertex color (zone ambient)
#define UPROG_REFLECT     5     // oaxMetal: the probe reflection (interaction_fp ULIGHT_REFLECT)
#define UPROG_MODES       6

#define UANIM_NONE   0
#define UANIM_VERTEX 1
#define UANIM_BONE   2

#define USHADOW_NONE 0
#define USHADOW_CUBE 1
#define USHADOW_2D   2

typedef struct {
	shaderProgram_t sp;
	qboolean        valid;
	GLint           lightProjS, lightProjT, lightProjQ, lightFalloffS;
	GLint           lightOriginW, lightColor, diffuseColor, specularColor;
	GLint           shadowParams, shadowMatrix;
	GLint           texNormal, texSpecular, texProj, texFalloff, texShadowCube, texShadow2D;
	GLint           physLight, physLight2, physCurveCount, physSpot;
	GLint           physCurve[ULIGHT_MAX_FALLOFF_POINTS];
	GLint           texCube, metalParms, reflectLod, gridLightDir, gridLight;
} uProgram_t;

static uProgram_t uprogs[UPROG_MODES][3][3];

// texture units for the light images (units the interaction program does
// not use for anything else)
#define UTMU_PROJ        TB_LIGHTMAP
#define UTMU_FALLOFF     TB_DELUXEMAP
#define UTMU_SHADOW2D    TB_SHADOWMAP
#define UTMU_SHADOWCUBE  TB_CUBEMAP

// shadow maps
#define MAX_SHADOW_SLOTS 8

typedef struct {
	image_t   *image;
	uLight_t  *owner;
	int        stamp;
	int        lastUsed;
} shadowSlot_t;

static shadowSlot_t cubeSlots[MAX_SHADOW_SLOTS];
static shadowSlot_t flatSlots[MAX_SHADOW_SLOTS];
static image_t     *scratchCube, *scratchFlat;
static FBO_t       *shadowFbo, *shadowReadFbo;
static int          shadowSize;
static qboolean     shadowResourcesTried;

/*
=====================================================================

PROGRAMS

=====================================================================
*/

static void InitProgram( int mode, int anim, int shadow ) {
	uProgram_t *p = &uprogs[mode][anim][shadow];
	char extra[1024];
	int attribs = ATTR_POSITION | ATTR_NORMAL;
	GLuint prog;

	extra[0] = 0;
	if ( qglesMajorVersion >= 3 ) {
		Q_strcat( extra, sizeof( extra ), "precision highp samplerCubeShadow;\n" );
	}
	if ( mode == UPROG_DEPTH ) {
		Q_strcat( extra, sizeof( extra ), "#define ULIGHT_DEPTH\n" );
	} else {
		attribs |= ATTR_TANGENT | ATTR_TEXCOORD;
		if ( mode == UPROG_AMBIENT ) {
			Q_strcat( extra, sizeof( extra ), "#define ULIGHT_AMBIENT\n" );
		} else if ( mode == UPROG_AMBIENTZONE ) {
			Q_strcat( extra, sizeof( extra ), "#define ULIGHT_AMBIENT\n#define ULIGHT_ZONEAMBIENT\n" );
			attribs |= ATTR_COLOR;
		} else if ( mode == UPROG_PHYSICAL ) {
			Q_strcat( extra, sizeof( extra ), "#define ULIGHT_PHYSICAL\n" );
		} else if ( mode == UPROG_REFLECT ) {
			Q_strcat( extra, sizeof( extra ), "#define ULIGHT_REFLECT\n" );
		}
	}
	if ( anim == UANIM_VERTEX ) {
		Q_strcat( extra, sizeof( extra ), "#define USE_VERTEX_ANIMATION\n" );
		attribs |= ATTR_POSITION2 | ATTR_NORMAL2;
		if ( mode != UPROG_DEPTH ) {
			attribs |= ATTR_TANGENT2;
		}
	} else if ( anim == UANIM_BONE ) {
		Q_strcat( extra, sizeof( extra ), va( "#define USE_BONE_ANIMATION\n#define MAX_GLSL_BONES %d\n", glRefConfig.glslMaxAnimatedBones ) );
		attribs |= ATTR_BONE_INDEXES | ATTR_BONE_WEIGHTS;
	}
	if ( shadow == USHADOW_CUBE ) {
		Q_strcat( extra, sizeof( extra ), "#define USE_SHADOW_CUBE\n" );
	} else if ( shadow == USHADOW_2D ) {
		Q_strcat( extra, sizeof( extra ), "#define USE_SHADOW_2D\n" );
	}
	if ( glRefConfig.swizzleNormalmap ) {
		Q_strcat( extra, sizeof( extra ), "#define SWIZZLE_NORMALMAP\n" );
	}

	if ( !GLSL_InitOAXShader( &p->sp, "interaction", attribs, extra, fallbackShader_interaction_vp, fallbackShader_interaction_fp ) ) {
		ri.Printf( PRINT_WARNING, "unified lighting: interaction program %d/%d/%d failed\n", mode, anim, shadow );
		return;
	}
	prog = p->sp.program;
	p->lightProjS = qglGetUniformLocation( prog, "u_LightProjS" );
	p->lightProjT = qglGetUniformLocation( prog, "u_LightProjT" );
	p->lightProjQ = qglGetUniformLocation( prog, "u_LightProjQ" );
	p->lightFalloffS = qglGetUniformLocation( prog, "u_LightFalloffS" );
	p->lightOriginW = qglGetUniformLocation( prog, "u_LightOriginW" );
	p->lightColor = qglGetUniformLocation( prog, "u_LightColor" );
	p->diffuseColor = qglGetUniformLocation( prog, "u_DiffuseColor" );
	p->specularColor = qglGetUniformLocation( prog, "u_SpecularColor" );
	p->shadowParams = qglGetUniformLocation( prog, "u_ShadowParams" );
	p->shadowMatrix = qglGetUniformLocation( prog, "u_ShadowMatrix" );
	p->texNormal = qglGetUniformLocation( prog, "u_NormalMap" );
	p->texSpecular = qglGetUniformLocation( prog, "u_SpecularMap" );
	p->texProj = qglGetUniformLocation( prog, "u_LightProjMap" );
	p->texFalloff = qglGetUniformLocation( prog, "u_LightFalloffMap" );
	p->texShadowCube = qglGetUniformLocation( prog, "u_ShadowCube" );
	p->texShadow2D = qglGetUniformLocation( prog, "u_Shadow2D" );
	p->physLight = qglGetUniformLocation( prog, "u_PhysLight" );
	p->physLight2 = qglGetUniformLocation( prog, "u_PhysLight2" );
	p->physSpot = qglGetUniformLocation( prog, "u_PhysSpot" );
	{
		int k;

		for ( k = 0; k < ULIGHT_MAX_FALLOFF_POINTS; k++ ) {
			p->physCurve[k] = qglGetUniformLocation( prog, va( "u_PhysCurve[%d]", k ) );
		}
	}
	p->physCurveCount = qglGetUniformLocation( prog, "u_PhysCurveCount" );
	p->texCube = qglGetUniformLocation( prog, "u_CubeMap" );
	p->metalParms = qglGetUniformLocation( prog, "u_MetalParms" );
	p->reflectLod = qglGetUniformLocation( prog, "u_ReflectLod" );
	p->gridLightDir = qglGetUniformLocation( prog, "u_GridLightDir" );
	p->gridLight = qglGetUniformLocation( prog, "u_GridLight" );

	GLSL_SetUniformInt( &p->sp, UNIFORM_DIFFUSEMAP, TB_DIFFUSEMAP );
	if ( p->texNormal >= 0 ) {
		qglProgramUniform1iEXT( prog, p->texNormal, TB_NORMALMAP );
	}
	if ( p->texSpecular >= 0 ) {
		qglProgramUniform1iEXT( prog, p->texSpecular, TB_SPECULARMAP );
	}
	if ( p->texProj >= 0 ) {
		qglProgramUniform1iEXT( prog, p->texProj, UTMU_PROJ );
	}
	if ( p->texFalloff >= 0 ) {
		qglProgramUniform1iEXT( prog, p->texFalloff, UTMU_FALLOFF );
	}
	if ( p->texShadowCube >= 0 ) {
		qglProgramUniform1iEXT( prog, p->texShadowCube, UTMU_SHADOWCUBE );
	}
	if ( p->texShadow2D >= 0 ) {
		qglProgramUniform1iEXT( prog, p->texShadow2D, UTMU_SHADOW2D );
	}
	if ( p->texCube >= 0 ) {
		qglProgramUniform1iEXT( prog, p->texCube, TB_CUBEMAP );
	}
	p->valid = qtrue;
}

void RB_ULightInit( void ) {
	int mode, anim, shadow;

	Com_Memset( uprogs, 0, sizeof( uprogs ) );
	Com_Memset( cubeSlots, 0, sizeof( cubeSlots ) );
	Com_Memset( flatSlots, 0, sizeof( flatSlots ) );
	Com_Memset( &ulb, 0, sizeof( ulb ) );
	scratchCube = scratchFlat = NULL;
	shadowFbo = shadowReadFbo = NULL;
	shadowResourcesTried = qfalse;

	if ( !glRefConfig.framebufferObject ) {
		return;
	}
	for ( mode = 0; mode < UPROG_MODES; mode++ ) {
		for ( anim = 0; anim < 3; anim++ ) {
			if ( anim == UANIM_VERTEX && !glRefConfig.gpuVertexAnimation ) {
				continue;
			}
			if ( anim == UANIM_BONE && !glRefConfig.glslMaxAnimatedBones ) {
				continue;
			}
			for ( shadow = 0; shadow < 3; shadow++ ) {
				if ( shadow && mode != UPROG_INTERACTION && mode != UPROG_PHYSICAL ) {
					continue;
				}
				InitProgram( mode, anim, shadow );
			}
		}
	}
}

void RB_ULightShutdown( void ) {
	int mode, anim, shadow;

	for ( mode = 0; mode < UPROG_MODES; mode++ ) {
		for ( anim = 0; anim < 3; anim++ ) {
			for ( shadow = 0; shadow < 3; shadow++ ) {
				if ( uprogs[mode][anim][shadow].valid ) {
					GLSL_DeleteGPUShader( &uprogs[mode][anim][shadow].sp );
				}
			}
		}
	}
	Com_Memset( uprogs, 0, sizeof( uprogs ) );
	// images and FBOs are freed with the renderer's own lists
	Com_Memset( cubeSlots, 0, sizeof( cubeSlots ) );
	Com_Memset( flatSlots, 0, sizeof( flatSlots ) );
	scratchCube = scratchFlat = NULL;
	shadowFbo = shadowReadFbo = NULL;
	shadowResourcesTried = qfalse;
}

/*
=====================================================================

SHADOW MAPS

=====================================================================
*/

static image_t *CreateShadowImage( const char *name, qboolean cube ) {
	image_t *img = R_CreateImage( name, NULL, shadowSize, shadowSize, IMGTYPE_COLORALPHA,
		IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE | ( cube ? IMGFLAG_CUBEMAP : 0 ), GL_DEPTH_COMPONENT24 );
	GLenum target = cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;

	// hardware compare and 2x2 PCF
	qglTextureParameteriEXT( img->texnum, target, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE );
	qglTextureParameteriEXT( img->texnum, target, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL );
	qglTextureParameteriEXT( img->texnum, target, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	qglTextureParameteriEXT( img->texnum, target, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	return img;
}

static qboolean ShadowResources( void ) {
	if ( shadowFbo ) {
		return qtrue;
	}
	if ( shadowResourcesTried ) {
		return qfalse;
	}
	shadowResourcesTried = qtrue;
	shadowSize = r_shadowMapSizeU->integer;
	if ( shadowSize > glRefConfig.maxRenderbufferSize ) {
		shadowSize = glRefConfig.maxRenderbufferSize;
	}
	scratchCube = CreateShadowImage( "*ulightShadowCube", qtrue );
	scratchFlat = CreateShadowImage( "*ulightShadow2D", qfalse );

	shadowFbo = FBO_Create( "_ulightShadow", shadowSize, shadowSize );
	// a color buffer keeps the FBO complete on drivers that want one
	FBO_CreateBuffer( shadowFbo, GL_RGBA8, 0, 0 );
	FBO_AttachImage( shadowFbo, scratchFlat, GL_DEPTH_ATTACHMENT, 0 );
	if ( !R_CheckFBO( shadowFbo ) ) {
		ri.Printf( PRINT_WARNING, "unified lighting: shadow FBO incomplete\n" );
		shadowFbo = NULL;
		return qfalse;
	}
	shadowReadFbo = FBO_Create( "_ulightShadowRead", shadowSize, shadowSize );
	FBO_Bind( NULL );
	return qtrue;
}

static shadowSlot_t *FindSlot( shadowSlot_t *slots, uLight_t *l, qboolean cube, qboolean *valid ) {
	shadowSlot_t *best = NULL;
	int i;

	*valid = qfalse;
	for ( i = 0; i < MAX_SHADOW_SLOTS; i++ ) {
		if ( slots[i].owner == l && slots[i].image ) {
			slots[i].lastUsed = tr.frameCount;
			*valid = slots[i].stamp == l->derivedCount;
			return &slots[i];
		}
	}
	// a free slot, or the least recently used one not used this frame
	for ( i = 0; i < MAX_SHADOW_SLOTS; i++ ) {
		if ( !slots[i].image ) {
			best = &slots[i];
			break;
		}
		if ( slots[i].lastUsed != tr.frameCount && ( !best || slots[i].lastUsed < best->lastUsed ) ) {
			best = &slots[i];
		}
	}
	if ( !best ) {
		return NULL;
	}
	if ( !best->image ) {
		best->image = CreateShadowImage( va( "*ulightShadow%s%d", cube ? "Cube" : "2D", (int)( best - slots ) ), cube );
	}
	best->owner = l;
	best->stamp = -1;
	best->lastUsed = tr.frameCount;
	return best;
}

// column-major 4x4 helpers
static void MatMul( const float *a, const float *b, float *out ) {
	int i, j;

	for ( i = 0; i < 4; i++ ) {
		for ( j = 0; j < 4; j++ ) {
			out[j * 4 + i] = a[0 * 4 + i] * b[j * 4 + 0] + a[1 * 4 + i] * b[j * 4 + 1] + a[2 * 4 + i] * b[j * 4 + 2] + a[3 * 4 + i] * b[j * 4 + 3];
		}
	}
}

// a matrix from four row vectors (each a plane / vec4 applied to (x y z 1))
static void MatFromRows( const vec4_t r0, const vec4_t r1, const vec4_t r2, const vec4_t r3, float *m ) {
	int j;

	for ( j = 0; j < 4; j++ ) {
		m[j * 4 + 0] = r0[j];
		m[j * 4 + 1] = r1[j];
		m[j * 4 + 2] = r2[j];
		m[j * 4 + 3] = r3[j];
	}
}

// GL cube map face directions and up vectors (the cube map lookup rules)
static const float cubeFwd[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
static const float cubeUp[6][3] = { { 0, -1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }, { 0, -1, 0 }, { 0, -1, 0 } };

static float LightFar( uLight_t *l, const vec3_t eye ) {
	float far = 1;
	int i;

	for ( i = 0; i < l->numFrustumVerts; i++ ) {
		vec3_t d;

		VectorSubtract( l->frustumVerts[i], eye, d );
		far = MAX( far, VectorLength( d ) );
	}
	return far;
}

/*
=================
ShadowView

Projection (clip from world) for one shadow map face; also fills the
lookup parameters for the interaction program.
=================
*/
static void ShadowView( uLight_t *l, int face, float *clip, float *lookup, vec4_t params ) {
	float nearZ = 1.0f;

	if ( l->parms.pointLight && !l->parms.parallel ) {
		const float *eye = l->globalLightOrigin;
		float farZ = LightFar( l, eye );
		vec3_t f, u, r;
		float view[16], proj[16];

		VectorCopy( cubeFwd[face], f );
		VectorCopy( cubeUp[face], u );
		CrossProduct( f, u, r );
		Com_Memset( view, 0, sizeof( view ) );
		view[0] = r[0]; view[4] = r[1]; view[8] = r[2];  view[12] = -DotProduct( r, eye );
		view[1] = u[0]; view[5] = u[1]; view[9] = u[2];  view[13] = -DotProduct( u, eye );
		view[2] = -f[0]; view[6] = -f[1]; view[10] = -f[2]; view[14] = DotProduct( f, eye );
		view[15] = 1;
		Com_Memset( proj, 0, sizeof( proj ) );
		proj[0] = 1;
		proj[5] = 1;
		proj[10] = -( farZ + nearZ ) / ( farZ - nearZ );
		proj[11] = -1;
		proj[14] = -2.0f * farZ * nearZ / ( farZ - nearZ );
		MatMul( proj, view, clip );
		VectorSet4( params, nearZ, farZ, r_ulightShadowBias->value, 1.5f / shadowSize );
		return;
	}

	if ( !l->parms.parallel ) {
		// projected light: clip = (2S - Q, 2T - Q, a Q + b, Q); Q is the
		// distance along the light's normal
		vec4_t rx, ry, rz, rw;
		float farZ = 1, a, b;
		int i;

		for ( i = 0; i < l->numFrustumVerts; i++ ) {
			const float *v = l->frustumVerts[i];

			farZ = MAX( farZ, l->lightProject[2][0] * v[0] + l->lightProject[2][1] * v[1] + l->lightProject[2][2] * v[2] + l->lightProject[2][3] );
		}
		farZ *= 1.01f;
		a = ( farZ + nearZ ) / ( farZ - nearZ );
		b = -2.0f * farZ * nearZ / ( farZ - nearZ );
		for ( i = 0; i < 4; i++ ) {
			rx[i] = 2 * l->lightProject[0][i] - l->lightProject[2][i];
			ry[i] = 2 * l->lightProject[1][i] - l->lightProject[2][i];
			rz[i] = a * l->lightProject[2][i];
			rw[i] = l->lightProject[2][i];
		}
		rz[3] += b;
		MatFromRows( rx, ry, rz, rw, clip );
		// lookup: (S, T, Q, Q); the shader divides S and T by Q and turns Q
		// into the same depth as the projection above
		for ( i = 0; i < 4; i++ ) {
			rx[i] = l->lightProject[0][i];
			ry[i] = l->lightProject[1][i];
			rz[i] = l->lightProject[2][i];
		}
		MatFromRows( rx, ry, rz, rw, lookup );
		VectorSet4( params, nearZ, farZ, r_ulightShadowBias->value, 1.5f / shadowSize );
		return;
	}

	// parallel light: orthographic along the light direction over the volume
	{
		vec3_t dir, side, up;
		vec4_t rx, ry, rz, rw;
		float mins[3] = { 1e9f, 1e9f, 1e9f }, maxs[3] = { -1e9f, -1e9f, -1e9f };
		int i;

		VectorSubtract( l->parms.origin, l->globalLightOrigin, dir );
		VectorNormalize( dir );
		PerpendicularVector( side, dir );
		CrossProduct( dir, side, up );
		for ( i = 0; i < l->numFrustumVerts; i++ ) {
			float p[3];
			int k;

			p[0] = DotProduct( l->frustumVerts[i], side );
			p[1] = DotProduct( l->frustumVerts[i], up );
			p[2] = DotProduct( l->frustumVerts[i], dir );
			for ( k = 0; k < 3; k++ ) {
				mins[k] = MIN( mins[k], p[k] );
				maxs[k] = MAX( maxs[k], p[k] );
			}
		}
		mins[2] -= 4096;    // casters above the volume
		VectorCopy( side, rx );
		rx[3] = 0;
		VectorCopy( up, ry );
		ry[3] = 0;
		VectorCopy( dir, rz );
		rz[3] = 0;
		VectorSet4( rw, 0, 0, 0, 1 );
		for ( i = 0; i < 3; i++ ) {
			float *row = i == 0 ? rx : i == 1 ? ry : rz;
			float s = 2.0f / ( maxs[i] - mins[i] );

			row[0] *= s; row[1] *= s; row[2] *= s;
			row[3] = -( maxs[i] + mins[i] ) / ( maxs[i] - mins[i] );
		}
		MatFromRows( rx, ry, rz, rw, clip );
		// lookup: clip * 0.5 + 0.5
		for ( i = 0; i < 4; i++ ) {
			rx[i] = 0.5f * rx[i];
			ry[i] = 0.5f * ry[i];
			rz[i] = 0.5f * rz[i];
		}
		rx[3] += 0.5f;
		ry[3] += 0.5f;
		rz[3] += 0.5f;
		MatFromRows( rx, ry, rz, rw, lookup );
		VectorSet4( params, 0, 1, 2.0f * r_ulightShadowBias->value, 1.0f );
	}
}

static const float identityMatrix[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

/*
=================
RenderShadowFace

Draws a caster list into one face of a shadow image.
=================
*/
static void RenderShadowFace( image_t *img, int face, const float *clip, uLight_t *l, drawSurf_t *surfs, int numSurfs, qboolean clear ) {
	viewParms_t saved = backEnd.viewParms;
	orientationr_t savedOr = backEnd.or;

	FBO_AttachImage( shadowFbo, img, GL_DEPTH_ATTACHMENT, face );
	FBO_Bind( shadowFbo );
	qglViewport( 0, 0, shadowSize, shadowSize );
	qglScissor( 0, 0, shadowSize, shadowSize );
	if ( clear ) {
		GL_State( GLS_DEPTHMASK_TRUE );
		qglClear( GL_DEPTH_BUFFER_BIT );
	}
	ulw.statShadowPasses++;
	if ( !numSurfs ) {
		return;
	}

	// the shadow view: world-to-clip in the projection, identity world
	Com_Memcpy( backEnd.viewParms.world.modelMatrix, identityMatrix, sizeof( identityMatrix ) );
	VectorCopy( l->globalLightOrigin, backEnd.viewParms.or.origin );
	backEnd.viewParms.isMirror = qfalse;
	backEnd.viewParms.isPortal = qfalse;
	GL_SetProjectionMatrix( (float *)clip );

	qglColorMask( GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE );
	qglEnable( GL_POLYGON_OFFSET_FILL );
	qglPolygonOffset( 1.5f, 2.0f );
	ulb.mode = ULB_DEPTH;
	RB_RenderDrawSurfList( surfs, numSurfs );
	ulb.mode = ULB_NONE;
	qglPolygonOffset( r_offsetFactor->value, r_offsetUnits->value );
	qglDisable( GL_POLYGON_OFFSET_FILL );
	qglColorMask( !backEnd.colorMask[0], !backEnd.colorMask[1], !backEnd.colorMask[2], !backEnd.colorMask[3] );

	backEnd.viewParms = saved;
	backEnd.or = savedOr;
}

static void CopyShadowFace( image_t *src, image_t *dst, int face ) {
	FBO_AttachImage( shadowReadFbo, src, GL_DEPTH_ATTACHMENT, face );
	FBO_AttachImage( shadowFbo, dst, GL_DEPTH_ATTACHMENT, face );
	FBO_FastBlit( shadowReadFbo, NULL, shadowFbo, NULL, GL_DEPTH_BUFFER_BIT, GL_NEAREST );
}

/*
=================
RB_ULightShadowMaps

Makes the light's shadow map current; returns the shadow type.
=================
*/
static int RB_ULightShadowMaps( uViewLight_t *vl, image_t **outImage ) {
	uLight_t *l = vl->light;
	qboolean cube = l->parms.pointLight && !l->parms.parallel;
	int faces = cube ? 6 : 1, face;
	drawSurf_t *casters = R_ULightSurfList( vl->firstCaster );
	int numStatic = vl->shadowSize, numDynamic = vl->numCaster - vl->shadowSize;
	shadowSlot_t *slot = NULL;
	qboolean valid = qfalse;
	image_t *target;
	float clip[16];

	if ( !ShadowResources() ) {
		return USHADOW_NONE;
	}
	if ( !l->dynamic ) {
		slot = FindSlot( cube ? cubeSlots : flatSlots, l, cube, &valid );
	}

	if ( slot ) {
		// the static world layer
		if ( !valid ) {
			for ( face = 0; face < faces; face++ ) {
				ShadowView( l, face, clip, ulb.shadowMatrix, ulb.shadowParams );
				RenderShadowFace( slot->image, face, clip, l, casters, numStatic, qtrue );
			}
			slot->stamp = l->derivedCount;
		} else {
			ulw.statShadowCacheHits++;
		}
		target = slot->image;
		if ( numDynamic > 0 ) {
			// the dynamic overlay: static faces copied, entities drawn over
			target = cube ? scratchCube : scratchFlat;
			for ( face = 0; face < faces; face++ ) {
				ShadowView( l, face, clip, ulb.shadowMatrix, ulb.shadowParams );
				CopyShadowFace( slot->image, target, face );
				RenderShadowFace( target, face, clip, l, casters + numStatic, numDynamic, qfalse );
			}
		}
	} else {
		target = cube ? scratchCube : scratchFlat;
		for ( face = 0; face < faces; face++ ) {
			ShadowView( l, face, clip, ulb.shadowMatrix, ulb.shadowParams );
			RenderShadowFace( target, face, clip, l, casters, vl->numCaster, qtrue );
		}
	}
	// lookup parameters (any face; the cube lookup needs only near/far)
	ShadowView( l, 0, clip, ulb.shadowMatrix, ulb.shadowParams );
	*outImage = target;
	return cube ? USHADOW_CUBE : USHADOW_2D;
}

/*
=====================================================================

DRAWING

=====================================================================
*/

static void SetDiffuseColor( uProgram_t *p, shaderStage_t *pStage ) {
	vec4_t c;

	VectorSet4( c, 1, 1, 1, 1 );
	switch ( pStage->rgbGen ) {
	case CGEN_CONST:
		c[0] = pStage->constantColor[0] / 255.0f;
		c[1] = pStage->constantColor[1] / 255.0f;
		c[2] = pStage->constantColor[2] / 255.0f;
		break;
	case CGEN_ENTITY:
		if ( backEnd.currentEntity ) {
			c[0] = backEnd.currentEntity->e.shaderRGBA[0] / 255.0f;
			c[1] = backEnd.currentEntity->e.shaderRGBA[1] / 255.0f;
			c[2] = backEnd.currentEntity->e.shaderRGBA[2] / 255.0f;
		}
		break;
	default:
		break;
	}
	if ( pStage->oaxTinted ) {	// oaxTint (docs/materials.md)
		c[0] *= pStage->oaxTint[0]; c[1] *= pStage->oaxTint[1]; c[2] *= pStage->oaxTint[2];
	}
	if ( p->diffuseColor >= 0 ) {
		qglProgramUniform4fEXT( p->sp.program, p->diffuseColor, c[0], c[1], c[2], c[3] );
	}
}

static void SetCommonUniforms( uProgram_t *p, shaderCommands_t *input ) {
	int deformGen;
	vec5_t deformParams;

	ComputeDeformValues( &deformGen, deformParams );
	GLSL_SetUniformMat4( &p->sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection );
	// the world's orientation carries no transform matrix
	GLSL_SetUniformMat4( &p->sp, UNIFORM_MODELMATRIX, backEnd.currentEntity == &tr.worldEntity ? (float *)identityMatrix : backEnd.or.transformMatrix );
	GLSL_SetUniformFloat( &p->sp, UNIFORM_VERTEXLERP, glState.vertexAttribsInterpolation );
	if ( glState.boneAnimation ) {
		GLSL_SetUniformMat4BoneMatrix( &p->sp, UNIFORM_BONEMATRIX, glState.boneMatrix, glState.boneAnimation );
	}
	GLSL_SetUniformInt( &p->sp, UNIFORM_DEFORMGEN, deformGen );
	if ( deformGen != DGEN_NONE ) {
		GLSL_SetUniformFloat5( &p->sp, UNIFORM_DEFORMPARAMS, deformParams );
		GLSL_SetUniformFloat( &p->sp, UNIFORM_TIME, tess.shaderTime );
	}
}

static uProgram_t *PickProgram( int mode, int shadow ) {
	int anim = UANIM_NONE;
	uProgram_t *p;

	if ( glState.vertexAnimation ) {
		anim = UANIM_VERTEX;
	} else if ( glState.boneAnimation ) {
		anim = UANIM_BONE;
	}
	p = &uprogs[mode][anim][shadow];
	return p->valid ? p : NULL;
}

/*
=================
RB_ULightStageIterator

Called from RB_StageIteratorGeneric while a light pass is drawing.
=================
*/
qboolean RB_ULightStageIterator( shaderCommands_t *input ) {
	shaderStage_t *pStage;
	uProgram_t *p;
	int stageNum;

	if ( ulb.mode == ULB_NONE ) {
		return qfalse;
	}

	if ( ulb.mode == ULB_DEPTH ) {
		if ( input->shader->sort > SS_OPAQUE ) {
			return qtrue;
		}
		p = PickProgram( UPROG_DEPTH, USHADOW_NONE );
		if ( !p ) {
			return qtrue;
		}
		GLSL_BindProgram( &p->sp );
		SetCommonUniforms( p, input );
		GL_Cull( CT_TWO_SIDED );
		GL_State( GLS_DEPTHMASK_TRUE );
		R_DrawElements( input->numIndexes, input->firstIndex );
		return qtrue;
	}

	stageNum = R_ULightInteractionStage( input->shader );
	if ( stageNum < 0 || !( pStage = input->xstages[stageNum] ) ) {
		return qtrue;
	}

	if ( ulb.mode == ULB_REFLECT ) {
		// oaxMetal: the nearest probe's reflection, added (docs/materials.md)
		cubemap_t *cm;
		vec4_t v;
		int mips, size;

		if ( !input->shader->oaxMetal || !input->cubemapIndex || input->cubemapIndex > tr.numCubemaps ) {
			return qtrue;
		}
		cm = &tr.cubemaps[input->cubemapIndex - 1];
		if ( !cm->image || !( p = PickProgram( UPROG_REFLECT, USHADOW_NONE ) ) ) {
			return qtrue;
		}
		GLSL_BindProgram( &p->sp );
		SetCommonUniforms( p, input );
		{
			vec4_t texMatrix[8];

			ComputeTexMods( pStage, TB_DIFFUSEMAP, texMatrix );
			GLSL_SetUniformVec4( &p->sp, UNIFORM_DIFFUSETEXMATRIX0, texMatrix[0] );
			GLSL_SetUniformVec4( &p->sp, UNIFORM_DIFFUSETEXMATRIX1, texMatrix[1] );
			GLSL_SetUniformVec4( &p->sp, UNIFORM_DIFFUSETEXMATRIX2, texMatrix[2] );
			GLSL_SetUniformVec4( &p->sp, UNIFORM_DIFFUSETEXMATRIX3, texMatrix[3] );
			GLSL_SetUniformVec4( &p->sp, UNIFORM_DIFFUSETEXMATRIX4, texMatrix[4] );
			GLSL_SetUniformVec4( &p->sp, UNIFORM_DIFFUSETEXMATRIX5, texMatrix[5] );
			GLSL_SetUniformVec4( &p->sp, UNIFORM_DIFFUSETEXMATRIX6, texMatrix[6] );
			GLSL_SetUniformVec4( &p->sp, UNIFORM_DIFFUSETEXMATRIX7, texMatrix[7] );
		}
		GLSL_SetUniformVec3( &p->sp, UNIFORM_VIEWORIGIN, backEnd.viewParms.or.origin );
		GLSL_SetUniformVec4( &p->sp, UNIFORM_NORMALSCALE, pStage->bundle[TB_NORMALMAP].image[0] ? pStage->normalScale : colorWhite );
		// the stock renderer's parallax term (tr_shade.c)
		VectorSubtract( cm->origin, backEnd.viewParms.or.origin, v );
		v[3] = 1.0f;
		VectorScale4( v, 1.0f / cm->parallaxRadius, v );
		GLSL_SetUniformVec4( &p->sp, UNIFORM_CUBEMAPINFO, v );
		if ( p->metalParms >= 0 ) {
			const float *m = input->shader->oaxMetalParms;

			qglProgramUniform4fEXT( p->sp.program, p->metalParms, m[0], m[1], m[2], m[3] );
		}
		// the blurriest mip roughness 1 reads: two above the 1x1 one, as the
		// stock renderer's ROUGHNESS_MIPS
		for ( mips = 0, size = cm->image->width; size; size >>= 1 ) {
			mips++;
		}
		if ( p->reflectLod >= 0 ) {
			qglProgramUniform1fEXT( p->sp.program, p->reflectLod, (float)MAX( 1, mips - 2 ) );
		}
		// on a lightmapped map a model's highlight comes from its light grid
		// light (in the frame's units: light 1 is identityLight)
		if ( p->gridLightDir >= 0 ) {
			trRefEntity_t *e = backEnd.currentEntity;
			qboolean grid = ulb.view->lightingModel == ULIGHT_LIGHTMAP && e && e != &tr.worldEntity;

			qglProgramUniform4fEXT( p->sp.program, p->gridLightDir, grid ? e->lightDir[0] : 0, grid ? e->lightDir[1] : 0, grid ? e->lightDir[2] : 1, grid ? 1.0f : 0.0f );
			if ( grid && p->gridLight >= 0 ) {
				float s = tr.identityLight / 255.0f;

				qglProgramUniform3fEXT( p->sp.program, p->gridLight, e->directedLight[0] * s, e->directedLight[1] * s, e->directedLight[2] * s );
			}
		}
		R_BindAnimatedImageToTMU( &pStage->bundle[TB_DIFFUSEMAP], TB_DIFFUSEMAP );
		if ( pStage->bundle[TB_NORMALMAP].image[0] ) {
			R_BindAnimatedImageToTMU( &pStage->bundle[TB_NORMALMAP], TB_NORMALMAP );
		} else {
			GL_BindToTMU( ulightImages_flat, TB_NORMALMAP );
		}
		GL_BindToTMU( cm->image, TB_CUBEMAP );
		GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL );
		R_DrawElements( input->numIndexes, input->firstIndex );
		ulw.statDraws++;
		oaxFxStats.metalDraws++;
		return qtrue;
	}

	if ( ulb.mode == ULB_AMBIENT ) {
		// step 7.5 B: zone ambient: world vertices carry it, an entity takes
		// the zone at its origin
		qboolean zoneWorld = ulw.zoneAmbient && backEnd.currentEntity == &tr.worldEntity && !glState.vertexAnimation && !glState.boneAnimation;
		vec3_t ambient;

		VectorCopy( ulb.view->ambient, ambient );
		if ( zoneWorld ) {
			float s = tr.identityLight > 0 ? 1.0f / tr.identityLight : 1.0f;

			VectorSet( ambient, s, s, s );
		} else if ( ulw.zoneAmbient && backEnd.currentEntity && backEnd.currentEntity != &tr.worldEntity ) {
			R_ULightZoneAmbientAt( ( backEnd.currentEntity->e.renderfx & RF_LIGHTING_ORIGIN ) ? backEnd.currentEntity->e.lightingOrigin : backEnd.currentEntity->e.origin, ambient );
		}
		p = PickProgram( zoneWorld ? UPROG_AMBIENTZONE : UPROG_AMBIENT, USHADOW_NONE );
		if ( !p ) {
			return qtrue;
		}
		GLSL_BindProgram( &p->sp );
		SetCommonUniforms( p, input );
		{
			vec4_t texMatrix[8];

			ComputeTexMods( pStage, TB_DIFFUSEMAP, texMatrix );
			GLSL_SetUniformVec4( &p->sp, UNIFORM_DIFFUSETEXMATRIX0, texMatrix[0] );
			GLSL_SetUniformVec4( &p->sp, UNIFORM_DIFFUSETEXMATRIX1, texMatrix[1] );
			GLSL_SetUniformVec4( &p->sp, UNIFORM_DIFFUSETEXMATRIX2, texMatrix[2] );
			GLSL_SetUniformVec4( &p->sp, UNIFORM_DIFFUSETEXMATRIX3, texMatrix[3] );
			GLSL_SetUniformVec4( &p->sp, UNIFORM_DIFFUSETEXMATRIX4, texMatrix[4] );
			GLSL_SetUniformVec4( &p->sp, UNIFORM_DIFFUSETEXMATRIX5, texMatrix[5] );
			GLSL_SetUniformVec4( &p->sp, UNIFORM_DIFFUSETEXMATRIX6, texMatrix[6] );
			GLSL_SetUniformVec4( &p->sp, UNIFORM_DIFFUSETEXMATRIX7, texMatrix[7] );
		}
		SetDiffuseColor( p, pStage );
		// the frame convention: light 1 is identityLight in the render target,
		// which the final exposure (2^r_cameraExposure, 2 by default, the
		// stock overbright compensation) brings back to the texture at 1x
		VectorScale( ambient, tr.identityLight, ambient );
		GLSL_SetUniformVec3( &p->sp, UNIFORM_AMBIENTLIGHT, ambient );
		R_BindAnimatedImageToTMU( &pStage->bundle[TB_DIFFUSEMAP], TB_DIFFUSEMAP );
		GL_State( GLS_DEPTHFUNC_EQUAL );
		R_DrawElements( input->numIndexes, input->firstIndex );
		ulw.statDraws++;
		return qtrue;
	}

	// interaction
	{
		uViewLight_t *vl = ulb.vl;
		uLight_t *l = vl->light;
		GLuint prog;
		vec4_t texMatrix[8];
		qboolean hasSpec = pStage->bundle[TB_SPECULARMAP].image[0] != NULL;
		float specScale = r_ulightSpecular->integer && !l->parms.noSpecular ? 1.0f : 0.0f;

		p = PickProgram( l->parms.phys.physical ? UPROG_PHYSICAL : UPROG_INTERACTION, ulb.shadowType == USHADOW_CUBE ? USHADOW_CUBE : ulb.shadowType == USHADOW_2D ? USHADOW_2D : USHADOW_NONE );
		if ( !p ) {
			return qtrue;
		}
		prog = p->sp.program;
		GLSL_BindProgram( &p->sp );
		SetCommonUniforms( p, input );
		ComputeTexMods( pStage, TB_DIFFUSEMAP, texMatrix );
		GLSL_SetUniformVec4( &p->sp, UNIFORM_DIFFUSETEXMATRIX0, texMatrix[0] );
		GLSL_SetUniformVec4( &p->sp, UNIFORM_DIFFUSETEXMATRIX1, texMatrix[1] );
		GLSL_SetUniformVec4( &p->sp, UNIFORM_DIFFUSETEXMATRIX2, texMatrix[2] );
		GLSL_SetUniformVec4( &p->sp, UNIFORM_DIFFUSETEXMATRIX3, texMatrix[3] );
		GLSL_SetUniformVec4( &p->sp, UNIFORM_DIFFUSETEXMATRIX4, texMatrix[4] );
		GLSL_SetUniformVec4( &p->sp, UNIFORM_DIFFUSETEXMATRIX5, texMatrix[5] );
		GLSL_SetUniformVec4( &p->sp, UNIFORM_DIFFUSETEXMATRIX6, texMatrix[6] );
		GLSL_SetUniformVec4( &p->sp, UNIFORM_DIFFUSETEXMATRIX7, texMatrix[7] );
		GLSL_SetUniformVec3( &p->sp, UNIFORM_VIEWORIGIN, backEnd.viewParms.or.origin );
		GLSL_SetUniformVec4( &p->sp, UNIFORM_NORMALSCALE, pStage->bundle[TB_NORMALMAP].image[0] ? pStage->normalScale : colorWhite );
		SetDiffuseColor( p, pStage );

		qglProgramUniform4fEXT( prog, p->lightProjS, l->lightProject[0][0], l->lightProject[0][1], l->lightProject[0][2], l->lightProject[0][3] );
		qglProgramUniform4fEXT( prog, p->lightProjT, l->lightProject[1][0], l->lightProject[1][1], l->lightProject[1][2], l->lightProject[1][3] );
		qglProgramUniform4fEXT( prog, p->lightProjQ, l->lightProject[2][0], l->lightProject[2][1], l->lightProject[2][2], l->lightProject[2][3] );
		qglProgramUniform4fEXT( prog, p->lightFalloffS, l->lightProject[3][0], l->lightProject[3][1], l->lightProject[3][2], l->lightProject[3][3] );
		qglProgramUniform3fEXT( prog, p->lightOriginW, l->globalLightOrigin[0], l->globalLightOrigin[1], l->globalLightOrigin[2] );
		// light 1 is identityLight in the render target (see the ambient pass)
		qglProgramUniform3fEXT( prog, p->lightColor, vl->color[0] * tr.identityLight, vl->color[1] * tr.identityLight, vl->color[2] * tr.identityLight );
		if ( hasSpec ) {
			qglProgramUniform4fEXT( prog, p->specularColor, specScale, specScale, specScale, 1 );
		} else {
			qglProgramUniform4fEXT( prog, p->specularColor, 0.2f * specScale, 0.2f * specScale, 0.2f * specScale, 1 );
		}
		if ( p->shadowParams >= 0 ) {
			qglProgramUniform4fEXT( prog, p->shadowParams, ulb.shadowParams[0], ulb.shadowParams[1], ulb.shadowParams[2], ulb.shadowParams[3] );
		}
		if ( p->shadowMatrix >= 0 ) {
			qglProgramUniformMatrix4fvEXT( prog, p->shadowMatrix, 1, GL_FALSE, ulb.shadowMatrix );
		}

		R_BindAnimatedImageToTMU( &pStage->bundle[TB_DIFFUSEMAP], TB_DIFFUSEMAP );
		if ( pStage->bundle[TB_NORMALMAP].image[0] ) {
			R_BindAnimatedImageToTMU( &pStage->bundle[TB_NORMALMAP], TB_NORMALMAP );
		} else {
			GL_BindToTMU( ulightImages_flat, TB_NORMALMAP );
		}
		if ( hasSpec ) {
			R_BindAnimatedImageToTMU( &pStage->bundle[TB_SPECULARMAP], TB_SPECULARMAP );
		} else {
			GL_BindToTMU( ulightImages_white, TB_SPECULARMAP );
		}
		GL_BindToTMU( l->projImage, UTMU_PROJ );
		GL_BindToTMU( l->falloffImage, UTMU_FALLOFF );
		if ( l->parms.phys.physical ) {
			const uLightPhys_t *ph = &l->parms.phys;
			int k;

			qglProgramUniform4fEXT( prog, p->physLight, 1.0f / ph->radius, ph->intensity, ph->cap, ph->capKnee );
			qglProgramUniform4fEXT( prog, p->physLight2, (float)ph->falloffMode, ph->invsqMin, ph->lambert ? 1.0f : 0.0f, ulw.overbright * tr.identityLight );
			qglProgramUniform4fEXT( prog, p->physSpot, ph->spotDir[0], ph->spotDir[1], ph->spotDir[2], ph->spotScale );
			for ( k = 0; k < ph->numPoints; k++ ) {
				if ( p->physCurve[k] >= 0 ) {
					qglProgramUniform2fEXT( prog, p->physCurve[k], ph->points[k][0], ph->points[k][1] );
				}
			}
			if ( p->physCurveCount >= 0 ) {
				qglProgramUniform1iEXT( prog, p->physCurveCount, ph->numPoints );
			}
			if ( ph->falloffMode == ULF_IMAGE && ph->falloffImage ) {
				GL_BindToTMU( ph->falloffImage, UTMU_FALLOFF );
			}
		}

		GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL );
		R_DrawElements( input->numIndexes, input->firstIndex );
		ulw.statDraws++;
	}
	return qtrue;
}

/*
=================
RB_ULightSkipStage

In unified views the stock pass leaves out the interaction stage of opaque
surfaces: the ambient and light passes drew it.
=================
*/
qboolean RB_ULightSkipStage( shaderCommands_t *input, int stage ) {
	if ( !ulb.skipLitStages || input->shader->sort > SS_OPAQUE ) {
		return qfalse;
	}
	if ( backEnd.currentEntity && ( backEnd.currentEntity->e.renderfx & RF_DEPTHHACK ) ) {
		return qfalse;
	}
	return stage == R_ULightInteractionStage( input->shader );
}

static void RestoreView( void ) {
	qglViewport( backEnd.viewParms.viewportX, backEnd.viewParms.viewportY, backEnd.viewParms.viewportWidth, backEnd.viewParms.viewportHeight );
	qglScissor( backEnd.viewParms.viewportX, backEnd.viewParms.viewportY, backEnd.viewParms.viewportWidth, backEnd.viewParms.viewportHeight );
	GL_SetProjectionMatrix( backEnd.viewParms.projectionMatrix );
	GL_SetModelviewMatrix( backEnd.viewParms.world.modelMatrix );
}

/*
=================
RB_DrawULights

The light-major pass of a view.
=================
*/
void RB_DrawULights( void ) {
	uView_t *view = R_ULightGetView( backEnd.viewParms.ulightView );
	FBO_t *viewFbo = glState.currentFBO;
	int i;

	if ( !view ) {
		return;
	}
	ulb.view = view;

	// ambient
	if ( view->numAmbient ) {
		ulb.mode = ULB_AMBIENT;
		RB_RenderDrawSurfList( R_ULightSurfList( view->firstAmbient ), view->numAmbient );
	}

	for ( i = 0; i < view->numLights; i++ ) {
		uViewLight_t *vl = &view->lights[i];
		image_t *shadowImage = NULL;

		ulb.shadowType = USHADOW_NONE;
		if ( vl->shadows ) {
			if ( view->shadowMode == ULIGHT_SHADOW_STENCIL ) {
				ulb.mode = ULB_NONE;
				FBO_Bind( viewFbo );
				RestoreView();
				qglScissor( vl->scissor[0], vl->scissor[1], vl->scissor[2], vl->scissor[3] );
				RB_ULightStencilShadows( vl );
			} else {
				ulb.shadowType = RB_ULightShadowMaps( vl, &shadowImage );
				FBO_Bind( viewFbo );
				RestoreView();
			}
		}
		if ( shadowImage ) {
			GL_BindToTMU( shadowImage, ulb.shadowType == USHADOW_CUBE ? UTMU_SHADOWCUBE : UTMU_SHADOW2D );
		}

		qglScissor( vl->scissor[0], vl->scissor[1], vl->scissor[2], vl->scissor[3] );
		ulb.mode = ULB_INTERACTION;
		ulb.vl = vl;
		RB_RenderDrawSurfList( R_ULightSurfList( vl->firstLit ), vl->numLit );
		ulb.mode = ULB_NONE;
		if ( vl->shadows && view->shadowMode == ULIGHT_SHADOW_STENCIL ) {
			qglDisable( GL_STENCIL_TEST );
		}
	}

	ulb.mode = ULB_NONE;
	ulb.vl = NULL;
	FBO_Bind( viewFbo );
	RestoreView();

	// oaxMetal: the probe reflections (a probe's capture view draws none:
	// it is VPF_NOCUBEMAPS)
	if ( view->numMetal && !( backEnd.viewParms.flags & VPF_NOCUBEMAPS ) ) {
		ulb.mode = ULB_REFLECT;
		RB_RenderDrawSurfList( R_ULightSurfList( view->firstMetal ), view->numMetal );
		ulb.mode = ULB_NONE;
	}
}

/*
=================
RB_ULightDrawViewSurfs

Replaces the stock RB_RenderDrawSurfList call for a view with unified
lighting: the light pass goes between the opaque and translucent surfaces.
=================
*/
void RB_ULightDrawViewSurfs( drawSurf_t *drawSurfs, int numDrawSurfs ) {
	uView_t *view = R_ULightGetView( backEnd.viewParms.ulightView );
	int split;

	if ( !view ) {
		RB_RenderDrawSurfList( drawSurfs, numDrawSurfs );
		return;
	}

	for ( split = 0; split < numDrawSurfs; split++ ) {
		int entityNum, fogNum, dlighted, pshadowed;
		shader_t *sh;

		R_DecomposeSort( drawSurfs[split].sort, &entityNum, &sh, &fogNum, &dlighted, &pshadowed );
		if ( sh->sort > SS_OPAQUE ) {
			break;
		}
	}

	if ( view->lightingModel == ULIGHT_UNIFIED ) {
		RB_DrawULights();
		ulb.skipLitStages = qtrue;
		RB_RenderDrawSurfList( drawSurfs, split );
		ulb.skipLitStages = qfalse;
	} else {
		RB_RenderDrawSurfList( drawSurfs, split );
		RB_DrawULights();
	}
	if ( split < numDrawSurfs ) {
		RB_RenderDrawSurfList( drawSurfs + split, numDrawSurfs - split );
	}
}
