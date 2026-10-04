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
tr_oax.c: renderergl2 state for the oax map features.

- Light styles: tr.oaxLightStyles[], read by `rgbGen lightstyle <n>` and
  `alphaGen lightstyle <n>` stages, set by cgame through
  CG_OAX_R_SETLIGHTSTYLE. Every style is 1.0 ("on") until set, so a map
  looks right on a stock cgame.
- View fog: a depth-based fog step on the FBO path (RB_OAXViewFog in
  tr_postprocess.c), set by cgame through CG_OAX_R_SETVIEWFOG. The cheat
  cvar r_oaxViewFog "r g b density start end" overrides it for tests.
- Sky portals: a scene flagged RDF_OAX_SKYPORTAL renders the sky room into
  the shared HDR FBO first; the main scene, flagged RDF_OAX_UNDERSKY, then
  draws over it without drawing sky. Entities are split between the two
  scenes by the BSP area of their origin, so the cgame can submit one
  entity list for both. r_oaxSkyPortal 0 turns the feature off (the map's
  skyparms box is drawn instead).
===========================================================================
*/

#include "tr_local.h"

cvar_t	*r_oaxSkyPortal;
cvar_t	*r_oaxViewFog;

static float	oaxFog[4];			// rgb, density
static float	oaxFogRange[2];		// start, end

extern const char *fallbackShader_oaxviewfog_vp;
extern const char *fallbackShader_oaxviewfog_fp;
extern const char *fallbackShader_oaxproc_vp;
extern const char *fallbackShader_oaxproc_fp;

cvar_t *r_oaxSkyModelLight;

void R_OAXRegisterCvars( void ) {
	r_oaxSkyPortal = ri.Cvar_Get( "r_oaxSkyPortal", "1", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_oaxSkyPortal, "Draw oax sky portals (0 draws the map's fallback skybox)." );
	r_oaxViewFog = ri.Cvar_Get( "r_oaxViewFog", "", CVAR_CHEAT );
	ri.Cvar_SetDescription( r_oaxViewFog, "Test override for the oax view fog: \"r g b density start end\"." );
	ri.Cmd_AddCommand( "imageprogram", R_OAXImageProgram_f );
	R_OAXSurfIdRegisterCvars();
	r_oaxSkyModelLight = ri.Cvar_Get( "r_oaxSkyModelLight", "1", CVAR_CHEAT );
	ri.Cvar_SetDescription( r_oaxSkyModelLight, "Light models in a sky portal scene that have no light grid light (worldspawn oaxSkyAmbient / oaxSkyLight); 0 leaves them to the grid as Q3 does." );
	R_OAXFxRegisterCvars();		// phase 6 effects (tr_oax_fx.c)
	R_OAXBloomRegisterCvars();
	R_OAXDisplayRegisterCvars();
	R_OAXWaterReset();
	R_OAXResetMapState();
	R_OAXProcReset();
}

/*
=================
R_OAXResetMapState

Called when a world map loads: all light styles on, no view fog.
=================
*/
void R_OAXResetMapState( void ) {
	int i;

	for ( i = 0; i < OAX_MAX_LIGHTSTYLES; i++ ) {
		VectorSet( tr.oaxLightStyles[i], 1.0f, 1.0f, 1.0f );
	}
	VectorSet4( oaxFog, 0, 0, 0, 0 );
	oaxFogRange[0] = oaxFogRange[1] = 0;
	tr.oaxSkyArea = -1;
}

void RE_OAXSetLightStyle( int style, float r, float g, float b ) {
	if ( style < 0 || style >= OAX_MAX_LIGHTSTYLES ) {
		return;
	}
	VectorSet( tr.oaxLightStyles[style], r, g, b );
}

void RE_OAXSetViewFog( const float *rgb, float density, float start, float end ) {
	if ( density <= 0 || !rgb ) {
		VectorSet4( oaxFog, 0, 0, 0, 0 );
		return;
	}
	VectorSet4( oaxFog, rgb[0], rgb[1], rgb[2], density );
	oaxFogRange[0] = start;
	oaxFogRange[1] = end;
}

/*
=================
RE_OAXFeatures

The oax_features tokens this renderer implements.
=================
*/
const char *RE_OAXFeatures( void ) {
	return va( "skyportal lightstyle proc viewfog physics_skel %s", R_OAXFxFeatures() );
}

static int R_OAXPointArea( const vec3_t p ) {
	mnode_t	*node;

	if ( !tr.world ) {
		return -1;
	}
	node = tr.world->nodes;
	while ( node->contents == -1 ) {
		if ( DotProduct( p, node->plane->normal ) - node->plane->dist > 0 ) {
			node = node->children[0];
		} else {
			node = node->children[1];
		}
	}
	return node->area;
}

