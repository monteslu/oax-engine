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
tr_procedural.c: procedural textures for shader stages (oax).

    procedural fire   <size> [cooling] [sparks]
    procedural water  <size> <source image> [damping] [strength]
    procedural wet    <size> <source image> [amount] [speed]
    procedural ice    <size> <source image> [amount] [speed]
    procedural plasma <size> [r g b]

The stage samples a <size> x <size> RGBA8 render target that GLSL passes
(glsl/oaxproc_*.glsl) fill:
- fire: a cellular heat field (sparks along the bottom edge, heat rising
  and cooling), ping-pong in RGBA16F, mapped through a fire palette.
- water: a damped wave equation on a height field (raindrops seeded per
  step), ping-pong in RGBA16F; the output is the source image refracted
  by the height gradient.
- wet, ice: the source image distorted by a time-varying analytic field.
- plasma: analytic.

Updates are tied to shader time, not frame time: the simulation advances
in fixed 1/30 s steps to floor(shaderTime * 30), at most once per frame,
and only for textures a visible surface used this frame (marked when the
draw surface is added). Every random choice hashes (seed, step, texel)
with integer math, and the seed is fixed per texture, so a frame is the
same on every host and r_fixedShaderTime freezes it. A simulation that
is new, went backwards in time or skipped more than OAX_PROC_WINDOW steps
restarts from its seed state OAX_PROC_WINDOW steps back.
===========================================================================
*/

#include "tr_local.h"

#define OAX_PROC_MAX		32
#define OAX_PROC_HZ			30
#define OAX_PROC_WINDOW		48

typedef enum {
	OAX_PROC_FIRE,
	OAX_PROC_WATER,
	OAX_PROC_WET,
	OAX_PROC_ICE,
	OAX_PROC_PLASMA
} oaxProcType_t;

typedef struct {
	oaxProcType_t	type;
	int				size;
	image_t			*out;
	image_t			*state[2];
	image_t			*source;
	FBO_t			*outFbo;
	FBO_t			*stateFbo[2];
	vec4_t			params;
	unsigned		seed;
	int				cur;			// state[cur] is the latest step
	int				lastStep;		// -1: never simulated
	int				visibleFrame;
} oaxProc_t;

static oaxProc_t	procs[OAX_PROC_MAX];
static int			numProcs;

FBO_t *FBO_Create(const char *name, int width, int height);
qboolean R_CheckFBO(const FBO_t * fbo);

void R_OAXProcReset( void ) {
	Com_Memset( procs, 0, sizeof( procs ) );
	numProcs = 0;
}

static unsigned Proc_Hash( const char *s ) {
	unsigned h = 2166136261u;

	while ( *s ) {
		h ^= (byte)*s++;
		h *= 16777619u;
	}
	return h;
}

