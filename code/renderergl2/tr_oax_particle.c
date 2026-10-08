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
tr_oax_particle.c: stateless GPU particles driven by particle decls.

Adapted from DOOM-3 neo/framework/DeclParticle.cpp (idDeclParticle::Parse,
ParseParticleStage, ParseParms, ParseParametric, idParticleStage::Default,
the derived cycleMsec, and the ParticleOrigin / ParticleVerts /
ParticleTexCoords / ParticleColors math) and neo/renderer/Model_prt.cpp
(idRenderModelPrt::InstantiateDynamicModel's per-index age, cycle and
spawn-bunching rules).
Changes: ported from C++ to C; decls are read from the .prt files in particles/ with
the Q3 tokenizer; materials are Q3 shaders; parametric tables are not
supported (from/to only); the per-particle math runs in the vertex shader
(glsl/oaxparticle_vp.glsl) over a static index buffer instead of building
vertices on the CPU, with an integer hash per (seed, cycle, index) in
place of idRandom; "aimed" particles are one quad from where the particle
was trailTime ago instead of a chain of trail quads; animation frames do
not cross fade; added the oax keywords softDistance, distort (heat haze: the
scene behind the particle, shifted) and lit (the light grid's colour at the system's
origin) and per-system tint,
scale, stop time and seed (oaxFx_t).

A particle system is a pure function of (decl, oaxFx_t, particle index,
time): nothing about a particle is stored between frames.
===========================================================================
*/

#include "tr_local.h"

static oaxPrtDecl_t	*prtDecls[OAX_PRT_MAX_DECLS];
static int			numPrtDecls;

// the text of every .prt file in particles/, and where each decl starts in it
#define MAX_PRT_DEFS	1024
static char		*prtText;
static char		*prtDefName[MAX_PRT_DEFS];
static char		*prtDefText[MAX_PRT_DEFS];
static int		numPrtDefs;

static vao_t	*prtVao;

/*
================
ParseParms

Parses a variable length list of parms on one line
================
*/
static void ParseParms( char **text, float *parms, int maxParms ) {
	int count = 0;
	char *token;

	memset( parms, 0, maxParms * sizeof( *parms ) );
	while ( 1 ) {
		token = COM_ParseExt( text, qfalse );
		if ( !token[0] ) {
			return;
		}
		if ( count == maxParms ) {
			ri.Printf( PRINT_WARNING, "WARNING: particle decl: too many parms on line\n" );
			continue;
		}
		parms[count++] = atof( token );
	}
}

/*
================
ParseParametric
================
*/
static void ParseParametric( char **text, oaxPrtParm_t *parm ) {
	char *token;
	char *save;

	parm->from = parm->to = 0.0f;
	token = COM_ParseExt( text, qtrue );
	if ( !token[0] ) {
		return;
	}
	parm->from = parm->to = atof( token );
	save = *text;
	token = COM_ParseExt( text, qtrue );
	if ( !Q_stricmp( token, "to" ) ) {
		token = COM_ParseExt( text, qtrue );
		parm->to = atof( token );
	} else {
		*text = save;
	}
}

static float ParseFloat( char **text ) {
	return atof( COM_ParseExt( text, qtrue ) );
}

static qboolean ParseBool( char **text ) {
	char *token = COM_ParseExt( text, qtrue );
	return !Q_stricmp( token, "true" ) || atoi( token ) != 0;
}

/*
================
StageDefault

idParticleStage::Default
================
*/
static void StageDefault( oaxPrtStage_t *s ) {
	memset( s, 0, sizeof( *s ) );
	s->material = tr.defaultShader;
	s->totalParticles = 100;
	s->spawnBunching = 1.0f;
	s->particleLife = 1.5f;
	s->distributionType = OAX_PDIST_RECT;
	s->distributionParms[0] = s->distributionParms[1] = s->distributionParms[2] = 8.0f;
	s->directionType = OAX_PDIR_CONE;
	s->directionParms[0] = 90.0f;
	s->orientation = OAX_POR_VIEW;
	s->speed.from = s->speed.to = 150.0f;
	s->gravity = 1.0f;
	s->size.from = s->size.to = 4.0f;
	s->aspect.from = s->aspect.to = 1.0f;
	VectorSet4( s->color, 1, 1, 1, 1 );
	s->fadeInFraction = 0.1f;
	s->fadeOutFraction = 0.25f;
	s->randomDistribution = qtrue;
	s->cycleMsec = ( s->particleLife + s->deadTime ) * 1000;
}

