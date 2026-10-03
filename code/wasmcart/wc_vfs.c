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
wasmcart platform backend: the cart's virtual filesystem.

Two namespaces, both reached through the engine's ordinary stdio calls
(Sys_FOpen returns a real FILE* built with fopencookie):

  read-only   the cart's bundled assets, named by files.idx (one path per
              line, written by misc/wasmcart/pack-cart.mjs; not every host
              synthesizes _filelist.txt for an unpacked cart). A file is fetched with wc_load_asset only
              when it is opened, and freed when it is closed. The engine sees
              them under fs_basepath ".", e.g. "./baseoa/maps/oa_dm1.bsp".

  writable    everything under fs_homepath ("home/..."): configs, demos,
              screenshots, downloaded pk3s. Held in memory and serialized
              into the save region the host persists (SRAM).

Lookups are case-insensitive, matching how the engine treats pk3 contents.
===========================================================================
*/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <ctype.h>

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "wc_local.h"

#define VFS_INDEX_ASSET   "files.idx"
#define VFS_HOME_PREFIX   "home/"
#define VFS_SAVE_MAGIC    0x46564f57u   // "WOVF"
#define VFS_SAVE_VERSION  1
#define VFS_MAX_LIST      0x2000

typedef struct vfsEntry_s {
	char               *path;       // normalized, original case
	unsigned int        hash;
	int                 size;
	qboolean            writable;
	byte               *data;       // writable entries only
	struct vfsEntry_s  *hashNext;
	struct vfsEntry_s  *next;       // all entries, insertion order
} vfsEntry_t;

#define VFS_HASH_SIZE 8192

static vfsEntry_t *vfsHash[VFS_HASH_SIZE];
static vfsEntry_t *vfsFirst, *vfsLast;
static int         vfsCount;

static uint8_t     wc_save_region[WC_SAVE_SIZE] __attribute__((aligned(8)));
static qboolean    vfsDirty;

uint8_t *WC_VFS_SaveRegion( void ) {
	return wc_save_region;
}

/*
=================
Paths
=================
*/
static const char *VFS_Normalize( const char *ospath, char *out, int size ) {
	const char *p = ospath;
	int         i = 0;

	while ( p[0] == '.' && p[1] == '/' ) {
		p += 2;
	}
	while ( *p == '/' ) {
		p++;
	}
	for ( ; *p && i < size - 1; p++ ) {
		char c = *p == '\\' ? '/' : *p;
		if ( c == '/' && i > 0 && out[i - 1] == '/' ) {
			continue;
		}
		out[i++] = c;
	}
	out[i] = '\0';
	return out;
}

static unsigned int VFS_Hash( const char *path ) {
	unsigned int h = 5381;
	for ( ; *path; path++ ) {
		h = h * 33 + (unsigned char)tolower( *path );
	}
	return h;
}

static vfsEntry_t *VFS_Find( const char *path ) {
	unsigned int h = VFS_Hash( path );
	vfsEntry_t  *e;

	for ( e = vfsHash[h & ( VFS_HASH_SIZE - 1 )]; e; e = e->hashNext ) {
		if ( e->hash == h && !Q_stricmp( e->path, path ) ) {
			return e;
		}
	}
	return NULL;
}

static vfsEntry_t *VFS_Add( const char *path, int size, qboolean writable ) {
	vfsEntry_t *e = calloc( 1, sizeof( *e ) );
	int         len = (int)strlen( path );

	e->path = malloc( len + 1 );
	memcpy( e->path, path, len + 1 );
	e->hash = VFS_Hash( path );
	e->size = size;
	e->writable = writable;
	e->hashNext = vfsHash[e->hash & ( VFS_HASH_SIZE - 1 )];
	vfsHash[e->hash & ( VFS_HASH_SIZE - 1 )] = e;
	if ( vfsLast ) {
		vfsLast->next = e;
	} else {
		vfsFirst = e;
	}
	vfsLast = e;
	vfsCount++;
	return e;
}

