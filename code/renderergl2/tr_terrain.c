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
tr_terrain.c: heightmap terrain and instanced foliage (phase 7).

Data: the map's OAX_TERRAIN lump (qcommon/oax_terrain.h), parsed with the
same code collision uses, so the full-detail mesh is exactly the collision
surface (same samples, same (i,j)-(i+1,j+1) diagonal).

Terrain: chunks of TCHUNK x TCHUNK cells, each with its own static vertices
(position + normal). Level of detail is geomipmapping: a chunk at level L
draws every 2^L-th sample. Neighbouring chunks differ by at most one level
(relaxed every frame), and a chunk whose neighbour is coarser draws a
"stitched" index pattern that snaps the edge's odd vertices onto the even
ones, so both chunks share exactly the same edge segments: no cracks, no
T-junctions. All patterns live in one static index buffer.

Shading: up to four layer textures blended by the splat map, lit by the sky
shader's sun (q3map_sun / q3gl2_sun) plus ambient, multiplied by the screen
space sun shadow mask (the cascaded sun shadow maps) when it is on.

Foliage: grass and trees from OAXTerrain_CellFoliage (the same deterministic
placement collision uses for tree trunks), stored per chunk in one instance
buffer and drawn with glDrawElementsInstanced (per-instance attributes with
divisor 1). Distance fade is an ordered dither, so there is no pop.

Passes (tr_backend.c RB_DrawSurfs): terrain and foliage depth are drawn
FIRST in each view's depth prepass (main views and the sun shadow cascades),
the occlusion queries are issued right after them against terrain-only
depth, and the colour pass draws them before the stock surface list.

Occlusion (main views only): each frustum-visible chunk gets an
ANY_SAMPLES_PASSED(_CONSERVATIVE) query of its bounds inflated by
r_oaxOcclusionMargin. Results arrive a frame or more later (WebGL2 rule), so
a chunk is skipped only while: its latest result says hidden, the query box
was entirely on screen and in front of the near plane when issued (a box
partly off screen tests nothing there), and the camera has moved less than
a quarter of the margin since. Anything unknown is drawn. Only terrain
depth is in the buffer when queries run, so moving entities never occlude.
===========================================================================
*/

#include "tr_local.h"
#include "../qcommon/oax_terrain.h"

extern const char *fallbackShader_oaxterrain_vp;
extern const char *fallbackShader_oaxterrain_fp;
extern const char *fallbackShader_oaxfoliage_vp;
extern const char *fallbackShader_oaxfoliage_fp;

#define TCHUNK			16
#define TLODS			5		// steps 1, 2, 4, 8, 16
#define TCHUNK_VERTS	( ( TCHUNK + 1 ) * ( TCHUNK + 1 ) )
#define MAX_TVIEWS		32
#define TVERT_STRIDE	24		// float3 position, float3 normal
#define FVERT_STRIDE	32		// float3 position, float3 normal, float2 uv
#define FINST_STRIDE	32		// float4 origin+scale, float4 cos sin rand kind

cvar_t *r_oaxTerrain;
cvar_t *r_oaxTerrainLodDist;
cvar_t *r_oaxTerrainDebug;
cvar_t *r_oaxOcclusion;
cvar_t *r_oaxOcclusionMargin;
cvar_t *r_oaxFoliage;
cvar_t *r_oaxShadowOffset;

#define TFOL_MAX_PARTS		32		// surfaces over all variants of a foliage model
#define TFOL_MAX_VARIANTS	8

typedef struct {
	int			first, count;		// indexes (GL_UNSIGNED_INT) in modelIbo
	int			variant;
	image_t		*image;
} tFoliagePart_t;

typedef struct {
	int			terrain;
	int			ci, cj;
	vec3_t		bounds[2];			// surface and foliage
	int			vertBase;			// first vertex in the terrain buffer
	int			instFirst[OAX_TERRAIN_MAX_FOLIAGE];
	int			instCount[OAX_TERRAIN_MAX_FOLIAGE];
	int			varCount[OAX_TERRAIN_MAX_FOLIAGE][TFOL_MAX_VARIANTS];	// model foliage: instances per variant, in order
	int			lod, lodFrame;
	// occlusion
	int			query;				// index + 1 into tw.queries, 0 none
	qboolean	pending;
	vec3_t		queryEye;
	qboolean	resultValid, hidden;
	vec3_t		resultEye;
	qboolean	frozen;				// r_oaxOcclusion 2 (control): first result kept forever
} tChunk_t;



typedef struct {
	oaxTerrainInfo_t info;
	int			chunksX, chunksY;
	tChunk_t	*chunks;
	GLuint		vbo;
	GLuint		vao;
	image_t		*layers[OAX_TERRAIN_MAX_LAYERS];
	image_t		*splat;
	image_t		*folImage[OAX_TERRAIN_MAX_FOLIAGE];
	GLuint		instVbo;
	int			numInstances;
	GLuint		folVao;
	// model foliage (OAX_FOLIAGE_MODEL): every model's surfaces in one buffer,
	// normalised to unit height with the base at z 0 (instance scale = height)
	GLuint		modelVbo, modelIbo, modelVao;
	int			numParts[OAX_TERRAIN_MAX_FOLIAGE];
	tFoliagePart_t parts[OAX_TERRAIN_MAX_FOLIAGE][TFOL_MAX_PARTS];
} tTerrain_t;

typedef struct {
	tChunk_t	*chunk;
	byte		lod, mask;
} tViewChunk_t;

typedef struct {
	int			num;
	tViewChunk_t *list;
	qboolean	main;			// a player view: occlusion applies
	qboolean	shadow;			// a sun shadow cascade
	vec3_t		eye;
} tView_t;

static struct {
	qboolean	loaded;
	int			numTerrains;
	tTerrain_t	terrains[OAX_TERRAIN_MAX];
	int			totalChunks;
	// index patterns: [lod][mask] -> first index, count
	GLuint		ibo;
	int			patFirst[TLODS][16], patCount[TLODS][16];
	// foliage meshes
	GLuint		meshVbo, meshIbo;
	int			meshFirst[2], meshCount[2];
	// occlusion boxes
	GLuint		boxVbo, boxIbo, boxVao;
	// programs
	shaderProgram_t	terrainProg[3];		// depth, colour, crack test
	shaderProgram_t	foliageProg[2];		// depth, colour
	qboolean	progsOk;
	tView_t		views[MAX_TVIEWS];
	int			viewSeq;
	// query objects (outside the hunk, so they can be freed after it is cleared)
	GLuint		queries[4096];
	int			numQueries;
	// stats of the last main view
	int			statVisible, statDrawn, statOccluded, statQueries, statFoliage, statTris;
	int			statLods, statStitched, statGrass, statTrees;
	int			statShadowChunks, statShadowViews;	// accumulated since the last main colour pass
	int			statFoliageTotal;
	int			statStitchOk;
} tw;

static int		TerrainLocs[3][16];
static int		FoliageLocs[2][16];

enum { TU_LAYERSCALE, TU_SPLATXFORM, TU_SUNDIR, TU_SUNCOLOR, TU_AMBIENT, TU_SCREEN, TU_DEBUG, TU_TRIPLANAR,
	TU_SURFACEFX, TU_VIEWORIGIN, TU_LAYER0, TU_LAYER1, TU_LAYER2, TU_LAYER3, TU_SPLAT, TU_SHADOW, TU_NUM };
enum { FU_FADE, FU_VIEWORIGIN, FU_SUNDIR, FU_SUNCOLOR, FU_AMBIENT, FU_SCREEN, FU_TEX, FU_SHADOW, FU_WIND, FU_TIME, FU_A2C, FU_NUM };

void R_OAXTerrainRegisterCvars( void ) {
	r_oaxTerrain = ri.Cvar_Get( "r_oaxTerrain", "1", CVAR_CHEAT );
	ri.Cvar_SetDescription( r_oaxTerrain, "Draw oax heightmap terrain (0 hides it; collision is unaffected)." );
	r_oaxTerrainLodDist = ri.Cvar_Get( "r_oaxTerrainLodDist", "1200", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_oaxTerrainLodDist, "Distance at which terrain chunks drop to the next level of detail (0: always full detail)." );
	r_oaxTerrainDebug = ri.Cvar_Get( "r_oaxTerrainDebug", "0", CVAR_CHEAT );
	ri.Cvar_SetDescription( r_oaxTerrainDebug, "Terrain test modes: 1 flat green (crack test), 2 flat green without edge stitching (must crack), 3 terrain and foliage cast no sun shadows, 4 terrain shows the sun shadow mask, 5 both 3 and 4." );
	r_oaxOcclusion = ri.Cvar_Get( "r_oaxOcclusion", "1", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_oaxOcclusion, "Occlusion queries for terrain chunks and foliage: 0 off, 1 conservative, 2 frozen first results (a broken mode for tests)." );
	r_oaxOcclusionMargin = ri.Cvar_Get( "r_oaxOcclusionMargin", "96", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_oaxOcclusionMargin, "Units occlusion query boxes are inflated by; results are dropped once the camera moves a quarter of it." );
	r_oaxShadowOffset = ri.Cvar_Get( "r_oaxShadowOffset", "2", CVAR_CHEAT );
	ri.Cvar_SetDescription( r_oaxShadowOffset, "Polygon offset (factor; units are twice it) of terrain drawn into sun shadow maps." );
	r_oaxFoliage = ri.Cvar_Get( "r_oaxFoliage", "1", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_oaxFoliage, "Draw instanced terrain foliage." );
}

// ---- programs -------------------------------------------------------------------------