/*
=================
R_OAXParseProcedural

`procedural <type> <size> [params]` in a shader stage.
=================
*/
image_t *R_OAXParseProcedural( char **text, const char *shaderName ) {
	oaxProc_t	*p;
	char		*token;
	char		typeName[32];
	int			i, size;
	imgFlags_t	flags = IMGFLAG_NO_COMPRESSION;

	token = COM_ParseExt( text, qfalse );
	Q_strncpyz( typeName, token, sizeof( typeName ) );
	size = atoi( COM_ParseExt( text, qfalse ) );

	if ( numProcs == OAX_PROC_MAX ) {
		ri.Printf( PRINT_WARNING, "WARNING: too many procedural textures (shader '%s')\n", shaderName );
		return NULL;
	}
	if ( size < 8 || size > 512 || ( size & ( size - 1 ) ) ) {
		ri.Printf( PRINT_WARNING, "WARNING: procedural size %d must be a power of two from 8 to 512 (shader '%s')\n", size, shaderName );
		return NULL;
	}

	p = &procs[numProcs];
	Com_Memset( p, 0, sizeof( *p ) );
	p->size = size;
	p->lastStep = -1;
	p->visibleFrame = -1;
	p->seed = Proc_Hash( va( "%s/%s/%d", shaderName, typeName, numProcs ) );

	if ( !Q_stricmp( typeName, "fire" ) ) {
		p->type = OAX_PROC_FIRE;
		VectorSet4( p->params, 0.018f, 0.55f, 0.0f, 0.0f );
	} else if ( !Q_stricmp( typeName, "water" ) ) {
		p->type = OAX_PROC_WATER;
		VectorSet4( p->params, 0.985f, 0.04f, 0.0f, 0.0f );
	} else if ( !Q_stricmp( typeName, "wet" ) ) {
		p->type = OAX_PROC_WET;
		VectorSet4( p->params, 0.012f, 1.0f, 0.0f, 0.0f );
	} else if ( !Q_stricmp( typeName, "ice" ) ) {
		p->type = OAX_PROC_ICE;
		VectorSet4( p->params, 0.02f, 0.25f, 0.0f, 0.0f );
	} else if ( !Q_stricmp( typeName, "plasma" ) ) {
		p->type = OAX_PROC_PLASMA;
		VectorSet4( p->params, 1.0f, 0.4f, 0.9f, 0.0f );
	} else {
		ri.Printf( PRINT_WARNING, "WARNING: unknown procedural type '%s' in shader '%s'\n", typeName, shaderName );
		return NULL;
	}

	if ( p->type == OAX_PROC_WATER || p->type == OAX_PROC_WET || p->type == OAX_PROC_ICE ) {
		token = COM_ParseExt( text, qfalse );
		p->source = token[0] ? R_FindImageFile( token, IMGTYPE_COLORALPHA, IMGFLAG_MIPMAP | IMGFLAG_PICMIP ) : NULL;
		if ( !p->source ) {
			ri.Printf( PRINT_WARNING, "WARNING: procedural %s needs a source image (shader '%s')\n", typeName, shaderName );
			return NULL;
		}
	}

	// optional numbers override the defaults in order
	for ( i = 0; i < 3; i++ ) {
		token = COM_ParseExt( text, qfalse );
		if ( !token[0] ) {
			break;
		}
		p->params[i] = atof( token );
	}

	p->out = R_CreateImage( va( "*proc%d", numProcs ), NULL, size, size, IMGTYPE_COLORALPHA, flags, GL_RGBA8 );
	if ( p->type == OAX_PROC_FIRE || p->type == OAX_PROC_WATER ) {
		p->state[0] = R_CreateImage( va( "*proc%da", numProcs ), NULL, size, size, IMGTYPE_COLORALPHA, flags, GL_RGBA16F );
		p->state[1] = R_CreateImage( va( "*proc%db", numProcs ), NULL, size, size, IMGTYPE_COLORALPHA, flags, GL_RGBA16F );
	}
	numProcs++;
	return p->out;
}

/*
=================
R_OAXProcMarkVisible

A draw surface with this shader was added: update its procedurals.
=================
*/
void R_OAXProcMarkVisible( const shader_t *shader ) {
	int i, j;

	for ( i = 0; i < shader->numUnfoggedPasses && i < MAX_SHADER_STAGES; i++ ) {
		const shaderStage_t *stage = shader->stages[i];

		if ( !stage ) {
			break;
		}
		for ( j = 0; j < numProcs; j++ ) {
			if ( procs[j].out == stage->bundle[0].image[0] || procs[j].out == stage->bundle[TB_DIFFUSEMAP].image[0] ) {
				procs[j].visibleFrame = tr.frameCount;
			}
		}
	}
}

static FBO_t *Proc_Fbo( image_t *image ) {
	FBO_t *oldFbo = glState.currentFBO;
	FBO_t *fbo = FBO_Create( va( "_%s", image->imgName + 1 ), image->width, image->height );

	FBO_AttachImage( fbo, image, GL_COLOR_ATTACHMENT0, 0 );
	R_CheckFBO( fbo );
	FBO_Bind( oldFbo );
	return fbo;
}