/*
================
StageRadius

A bound on how far a particle of this stage gets from the system origin
(DOOM-3 samples GetStageBounds; this is an analytic over-estimate).
================
*/
static float StageRadius( const oaxPrtStage_t *s ) {
	float r, life = s->particleLife, speed;

	r = sqrt( s->distributionParms[0] * s->distributionParms[0] + s->distributionParms[1] * s->distributionParms[1]
		+ s->distributionParms[2] * s->distributionParms[2] );
	r += VectorLength( s->offset );
	if ( s->customPathType == OAX_PPATH_STANDARD ) {
		speed = MAX( fabs( s->speed.from ), fabs( s->speed.to ) );
		r += speed * life * ( 1.0f + fabs( s->directionParms[0] ) );
	} else {
		r += fabs( s->customPathParms[0] ) + fabs( s->customPathParms[1] ) + fabs( s->customPathParms[2] )
			+ ( fabs( s->customPathParms[3] ) + fabs( s->customPathParms[4] ) ) * life;
	}
	r += fabs( s->gravity ) * life * life;
	r += MAX( fabs( s->size.from ), fabs( s->size.to ) ) * MAX( 1.0f, MAX( s->aspect.from, s->aspect.to ) );
	return r + s->boundsExpansion;
}

/*
================
ParseParticleStage

idDeclParticle::ParseParticleStage
================
*/
static void ParseParticleStage( char **text, oaxPrtStage_t *stage, const char *declName ) {
	char *token;

	StageDefault( stage );

	while ( 1 ) {
		token = COM_ParseExt( text, qtrue );
		if ( !token[0] || !Q_stricmp( token, "}" ) ) {
			break;
		}
		if ( !Q_stricmp( token, "material" ) ) {
			token = COM_ParseExt( text, qtrue );
			stage->material = R_FindShader( token, LIGHTMAP_NONE, qtrue );
			continue;
		}
		if ( !Q_stricmp( token, "count" ) ) {
			stage->totalParticles = atoi( COM_ParseExt( text, qtrue ) );
			continue;
		}
		if ( !Q_stricmp( token, "time" ) ) {
			stage->particleLife = ParseFloat( text );
			continue;
		}
		if ( !Q_stricmp( token, "cycles" ) ) {
			stage->cycles = ParseFloat( text );
			continue;
		}
		if ( !Q_stricmp( token, "timeOffset" ) ) {
			stage->timeOffset = ParseFloat( text );
			continue;
		}
		if ( !Q_stricmp( token, "deadTime" ) ) {
			stage->deadTime = ParseFloat( text );
			continue;
		}
		if ( !Q_stricmp( token, "randomDistribution" ) ) {
			stage->randomDistribution = ParseBool( text );
			continue;
		}
		if ( !Q_stricmp( token, "bunching" ) ) {
			stage->spawnBunching = ParseFloat( text );
			continue;
		}
		if ( !Q_stricmp( token, "distribution" ) ) {
			token = COM_ParseExt( text, qfalse );
			if ( !Q_stricmp( token, "rect" ) ) {
				stage->distributionType = OAX_PDIST_RECT;
			} else if ( !Q_stricmp( token, "cylinder" ) ) {
				stage->distributionType = OAX_PDIST_CYLINDER;
			} else if ( !Q_stricmp( token, "sphere" ) ) {
				stage->distributionType = OAX_PDIST_SPHERE;
			} else {
				ri.Printf( PRINT_WARNING, "WARNING: particle %s: bad distribution type %s\n", declName, token );
			}
			ParseParms( text, stage->distributionParms, 4 );
			continue;
		}
		if ( !Q_stricmp( token, "direction" ) ) {
			token = COM_ParseExt( text, qfalse );
			if ( !Q_stricmp( token, "cone" ) ) {
				stage->directionType = OAX_PDIR_CONE;
			} else if ( !Q_stricmp( token, "outward" ) ) {
				stage->directionType = OAX_PDIR_OUTWARD;
			} else {
				ri.Printf( PRINT_WARNING, "WARNING: particle %s: bad direction type %s\n", declName, token );
			}
			ParseParms( text, stage->directionParms, 4 );
			continue;
		}
		if ( !Q_stricmp( token, "orientation" ) ) {
			token = COM_ParseExt( text, qfalse );
			if ( !Q_stricmp( token, "view" ) ) {
				stage->orientation = OAX_POR_VIEW;
			} else if ( !Q_stricmp( token, "aimed" ) ) {
				stage->orientation = OAX_POR_AIMED;
			} else if ( !Q_stricmp( token, "x" ) ) {
				stage->orientation = OAX_POR_X;
			} else if ( !Q_stricmp( token, "y" ) ) {
				stage->orientation = OAX_POR_Y;
			} else if ( !Q_stricmp( token, "z" ) ) {
				stage->orientation = OAX_POR_Z;
			} else {
				ri.Printf( PRINT_WARNING, "WARNING: particle %s: bad orientation type %s\n", declName, token );
			}
			ParseParms( text, stage->orientationParms, 4 );
			continue;
		}
		if ( !Q_stricmp( token, "customPath" ) ) {
			token = COM_ParseExt( text, qfalse );
			if ( !Q_stricmp( token, "standard" ) ) {
				stage->customPathType = OAX_PPATH_STANDARD;
			} else if ( !Q_stricmp( token, "helix" ) ) {
				stage->customPathType = OAX_PPATH_HELIX;
			} else if ( !Q_stricmp( token, "flies" ) ) {
				stage->customPathType = OAX_PPATH_FLIES;
			} else if ( !Q_stricmp( token, "spherical" ) ) {
				stage->customPathType = OAX_PPATH_ORBIT;
			} else if ( !Q_stricmp( token, "drip" ) ) {
				stage->customPathType = OAX_PPATH_DRIP;
			} else {
				ri.Printf( PRINT_WARNING, "WARNING: particle %s: bad path type %s\n", declName, token );
			}
			ParseParms( text, stage->customPathParms, 8 );
			continue;
		}
		if ( !Q_stricmp( token, "speed" ) ) {
			ParseParametric( text, &stage->speed );
			continue;
		}
		if ( !Q_stricmp( token, "rotation" ) ) {
			ParseParametric( text, &stage->rotationSpeed );
			continue;
		}
		if ( !Q_stricmp( token, "angle" ) ) {
			stage->initialAngle = ParseFloat( text );
			continue;
		}
		if ( !Q_stricmp( token, "entityColor" ) ) {
			stage->entityColor = ParseBool( text );
			continue;
		}
		if ( !Q_stricmp( token, "size" ) ) {
			ParseParametric( text, &stage->size );
			continue;
		}
		if ( !Q_stricmp( token, "aspect" ) ) {
			ParseParametric( text, &stage->aspect );
			continue;
		}
		if ( !Q_stricmp( token, "fadeIn" ) ) {
			stage->fadeInFraction = ParseFloat( text );
			continue;
		}
		if ( !Q_stricmp( token, "fadeOut" ) ) {
			stage->fadeOutFraction = ParseFloat( text );
			continue;
		}
		if ( !Q_stricmp( token, "fadeIndex" ) ) {
			stage->fadeIndexFraction = ParseFloat( text );
			continue;
		}
		if ( !Q_stricmp( token, "color" ) ) {
			stage->color[0] = ParseFloat( text );
			stage->color[1] = ParseFloat( text );
			stage->color[2] = ParseFloat( text );
			stage->color[3] = ParseFloat( text );
			continue;
		}
		if ( !Q_stricmp( token, "fadeColor" ) ) {
			stage->fadeColor[0] = ParseFloat( text );
			stage->fadeColor[1] = ParseFloat( text );
			stage->fadeColor[2] = ParseFloat( text );
			stage->fadeColor[3] = ParseFloat( text );
			continue;
		}
		if ( !Q_stricmp( token, "offset" ) ) {
			stage->offset[0] = ParseFloat( text );
			stage->offset[1] = ParseFloat( text );
			stage->offset[2] = ParseFloat( text );
			continue;
		}
		if ( !Q_stricmp( token, "animationFrames" ) ) {
			stage->animationFrames = atoi( COM_ParseExt( text, qtrue ) );
			continue;
		}
		if ( !Q_stricmp( token, "animationRate" ) ) {
			stage->animationRate = ParseFloat( text );
			continue;
		}
		if ( !Q_stricmp( token, "boundsExpansion" ) ) {
			stage->boundsExpansion = ParseFloat( text );
			continue;
		}
		if ( !Q_stricmp( token, "gravity" ) ) {
			char *save = *text;

			token = COM_ParseExt( text, qtrue );
			if ( !Q_stricmp( token, "world" ) ) {
				stage->worldGravity = qtrue;
			} else {
				*text = save;
			}
			stage->gravity = ParseFloat( text );
			continue;
		}
		// oax: lit, the stage's colour follows the light grid at the system's origin
		if ( !Q_stricmp( token, "lit" ) ) {
			stage->lit = ParseFloat( text ) != 0;
			continue;
		}
		// oax: heat haze, the particle shows the scene behind it shifted outward by
		// this fraction of the screen times its alpha (0.01 is a shimmer)
		if ( !Q_stricmp( token, "distort" ) ) {
			stage->distort = ParseFloat( text );
			continue;
		}
		// oax: soft particles, fade over this many units in front of the scene (-1 off)
		if ( !Q_stricmp( token, "softDistance" ) ) {
			stage->softDistance = ParseFloat( text );
			continue;
		}
		ri.Printf( PRINT_WARNING, "WARNING: particle %s: unknown token %s\n", declName, token );
		SkipRestOfLine( text );
	}

	// derive values
	if ( stage->totalParticles < 0 ) {
		stage->totalParticles = 0;
	}
	if ( stage->totalParticles > OAX_PRT_MAX_PARTICLES ) {
		ri.Printf( PRINT_WARNING, "WARNING: particle %s: count %d clamped to %d\n", declName, stage->totalParticles, OAX_PRT_MAX_PARTICLES );
		stage->totalParticles = OAX_PRT_MAX_PARTICLES;
	}
	stage->cycleMsec = ( stage->particleLife + stage->deadTime ) * 1000;
	stage->radius = StageRadius( stage );
}