static void InitPrograms( void ) {
	static const char *tnames[TU_NUM] = { "u_LayerScale", "u_SplatXform", "u_SunDir", "u_SunColor", "u_Ambient", "u_ScreenInfo", "u_Debug", "u_Triplanar",
		"u_SurfaceFx", "u_ViewOrigin", "u_Layer0", "u_Layer1", "u_Layer2", "u_Layer3", "u_Splat", "u_ScreenShadow" };
	static const char *fnames[FU_NUM] = { "u_Fade", "u_ViewOrigin", "u_SunDir", "u_SunColor", "u_Ambient", "u_ScreenInfo", "u_Tex", "u_ScreenShadow", "u_Wind", "u_Time", "u_A2C" };
	int i, k;

	tw.progsOk = qfalse;
	for ( i = 0; i < 3; i++ ) {
		const char *extra = i == 0 ? "#define TERRAIN_DEPTH\n" : i == 2 ? "#define TERRAIN_FLAT\n" : "";
		if ( !GLSL_InitOAXShader( &tw.terrainProg[i], "oaxterrain", ATTR_POSITION | ATTR_NORMAL, extra,
			fallbackShader_oaxterrain_vp, fallbackShader_oaxterrain_fp ) ) {
			return;
		}
		for ( k = 0; k < TU_NUM; k++ ) {
			TerrainLocs[i][k] = qglGetUniformLocation( tw.terrainProg[i].program, tnames[k] );
		}
	}
	for ( i = 0; i < 2; i++ ) {
		if ( !GLSL_InitOAXShader( &tw.foliageProg[i], "oaxfoliage", ATTR_POSITION | ATTR_NORMAL | ATTR_TEXCOORD | ATTR_POSITION2 | ATTR_NORMAL2,
			i == 0 ? "#define FOLIAGE_DEPTH\n" : "", fallbackShader_oaxfoliage_vp, fallbackShader_oaxfoliage_fp ) ) {
			return;
		}
		for ( k = 0; k < FU_NUM; k++ ) {
			FoliageLocs[i][k] = qglGetUniformLocation( tw.foliageProg[i].program, fnames[k] );
		}
	}
	tw.progsOk = qtrue;
}

// ---- static index patterns ---------------------------------------------------------------

// mask bits: 1 = -y edge, 2 = +x edge, 4 = +y edge, 8 = -x edge has a coarser neighbour
static int SnapIndex( int x, int y, int s, int mask, qboolean stitch ) {
	if ( stitch ) {
		if ( ( mask & 1 ) && y == 0 && ( x / s ) % 2 == 1 ) x -= s;
		else if ( ( mask & 4 ) && y == TCHUNK && ( x / s ) % 2 == 1 ) x -= s;
		else if ( ( mask & 8 ) && x == 0 && ( y / s ) % 2 == 1 ) y -= s;
		else if ( ( mask & 2 ) && x == TCHUNK && ( y / s ) % 2 == 1 ) y -= s;
	}
	return y * ( TCHUNK + 1 ) + x;
}

// a triangle of chunk-local vertex indices, unless it has no area
static int EmitTri( unsigned short *out, int a, int b, int c ) {
	int ax = a % ( TCHUNK + 1 ), ay = a / ( TCHUNK + 1 ), bx = b % ( TCHUNK + 1 ), by = b / ( TCHUNK + 1 );
	int cx = c % ( TCHUNK + 1 ), cy = c / ( TCHUNK + 1 );

	if ( ( bx - ax ) * ( cy - ay ) - ( by - ay ) * ( cx - ax ) == 0 ) {
		return 0;
	}
	out[0] = a; out[1] = b; out[2] = c;
	return 3;
}

static int BuildPattern( unsigned short *out, int lod, int mask, qboolean stitch ) {
	int s = 1 << lod, x, y, n = 0;

	for ( y = 0; y < TCHUNK; y += s ) {
		for ( x = 0; x < TCHUNK; x += s ) {
			int a = SnapIndex( x, y, s, mask, stitch ), b = SnapIndex( x + s, y, s, mask, stitch );
			int c = SnapIndex( x, y + s, s, mask, stitch ), d = SnapIndex( x + s, y + s, s, mask, stitch );
			// (a, b, d) and (a, d, c): the collision split; degenerate ones dropped
			n += EmitTri( out + n, a, b, d );
			n += EmitTri( out + n, a, d, c );
		}
	}
	return n;
}

// 2D signed area check of every pattern: the stitched triangles must tile the
// chunk exactly (sum of areas == chunk area, none inverted)
static qboolean CheckPattern( const unsigned short *idx, int n ) {
	double sum = 0;
	int k;

	for ( k = 0; k < n; k += 3 ) {
		int a = idx[k], b = idx[k + 1], c = idx[k + 2];
		double ax = a % ( TCHUNK + 1 ), ay = a / ( TCHUNK + 1 );
		double bx = b % ( TCHUNK + 1 ), by = b / ( TCHUNK + 1 );
		double cx = c % ( TCHUNK + 1 ), cy = c / ( TCHUNK + 1 );
		double area = ( ( bx - ax ) * ( cy - ay ) - ( by - ay ) * ( cx - ax ) ) * 0.5;
		if ( area < 0 ) {
			return qfalse;
		}
		sum += area;
	}
	return sum == (double)( TCHUNK * TCHUNK );
}

static void BuildPatterns( void ) {
	unsigned short *all = ri.Hunk_AllocateTempMemory( TLODS * 16 * TCHUNK * TCHUNK * 6 * sizeof( unsigned short ) * 2 );
	int lod, mask, total = 0, ok = 1;

	for ( lod = 0; lod < TLODS; lod++ ) {
		for ( mask = 0; mask < 16; mask++ ) {
			int n = BuildPattern( all + total, lod, mask, qtrue );
			// the coarsest level never has a coarser neighbour
			if ( ( lod < TLODS - 1 || !mask ) && !CheckPattern( all + total, n ) ) {
				ok = 0;
			}
			tw.patFirst[lod][mask] = total;
			tw.patCount[lod][mask] = n;
			total += n;
		}
	}
	// debug: the same patterns without stitching (r_oaxTerrainDebug 2)
	for ( lod = 0; lod < TLODS; lod++ ) {
		int n = BuildPattern( all + total, lod, 0, qfalse );
		tw.patFirst[lod][0] |= 0;	// unchanged
		// stored after the stitched set: lod-major, one per lod
		all[total + n] = 0;
		total += n;
	}
	tw.statStitchOk = ok;
	qglGenBuffers( 1, &tw.ibo );
	qglBindBuffer( GL_ELEMENT_ARRAY_BUFFER, tw.ibo );
	qglBufferData( GL_ELEMENT_ARRAY_BUFFER, total * sizeof( unsigned short ), all, GL_STATIC_DRAW );
	qglBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );
	ri.Hunk_FreeTempMemory( all );
}

// first index of the unstitched pattern of a level (after the stitched set)
static int UnstitchedFirst( int lod ) {
	int l, first = tw.patFirst[TLODS - 1][15] + tw.patCount[TLODS - 1][15];

	for ( l = 0; l < lod; l++ ) {
		first += tw.patCount[l][0];
	}
	return first;
}

// ---- foliage meshes ------------------------------------------------------------------------

typedef struct {
	float	*v;		// FVERT_STRIDE / 4 floats each
	unsigned short *i;
	int		nv, ni;
} meshBuild_t;

static int MeshVert( meshBuild_t *m, float x, float y, float z, float nx, float ny, float nz, float u, float t ) {
	float *p = m->v + m->nv * 8;
	p[0] = x; p[1] = y; p[2] = z; p[3] = nx; p[4] = ny; p[5] = nz; p[6] = u; p[7] = t;
	return m->nv++;
}

static void MeshTri( meshBuild_t *m, int a, int b, int c ) {
	m->i[m->ni++] = a; m->i[m->ni++] = b; m->i[m->ni++] = c;
}

// grass: three crossed blades quads, unit height, base at z 0
static void BuildGrass( meshBuild_t *m ) {
	int k;

	for ( k = 0; k < 3; k++ ) {
		float a = k * M_PI / 3.0f, c = cos( a ) * 0.35f, s = sin( a ) * 0.35f;
		int base = m->nv;
		// normals lean up so blades light like the ground under them
		MeshVert( m, -c, -s, 0, 0, 0, 1, 0, 1 );
		MeshVert( m, c, s, 0, 0, 0, 1, 1, 1 );
		MeshVert( m, c * 1.1f, s * 1.1f, 1, 0, 0, 1, 1, 0 );
		MeshVert( m, -c * 1.1f, -s * 1.1f, 1, 0, 0, 1, 0, 0 );
		MeshTri( m, base, base + 1, base + 2 );
		MeshTri( m, base, base + 2, base + 3 );
	}
}

// tree: a six-sided trunk and three stacked cones, unit height; the texture
// atlas has bark in u 0..0.25 and foliage in u 0.25..1
static void BuildTree( meshBuild_t *m ) {
	const int sides = 8;
	int k, c;
	static const float cone[3][3] = { { 0.22f, 0.30f, 0.65f }, { 0.45f, 0.24f, 0.85f }, { 0.65f, 0.16f, 1.0f } };	// base z, radius, top z

	for ( k = 0; k <= 6; k++ ) {
		float a = k * 2.0f * M_PI / 6.0f, x = cos( a ), y = sin( a );
		MeshVert( m, x * 0.035f, y * 0.035f, -0.05f, x, y, 0, k / 6.0f * 0.25f, 1 );
		MeshVert( m, x * 0.022f, y * 0.022f, 0.5f, x, y, 0, k / 6.0f * 0.25f, 0 );
		if ( k ) {
			int b = m->nv - 4;
			MeshTri( m, b, b + 2, b + 3 );
			MeshTri( m, b, b + 3, b + 1 );
		}
	}
	for ( c = 0; c < 3; c++ ) {
		float z0 = cone[c][0], r = cone[c][1], z1 = cone[c][2];
		float slope = r / ( z1 - z0 ), nl = 1.0f / sqrt( 1.0f + slope * slope );
		int base = m->nv, top;
		for ( k = 0; k <= sides; k++ ) {
			float a = k * 2.0f * M_PI / sides, x = cos( a ), y = sin( a );
			MeshVert( m, x * r, y * r, z0, x * nl, y * nl, slope * nl, 0.25f + 0.75f * k / sides, 1 );
		}
		top = MeshVert( m, 0, 0, z1, 0, 0, 1, 0.625f, 0 );
		for ( k = 0; k < sides; k++ ) {
			MeshTri( m, base + k, base + k + 1, top );
		}
		// underside
		top = MeshVert( m, 0, 0, z0 + 0.02f, 0, 0, -1, 0.625f, 0.5f );
		for ( k = 0; k < sides; k++ ) {
			int b0 = MeshVert( m, cos( k * 2.0f * M_PI / sides ) * r, sin( k * 2.0f * M_PI / sides ) * r, z0, 0, 0, -1, 0.3f, 1 );
			int b1 = MeshVert( m, cos( ( k + 1 ) * 2.0f * M_PI / sides ) * r, sin( ( k + 1 ) * 2.0f * M_PI / sides ) * r, z0, 0, 0, -1, 0.9f, 1 );
			MeshTri( m, b1, b0, top );
		}
	}
}

