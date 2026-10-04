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
oax_terrain.c: the OAX_TERRAIN lump (see oax_terrain.h).

Compiled into the engine (collision, navigation) and into the renderer
module, so it uses nothing but the C library and exact float operations:
+, -, *, / and sqrtf are correctly rounded on every build, and the file is
compiled with -ffp-contract=off, so heights, normals and foliage placement
are the same bits on native and wasm.
===========================================================================
*/

#include <string.h>
#include <math.h>
#include "oax_terrain.h"

static int RdInt( const unsigned char *p ) {
	return (int)( (unsigned)p[0] | ( (unsigned)p[1] << 8 ) | ( (unsigned)p[2] << 16 ) | ( (unsigned)p[3] << 24 ) );
}

static float RdFloat( const unsigned char *p ) {
	union { int i; float f; } u;
	u.i = RdInt( p );
	return u.f;
}

static void Err( char *err, int errSize, const char *msg ) {
	if ( err && errSize > 0 ) {
		strncpy( err, msg, errSize - 1 );
		err[errSize - 1] = 0;
	}
}

/*
=================
OAXTerrain_FindLump

The BSPX directory (see qcommon/bspx.c) sits after the last standard lump,
4-byte aligned: "BSPX", count, count x { char name[24]; int ofs; int len; }.
Kept here so the renderer module, which does not link bspx.c, can use it.
=================
*/
const void *OAXTerrain_FindLump( const void *bsp, int bspLen, int *outLen ) {
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
	if ( count < 0 || at + 8 + count * 32 > bspLen ) {
		return NULL;
	}
	for ( i = 0; i < count; i++ ) {
		const unsigned char *e = b + at + 8 + i * 32;
		int ofs = RdInt( e + 24 ), len = RdInt( e + 28 );
		if ( strncmp( (const char *)e, OAX_TERRAIN_LUMP, 24 ) ) {
			continue;
		}
		if ( ofs < 0 || len < 0 || ofs + len > bspLen ) {
			return NULL;
		}
		if ( outLen ) {
			*outLen = len;
		}
		return b + ofs;
	}
	return NULL;
}

/*
=================
OAXTerrain_Parse
=================
*/
int OAXTerrain_Parse( const void *lump, int len, oaxTerrainInfo_t *out, int maxOut, char *err, int errSize ) {
	const unsigned char *b = lump;
	int n, k, at;

	if ( !b || len < (int)sizeof( oaxTerrainLumpHeader_t ) || memcmp( b, OAX_TERRAIN_IDENT, 4 ) ) {
		Err( err, errSize, "not an OAX_TERRAIN lump" );
		return 0;
	}
	if ( RdInt( b + 4 ) != OAX_TERRAIN_VERSION ) {
		Err( err, errSize, "unsupported OAX_TERRAIN version" );
		return 0;
	}
	n = RdInt( b + 8 );
	if ( n < 0 || n > maxOut ) {
		Err( err, errSize, "too many terrains" );
		return 0;
	}
	at = sizeof( oaxTerrainLumpHeader_t );
	for ( k = 0; k < n; k++ ) {
		const unsigned char *r = b + at;
		oaxTerrainInfo_t *t = &out[k];
		const oaxTerrainDisk_t *d = (const oaxTerrainDisk_t *)r;
		int size, sx, sy, f, cells;

		if ( at + (int)sizeof( oaxTerrainDisk_t ) > len ) {
			Err( err, errSize, "truncated terrain record" );
			return 0;
		}
		size = RdInt( r );
		memset( t, 0, sizeof( *t ) );
		t->origin[0] = RdFloat( r + 4 );
		t->origin[1] = RdFloat( r + 8 );
		t->origin[2] = RdFloat( r + 12 );
		t->cellSize = RdFloat( r + 16 );
		sx = t->samplesX = RdInt( r + 20 );
		sy = t->samplesY = RdInt( r + 24 );
		t->heightScale = RdFloat( r + 28 );
		t->bottom = RdFloat( r + 32 );
		t->contents = RdInt( r + 36 );
		t->surfaceFlags = RdInt( r + 40 );
		t->numLayers = RdInt( r + 44 );
		if ( sx < 2 || sy < 2 || sx > OAX_TERRAIN_MAX_SAMPLES || sy > OAX_TERRAIN_MAX_SAMPLES ||
			!( t->cellSize > 0.0f ) || t->numLayers < 0 || t->numLayers > OAX_TERRAIN_MAX_LAYERS ) {
			Err( err, errSize, "bad terrain dimensions" );
			return 0;
		}
		memcpy( t->layerShader, d->layerShader, sizeof( t->layerShader ) );
		for ( f = 0; f < OAX_TERRAIN_MAX_LAYERS; f++ ) {
			t->layerShader[f][OAX_TERRAIN_NAME - 1] = 0;
			t->layerTexScale[f] = RdFloat( (const unsigned char *)&d->layerTexScale[f] );
		}
		t->foliageSeed = RdInt( (const unsigned char *)&d->foliageSeed );
		t->flags = RdInt( (const unsigned char *)&d->flags );
		t->numFoliage = RdInt( (const unsigned char *)&d->numFoliage );
		if ( t->numFoliage < 0 || t->numFoliage > OAX_TERRAIN_MAX_FOLIAGE ) {
			Err( err, errSize, "bad foliage count" );
			return 0;
		}
		for ( f = 0; f < t->numFoliage; f++ ) {
			const unsigned char *fp = (const unsigned char *)&d->foliage[f];
			oaxFoliageDisk_t *o = &t->foliage[f];
			o->kind = RdInt( fp );
			memcpy( o->shader, fp + 4, OAX_TERRAIN_NAME );
			o->shader[OAX_TERRAIN_NAME - 1] = 0;
			fp += 4 + OAX_TERRAIN_NAME;
			o->channel = RdInt( fp ) & 3;
			o->density = RdFloat( fp + 4 );
			o->sizeMin = RdFloat( fp + 8 );
			o->sizeMax = RdFloat( fp + 12 );
			o->fadeStart = RdFloat( fp + 16 );
			o->fadeEnd = RdFloat( fp + 20 );
			o->collideRadius = RdFloat( fp + 24 );
			o->collideHeight = RdFloat( fp + 28 );
			o->maxSlope = RdFloat( fp + 32 );
			o->variants = RdFloat( fp + 36 );
		}
		{
			int ho = RdInt( (const unsigned char *)&d->heightsOfs );
			int so = RdInt( (const unsigned char *)&d->splatOfs );
			int dd = RdInt( (const unsigned char *)&d->densityOfs );
			cells = sx * sy;
			if ( size < (int)sizeof( oaxTerrainDisk_t ) || at + size > len ||
				ho < 0 || ho + cells * 2 > size || so < 0 || so + cells * 4 > size || dd < 0 || dd + cells * 4 > size ) {
				Err( err, errSize, "terrain arrays out of range" );
				return 0;
			}
			t->heights = r + ho;
			t->splat = r + so;
			t->density = r + dd;
		}
		at += ( size + 3 ) & ~3;
	}
	return n;
}