/*
=================
R_OAXBeginScene

End of RE_BeginScene: per-scene oax state.
=================
*/
void R_OAXBeginScene( void ) {
	tr.refdef.oaxViewFog[3] = 0;

	if ( tr.refdef.rdflags & RDF_NOWORLDMODEL ) {
		tr.refdef.rdflags &= ~( RDF_OAX_SKYPORTAL | RDF_OAX_UNDERSKY );
		return;
	}

	if ( !r_oaxSkyPortal->integer ) {
		tr.refdef.rdflags &= ~RDF_OAX_UNDERSKY;
	}

	if ( tr.refdef.rdflags & RDF_OAX_SKYPORTAL ) {
		// the sky room: no dlights, marks or particles from the arena
		tr.oaxSkyArea = R_OAXPointArea( tr.refdef.vieworg );
		tr.refdef.num_dlights = 0;
		tr.refdef.numPolys = 0;
		return;
	}

	if ( !( tr.refdef.rdflags & RDF_OAX_UNDERSKY ) ) {
		tr.oaxSkyArea = -1;
	}

	if ( r_oaxViewFog->string[0] ) {
		float v[6] = { 0, 0, 0, 0, 0, 0 };

		sscanf( r_oaxViewFog->string, "%f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5] );
		VectorSet4( tr.refdef.oaxViewFog, v[0], v[1], v[2], v[3] );
		tr.refdef.oaxViewFogRange[0] = v[4];
		tr.refdef.oaxViewFogRange[1] = v[5];
	} else {
		Vector4Copy( oaxFog, tr.refdef.oaxViewFog );
		tr.refdef.oaxViewFogRange[0] = oaxFogRange[0];
		tr.refdef.oaxViewFogRange[1] = oaxFogRange[1];
	}
}

/*
=================
R_OAXCullEntity

Sky portal scenes keep only the entities in the sky camera's area; the
scene drawn under them drops those entities.
=================
*/
qboolean R_OAXCullEntity( const trRefEntity_t *ent ) {
	int area;

	if ( tr.oaxSkyArea < 0 || !( tr.refdef.rdflags & ( RDF_OAX_SKYPORTAL | RDF_OAX_UNDERSKY ) ) ) {
		return qfalse;
	}
	area = R_OAXPointArea( ent->e.origin );
	if ( tr.refdef.rdflags & RDF_OAX_SKYPORTAL ) {
		return area != tr.oaxSkyArea;
	}
	return area == tr.oaxSkyArea;
}

/*
=================
R_OAXInitGLSL
=================
*/
void R_OAXInitGLSL( void ) {
	static const char *procDefines[OAX_PROC_NUM_PROGRAMS] = {
		"#define PROC_FIRE_STEP\n",
		"#define PROC_FIRE_COLOR\n",
		"#define PROC_WATER_STEP\n",
		"#define PROC_WATER_COLOR\n",
		"#define PROC_WET\n",
		"#define PROC_ICE\n",
		"#define PROC_PLASMA\n",
	};
	int attribs = ATTR_POSITION | ATTR_TEXCOORD;
	int i;

	// the passes need GLSL 1.30 / ES 3.00 (integer hashing, texelFetch);
	// without them the features just stay off
	if ( !glRefConfig.framebufferObject || glRefConfig.glslMajorVersion < 1
		|| ( glRefConfig.glslMajorVersion == 1 && glRefConfig.glslMinorVersion < 30 ) ) {
		return;
	}

	if ( GLSL_InitGPUShader( &tr.oaxViewFogShader, "oaxviewfog", attribs, qtrue, "", qtrue,
			fallbackShader_oaxviewfog_vp, fallbackShader_oaxviewfog_fp ) ) {
		GLSL_InitUniforms( &tr.oaxViewFogShader );
		GLSL_SetUniformInt( &tr.oaxViewFogShader, UNIFORM_SCREENDEPTHMAP, TB_COLORMAP );
		GLSL_FinishGPUShader( &tr.oaxViewFogShader );
	} else {
		ri.Printf( PRINT_WARNING, "WARNING: oaxviewfog shader failed; view fog is off\n" );
	}

	for ( i = 0; i < OAX_PROC_NUM_PROGRAMS; i++ ) {
		if ( !GLSL_InitGPUShader( &tr.oaxProcShader[i], "oaxproc", attribs, qtrue, procDefines[i], qtrue,
				fallbackShader_oaxproc_vp, fallbackShader_oaxproc_fp ) ) {
			ri.Printf( PRINT_WARNING, "WARNING: oaxproc shader %d failed; procedural textures are off\n", i );
			continue;
		}
		GLSL_InitUniforms( &tr.oaxProcShader[i] );
		GLSL_SetUniformInt( &tr.oaxProcShader[i], UNIFORM_TEXTUREMAP, TB_COLORMAP );
		GLSL_SetUniformInt( &tr.oaxProcShader[i], UNIFORM_LEVELSMAP, TB_LEVELSMAP );
		GLSL_FinishGPUShader( &tr.oaxProcShader[i] );
	}

	R_OAXFxInitGLSL();
	R_OAXDisplayInitGLSL();
	R_OAXEnvInitGLSL();
}

void R_OAXShutdownGLSL( void ) {
	int i;

	GLSL_DeleteGPUShader( &tr.oaxViewFogShader );
	for ( i = 0; i < OAX_PROC_NUM_PROGRAMS; i++ ) {
		GLSL_DeleteGPUShader( &tr.oaxProcShader[i] );
	}
	R_OAXFxShutdownGLSL();
	R_OAXDisplayShutdownGLSL();
	R_OAXEnvShutdownGLSL();
}