static void BuildMeshes( void ) {
	meshBuild_t m;

	m.v = ri.Hunk_AllocateTempMemory( 1024 * FVERT_STRIDE );
	m.i = ri.Hunk_AllocateTempMemory( 4096 * sizeof( unsigned short ) );
	m.nv = m.ni = 0;
	tw.meshFirst[0] = m.ni;
	BuildGrass( &m );
	tw.meshCount[0] = m.ni - tw.meshFirst[0];
	tw.meshFirst[1] = m.ni;
	BuildTree( &m );
	tw.meshCount[1] = m.ni - tw.meshFirst[1];
	qglGenBuffers( 1, &tw.meshVbo );
	qglBindBuffer( GL_ARRAY_BUFFER, tw.meshVbo );
	qglBufferData( GL_ARRAY_BUFFER, m.nv * FVERT_STRIDE, m.v, GL_STATIC_DRAW );
	qglGenBuffers( 1, &tw.meshIbo );
	qglBindBuffer( GL_ELEMENT_ARRAY_BUFFER, tw.meshIbo );
	qglBufferData( GL_ELEMENT_ARRAY_BUFFER, m.ni * sizeof( unsigned short ), m.i, GL_STATIC_DRAW );
	qglBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );
	qglBindBuffer( GL_ARRAY_BUFFER, 0 );
	ri.Hunk_FreeTempMemory( m.i );
	ri.Hunk_FreeTempMemory( m.v );
}

// ---- load ------------------------------------------------------------------------------------

static image_t *ShaderImage( const char *name ) {
	shader_t *sh;

	if ( !name[0] ) {
		return NULL;
	}
	sh = R_FindShader( name, LIGHTMAP_NONE, qtrue );
	if ( sh && !sh->defaultShader && sh->stages[0] && sh->stages[0]->bundle[0].image[0] ) {
		return sh->stages[0]->bundle[0].image[0];
	}
	ri.Printf( PRINT_WARNING, "terrain: no image for %s\n", name );
	return tr.whiteImage;
}

/*
FoliageVariants: how many models an OAX_FOLIAGE_MODEL type draws; an
instance's variant comes from its position, so every build picks the same.
*/
static int FoliageVariants( const oaxFoliageDisk_t *fd ) {
	int n = (int)fd->variants;
	if ( fd->kind != OAX_FOLIAGE_MODEL || n < 1 ) {
		return 1;
	}
	return n > TFOL_MAX_VARIANTS ? TFOL_MAX_VARIANTS : n;
}

static int FoliageVariantOf( const oaxFoliageInstance_t *fi, int numVariants ) {
	unsigned h = (unsigned)(int)fi->origin[0] * 73856093U ^ (unsigned)(int)fi->origin[1] * 19349663U;
	h ^= h >> 13;
	h *= 0x5bd1e995U;
	h ^= h >> 15;
	return numVariants > 1 ? (int)( h % (unsigned)numVariants ) : 0;
}

/*
FoliageModelName: variant v of a foliage model: the name itself for one
variant, else <name>_<v + 1>.md3 ("models/x/pine.md3" -> "models/x/pine_2.md3").
*/
static void FoliageModelName( const oaxFoliageDisk_t *fd, int v, char *out, int size ) {
	char base[MAX_QPATH];
	if ( FoliageVariants( fd ) <= 1 ) {
		Q_strncpyz( out, fd->shader, size );
		return;
	}
	COM_StripExtension( fd->shader, base, sizeof( base ) );
	Com_sprintf( out, size, "%s_%d.md3", base, v + 1 );
}

/*
LoadFoliageModels: the surfaces of every variant of every OAX_FOLIAGE_MODEL
type, from LOD 0, frame 0 of each MD3, into one vertex/index buffer per
terrain with a VAO that reads the per-instance data from instVbo like folVao
does. Each variant is scaled to unit height with its base at z 0, so an
instance's scale is its height in world units.
*/
static void LoadFoliageModels( tTerrain_t *t ) {
	const oaxTerrainInfo_t *in = &t->info;
	int f, s, v, vv, totalVerts = 0, totalIdx = 0, nv = 0, ni = 0;
	float *verts;
	unsigned int *idx;
	mdvModel_t *mdv[OAX_TERRAIN_MAX_FOLIAGE][TFOL_MAX_VARIANTS];

	Com_Memset( mdv, 0, sizeof( mdv ) );
	for ( f = 0; f < in->numFoliage; f++ ) {
		if ( in->foliage[f].kind != OAX_FOLIAGE_MODEL ) {
			continue;
		}
		for ( vv = 0; vv < FoliageVariants( &in->foliage[f] ); vv++ ) {
			char name[MAX_QPATH];
			model_t *mod;
			FoliageModelName( &in->foliage[f], vv, name, sizeof( name ) );
			mod = R_GetModelByHandle( RE_RegisterModel( name ) );
			if ( !mod || mod->type != MOD_MESH || !mod->mdv[0] ) {
				ri.Printf( PRINT_WARNING, "terrain: foliage model %s not loaded\n", name );
				continue;
			}
			mdv[f][vv] = mod->mdv[0];
			for ( s = 0; s < mdv[f][vv]->numSurfaces; s++ ) {
				totalVerts += mdv[f][vv]->surfaces[s].numVerts;
				totalIdx += mdv[f][vv]->surfaces[s].numIndexes;
			}
		}
	}
	if ( !totalVerts ) {
		return;
	}
	verts = ri.Hunk_AllocateTempMemory( totalVerts * FVERT_STRIDE );
	idx = ri.Hunk_AllocateTempMemory( totalIdx * sizeof( unsigned int ) );
	for ( f = 0; f < in->numFoliage; f++ ) {
		for ( vv = 0; vv < TFOL_MAX_VARIANTS; vv++ ) {
			const mdvModel_t *m = mdv[f][vv];
			float zmin = 1e30f, zmax = -1e30f, h;
			if ( !m ) {
				continue;
			}
			for ( s = 0; s < m->numSurfaces; s++ ) {
				for ( v = 0; v < m->surfaces[s].numVerts; v++ ) {
					zmin = MIN( zmin, m->surfaces[s].verts[v].xyz[2] );
					zmax = MAX( zmax, m->surfaces[s].verts[v].xyz[2] );
				}
			}
			h = zmax > zmin ? zmax - zmin : 1.0f;
			for ( s = 0; s < m->numSurfaces && t->numParts[f] < TFOL_MAX_PARTS; s++ ) {
				const mdvSurface_t *surf = &m->surfaces[s];
				tFoliagePart_t *p = &t->parts[f][t->numParts[f]++];
				shader_t *sh = surf->numShaderIndexes ? R_GetShaderByHandle( surf->shaderIndexes[0] ) : tr.defaultShader;
				int base = nv;
				p->image = sh && !sh->defaultShader && sh->stages[0] && sh->stages[0]->bundle[0].image[0] ? sh->stages[0]->bundle[0].image[0] : tr.whiteImage;
				p->variant = vv;
				p->first = ni;
				p->count = surf->numIndexes;
				for ( v = 0; v < surf->numVerts; v++ ) {
					float *o = verts + nv * ( FVERT_STRIDE / 4 );
					const mdvVertex_t *mv = &surf->verts[v];
					o[0] = mv->xyz[0] / h;
					o[1] = mv->xyz[1] / h;
					o[2] = ( mv->xyz[2] - zmin ) / h;
					o[3] = mv->normal[0] / 32767.0f;
					o[4] = mv->normal[1] / 32767.0f;
					o[5] = mv->normal[2] / 32767.0f;
					o[6] = surf->st[v].st[0];
					o[7] = surf->st[v].st[1];
					nv++;
				}
				for ( v = 0; v < surf->numIndexes; v++ ) {
					idx[ni++] = base + surf->indexes[v];
				}
			}
		}
	}
	qglGenBuffers( 1, &t->modelVbo );
	qglBindBuffer( GL_ARRAY_BUFFER, t->modelVbo );
	qglBufferData( GL_ARRAY_BUFFER, nv * FVERT_STRIDE, verts, GL_STATIC_DRAW );
	qglGenBuffers( 1, &t->modelIbo );
	qglGenVertexArrays( 1, &t->modelVao );
	qglBindVertexArray( t->modelVao );
	qglBindBuffer( GL_ELEMENT_ARRAY_BUFFER, t->modelIbo );
	qglBufferData( GL_ELEMENT_ARRAY_BUFFER, ni * sizeof( unsigned int ), idx, GL_STATIC_DRAW );
	qglEnableVertexAttribArray( ATTR_INDEX_POSITION );
	qglEnableVertexAttribArray( ATTR_INDEX_NORMAL );
	qglEnableVertexAttribArray( ATTR_INDEX_TEXCOORD );
	qglVertexAttribPointer( ATTR_INDEX_POSITION, 3, GL_FLOAT, GL_FALSE, FVERT_STRIDE, BUFFER_OFFSET( 0 ) );
	qglVertexAttribPointer( ATTR_INDEX_NORMAL, 3, GL_FLOAT, GL_FALSE, FVERT_STRIDE, BUFFER_OFFSET( 12 ) );
	qglVertexAttribPointer( ATTR_INDEX_TEXCOORD, 2, GL_FLOAT, GL_FALSE, FVERT_STRIDE, BUFFER_OFFSET( 24 ) );
	qglBindBuffer( GL_ARRAY_BUFFER, t->instVbo );
	qglEnableVertexAttribArray( ATTR_INDEX_POSITION2 );
	qglEnableVertexAttribArray( ATTR_INDEX_NORMAL2 );
	qglVertexAttribPointer( ATTR_INDEX_POSITION2, 4, GL_FLOAT, GL_FALSE, FINST_STRIDE, BUFFER_OFFSET( 0 ) );
	qglVertexAttribPointer( ATTR_INDEX_NORMAL2, 4, GL_FLOAT, GL_FALSE, FINST_STRIDE, BUFFER_OFFSET( 16 ) );
	qglVertexAttribDivisor( ATTR_INDEX_POSITION2, 1 );
	qglVertexAttribDivisor( ATTR_INDEX_NORMAL2, 1 );
	qglBindVertexArray( 0 );
	qglBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );
	qglBindBuffer( GL_ARRAY_BUFFER, 0 );
	ri.Hunk_FreeTempMemory( idx );
	ri.Hunk_FreeTempMemory( verts );
}

