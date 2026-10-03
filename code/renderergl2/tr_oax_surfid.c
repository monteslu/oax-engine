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
tr_oax_surfid.c: the surface id pass (step 7.5 D, docs/test-hooks.md).

When a test asks for it, the main world view is drawn a second time into a
private RGBA8 target with every surface writing its draw id: a 24-bit
integer held exactly in three 8-bit channels (portable to ES 3.0 / WebGL,
which have no guaranteed integer or float read-back). A table kept for the
frame maps each draw id to what was drawn: the world surface index (or the
entity and its model), and the material (shader index and name). The pass
uses the generic vertex program, so deforms, vertex and bone animation and
the first stage's alpha test match the real draw; blending is off and the
nearest surface wins each pixel.

- r_oaxSurfaceIdAt "x y [x y ...]" (cheat, up to 16 points, window pixels
  from the top left like a screenshot): every frame while set, publishes
  r_surfid<i> = "x y drawId kind index entity shaderIndex shaderName [model]"
  and r_surfid_frame.
- r_oaxSurfaceIdDump <n> (cheat): the next n frames each write a whole-frame
  map, surfids/frame<count>.txt in the home directory, also kept as the
  debug blob (the wasmcart "debug_blob" field). Format in docs/test-hooks.md.
- r_oaxSurfaceIdOpaque 1: only opaque surfaces (sort <= SS_OPAQUE) take
  pixels, so a translucent or additive surface does not hide the material
  behind it.

Off (both cvars empty / 0) the pass never runs and nothing is allocated.
Particle systems, trails and flares are not drawn by the pass.
===========================================================================
*/

#include "tr_local.h"

extern const char *fallbackShader_generic_vp;
extern const char *fallbackShader_oaxsurfid_fp;

#define SURFID_MAX_DRAWS	0x40000
#define SURFID_MAX_POINTS	16

cvar_t	*r_oaxSurfaceIdAt;
cvar_t	*r_oaxSurfaceIdDump;
cvar_t	*r_oaxSurfaceIdOpaque;

typedef struct {
	const surfaceType_t	*surface;
	int					entityNum;
	const shader_t		*shader;
} surfIdDraw_t;

static surfIdDraw_t		draws[SURFID_MAX_DRAWS];
static int				numDraws;

static shaderProgram_t	programs[GENERICDEF_COUNT];
static GLint			idLocations[GENERICDEF_COUNT];
static qboolean			programsReady, programsFailed;

static FBO_t			idFbo;
static qboolean			idFboReady;

static int				passFrame;		// tr.frameCount of the last pass
static int				dumpCount;

void R_OAXSurfIdRegisterCvars( void ) {
	r_oaxSurfaceIdAt = ri.Cvar_Get( "r_oaxSurfaceIdAt", "", CVAR_CHEAT | CVAR_TEMP );
	ri.Cvar_SetDescription( r_oaxSurfaceIdAt, "Report the surface and material under these window pixels (\"x y [x y ...]\", from the top left) as debug values r_surfid<i>, every frame." );
	r_oaxSurfaceIdDump = ri.Cvar_Get( "r_oaxSurfaceIdDump", "0", CVAR_CHEAT | CVAR_TEMP );
	ri.Cvar_SetDescription( r_oaxSurfaceIdDump, "Write a whole-frame surface and material id map for the next N frames (surfids/frame<n>.txt and the debug blob)." );
	r_oaxSurfaceIdOpaque = ri.Cvar_Get( "r_oaxSurfaceIdOpaque", "0", CVAR_CHEAT | CVAR_TEMP );
	ri.Cvar_SetDescription( r_oaxSurfaceIdOpaque, "Surface ids: only opaque surfaces take pixels." );
}

/* the pass runs this frame */
static qboolean SurfIdWanted( void ) {
	return r_oaxSurfaceIdAt->string[0] || r_oaxSurfaceIdDump->integer > 0;
}

