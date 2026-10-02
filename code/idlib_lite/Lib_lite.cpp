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

Adapted from DOOM-3 neo/idlib/Lib.cpp, neo/idlib/math/Math.cpp,
Vector.cpp and Angles.cpp: idLib's static interface, the idMath constants,
idVec3::ToAngles/ToYaw/ToPitch and idAngles::ToVectors/ToForward.
Changed: idLib::common is a small printer that forwards to the engine and
whose Error() calls an installable non-returning handler instead of
throwing; atan2/sin/cos are the engine's deterministic functions.

===========================================================================
*/

#include "idlib_lite.h"

static idCommon				commonLite;
idCommon *					idLib::common = &commonLite;
int							idLib::frameNumber = 0;

static idLibErrorHandler_t	errorHandler;

idLibErrorHandler_t idLib_SetErrorHandler( idLibErrorHandler_t handler ) {
	idLibErrorHandler_t old = errorHandler;
	errorHandler = handler;
	return old;
}

static void idLib_Fail( const char *text ) {
	if ( errorHandler ) {
		errorHandler( text );
	}
	idlib_FatalError( text );
}

#define FORMAT_TEXT( text, fmt ) \
	char text[MAX_STRING_CHARS]; \
	va_list argptr; \
	va_start( argptr, fmt ); \
	idStr::vsnPrintf( text, sizeof( text ), fmt, argptr ); \
	va_end( argptr );

void idCommon::Printf( const char *fmt, ... ) {
	FORMAT_TEXT( text, fmt );
	idlib_Print( text );
}

void idCommon::DPrintf( const char *fmt, ... ) {
	FORMAT_TEXT( text, fmt );
	idlib_DPrint( text );
}

void idCommon::Warning( const char *fmt, ... ) {
	FORMAT_TEXT( text, fmt );
	idlib_Print( "^3WARNING: " );
	idlib_Print( text );
	idlib_Print( "\n" );
}

void idCommon::DWarning( const char *fmt, ... ) {
	FORMAT_TEXT( text, fmt );
	idlib_DPrint( "WARNING: " );
	idlib_DPrint( text );
	idlib_DPrint( "\n" );
}

void idCommon::Error( const char *fmt, ... ) {
	FORMAT_TEXT( text, fmt );
	idLib_Fail( text );
}

void idCommon::FatalError( const char *fmt, ... ) {
	FORMAT_TEXT( text, fmt );
	idLib_Fail( text );
}

void idLib::Error( const char *fmt, ... ) {
	FORMAT_TEXT( text, fmt );
	idLib_Fail( text );
}

void idLib::Warning( const char *fmt, ... ) {
	FORMAT_TEXT( text, fmt );
	commonLite.Warning( "%s", text );
}

// ---- idMath --------------------------------------------------------------------

const float	idMath::PI				= 3.14159265358979323846f;
const float	idMath::TWO_PI			= 2.0f * PI;
const float	idMath::HALF_PI			= 0.5f * PI;
const float	idMath::ONEFOURTH_PI	= 0.25f * PI;
const float idMath::E				= 2.71828182845904523536f;
const float idMath::SQRT_TWO		= 1.41421356237309504880f;
const float idMath::SQRT_THREE		= 1.73205080756887729352f;
const float	idMath::SQRT_1OVER2		= 0.70710678118654752440f;
const float	idMath::SQRT_1OVER3		= 0.57735026918962576450f;
const float	idMath::M_DEG2RAD		= PI / 180.0f;
const float	idMath::M_RAD2DEG		= 180.0f / PI;
const float	idMath::M_SEC2MS		= 1000.0f;
const float	idMath::M_MS2SEC		= 0.001f;
const float	idMath::INFINITY		= 1e30f;
const float idMath::FLT_EPSILON		= 1.192092896e-07f;

idVec3 vec3_origin_lite( 0.0f, 0.0f, 0.0f );

/*
=============
idVec3::ToYaw
=============
*/
float idVec3::ToYaw( void ) const {
	float yaw;

	if ( ( y == 0.0f ) && ( x == 0.0f ) ) {
		yaw = 0.0f;
	} else {
		yaw = RAD2DEG( idMath::ATan( y, x ) );
		if ( yaw < 0.0f ) {
			yaw += 360.0f;
		}
	}

	return yaw;
}

/*
=============
idVec3::ToPitch
=============
*/
float idVec3::ToPitch( void ) const {
	float	forward;
	float	pitch;

	if ( ( x == 0.0f ) && ( y == 0.0f ) ) {
		if ( z > 0.0f ) {
			pitch = 90.0f;
		} else {
			pitch = 270.0f;
		}
	} else {
		forward = ( float )idMath::Sqrt( x * x + y * y );
		pitch = RAD2DEG( idMath::ATan( z, forward ) );
		if ( pitch < 0.0f ) {
			pitch += 360.0f;
		}
	}

	return pitch;
}

/*
=============
idVec3::ToAngles
=============
*/
idAngles idVec3::ToAngles( void ) const {
	float forward;
	float yaw;
	float pitch;

	if ( ( x == 0.0f ) && ( y == 0.0f ) ) {
		yaw = 0.0f;
		if ( z > 0.0f ) {
			pitch = 90.0f;
		} else {
			pitch = 270.0f;
		}
	} else {
		yaw = RAD2DEG( idMath::ATan( y, x ) );
		if ( yaw < 0.0f ) {
			yaw += 360.0f;
		}

		forward = ( float )idMath::Sqrt( x * x + y * y );
		pitch = RAD2DEG( idMath::ATan( z, forward ) );
		if ( pitch < 0.0f ) {
			pitch += 360.0f;
		}
	}

	return idAngles( -pitch, yaw, 0.0f );
}

/*
=================
idAngles::ToVectors
=================
*/
void idAngles::ToVectors( idVec3 *forward, idVec3 *right, idVec3 *up ) const {
	float sr, sp, sy, cr, cp, cy;

	idMath::SinCos( DEG2RAD( yaw ), sy, cy );
	idMath::SinCos( DEG2RAD( pitch ), sp, cp );
	idMath::SinCos( DEG2RAD( roll ), sr, cr );

	if ( forward ) {
		forward->Set( cp * cy, cp * sy, -sp );
	}

	if ( right ) {
		right->Set( -sr * sp * cy + cr * sy, -sr * sp * sy + -cr * cy, -sr * cp );
	}

	if ( up ) {
		up->Set( cr * sp * cy + -sr * -sy, cr * sp * sy + -sr * cy, cr * cp );
	}
}

/*
=================
idAngles::ToForward
=================
*/
idVec3 idAngles::ToForward( void ) const {
	float sp, sy, cp, cy;

	idMath::SinCos( DEG2RAD( yaw ), sy, cy );
	idMath::SinCos( DEG2RAD( pitch ), sp, cp );

	return idVec3( cp * cy, cp * sy, -sp );
}

/*
=============
idVec3::ToString
=============
*/
const char *idVec3::ToString( int precision ) const {
	return idStr::FloatArrayToString( ToFloatPtr(), 3, precision );
}
