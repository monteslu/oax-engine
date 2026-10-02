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
Adapted from DOOM-3 neo/idlib/Lib.cpp, neo/idlib/Dict.cpp,
neo/idlib/math/Math.cpp, neo/idlib/math/Vector.cpp and
neo/idlib/math/Rotation.cpp for the oa-engine GUI module.
Changes: the library glue for the lite classes in Lib.h, Math.h and
Dict.h: messages and file reads go through the engine import table,
errors longjmp to the innermost idLibGuard, memory is the C heap, and the
dictionary is a list of idStr pairs. Lives in namespace idgui.
*/

#include "../idgui_precompiled.h"

namespace idgui {

idguiImport_t			gImport;

static idCommonLite		commonLocal;
static idFileSystemLite	fileSystemLocal;
idCommonLite *			idLib::common = &commonLocal;
idFileSystemLite *		idLib::fileSystem = &fileSystemLocal;
idCommonLite *			common = &commonLocal;
idFileSystemLite *		fileSystem = &fileSystemLocal;

idVec4 colorBlack	= idVec4( 0.00f, 0.00f, 0.00f, 1.00f );
idVec4 colorWhite	= idVec4( 1.00f, 1.00f, 1.00f, 1.00f );

idVec3 vec3_origin( 0.0f, 0.0f, 0.0f );
idMat3 mat3_identity( idVec3( 1, 0, 0 ), idVec3( 0, 1, 0 ), idVec3( 0, 0, 1 ) );

const float	idMath::PI				= 3.14159265358979323846f;
const float	idMath::TWO_PI			= 2.0f * PI;
const float	idMath::HALF_PI			= 0.5f * PI;
const float	idMath::SQRT_1OVER2		= 0.70710678118654752440f;
const float	idMath::M_DEG2RAD		= PI / 180.0f;
const float	idMath::M_RAD2DEG		= 180.0f / PI;
const float	idMath::M_SEC2MS		= 1000.0f;
const float	idMath::M_MS2SEC		= 0.001f;
const float	idMath::INFINITY		= 1e30f;
const float	idMath::FLT_EPSILON		= 1.192092896e-07f;

/*
===============================================================================
	errors and messages
===============================================================================
*/

static idLibGuard *guardTop;

void idLib_PushGuard( idLibGuard *g ) {
	g->prev = guardTop;
	guardTop = g;
}

void idLib_PopGuard( idLibGuard *g ) {
	if ( guardTop == g ) {
		guardTop = g->prev;
	}
}

static void Message( void ( *out )( const char * ), const char *prefix, const char *fmt, va_list ap ) {
	char text[MAX_STRING_CHARS];
	int len = 0;

	if ( prefix ) {
		len = idStr::snPrintf( text, sizeof( text ), "%s", prefix );
	}
	idStr::vsnPrintf( text + len, sizeof( text ) - len, fmt, ap );
	if ( out ) {
		out( text );
	}
}

void idCommonLite::Printf( const char *fmt, ... ) {
	va_list ap;
	va_start( ap, fmt );
	Message( gImport.Print, NULL, fmt, ap );
	va_end( ap );
}

void idCommonLite::DPrintf( const char *fmt, ... ) {
}

void idCommonLite::Warning( const char *fmt, ... ) {
	va_list ap;
	va_start( ap, fmt );
	Message( gImport.Warning, "idgui: ", fmt, ap );
	va_end( ap );
}

void idCommonLite::DWarning( const char *fmt, ... ) {
}

void idCommonLite::Error( const char *fmt, ... ) {
	va_list ap;
	va_start( ap, fmt );
	Message( gImport.Warning, "idgui error: ", fmt, ap );
	va_end( ap );
	if ( guardTop ) {
		longjmp( guardTop->jb, 1 );
	}
	abort();
}

void idCommonLite::FatalError( const char *fmt, ... ) {
	va_list ap;
	va_start( ap, fmt );
	Message( gImport.Warning, "idgui fatal: ", fmt, ap );
	va_end( ap );
	if ( guardTop ) {
		longjmp( guardTop->jb, 1 );
	}
	abort();
}

void idLib::Error( const char *fmt, ... ) {
	char text[MAX_STRING_CHARS];
	va_list ap;
	va_start( ap, fmt );
	idStr::vsnPrintf( text, sizeof( text ), fmt, ap );
	va_end( ap );
	common->Error( "%s", text );
}

void idLib::Warning( const char *fmt, ... ) {
	char text[MAX_STRING_CHARS];
	va_list ap;
	va_start( ap, fmt );
	idStr::vsnPrintf( text, sizeof( text ), fmt, ap );
	va_end( ap );
	common->Warning( "%s", text );
}

int idFileSystemLite::ReadFile( const char *path, void **buffer ) {
	void *raw = NULL;
	int len;

	if ( buffer ) {
		*buffer = NULL;
	}
	if ( !gImport.ReadFile ) {
		return -1;
	}
	len = gImport.ReadFile( path, &raw );
	if ( len < 0 || !raw ) {
		return -1;
	}
	if ( !buffer ) {
		gImport.FreeFile( raw );
		return len;
	}
	// a private NUL-terminated copy: the engine's buffer is freed at once
	char *copy = (char *)Mem_Alloc( len + 1 );
	memcpy( copy, raw, len );
	copy[len] = '\0';
	gImport.FreeFile( raw );
	*buffer = copy;
	return len;
}

void idFileSystemLite::FreeFile( void *buffer ) {
	Mem_Free( buffer );
}

/*
===============================================================================
	memory
===============================================================================
*/

void *Mem_Alloc( const int size ) {
	void *p = malloc( size > 0 ? size : 1 );
	if ( !p ) {
		common->FatalError( "Mem_Alloc: out of memory (%d bytes)", size );
	}
	return p;
}

void *Mem_ClearedAlloc( const int size ) {
	void *p = Mem_Alloc( size );
	memset( p, 0, size > 0 ? size : 1 );
	return p;
}

void Mem_Free( void *ptr ) {
	free( ptr );
}

char *Mem_CopyString( const char *in ) {
	char *out = (char *)Mem_Alloc( strlen( in ) + 1 );
	strcpy( out, in );
	return out;
}

/*
===============================================================================
	vectors and rotation
===============================================================================
*/

const char *idVec2::ToString( int precision ) const {
	return idStr::FloatArrayToString( ToFloatPtr(), GetDimension(), precision );
}

const char *idVec3::ToString( int precision ) const {
	return idStr::FloatArrayToString( ToFloatPtr(), GetDimension(), precision );
}

const char *idVec4::ToString( int precision ) const {
	return idStr::FloatArrayToString( ToFloatPtr(), GetDimension(), precision );
}

idMat3 idRotation::ToMat3( void ) const {
	float wx, wy, wz;
	float xx, yy, yz;
	float xy, xz, zz;
	float x2, y2, z2;
	float a, c, s, x, y, z;
	idMat3 axis;

	a = angle * ( idMath::M_DEG2RAD * 0.5f );
	idMath::SinCos( a, s, c );

	x = vec[0] * s;
	y = vec[1] * s;
	z = vec[2] * s;

	x2 = x + x;
	y2 = y + y;
	z2 = z + z;

	xx = x * x2;
	xy = x * y2;
	xz = x * z2;

	yy = y * y2;
	yz = y * z2;
	zz = z * z2;

	wx = c * x2;
	wy = c * y2;
	wz = c * z2;

	axis[ 0 ][ 0 ] = 1.0f - ( yy + zz );
	axis[ 0 ][ 1 ] = xy - wz;
	axis[ 0 ][ 2 ] = xz + wy;

	axis[ 1 ][ 0 ] = xy + wz;
	axis[ 1 ][ 1 ] = 1.0f - ( xx + zz );
	axis[ 1 ][ 2 ] = yz - wx;

	axis[ 2 ][ 0 ] = xz - wy;
	axis[ 2 ][ 1 ] = yz + wx;
	axis[ 2 ][ 2 ] = 1.0f - ( xx + yy );

	return axis;
}

/*
===============================================================================
	idDict
===============================================================================
*/

int idDict::FindKeyIndex( const char *key ) const {
	if ( !key || !key[0] ) {
		return -1;
	}
	for ( int i = 0; i < args.Num(); i++ ) {
		if ( args[i].key.Icmp( key ) == 0 ) {
			return i;
		}
	}
	return -1;
}

const idKeyValue *idDict::FindKey( const char *key ) const {
	int i = FindKeyIndex( key );
	return i >= 0 ? &args[i] : NULL;
}

void idDict::Set( const char *key, const char *value ) {
	if ( !key || !key[0] ) {
		return;
	}
	int i = FindKeyIndex( key );
	if ( i >= 0 ) {
		args[i].value = value ? value : "";
		return;
	}
	idKeyValue kv;
	kv.key = key;
	kv.value = value ? value : "";
	args.Append( kv );
}

void idDict::Delete( const char *key ) {
	int i = FindKeyIndex( key );
	if ( i >= 0 ) {
		args.RemoveIndex( i );
	}
}

const idKeyValue *idDict::MatchPrefix( const char *prefix, const idKeyValue *lastMatch ) const {
	int start = 0;
	int len = strlen( prefix );

	if ( lastMatch ) {
		start = ( lastMatch - args.Ptr() ) + 1;
	}
	for ( int i = start; i < args.Num(); i++ ) {
		if ( !args[i].key.Icmpn( prefix, len ) ) {
			return &args[i];
		}
	}
	return NULL;
}

idVec3 idDict::GetVector( const char *key, const char *defaultString ) const {
	idVec3 out;
	out.Zero();
	sscanf( GetString( key, defaultString ? defaultString : "0 0 0" ), "%f %f %f", &out.x, &out.y, &out.z );
	return out;
}

idVec2 idDict::GetVec2( const char *key, const char *defaultString ) const {
	idVec2 out;
	out.Zero();
	sscanf( GetString( key, defaultString ? defaultString : "0 0" ), "%f %f", &out.x, &out.y );
	return out;
}

idVec4 idDict::GetVec4( const char *key, const char *defaultString ) const {
	idVec4 out;
	out.Zero();
	sscanf( GetString( key, defaultString ? defaultString : "0 0 0 0" ), "%f %f %f %f", &out.x, &out.y, &out.z, &out.w );
	return out;
}

} // namespace idgui