/*
=================
R_OAXSurfIdBeginFrame

Front end, from RE_BeginFrame: builds the programs the first time the pass
is wanted (the GL context is current; pending commands are flushed first).
=================
*/
void R_OAXSurfIdBeginFrame( void ) {
	int i;

	if ( !SurfIdWanted() || programsReady || programsFailed ) {
		return;
	}
	if ( !glRefConfig.framebufferObject ) {
		ri.Printf( PRINT_WARNING, "surface ids need framebuffer objects; the pass is off\n" );
		programsFailed = qtrue;
		return;
	}
	R_IssuePendingRenderCommands();

	for ( i = 0; i < GENERICDEF_COUNT; i++ ) {
		char extradefines[1024];
		int attribs = ATTR_POSITION | ATTR_TEXCOORD | ATTR_LIGHTCOORD | ATTR_NORMAL | ATTR_COLOR;

		idLocations[i] = -1;
		if ( i & GENERICDEF_USE_FOG ) {
			continue;
		}
		if ( ( i & GENERICDEF_USE_VERTEX_ANIMATION ) && ( i & GENERICDEF_USE_BONE_ANIMATION ) ) {
			continue;
		}
		if ( ( i & GENERICDEF_USE_BONE_ANIMATION ) && !glRefConfig.glslMaxAnimatedBones ) {
			continue;
		}
		if ( ( i & GENERICDEF_USE_VERTEX_ANIMATION ) && !glRefConfig.gpuVertexAnimation ) {
			continue;
		}
		extradefines[0] = '\0';
		if ( i & GENERICDEF_USE_DEFORM_VERTEXES ) {
			Q_strcat( extradefines, sizeof( extradefines ), "#define USE_DEFORM_VERTEXES\n" );
		}
		if ( i & GENERICDEF_USE_TCGEN_AND_TCMOD ) {
			Q_strcat( extradefines, sizeof( extradefines ), "#define USE_TCGEN\n#define USE_TCMOD\n" );
		}
		if ( i & GENERICDEF_USE_VERTEX_ANIMATION ) {
			Q_strcat( extradefines, sizeof( extradefines ), "#define USE_VERTEX_ANIMATION\n" );
			attribs |= ATTR_POSITION2 | ATTR_NORMAL2;
		} else if ( i & GENERICDEF_USE_BONE_ANIMATION ) {
			Q_strcat( extradefines, sizeof( extradefines ), va( "#define USE_BONE_ANIMATION\n#define MAX_GLSL_BONES %d\n", glRefConfig.glslMaxAnimatedBones ) );
			attribs |= ATTR_BONE_INDEXES | ATTR_BONE_WEIGHTS;
		}
		if ( i & GENERICDEF_USE_RGBAGEN ) {
			Q_strcat( extradefines, sizeof( extradefines ), "#define USE_RGBAGEN\n" );
		}
		if ( !GLSL_InitGPUShader( &programs[i], "oaxsurfid", attribs, qtrue, extradefines, qtrue,
				fallbackShader_generic_vp, fallbackShader_oaxsurfid_fp ) ) {
			ri.Printf( PRINT_WARNING, "WARNING: oaxsurfid program %d failed; surface ids are off\n", i );
			programsFailed = qtrue;
			return;
		}
		GLSL_InitUniforms( &programs[i] );
		GLSL_SetUniformInt( &programs[i], UNIFORM_DIFFUSEMAP, TB_DIFFUSEMAP );
		GLSL_FinishGPUShader( &programs[i] );
		idLocations[i] = qglGetUniformLocation( programs[i].program, "u_OaxSurfId" );
	}
	programsReady = qtrue;
}

void R_OAXSurfIdShutdown( void ) {
	int i;

	if ( programsReady ) {
		for ( i = 0; i < GENERICDEF_COUNT; i++ ) {
			if ( programs[i].program ) {
				GLSL_DeleteGPUShader( &programs[i] );
			}
		}
	}
	memset( programs, 0, sizeof( programs ) );
	programsReady = programsFailed = qfalse;

	if ( idFboReady ) {
		if ( idFbo.colorBuffers[0] ) {
			qglDeleteRenderbuffers( 1, &idFbo.colorBuffers[0] );
		}
		if ( idFbo.depthBuffer ) {
			qglDeleteRenderbuffers( 1, &idFbo.depthBuffer );
		}
		if ( idFbo.frameBuffer ) {
			qglDeleteFramebuffers( 1, &idFbo.frameBuffer );
		}
		if ( glState.currentFBO == &idFbo ) {
			glState.currentFBO = NULL;
		}
	}
	memset( &idFbo, 0, sizeof( idFbo ) );
	idFboReady = qfalse;
	numDraws = 0;
}

