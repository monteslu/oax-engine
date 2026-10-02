/*
===========================================================================
idlib_bridge.c: the engine services idlib_lite (C++) uses, as plain C
calls, so no ioquake3 header has to compile as C++.

New code for the oax engine (GPLv3, like the id Tech 4 code it serves).
===========================================================================
*/

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "../qcommon/oax.h"
#include <time.h>

void idlib_Print( const char *text ) {
	Com_Printf( "%s", text );
}

void idlib_DPrint( const char *text ) {
	Com_DPrintf( "%s", text );
}

int idlib_ReadFile( const char *path, void **buffer ) {
	long len = FS_ReadFile( path, buffer );
	if ( len < 0 || !*buffer ) {
		*buffer = NULL;
		return -1;
	}
	return (int)len;
}

void idlib_FreeFile( void *buffer ) {
	if ( buffer ) {
		FS_FreeFile( buffer );
	}
}

double idlib_Sin( double x ) {
	return Q_detSin( x );
}

double idlib_Cos( double x ) {
	return Q_detCos( x );
}

double idlib_Atan2( double y, double x ) {
	return Q_detAtan2( y, x );
}

int idlib_Milliseconds( void ) {
	return Sys_Milliseconds();
}

void idlib_FatalError( const char *text ) {
	Com_Error( ERR_DROP, "%s", text );
}

void idlib_DebugSet( const char *name, const char *value ) {
	Com_DebugSet( name, value );
}

// a monotonic microsecond clock for statistics only (never gameplay). A
// wasmcart has no clock but its frame time, so there it does not advance
// within a frame.
long long idlib_Microseconds( void ) {
	struct timespec ts;

	clock_gettime( CLOCK_MONOTONIC, &ts );
	return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