/*
================
R_OAXPrtLoadDecls

Reads every .prt file in particles/. Decls are parsed when first registered.
================
*/
void R_OAXPrtLoadDecls( void ) {
	char	**files;
	char	*buffers[MAX_PRT_DEFS];
	int		numFiles, i, sum = 0;
	char	*p, *token, *start;

	numPrtDecls = 0;
	numPrtDefs = 0;
	prtText = NULL;

	files = ri.FS_ListFiles( "particles", ".prt", &numFiles );
	if ( !files || !numFiles ) {
		ri.FS_FreeFileList( files );
		return;
	}
	if ( numFiles > MAX_PRT_DEFS ) {
		numFiles = MAX_PRT_DEFS;
	}
	// sorted, so every build reads them in the same order
	for ( i = 0; i < numFiles; i++ ) {
		buffers[i] = NULL;
		if ( ri.FS_ReadFile( va( "particles/%s", files[i] ), (void **)&buffers[i] ) > 0 ) {
			sum += strlen( buffers[i] ) + 1;
		}
	}
	prtText = ri.Hunk_Alloc( sum + 1, h_low );
	p = prtText;
	for ( i = 0; i < numFiles; i++ ) {
		if ( buffers[i] ) {
			strcpy( p, buffers[i] );
			p += strlen( buffers[i] ) + 1;
			ri.FS_FreeFile( buffers[i] );
		}
	}
	ri.FS_FreeFileList( files );

	// index "particle <name> { ... }"; COM_Compress-free so line breaks stay
	for ( p = prtText; p < prtText + sum; p += strlen( p ) + 1 ) {
		char *text = p;

		COM_BeginParseSession( "particles" );
		while ( 1 ) {
			token = COM_ParseExt( &text, qtrue );
			if ( !token[0] ) {
				break;
			}
			if ( Q_stricmp( token, "particle" ) ) {
				// some other decl type, or junk: skip its block
				token = COM_ParseExt( &text, qtrue );
				if ( !Q_stricmp( token, "{" ) ) {
					SkipBracedSection( &text, 1 );
				} else {
					token = COM_ParseExt( &text, qtrue );
					if ( !Q_stricmp( token, "{" ) ) {
						SkipBracedSection( &text, 1 );
					}
				}
				continue;
			}
			token = COM_ParseExt( &text, qtrue );
			if ( !token[0] || numPrtDefs >= MAX_PRT_DEFS ) {
				break;
			}
			prtDefName[numPrtDefs] = ri.Hunk_Alloc( strlen( token ) + 1, h_low );
			strcpy( prtDefName[numPrtDefs], token );
			start = text;
			token = COM_ParseExt( &text, qtrue );
			if ( Q_stricmp( token, "{" ) ) {
				ri.Printf( PRINT_WARNING, "WARNING: particle %s: expected {\n", prtDefName[numPrtDefs] );
				break;
			}
			prtDefText[numPrtDefs] = start;
			numPrtDefs++;
			SkipBracedSection( &text, 1 );
		}
	}
	ri.Printf( PRINT_DEVELOPER, "%d particle decls in %d files\n", numPrtDefs, numFiles );
}

