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
tr_oax_fx.c: renderergl2 effects, phase 6: the shared parts.

- Cvars for every effect (r_oaxParticles, r_oaxSoftParticles, r_oaxDecals,
  r_oaxTrails, r_oaxWater, r_oaxBloom*).
- The per-frame scene lists of particle systems (RE_OAXAddFx): like
  polys, the cgame adds them every frame they should draw; each scene
  takes the ones added since the previous scene.
- The scene copy: the opaque scene's colour and depth copied once per
  view, the first time a soft particle or a water surface needs them, so
  those passes never read a buffer they draw into.
- GLSL programs and the named debug values (counts) of the effects.

Every effect is opt-in: particles, decals and trails only exist when the
cgame adds them, water only on shaders with the oaxWater keyword, and
bloom only with r_oaxBloom 1. A stock map renders exactly as before.
===========================================================================
*/

#include "tr_local.h"

cvar_t	*r_oaxParticles;
cvar_t	*r_oaxSoftParticles;
cvar_t	*r_oaxDecals;
cvar_t	*r_oaxTrails;
cvar_t	*r_oaxWater;
cvar_t	*r_oaxBloom;

oaxFxStats_t oaxFxStats;

static oaxSceneFx_t			sceneFx[OAX_MAX_SCENE_FX];
static int					numSceneFx, firstSceneFx;
static srfOaxParticles_t	prtSurfs[OAX_MAX_PRT_SURFS];
static int					numPrtSurfs;

// the clocks of the last world scene, for RE_OAXAddFx's answer
static int					lastRefdefMs, lastShaderMs;

// scene copy
static FBO_t	*copyFbo;
static image_t	*copyColor, *copyDepth;
static qboolean	copyValid;

extern const char *fallbackShader_oaxparticle_vp;
extern const char *fallbackShader_oaxparticle_fp;
extern const char *fallbackShader_oaxwater_vp;
extern const char *fallbackShader_oaxwater_fp;
extern const char *fallbackShader_oaxbloom_vp;
extern const char *fallbackShader_oaxbloom_fp;

void R_OAXFxRegisterCvars( void ) {
	r_oaxParticles = ri.Cvar_Get( "r_oaxParticles", "1", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_oaxParticles, "Draw oax particle systems (particles/*.prt) the cgame adds." );
	r_oaxSoftParticles = ri.Cvar_Get( "r_oaxSoftParticles", "12", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_oaxSoftParticles, "Soft particles: fade particles over this many units in front of the scene behind them (0 = off)." );
	r_oaxDecals = ri.Cvar_Get( "r_oaxDecals", "1", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_oaxDecals, "Projected decals from the cgame (0 = off; the cgame then makes stock marks)." );
	r_oaxTrails = ri.Cvar_Get( "r_oaxTrails", "1", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_oaxTrails, "Draw ribbon trails the cgame adds." );
	r_oaxWater = ri.Cvar_Get( "r_oaxWater", "1", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_oaxWater, "oaxWater shaders: 1 reflection and refraction, 2 refraction only, 0 the shader's own stages." );
	r_oaxBloom = ri.Cvar_Get( "r_oaxBloom", "0", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_oaxBloom, "Bloom in the HDR post-process chain (r_oaxBloomThreshold, r_oaxBloomKnee, r_oaxBloomIntensity, r_oaxBloomLevels)." );
}

/*
=================
CreateSpriteImages

Built-in sprite textures for effect shaders (the shaders the oax cgame
ships use them, so no art is needed):
  *oaxsoft    white, alpha a soft round falloff (alpha-blended smoke)
  *oaxglow    the falloff in colour and alpha (additive glows)
  *oaxspark   a thin streak along t (aimed sparks)
  *oaxribbon  a falloff across s, constant along t (ribbon trails)
  *oaxring    white, alpha a soft thin ring (water ripples)
  *oaxtread   white, alpha chevron tread bars across a soft-edged band
              along t (tyre tracks)
=================
*/
static float Falloff( float r ) {
	float f = 1.0f - r * r;

	return f <= 0 ? 0 : f * f;
}