static int Clampi( int v, int lo, int hi ) {
	return v < lo ? lo : v > hi ? hi : v;
}

float OAXTerrain_SampleZ( const oaxTerrainInfo_t *t, int i, int j ) {
	const unsigned char *h;

	i = Clampi( i, 0, t->samplesX - 1 );
	j = Clampi( j, 0, t->samplesY - 1 );
	h = t->heights + ( j * t->samplesX + i ) * 2;
	return t->origin[2] + (float)( h[0] | ( h[1] << 8 ) ) * t->heightScale;
}

void OAXTerrain_CellTriangles( const oaxTerrainInfo_t *t, int i, int j, float tri[2][3][3] ) {
	float x0 = t->origin[0] + (float)i * t->cellSize;
	float y0 = t->origin[1] + (float)j * t->cellSize;
	float x1 = t->origin[0] + (float)( i + 1 ) * t->cellSize;
	float y1 = t->origin[1] + (float)( j + 1 ) * t->cellSize;
	float za = OAXTerrain_SampleZ( t, i, j );
	float zb = OAXTerrain_SampleZ( t, i + 1, j );
	float zc = OAXTerrain_SampleZ( t, i, j + 1 );
	float zd = OAXTerrain_SampleZ( t, i + 1, j + 1 );

	// a=(i,j) b=(i+1,j) d=(i+1,j+1)
	tri[0][0][0] = x0; tri[0][0][1] = y0; tri[0][0][2] = za;
	tri[0][1][0] = x1; tri[0][1][1] = y0; tri[0][1][2] = zb;
	tri[0][2][0] = x1; tri[0][2][1] = y1; tri[0][2][2] = zd;
	// a d c=(i,j+1)
	tri[1][0][0] = x0; tri[1][0][1] = y0; tri[1][0][2] = za;
	tri[1][1][0] = x1; tri[1][1][1] = y1; tri[1][1][2] = zd;
	tri[1][2][0] = x0; tri[1][2][1] = y1; tri[1][2][2] = zc;
}

static void TriNormal( float tri[3][3], float n[3] ) {
	float e1[3], e2[3], l;

	e1[0] = tri[1][0] - tri[0][0]; e1[1] = tri[1][1] - tri[0][1]; e1[2] = tri[1][2] - tri[0][2];
	e2[0] = tri[2][0] - tri[0][0]; e2[1] = tri[2][1] - tri[0][1]; e2[2] = tri[2][2] - tri[0][2];
	n[0] = e1[1] * e2[2] - e1[2] * e2[1];
	n[1] = e1[2] * e2[0] - e1[0] * e2[2];
	n[2] = e1[0] * e2[1] - e1[1] * e2[0];
	l = sqrtf( n[0] * n[0] + n[1] * n[1] + n[2] * n[2] );
	if ( l > 0.0f ) {
		n[0] /= l; n[1] /= l; n[2] /= l;
	} else {
		n[0] = 0; n[1] = 0; n[2] = 1;
	}
}

