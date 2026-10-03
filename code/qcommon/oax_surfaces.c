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
oax_surfaces.c: OAX_SURFACES and OAX_COLLISION parsing (see oax_surfaces.h
and docs/map-format.md).
===========================================================================
*/

#include <string.h>
#include <stdio.h>
#include "oax_surfaces.h"

static int RdInt( const unsigned char *p ) {
	return (int)( (unsigned)p[0] | ( (unsigned)p[1] << 8 ) | ( (unsigned)p[2] << 16 ) | ( (unsigned)p[3] << 24 ) );
}

static float RdFloat( const unsigned char *p ) {
	union { int i; float f; } u;
	u.i = RdInt( p );
	return u.f;
}

static int Err( char *err, int errSize, const char *msg ) {
	if ( err && errSize > 0 ) {
		strncpy( err, msg, errSize - 1 );
		err[errSize - 1] = 0;
	}
	return 0;
}

unsigned OAX_Fnv1a( unsigned h, const void *data, int len ) {
	const unsigned char *p = data;
	int i;

	for ( i = 0; i < len; i++ ) {
		h = ( h ^ p[i] ) * 16777619U;
	}
	return h;
}

/*
=================
OAX_FindBspxLump

The BSPX directory sits after the last standard lump, 4-byte aligned:
"BSPX", count, count x { char name[24]; int ofs; int len; }.
=================
*/
const void *OAX_FindBspxLump( const void *bsp, int bspLen, const char *name, int *outLen ) {
	const unsigned char *b = bsp;
	int i, end = 8 + 17 * 8, at, count;

	if ( !b || bspLen < end || memcmp( b, "IBSP", 4 ) ) {
		return NULL;
	}
	for ( i = 0; i < 17; i++ ) {
		int e = RdInt( b + 8 + i * 8 ) + RdInt( b + 12 + i * 8 );
		if ( e > end ) {
			end = e;
		}
	}
	at = ( end + 3 ) & ~3;
	if ( at + 8 > bspLen || memcmp( b + at, "BSPX", 4 ) ) {
		return NULL;
	}
	count = RdInt( b + at + 4 );
	if ( count < 0 || count > 4096 || at + 8 + count * 32 > bspLen ) {
		return NULL;
	}
	for ( i = 0; i < count; i++ ) {
		const unsigned char *e = b + at + 8 + i * 32;
		int ofs = RdInt( e + 24 ), len = RdInt( e + 28 );
		if ( strncmp( (const char *)e, name, 24 ) ) {
			continue;
		}
		if ( ofs < 0 || len < 0 || ofs > bspLen || len > bspLen - ofs ) {
			return NULL;
		}
		if ( outLen ) {
			*outLen = len;
		}
		return b + ofs;
	}
	return NULL;
}

// a table: count records of `size` bytes at ofs, inside the lump
static int Table( const unsigned char *lump, int len, const unsigned char *h, int minSize,
	int *count, const unsigned char **data, int *size, const char *what, char *err, int errSize ) {
	char msg[96];
	int n = RdInt( h ), ofs = RdInt( h + 4 ), sz = minSize ? RdInt( h + 8 ) : 4;

	if ( n < 0 || ofs < 0 || ( ofs & 3 ) || sz < ( minSize ? minSize : 4 ) || sz > 4096 ) {
		snprintf( msg, sizeof( msg ), "bad %s table (count %d, offset %d, size %d)", what, n, ofs, sz );
		return Err( err, errSize, msg );
	}
	if ( ofs > len || ( n && n > ( len - ofs ) / sz ) ) {
		snprintf( msg, sizeof( msg ), "%s table runs past the lump", what );
		return Err( err, errSize, msg );
	}
	*count = n;
	*data = lump + ofs;
	if ( size ) {
		*size = sz;
	}
	return 1;
}