static void CreateSpriteImages( void ) {
	enum { S = 64 };
	byte *data = ri.Hunk_AllocateTempMemory( S * S * 4 );
	int x, y, k;

	static const char *names[6] = { "*oaxsoft", "*oaxglow", "*oaxspark", "*oaxribbon", "*oaxring", "*oaxtread" };

	for ( k = 0; k < 6; k++ ) {
		for ( y = 0; y < S; y++ ) {
			for ( x = 0; x < S; x++ ) {
				float u = ( x + 0.5f ) / S * 2.0f - 1.0f, v = ( y + 0.5f ) / S * 2.0f - 1.0f;
				float a;
				byte *p = data + ( y * S + x ) * 4;

				switch ( k ) {
				case 0:
				case 1:
					a = Falloff( sqrt( u * u + v * v ) );
					break;
				case 2:
					a = Falloff( fabs( u ) * 3.0f ) * Falloff( fabs( v ) );
					break;
				case 4:
					a = Falloff( ( sqrt( u * u + v * v ) - 0.8f ) * 6.0f );
					break;
				case 5: {
					float bar = v * 4.0f + fabs( u ) * 0.8f;

					bar -= floor( bar );
					a = ( bar < 0.55f ? 1.0f : 0.35f ) * Falloff( fabs( u ) * 1.05f );
					break;
				}
				default:
					a = Falloff( fabs( u ) );
					break;
				}
				p[3] = (byte)( a * 255.0f + 0.5f );
				p[0] = p[1] = p[2] = ( k == 0 || k >= 4 ) ? 255 : p[3];
			}
		}
		R_CreateImage( names[k], data, S, S,
			IMGTYPE_COLORALPHA, IMGFLAG_MIPMAP | IMGFLAG_CLAMPTOEDGE | IMGFLAG_NO_COMPRESSION | IMGFLAG_NOLIGHTSCALE, GL_RGBA8 );
	}
	ri.Hunk_FreeTempMemory( data );
}

/*
=================
R_OAXFxInit

From R_Init, after the images and VAOs, before the shaders: sprite
images, the static particle index buffer, the particle decls, decal
state.
=================
*/
void R_OAXFxInit( void ) {
	memset( &oaxFxStats, 0, sizeof( oaxFxStats ) );
	numSceneFx = firstSceneFx = numPrtSurfs = 0;
	lastRefdefMs = lastShaderMs = 0;
	copyFbo = NULL;
	copyColor = copyDepth = NULL;
	copyValid = qfalse;
	CreateSpriteImages();
	R_OAXPrtInitVao();
	R_OAXPrtLoadDecls();
	R_OAXDecalsWorldLoaded();
	R_OAXTrailsReset();
}

/*
=================
R_OAXFxWorldLoaded

A new world map: no decals from the last one.
=================
*/
void R_OAXFxWorldLoaded( void ) {
	R_OAXDecalsWorldLoaded();
}

const char *R_OAXFxFeatures( void ) {
	return "particles decals trails water bloom";
}