/*
=================
RB_OAXSurfIdProgram

The id program for a generic-shader attribute set, and its id uniform.
=================
*/
shaderProgram_t *RB_OAXSurfIdProgram( int genericAttribs, GLint *idLoc ) {
	genericAttribs &= ~GENERICDEF_USE_FOG;
	if ( !programsReady || !programs[genericAttribs].program ) {
		return NULL;
	}
	*idLoc = idLocations[genericAttribs];
	return &programs[genericAttribs];
}

/*
=================
RB_OAXSurfIdSurface

From RB_RenderDrawSurfList in the id pass, before a surface's triangles are
added: returns the draw id for it (0 = skip the surface).
=================
*/
int RB_OAXSurfIdSurface( const surfaceType_t *surface, int entityNum, const shader_t *shader ) {
	switch ( *surface ) {
	case SF_FLARE:
	case SF_OAX_PARTICLES:
	case SF_OAX_TRAIL:
	case SF_BAD:
	case SF_SKIP:
		return 0;
	default:
		break;
	}
	if ( r_oaxSurfaceIdOpaque->integer && shader->sort > SS_OPAQUE ) {
		return 0;
	}
	if ( numDraws >= SURFID_MAX_DRAWS - 1 ) {
		return 0;
	}
	numDraws++;		// id 0 is "nothing"
	draws[numDraws].surface = surface;
	draws[numDraws].entityNum = entityNum;
	draws[numDraws].shader = shader;
	return numDraws;
}

/* the world surface (or brush model surface) index of a drawn surface, or -1 */
static int WorldSurfaceIndex( const surfaceType_t *surface ) {
	int i;

	if ( !tr.world ) {
		return -1;
	}
	for ( i = 0; i < tr.world->numsurfaces; i++ ) {
		if ( tr.world->surfaces[i].data == surface ) {
			return i;
		}
	}
	return -1;
}

/*
=================
DescribeDraw

"kind index entity shaderIndex shaderName [model]" for a draw id.
kind: none, world (index = world surface), bmodel (a brush model entity's
surface, index = world surface), poly, model (index = -1, the entity's model
name follows), sprite, decal, other.
=================
*/
static const char *DescribeDraw( int id ) {
	const surfIdDraw_t *d;
	const char *kind = "other";
	const char *model = "";
	int index = -1;

	if ( id <= 0 || id > numDraws ) {
		return "none -1 -1 -1 -";
	}
	d = &draws[id];
	switch ( *d->surface ) {
	case SF_FACE:
	case SF_GRID:
	case SF_TRIANGLES:
		index = WorldSurfaceIndex( d->surface );
		kind = d->entityNum == REFENTITYNUM_WORLD ? "world" : "bmodel";
		if ( index < 0 ) {
			kind = "triangles";
		}
		break;
	case SF_POLY:
		kind = "poly";
		break;
	case SF_MDV:
	case SF_MDR:
	case SF_IQM:
	case SF_VAO_MDVMESH:
	case SF_VAO_IQM:
		kind = "model";
		break;
	case SF_ENTITY:
		kind = "sprite";
		break;
	case SF_OAX_DECAL:
		kind = "decal";
		break;
	default:
		break;
	}
	if ( d->entityNum != REFENTITYNUM_WORLD && d->entityNum >= 0 && d->entityNum < backEnd.refdef.num_entities
		&& !strcmp( kind, "model" ) ) {
		model_t *m = R_GetModelByHandle( backEnd.refdef.entities[d->entityNum].e.hModel );
		if ( m ) {
			model = m->name;
		}
	}
	// a surface-world shader variant ("#oaxsurf<k>_<hash>", tr_surfworld.c)
	// is reported as its material, the variant name after it
	{
		const void *variant = R_OAXSurfVariant( d->shader->name );

		if ( variant ) {
			return va( "%s %d %d %d %s%s%s variant=%s", kind, index, d->entityNum == REFENTITYNUM_WORLD ? -1 : d->entityNum,
				d->shader->index, R_OAXSurfVariantMaterial( variant ), model[0] ? " " : "", model, d->shader->name );
		}
	}
	return va( "%s %d %d %d %s%s%s", kind, index, d->entityNum == REFENTITYNUM_WORLD ? -1 : d->entityNum,
		d->shader->index, d->shader->name, model[0] ? " " : "", model );
}