static void LoadTerrain( tTerrain_t *t ) {
	const oaxTerrainInfo_t *in = &t->info;
	int sx = in->samplesX, sy = in->samplesY, ci, cj, f, k;
	float *verts;
	byte *splat;
	float *inst;
	int maxInst = 0, numInst = 0;

	t->chunksX = ( sx - 1 + TCHUNK - 1 ) / TCHUNK;
	t->chunksY = ( sy - 1 + TCHUNK - 1 ) / TCHUNK;
	t->chunks = ri.Hunk_Alloc( t->chunksX * t->chunksY * sizeof( tChunk_t ), h_low );

	// vertices, chunk by chunk (each chunk owns its border vertices)
	verts = ri.Hunk_AllocateTempMemory( t->chunksX * t->chunksY * TCHUNK_VERTS * TVERT_STRIDE );
	for ( cj = 0; cj < t->chunksY; cj++ ) {
		for ( ci = 0; ci < t->chunksX; ci++ ) {
			tChunk_t *c = &t->chunks[cj * t->chunksX + ci];
			int x, y;
			c->terrain = t - tw.terrains;
			c->ci = ci;
			c->cj = cj;
			c->vertBase = ( cj * t->chunksX + ci ) * TCHUNK_VERTS;
			ClearBounds( c->bounds[0], c->bounds[1] );
			for ( y = 0; y <= TCHUNK; y++ ) {
				for ( x = 0; x <= TCHUNK; x++ ) {
					// samples past the grid edge clamp (a degenerate strip, never seen)
					int i = ci * TCHUNK + x, j = cj * TCHUNK + y;
					float *v = verts + ( c->vertBase + y * ( TCHUNK + 1 ) + x ) * 6;
					int ii = i > sx - 1 ? sx - 1 : i, jj = j > sy - 1 ? sy - 1 : j;
					vec3_t n;
					v[0] = in->origin[0] + (float)ii * in->cellSize;
					v[1] = in->origin[1] + (float)jj * in->cellSize;
					v[2] = OAXTerrain_SampleZ( in, ii, jj );
					n[0] = ( OAXTerrain_SampleZ( in, ii - 1, jj ) - OAXTerrain_SampleZ( in, ii + 1, jj ) );
					n[1] = ( OAXTerrain_SampleZ( in, ii, jj - 1 ) - OAXTerrain_SampleZ( in, ii, jj + 1 ) );
					n[2] = 2.0f * in->cellSize;
					VectorNormalize( n );
					VectorCopy( n, v + 3 );
					AddPointToBounds( v, c->bounds[0], c->bounds[1] );
				}
			}
		}
	}
	qglGenBuffers( 1, &t->vbo );
	qglBindBuffer( GL_ARRAY_BUFFER, t->vbo );
	qglBufferData( GL_ARRAY_BUFFER, t->chunksX * t->chunksY * TCHUNK_VERTS * TVERT_STRIDE, verts, GL_STATIC_DRAW );
	ri.Hunk_FreeTempMemory( verts );

	qglGenVertexArrays( 1, &t->vao );
	qglBindVertexArray( t->vao );
	qglBindBuffer( GL_ARRAY_BUFFER, t->vbo );
	qglBindBuffer( GL_ELEMENT_ARRAY_BUFFER, tw.ibo );
	qglEnableVertexAttribArray( ATTR_INDEX_POSITION );
	qglEnableVertexAttribArray( ATTR_INDEX_NORMAL );
	qglVertexAttribPointer( ATTR_INDEX_POSITION, 3, GL_FLOAT, GL_FALSE, TVERT_STRIDE, BUFFER_OFFSET( 0 ) );
	qglVertexAttribPointer( ATTR_INDEX_NORMAL, 3, GL_FLOAT, GL_FALSE, TVERT_STRIDE, BUFFER_OFFSET( 12 ) );
	qglBindVertexArray( 0 );
	qglBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );

	// splat map
	splat = ri.Hunk_AllocateTempMemory( sx * sy * 4 );
	Com_Memcpy( splat, in->splat, sx * sy * 4 );
	t->splat = R_CreateImage( va( "*oaxsplat%d", (int)( t - tw.terrains ) ), splat, sx, sy, IMGTYPE_COLORALPHA,
		IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE | IMGFLAG_NOLIGHTSCALE, GL_RGBA8 );
	ri.Hunk_FreeTempMemory( splat );
	for ( k = 0; k < OAX_TERRAIN_MAX_LAYERS; k++ ) {
		t->layers[k] = k < in->numLayers ? ShaderImage( in->layerShader[k] ) : NULL;
	}

	// foliage instances, chunk-major then type
	for ( f = 0; f < in->numFoliage; f++ ) {
		t->folImage[f] = ShaderImage( in->foliage[f].shader );
	}
	// count the instances first: sizing for every cell at full density is
	// 160 MB on a 512 x 320 cell terrain, far over the hunk
	for ( cj = 0; cj < t->chunksY; cj++ ) {
		for ( ci = 0; ci < t->chunksX; ci++ ) {
			for ( f = 0; f < in->numFoliage; f++ ) {
				int x, y;
				for ( y = 0; y < TCHUNK; y++ ) {
					for ( x = 0; x < TCHUNK; x++ ) {
						oaxFoliageInstance_t fi[OAX_FOLIAGE_MAX_PER_CELL];
						maxInst += OAXTerrain_CellFoliage( in, f, ci * TCHUNK + x, cj * TCHUNK + y, fi );
					}
				}
			}
		}
	}
	inst = maxInst ? ri.Hunk_AllocateTempMemory( maxInst * FINST_STRIDE ) : NULL;
	for ( cj = 0; cj < t->chunksY; cj++ ) {
		for ( ci = 0; ci < t->chunksX; ci++ ) {
			tChunk_t *c = &t->chunks[cj * t->chunksX + ci];
			for ( f = 0; f < in->numFoliage; f++ ) {
				int x, y, vv, nvar = FoliageVariants( &in->foliage[f] );
				c->instFirst[f] = numInst;
				// model foliage: instances grouped by variant, one draw each
				for ( vv = 0; vv < nvar; vv++ ) {
				int before = numInst;
				for ( y = 0; y < TCHUNK; y++ ) {
					for ( x = 0; x < TCHUNK; x++ ) {
						oaxFoliageInstance_t fi[OAX_FOLIAGE_MAX_PER_CELL];
						int n = OAXTerrain_CellFoliage( in, f, ci * TCHUNK + x, cj * TCHUNK + y, fi ), m;
						for ( m = 0; m < n; m++ ) {
							float *p = inst + numInst * 8;
							vec3_t top;
							if ( nvar > 1 && FoliageVariantOf( &fi[m], nvar ) != vv ) {
								continue;
							}
							p[0] = fi[m].origin[0];
							p[1] = fi[m].origin[1];
							p[2] = fi[m].origin[2];
							p[3] = fi[m].scale;
							p[4] = cos( DEG2RAD( fi[m].yaw ) );
							p[5] = sin( DEG2RAD( fi[m].yaw ) );
							p[6] = ( ( numInst * 2654435761U ) >> 8 & 0xffff ) / 65535.0f;
							p[7] = in->foliage[f].kind;
							VectorCopy( fi[m].origin, top );
							top[2] += fi[m].scale;
							AddPointToBounds( top, c->bounds[0], c->bounds[1] );
							numInst++;
						}
					}
				}
				c->varCount[f][vv] = numInst - before;
				}
				c->instCount[f] = numInst - c->instFirst[f];
			}
		}
	}
	t->numInstances = numInst;
	tw.statFoliageTotal += numInst;
	if ( numInst ) {
		qglGenBuffers( 1, &t->instVbo );
		qglBindBuffer( GL_ARRAY_BUFFER, t->instVbo );
		qglBufferData( GL_ARRAY_BUFFER, numInst * FINST_STRIDE, inst, GL_STATIC_DRAW );

		qglGenVertexArrays( 1, &t->folVao );
		qglBindVertexArray( t->folVao );
		qglBindBuffer( GL_ARRAY_BUFFER, tw.meshVbo );
		qglBindBuffer( GL_ELEMENT_ARRAY_BUFFER, tw.meshIbo );
		qglEnableVertexAttribArray( ATTR_INDEX_POSITION );
		qglEnableVertexAttribArray( ATTR_INDEX_NORMAL );
		qglEnableVertexAttribArray( ATTR_INDEX_TEXCOORD );
		qglVertexAttribPointer( ATTR_INDEX_POSITION, 3, GL_FLOAT, GL_FALSE, FVERT_STRIDE, BUFFER_OFFSET( 0 ) );
		qglVertexAttribPointer( ATTR_INDEX_NORMAL, 3, GL_FLOAT, GL_FALSE, FVERT_STRIDE, BUFFER_OFFSET( 12 ) );
		qglVertexAttribPointer( ATTR_INDEX_TEXCOORD, 2, GL_FLOAT, GL_FALSE, FVERT_STRIDE, BUFFER_OFFSET( 24 ) );
		qglBindBuffer( GL_ARRAY_BUFFER, t->instVbo );
		qglEnableVertexAttribArray( ATTR_INDEX_POSITION2 );
		qglEnableVertexAttribArray( ATTR_INDEX_NORMAL2 );
		qglVertexAttribPointer( ATTR_INDEX_POSITION2, 4, GL_FLOAT, GL_FALSE, FINST_STRIDE, BUFFER_OFFSET( 0 ) );
		qglVertexAttribPointer( ATTR_INDEX_NORMAL2, 4, GL_FLOAT, GL_FALSE, FINST_STRIDE, BUFFER_OFFSET( 16 ) );
		qglVertexAttribDivisor( ATTR_INDEX_POSITION2, 1 );
		qglVertexAttribDivisor( ATTR_INDEX_NORMAL2, 1 );
		qglBindVertexArray( 0 );
		qglBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );
	}
	qglBindBuffer( GL_ARRAY_BUFFER, 0 );
	if ( inst ) {
		ri.Hunk_FreeTempMemory( inst );
	}
	if ( numInst ) {
		LoadFoliageModels( t );
	}
}