/*
=================
Save region (SRAM)
=================
*/
static void VFS_LoadSave( void ) {
	const uint8_t *p = wc_save_region;
	const uint8_t *end = wc_save_region + WC_SAVE_SIZE;
	uint32_t       magic, version, count, i;

	memcpy( &magic, p, 4 );
	memcpy( &version, p + 4, 4 );
	memcpy( &count, p + 8, 4 );
	if ( magic != VFS_SAVE_MAGIC || version != VFS_SAVE_VERSION ) {
		return;
	}
	p += 12;

	for ( i = 0; i < count; i++ ) {
		uint16_t    plen;
		uint32_t    size;
		char        path[MAX_OSPATH];
		vfsEntry_t *e;

		if ( p + 2 > end ) break;
		memcpy( &plen, p, 2 );
		p += 2;
		if ( plen == 0 || plen >= sizeof( path ) || p + plen + 4 > end ) break;
		memcpy( path, p, plen );
		path[plen] = '\0';
		p += plen;
		memcpy( &size, p, 4 );
		p += 4;
		if ( p + size > end ) break;

		e = VFS_Find( path );
		if ( !e ) {
			e = VFS_Add( path, 0, qtrue );
		}
		e->writable = qtrue;
		e->data = malloc( size ? size : 1 );
		memcpy( e->data, p, size );
		e->size = (int)size;
		p += size;
	}
}

void WC_VFS_Flush( void ) {
	uint8_t    *p = wc_save_region + 12;
	uint8_t    *end = wc_save_region + WC_SAVE_SIZE;
	uint32_t    count = 0, magic = VFS_SAVE_MAGIC, version = VFS_SAVE_VERSION;
	vfsEntry_t *e;

	if ( !vfsDirty ) {
		return;
	}
	vfsDirty = qfalse;

	for ( e = vfsFirst; e; e = e->next ) {
		uint16_t plen;
		uint32_t size;

		if ( !e->writable ) {
			continue;
		}
		plen = (uint16_t)strlen( e->path );
		size = (uint32_t)e->size;
		if ( p + 2 + plen + 4 + size > end ) {
			Com_Printf( S_COLOR_YELLOW "WARNING: save region full, %s not persisted\n", e->path );
			continue;
		}
		memcpy( p, &plen, 2 );
		p += 2;
		memcpy( p, e->path, plen );
		p += plen;
		memcpy( p, &size, 4 );
		p += 4;
		memcpy( p, e->data, size );
		p += size;
		count++;
	}
	memset( p, 0, end - p );
	memcpy( wc_save_region, &magic, 4 );
	memcpy( wc_save_region + 4, &version, 4 );
	memcpy( wc_save_region + 8, &count, 4 );
}

/*
=================
WC_VFS_Init
=================
*/
void WC_VFS_Init( void ) {
	int   size = wc_asset_size( VFS_INDEX_ASSET, sizeof( VFS_INDEX_ASSET ) - 1 );
	char *text, *line, *next;
	int   assets = 0;

	if ( size > 0 ) {
		text = malloc( size + 1 );
		wc_load_asset( VFS_INDEX_ASSET, sizeof( VFS_INDEX_ASSET ) - 1, text, size );
		text[size] = '\0';

		for ( line = text; line && *line; line = next ) {
			char *cr;

			next = strchr( line, '\n' );
			if ( next ) {
				*next++ = '\0';
			}
			if ( ( cr = strchr( line, '\r' ) ) != NULL ) {
				*cr = '\0';
			}
			if ( line[0] && !VFS_Find( line ) ) {
				VFS_Add( line, -1, qfalse );	// size is read when the file is opened
				assets++;
			}
		}
		free( text );
	}

	// A deterministic replay must not depend on whatever an earlier run saved
	// (configs change binds, cvars, picmip...), so it starts from a clean home.
	if ( !wc_deterministic ) {
		VFS_LoadSave();
	}

	Com_Printf( "wasmcart vfs: %d bundled files, %d saved files\n", assets, vfsCount - assets );
}

/*
=================
FILE* backends
=================
*/
typedef struct {
	vfsEntry_t *entry;      // writable target, or NULL for a read-only asset
	byte       *data;
	int         size;
	int         cap;
	int         pos;
	qboolean    write;
} vfsCookie_t;

static ssize_t VFS_Read( void *c, char *buf, size_t n ) {
	vfsCookie_t *ck = c;
	int          left = ck->size - ck->pos;

	if ( left <= 0 ) {
		return 0;
	}
	if ( (int)n > left ) {
		n = left;
	}
	memcpy( buf, ck->data + ck->pos, n );
	ck->pos += (int)n;
	return (ssize_t)n;
}

static ssize_t VFS_Write( void *c, const char *buf, size_t n ) {
	vfsCookie_t *ck = c;

	if ( !ck->write ) {
		return -1;
	}
	if ( ck->pos + (int)n > ck->cap ) {
		int cap = ck->cap ? ck->cap : 4096;
		while ( cap < ck->pos + (int)n ) {
			cap *= 2;
		}
		ck->data = realloc( ck->data, cap );
		ck->cap = cap;
	}
	memcpy( ck->data + ck->pos, buf, n );
	ck->pos += (int)n;
	if ( ck->pos > ck->size ) {
		ck->size = ck->pos;
	}
	return (ssize_t)n;
}