/*
=================
OAXSurf_Parse
=================
*/
int OAXSurf_Parse( const void *lumpData, int len, oaxSurfLump_t *out, char *err, int errSize ) {
	const unsigned char *lump = lumpData;
	char msg[128];
	int version, headerSize, i, j;

	memset( out, 0, sizeof( *out ) );
	if ( !lump || len < OAX_SURFACES_HEADER || memcmp( lump, OAX_SURFACES_IDENT, 4 ) ) {
		return Err( err, errSize, "not an OSRF lump" );
	}
	version = RdInt( lump + 4 );
	headerSize = RdInt( lump + 8 );
	if ( version != OAX_SURFACES_VERSION ) {
		snprintf( msg, sizeof( msg ), "version %d (this engine reads %d)", version, OAX_SURFACES_VERSION );
		return Err( err, errSize, msg );
	}
	if ( headerSize < OAX_SURFACES_HEADER || headerSize > len ) {
		return Err( err, errSize, "bad header size" );
	}
	if ( !Table( lump, len, lump + 16, OAX_SURF_MATERIAL_SIZE, &out->numMaterials, &out->materials, &out->materialSize, "material", err, errSize )
		|| !Table( lump, len, lump + 28, OAX_SURF_SURFACE_SIZE, &out->numSurfaces, &out->surfaces, &out->surfaceSize, "surface", err, errSize )
		|| !Table( lump, len, lump + 40, OAX_SURF_VERT_SIZE, &out->numVerts, &out->verts, &out->vertSize, "vertex", err, errSize )
		|| !Table( lump, len, lump + 52, 0, &out->numIndexes, &out->indexes, NULL, "index", err, errSize )
		|| !Table( lump, len, lump + 64, 0, &out->numLeafRefs, &out->leafRefs, NULL, "leaf reference", err, errSize ) ) {
		return 0;
	}
	if ( RdInt( lump + 60 ) != 4 || RdInt( lump + 72 ) != 4 ) {
		return Err( err, errSize, "index and leaf reference size must be 4" );
	}
	out->lump = lump;
	out->lumpLen = len;

	for ( i = 0; i < out->numMaterials; i++ ) {
		const unsigned char *m = out->materials + i * out->materialSize;
		if ( !memchr( m, 0, OAX_SURF_NAME ) || !m[0] ) {
			snprintf( msg, sizeof( msg ), "material %d has no NUL-terminated name", i );
			return Err( err, errSize, msg );
		}
	}
	for ( i = 0; i < out->numSurfaces; i++ ) {
		oaxSurf_t s;

		OAXSurf_Surface( out, i, &s );
		if ( s.material < 0 || s.material >= out->numMaterials ) {
			snprintf( msg, sizeof( msg ), "surface %d: material %d out of range", i, s.material );
			return Err( err, errSize, msg );
		}
		if ( s.flags & ~OSF_KNOWN ) {
			snprintf( msg, sizeof( msg ), "surface %d: unknown flags %x", i, s.flags & ~OSF_KNOWN );
			return Err( err, errSize, msg );
		}
		if ( s.model < 0 ) {
			snprintf( msg, sizeof( msg ), "surface %d: model %d", i, s.model );
			return Err( err, errSize, msg );
		}
		if ( s.numVerts < 3 || s.firstVert < 0 || s.firstVert > out->numVerts - s.numVerts ) {
			snprintf( msg, sizeof( msg ), "surface %d: vertex range %d+%d out of range", i, s.firstVert, s.numVerts );
			return Err( err, errSize, msg );
		}
		if ( s.numIndexes < 0 || s.numIndexes % 3 || s.firstIndex < 0 || s.firstIndex > out->numIndexes - s.numIndexes ) {
			snprintf( msg, sizeof( msg ), "surface %d: index range %d+%d out of range", i, s.firstIndex, s.numIndexes );
			return Err( err, errSize, msg );
		}
		for ( j = 0; j < s.numIndexes; j++ ) {
			int v = OAXSurf_Index( out, s.firstIndex + j );
			if ( v < 0 || v >= s.numVerts ) {
				snprintf( msg, sizeof( msg ), "surface %d: index %d outside its %d vertices", i, v, s.numVerts );
				return Err( err, errSize, msg );
			}
		}
		if ( s.firstLeaf != -1 && ( s.numLeafs < 0 || s.firstLeaf < 0 || s.firstLeaf > out->numLeafRefs - s.numLeafs ) ) {
			snprintf( msg, sizeof( msg ), "surface %d: leaf range %d+%d out of range", i, s.firstLeaf, s.numLeafs );
			return Err( err, errSize, msg );
		}
	}
	return 1;
}