/*
================
ParseDecl

idDeclParticle::Parse
================
*/
static void ParseDecl( oaxPrtDecl_t *decl, char *text ) {
	char *token;
	int i;

	token = COM_ParseExt( &text, qtrue );	// {
	while ( 1 ) {
		token = COM_ParseExt( &text, qtrue );
		if ( !token[0] || !Q_stricmp( token, "}" ) ) {
			break;
		}
		if ( !Q_stricmp( token, "{" ) ) {
			if ( decl->numStages >= OAX_PRT_MAX_STAGES ) {
				ri.Printf( PRINT_WARNING, "WARNING: particle %s: more than %d stages\n", decl->name, OAX_PRT_MAX_STAGES );
				SkipBracedSection( &text, 1 );
				continue;
			}
			ParseParticleStage( &text, &decl->stages[decl->numStages], decl->name );
			decl->numStages++;
			continue;
		}
		if ( !Q_stricmp( token, "depthHack" ) ) {
			ParseFloat( &text );	// DOOM-3 view-weapon depth hack: not used
			continue;
		}
		ri.Printf( PRINT_WARNING, "WARNING: particle %s: bad token %s\n", decl->name, token );
		SkipRestOfLine( &text );
	}

	decl->radius = 0;
	decl->durationMsec = 0;
	for ( i = 0; i < decl->numStages; i++ ) {
		const oaxPrtStage_t *s = &decl->stages[i];
		int end;

		decl->radius = MAX( decl->radius, s->radius );
		if ( s->cycles <= 0 ) {
			decl->durationMsec = -1;
		} else if ( decl->durationMsec >= 0 ) {
			// the last particle is born at (cycles - 1) * cycleMsec + its bunch offset and lives particleLife
			end = s->timeOffset * 1000 + ( s->cycles - 1 ) * s->cycleMsec + s->particleLife * 1000 * s->spawnBunching
				+ s->particleLife * 1000 + 1;
			decl->durationMsec = MAX( decl->durationMsec, end );
		}
	}
	if ( decl->durationMsec < 0 ) {
		decl->durationMsec = 0;
	}
}