static int VFS_Seek( void *c, off_t *offset, int whence ) {
	vfsCookie_t *ck = c;
	off_t        base;

	switch ( whence ) {
	case SEEK_SET: base = 0; break;
	case SEEK_CUR: base = ck->pos; break;
	case SEEK_END: base = ck->size; break;
	default: return -1;
	}
	base += *offset;
	if ( base < 0 ) {
		return -1;
	}
	ck->pos = (int)base;
	*offset = base;
	return 0;
}

static int VFS_Close( void *c ) {
	vfsCookie_t *ck = c;

	if ( ck->write && ck->entry ) {
		free( ck->entry->data );
		ck->entry->data = ck->data ? ck->data : malloc( 1 );
		ck->entry->size = ck->size;
		ck->entry->writable = qtrue;
		vfsDirty = qtrue;
		WC_VFS_Flush();
	} else {
		free( ck->data );
	}
	free( ck );
	return 0;
}

static const cookie_io_functions_t vfsFuncs = { VFS_Read, VFS_Write, VFS_Seek, VFS_Close };

FILE *Sys_FOpen( const char *ospath, const char *mode ) {
	char         path[MAX_OSPATH];
	vfsEntry_t  *e;
	vfsCookie_t *ck;

	VFS_Normalize( ospath, path, sizeof( path ) );
	if ( !path[0] ) {
		return NULL;
	}
	e = VFS_Find( path );

	ck = calloc( 1, sizeof( *ck ) );

	if ( mode[0] == 'r' ) {
		if ( !e ) {
			free( ck );
			return NULL;
		}
		if ( e->writable ) {
			ck->data = malloc( e->size ? e->size : 1 );
			memcpy( ck->data, e->data, e->size );
		} else {
			if ( e->size < 0 ) {
				e->size = wc_asset_size( e->path, (unsigned int)strlen( e->path ) );
			}
			if ( e->size < 0 ) {
				free( ck );
				return NULL;
			}
			ck->data = malloc( e->size ? e->size : 1 );
			if ( wc_load_asset( e->path, (unsigned int)strlen( e->path ), ck->data, e->size ) != e->size ) {
				free( ck->data );
				free( ck );
				return NULL;
			}
		}
		ck->size = ck->cap = e->size;
		return fopencookie( ck, "rb", vfsFuncs );
	}

	// Writes are confined to the home namespace; bundled assets are read-only.
	if ( Q_stricmpn( path, VFS_HOME_PREFIX, sizeof( VFS_HOME_PREFIX ) - 1 ) ) {
		free( ck );
		return NULL;
	}
	if ( !e ) {
		e = VFS_Add( path, 0, qtrue );
		e->data = malloc( 1 );
	}
	ck->entry = e;
	ck->write = qtrue;
	if ( mode[0] == 'a' && e->size ) {
		ck->cap = ck->size = ck->pos = e->size;
		ck->data = malloc( e->size );
		memcpy( ck->data, e->data, e->size );
	}
	return fopencookie( ck, mode[0] == 'a' ? "ab" : "wb", vfsFuncs );
}

/*
=================
remove / rename

files.c deletes and renames through libc (demo and config rotation); in a
cart those are operations on the writable table.
=================
*/
static void VFS_Unlink( vfsEntry_t *dead ) {
	vfsEntry_t **link, *prev = NULL, *e;

	for ( link = &vfsHash[dead->hash & ( VFS_HASH_SIZE - 1 )]; *link; link = &( *link )->hashNext ) {
		if ( *link == dead ) {
			*link = dead->hashNext;
			break;
		}
	}
	for ( e = vfsFirst; e; prev = e, e = e->next ) {
		if ( e == dead ) {
			if ( prev ) {
				prev->next = e->next;
			} else {
				vfsFirst = e->next;
			}
			if ( vfsLast == e ) {
				vfsLast = prev;
			}
			break;
		}
	}
	vfsCount--;
	free( dead->data );
	free( dead->path );
	free( dead );
}

int remove( const char *ospath ) {
	char        path[MAX_OSPATH];
	vfsEntry_t *e = VFS_Find( VFS_Normalize( ospath, path, sizeof( path ) ) );

	if ( !e || !e->writable ) {
		return -1;
	}
	VFS_Unlink( e );
	vfsDirty = qtrue;
	WC_VFS_Flush();
	return 0;
}

