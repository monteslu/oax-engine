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
tr_oax_bloom.c: bloom in the HDR post-process chain (phase 6).

From RB_PostProcess, in scene light after the view fog and before the
tonemap, when r_oaxBloom is 1 (off by default, so stock frames stay
bit-identical):

1. prefilter: the view at half size, bright parts only (soft-knee
   threshold r_oaxBloomThreshold / r_oaxBloomKnee, in scene light);
2. a pyramid of r_oaxBloomLevels levels, each half the last (13-tap
   downsample);
3. back up the pyramid, each level added into the next larger one with a
   tent filter;
4. the half-size result added onto the scene, times r_oaxBloomIntensity.
===========================================================================
*/

#include "tr_local.h"

#define MAX_BLOOM_LEVELS 6

static cvar_t	*r_oaxBloomThreshold;
static cvar_t	*r_oaxBloomKnee;
static cvar_t	*r_oaxBloomIntensity;
static cvar_t	*r_oaxBloomLevels;

static FBO_t	*bloomFbo[MAX_BLOOM_LEVELS];
static image_t	*bloomImage[MAX_BLOOM_LEVELS];
static FBO_t	*compositeFbo;
static int		bloomWidth, bloomHeight;

void R_OAXBloomRegisterCvars( void ) {
	r_oaxBloomThreshold = ri.Cvar_Get( "r_oaxBloomThreshold", "1.0", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_oaxBloomThreshold, "Bloom: scene light above this blooms." );
	r_oaxBloomKnee = ri.Cvar_Get( "r_oaxBloomKnee", "0.5", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_oaxBloomKnee, "Bloom: soft knee below the threshold, as a fraction of it." );
	r_oaxBloomIntensity = ri.Cvar_Get( "r_oaxBloomIntensity", "0.6", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_oaxBloomIntensity, "Bloom: how much of the blurred light is added back." );
	r_oaxBloomLevels = ri.Cvar_Get( "r_oaxBloomLevels", "5", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_oaxBloomLevels, "Bloom: pyramid levels (1-6); more is wider." );
	memset( bloomFbo, 0, sizeof( bloomFbo ) );
	memset( bloomImage, 0, sizeof( bloomImage ) );
	compositeFbo = NULL;
	bloomWidth = bloomHeight = 0;
}

static qboolean BloomTargets( void ) {
	int hdrFormat = ( r_hdr->integer && glRefConfig.textureFloat ) ? GL_RGBA16F_ARB : GL_RGBA8;
	int i, w, h;

	if ( bloomFbo[0] ) {
		return qtrue;
	}
	if ( !tr.renderImage ) {
		return qfalse;
	}
	w = bloomWidth = MAX( tr.renderImage->width / 2, 1 );
	h = bloomHeight = MAX( tr.renderImage->height / 2, 1 );
	for ( i = 0; i < MAX_BLOOM_LEVELS; i++ ) {
		bloomImage[i] = R_CreateImage( va( "*oaxBloom%d", i ), NULL, w, h, IMGTYPE_COLORALPHA,
			IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, hdrFormat );
		bloomFbo[i] = FBO_Create( va( "_oaxBloom%d", i ), w, h );
		FBO_AttachImage( bloomFbo[i], bloomImage[i], GL_COLOR_ATTACHMENT0, 0 );
		R_CheckFBO( bloomFbo[i] );
		w = MAX( w / 2, 1 );
		h = MAX( h / 2, 1 );
	}
	compositeFbo = FBO_Create( "_oaxBloomComposite", tr.renderImage->width, tr.renderImage->height );
	FBO_AttachImage( compositeFbo, tr.renderImage, GL_COLOR_ATTACHMENT0, 0 );
	R_CheckFBO( compositeFbo );
	return qtrue;
}

/*
=============
RB_OAXBloom

box: the view in the render target's pixels (x, y from the top left as
RB_PostProcess passes it, width, height).
=============
*/
void RB_OAXBloom( FBO_t *srcFbo, ivec4_t box ) {
	FBO_t	*oldFbo = glState.currentFBO;
	vec4_t	corners, parms;
	int		i, levels;
	float	w, h;

	if ( !r_oaxBloom->integer || !srcFbo || !tr.renderImage ) {
		return;
	}
	for ( i = 0; i < 4; i++ ) {
		if ( !tr.oaxBloomShader[i].program ) {
			return;
		}
	}
	if ( !BloomTargets() ) {
		return;
	}
	levels = r_oaxBloomLevels->integer;
	if ( levels < 1 ) {
		levels = 1;
	}
	if ( levels > MAX_BLOOM_LEVELS ) {
		levels = MAX_BLOOM_LEVELS;
	}

	// 1. prefilter the view into level 0
	w = tr.renderImage->width;
	h = tr.renderImage->height;
	corners[0] = box[0] / w;
	corners[1] = ( box[1] + box[3] ) / h;
	corners[2] = ( box[0] + box[2] ) / w;
	corners[3] = box[1] / h;
	VectorSet4( parms, r_oaxBloomThreshold->value, r_oaxBloomKnee->value, 0, 0 );
	FBO_BlitFromTexture( tr.renderImage, corners, NULL, bloomFbo[0], NULL, &tr.oaxBloomShader[0], parms, GLS_DEPTHTEST_DISABLE );
	oaxFxStats.bloomPasses++;

	// 2. down the pyramid
	for ( i = 1; i < levels; i++ ) {
		FBO_BlitFromTexture( bloomImage[i - 1], NULL, NULL, bloomFbo[i], NULL, &tr.oaxBloomShader[1], NULL, GLS_DEPTHTEST_DISABLE );
		oaxFxStats.bloomPasses++;
	}

	// 3. back up, adding each level into the next larger one
	VectorSet4( parms, 1.0f, 0, 0, 0 );
	for ( i = levels - 1; i > 0; i-- ) {
		FBO_BlitFromTexture( bloomImage[i], NULL, NULL, bloomFbo[i - 1], NULL, &tr.oaxBloomShader[2], parms,
			GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE );
		oaxFxStats.bloomPasses++;
	}

	// 4. onto the scene. FBO_BlitFromTexture's quad for an explicit
	// destination box is upside down against its full-target quad (the one
	// the prefilter drew), so the source corners are flipped here.
	VectorSet4( parms, r_oaxBloomIntensity->value, 0, 0, 0 );
	VectorSet4( corners, 0.0f, 0.0f, 1.0f, 1.0f );
	FBO_BlitFromTexture( bloomImage[0], corners, NULL, compositeFbo, box, &tr.oaxBloomShader[3], parms,
		GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE );
	oaxFxStats.bloomPasses++;

	FBO_Bind( oldFbo );
}
