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
bspx.c: BSPX extension lumps in Q3 BSP (v46) files.

The BSPX convention (FTE, QuakeSpasm): after the last standard lump,
aligned to 4 bytes, the file holds the magic "BSPX", an int count, then
count entries of { char name[24]; int ofs; int len; }. Offsets are from
the start of the file. Loaders that read lumps by offset (ours, q3map2,
bspc) never see the block.

A map without extension lumps still loads exactly as a stock map. Most
lumps are caches or manifests the engine can recompute or default; some
carry content (OAX_TERRAIN, OAX_SURFACES, OAX_COLLISION: see
docs/map-format.md), and a map without them simply has none of it.
Writer: misc/tools/bspx.mjs.
===========================================================================
*/

#include "q_shared.h"
#include "qcommon.h"
#include "qfiles.h"
#include "oax.h"

typedef struct {
	char name[24];
	int  ofs;
	int  len;
} bspxLump_t;

// the current map's extension lumps, copied out of the file at load
static byte *bspxData;
static int   bspxDataLen;
static int   bspxDataBase;   // file offset bspxData starts at

static int BSPX_Offset( const void *bsp, int bspLen ) {
	const dheader_t *h = bsp;
	int i, end = sizeof( dheader_t );

	if ( bspLen < (int)sizeof( dheader_t ) ) {
		return -1;
	}
	for ( i = 0; i < HEADER_LUMPS; i++ ) {
		int e = LittleLong( h->lumps[i].fileofs ) + LittleLong( h->lumps[i].filelen );
		if ( e > end ) {
			end = e;
		}
	}
	end = ( end + 3 ) & ~3;
	if ( end + 8 > bspLen || memcmp( (const byte *)bsp + end, "BSPX", 4 ) ) {
		return -1;
	}
	return end;
}

/*
=================
BSPX_Find

Finds an extension lump in a whole BSP file in memory.
=================
*/
const void *BSPX_Find( const void *bsp, int bspLen, const char *name, int *outLen ) {
	int at = BSPX_Offset( bsp, bspLen ), count, i;
	const bspxLump_t *l;

	if ( at < 0 ) {
		return NULL;
	}
	count = LittleLong( *(const int *)( (const byte *)bsp + at + 4 ) );
	if ( count < 0 || at + 8 + count * (int)sizeof( bspxLump_t ) > bspLen ) {
		return NULL;
	}
	l = (const bspxLump_t *)( (const byte *)bsp + at + 8 );
	for ( i = 0; i < count; i++ ) {
		int ofs = LittleLong( l[i].ofs ), len = LittleLong( l[i].len );
		if ( Q_strncmp( l[i].name, name, sizeof( l[i].name ) ) ) {
			continue;
		}
		if ( ofs < 0 || len < 0 || ofs + len > bspLen ) {
			return NULL;
		}
		if ( outLen ) {
			*outLen = len;
		}
		return (const byte *)bsp + ofs;
	}
	return NULL;
}

/*
=================
BSPX_SetCurrentMap

Called by CM_LoadMap with the whole file before it is freed: keeps the
extension block (header, directory and lump data) on the hunk.
=================
*/
void BSPX_SetCurrentMap( const void *bsp, int bspLen ) {
	int at = BSPX_Offset( bsp, bspLen );

	bspxData = NULL;
	bspxDataLen = 0;
	bspxDataBase = 0;
	if ( at < 0 ) {
		return;
	}
	bspxDataLen = bspLen - at;
	bspxDataBase = at;
	bspxData = Hunk_Alloc( bspxDataLen, h_high );
	Com_Memcpy( bspxData, (const byte *)bsp + at, bspxDataLen );
}

/*
=================
BSPX_ReadCurrentMap

Copies a lump of the current map into buf. Returns the lump's full
length (which may exceed size), or -1 if the map has no such lump.
=================
*/
int BSPX_ReadCurrentMap( const char *name, void *buf, int size ) {
	int count, i;
	const bspxLump_t *l;

	if ( !bspxData || bspxDataLen < 8 ) {
		return -1;
	}
	count = LittleLong( *(const int *)( bspxData + 4 ) );
	if ( count < 0 || 8 + count * (int)sizeof( bspxLump_t ) > bspxDataLen ) {
		return -1;
	}
	l = (const bspxLump_t *)( bspxData + 8 );
	for ( i = 0; i < count; i++ ) {
		int ofs = LittleLong( l[i].ofs ) - bspxDataBase, len = LittleLong( l[i].len );
		if ( Q_strncmp( l[i].name, name, sizeof( l[i].name ) ) ) {
			continue;
		}
		if ( ofs < 0 || len < 0 || ofs + len > bspxDataLen ) {
			return -1;
		}
		if ( buf && size > 0 ) {
			Com_Memcpy( buf, bspxData + ofs, len < size ? len : size );
		}
		return len;
	}
	return -1;
}