/*
================
RE_OAXRegisterFx
================
*/
qhandle_t RE_OAXRegisterFx( const char *name ) {
	oaxPrtDecl_t *decl;
	int i;

	if ( !name || !name[0] || !tr.registered ) {
		return 0;
	}
	for ( i = 0; i < numPrtDecls; i++ ) {
		if ( !Q_stricmp( prtDecls[i]->name, name ) ) {
			return i + 1;
		}
	}
	if ( numPrtDecls >= OAX_PRT_MAX_DECLS ) {
		ri.Printf( PRINT_WARNING, "WARNING: RE_OAXRegisterFx: more than %d particle decls\n", OAX_PRT_MAX_DECLS );
		return 0;
	}
	for ( i = 0; i < numPrtDefs; i++ ) {
		if ( !Q_stricmp( prtDefName[i], name ) ) {
			break;
		}
	}
	if ( i == numPrtDefs ) {
		ri.Printf( PRINT_DEVELOPER, "RE_OAXRegisterFx: no particle decl %s\n", name );
		return 0;
	}
	decl = ri.Hunk_Alloc( sizeof( *decl ), h_low );
	Q_strncpyz( decl->name, name, sizeof( decl->name ) );
	COM_BeginParseSession( name );
	ParseDecl( decl, prtDefText[i] );
	prtDecls[numPrtDecls++] = decl;
	return numPrtDecls;
}

oaxPrtDecl_t *R_OAXGetPrtDecl( qhandle_t h ) {
	if ( h < 1 || h > numPrtDecls ) {
		return NULL;
	}
	return prtDecls[h - 1];
}

/*
================
R_OAXPrtInitVao

The static index buffer every particle draw shares: one quad per particle
index, attr_Position = ( index, corner s, corner t ).
================
*/
void R_OAXPrtInitVao( void ) {
	float		*verts;
	uint16_t	*indexes;
	int			i, vsize, isize;

	prtVao = NULL;
	numPrtDecls = 0;
	vsize = OAX_PRT_MAX_PARTICLES * 4 * 3 * sizeof( float );
	isize = OAX_PRT_MAX_PARTICLES * 6 * sizeof( uint16_t );
	verts = ri.Hunk_AllocateTempMemory( vsize );
	indexes = ri.Hunk_AllocateTempMemory( isize );
	for ( i = 0; i < OAX_PRT_MAX_PARTICLES; i++ ) {
		float *v = verts + i * 12;

		// DOOM-3 vertex order: 0 1 / 2 3
		v[0] = i; v[1] = 0; v[2] = 0;
		v[3] = i; v[4] = 1; v[5] = 0;
		v[6] = i; v[7] = 0; v[8] = 1;
		v[9] = i; v[10] = 1; v[11] = 1;
		indexes[i * 6 + 0] = i * 4 + 0;
		indexes[i * 6 + 1] = i * 4 + 2;
		indexes[i * 6 + 2] = i * 4 + 3;
		indexes[i * 6 + 3] = i * 4 + 0;
		indexes[i * 6 + 4] = i * 4 + 3;
		indexes[i * 6 + 5] = i * 4 + 1;
	}
	prtVao = R_CreateVao( "_oaxParticles", (byte *)verts, vsize, (byte *)indexes, isize, VAO_USAGE_STATIC );
	prtVao->attribs[ATTR_INDEX_POSITION].enabled = 1;
	prtVao->attribs[ATTR_INDEX_POSITION].count = 3;
	prtVao->attribs[ATTR_INDEX_POSITION].type = GL_FLOAT;
	prtVao->attribs[ATTR_INDEX_POSITION].normalized = GL_FALSE;
	prtVao->attribs[ATTR_INDEX_POSITION].stride = 3 * sizeof( float );
	prtVao->attribs[ATTR_INDEX_POSITION].offset = 0;
	Vao_SetVertexPointers( prtVao );
	ri.Hunk_FreeTempMemory( indexes );
	ri.Hunk_FreeTempMemory( verts );
}