int rename( const char *from, const char *to ) {
	char        fromPath[MAX_OSPATH], toPath[MAX_OSPATH];
	vfsEntry_t *src, *dst;

	src = VFS_Find( VFS_Normalize( from, fromPath, sizeof( fromPath ) ) );
	VFS_Normalize( to, toPath, sizeof( toPath ) );
	if ( !src || !src->writable || Q_stricmpn( toPath, VFS_HOME_PREFIX, sizeof( VFS_HOME_PREFIX ) - 1 ) ) {
		return -1;
	}
	dst = VFS_Find( toPath );
	if ( dst == src ) {
		return 0;
	}
	if ( !dst ) {
		dst = VFS_Add( toPath, 0, qtrue );
	}
	free( dst->data );
	dst->data = src->data;
	dst->size = src->size;
	dst->writable = qtrue;
	src->data = NULL;
	VFS_Unlink( src );
	vfsDirty = qtrue;
	WC_VFS_Flush();
	return 0;
}

/*
=================
Listing

Same results as sys_unix.c's readdir walk, served from the entry table.
=================
*/
static qboolean VFS_InDir( const char *path, const char *dir, int dirLen, const char **rest ) {
	if ( dirLen == 0 ) {
		*rest = path;
		return qtrue;
	}
	if ( Q_stricmpn( path, dir, dirLen ) || path[dirLen] != '/' ) {
		return qfalse;
	}
	*rest = path + dirLen + 1;
	return qtrue;
}

static qboolean VFS_Listed( char **list, int n, const char *name ) {
	int i;
	for ( i = 0; i < n; i++ ) {
		if ( !Q_stricmp( list[i], name ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

static char **VFS_CopyList( char **list, int n, int *numfiles ) {
	char **copy;
	int    i;

	*numfiles = n;
	if ( !n ) {
		return NULL;
	}
	copy = Z_Malloc( ( n + 1 ) * sizeof( *copy ) );
	for ( i = 0; i < n; i++ ) {
		copy[i] = list[i];
	}
	copy[n] = NULL;
	return copy;
}

char **Sys_ListFiles( const char *directory, const char *extension, char *filter, int *numfiles, qboolean wantsubs ) {
	static char *list[VFS_MAX_LIST];
	char         dir[MAX_OSPATH];
	int          dirLen, extLen, n = 0;
	qboolean     dironly = wantsubs;
	vfsEntry_t  *e;

	VFS_Normalize( directory, dir, sizeof( dir ) );
	dirLen = (int)strlen( dir );

	if ( filter ) {
		// recursive, paths relative to the directory
		for ( e = vfsFirst; e && n < VFS_MAX_LIST - 1; e = e->next ) {
			const char *rest;
			char        name[MAX_OSPATH];

			if ( !VFS_InDir( e->path, dir, dirLen, &rest ) ) {
				continue;
			}
			if ( strchr( rest, '/' ) ) {
				Q_strncpyz( name, rest, sizeof( name ) );
			} else {
				Com_sprintf( name, sizeof( name ), "/%s", rest );
			}
			if ( !Com_FilterPath( filter, name, qfalse ) ) {
				continue;
			}
			list[n++] = CopyString( name );
		}
		return VFS_CopyList( list, n, numfiles );
	}

	if ( !extension ) {
		extension = "";
	}
	if ( extension[0] == '/' && extension[1] == '\0' ) {
		extension = "";
		dironly = qtrue;
	}
	extLen = (int)strlen( extension );

	for ( e = vfsFirst; e && n < VFS_MAX_LIST - 1; e = e->next ) {
		const char *rest, *slash;
		char        name[MAX_OSPATH];
		int         len;

		if ( !VFS_InDir( e->path, dir, dirLen, &rest ) ) {
			continue;
		}
		slash = strchr( rest, '/' );
		if ( dironly ) {
			if ( !slash ) {
				continue;
			}
			len = (int)( slash - rest );
			if ( len >= (int)sizeof( name ) ) {
				continue;
			}
			memcpy( name, rest, len );
			name[len] = '\0';
		} else {
			if ( slash ) {
				continue;
			}
			Q_strncpyz( name, rest, sizeof( name ) );
			len = (int)strlen( name );
		}
		if ( extLen && ( len < extLen || Q_stricmp( name + len - extLen, extension ) ) ) {
			continue;
		}
		if ( VFS_Listed( list, n, name ) ) {
			continue;
		}
		list[n++] = CopyString( name );
	}
	return VFS_CopyList( list, n, numfiles );
}

void Sys_FreeFileList( char **list ) {
	int i;

	if ( !list ) {
		return;
	}
	for ( i = 0; list[i]; i++ ) {
		Z_Free( list[i] );
	}
	Z_Free( list );
}
