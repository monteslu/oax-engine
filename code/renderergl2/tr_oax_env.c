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
tr_oax_env.c: a map's outdoor environment, opt-in by worldspawn keys. A map
without any of them renders exactly as before.

  "oax_wind"        "<strength> <speed> <yaw degrees>"
                    foliage sways: a plant's top moves strength * (20 +
                    0.06 * height) units in a full gust (0.4 is a fair
                    breeze: grass leans, trees move a few percent), speed
                    the gust frequency in Hz, yaw where the wind blows to
  "oax_foliageaa"   "1": foliage edges by alpha to coverage (needs MSAA)
  "oax_atmosphere"  "<r> <g> <b> <density> <falloff> <baseZ> <sunScatter>"
                    height fog plus distance haze: extinction per unit at
                    baseZ, decaying by exp(-falloff * (z - baseZ)) above
                    it; sunScatter brightens the fog toward the sun
  "oax_clouds"      "<scale> <speedX> <speedY> <coverage> <darkness>"
                    cloud shadows: scrolling noise (scale units per cell)
                    over the sun-lit areas; coverage 0..1 how much sky is
                    cloud, darkness 0..1 how much a cloud takes from the sun
  "oax_grade"       "<saturation> <contrast> <r> <g> <b> <vignette>"
                    colour grading after tonemapping: saturation and
                    contrast 1 = unchanged, r g b a multiplier, vignette
                    0..1 the darkening at the corners

Each feature reads tr.oaxEnv; r_oaxEnv 0 turns them all off.
===========================================================================
*/

#include "tr_local.h"

cvar_t *r_oaxEnv;

// up to n numbers from a value (sscanf: the tokenizer's buffer holds the value)
static void EnvVec( const char *v, float *out, int n ) {
	float f[8];
	int i, got;

	got = sscanf( v, "%f %f %f %f %f %f %f %f", &f[0], &f[1], &f[2], &f[3], &f[4], &f[5], &f[6], &f[7] );
	for ( i = 0; i < n && i < got; i++ ) {
		out[i] = f[i];
	}
}

void R_OAXEnvRegisterCvars( void ) {
	r_oaxEnv = ri.Cvar_Get( "r_oaxEnv", "1", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_oaxEnv, "Map environment effects the map asks for (wind, atmosphere, clouds, grading, foliage alpha to coverage); 0 turns them off." );
}

/*
=================
R_OAXEnvLoadWorld

From the world's entity string: the worldspawn keys above.
=================
*/
void R_OAXEnvLoadWorld( void ) {
	char *p, *tok;
	char key[MAX_TOKEN_CHARS], value[MAX_TOKEN_CHARS];
	oaxEnv_t *e = &tr.oaxEnv;

	Com_Memset( e, 0, sizeof( *e ) );
	if ( !tr.world || !tr.world->entityString ) {
		return;
	}
	p = tr.world->entityString;
	tok = COM_ParseExt( &p, qtrue );
	if ( tok[0] != '{' ) {
		return;
	}
	for ( ;; ) {
		tok = COM_ParseExt( &p, qtrue );
		if ( !tok[0] || tok[0] == '}' ) {
			break;
		}
		Q_strncpyz( key, tok, sizeof( key ) );
		tok = COM_ParseExt( &p, qfalse );
		Q_strncpyz( value, tok, sizeof( value ) );
		tok = value;
		if ( !Q_stricmp( key, "oax_wind" ) ) {
			e->wind[2] = 30.0f;
			EnvVec( tok, e->wind, 3 );
			e->hasWind = e->wind[0] > 0.0f;
		} else if ( !Q_stricmp( key, "oax_foliageaa" ) ) {
			e->foliageA2C = atoi( tok ) != 0;
		} else if ( !Q_stricmp( key, "oax_atmosphere" ) ) {
			EnvVec( tok, e->atmos, 7 );
			e->hasAtmos = e->atmos[3] > 0.0f;
		} else if ( !Q_stricmp( key, "oax_clouds" ) ) {
			EnvVec( tok, e->clouds, 5 );
			e->hasClouds = e->clouds[0] > 0.0f && e->clouds[4] > 0.0f;
		} else if ( !Q_stricmp( key, "oax_grade" ) ) {
			e->grade[0] = e->grade[1] = 1.0f;
			e->grade[2] = e->grade[3] = e->grade[4] = 1.0f;
			EnvVec( tok, e->grade, 6 );
			e->hasGrade = qtrue;
		}
	}
	if ( e->hasWind || e->hasAtmos || e->hasClouds || e->hasGrade || e->foliageA2C ) {
		ri.Printf( PRINT_ALL, "oax env:%s%s%s%s%s\n", e->hasWind ? " wind" : "", e->foliageA2C ? " foliageaa" : "",
			e->hasAtmos ? " atmosphere" : "", e->hasClouds ? " clouds" : "", e->hasGrade ? " grade" : "" );
	}
}