/*
================
StageTiming

The system clock to a stage's age (Model_prt.cpp): ms since the stage
started (may be negative), and the stop time on the same clock.
================
*/
static int StageAge( const oaxPrtStage_t *stage, int sysAgeMs ) {
	return sysAgeMs - (int)( stage->timeOffset * 1000 );
}

/*
================
R_OAXPrtStageLiveCount

The particles of a stage drawn at this age, by the same rules the vertex
shader applies (for the debug counts and to skip idle stages). stopAgeMs
is on the stage clock, < 0 = never.
================
*/
static int PrtStageLiveCount( const oaxPrtStage_t *stage, int stageAge, int stopAge ) {
	int index, live = 0;
	int lifeMs = stage->particleLife * 1000;

	if ( !stage->cycleMsec || stage->totalParticles <= 0 || lifeMs <= 0 ) {
		return 0;
	}
	for ( index = 0; index < stage->totalParticles; index++ ) {
		int bunchOffset = lifeMs * stage->spawnBunching * index / stage->totalParticles;
		int particleAge = stageAge - bunchOffset;
		int particleCycle, inCycleTime;

		if ( particleAge < 0 ) {
			continue;
		}
		particleCycle = particleAge / stage->cycleMsec;
		if ( stage->cycles && particleCycle >= stage->cycles ) {
			continue;
		}
		inCycleTime = particleAge - particleCycle * stage->cycleMsec;
		if ( stopAge >= 0 && stageAge - inCycleTime >= stopAge ) {
			continue;
		}
		if ( inCycleTime > lifeMs ) {
			continue;
		}
		live++;
	}
	return live;
}

/*
================
R_OAXPrtSystemState

1 while some stage of the system has particles alive or still to be born
at timeMs, 0 once it is over.
================
*/
int R_OAXPrtSystemState( const oaxPrtDecl_t *decl, int timeMs, const oaxFx_t *fx ) {
	int age = timeMs - fx->startTime;

	if ( !decl ) {
		return 0;
	}
	if ( fx->stopTime && timeMs >= fx->stopTime ) {
		// stopped: alive until the last particle born before the stop dies
		int i, longest = 0;

		for ( i = 0; i < decl->numStages; i++ ) {
			longest = MAX( longest, (int)( decl->stages[i].particleLife * 1000 ) );
		}
		return timeMs < fx->stopTime + longest + 1;
	}
	if ( decl->durationMsec <= 0 ) {
		return 1;
	}
	return age < decl->durationMsec;
}

/*
================
R_OAXAddParticleSurfaces

Per view: a drawsurf for every live stage of every particle system in the
scene, in the stage material's sort.
================
*/
void R_OAXAddParticleSurfaces( void ) {
	int i, j;

	for ( i = 0; i < tr.refdef.oaxNumFx; i++ ) {
		oaxSceneFx_t *sfx = R_OAXSceneFx( tr.refdef.oaxFirstFx + i );

		if ( !sfx || !sfx->numSurfs ) {
			continue;
		}
		if ( R_CullBox( sfx->bounds ) == CULL_OUT ) {
			continue;
		}
		for ( j = 0; j < sfx->numSurfs; j++ ) {
			srfOaxParticles_t *surf = R_OAXPrtSurf( sfx->firstSurf + j );
			const oaxPrtStage_t *stage = &sfx->decl->stages[surf->stage];

			R_AddDrawSurf( (surfaceType_t *)surf, stage->material, 0, qfalse, qfalse, 0 );
		}
	}
}

/*
================
R_OAXPrtBuildSurfaces

From RE_BeginScene: the clock of each system added for this scene, and a
surface for each stage that has particles to draw now.
================
*/
void R_OAXPrtBuildSurfaces( oaxSceneFx_t *sfx ) {
	int i;
	float r;

	sfx->numSurfs = 0;
	sfx->firstSurf = -1;
	if ( !sfx->decl ) {
		return;
	}
	for ( i = 0; i < sfx->decl->numStages; i++ ) {
		const oaxPrtStage_t *stage = &sfx->decl->stages[i];
		int stageAge = StageAge( stage, sfx->timeMs - sfx->fx.startTime );
		int stopAge = sfx->fx.stopTime ? StageAge( stage, sfx->fx.stopTime - sfx->fx.startTime ) : -1;
		srfOaxParticles_t *surf;

		if ( !stage->material || !stage->cycleMsec || stage->totalParticles <= 0 ) {
			continue;
		}
		if ( stageAge < 0 ) {
			continue;
		}
		if ( stage->cycles > 0 && stageAge > stage->cycles * stage->cycleMsec + stage->particleLife * 1000 ) {
			continue;
		}
		if ( !PrtStageLiveCount( stage, stageAge, stopAge ) ) {
			continue;
		}
		surf = R_OAXPrtNewSurf( sfx );
		if ( !surf ) {
			break;
		}
		surf->stage = i;
	}
	r = sfx->decl->radius * ( sfx->fx.scale > 0 ? sfx->fx.scale : 1.0f );
	for ( i = 0; i < 3; i++ ) {
		sfx->bounds[0][i] = sfx->fx.origin[i] - r;
		sfx->bounds[1][i] = sfx->fx.origin[i] + r;
	}
}