static void FreeGL( void ) {
	int k, c;

	for ( k = 0; k < tw.numTerrains; k++ ) {
		tTerrain_t *t = &tw.terrains[k];
		if ( t->vbo ) qglDeleteBuffers( 1, &t->vbo );
		if ( t->instVbo ) qglDeleteBuffers( 1, &t->instVbo );
		if ( t->vao ) qglDeleteVertexArrays( 1, &t->vao );
		if ( t->folVao ) qglDeleteVertexArrays( 1, &t->folVao );
		if ( t->modelVbo ) qglDeleteBuffers( 1, &t->modelVbo );
		if ( t->modelIbo ) qglDeleteBuffers( 1, &t->modelIbo );
		if ( t->modelVao ) qglDeleteVertexArrays( 1, &t->modelVao );
	}
	if ( tw.numQueries ) qglDeleteQueries( tw.numQueries, tw.queries );
	if ( tw.progsOk ) {
		for ( c = 0; c < 3; c++ ) GLSL_DeleteGPUShader( &tw.terrainProg[c] );
		for ( c = 0; c < 2; c++ ) GLSL_DeleteGPUShader( &tw.foliageProg[c] );
	}
	if ( tw.ibo ) qglDeleteBuffers( 1, &tw.ibo );
	if ( tw.meshVbo ) qglDeleteBuffers( 1, &tw.meshVbo );
	if ( tw.meshIbo ) qglDeleteBuffers( 1, &tw.meshIbo );
	if ( tw.boxVbo ) qglDeleteBuffers( 1, &tw.boxVbo );
	if ( tw.boxIbo ) qglDeleteBuffers( 1, &tw.boxIbo );
	if ( tw.boxVao ) qglDeleteVertexArrays( 1, &tw.boxVao );
	Com_Memset( &tw, 0, sizeof( tw ) );
}

void R_OAXTerrainShutdown( void ) {
	if ( tw.loaded ) {
		FreeGL();
	}
	Com_Memset( &tw, 0, sizeof( tw ) );
}

/*
=================
R_OAXTerrainLoadWorld

Called from RE_LoadWorldMap with the whole BSP file, after tr.world is set.
A map without OAX_TERRAIN costs nothing.
=================
*/
void R_OAXTerrainLoadWorld( const void *bsp, int bspLen ) {
	oaxTerrainInfo_t infos[OAX_TERRAIN_MAX];
	const void *lump;
	int len, n, k, v;
	char err[128];

	R_OAXTerrainShutdown();
	lump = OAXTerrain_FindLump( bsp, bspLen, &len );
	if ( !lump ) {
		return;
	}
	if ( !glRefConfig.vertexArrayObject || !qglVertexAttribDivisor || !qglDrawElementsInstanced ) {
		ri.Printf( PRINT_WARNING, "OAX_TERRAIN: needs vertex array objects and instancing; terrain not drawn\n" );
		return;
	}
	n = OAXTerrain_Parse( lump, len, infos, OAX_TERRAIN_MAX, err, sizeof( err ) );
	if ( !n ) {
		ri.Printf( PRINT_WARNING, "OAX_TERRAIN ignored: %s\n", err );
		return;
	}
	InitPrograms();
	if ( !tw.progsOk ) {
		ri.Printf( PRINT_WARNING, "OAX_TERRAIN: programs failed; terrain not drawn\n" );
		return;
	}
	BuildPatterns();
	BuildMeshes();
	for ( k = 0; k < n; k++ ) {
		tTerrain_t *t = &tw.terrains[k];
		int cells = infos[k].samplesX * infos[k].samplesY;
		byte *copy = ri.Hunk_Alloc( cells * 10, h_low );
		t->info = infos[k];
		Com_Memcpy( copy, infos[k].heights, cells * 2 );
		Com_Memcpy( copy + cells * 2, infos[k].splat, cells * 4 );
		Com_Memcpy( copy + cells * 6, infos[k].density, cells * 4 );
		t->info.heights = copy;
		t->info.splat = copy + cells * 2;
		t->info.density = copy + cells * 6;
		tw.numTerrains = k + 1;
		LoadTerrain( t );
		tw.totalChunks += t->chunksX * t->chunksY;
	}
	for ( v = 0; v < MAX_TVIEWS; v++ ) {
		tw.views[v].list = ri.Hunk_Alloc( tw.totalChunks * sizeof( tViewChunk_t ), h_low );
	}

	// occlusion box geometry: a unit cube, scaled per query in the vertex data
	{
		static const unsigned short boxIdx[36] = {
			0, 1, 3, 0, 3, 2, 4, 6, 7, 4, 7, 5, 0, 4, 5, 0, 5, 1,
			2, 3, 7, 2, 7, 6, 0, 2, 6, 0, 6, 4, 1, 5, 7, 1, 7, 3 };
		qglGenBuffers( 1, &tw.boxVbo );
		qglGenBuffers( 1, &tw.boxIbo );
		qglGenVertexArrays( 1, &tw.boxVao );
		qglBindVertexArray( tw.boxVao );
		qglBindBuffer( GL_ARRAY_BUFFER, tw.boxVbo );
		qglBufferData( GL_ARRAY_BUFFER, tw.totalChunks * 8 * 12, NULL, GL_DYNAMIC_DRAW );
		qglBindBuffer( GL_ELEMENT_ARRAY_BUFFER, tw.boxIbo );
		qglBufferData( GL_ELEMENT_ARRAY_BUFFER, sizeof( boxIdx ), boxIdx, GL_STATIC_DRAW );
		qglEnableVertexAttribArray( ATTR_INDEX_POSITION );
		qglVertexAttribPointer( ATTR_INDEX_POSITION, 3, GL_FLOAT, GL_FALSE, 12, BUFFER_OFFSET( 0 ) );
		qglBindVertexArray( 0 );
		qglBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );
		qglBindBuffer( GL_ARRAY_BUFFER, 0 );
	}
	glState.currentVao = NULL;
	tw.loaded = qtrue;
	ri.Printf( PRINT_ALL, "OAX_TERRAIN: %d terrain(s), %d chunks, %d foliage instances, stitching %s\n",
		n, tw.totalChunks, tw.statFoliageTotal, tw.statStitchOk ? "ok" : "BROKEN" );
	if ( ri.DebugSet ) {
		ri.DebugSet( "r_terrain_chunks", va( "%d", tw.totalChunks ) );
		ri.DebugSet( "r_terrain_stitch_ok", va( "%d", tw.statStitchOk ) );
		ri.DebugSet( "r_foliage_instances_total", va( "%d", tw.statFoliageTotal ) );
	}
}

// ---- front end: chunks for a view ------------------------------------------------------------

static float BoxDistance( const vec3_t p, vec3_t b[2] ) {
	vec3_t d;
	int i;

	for ( i = 0; i < 3; i++ ) {
		d[i] = p[i] < b[0][i] ? b[0][i] - p[i] : p[i] > b[1][i] ? p[i] - b[1][i] : 0;
	}
	return VectorLength( d );
}