// height on cell (i, j) at fractions (u, v) in [0, 1]
static float CellZ( const oaxTerrainInfo_t *t, int i, int j, float u, float v, float normal[3] ) {
	float za = OAXTerrain_SampleZ( t, i, j );
	float zb = OAXTerrain_SampleZ( t, i + 1, j );
	float zc = OAXTerrain_SampleZ( t, i, j + 1 );
	float zd = OAXTerrain_SampleZ( t, i + 1, j + 1 );

	if ( normal ) {
		float tri[2][3][3];
		OAXTerrain_CellTriangles( t, i, j, tri );
		TriNormal( tri[u >= v ? 0 : 1], normal );
	}
	if ( u >= v ) {
		return za + u * ( zb - za ) + v * ( zd - zb );
	}
	return za + u * ( zd - zc ) + v * ( zc - za );
}

int OAXTerrain_HeightAt( const oaxTerrainInfo_t *t, float x, float y, float *z, float normal[3] ) {
	float fx = ( x - t->origin[0] ) / t->cellSize;
	float fy = ( y - t->origin[1] ) / t->cellSize;
	int i, j;

	if ( !( fx >= 0.0f ) || !( fy >= 0.0f ) || fx > (float)( t->samplesX - 1 ) || fy > (float)( t->samplesY - 1 ) ) {
		return 0;
	}
	i = (int)fx;
	j = (int)fy;
	if ( i > t->samplesX - 2 ) {
		i = t->samplesX - 2;
	}
	if ( j > t->samplesY - 2 ) {
		j = t->samplesY - 2;
	}
	*z = CellZ( t, i, j, fx - (float)i, fy - (float)j, normal );
	return 1;
}

void OAXTerrain_Bounds( const oaxTerrainInfo_t *t, float mins[3], float maxs[3] ) {
	int i, n = t->samplesX * t->samplesY, hmax = 0;

	for ( i = 0; i < n; i++ ) {
		int h = t->heights[i * 2] | ( t->heights[i * 2 + 1] << 8 );
		if ( h > hmax ) {
			hmax = h;
		}
	}
	mins[0] = t->origin[0];
	mins[1] = t->origin[1];
	mins[2] = t->bottom;
	maxs[0] = t->origin[0] + (float)( t->samplesX - 1 ) * t->cellSize;
	maxs[1] = t->origin[1] + (float)( t->samplesY - 1 ) * t->cellSize;
	maxs[2] = t->origin[2] + (float)hmax * t->heightScale;
}

// ---- deterministic foliage ----------------------------------------------------

static unsigned Hash32( unsigned x ) {
	// lowbias32 (Chris Wellons, public domain)
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

static float Rand01( unsigned *state ) {
	*state = Hash32( *state + 0x9e3779b9U );
	return (float)( *state >> 8 ) * ( 1.0f / 16777216.0f );
}

int OAXTerrain_CellFoliage( const oaxTerrainInfo_t *t, int f, int i, int j, oaxFoliageInstance_t *out ) {
	const oaxFoliageDisk_t *fd;
	unsigned state;
	int c, sum, n, k, count = 0;
	float expected;

	if ( f < 0 || f >= t->numFoliage || i < 0 || j < 0 || i >= t->samplesX - 1 || j >= t->samplesY - 1 ) {
		return 0;
	}
	fd = &t->foliage[f];
	c = fd->channel;
	sum = t->density[( j * t->samplesX + i ) * 4 + c] + t->density[( j * t->samplesX + i + 1 ) * 4 + c] +
		t->density[( ( j + 1 ) * t->samplesX + i ) * 4 + c] + t->density[( ( j + 1 ) * t->samplesX + i + 1 ) * 4 + c];
	if ( !sum || !( fd->density > 0.0f ) ) {
		return 0;
	}
	state = Hash32( (unsigned)t->foliageSeed ^ Hash32( (unsigned)f * 73856093U ^ (unsigned)i * 19349663U ^ (unsigned)j * 83492791U ) );
	expected = (float)sum * ( 1.0f / 1020.0f ) * fd->density;
	n = (int)expected;
	if ( Rand01( &state ) < expected - (float)n ) {
		n++;
	}
	if ( n > OAX_FOLIAGE_MAX_PER_CELL ) {
		n = OAX_FOLIAGE_MAX_PER_CELL;
	}
	for ( k = 0; k < n; k++ ) {
		oaxFoliageInstance_t *o = &out[count];
		float u = Rand01( &state ), v = Rand01( &state );
		float yaw = Rand01( &state ), sc = Rand01( &state );

		o->origin[0] = t->origin[0] + ( (float)i + u ) * t->cellSize;
		o->origin[1] = t->origin[1] + ( (float)j + v ) * t->cellSize;
		o->origin[2] = CellZ( t, i, j, u, v, o->normal );
		o->yaw = yaw * 360.0f;
		o->scale = fd->sizeMin + sc * ( fd->sizeMax - fd->sizeMin );
		if ( o->normal[2] < fd->maxSlope ) {
			continue;
		}
		count++;
	}
	return count;
}