/*
=================
GLSL
=================
*/
void R_OAXFxInitGLSL( void ) {
	static const char *bloomDefines[4] = {
		"#define BLOOM_PREFILTER\n",
		"#define BLOOM_DOWN\n",
		"#define BLOOM_UP\n",
		"#define BLOOM_COMPOSITE\n",
	};
	int i;

	if ( GLSL_InitGPUShader( &tr.oaxParticleShader, "oaxparticle", ATTR_POSITION, qtrue, "", qtrue,
			fallbackShader_oaxparticle_vp, fallbackShader_oaxparticle_fp ) ) {
		GLSL_InitUniforms( &tr.oaxParticleShader );
		GLSL_SetUniformInt( &tr.oaxParticleShader, UNIFORM_DIFFUSEMAP, TB_COLORMAP );
		GLSL_SetUniformInt( &tr.oaxParticleShader, UNIFORM_SCREENDEPTHMAP, TB_SHADOWMAP );
		GLSL_FinishGPUShader( &tr.oaxParticleShader );
	} else {
		ri.Printf( PRINT_WARNING, "WARNING: oaxparticle shader failed; particles are off\n" );
	}

	if ( GLSL_InitGPUShader( &tr.oaxWaterShader, "oaxwater", ATTR_POSITION | ATTR_NORMAL, qtrue, "", qtrue,
			fallbackShader_oaxwater_vp, fallbackShader_oaxwater_fp ) ) {
		GLSL_InitUniforms( &tr.oaxWaterShader );
		GLSL_SetUniformInt( &tr.oaxWaterShader, UNIFORM_DIFFUSEMAP, TB_COLORMAP );
		GLSL_SetUniformInt( &tr.oaxWaterShader, UNIFORM_LIGHTMAP, TB_LIGHTMAP );
		GLSL_SetUniformInt( &tr.oaxWaterShader, UNIFORM_NORMALMAP, TB_NORMALMAP );
		GLSL_SetUniformInt( &tr.oaxWaterShader, UNIFORM_SCREENDEPTHMAP, TB_SHADOWMAP );
		GLSL_FinishGPUShader( &tr.oaxWaterShader );
	} else {
		ri.Printf( PRINT_WARNING, "WARNING: oaxwater shader failed; water draws its fallback stages\n" );
	}

	for ( i = 0; i < 4; i++ ) {
		if ( !GLSL_InitGPUShader( &tr.oaxBloomShader[i], "oaxbloom", ATTR_POSITION | ATTR_TEXCOORD, qtrue, bloomDefines[i], qtrue,
				fallbackShader_oaxbloom_vp, fallbackShader_oaxbloom_fp ) ) {
			ri.Printf( PRINT_WARNING, "WARNING: oaxbloom shader %d failed; bloom is off\n", i );
			continue;
		}
		GLSL_InitUniforms( &tr.oaxBloomShader[i] );
		GLSL_SetUniformInt( &tr.oaxBloomShader[i], UNIFORM_TEXTUREMAP, TB_COLORMAP );
		GLSL_FinishGPUShader( &tr.oaxBloomShader[i] );
	}
}

void R_OAXFxShutdownGLSL( void ) {
	int i;

	GLSL_DeleteGPUShader( &tr.oaxParticleShader );
	GLSL_DeleteGPUShader( &tr.oaxWaterShader );
	for ( i = 0; i < 4; i++ ) {
		GLSL_DeleteGPUShader( &tr.oaxBloomShader[i] );
	}
}

/*
=================
Scene lists
=================
*/
void R_OAXFxInitNextFrame( void ) {
	numSceneFx = firstSceneFx = 0;
	numPrtSurfs = 0;
	R_OAXTrailsInitNextFrame();
}

void R_OAXFxClearScene( void ) {
	firstSceneFx = numSceneFx;
	R_OAXTrailsClearScene();
}

void R_OAXFxEndScene( void ) {
	firstSceneFx = numSceneFx;
	R_OAXTrailsEndScene();
}

oaxSceneFx_t *R_OAXSceneFx( int index ) {
	if ( index < 0 || index >= numSceneFx ) {
		return NULL;
	}
	return &sceneFx[index];
}

srfOaxParticles_t *R_OAXPrtSurf( int index ) {
	return &prtSurfs[index];
}

srfOaxParticles_t *R_OAXPrtNewSurf( oaxSceneFx_t *sfx ) {
	srfOaxParticles_t *surf;

	if ( numPrtSurfs >= OAX_MAX_PRT_SURFS ) {
		return NULL;
	}
	surf = &prtSurfs[numPrtSurfs];
	surf->surfaceType = SF_OAX_PARTICLES;
	surf->fx = sfx - sceneFx;
	surf->stage = 0;
	if ( !sfx->numSurfs ) {
		sfx->firstSurf = numPrtSurfs;
	}
	sfx->numSurfs++;
	numPrtSurfs++;
	return surf;
}

/*
=================
R_OAXFxTime

A system's clock: the shader clock (frozen by r_fixedShaderTime) or the
refdef clock, in ms.
=================
*/
static int FxClock( const oaxFx_t *fx, int refdefMs, int shaderMs ) {
	return ( fx->flags & OAXFX_SHADERTIME ) ? shaderMs : refdefMs;
}

