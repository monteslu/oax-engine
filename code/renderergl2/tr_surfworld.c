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
tr_surfworld.c: the surface world (OAX_SURFACES, step 7.5).

A map's OAX_SURFACES lump (docs/map-format.md) carries render surfaces
that need no volume: polygons and meshes with materials, texture matrices,
tints and flags, each bound to the BSP leaves it passes through. They are
loaded as ordinary world surfaces, so every world path draws, culls and
lights them: PVS clusters and areas (area portals, doors), fog, marks,
dynamic lights, the unified lighting interactions and shadows, the world
vertex cache. Brush faces and surface-world surfaces coexist.

How: before the BSP's lumps are parsed, R_OAXSurfWorldRewrite builds a
copy of the file in which the shader, draw vertex, index, surface, leaf
surface, leaf and model lumps carry the extra surfaces (as triangle soups,
each world surface listed in its leaves, each inline-model surface in its
model's range). The stock loaders then read that copy unchanged. A map
without the lump never reaches any of this.

Per-surface material changes (two-sided, masked, translucent, additive,
tint) are shader variants: the surface's shader is named "#oaxsurf<k>_<hash>"
and R_FindShaderEx (tr_shader.c) builds it from the material's own script or
image, then R_OAXSurfVariantApply edits it before FinishShader.

Debug values: r_surfworld_surfaces (loaded), r_surfworld_skipped
(invisible), r_surfworld_unknown (+ _names: materials that resolved to
nothing), r_surfworld_variants, r_surfworld_drawn (in the player's view's
PVS and areas this frame), r_surfworld_hash.
===========================================================================
*/

#include "tr_local.h"
#include "../qcommon/oax_surfaces.h"

cvar_t *r_oaxSurfaces;

typedef struct {
	char		name[MAX_QPATH];		// "#oaxsurf<k>_<hash>"
	char		material[MAX_QPATH];
	unsigned	flags;					// OSF_SHADER_BITS
	float		tint[4];
} surfVariant_t;

typedef struct {
	int			worldSurf;				// index into s_worldData.surfaces
	int			lumpSurf;
	unsigned	lightMask;
	unsigned	flags;
	float		plane[4];
	float		tint[4];
	int			firstVert;				// in the lump, for tangents and colours
	int			numVerts;
	int			variant;
} surfRecord_t;

static struct {
	surfVariant_t	*variants;
	int				numVariants;
	surfRecord_t	*recs;
	int				numRecs;
	byte			*fileCopy;			// the rewritten BSP (freed after loading)
	int				skipped;
	int				unknown;
	char			unknownNames[512];
	unsigned		hash;
	oaxSurfVert_t	*verts;				// lump vertices kept for the post-load pass
	int				numVerts;
} sw;

void R_OAXSurfWorldRegisterCvars( void ) {
	r_oaxSurfaces = ri.Cvar_Get( "r_oaxSurfaces", "1", CVAR_CHEAT | CVAR_LATCH );
	ri.Cvar_SetDescription( r_oaxSurfaces, "Load the surface world (OAX_SURFACES) of maps that have one (0: the map without it, for comparisons; takes effect at the next map load)." );
}

static void FreeState( void ) {
	if ( sw.variants ) ri.Free( sw.variants );
	if ( sw.recs ) ri.Free( sw.recs );
	if ( sw.fileCopy ) ri.Free( sw.fileCopy );
	if ( sw.verts ) ri.Free( sw.verts );
	Com_Memset( &sw, 0, sizeof( sw ) );
}

void R_OAXSurfWorldShutdown( void ) {
	FreeState();
}

/*
=====================================================================

SHADER VARIANTS

=====================================================================
*/

const void *R_OAXSurfVariant( const char *name ) {
	int i;

	if ( Q_strncmp( name, "#oaxsurf", 8 ) ) {
		return NULL;
	}
	for ( i = 0; i < sw.numVariants; i++ ) {
		if ( !Q_stricmp( sw.variants[i].name, name ) ) {
			return &sw.variants[i];
		}
	}
	return NULL;
}

const char *R_OAXSurfVariantMaterial( const void *variant ) {
	return ( (const surfVariant_t *)variant )->material;
}

static byte TintByte( float base, float t ) {
	float v = base * t;

	return (byte)( v < 0 ? 0 : v > 255 ? 255 : v + 0.5f );
}

/*
=================
R_OAXSurfVariantApply

Called by R_FindShaderEx on the working shader before FinishShader, as if
the material's script had said `cull none`, `alphaFunc GE128`, a blendFunc
and `rgbGen const`.
=================
*/
void R_OAXSurfVariantApply( const void *variant, shader_t *sh, shaderStage_t *stages ) {
	const surfVariant_t *v = variant;
	int i, first = -1;
	qboolean tinted = v->tint[0] != 1.0f || v->tint[1] != 1.0f || v->tint[2] != 1.0f;

	for ( i = 0; i < MAX_SHADER_STAGES; i++ ) {
		if ( stages[i].active ) {
			first = i;
			break;
		}
	}
	if ( v->flags & OSF_TWOSIDED ) {
		sh->cullType = CT_TWO_SIDED;
	}
	if ( v->flags & OSF_NOSHADOW ) {
		sh->oaxNoShadow = qtrue;
	}
	if ( first >= 0 && ( v->flags & OSF_MASKED ) ) {
		stages[first].stateBits = ( stages[first].stateBits & ~GLS_ATEST_BITS ) | GLS_ATEST_GE_80;
	}
	if ( first >= 0 && ( v->flags & ( OSF_TRANSLUCENT | OSF_ADDITIVE ) ) ) {
		shaderStage_t *st = &stages[first];

		st->stateBits &= ~( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS | GLS_DEPTHMASK_TRUE );
		if ( ( v->flags & OSF_ADDITIVE ) && ( v->flags & OSF_TRANSLUCENT ) ) {
			// lit additive (UE1 Translucent without Unlit: water): its
			// vertex light (the ambient) x texel, the tint on top (oaxTint,
			// below), added to what is behind it; the unified lights add
			// their own light x texel over it (tr_ulight.c oaxLitBlend)
			st->stateBits |= GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE;
			st->rgbGen = CGEN_EXACT_VERTEX;
			sh->oaxLitBlend = qtrue;
		} else if ( v->flags & OSF_ADDITIVE ) {
			// a glow: added unlit, tint x texel on screen (identityLight in
			// the render target; the frame is scaled by the overbright at the end)
			st->stateBits |= GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE;
			st->rgbGen = CGEN_CONST;
			st->constantColor[0] = TintByte( 255.0f * tr.identityLight, v->tint[0] );
			st->constantColor[1] = TintByte( 255.0f * tr.identityLight, v->tint[1] );
			st->constantColor[2] = TintByte( 255.0f * tr.identityLight, v->tint[2] );
			st->constantColor[3] = 255;
		} else {
			// a pane: lit by its vertex light whatever the material's stage
			// said (light interactions skip blended surfaces), the tint on
			// top (oaxTint, below), opacity tint[3]; the vertex colours are
			// written for this in R_OAXSurfWorldFinishLoad
			st->stateBits |= GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA;
			st->rgbGen = CGEN_EXACT_VERTEX;
			st->alphaGen = AGEN_CONST;
			st->constantColor[3] = TintByte( 255.0f, v->tint[3] );
		}
		sh->sort = SS_BLEND0;
	}
	if ( tinted ) {
		// the material keyword oaxTint's own path (tr_shader.c
		// R_OAXFinishMaterialKeywords, applied in FinishShader): every stage
		// that carries the surface colour, in the generic pass and the light
		// interactions alike; combined with the material's own oaxTint
		if ( !sh->oaxHasTint ) {
			VectorSet( sh->oaxTint, 1, 1, 1 );
			sh->oaxHasTint = qtrue;
		}
		sh->oaxTint[0] *= v->tint[0];
		sh->oaxTint[1] *= v->tint[1];
		sh->oaxTint[2] *= v->tint[2];
	}
}

/*
=====================================================================

LOADING

=====================================================================
*/

static const byte *Lump( const byte *file, const dheader_t *h, int lump, int recSize, int *count ) {
	int ofs = LittleLong( h->lumps[lump].fileofs ), len = LittleLong( h->lumps[lump].filelen );

	*count = recSize ? len / recSize : 0;
	return file + ofs;
}

// the BSP's leaves a box touches that are not solid (unbound surfaces)
static void BoxLeaves_r( const byte *file, const dheader_t *h, int node, const vec3_t mins, const vec3_t maxs, int *out, int *n, int max ) {
	int numNodes, numPlanes, numLeafs;
	const dnode_t *nodes = (const dnode_t *)Lump( file, h, LUMP_NODES, sizeof( dnode_t ), &numNodes );
	const dplane_t *planes = (const dplane_t *)Lump( file, h, LUMP_PLANES, sizeof( dplane_t ), &numPlanes );
	const dleaf_t *leafs = (const dleaf_t *)Lump( file, h, LUMP_LEAFS, sizeof( dleaf_t ), &numLeafs );

	while ( node >= 0 ) {
		const dnode_t *nd;
		const dplane_t *pl;
		float dmin, dmax;
		int k;

		if ( node >= numNodes ) {
			return;
		}
		nd = &nodes[node];
		if ( LittleLong( nd->planeNum ) < 0 || LittleLong( nd->planeNum ) >= numPlanes ) {
			return;
		}
		pl = &planes[LittleLong( nd->planeNum )];
		dmin = dmax = -LittleFloat( pl->dist );
		for ( k = 0; k < 3; k++ ) {
			float nk = LittleFloat( pl->normal[k] );
			if ( nk > 0 ) {
				dmin += nk * mins[k];
				dmax += nk * maxs[k];
			} else {
				dmin += nk * maxs[k];
				dmax += nk * mins[k];
			}
		}
		if ( dmin >= 0 ) {
			node = LittleLong( nd->children[0] );
		} else if ( dmax < 0 ) {
			node = LittleLong( nd->children[1] );
		} else {
			BoxLeaves_r( file, h, LittleLong( nd->children[0] ), mins, maxs, out, n, max );
			node = LittleLong( nd->children[1] );
		}
	}
	node = -1 - node;
	if ( node < numLeafs && LittleLong( leafs[node].cluster ) != -1 && *n < max ) {
		out[( *n )++] = node;
	}
}

typedef struct {
	int		leaf;
	int		surf;		// new surface index
} leafRef_t;

static int LeafRefCompare( const void *a, const void *b ) {
	const leafRef_t *x = a, *y = b;

	if ( x->leaf != y->leaf ) {
		return x->leaf - y->leaf;
	}
	return x->surf - y->surf;
}

static void AppendLump( byte *base, int *at, dheader_t *h, int lump, const void *data, int len ) {
	*at = ( *at + 3 ) & ~3;
	Com_Memcpy( base + *at, data, len );
	h->lumps[lump].fileofs = LittleLong( *at );
	h->lumps[lump].filelen = LittleLong( len );
	*at += len;
}

/*
=================
R_OAXSurfWorldRewrite

Called by RE_LoadWorldMap with the whole file (header already byte
swapped). Returns the header to load from: the file's own when the map has
no surface world, else a rewritten copy.
=================
*/
dheader_t *R_OAXSurfWorldRewrite( const byte *file, int fileLen ) {
	dheader_t *h = (dheader_t *)file, *nh;
	const void *lumpData;
	oaxSurfLump_t L;
	char err[160];
	int lumpLen, i, j, k;
	int numModels, numSurfs, numVerts, numIndexes, numLeafSurfs, numLeafs, numShaders;
	const dmodel_t *models;
	const dsurface_t *surfs;
	const drawVert_t *verts;
	const int *indexes, *leafSurfs;
	const dleaf_t *leafs;
	const dshader_t *shaders;
	int *variantOf, *newIndexOfOld, *newIndexOfLump;
	int addVerts = 0, addIndexes = 0, addSurfs = 0, numRefs = 0, maxRefs = 1;
	leafRef_t *refs;
	int *found;
	dsurface_t *outSurfs;
	drawVert_t *outVerts;
	int *outIndexes, *outLeafSurfs;
	dleaf_t *outLeafs;
	dmodel_t *outModels;
	dshader_t *outShaders;
	int at, size, ns, nv, ni, nls;

	FreeState();
	if ( !r_oaxSurfaces ) {
		R_OAXSurfWorldRegisterCvars();
	}
	lumpData = OAX_FindBspxLump( file, fileLen, OAX_SURFACES_LUMP, &lumpLen );
	if ( !lumpData ) {
		return h;
	}
	sw.hash = OAX_Fnv1a( 2166136261U, lumpData, lumpLen );
	if ( !r_oaxSurfaces->integer ) {
		ri.Printf( PRINT_ALL, "OAX_SURFACES: not loaded (r_oaxSurfaces 0)\n" );
		return h;
	}
	if ( !OAXSurf_Parse( lumpData, lumpLen, &L, err, sizeof( err ) ) ) {
		sw.hash = 0;
		ri.Printf( PRINT_WARNING, "WARNING: OAX_SURFACES ignored: %s\n", err );
		return h;
	}

	models = (const dmodel_t *)Lump( file, h, LUMP_MODELS, sizeof( dmodel_t ), &numModels );
	surfs = (const dsurface_t *)Lump( file, h, LUMP_SURFACES, sizeof( dsurface_t ), &numSurfs );
	verts = (const drawVert_t *)Lump( file, h, LUMP_DRAWVERTS, sizeof( drawVert_t ), &numVerts );
	indexes = (const int *)Lump( file, h, LUMP_DRAWINDEXES, sizeof( int ), &numIndexes );
	leafSurfs = (const int *)Lump( file, h, LUMP_LEAFSURFACES, sizeof( int ), &numLeafSurfs );
	leafs = (const dleaf_t *)Lump( file, h, LUMP_LEAFS, sizeof( dleaf_t ), &numLeafs );
	shaders = (const dshader_t *)Lump( file, h, LUMP_SHADERS, sizeof( dshader_t ), &numShaders );
	if ( numModels < 1 ) {
		ri.Printf( PRINT_WARNING, "WARNING: OAX_SURFACES ignored: the map has no world model\n" );
		return h;
	}

	// shader variants and sizes
	variantOf = ri.Malloc( sizeof( int ) * ( L.numSurfaces + 1 ) );
	sw.variants = ri.Malloc( sizeof( surfVariant_t ) * ( L.numSurfaces + 1 ) );
	for ( i = 0; i < L.numSurfaces; i++ ) {
		oaxSurf_t s;
		surfVariant_t key;
		unsigned hsh;

		OAXSurf_Surface( &L, i, &s );
		variantOf[i] = -1;
		if ( ( s.flags & OSF_INVISIBLE ) || s.model >= numModels ) {
			if ( s.model >= numModels ) {
				ri.Printf( PRINT_WARNING, "WARNING: OAX_SURFACES surface %d: model %d does not exist\n", i, s.model );
			}
			sw.skipped++;
			continue;
		}
		Com_Memset( &key, 0, sizeof( key ) );
		Q_strncpyz( key.material, OAXSurf_MaterialName( &L, s.material ), sizeof( key.material ) );
		key.flags = s.flags & OSF_SHADER_BITS;
		for ( k = 0; k < 4; k++ ) {
			key.tint[k] = s.tint[k];
		}
		if ( !( key.flags & OSF_TRANSLUCENT ) ) {
			key.tint[3] = 1.0f;
		}
		for ( j = 0; j < sw.numVariants; j++ ) {
			if ( sw.variants[j].flags == key.flags && !memcmp( sw.variants[j].tint, key.tint, sizeof( key.tint ) )
				&& !Q_stricmp( sw.variants[j].material, key.material ) ) {
				break;
			}
		}
		if ( j == sw.numVariants ) {
			hsh = OAX_Fnv1a( 2166136261U, key.material, strlen( key.material ) );
			hsh = OAX_Fnv1a( hsh, &key.flags, sizeof( key.flags ) );
			hsh = OAX_Fnv1a( hsh, key.tint, sizeof( key.tint ) );
			Com_sprintf( key.name, sizeof( key.name ), "#oaxsurf%d_%08x", j, hsh );
			sw.variants[sw.numVariants++] = key;
		}
		variantOf[i] = j;
		addSurfs++;
		addVerts += s.numVerts;
		addIndexes += 3 * OAXSurf_NumTris( &s );
		if ( s.model == 0 ) {
			maxRefs += s.firstLeaf >= 0 ? s.numLeafs : 64;
		}
	}

	ns = numSurfs + addSurfs;
	nv = numVerts + addVerts;
	ni = numIndexes + addIndexes;
	outSurfs = ri.Malloc( sizeof( dsurface_t ) * ( ns + 1 ) );
	outVerts = ri.Malloc( sizeof( drawVert_t ) * ( nv + 1 ) );
	outIndexes = ri.Malloc( sizeof( int ) * ( ni + 1 ) );
	outShaders = ri.Malloc( sizeof( dshader_t ) * ( numShaders + sw.numVariants + 1 ) );
	outModels = ri.Malloc( sizeof( dmodel_t ) * numModels );
	outLeafs = ri.Malloc( sizeof( dleaf_t ) * ( numLeafs + 1 ) );
	newIndexOfOld = ri.Malloc( sizeof( int ) * ( numSurfs + 1 ) );
	newIndexOfLump = ri.Malloc( sizeof( int ) * ( L.numSurfaces + 1 ) );
	refs = ri.Malloc( sizeof( leafRef_t ) * maxRefs );
	found = ri.Malloc( sizeof( int ) * ( numLeafs + 1 ) );
	sw.recs = ri.Malloc( sizeof( surfRecord_t ) * ( addSurfs + 1 ) );
	sw.numVerts = L.numVerts;
	sw.verts = ri.Malloc( sizeof( oaxSurfVert_t ) * ( L.numVerts + 1 ) );
	for ( i = 0; i < L.numVerts; i++ ) {
		OAXSurf_Vert( &L, i, &sw.verts[i] );
	}

	// shaders: the map's, then one per variant
	Com_Memcpy( outShaders, shaders, sizeof( dshader_t ) * numShaders );
	for ( j = 0; j < sw.numVariants; j++ ) {
		dshader_t *d = &outShaders[numShaders + j];
		Com_Memset( d, 0, sizeof( *d ) );
		Q_strncpyz( d->shader, sw.variants[j].name, sizeof( d->shader ) );
	}
	Com_Memcpy( outVerts, verts, sizeof( drawVert_t ) * numVerts );
	Com_Memcpy( outIndexes, indexes, sizeof( int ) * numIndexes );
	for ( i = 0; i < numSurfs; i++ ) {
		newIndexOfOld[i] = -1;
	}

	// surfaces, model by model: the model's own, then its surface-world ones
	{
		int s = 0, v = numVerts, x = numIndexes;

		for ( k = 0; k < numModels; k++ ) {
			int first = LittleLong( models[k].firstSurface ), num = LittleLong( models[k].numSurfaces );

			outModels[k] = models[k];
			outModels[k].firstSurface = LittleLong( s );
			for ( i = first; i < first + num && i < numSurfs; i++ ) {
				if ( i >= 0 && newIndexOfOld[i] < 0 ) {
					outSurfs[s] = surfs[i];
					newIndexOfOld[i] = s++;
				}
			}
			for ( i = 0; i < L.numSurfaces; i++ ) {
				oaxSurf_t so;
				dsurface_t *d;
				surfRecord_t *rec;
				vec3_t *accum = NULL;
				int t;

				if ( variantOf[i] < 0 ) {
					continue;
				}
				OAXSurf_Surface( &L, i, &so );
				if ( so.model != k ) {
					continue;
				}
				d = &outSurfs[s];
				Com_Memset( d, 0, sizeof( *d ) );
				d->shaderNum = LittleLong( numShaders + variantOf[i] );
				d->fogNum = LittleLong( -1 );
				d->surfaceType = LittleLong( MST_TRIANGLE_SOUP );
				d->firstVert = LittleLong( v );
				d->numVerts = LittleLong( so.numVerts );
				d->firstIndex = LittleLong( x - 0 );
				d->numIndexes = LittleLong( 3 * OAXSurf_NumTris( &so ) );
				d->lightmapNum = LittleLong( LIGHTMAP_BY_VERTEX );

				// mesh normals: area-weighted triangle normals per vertex
				if ( so.numIndexes && !( so.flags & OSF_NORMALS ) ) {
					accum = ri.Malloc( sizeof( vec3_t ) * so.numVerts );
					Com_Memset( accum, 0, sizeof( vec3_t ) * so.numVerts );
					for ( t = 0; t < OAXSurf_NumTris( &so ); t++ ) {
						int tv[3];
						vec3_t e1, e2, n;

						OAXSurf_Tri( &L, &so, t, tv );
						VectorSubtract( sw.verts[tv[1]].xyz, sw.verts[tv[0]].xyz, e1 );
						VectorSubtract( sw.verts[tv[2]].xyz, sw.verts[tv[0]].xyz, e2 );
						CrossProduct( e1, e2, n );
						VectorAdd( accum[tv[0] - so.firstVert], n, accum[tv[0] - so.firstVert] );
						VectorAdd( accum[tv[1] - so.firstVert], n, accum[tv[1] - so.firstVert] );
						VectorAdd( accum[tv[2] - so.firstVert], n, accum[tv[2] - so.firstVert] );
					}
				}
				for ( j = 0; j < so.numVerts; j++ ) {
					const oaxSurfVert_t *sv = &sw.verts[so.firstVert + j];
					drawVert_t *dv = &outVerts[v + j];
					float c[4];

					Com_Memset( dv, 0, sizeof( *dv ) );
					VectorCopy( sv->xyz, dv->xyz );
					if ( so.flags & OSF_UVMATRIX ) {
						double p[3] = { sv->xyz[0], sv->xyz[1], sv->xyz[2] };
						dv->st[0] = (float)( so.uv[0][0] * p[0] + so.uv[0][1] * p[1] + so.uv[0][2] * p[2] + so.uv[0][3] );
						dv->st[1] = (float)( so.uv[1][0] * p[0] + so.uv[1][1] * p[1] + so.uv[1][2] * p[2] + so.uv[1][3] );
					} else {
						dv->st[0] = sv->st[0];
						dv->st[1] = sv->st[1];
					}
					if ( so.flags & OSF_NORMALS ) {
						VectorCopy( sv->normal, dv->normal );
					} else if ( accum ) {
						VectorCopy( accum[j], dv->normal );
						if ( !VectorNormalize( dv->normal ) ) {
							VectorSet( dv->normal, 0, 0, 1 );
						}
					} else {
						VectorCopy( so.plane, dv->normal );
					}
					// the tint is the shader variant's (oaxTint), not the vertex colours'
					for ( t = 0; t < 4; t++ ) {
						c[t] = ( so.flags & OSF_COLORS ) ? sv->color[t] : 255.0f;
						dv->color[t] = (byte)( c[t] < 0 ? 0 : c[t] > 255 ? 255 : c[t] + 0.5f );
					}
				}
				if ( accum ) {
					ri.Free( accum );
				}
				for ( t = 0; t < OAXSurf_NumTris( &so ); t++ ) {
					int tv[3];

					// the lump winds counter-clockwise seen from the front; Q3
					// draw surfaces wind clockwise
					OAXSurf_Tri( &L, &so, t, tv );
					outIndexes[x++] = tv[0] - so.firstVert;
					outIndexes[x++] = tv[2] - so.firstVert;
					outIndexes[x++] = tv[1] - so.firstVert;
				}
				v += so.numVerts;

				rec = &sw.recs[sw.numRecs++];
				rec->worldSurf = s;
				rec->lumpSurf = i;
				rec->lightMask = so.lightMask;
				rec->flags = so.flags;
				Vector4Copy( so.plane, rec->plane );
				Vector4Copy( so.tint, rec->tint );
				rec->firstVert = so.firstVert;
				rec->numVerts = so.numVerts;
				rec->variant = variantOf[i];
				newIndexOfLump[i] = s;

				// the leaves of a world surface: the lump's own list, plus
				// every leaf the surface's box reaches (a converter may list
				// the leaf of one point; a converted map's scenery or hull can
				// be one polygon thousands of units long, crossing hundreds of
				// leaves, and a leaf that does not list it never draws or
				// lights it). Duplicates go when the refs are sorted.
				if ( k == 0 ) {
					if ( so.firstLeaf >= 0 ) {
						for ( j = 0; j < so.numLeafs && numRefs < maxRefs; j++ ) {
							int lf = OAXSurf_LeafRef( &L, so.firstLeaf + j );
							if ( lf >= 0 && lf < numLeafs && LittleLong( leafs[lf].cluster ) != -1 ) {
								refs[numRefs].leaf = lf;
								refs[numRefs].surf = s;
								numRefs++;
							}
						}
					}
					{
						int n = 0;
						vec3_t mins, maxs;

						ClearBounds( mins, maxs );
						for ( j = 0; j < so.numVerts; j++ ) {
							AddPointToBounds( sw.verts[so.firstVert + j].xyz, mins, maxs );
						}
						for ( j = 0; j < 3; j++ ) {
							mins[j] -= 1.0f;
							maxs[j] += 1.0f;
						}
						BoxLeaves_r( file, h, 0, mins, maxs, found, &n, numLeafs );
						if ( numRefs + n > maxRefs ) {
							leafRef_t *grown;

							maxRefs = ( numRefs + n ) * 2;
							grown = ri.Malloc( sizeof( leafRef_t ) * maxRefs );
							Com_Memcpy( grown, refs, sizeof( leafRef_t ) * numRefs );
							ri.Free( refs );
							refs = grown;
						}
						for ( j = 0; j < n && numRefs < maxRefs; j++ ) {
							refs[numRefs].leaf = found[j];
							refs[numRefs].surf = s;
							numRefs++;
						}
					}
				}
				s++;
			}
			outModels[k].numSurfaces = LittleLong( s - LittleLong( outModels[k].firstSurface ) );
		}
		// surfaces no model listed (q3map2 writes none): keep them, last
		for ( i = 0; i < numSurfs; i++ ) {
			if ( newIndexOfOld[i] < 0 ) {
				outSurfs[s] = surfs[i];
				newIndexOfOld[i] = s++;
			}
		}
		ns = s;
		nv = v;
		ni = x;
	}
	// leaf surfaces: each leaf's own (renumbered) then its surface-world ones
	{
		int n = 0, r = 0, filledRefs = numRefs;

		qsort( refs, filledRefs, sizeof( leafRef_t ), LeafRefCompare );
		nls = numLeafSurfs + filledRefs;
		outLeafSurfs = ri.Malloc( sizeof( int ) * ( nls + 1 ) );
		for ( i = 0; i < numLeafs; i++ ) {
			int first = LittleLong( leafs[i].firstLeafSurface ), num = LittleLong( leafs[i].numLeafSurfaces );

			outLeafs[i] = leafs[i];
			outLeafs[i].firstLeafSurface = LittleLong( n );
			for ( j = first; j < first + num && j < numLeafSurfs; j++ ) {
				int old = LittleLong( leafSurfs[j] );
				outLeafSurfs[n++] = LittleLong( old >= 0 && old < numSurfs ? newIndexOfOld[old] : old );
			}
			while ( r < filledRefs && refs[r].leaf < i ) {
				r++;
			}
			while ( r < filledRefs && refs[r].leaf == i ) {
				if ( r == 0 || refs[r].leaf != refs[r - 1].leaf || refs[r].surf != refs[r - 1].surf ) {
					outLeafSurfs[n++] = LittleLong( refs[r].surf );
				}
				r++;
			}
			outLeafs[i].numLeafSurfaces = LittleLong( n - LittleLong( outLeafs[i].firstLeafSurface ) );
		}
		nls = n;
	}

	// the copy: the whole file, then the new lumps
	size = fileLen + sizeof( dshader_t ) * ( numShaders + sw.numVariants ) + sizeof( drawVert_t ) * nv + sizeof( int ) * ni
		+ sizeof( dsurface_t ) * ns + sizeof( int ) * nls + sizeof( dleaf_t ) * numLeafs + sizeof( dmodel_t ) * numModels + 64;
	sw.fileCopy = ri.Malloc( size );
	Com_Memcpy( sw.fileCopy, file, fileLen );
	nh = (dheader_t *)sw.fileCopy;
	at = fileLen;
	AppendLump( sw.fileCopy, &at, nh, LUMP_SHADERS, outShaders, sizeof( dshader_t ) * ( numShaders + sw.numVariants ) );
	AppendLump( sw.fileCopy, &at, nh, LUMP_DRAWVERTS, outVerts, sizeof( drawVert_t ) * nv );
	AppendLump( sw.fileCopy, &at, nh, LUMP_DRAWINDEXES, outIndexes, sizeof( int ) * ni );
	AppendLump( sw.fileCopy, &at, nh, LUMP_SURFACES, outSurfs, sizeof( dsurface_t ) * ns );
	AppendLump( sw.fileCopy, &at, nh, LUMP_LEAFSURFACES, outLeafSurfs, sizeof( int ) * nls );
	AppendLump( sw.fileCopy, &at, nh, LUMP_LEAFS, outLeafs, sizeof( dleaf_t ) * numLeafs );
	AppendLump( sw.fileCopy, &at, nh, LUMP_MODELS, outModels, sizeof( dmodel_t ) * numModels );

	ri.Free( variantOf );
	ri.Free( outSurfs );
	ri.Free( outVerts );
	ri.Free( outIndexes );
	ri.Free( outShaders );
	ri.Free( outModels );
	ri.Free( outLeafs );
	ri.Free( outLeafSurfs );
	ri.Free( newIndexOfOld );
	ri.Free( newIndexOfLump );
	ri.Free( refs );
	ri.Free( found );

	ri.Printf( PRINT_ALL, "OAX_SURFACES: %d surfaces (%d skipped), %d triangles, %d materials, %d shader variants\n",
		sw.numRecs, sw.skipped, addIndexes / 3, L.numMaterials, sw.numVariants );
	return nh;
}

/*
=================
R_OAXSurfWorldSetup

After the world loaded (tr.world set), before unified lighting computes
its light interactions: light masks, plane culling and tangents of the
surface-world surfaces, and unknown materials.
=================
*/
void R_OAXSurfWorldSetup( void ) {
	int i, j;

	if ( !tr.world ) {
		return;
	}
	sw.unknown = 0;
	sw.unknownNames[0] = 0;

	for ( i = 0; i < sw.numRecs; i++ ) {
		const surfRecord_t *rec = &sw.recs[i];
		msurface_t *ms;
		srfBspSurface_t *srf;

		if ( rec->worldSurf < 0 || rec->worldSurf >= tr.world->numsurfaces ) {
			continue;
		}
		ms = &tr.world->surfaces[rec->worldSurf];
		srf = (srfBspSurface_t *)ms->data;
		ms->oaxLightMask = rec->lightMask;
		if ( ms->shader == tr.defaultShader ) {
			const char *mat = sw.variants[rec->variant].material;

			sw.unknown++;
			if ( !strstr( sw.unknownNames, mat ) && strlen( sw.unknownNames ) + strlen( mat ) + 2 < sizeof( sw.unknownNames ) ) {
				Q_strcat( sw.unknownNames, sizeof( sw.unknownNames ), va( "%s%s", sw.unknownNames[0] ? " " : "", mat ) );
			}
		}
		// a polygon culls by its plane, like a brush face
		if ( !( rec->plane[0] == 0 && rec->plane[1] == 0 && rec->plane[2] == 0 ) ) {
			VectorCopy( rec->plane, ms->cullinfo.plane.normal );
			ms->cullinfo.plane.dist = rec->plane[3];
			ms->cullinfo.plane.type = PlaneTypeForNormal( ms->cullinfo.plane.normal );
			SetPlaneSignbits( &ms->cullinfo.plane );
			ms->cullinfo.type |= CULLINFO_PLANE;
		}
		if ( !srf || srf->surfaceType != SF_TRIANGLES || srf->numVerts != rec->numVerts || !( rec->flags & OSF_TANGENTS ) ) {
			continue;
		}
		for ( j = 0; j < srf->numVerts; j++ ) {
			vec4_t t;
			Vector4Copy( sw.verts[rec->firstVert + j].tangent, t );
			R_VaoPackTangent( srf->verts[j].tangent, t );
		}
	}
}

/*
=================
R_OAXSurfWorldFinishLoad

After unified lighting loaded (its model decided, the ambient floor
written): vertex colours of the surface-world surfaces, the lighting
statistics. Frees the rewritten file.
=================
*/
void R_OAXSurfWorldFinishLoad( void ) {
	int i, j;
	qboolean grid, unified;
	vec3_t surfAmbient;
	int zoned = 0;

	if ( sw.fileCopy ) {
		ri.Free( sw.fileCopy );
		sw.fileCopy = NULL;
	}
	if ( !tr.world ) {
		return;
	}
	unified = R_ULightLightingModel() == ULIGHT_UNIFIED;
	grid = !unified && tr.world->lightGridData != NULL;

	for ( i = 0; i < sw.numRecs; i++ ) {
		const surfRecord_t *rec = &sw.recs[i];
		srfBspSurface_t *srf;

		if ( rec->worldSurf < 0 || rec->worldSurf >= tr.world->numsurfaces ) {
			continue;
		}
		srf = (srfBspSurface_t *)tr.world->surfaces[rec->worldSurf].data;
		if ( !srf || srf->surfaceType != SF_TRIANGLES || srf->numVerts != rec->numVerts ) {
			continue;
		}
		// unified: the surface's ambient floor, a zone's (func_oax_zone
		// `ambient`, by the zone its centre faces into) else the world's
		if ( unified ) {
			vec3_t centre, normal, n, p;

			VectorCopy( ulw.ambient, surfAmbient );
			if ( ulw.zoneAmbient && srf->numVerts ) {
				VectorClear( centre );
				VectorClear( normal );
				for ( j = 0; j < srf->numVerts; j++ ) {
					R_VaoUnpackNormal( n, srf->verts[j].normal );
					VectorAdd( centre, srf->verts[j].xyz, centre );
					VectorAdd( normal, n, normal );
				}
				VectorScale( centre, 1.0f / srf->numVerts, centre );
				VectorNormalize( normal );
				VectorMA( centre, 2.0f, normal, p );
				if ( R_ULightZoneAmbientAt( p, n ) ) {
					VectorCopy( n, surfAmbient );
					zoned++;
				}
			}
		}
		for ( j = 0; j < srf->numVerts; j++ ) {
			const oaxSurfVert_t *sv = &sw.verts[rec->firstVert + j];
			vec4_t c;

			if ( rec->flags & OSF_COLORS ) {
				VectorSet4( c, sv->color[0] / 255.0f, sv->color[1] / 255.0f, sv->color[2] / 255.0f, sv->color[3] / 255.0f );
			} else {
				VectorSet4( c, 1, 1, 1, 1 );
			}
			if ( unified ) {
				// the ambient floor every stock stage multiplies by (tr_ulight.c)
				VectorScale( c, tr.identityLight, c );
				if ( rec->flags & OSF_TRANSLUCENT ) {
					// the pane's exact-vertex stage scales by the overbright and so
					// does the frame: ambient x texel on screen, like the opaque
					// surfaces' ambient pass
					VectorScale( c, tr.identityLight, c );
				}
				c[0] *= surfAmbient[0];
				c[1] *= surfAmbient[1];
				c[2] *= surfAmbient[2];
			} else if ( grid ) {
				vec3_t amb, dir, dirCol, n;
				float d, scale = ( 1 << ( r_mapOverBrightBits->integer - tr.overbrightBits ) ) / 255.0f, mx;

				R_LightForPointWorld( tr.world, srf->verts[j].xyz, amb, dirCol, dir );
				R_VaoUnpackNormal( n, srf->verts[j].normal );
				d = DotProduct( n, dir );
				if ( d < 0 ) {
					d = 0;
				}
				amb[0] = ( amb[0] + dirCol[0] * d ) * scale;
				amb[1] = ( amb[1] + dirCol[1] * d ) * scale;
				amb[2] = ( amb[2] + dirCol[2] * d ) * scale;
				mx = MAX( amb[0], MAX( amb[1], amb[2] ) );
				if ( mx > 1.0f ) {
					VectorScale( amb, 1.0f / mx, amb );
				}
				c[0] *= amb[0];
				c[1] *= amb[1];
				c[2] *= amb[2];
			}
			R_VaoPackColor( srf->verts[j].color, c );
		}
	}
	// unified lighting: which surfaces the map's lights reach, and the
	// pairs light-mask groups keep apart (tests read these)
	{
		int lit = 0, pairs = 0, excluded = 0, violations = 0, noShadow = 0, noShadowLit = 0, l, k;

		for ( i = 0; i < sw.numRecs; i++ ) {
			const surfRecord_t *rec = &sw.recs[i];
			msurface_t *ms;
			unsigned mask = rec->lightMask ? rec->lightMask : 1u;
			qboolean any = qfalse;

			if ( rec->worldSurf < 0 || rec->worldSurf >= tr.world->numWorldSurfaces ) {
				continue;
			}
			ms = &tr.world->surfaces[rec->worldSurf];
			for ( l = 0; l < ulw.numMapLights; l++ ) {
				const uLight_t *ul = &ulw.lights[l];
				qboolean in = qfalse;

				for ( k = 0; k < ul->numWorldSurfs; k++ ) {
					if ( ul->worldSurfs[k] == rec->worldSurf ) {
						in = qtrue;
						break;
					}
				}
				if ( in ) {
					pairs++;
					any = qtrue;
					if ( !( mask & ul->parms.lightMask ) ) {
						violations++;
					}
				} else if ( !( mask & ul->parms.lightMask ) && ( ms->cullinfo.type & CULLINFO_BOX )
					&& ms->cullinfo.bounds[0][0] <= ul->bounds[1][0] && ms->cullinfo.bounds[1][0] >= ul->bounds[0][0]
					&& ms->cullinfo.bounds[0][1] <= ul->bounds[1][1] && ms->cullinfo.bounds[1][1] >= ul->bounds[0][1]
					&& ms->cullinfo.bounds[0][2] <= ul->bounds[1][2] && ms->cullinfo.bounds[1][2] >= ul->bounds[0][2] ) {
					excluded++;
				}
			}
			if ( any ) {
				lit++;
			}
			if ( rec->flags & OSF_NOSHADOW ) {
				noShadow++;
				noShadowLit += any;
			}
		}
		if ( ri.DebugSet ) {
			ri.DebugSet( "r_surfworld_lit", va( "%d", lit ) );
			ri.DebugSet( "r_surfworld_noshadow", va( "%d", noShadow ) );
			ri.DebugSet( "r_surfworld_noshadow_lit", va( "%d", noShadowLit ) );
			ri.DebugSet( "r_surfworld_interactions", va( "%d", pairs ) );
			ri.DebugSet( "r_surfworld_mask_excluded", va( "%d", excluded ) );
			ri.DebugSet( "r_surfworld_mask_violations", va( "%d", violations ) );
		}
	}
	if ( sw.unknown ) {
		ri.Printf( PRINT_WARNING, "WARNING: OAX_SURFACES: %d surfaces have unknown materials: %s\n", sw.unknown, sw.unknownNames );
	}
	if ( ri.DebugSet ) {
		ri.DebugSet( "r_surfworld_surfaces", va( "%d", sw.numRecs ) );
		ri.DebugSet( "r_surfworld_skipped", va( "%d", sw.skipped ) );
		ri.DebugSet( "r_surfworld_variants", va( "%d", sw.numVariants ) );
		ri.DebugSet( "r_surfworld_unknown", va( "%d", sw.unknown ) );
		ri.DebugSet( "r_surfworld_zone_ambient", va( "%d", zoned ) );
		ri.DebugSet( "r_surfworld_unknown_names", sw.unknownNames );
		ri.DebugSet( "r_surfworld_hash", va( "%08x", sw.numRecs ? sw.hash : 0 ) );
	}
}

/*
=================
R_OAXSurfWorldCountView

How many surface-world surfaces the current view's PVS and areas reached.
=================
*/
int R_OAXSurfWorldCountView( void ) {
	int i, n = 0;

	if ( !tr.world ) {
		return 0;
	}
	for ( i = 0; i < sw.numRecs; i++ ) {
		int s = sw.recs[i].worldSurf;
		if ( s >= 0 && s < tr.world->numWorldSurfaces && tr.world->surfacesViewCount[s] == tr.viewCount ) {
			n++;
		}
	}
	return n;
}

qboolean R_OAXSurfWorldActive( void ) {
	return sw.numRecs > 0;
}