qboolean R_OAXEnvOn( void ) {
	return r_oaxEnv && r_oaxEnv->integer;
}

// ---- passes ------------------------------------------------------------------

extern const char *fallbackShader_oaxatmos_vp;
extern const char *fallbackShader_oaxatmos_fp;
extern const char *fallbackShader_oaxgrade_vp;
extern const char *fallbackShader_oaxgrade_fp;

void R_OAXEnvInitGLSL( void ) {
	if ( GLSL_InitGPUShader( &tr.oaxAtmosShader, "oaxatmos", ATTR_POSITION | ATTR_TEXCOORD, qtrue, "", qtrue,
			fallbackShader_oaxatmos_vp, fallbackShader_oaxatmos_fp ) ) {
		GLSL_InitUniforms( &tr.oaxAtmosShader );
		GLSL_SetUniformInt( &tr.oaxAtmosShader, UNIFORM_SCREENDEPTHMAP, TB_COLORMAP );
		GLSL_FinishGPUShader( &tr.oaxAtmosShader );
	} else {
		ri.Printf( PRINT_WARNING, "WARNING: oaxatmos shader failed; oax_atmosphere is off\n" );
	}
	if ( GLSL_InitGPUShader( &tr.oaxGradeShader, "oaxgrade", ATTR_POSITION | ATTR_TEXCOORD, qtrue, "", qtrue,
			fallbackShader_oaxgrade_vp, fallbackShader_oaxgrade_fp ) ) {
		GLSL_InitUniforms( &tr.oaxGradeShader );
		GLSL_SetUniformInt( &tr.oaxGradeShader, UNIFORM_TEXTUREMAP, TB_COLORMAP );
		GLSL_FinishGPUShader( &tr.oaxGradeShader );
	} else {
		ri.Printf( PRINT_WARNING, "WARNING: oaxgrade shader failed; oax_grade is off\n" );
	}
}

void R_OAXEnvShutdownGLSL( void ) {
	GLSL_DeleteGPUShader( &tr.oaxAtmosShader );
	GLSL_DeleteGPUShader( &tr.oaxGradeShader );
}

FBO_t *FBO_Create( const char *name, int width, int height );
qboolean R_CheckFBO( const FBO_t *fbo );