/*
=================
R_OAXFxBeginScene

From RE_BeginScene: this scene's particle systems and trails, with their
clocks and stage surfaces.
=================
*/
void R_OAXFxBeginScene( void ) {
	int i;
	int refdefMs = tr.refdef.time;
	int shaderMs = (int)floor( tr.refdef.floatTime * 1000.0 + 0.5 );

	tr.refdef.oaxFirstFx = firstSceneFx;
	tr.refdef.oaxNumFx = numSceneFx - firstSceneFx;
	R_OAXTrailsBeginScene();

	if ( tr.refdef.rdflags & ( RDF_NOWORLDMODEL | RDF_OAX_SKYPORTAL ) ) {
		tr.refdef.oaxNumFx = 0;
		tr.refdef.oaxNumTrails = 0;
		return;
	}
	lastRefdefMs = refdefMs;
	lastShaderMs = shaderMs;

	for ( i = firstSceneFx; i < numSceneFx; i++ ) {
		oaxSceneFx_t *sfx = &sceneFx[i];

		if ( sfx->firstSurf != -2 ) {
			continue;	// built for an earlier scene of this frame
		}
		sfx->timeMs = FxClock( &sfx->fx, refdefMs, shaderMs );
		R_OAXPrtBuildSurfaces( sfx );
		if ( sfx->numSurfs ) {
			oaxFxStats.fxDrawn++;
		}
	}
}

/*
=================
RE_OAXAddFx

Adds a particle system to the scene being built. The answer (alive or
done) is by the clocks of the last world scene, so a cgame can free a
finished system one frame after its last particle died.
=================
*/
int RE_OAXAddFx( const oaxFx_t *fx ) {
	oaxPrtDecl_t *decl;
	oaxSceneFx_t *sfx;
	int state;

	if ( !tr.registered || !fx ) {
		return 0;
	}
	decl = R_OAXGetPrtDecl( fx->handle );
	if ( !decl ) {
		return 0;
	}
	state = R_OAXPrtSystemState( decl, FxClock( fx, lastRefdefMs, lastShaderMs ), fx );
	if ( !state || !r_oaxParticles->integer ) {
		return state;
	}
	if ( numSceneFx >= OAX_MAX_SCENE_FX ) {
		ri.Printf( PRINT_DEVELOPER, "WARNING: RE_OAXAddFx: more than %d particle systems\n", OAX_MAX_SCENE_FX );
		return state;
	}
	sfx = &sceneFx[numSceneFx++];
	memset( sfx, 0, sizeof( *sfx ) );
	sfx->fx = *fx;
	sfx->decl = decl;
	sfx->firstSurf = -2;
	if ( VectorLength( sfx->fx.axis[0] ) < 0.001f || VectorLength( sfx->fx.axis[2] ) < 0.001f ) {
		AxisClear( sfx->fx.axis );
	}
	oaxFxStats.fxAdded++;
	return state;
}

/*
=================
R_OAXAddFxSurfaces

Per view, from R_GenerateDrawSurfs: particles, trails and world decals.
=================
*/
void R_OAXAddFxSurfaces( void ) {
	if ( tr.refdef.rdflags & ( RDF_NOWORLDMODEL | RDF_OAX_SKYPORTAL ) ) {
		return;
	}
	if ( tr.viewParms.flags & ( VPF_SHADOWMAP | VPF_DEPTHSHADOW ) ) {
		return;
	}
	tr.currentEntityNum = REFENTITYNUM_WORLD;
	tr.shiftedEntityNum = tr.currentEntityNum << QSORT_REFENTITYNUM_SHIFT;
	if ( r_oaxParticles->integer ) {
		R_OAXAddParticleSurfaces();
	}
	if ( r_oaxTrails->integer ) {
		R_OAXAddTrailSurfaces();
	}
	if ( r_oaxDecals->integer ) {
		R_OAXAddWorldDecals();
	}
}

/*
=================
Scene copy
=================
*/
void RB_OAXFxBeginView( void ) {
	copyValid = qfalse;
}

void RB_OAXSceneCopyInvalidate( void ) {
	copyValid = qfalse;
}

void RB_OAXRestoreViewTarget( void ) {
	FBO_t *fbo = backEnd.viewParms.targetFbo ? backEnd.viewParms.targetFbo : tr.renderFbo;

	FBO_Bind( fbo );
	qglViewport( backEnd.viewParms.viewportX, backEnd.viewParms.viewportY,
		backEnd.viewParms.viewportWidth, backEnd.viewParms.viewportHeight );
	qglScissor( backEnd.viewParms.viewportX, backEnd.viewParms.viewportY,
		backEnd.viewParms.viewportWidth, backEnd.viewParms.viewportHeight );
}