static int PixelId( const byte *p ) {
	return p[0] | ( p[1] << 8 ) | ( p[2] << 16 );
}

/*
=================
SurfIdReport

The pixels the tests asked for, then a whole-frame dump if one is pending.
The id target is bound.
=================
*/
static void SurfIdReport( void ) {
	char	buf[MAX_CVAR_VALUE_STRING];
	char	*p, *tok;
	int		n = 0;
	int		w = glConfig.vidWidth, h = glConfig.vidHeight;

	Q_strncpyz( buf, r_oaxSurfaceIdAt->string, sizeof( buf ) );
	p = buf;
	while ( n < SURFID_MAX_POINTS ) {
		int x, y;
		byte px[4];
		char name[32], value[512];

		tok = COM_Parse( &p );
		if ( !tok[0] ) {
			break;
		}
		x = atoi( tok );
		tok = COM_Parse( &p );
		if ( !tok[0] ) {
			break;
		}
		y = atoi( tok );
		// (no nested va(): its two buffers rotate)
		Com_sprintf( name, sizeof( name ), "r_surfid%d", n );
		if ( x < 0 || y < 0 || x >= w || y >= h ) {
			Com_sprintf( value, sizeof( value ), "%d %d 0 none -1 -1 -1 - (outside %dx%d)", x, y, w, h );
			ri.DebugSet( name, value );
			n++;
			continue;
		}
		qglReadPixels( x, h - 1 - y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px );
		Com_sprintf( value, sizeof( value ), "%d %d %d ", x, y, PixelId( px ) );
		Q_strcat( value, sizeof( value ), DescribeDraw( PixelId( px ) ) );
		ri.DebugSet( name, value );
		n++;
	}
	if ( n || r_oaxSurfaceIdDump->integer > 0 ) {
		ri.DebugSet( "r_surfid_frame", va( "%d %d %d %d", tr.frameCount, w, h, numDraws ) );
	}

	if ( r_oaxSurfaceIdDump->integer > 0 ) {
		byte	*pixels = ri.Hunk_AllocateTempMemory( w * h * 4 );
		int		cap = 4 << 20, len = 0;
		char	*out = ri.Hunk_AllocateTempMemory( cap );
		byte	*used = ri.Hunk_AllocateTempMemory( numDraws + 1 );
		int		x, y, i;
		qboolean truncated = qfalse;

		Com_Memset( used, 0, numDraws + 1 );
		qglReadPixels( 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels );
		for ( i = 0; i < w * h; i++ ) {
			int id = PixelId( pixels + i * 4 );
			if ( id <= numDraws ) {
				used[id] = 1;
			}
		}
		len += Com_sprintf( out + len, cap - len, "oaxsurfids 1\nsize %d %d\nframe %d\n", w, h, tr.frameCount );
		for ( i = 1; i <= numDraws; i++ ) {
			if ( used[i] && len < cap - 1024 ) {
				len += Com_sprintf( out + len, cap - len, "surf %d %s\n", i, DescribeDraw( i ) );
			}
		}
		// rows top to bottom, runs of "id*count"
		for ( y = 0; y < h && !truncated; y++ ) {
			const byte *row = pixels + ( h - 1 - y ) * w * 4;

			len += Com_sprintf( out + len, cap - len, "row %d", y );
			for ( x = 0; x < w; ) {
				int id = PixelId( row + x * 4 ), run = 1;

				while ( x + run < w && PixelId( row + ( x + run ) * 4 ) == id ) {
					run++;
				}
				if ( len > cap - 64 ) {
					truncated = qtrue;
					break;
				}
				len += Com_sprintf( out + len, cap - len, " %d*%d", id, run );
				x += run;
			}
			len += Com_sprintf( out + len, cap - len, "\n" );
		}
		if ( truncated ) {
			len += Com_sprintf( out + len, cap - len, "truncated\n" );
		}
		ri.FS_WriteFile( va( "surfids/frame%d.txt", tr.frameCount ), out, len );
		if ( ri.DebugSetBlob ) {
			ri.DebugSetBlob( out, len );
		}
		ri.Hunk_FreeTempMemory( used );
		ri.Hunk_FreeTempMemory( out );
		ri.Hunk_FreeTempMemory( pixels );
		ri.Cvar_Set( "r_oaxSurfaceIdDump", va( "%d", r_oaxSurfaceIdDump->integer - 1 ) );
		dumpCount++;
	}
}