static void ComputeLods( const vec3_t eye ) {
	int k, i, changed, pass;
	float lodDist = r_oaxTerrainLodDist->value;

	for ( k = 0; k < tw.numTerrains; k++ ) {
		tTerrain_t *t = &tw.terrains[k];
		int nc = t->chunksX * t->chunksY;
		for ( i = 0; i < nc; i++ ) {
			tChunk_t *c = &t->chunks[i];
			int lod = 0;
			if ( lodDist > 0 ) {
				float d = BoxDistance( eye, c->bounds );
				while ( lod < TLODS - 1 && d >= lodDist * (float)( 1 << lod ) ) {
					lod++;
				}
			}
			c->lod = lod;
		}
		// neighbours differ by at most one level: lower the coarse side
		for ( pass = 0, changed = 1; changed && pass < TLODS * 2; pass++ ) {
			int x, y;
			changed = 0;
			for ( y = 0; y < t->chunksY; y++ ) {
				for ( x = 0; x < t->chunksX; x++ ) {
					tChunk_t *c = &t->chunks[y * t->chunksX + x];
					int m = c->lod;
					if ( x > 0 && t->chunks[y * t->chunksX + x - 1].lod + 1 < m ) m = t->chunks[y * t->chunksX + x - 1].lod + 1;
					if ( x < t->chunksX - 1 && t->chunks[y * t->chunksX + x + 1].lod + 1 < m ) m = t->chunks[y * t->chunksX + x + 1].lod + 1;
					if ( y > 0 && t->chunks[( y - 1 ) * t->chunksX + x].lod + 1 < m ) m = t->chunks[( y - 1 ) * t->chunksX + x].lod + 1;
					if ( y < t->chunksY - 1 && t->chunks[( y + 1 ) * t->chunksX + x].lod + 1 < m ) m = t->chunks[( y + 1 ) * t->chunksX + x].lod + 1;
					if ( m != c->lod ) {
						c->lod = m;
						changed = 1;
					}
				}
			}
		}
	}
}

static int StitchMask( tTerrain_t *t, tChunk_t *c ) {
	int mask = 0, x = c->ci, y = c->cj;

	if ( y > 0 && t->chunks[( y - 1 ) * t->chunksX + x].lod > c->lod ) mask |= 1;
	if ( x < t->chunksX - 1 && t->chunks[y * t->chunksX + x + 1].lod > c->lod ) mask |= 2;
	if ( y < t->chunksY - 1 && t->chunks[( y + 1 ) * t->chunksX + x].lod > c->lod ) mask |= 4;
	if ( x > 0 && t->chunks[y * t->chunksX + x - 1].lod > c->lod ) mask |= 8;
	return mask;
}

/*
=================
R_OAXTerrainAddView

From R_AddWorldSurfaces: frustum-cull the chunks for tr.viewParms, choose
their levels (from the scene camera, so shadow cascades draw the same
geometry the eye sees), and leave the list for the back end.
=================
*/
void R_OAXTerrainAddView( void ) {
	tView_t *v;
	int k, i, slot;

	tr.viewParms.oaxTerrainView = 0;
	if ( !tw.loaded || !r_oaxTerrain->integer || ( tr.refdef.rdflags & RDF_OAX_SKYPORTAL ) ) {
		return;
	}
	if ( ( r_oaxTerrainDebug->integer == 3 || r_oaxTerrainDebug->integer == 5 ) && ( tr.viewParms.flags & VPF_DEPTHSHADOW ) ) {
		return;	// debug: terrain casts no sun shadows
	}
	slot = tw.viewSeq++ % MAX_TVIEWS;
	v = &tw.views[slot];
	v->num = 0;
	v->shadow = ( tr.viewParms.flags & VPF_DEPTHSHADOW ) != 0;
	v->main = !v->shadow && !( tr.viewParms.flags & VPF_SHADOWMAP ) && !tr.viewParms.isPortal;
	VectorCopy( tr.refdef.vieworg, v->eye );
	ComputeLods( tr.refdef.vieworg );
	for ( k = 0; k < tw.numTerrains; k++ ) {
		tTerrain_t *t = &tw.terrains[k];
		for ( i = 0; i < t->chunksX * t->chunksY; i++ ) {
			tChunk_t *c = &t->chunks[i];
			if ( R_CullBox( c->bounds ) == CULL_OUT ) {
				continue;
			}
			v->list[v->num].chunk = c;
			// sun shadow cascades draw full detail: the whole-level cascade is
			// rendered once and kept (tr_scene.c), so it must not depend on
			// where the camera was that frame
			v->list[v->num].lod = v->shadow ? 0 : c->lod;
			v->list[v->num].mask = v->shadow ? 0 : StitchMask( t, c );
			v->num++;
			AddPointToBounds( c->bounds[0], tr.viewParms.visBounds[0], tr.viewParms.visBounds[1] );
			AddPointToBounds( c->bounds[1], tr.viewParms.visBounds[0], tr.viewParms.visBounds[1] );
		}
	}
	tr.viewParms.oaxTerrainView = slot + 1;
}

// ---- back end --------------------------------------------------------------------------------

static tView_t *BackendView( void ) {
	int s = backEnd.viewParms.oaxTerrainView;

	if ( !tw.loaded || s <= 0 || s > MAX_TVIEWS || !tw.progsOk ) {
		return NULL;
	}
	return &tw.views[s - 1];
}

// should this chunk be skipped as occluded?
static qboolean ChunkOccluded( const tView_t *v, tChunk_t *c ) {
	float margin = r_oaxOcclusionMargin->value;

	if ( !v->main || !r_oaxOcclusion->integer || !glRefConfig.occlusionQuery || !c->resultValid || !c->hidden ) {
		return qfalse;
	}
	if ( r_oaxOcclusion->integer == 2 ) {
		return qtrue;	// the broken control mode: stale results, no camera check
	}
	return Distance( v->eye, c->resultEye ) <= margin * 0.25f;
}

// the inflated query box, if it is entirely on screen and in front of the eye
static qboolean QueryBox( const tChunk_t *c, float margin, vec3_t out[8] ) {
	int k, i;

	for ( k = 0; k < 8; k++ ) {
		vec4_t p, clip;
		out[k][0] = ( k & 1 ) ? c->bounds[1][0] + margin : c->bounds[0][0] - margin;
		out[k][1] = ( k & 2 ) ? c->bounds[1][1] + margin : c->bounds[0][1] - margin;
		out[k][2] = ( k & 4 ) ? c->bounds[1][2] + margin : c->bounds[0][2] - margin;
		VectorCopy( out[k], p );
		p[3] = 1.0f;
		for ( i = 0; i < 4; i++ ) {
			clip[i] = glState.modelviewProjection[i] * p[0] + glState.modelviewProjection[4 + i] * p[1] +
				glState.modelviewProjection[8 + i] * p[2] + glState.modelviewProjection[12 + i];
		}
		if ( clip[3] <= r_znear->value || fabs( clip[0] ) > clip[3] || fabs( clip[1] ) > clip[3] ) {
			return qfalse;
		}
	}
	return qtrue;
}

static void PollQueries( void ) {
	int k, i;

	for ( k = 0; k < tw.numTerrains; k++ ) {
		tTerrain_t *t = &tw.terrains[k];
		for ( i = 0; i < t->chunksX * t->chunksY; i++ ) {
			tChunk_t *c = &t->chunks[i];
			GLuint avail = 0, samples = 0;
			if ( !c->pending ) {
				continue;
			}
			qglGetQueryObjectuiv( tw.queries[c->query - 1], GL_QUERY_RESULT_AVAILABLE, &avail );
			if ( !avail ) {
				continue;
			}
			qglGetQueryObjectuiv( tw.queries[c->query - 1], GL_QUERY_RESULT, &samples );
			c->pending = qfalse;
			c->resultValid = qtrue;
			c->hidden = samples == 0;
			VectorCopy( c->queryEye, c->resultEye );
			if ( r_oaxOcclusion->integer == 2 ) {
				c->frozen = qtrue;
			}
		}
	}
}

static void IssueQueries( const tView_t *v ) {
	float margin = r_oaxOcclusion->integer == 2 ? 0.0f : r_oaxOcclusionMargin->value;
	vec3_t (*boxes)[8];
	tChunk_t **who;
	int i, n = 0;
	GLenum target = qglesMajorVersion >= 3 ? GL_ANY_SAMPLES_PASSED_CONSERVATIVE : glRefConfig.occlusionQueryTarget;

	if ( !v->main || !r_oaxOcclusion->integer || !glRefConfig.occlusionQuery ) {
		return;
	}
	boxes = ri.Hunk_AllocateTempMemory( v->num * sizeof( *boxes ) );
	who = ri.Hunk_AllocateTempMemory( v->num * sizeof( *who ) );
	for ( i = 0; i < v->num; i++ ) {
		tChunk_t *c = v->list[i].chunk;
		if ( c->pending || c->frozen ) {
			continue;
		}
		if ( !QueryBox( c, margin, boxes[n] ) ) {
			c->resultValid = qfalse;		// unknown: drawn
			continue;
		}
		who[n++] = c;
	}
	if ( n ) {
		qglBindVertexArray( tw.boxVao );
		glState.currentVao = NULL;
		qglBindBuffer( GL_ARRAY_BUFFER, tw.boxVbo );
		qglBufferSubData( GL_ARRAY_BUFFER, 0, n * 8 * 12, boxes );
		GLSL_BindProgram( &tw.terrainProg[0] );
		GLSL_SetUniformMat4( &tw.terrainProg[0], UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection );
		GL_State( GLS_DEPTHFUNC_BITS & 0 );		// depth test LEQUAL, no depth writes
		GL_Cull( CT_TWO_SIDED );
		for ( i = 0; i < n; i++ ) {
			tChunk_t *c = who[i];
			if ( !c->query ) {
				if ( tw.numQueries >= (int)ARRAY_LEN( tw.queries ) ) {
					continue;
				}
				qglGenQueries( 1, &tw.queries[tw.numQueries] );
				c->query = ++tw.numQueries;
			}
			qglVertexAttribPointer( ATTR_INDEX_POSITION, 3, GL_FLOAT, GL_FALSE, 12, BUFFER_OFFSET( i * 8 * 12 ) );
			qglBeginQuery( target, tw.queries[c->query - 1] );
			qglDrawElements( GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, BUFFER_OFFSET( 0 ) );
			qglEndQuery( target );
			c->pending = qtrue;
			VectorCopy( v->eye, c->queryEye );
		}
		qglBindVertexArray( 0 );
		qglBindBuffer( GL_ARRAY_BUFFER, 0 );
	}
	tw.statQueries = n;
	ri.Hunk_FreeTempMemory( who );
	ri.Hunk_FreeTempMemory( boxes );
}