/*
=================
RB_OAXAtmosphere

The map's height fog and haze, blended over the scene by the distance read
back from the depth buffer, in scene light before tonemapping (like the
view fog, into an FBO holding only tr.renderImage).
=================
*/
void RB_OAXAtmosphere( FBO_t *srcFbo, ivec4_t box ) {
	FBO_t *oldFbo = glState.currentFBO;
	shaderProgram_t *sp = &tr.oaxAtmosShader;
	const oaxEnv_t *e = &tr.oaxEnv;
	vec4_t v, color, texCorners;
	float w, h;

	if ( !R_OAXEnvOn() || !e->hasAtmos || !srcFbo || !tr.renderImage || !tr.renderDepthImage || !sp->program
		|| ( backEnd.refdef.rdflags & ( RDF_NOWORLDMODEL | RDF_OAX_SKYPORTAL ) ) ) {
		return;
	}
	if ( !tr.oaxFogFbo ) {
		tr.oaxFogFbo = FBO_Create( "_oaxfog", tr.renderImage->width, tr.renderImage->height );
		FBO_AttachImage( tr.oaxFogFbo, tr.renderImage, GL_COLOR_ATTACHMENT0, 0 );
		R_CheckFBO( tr.oaxFogFbo );
		FBO_Bind( oldFbo );
	}
	w = tr.renderDepthImage->width;
	h = tr.renderDepthImage->height;
	texCorners[0] = box[0] / w;
	texCorners[1] = ( box[1] + box[3] ) / h;
	texCorners[2] = ( box[0] + box[2] ) / w;
	texCorners[3] = box[1] / h;

	GLSL_BindProgram( sp );
	VectorSet4( v, r_znear->value, backEnd.viewParms.zFar, 0, 0 );
	GLSL_SetUniformVec4( sp, UNIFORM_VIEWINFO, v );
	VectorSet4( v, tan( backEnd.viewParms.fovX * M_PI / 360.0f ), tan( backEnd.viewParms.fovY * M_PI / 360.0f ), box[0] / w, box[1] / h );
	GLSL_SetUniformVec4( sp, UNIFORM_NORMALSCALE, v );
	VectorSet4( v, box[2] / w, box[3] / h, 0, 0 );
	GLSL_SetUniformVec4( sp, UNIFORM_SPECULARSCALE, v );
	GLSL_SetUniformVec3( sp, UNIFORM_VIEWORIGIN, backEnd.viewParms.or.origin );
	GLSL_SetUniformVec3( sp, UNIFORM_VIEWFORWARD, backEnd.viewParms.or.axis[0] );
	GLSL_SetUniformVec3( sp, UNIFORM_VIEWLEFT, backEnd.viewParms.or.axis[1] );
	GLSL_SetUniformVec3( sp, UNIFORM_VIEWUP, backEnd.viewParms.or.axis[2] );
	VectorSet4( v, tr.sunDirection[0], tr.sunDirection[1], tr.sunDirection[2], 0 );
	GLSL_SetUniformVec4( sp, UNIFORM_PRIMARYLIGHTORIGIN, v );
	VectorSet4( v, e->atmos[4], e->atmos[5], e->atmos[6], 16000.0f );
	GLSL_SetUniformVec4( sp, UNIFORM_FOGDISTANCE, v );
	VectorSet4( color, e->atmos[0], e->atmos[1], e->atmos[2], e->atmos[3] );

	FBO_BlitFromTexture( tr.renderDepthImage, texCorners, NULL, tr.oaxFogFbo, box, sp, color,
		GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA );
	FBO_Bind( oldFbo );
}

/*
=================
RB_OAXGrade

The map's colour grading and vignette, after tonemapping.
=================
*/
/*
=================
R_OAXGradeUniforms

The grading parameters for a shader that applies them itself (the tone map
when it writes straight to the screen, RB_PostProcess); off unless the map
grades and on is set.
=================
*/
void R_OAXGradeUniforms( shaderProgram_t *sp, qboolean on ) {
	const oaxEnv_t *e = &tr.oaxEnv;
	vec4_t v;
	vec3_t c;

	if ( on && R_OAXEnvOn() && e->hasGrade && !( backEnd.refdef.rdflags & RDF_NOWORLDMODEL ) ) {
		VectorSet4( v, e->grade[0], e->grade[1], e->grade[5], 1 );
		VectorSet( c, e->grade[2], e->grade[3], e->grade[4] );
	} else {
		VectorSet4( v, 1, 1, 0, 0 );
		VectorSet( c, 1, 1, 1 );
	}
	GLSL_SetUniformVec4( sp, UNIFORM_FOGDEPTH, v );
	GLSL_SetUniformVec3( sp, UNIFORM_DIRECTEDLIGHT, c );
}