/*
=================
RB_OAXSceneCopy

Copies what the main render target holds so far (colour and depth,
resolving MSAA) into textures. Only for the main scene target; returns
qfalse when there is nothing to copy from.
=================
*/
qboolean RB_OAXSceneCopy( void ) {
	FBO_t *src = glState.currentFBO;

	if ( copyValid ) {
		return qtrue;
	}
	if ( !glRefConfig.framebufferObject || !glRefConfig.framebufferBlit || !tr.renderFbo || src != tr.renderFbo
		|| backEnd.viewParms.targetFbo ) {
		return qfalse;
	}
	if ( !copyFbo ) {
		int hdrFormat = ( r_hdr->integer && glRefConfig.textureFloat ) ? GL_RGBA16F_ARB : GL_RGBA8;
		int depthFormat = R_ULightDepthFormat();

		copyColor = R_CreateImage( "*oaxSceneColor", NULL, src->width, src->height, IMGTYPE_COLORALPHA,
			IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, hdrFormat );
		copyDepth = R_CreateImage( "*oaxSceneDepth", NULL, src->width, src->height, IMGTYPE_COLORALPHA,
			IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, depthFormat );
		copyFbo = FBO_Create( "_oaxSceneCopy", src->width, src->height );
		FBO_AttachImage( copyFbo, copyColor, GL_COLOR_ATTACHMENT0, 0 );
		FBO_AttachImage( copyFbo, copyDepth, depthFormat == GL_DEPTH24_STENCIL8 ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT, 0 );
		R_CheckFBO( copyFbo );
	}
	FBO_FastBlit( src, NULL, copyFbo, NULL, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST );
	RB_OAXRestoreViewTarget();
	copyValid = qtrue;
	oaxFxStats.sceneCopies++;
	return qtrue;
}

image_t *RB_OAXSceneColor( void ) {
	return copyColor;
}

image_t *RB_OAXSceneDepth( void ) {
	return copyDepth;
}

/*
=================
RB_OAXFlushTess

Effects that draw themselves: send out whatever the batch already holds
(same material), and keep the batch open.
=================
*/
void RB_OAXFlushTess( void ) {
	shader_t *sh;
	int fogNum, cubemapIndex;

	if ( !tess.numIndexes ) {
		return;
	}
	sh = tess.shader;
	fogNum = tess.fogNum;
	cubemapIndex = tess.cubemapIndex;
	RB_EndSurface();
	RB_BeginSurface( sh, fogNum, cubemapIndex );
}

/*
=================
R_OAXFxPublishStats

Named debug values, once a frame (RE_EndFrame).
=================
*/
void R_OAXFxPublishStats( void ) {
	if ( ri.DebugSet && tr.world ) {
		ri.DebugSet( "r_fx_systems", va( "%d", oaxFxStats.fxAdded ) );
		ri.DebugSet( "r_fx_drawn", va( "%d", oaxFxStats.fxDrawn ) );
		ri.DebugSet( "r_particle_stages", va( "%d", oaxFxStats.prtStagesDrawn ) );
		ri.DebugSet( "r_particles_drawn", va( "%d", oaxFxStats.particlesDrawn ) );
		ri.DebugSet( "r_decals_live", va( "%d", R_OAXDecalsLive() ) );
		ri.DebugSet( "r_decals_drawn", va( "%d", oaxFxStats.decalsDrawn ) );
		ri.DebugSet( "r_decal_polys", va( "%d", oaxFxStats.decalPolysDrawn ) );
		ri.DebugSet( "r_trails", va( "%d", oaxFxStats.trailsAdded ) );
		ri.DebugSet( "r_trails_drawn", va( "%d", oaxFxStats.trailsDrawn ) );
		ri.DebugSet( "r_trail_points", va( "%d", oaxFxStats.trailPointsDrawn ) );
		ri.DebugSet( "r_water_reflections", va( "%d", oaxFxStats.waterReflections ) );
		ri.DebugSet( "r_water_surfs", va( "%d", oaxFxStats.waterSurfsDrawn ) );
		ri.DebugSet( "r_bloom_passes", va( "%d", oaxFxStats.bloomPasses ) );
		ri.DebugSet( "r_scene_copies", va( "%d", oaxFxStats.sceneCopies ) );
	}
	memset( &oaxFxStats, 0, sizeof( oaxFxStats ) );
}