static void SetSunUniforms( int *locs, int sunDir, int sunColor, int ambient, int screen, qboolean color ) {
	vec3_t sun, amb;
	float useShadow = 0;

	if ( !color ) {
		return;
	}
	// the same sun the stock lightall shading uses (tr_scene.c), with a
	// floor so maps without q3gl2_sun still read as daylight
	VectorCopy( backEnd.refdef.sunCol, sun );
	if ( VectorLength( sun ) < 0.01f ) {
		float scale = ( 1 << r_mapOverBrightBits->integer ) / 255.0f;
		VectorScale( tr.sunLight, scale, sun );
	}
	VectorScale( sun, tr.sunShadowScale, amb );
	if ( r_sunlightMode->integer && ( backEnd.viewParms.flags & VPF_USESUNLIGHT ) && tr.screenShadowImage ) {
		useShadow = 1;
		GL_BindToTMU( tr.screenShadowImage, 5 );
	}
	qglUniform3f( locs[sunDir], tr.sunDirection[0], tr.sunDirection[1], tr.sunDirection[2] );
	qglUniform3f( locs[sunColor], sun[0], sun[1], sun[2] );
	qglUniform3f( locs[ambient], amb[0], amb[1], amb[2] );
	qglUniform4f( locs[screen], 1.0f / glConfig.vidWidth, 1.0f / glConfig.vidHeight, 0, useShadow );
}

static void DrawTerrain( const tView_t *v, int pass ) {
	int debug = r_oaxTerrainDebug->integer;
	int pi = pass == 0 ? 0 : ( debug == 1 || debug == 2 ? 2 : 1 );
	shaderProgram_t *sp = &tw.terrainProg[pi];
	int *loc = TerrainLocs[pi];
	int k, i, tris = 0, drawn = 0, occluded = 0, lodBits = 0, stitched = 0;

	GLSL_BindProgram( sp );
	GLSL_SetUniformMat4( sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection );
	GL_State( GLS_DEPTHMASK_TRUE );
	// sun shadow casters: both sides, pushed away from the light by a slope
	// scaled offset so lit ground does not shadow itself (back faces alone
	// leave a cell-sized staircase along every terminator)
	GL_Cull( CT_TWO_SIDED );
	if ( v->shadow ) {
		qglEnable( GL_POLYGON_OFFSET_FILL );
		qglPolygonOffset( r_oaxShadowOffset->value, r_oaxShadowOffset->value * 2.0f );
	}
	if ( pass ) {
		qglUniform1i( loc[TU_LAYER0], 0 );
		qglUniform1i( loc[TU_LAYER1], 1 );
		qglUniform1i( loc[TU_LAYER2], 2 );
		qglUniform1i( loc[TU_LAYER3], 3 );
		qglUniform1i( loc[TU_SPLAT], 4 );
		qglUniform1i( loc[TU_SHADOW], 5 );
		SetSunUniforms( loc, TU_SUNDIR, TU_SUNCOLOR, TU_AMBIENT, TU_SCREEN, qtrue );
	}
	for ( k = 0; k < tw.numTerrains; k++ ) {
		tTerrain_t *t = &tw.terrains[k];
		const oaxTerrainInfo_t *in = &t->info;

		if ( pass ) {
			vec4_t scale;
			int l;
			for ( l = 0; l < 4; l++ ) {
				GL_BindToTMU( t->layers[l] ? t->layers[l] : tr.whiteImage, l );
				scale[l] = t->layers[l] && in->layerTexScale[l] > 0 ? 1.0f / in->layerTexScale[l] : 0.0f;
			}
			GL_BindToTMU( t->splat, 4 );
			qglUniform4f( loc[TU_LAYERSCALE], scale[0], scale[1], scale[2], scale[3] );
			qglUniform4f( loc[TU_SPLATXFORM], in->origin[0] - 0.5f * in->cellSize, in->origin[1] - 0.5f * in->cellSize,
				1.0f / ( in->cellSize * in->samplesX ), 1.0f / ( in->cellSize * in->samplesY ) );
			qglUniform1f( loc[TU_DEBUG], (float)debug );
			qglUniform1f( loc[TU_TRIPLANAR], ( in->flags & OAX_TERRAIN_TRIPLANAR ) ? 1.0f : 0.0f );
			qglUniform2f( loc[TU_SURFACEFX], ( in->flags & OAX_TERRAIN_MACRO ) ? 1.0f : 0.0f, ( in->flags & OAX_TERRAIN_DETAIL ) ? 1.0f : 0.0f );
			qglUniform3f( loc[TU_VIEWORIGIN], backEnd.viewParms.or.origin[0], backEnd.viewParms.or.origin[1], backEnd.viewParms.or.origin[2] );
		}
		qglBindVertexArray( t->vao );
		glState.currentVao = NULL;
		// attribute pointers below capture the bound array buffer
		qglBindBuffer( GL_ARRAY_BUFFER, t->vbo );
		for ( i = 0; i < v->num; i++ ) {
			const tViewChunk_t *vc = &v->list[i];
			tChunk_t *c = vc->chunk;
			int first, count;
			if ( c->terrain != k ) {
				continue;
			}
			if ( ChunkOccluded( v, c ) ) {
				occluded++;
				continue;
			}
			if ( debug == 2 ) {
				first = UnstitchedFirst( vc->lod );
				count = tw.patCount[vc->lod][0];
			} else {
				first = tw.patFirst[vc->lod][vc->mask];
				count = tw.patCount[vc->lod][vc->mask];
			}
			qglVertexAttribPointer( ATTR_INDEX_POSITION, 3, GL_FLOAT, GL_FALSE, TVERT_STRIDE, BUFFER_OFFSET( c->vertBase * TVERT_STRIDE ) );
			qglVertexAttribPointer( ATTR_INDEX_NORMAL, 3, GL_FLOAT, GL_FALSE, TVERT_STRIDE, BUFFER_OFFSET( c->vertBase * TVERT_STRIDE + 12 ) );
			qglDrawElements( GL_TRIANGLES, count, GL_UNSIGNED_SHORT, BUFFER_OFFSET( first * sizeof( unsigned short ) ) );
			tris += count / 3;
			drawn++;
			lodBits |= 1 << vc->lod;
			stitched += vc->mask != 0;
		}
	}
	qglBindVertexArray( 0 );
	qglBindBuffer( GL_ARRAY_BUFFER, 0 );
	if ( v->shadow ) {
		qglDisable( GL_POLYGON_OFFSET_FILL );
	}
	if ( pass && v->main ) {
		tw.statVisible = v->num;
		tw.statDrawn = drawn;
		tw.statOccluded = occluded;
		tw.statTris = tris;
		tw.statStitched = stitched;
	}
	if ( v->shadow ) {
		tw.statShadowChunks += drawn;
		tw.statShadowViews++;
	}
	if ( pass && v->main ) {
		for ( tw.statLods = 0; lodBits; lodBits &= lodBits - 1 ) {
			tw.statLods++;
		}
	}
}

/*
DrawModelFoliage: one OAX_FOLIAGE_MODEL type on the visible chunks: per model
surface (part), one instanced draw per chunk over that part's variant.
Returns the instances drawn (counted once per instance, not per part).
*/
static int DrawModelFoliage( tTerrain_t *t, int k, int f, const tView_t *v, int *loc ) {
	const oaxFoliageDisk_t *fd = &t->info.foliage[f];
	int i, pi, drawn = 0;

	if ( !t->modelVao || !t->numParts[f] ) {
		return 0;
	}
	qglBindVertexArray( t->modelVao );
	glState.currentVao = NULL;
	qglBindBuffer( GL_ARRAY_BUFFER, t->instVbo );
	qglUniform4f( loc[FU_FADE], fd->fadeStart, fd->fadeEnd > fd->fadeStart ? 1.0f / ( fd->fadeEnd - fd->fadeStart ) : 0.0f, v->shadow ? 0.0f : 1.0f, 0 );
	for ( pi = 0; pi < t->numParts[f]; pi++ ) {
		const tFoliagePart_t *p = &t->parts[f][pi];
		GL_BindToTMU( p->image, 0 );
		for ( i = 0; i < v->num; i++ ) {
			tChunk_t *c = v->list[i].chunk;
			int first, w, count;
			if ( c->terrain != k || !c->instCount[f] || ChunkOccluded( v, c ) ) {
				continue;
			}
			if ( !v->shadow && BoxDistance( v->eye, c->bounds ) > fd->fadeEnd ) {
				continue;
			}
			first = c->instFirst[f];
			for ( w = 0; w < p->variant; w++ ) {
				first += c->varCount[f][w];
			}
			count = c->varCount[f][p->variant];
			if ( !count ) {
				continue;
			}
			qglVertexAttribPointer( ATTR_INDEX_POSITION2, 4, GL_FLOAT, GL_FALSE, FINST_STRIDE, BUFFER_OFFSET( first * FINST_STRIDE ) );
			qglVertexAttribPointer( ATTR_INDEX_NORMAL2, 4, GL_FLOAT, GL_FALSE, FINST_STRIDE, BUFFER_OFFSET( first * FINST_STRIDE + 16 ) );
			qglDrawElementsInstanced( GL_TRIANGLES, p->count, GL_UNSIGNED_INT, BUFFER_OFFSET( p->first * sizeof( unsigned int ) ), count );
			if ( pi == 0 || t->parts[f][pi - 1].variant != p->variant ) {
				drawn += count;
			}
		}
	}
	return drawn;
}