/*
=================
RB_OAXSurfIdPass

From RB_DrawSurfs after a view is drawn: for the main world view (not a
portal, mirror, sky portal room, shadow or cubemap view), once per frame.
=================
*/
void RB_OAXSurfIdPass( drawSurf_t *drawSurfs, int numDrawSurfs ) {
	FBO_t	*oldFbo = glState.currentFBO;
	int		w = glConfig.vidWidth, h = glConfig.vidHeight;

	if ( !programsReady || !SurfIdWanted() ) {
		return;
	}
	if ( backEnd.refdef.rdflags & ( RDF_NOWORLDMODEL | RDF_OAX_SKYPORTAL ) ) {
		return;
	}
	if ( backEnd.viewParms.isPortal || ( backEnd.viewParms.flags & ( VPF_DEPTHSHADOW | VPF_SHADOWMAP ) )
		|| ( tr.renderCubeFbo && backEnd.viewParms.targetFbo == tr.renderCubeFbo ) ) {
		return;
	}
	if ( passFrame == tr.frameCount ) {
		return;
	}
	passFrame = tr.frameCount;

	if ( !idFboReady || idFbo.width != w || idFbo.height != h ) {
		if ( idFboReady ) {
			qglDeleteRenderbuffers( 1, &idFbo.colorBuffers[0] );
			qglDeleteRenderbuffers( 1, &idFbo.depthBuffer );
			qglDeleteFramebuffers( 1, &idFbo.frameBuffer );
			if ( glState.currentFBO == &idFbo ) {
				glState.currentFBO = NULL;
			}
		}
		memset( &idFbo, 0, sizeof( idFbo ) );
		Q_strncpyz( idFbo.name, "_oaxSurfId", sizeof( idFbo.name ) );
		idFbo.width = w;
		idFbo.height = h;
		qglGenFramebuffers( 1, &idFbo.frameBuffer );
		FBO_CreateBuffer( &idFbo, GL_RGBA8, 0, 0 );
		FBO_CreateBuffer( &idFbo, GL_DEPTH_COMPONENT24_ARB, 0, 0 );
		FBO_Bind( &idFbo );
		R_CheckFBO( &idFbo );
		idFboReady = qtrue;
	}

	FBO_Bind( &idFbo );
	qglViewport( 0, 0, w, h );
	qglScissor( 0, 0, w, h );
	GL_State( GLS_DEPTHMASK_TRUE );
	qglColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	qglClearColor( 0, 0, 0, 0 );
	qglClearDepth( 1.0f );
	qglClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
	qglDisable( GL_DITHER );

	qglViewport( backEnd.viewParms.viewportX, backEnd.viewParms.viewportY, backEnd.viewParms.viewportWidth, backEnd.viewParms.viewportHeight );
	qglScissor( backEnd.viewParms.viewportX, backEnd.viewParms.viewportY, backEnd.viewParms.viewportWidth, backEnd.viewParms.viewportHeight );

	numDraws = 0;
	backEnd.oaxIdFill = qtrue;
	RB_RenderDrawSurfList( drawSurfs, numDrawSurfs );
	backEnd.oaxIdFill = qfalse;

	FBO_Bind( &idFbo );
	SurfIdReport();

	qglEnable( GL_DITHER );
	qglColorMask( !backEnd.colorMask[0], !backEnd.colorMask[1], !backEnd.colorMask[2], !backEnd.colorMask[3] );
	FBO_Bind( oldFbo );
	SetViewportAndScissor();
}
