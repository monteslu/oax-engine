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
tr_oax_display.c: an opt-in display curve (step 7.5 B), written for this
engine.

r_displayCurve 1 puts the finished scene through a display curve
(brightness + gamma) of the kind some older OpenGL renderers apply to
everything they draw, screenshots included. With shader brightness
B = clamp(2 x r_displayCurveBrightness, 0.05, 2.99) and gamma
g = 1 / (1 + r_displayCurveGammaOffset), per pixel, v = max(r, g, b):

  B > 1: color *= (v + (1 - (2v - 1)^2) / 4 (B - 1)) / v, clamped to 0..1
  B < 1: color *= B
  then   color = color^g

The defaults are Brightness 1.0 and GammaOffset 0.1; Brightness 0.5 and
GammaOffset 0 are the identity. It lets an imported UE1 map look as it did on
such a renderer without baking the curve into the lighting, which stays
linear (docs/lights.md). Off by default: frames are untouched unless it
is set. It runs on the 3D scene, after tone mapping
and exposure, before the 2D overlays.
===========================================================================
*/

#include "tr_local.h"
#include "tr_fbo.h"

extern const char *fallbackShader_oaxdisplay_vp;
extern const char *fallbackShader_oaxdisplay_fp;

static cvar_t *r_displayCurve;
static cvar_t *r_displayCurveBrightness;
static cvar_t *r_displayCurveGammaOffset;

void R_OAXDisplayRegisterCvars( void ) {
	r_displayCurve = ri.Cvar_Get( "r_displayCurve", "0", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_displayCurve, "1: put the 3D scene through a brightness + gamma display curve (r_displayCurveBrightness, r_displayCurveGammaOffset). 0 (default): linear." );
	r_displayCurveBrightness = ri.Cvar_Get( "r_displayCurveBrightness", "1.0", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_displayCurveBrightness, "Display curve: brightness (default 1.0; 0.5 is neutral)." );
	r_displayCurveGammaOffset = ri.Cvar_Get( "r_displayCurveGammaOffset", "0.1", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_displayCurveGammaOffset, "Display curve: gamma offset (default 0.1; 0 is neutral)." );
}

void R_OAXDisplayInitGLSL( void ) {
	if ( !GLSL_InitGPUShader( &tr.oaxDisplayShader, "oaxdisplay", ATTR_POSITION | ATTR_TEXCOORD, qtrue, "", qtrue,
			fallbackShader_oaxdisplay_vp, fallbackShader_oaxdisplay_fp ) ) {
		ri.Printf( PRINT_WARNING, "WARNING: oaxdisplay shader failed; r_displayCurve does nothing\n" );
		return;
	}
	GLSL_InitUniforms( &tr.oaxDisplayShader );
	GLSL_SetUniformInt( &tr.oaxDisplayShader, UNIFORM_TEXTUREMAP, TB_COLORMAP );
	GLSL_FinishGPUShader( &tr.oaxDisplayShader );
}

void R_OAXDisplayShutdownGLSL( void ) {
	GLSL_DeleteGPUShader( &tr.oaxDisplayShader );
}

/*
=================
RB_OAXDisplayCurve

The curve on srcFbo's box, through the scratch FBO (an FBO cannot be read
and written at once).
=================
*/
void RB_OAXDisplayCurve( FBO_t *srcFbo, ivec4_t box ) {
	vec4_t parms;
	float B;

	if ( !r_displayCurve || !r_displayCurve->integer || !srcFbo || !tr.screenScratchFbo || !tr.oaxDisplayShader.program ) {
		return;
	}
	B = 2.0f * r_displayCurveBrightness->value;
	B = B < 0.05f ? 0.05f : B > 2.99f ? 2.99f : B;
	VectorSet4( parms, B, 1.0f / ( 1.0f + r_displayCurveGammaOffset->value ), 0, 0 );
	FBO_FastBlit( srcFbo, box, tr.screenScratchFbo, box, GL_COLOR_BUFFER_BIT, GL_NEAREST );
	FBO_BlitFromTexture( tr.screenScratchFbo->colorImage[0], NULL, NULL, srcFbo, box, &tr.oaxDisplayShader, parms, GLS_DEPTHTEST_DISABLE );
}