static void DrawFoliage( const tView_t *v, int pass ) {
	shaderProgram_t *sp = &tw.foliageProg[pass ? 1 : 0];
	int *loc = FoliageLocs[pass ? 1 : 0];
	int k, i, f, instances = 0, grass = 0, trees = 0;
	qboolean a2c;

	if ( !r_oaxFoliage->integer || r_oaxTerrainDebug->integer == 1 || r_oaxTerrainDebug->integer == 2 ) {
		return;
	}
	GLSL_BindProgram( sp );
	GLSL_SetUniformMat4( sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection );
	GL_State( GLS_DEPTHMASK_TRUE );
	GL_Cull( v->shadow ? CT_FRONT_SIDED : CT_TWO_SIDED );
	qglUniform1i( loc[FU_TEX], 0 );
	qglUniform3f( loc[FU_VIEWORIGIN], v->eye[0], v->eye[1], v->eye[2] );
	if ( pass ) {
		qglUniform1i( loc[FU_SHADOW], 5 );
		SetSunUniforms( loc, FU_SUNDIR, FU_SUNCOLOR, FU_AMBIENT, FU_SCREEN, qtrue );
	}
	// the map's wind (tr_oax_env.c), also in the shadow passes so shadows sway too
	if ( R_OAXEnvOn() && tr.oaxEnv.hasWind ) {
		float yaw = DEG2RAD( tr.oaxEnv.wind[2] );
		qglUniform4f( loc[FU_WIND], cos( yaw ), sin( yaw ), tr.oaxEnv.wind[0], tr.oaxEnv.wind[1] );
	} else {
		qglUniform4f( loc[FU_WIND], 0, 0, 0, 0 );
	}
	qglUniform1f( loc[FU_TIME], backEnd.refdef.floatTime );
	// alpha to coverage: colour pass of a multisampled view only
	a2c = pass && !v->shadow && R_OAXEnvOn() && tr.oaxEnv.foliageA2C && !backEnd.viewParms.targetFbo
		&& tr.msaaResolveFbo && glState.currentFBO == tr.renderFbo;
	qglUniform1f( loc[FU_A2C], a2c ? 1.0f : 0.0f );
	if ( a2c ) {
		qglEnable( GL_SAMPLE_ALPHA_TO_COVERAGE );
	}
	for ( k = 0; k < tw.numTerrains; k++ ) {
		tTerrain_t *t = &tw.terrains[k];
		if ( !t->numInstances ) {
			continue;
		}
		qglBindVertexArray( t->folVao );
		glState.currentVao = NULL;
		qglBindBuffer( GL_ARRAY_BUFFER, t->instVbo );
		for ( f = 0; f < t->info.numFoliage; f++ ) {
			const oaxFoliageDisk_t *fd = &t->info.foliage[f];
			int mesh = fd->kind == OAX_FOLIAGE_TREE ? 1 : 0;
			if ( fd->kind == OAX_FOLIAGE_MODEL ) {
				{
					int n = DrawModelFoliage( t, k, f, v, loc );
					instances += n;
					trees += n;
				}
				qglBindVertexArray( t->folVao );
				qglBindBuffer( GL_ARRAY_BUFFER, t->instVbo );
				continue;
			}
			// grass does not cast sun shadows (cheap, and too fine for the maps)
			if ( v->shadow && mesh == 0 ) {
				continue;
			}
			GL_BindToTMU( t->folImage[f] ? t->folImage[f] : tr.whiteImage, 0 );
			qglUniform4f( loc[FU_FADE], fd->fadeStart, fd->fadeEnd > fd->fadeStart ? 1.0f / ( fd->fadeEnd - fd->fadeStart ) : 0.0f, v->shadow ? 0.0f : 1.0f, 0 );
			for ( i = 0; i < v->num; i++ ) {
				tChunk_t *c = v->list[i].chunk;
				if ( c->terrain != k || !c->instCount[f] || ChunkOccluded( v, c ) ) {
					continue;
				}
				if ( !v->shadow && BoxDistance( v->eye, c->bounds ) > fd->fadeEnd ) {
					continue;
				}
				qglVertexAttribPointer( ATTR_INDEX_POSITION2, 4, GL_FLOAT, GL_FALSE, FINST_STRIDE, BUFFER_OFFSET( c->instFirst[f] * FINST_STRIDE ) );
				qglVertexAttribPointer( ATTR_INDEX_NORMAL2, 4, GL_FLOAT, GL_FALSE, FINST_STRIDE, BUFFER_OFFSET( c->instFirst[f] * FINST_STRIDE + 16 ) );
				qglDrawElementsInstanced( GL_TRIANGLES, tw.meshCount[mesh], GL_UNSIGNED_SHORT,
					BUFFER_OFFSET( tw.meshFirst[mesh] * sizeof( unsigned short ) ), c->instCount[f] );
				instances += c->instCount[f];
				if ( mesh ) trees += c->instCount[f]; else grass += c->instCount[f];
			}
		}
	}
	qglBindVertexArray( 0 );
	qglBindBuffer( GL_ARRAY_BUFFER, 0 );
	if ( a2c ) {
		qglDisable( GL_SAMPLE_ALPHA_TO_COVERAGE );
	}
	if ( pass && v->main ) {
		tw.statFoliage = instances;
		tw.statGrass = grass;
		tw.statTrees = trees;
	}
}

/*
=================
RB_OAXTerrainDepth

In a view's depth prepass, before the stock surfaces (colour writes are off):
terrain and foliage depth, then, for player views, the occlusion queries
against that terrain-only depth.
=================
*/
void RB_OAXTerrainDepth( void ) {
	tView_t *v = BackendView();

	if ( !v ) {
		return;
	}
	// the view's world matrix: the stock list sets it per entity, so here it
	// may still be the previous view's
	GL_SetModelviewMatrix( backEnd.viewParms.world.modelMatrix );
	if ( v->main ) {
		PollQueries();
	}
	DrawTerrain( v, 0 );
	DrawFoliage( v, 0 );
	if ( v->main ) {
		IssueQueries( v );
	}
	GL_State( GLS_DEFAULT );
}

/*
=================
RB_OAXTerrainColor

The colour pass, before the stock surface list (so translucent surfaces
blend over the terrain).
=================
*/
void RB_OAXTerrainColor( void ) {
	tView_t *v = BackendView();

	if ( !v ) {
		return;
	}
	GL_SetModelviewMatrix( backEnd.viewParms.world.modelMatrix );
	if ( !r_depthPrepass->integer && v->main ) {
		PollQueries();	// no prepass: the colour pass is the only one
	}
	if ( v->main ) {
		tw.statFoliage = tw.statGrass = tw.statTrees = 0;
	}
	DrawTerrain( v, 1 );
	RB_OAXProfZone( OAX_PZ_FOLIAGE );
	DrawFoliage( v, 1 );
	GL_State( GLS_DEFAULT );
	if ( v->main && ri.DebugSet ) {
		ri.DebugSet( "r_terrain_chunks_visible", va( "%d", tw.statVisible ) );
		ri.DebugSet( "r_terrain_chunks_drawn", va( "%d", tw.statDrawn ) );
		ri.DebugSet( "r_terrain_chunks_occluded", va( "%d", tw.statOccluded ) );
		ri.DebugSet( "r_terrain_queries", va( "%d", tw.statQueries ) );
		ri.DebugSet( "r_terrain_tris", va( "%d", tw.statTris ) );
		ri.DebugSet( "r_foliage_instances", va( "%d", tw.statFoliage ) );
		ri.DebugSet( "r_foliage_grass", va( "%d", tw.statGrass ) );
		ri.DebugSet( "r_foliage_trees", va( "%d", tw.statTrees ) );
		ri.DebugSet( "r_terrain_lods", va( "%d", tw.statLods ) );
		ri.DebugSet( "r_terrain_stitched", va( "%d", tw.statStitched ) );
		ri.DebugSet( "r_terrain_shadow_views", va( "%d", tw.statShadowViews ) );
		ri.DebugSet( "r_terrain_shadow_chunks", va( "%d", tw.statShadowChunks ) );
		tw.statShadowViews = tw.statShadowChunks = 0;
	}
}

/*
=================
R_OAXTerrainStats

The last main view's terrain chunks drawn and foliage instances (profiler).
=================
*/
void R_OAXTerrainStats( int *chunks, int *foliage ) {
	*chunks = tw.loaded ? tw.statDrawn : 0;
	*foliage = tw.loaded ? tw.statFoliage : 0;
}

/*
=================
R_OAXTerrainTriangles

The collision triangles of every terrain cell overlapping the box (world
xy), for projected decals; a cell's two triangles as
OAXTerrain_CellTriangles splits it. Returns how many were passed on.
=================
*/
int R_OAXTerrainTriangles( const vec3_t mins, const vec3_t maxs, void ( *tri )( void *ctx, float t[3][3] ), void *ctx ) {
	int k, i, j, n = 0;

	if ( !tw.loaded ) {
		return 0;
	}
	for ( k = 0; k < tw.numTerrains; k++ ) {
		const oaxTerrainInfo_t *in = &tw.terrains[k].info;
		int i0 = (int)floor( ( mins[0] - in->origin[0] ) / in->cellSize );
		int i1 = (int)floor( ( maxs[0] - in->origin[0] ) / in->cellSize );
		int j0 = (int)floor( ( mins[1] - in->origin[1] ) / in->cellSize );
		int j1 = (int)floor( ( maxs[1] - in->origin[1] ) / in->cellSize );

		i0 = MAX( i0, 0 );
		j0 = MAX( j0, 0 );
		i1 = MIN( i1, in->samplesX - 2 );
		j1 = MIN( j1, in->samplesY - 2 );
		for ( j = j0; j <= j1; j++ ) {
			for ( i = i0; i <= i1; i++ ) {
				float cell[2][3][3];

				OAXTerrain_CellTriangles( in, i, j, cell );
				tri( ctx, cell[0] );
				tri( ctx, cell[1] );
				n += 2;
			}
		}
	}
	return n;
}

qboolean R_OAXTerrainLoaded( void ) {
	return tw.loaded;
}