// one full-target pass: program prog reads `in` (TB_COLORMAP) and the
// source (TB_LEVELSMAP), writes dst
static void Proc_Pass( oaxProc_t *p, int prog, image_t *in, FBO_t *dst, int step ) {
	shaderProgram_t *sp = &tr.oaxProcShader[prog];
	vec4_t info;

	if ( !sp->program ) {
		return;
	}

	VectorSet4( info, (float)step, (float)( p->seed & 0xffff ), (float)( p->seed >> 16 ), 1.0f / p->size );
	GLSL_SetUniformVec4( sp, UNIFORM_VIEWINFO, info );
	GLSL_SetUniformFloat( sp, UNIFORM_TIME, (float)step / OAX_PROC_HZ );
	GL_BindToTMU( p->source ? p->source : tr.whiteImage, TB_LEVELSMAP );
	FBO_BlitFromTexture( in ? in : tr.whiteImage, NULL, NULL, dst, NULL, sp, p->params, GLS_DEPTHTEST_DISABLE );
}

static void Proc_Clear( FBO_t *fbo ) {
	FBO_t *oldFbo = glState.currentFBO;

	FBO_Bind( fbo );
	qglViewport( 0, 0, fbo->width, fbo->height );
	qglScissor( 0, 0, fbo->width, fbo->height );
	qglClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	qglClear( GL_COLOR_BUFFER_BIT );
	FBO_Bind( oldFbo );
}

static void Proc_Update( oaxProc_t *p, int step ) {
	int first, s;

	if ( !p->outFbo ) {
		p->outFbo = Proc_Fbo( p->out );
		if ( p->state[0] ) {
			p->stateFbo[0] = Proc_Fbo( p->state[0] );
			p->stateFbo[1] = Proc_Fbo( p->state[1] );
		}
	}

	switch ( p->type ) {
	case OAX_PROC_FIRE:
	case OAX_PROC_WATER:
		if ( p->lastStep < 0 || step < p->lastStep || step - p->lastStep > OAX_PROC_WINDOW ) {
			// restart from the seed state, a window back
			Proc_Clear( p->stateFbo[0] );
			p->cur = 0;
			first = step - OAX_PROC_WINDOW + 1;
			if ( first < 0 ) {
				first = 0;
			}
		} else {
			first = p->lastStep + 1;
		}
		for ( s = first; s <= step; s++ ) {
			Proc_Pass( p, p->type == OAX_PROC_FIRE ? OAX_PROC_PROG_FIRE_STEP : OAX_PROC_PROG_WATER_STEP,
				p->state[p->cur], p->stateFbo[p->cur ^ 1], s );
			p->cur ^= 1;
		}
		Proc_Pass( p, p->type == OAX_PROC_FIRE ? OAX_PROC_PROG_FIRE_COLOR : OAX_PROC_PROG_WATER_COLOR,
			p->state[p->cur], p->outFbo, step );
		break;
	case OAX_PROC_WET:
		Proc_Pass( p, OAX_PROC_PROG_WET, NULL, p->outFbo, step );
		break;
	case OAX_PROC_ICE:
		Proc_Pass( p, OAX_PROC_PROG_ICE, NULL, p->outFbo, step );
		break;
	case OAX_PROC_PLASMA:
		Proc_Pass( p, OAX_PROC_PROG_PLASMA, NULL, p->outFbo, step );
		break;
	}
	p->lastStep = step;
}

/*
=================
RB_OAXUpdateProcedurals

Start of a 3D view in the back end, before its FBO is bound: brings every
procedural texture used this frame to the current shader time.
=================
*/
void RB_OAXUpdateProcedurals( double shaderTime ) {
	int i, step;

	if ( !numProcs || !glRefConfig.framebufferObject ) {
		return;
	}
	step = (int)floor( shaderTime * OAX_PROC_HZ );
	if ( step < 0 ) {
		step = 0;
	}
	for ( i = 0; i < numProcs; i++ ) {
		if ( procs[i].visibleFrame == tr.frameCount && procs[i].lastStep != step ) {
			Proc_Update( &procs[i], step );
		}
	}
}
