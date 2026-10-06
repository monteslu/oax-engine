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
oax_common.c: engine-extension plumbing shared by every build.

- `oax_features` / `oax_version`: read-only cvars new gamecode probes
  before it uses an extension (see oax.h).
- Named debug values: engine and QVM code publish "name = value" pairs that
  tests read: on a wasmcart through the "debug_values" debug field, on any
  build with `debugvalues [file]`.
- `detmathhash`: hashes the deterministic math over a fixed input sweep, so
  a test can prove the native and wasm builds compute the same bits.
===========================================================================
*/

#include "q_shared.h"
#include "qcommon.h"
#include "oax.h"

static char oaxFeatures[MAX_CVAR_VALUE_STRING];

/*
=================
OAX_AddFeature
=================
*/
void OAX_AddFeature( const char *token ) {
	if ( strstr( va( " %s ", oaxFeatures ), va( " %s ", token ) ) ) {
		return;
	}
	if ( oaxFeatures[0] ) {
		Q_strcat( oaxFeatures, sizeof( oaxFeatures ), " " );
	}
	Q_strcat( oaxFeatures, sizeof( oaxFeatures ), token );
	Cvar_Set( "oax_features", oaxFeatures );
}

// ---- named debug values ------------------------------------------------------

// a big map's debug values outgrew 256 (one per navmesh link, per bot, per
// objective): the bot count was dropped and a bot match read "0 navmesh bots"
#define MAX_DEBUG_VALUES 1024

typedef struct {
	char name[48];
	char value[1024];
} debugValue_t;

static debugValue_t debugValues[MAX_DEBUG_VALUES];
static int          numDebugValues;

void Com_DebugSet( const char *name, const char *value ) {
	int i;

	for ( i = 0; i < numDebugValues; i++ ) {
		if ( !strcmp( debugValues[i].name, name ) ) {
			break;
		}
	}
	if ( i == numDebugValues ) {
		if ( numDebugValues == MAX_DEBUG_VALUES ) {
			static qboolean warned;

			if ( !warned ) {
				warned = qtrue;
				Com_Printf( S_COLOR_YELLOW "WARNING: more than %i debug values: \"%s\" and later new names are dropped\n", MAX_DEBUG_VALUES, name );
			}
			return;
		}
		numDebugValues++;
		Q_strncpyz( debugValues[i].name, name, sizeof( debugValues[i].name ) );
	}
	Q_strncpyz( debugValues[i].value, value, sizeof( debugValues[i].value ) );
}

char com_debugBlob[COM_DEBUG_BLOB_SIZE];

void Com_DebugSetBlob( const char *data, int len ) {
	if ( len > COM_DEBUG_BLOB_SIZE - 1 ) {
		len = COM_DEBUG_BLOB_SIZE - 1;
	}
	if ( len < 0 ) {
		len = 0;
	}
	memcpy( com_debugBlob, data, len );
	com_debugBlob[len] = 0;
}

void Com_DebugSetInt( const char *name, int value ) {
	Com_DebugSet( name, va( "%d", value ) );
}

void Com_DebugSetFloat( const char *name, float value ) {
	Com_DebugSet( name, va( "%.9g", value ) );
}

/*
=================
Com_DebugValuesText

"name value\n" lines, in the order the names were first set. Returns the
length written.
=================
*/
int Com_DebugValuesText( char *buf, int size ) {
	int i, len = 0;

	if ( size <= 0 ) {
		return 0;
	}
	buf[0] = '\0';
	for ( i = 0; i < numDebugValues; i++ ) {
		int n = Com_sprintf( buf + len, size - len, "%s %s\n", debugValues[i].name, debugValues[i].value );
		if ( len + n >= size - 1 ) {
			break;
		}
		len += n;
	}
	return len;
}

static void Com_DebugValues_f( void ) {
	static char text[MAX_DEBUG_VALUES * 1080];
	int len = Com_DebugValuesText( text, sizeof( text ) );

	if ( Cmd_Argc() > 1 ) {
		fileHandle_t f = FS_FOpenFileWrite_HomeData( Cmd_Argv( 1 ) );
		if ( !f ) {
			Com_Printf( "debugvalues: couldn't write %s\n", Cmd_Argv( 1 ) );
			return;
		}
		FS_Write( text, len, f );
		FS_FCloseFile( f );
		return;
	}
	Com_Printf( "%s", text );
}

// ---- deterministic math check --------------------------------------------------

// detmathhash [host|perturb]: "host" hashes the host libm instead (to show
// whether it differs from ours), "perturb" shifts the inputs (a control
// that must change the hash).
static void Com_DetMathHash_f( void ) {
	unsigned h = 2166136261u;
	int i;
	qboolean host = !Q_stricmp( Cmd_Argv( 1 ), "host" );
	float shift = !Q_stricmp( Cmd_Argv( 1 ), "perturb" ) ? 1.0e-6f : 0.0f;
	const char *name = host ? "detmath_hash_host" : shift != 0.0f ? "detmath_hash_perturbed" : "detmath_hash";

	// a sweep over the ranges gamecode uses (angles in radians, a few
	// turns either way) plus large arguments that need full reduction
	for ( i = 0; i < 200000; i++ ) {
		float x = ( i - 100000 ) * 0.000731f + ( i & 7 ) * 1.0e-7f + shift;
		float big = ( i - 100000 ) * 37.5f;
		float r[6];
		int k;

		if ( host ) {
			r[0] = (float)sin( x );
			r[1] = (float)cos( x );
			r[2] = (float)atan2( x, 0.37f - x * 0.5f );
			r[3] = (float)acos( ( i % 2001 - 1000 ) / 1000.0f );
			r[4] = (float)sin( big );
			r[5] = (float)cos( big );
		} else {
			r[0] = (float)Q_detSin( x );
			r[1] = (float)Q_detCos( x );
			r[2] = (float)Q_detAtan2( x, 0.37f - x * 0.5f );
			r[3] = Q_detAcosf( ( i % 2001 - 1000 ) / 1000.0f + shift );
			r[4] = (float)Q_detSin( big + shift * 1000.0f );
			r[5] = (float)Q_detCos( big );
		}
		for ( k = 0; k < 6; k++ ) {
			h = ( h ^ *(unsigned *)&r[k] ) * 16777619u;
		}
	}
	Com_DebugSet( name, va( "%08x", h ) );
	Com_Printf( "%s %08x\n", name, h );
}

/*
=================
OAX_Init
=================
*/
void OAX_Init( void ) {
	oaxFeatures[0] = '\0';
	Cvar_Get( "oax_features", "", CVAR_ROM );
	Cvar_Set( "oax_features", "" );
	Cvar_Get( "oax_version", va( "%d", OAX_VERSION ), CVAR_ROM );
	OAX_AddFeature( "debug" );
	OAX_AddFeature( "bspx" );
	OAX_AddFeature( "detmath" );
	Cmd_AddCommand( "debugvalues", Com_DebugValues_f );
	Cmd_AddCommand( "detmathhash", Com_DetMathHash_f );
}
