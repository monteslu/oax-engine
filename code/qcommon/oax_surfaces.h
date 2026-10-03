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
oax_surfaces.h: the surface world (OAX_SURFACES) and collision mesh
(OAX_COLLISION) BSPX lumps, step 7.5.

The format is specified in docs/map-format.md (sections "OAX_SURFACES" and
"OAX_COLLISION"); the writer is misc/tools/oax-surfaces.mjs. This file
parses and validates both lumps for every consumer: the collision model
(cm_oaxsurf.c: load-time validation, collision meshes), the navmesh, the
physics module and the renderer (renderergl2/tr_surfworld.c).

Compiled into the engine and into the renderer module: only the C
library, no engine allocation. Records are read byte by byte (no
alignment or host-endianness assumptions) with the strides the lump's
header gives, so a later version that appends fields still parses.
===========================================================================
*/
#ifndef OAX_SURFACES_H
#define OAX_SURFACES_H

#define OAX_SURFACES_LUMP		"OAX_SURFACES"
#define OAX_SURFACES_IDENT		"OSRF"
#define OAX_SURFACES_VERSION	1
#define OAX_SURFACES_HEADER		80
#define OAX_SURF_MATERIAL_SIZE	72
#define OAX_SURF_SURFACE_SIZE	112
#define OAX_SURF_VERT_SIZE		52
#define OAX_SURF_NAME			64

#define OAX_COLLISION_LUMP		"OAX_COLLISION"
#define OAX_COLLISION_IDENT		"OCOL"
#define OAX_COLLISION_VERSION	1
#define OAX_COLLISION_HEADER	48
#define OAX_COLL_MESH_SIZE		48
#define OAX_COLL_VERT_SIZE		12

// surface flags (OSF_*)
#define OSF_TWOSIDED	0x001
#define OSF_MASKED		0x002
#define OSF_TRANSLUCENT	0x004
#define OSF_ADDITIVE	0x008
#define OSF_INVISIBLE	0x010
#define OSF_UVMATRIX	0x020
#define OSF_NORMALS		0x040
#define OSF_TANGENTS	0x080
#define OSF_COLORS		0x100
#define OSF_DETAIL		0x200
#define OSF_NOSHADOW	0x400
#define OSF_KNOWN		0x7ff

// flags that change how the material is drawn (a shader variant per set)
#define OSF_SHADER_BITS	( OSF_TWOSIDED | OSF_MASKED | OSF_TRANSLUCENT | OSF_ADDITIVE | OSF_NOSHADOW )

typedef struct {
	int			numMaterials, numSurfaces, numVerts, numIndexes, numLeafRefs;
	const unsigned char	*materials, *surfaces, *verts, *indexes, *leafRefs;
	int			materialSize, surfaceSize, vertSize;
	const unsigned char	*lump;
	int			lumpLen;
} oaxSurfLump_t;

typedef struct {
	int			material;
	unsigned	flags;
	unsigned	lightMask;
	int			model;
	float		tint[4];
	float		uv[2][4];
	float		plane[4];
	int			firstVert, numVerts;
	int			firstIndex, numIndexes;
	int			firstLeaf, numLeafs;
	int			area;
	int			sourceId;
} oaxSurf_t;

typedef struct {
	float		xyz[3];
	float		st[2];
	float		normal[3];
	float		tangent[4];
	unsigned char color[4];
} oaxSurfVert_t;

typedef struct {
	int			numMeshes, numVerts, numIndexes;
	const unsigned char	*meshes, *verts, *indexes;
	int			meshSize, vertSize;
	const unsigned char	*lump;
	int			lumpLen;
} oaxCollLump_t;

typedef struct {
	int			contents;
	int			surfaceFlags;
	float		thickness;
	int			model;
	int			firstVert, numVerts;
	int			firstIndex, numIndexes;
	int			sourceId;
} oaxCollMesh_t;

// A BSPX lump by name in a whole BSP file in memory; NULL if absent.
const void *OAX_FindBspxLump( const void *bsp, int bspLen, const char *name, int *outLen );

// Parse and validate OAX_SURFACES: every table inside the lump, every
// surface's ranges inside its tables, every index inside its surface's
// vertices, every material index valid. Returns 1, or 0 with a reason.
int OAXSurf_Parse( const void *lump, int len, oaxSurfLump_t *out, char *err, int errSize );
void OAXSurf_Surface( const oaxSurfLump_t *l, int n, oaxSurf_t *out );
void OAXSurf_Vert( const oaxSurfLump_t *l, int n, oaxSurfVert_t *out );
int OAXSurf_Index( const oaxSurfLump_t *l, int n );
int OAXSurf_LeafRef( const oaxSurfLump_t *l, int n );
const char *OAXSurf_MaterialName( const oaxSurfLump_t *l, int n );	// NUL terminated inside the lump
// number of triangles of a surface (a polygon fans: numVerts - 2)
int OAXSurf_NumTris( const oaxSurf_t *s );
// the three vertex numbers (absolute) of triangle t of a surface
void OAXSurf_Tri( const oaxSurfLump_t *l, const oaxSurf_t *s, int t, int v[3] );

// Parse and validate OAX_COLLISION.
int OAXColl_Parse( const void *lump, int len, oaxCollLump_t *out, char *err, int errSize );
void OAXColl_Mesh( const oaxCollLump_t *l, int n, oaxCollMesh_t *out );
void OAXColl_Vert( const oaxCollLump_t *l, int n, float xyz[3] );
int OAXColl_Index( const oaxCollLump_t *l, int n );

// FNV-1a over bytes, for hashes tests compare across builds
unsigned OAX_Fnv1a( unsigned h, const void *data, int len );

#endif