/*
================
SeedHash

A 24-bit seed per system (exact in a float uniform).
================
*/
static unsigned int SeedHash( unsigned int x ) {
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x & 0xffffff;
}

static int SoftMode( const shaderStage_t *pStage ) {
	int src = pStage->stateBits & GLS_SRCBLEND_BITS;
	int dst = pStage->stateBits & GLS_DSTBLEND_BITS;

	// alpha blending fades by alpha; additive and modulating blends by colour
	if ( src == GLS_SRCBLEND_SRC_ALPHA && dst == GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA ) {
		return 1;
	}
	if ( ( src == GLS_SRCBLEND_DST_COLOR && dst == GLS_DSTBLEND_ZERO ) || ( src == GLS_SRCBLEND_ZERO && dst == GLS_DSTBLEND_SRC_COLOR ) ) {
		return 3;	// modulating: fade toward white, the blend's identity
	}
	return 2;
}

/*
================
RB_SurfaceOAXParticles

Draws one stage of one system straight from the static index buffer.
================
*/
#define PRT_PARMS 72

void RB_SurfaceOAXParticles( srfOaxParticles_t *surf ) {
	oaxSceneFx_t		*sfx = R_OAXSceneFx( surf->fx );
	const oaxPrtStage_t	*stage;
	shaderProgram_t		*sp = &tr.oaxParticleShader;
	shaderStage_t		*pStage;
	float				p[PRT_PARMS];
	int					stageAge, stopAge, lifeMs, baseCycle, inBase, live;
	float				scale, soft;
	vec4_t				viewInfo, tint;
	vec2_t				invRes;
	int					i;
	static GLint		locP = -2;
	static GLuint		locProgram;

	if ( !sfx || !sp->program || !prtVao || !r_oaxParticles->integer ) {
		return;
	}
	stage = &sfx->decl->stages[surf->stage];
	pStage = tess.shader ? tess.shader->stages[0] : NULL;
	if ( !pStage || !pStage->active ) {
		return;
	}
	if ( backEnd.depthFill || ( backEnd.viewParms.flags & ( VPF_SHADOWMAP | VPF_DEPTHSHADOW ) ) ) {
		return;
	}

	// anything batched with the same material goes out first
	RB_OAXFlushTess();

	stageAge = StageAge( stage, sfx->timeMs - sfx->fx.startTime );
	stopAge = sfx->fx.stopTime ? StageAge( stage, sfx->fx.stopTime - sfx->fx.startTime ) : -1;
	live = PrtStageLiveCount( stage, stageAge, stopAge );
	if ( !live ) {
		return;
	}
	lifeMs = stage->particleLife * 1000;
	baseCycle = stageAge / stage->cycleMsec;
	inBase = stageAge - baseCycle * stage->cycleMsec;
	scale = sfx->fx.scale > 0 ? sfx->fx.scale : 1.0f;
	if ( sfx->fx.rgba[0] || sfx->fx.rgba[1] || sfx->fx.rgba[2] || sfx->fx.rgba[3] ) {
		Vector4Copy( sfx->fx.rgba, tint );
	} else {
		VectorSet4( tint, 1, 1, 1, 1 );
	}

	if ( stage->lit && tr.world ) {
		vec3_t amb, dir, dirLight;

		// the light grid's ambient plus part of its directed light, 200 = a lit room
		if ( R_LightForPoint( sfx->fx.origin, amb, dirLight, dir ) ) {
			int k;

			for ( k = 0; k < 3; k++ ) {
				tint[k] *= Com_Clamp( 0.12f, 1.0f, ( amb[k] + 0.6f * dirLight[k] ) / 200.0f );
			}
		}
	}

	memset( p, 0, sizeof( p ) );
	p[0] = stage->totalParticles;
	p[1] = lifeMs;
	p[2] = stage->cycleMsec;
	p[3] = stage->spawnBunching;
	p[4] = baseCycle;
	p[5] = inBase;
	p[6] = stage->cycles;
	// the stop time relative to the start of the current cycle (1e9 = never)
	p[7] = stopAge < 0 ? 1e9f : (float)( stopAge - baseCycle * stage->cycleMsec );
	p[8] = stage->distributionType;
	p[9] = stage->randomDistribution;
	p[10] = stage->directionType;
	p[11] = stage->orientation;
	for ( i = 0; i < 4; i++ ) {
		p[12 + i] = stage->distributionParms[i];
	}
	p[16] = stage->directionParms[0];
	p[17] = stage->gravity;
	p[18] = stage->worldGravity;
	p[19] = stage->customPathType;
	for ( i = 0; i < 8; i++ ) {
		p[20 + i] = stage->customPathParms[i];
	}
	VectorCopy( stage->offset, p + 28 );
	p[31] = stage->initialAngle;
	p[32] = stage->speed.from;
	p[33] = stage->speed.to;
	p[34] = stage->rotationSpeed.from;
	p[35] = stage->rotationSpeed.to;
	p[36] = stage->size.from;
	p[37] = stage->size.to;
	p[38] = stage->aspect.from;
	p[39] = stage->aspect.to;
	for ( i = 0; i < 4; i++ ) {
		p[40 + i] = ( stage->entityColor ? 1.0f : stage->color[i] ) * tint[i];
		p[44 + i] = stage->fadeColor[i];
	}
	p[48] = stage->fadeInFraction;
	p[49] = stage->fadeOutFraction;
	p[50] = stage->fadeIndexFraction;
	p[51] = stage->animationFrames;
	p[52] = stage->animationRate;
	p[53] = SeedHash( (unsigned int)sfx->fx.seed * 2654435761U + surf->stage * 97U );
	p[54] = scale;
	p[55] = stage->orientationParms[1] > 0 ? stage->orientationParms[1] : 0.5f;	// aimed: trail time
	VectorCopy( sfx->fx.origin, p + 56 );
	VectorCopy( sfx->fx.axis[0], p + 60 );
	VectorCopy( sfx->fx.axis[1], p + 64 );
	VectorCopy( sfx->fx.axis[2], p + 68 );

	// soft particles: the stage's distance, or the cvar default; never in
	// reflection views (oblique projection) or without a scene copy
	soft = stage->softDistance;
	if ( soft == 0 ) {
		soft = r_oaxSoftParticles->value;
	}
	if ( soft > 0 && ( backEnd.viewParms.oaxReflection || backEnd.viewParms.isPortal || !r_oaxSoftParticles->value || !RB_OAXSceneCopy() ) ) {
		soft = 0;
	}

	GLSL_BindProgram( sp );
	if ( locP == -2 || locProgram != sp->program ) {
		locP = qglGetUniformLocation( sp->program, "u_P" );
		locProgram = sp->program;
	}
	qglUniform1fv( locP, PRT_PARMS, p );

	GLSL_SetUniformMat4( sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection );
	GLSL_SetUniformVec3( sp, UNIFORM_VIEWORIGIN, backEnd.viewParms.or.origin );
	GLSL_SetUniformVec3( sp, UNIFORM_VIEWFORWARD, backEnd.viewParms.or.axis[0] );
	GLSL_SetUniformVec3( sp, UNIFORM_VIEWLEFT, backEnd.viewParms.or.axis[1] );
	GLSL_SetUniformVec3( sp, UNIFORM_VIEWUP, backEnd.viewParms.or.axis[2] );
	// heat haze needs the scene colour copy; without it (reflection views, portals, no
	// copy) the stage draws as an ordinary blend of its texture
	if ( stage->distort > 0 && !backEnd.viewParms.oaxReflection && !backEnd.viewParms.isPortal && RB_OAXSceneCopy() ) {
		image_t *col = RB_OAXSceneColor();

		VectorSet4( viewInfo, r_znear->value, backEnd.viewParms.zFar, stage->distort, 4 );
		GLSL_SetUniformVec4( sp, UNIFORM_VIEWINFO, viewInfo );
		invRes[0] = 1.0f / col->width;
		invRes[1] = 1.0f / col->height;
		GL_BindToTMU( col, TB_LIGHTMAP );
	} else {
	VectorSet4( viewInfo, r_znear->value, backEnd.viewParms.zFar, soft, soft > 0 ? SoftMode( pStage ) : 0 );
	GLSL_SetUniformVec4( sp, UNIFORM_VIEWINFO, viewInfo );
	if ( soft > 0 ) {
		image_t *depth = RB_OAXSceneDepth();

		invRes[0] = 1.0f / depth->width;
		invRes[1] = 1.0f / depth->height;
		GL_BindToTMU( depth, TB_SHADOWMAP );
	} else {
		invRes[0] = invRes[1] = 0;
	}
	}
	GLSL_SetUniformVec2( sp, UNIFORM_INVTEXRES, invRes );

	R_BindAnimatedImageToTMU( &pStage->bundle[0], TB_COLORMAP );
	GL_State( ( pStage->stateBits & ~( GLS_DEPTHMASK_TRUE | GLS_ATEST_BITS ) ) );
	GL_Cull( CT_TWO_SIDED );

	R_BindVao( prtVao );
	qglDrawElements( GL_TRIANGLES, stage->totalParticles * 6, GL_UNSIGNED_SHORT, 0 );

	oaxFxStats.prtStagesDrawn++;
	oaxFxStats.particlesDrawn += live;
	backEnd.pc.c_totalIndexes += stage->totalParticles * 6;
}