/*
=================
R_OAXAtmosActive / R_OAXAtmosUniforms

Whether this view gets the map's atmosphere, and its parameters for a
shader that applies it itself (the tone map on the direct path: the same
computation as oaxatmos_fp.glsl, without the separate blended pass).
=================
*/
qboolean R_OAXAtmosActive( void ) {
	return R_OAXEnvOn() && tr.oaxEnv.hasAtmos && tr.renderDepthImage
		&& !( backEnd.refdef.rdflags & ( RDF_NOWORLDMODEL | RDF_OAX_SKYPORTAL ) );
}

void R_OAXAtmosUniforms( shaderProgram_t *sp, qboolean on ) {
	const oaxEnv_t *e = &tr.oaxEnv;
	vec4_t v;

	GLSL_SetUniformFloat( sp, UNIFORM_FOGEYET, on ? 1.0f : 0.0f );
	if ( !on ) {
		return;
	}
	GL_BindToTMU( tr.renderDepthImage, TB_SHADOWMAP );
	VectorSet4( v, r_znear->value, backEnd.viewParms.zFar, 0, 0 );
	GLSL_SetUniformVec4( sp, UNIFORM_VIEWINFO, v );
	VectorSet4( v, tan( backEnd.viewParms.fovX * M_PI / 360.0f ), tan( backEnd.viewParms.fovY * M_PI / 360.0f ), 0, 0 );
	GLSL_SetUniformVec4( sp, UNIFORM_NORMALSCALE, v );
	GLSL_SetUniformVec3( sp, UNIFORM_VIEWORIGIN, backEnd.viewParms.or.origin );
	GLSL_SetUniformVec3( sp, UNIFORM_VIEWFORWARD, backEnd.viewParms.or.axis[0] );
	GLSL_SetUniformVec3( sp, UNIFORM_VIEWLEFT, backEnd.viewParms.or.axis[1] );
	GLSL_SetUniformVec3( sp, UNIFORM_VIEWUP, backEnd.viewParms.or.axis[2] );
	VectorSet4( v, tr.sunDirection[0], tr.sunDirection[1], tr.sunDirection[2], 0 );
	GLSL_SetUniformVec4( sp, UNIFORM_PRIMARYLIGHTORIGIN, v );
	VectorSet4( v, e->atmos[4], e->atmos[5], e->atmos[6], 16000.0f );
	GLSL_SetUniformVec4( sp, UNIFORM_FOGDISTANCE, v );
	VectorSet4( v, e->atmos[0], e->atmos[1], e->atmos[2], e->atmos[3] );
	GLSL_SetUniformVec4( sp, UNIFORM_FOGCOLORMASK, v );
}

void RB_OAXGrade( FBO_t *srcFbo, ivec4_t box ) {
	shaderProgram_t *sp = &tr.oaxGradeShader;
	const oaxEnv_t *e = &tr.oaxEnv;
	vec4_t v, color;

	if ( !R_OAXEnvOn() || !e->hasGrade || !srcFbo || !tr.screenScratchFbo || !sp->program
		|| ( backEnd.refdef.rdflags & RDF_NOWORLDMODEL ) ) {
		return;
	}
	GLSL_BindProgram( sp );
	VectorSet4( v, e->grade[0], e->grade[1], e->grade[5], 0 );
	GLSL_SetUniformVec4( sp, UNIFORM_FOGDISTANCE, v );
	VectorSet4( color, e->grade[2], e->grade[3], e->grade[4], 1 );
	FBO_FastBlit( srcFbo, box, tr.screenScratchFbo, box, GL_COLOR_BUFFER_BIT, GL_NEAREST );
	FBO_BlitFromTexture( tr.screenScratchFbo->colorImage[0], NULL, NULL, srcFbo, box, sp, color, GLS_DEPTHTEST_DISABLE );
}