void OAXSurf_Surface( const oaxSurfLump_t *l, int n, oaxSurf_t *s ) {
	const unsigned char *p = l->surfaces + n * l->surfaceSize;
	int i;

	s->material = RdInt( p );
	s->flags = (unsigned)RdInt( p + 4 );
	s->lightMask = (unsigned)RdInt( p + 8 );
	s->model = RdInt( p + 12 );
	for ( i = 0; i < 4; i++ ) {
		s->tint[i] = RdFloat( p + 16 + i * 4 );
		s->uv[0][i] = RdFloat( p + 32 + i * 4 );
		s->uv[1][i] = RdFloat( p + 48 + i * 4 );
		s->plane[i] = RdFloat( p + 64 + i * 4 );
	}
	s->firstVert = RdInt( p + 80 );
	s->numVerts = RdInt( p + 84 );
	s->firstIndex = RdInt( p + 88 );
	s->numIndexes = RdInt( p + 92 );
	s->firstLeaf = RdInt( p + 96 );
	s->numLeafs = RdInt( p + 100 );
	s->area = RdInt( p + 104 );
	s->sourceId = RdInt( p + 108 );
}

void OAXSurf_Vert( const oaxSurfLump_t *l, int n, oaxSurfVert_t *v ) {
	const unsigned char *p = l->verts + n * l->vertSize;
	int i;

	for ( i = 0; i < 3; i++ ) {
		v->xyz[i] = RdFloat( p + i * 4 );
		v->normal[i] = RdFloat( p + 20 + i * 4 );
	}
	v->st[0] = RdFloat( p + 12 );
	v->st[1] = RdFloat( p + 16 );
	for ( i = 0; i < 4; i++ ) {
		v->tangent[i] = RdFloat( p + 32 + i * 4 );
		v->color[i] = p[48 + i];
	}
}

int OAXSurf_Index( const oaxSurfLump_t *l, int n ) {
	return RdInt( l->indexes + n * 4 );
}

int OAXSurf_LeafRef( const oaxSurfLump_t *l, int n ) {
	return RdInt( l->leafRefs + n * 4 );
}

const char *OAXSurf_MaterialName( const oaxSurfLump_t *l, int n ) {
	return (const char *)( l->materials + n * l->materialSize );
}

int OAXSurf_NumTris( const oaxSurf_t *s ) {
	return s->numIndexes ? s->numIndexes / 3 : s->numVerts - 2;
}

void OAXSurf_Tri( const oaxSurfLump_t *l, const oaxSurf_t *s, int t, int v[3] ) {
	if ( s->numIndexes ) {
		v[0] = s->firstVert + OAXSurf_Index( l, s->firstIndex + t * 3 );
		v[1] = s->firstVert + OAXSurf_Index( l, s->firstIndex + t * 3 + 1 );
		v[2] = s->firstVert + OAXSurf_Index( l, s->firstIndex + t * 3 + 2 );
	} else {
		v[0] = s->firstVert;
		v[1] = s->firstVert + t + 1;
		v[2] = s->firstVert + t + 2;
	}
}

/*
=================
OAXColl_Parse
=================
*/
int OAXColl_Parse( const void *lumpData, int len, oaxCollLump_t *out, char *err, int errSize ) {
	const unsigned char *lump = lumpData;
	char msg[128];
	int version, headerSize, i, j, ofs, n;

	memset( out, 0, sizeof( *out ) );
	if ( !lump || len < OAX_COLLISION_HEADER || memcmp( lump, OAX_COLLISION_IDENT, 4 ) ) {
		return Err( err, errSize, "not an OCOL lump" );
	}
	version = RdInt( lump + 4 );
	headerSize = RdInt( lump + 8 );
	if ( version != OAX_COLLISION_VERSION ) {
		snprintf( msg, sizeof( msg ), "version %d (this engine reads %d)", version, OAX_COLLISION_VERSION );
		return Err( err, errSize, msg );
	}
	if ( headerSize < OAX_COLLISION_HEADER || headerSize > len ) {
		return Err( err, errSize, "bad header size" );
	}
	if ( !Table( lump, len, lump + 16, OAX_COLL_MESH_SIZE, &out->numMeshes, &out->meshes, &out->meshSize, "mesh", err, errSize )
		|| !Table( lump, len, lump + 28, OAX_COLL_VERT_SIZE, &out->numVerts, &out->verts, &out->vertSize, "vertex", err, errSize ) ) {
		return 0;
	}
	n = RdInt( lump + 40 );
	ofs = RdInt( lump + 44 );
	if ( n < 0 || ofs < 0 || ( ofs & 3 ) || ofs > len || n > ( len - ofs ) / 4 ) {
		return Err( err, errSize, "bad index table" );
	}
	out->numIndexes = n;
	out->indexes = lump + ofs;
	out->lump = lump;
	out->lumpLen = len;
	for ( i = 0; i < out->numMeshes; i++ ) {
		oaxCollMesh_t m;

		OAXColl_Mesh( out, i, &m );
		if ( m.numVerts < 3 || m.firstVert < 0 || m.firstVert > out->numVerts - m.numVerts ) {
			snprintf( msg, sizeof( msg ), "mesh %d: vertex range out of range", i );
			return Err( err, errSize, msg );
		}
		if ( m.numIndexes < 3 || m.numIndexes % 3 || m.firstIndex < 0 || m.firstIndex > out->numIndexes - m.numIndexes ) {
			snprintf( msg, sizeof( msg ), "mesh %d: index range out of range", i );
			return Err( err, errSize, msg );
		}
		if ( !( m.thickness > 0.0f ) || m.thickness > 4096.0f ) {
			snprintf( msg, sizeof( msg ), "mesh %d: thickness must be in (0, 4096]", i );
			return Err( err, errSize, msg );
		}
		if ( m.model != 0 ) {
			snprintf( msg, sizeof( msg ), "mesh %d: model %d (version 1 has world meshes only)", i, m.model );
			return Err( err, errSize, msg );
		}
		for ( j = 0; j < m.numIndexes; j++ ) {
			int v = OAXColl_Index( out, m.firstIndex + j );
			if ( v < 0 || v >= m.numVerts ) {
				snprintf( msg, sizeof( msg ), "mesh %d: index %d outside its vertices", i, v );
				return Err( err, errSize, msg );
			}
		}
	}
	return 1;
}

void OAXColl_Mesh( const oaxCollLump_t *l, int n, oaxCollMesh_t *m ) {
	const unsigned char *p = l->meshes + n * l->meshSize;

	m->contents = RdInt( p );
	m->surfaceFlags = RdInt( p + 4 );
	m->thickness = RdFloat( p + 8 );
	m->model = RdInt( p + 12 );
	m->firstVert = RdInt( p + 16 );
	m->numVerts = RdInt( p + 20 );
	m->firstIndex = RdInt( p + 24 );
	m->numIndexes = RdInt( p + 28 );
	m->sourceId = RdInt( p + 32 );
}

void OAXColl_Vert( const oaxCollLump_t *l, int n, float xyz[3] ) {
	const unsigned char *p = l->verts + n * l->vertSize;

	xyz[0] = RdFloat( p );
	xyz[1] = RdFloat( p + 4 );
	xyz[2] = RdFloat( p + 8 );
}

int OAXColl_Index( const oaxCollLump_t *l, int n ) {
	return RdInt( l->indexes + n * 4 );
}
