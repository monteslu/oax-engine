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

Adapted from DOOM-3 neo/idlib/math/Math.h, Vector.h/.cpp and Angles.h/.cpp:
only idMath constants and the scalar functions the script VM uses, idVec3
and idAngles with the operations it uses. Changed: sin, cos and atan2 go
through the engine's deterministic musl functions and InvSqrt is an exact
1/sqrt (D3's table approximation and SSE paths are gone), so every build
gives the same bits.

===========================================================================
*/

#ifndef __MATH_LITE_H__
#define __MATH_LITE_H__

#ifdef INFINITY
#undef INFINITY
#endif

#define DEG2RAD(a)				( (a) * idMath::M_DEG2RAD )
#define RAD2DEG(a)				( (a) * idMath::M_RAD2DEG )
#define SEC2MS(t)				( idMath::FtoiRound( (t) * idMath::M_SEC2MS ) )
#define MS2SEC(t)				( (t) * idMath::M_MS2SEC )
#define INTSIGNBITNOTSET(i)		( ( ~( ( const unsigned int )( i ) ) ) >> 31 )
#define INTSIGNBITSET(i)		( ( ( const unsigned int )( i ) ) >> 31 )

class idMath {
public:
	static float				Sqrt( float x ) { return sqrtf( x ); }
	static float				InvSqrt( float x ) { return ( x > 0.0f ) ? 1.0f / sqrtf( x ) : INFINITY; }
	static float				Sin( float a ) { return (float)idlib_Sin( a ); }
	static float				Cos( float a ) { return (float)idlib_Cos( a ); }
	static void					SinCos( float a, float &s, float &c ) { s = Sin( a ); c = Cos( a ); }
	static float				ATan( float y, float x ) { return (float)idlib_Atan2( y, x ); }
	static float				Fabs( float f ) { return fabsf( f ); }
	static float				Floor( float f ) { return floorf( f ); }
	static int					Ftoi( float f ) { return (int)f; }
	static int					FtoiFast( float f ) { return (int)f; }
	static long					Ftol( float f ) { return (long)f; }
	// round to nearest, halves away from zero (D3's FtoiFast rounded with
	// the FPU's mode; this is the same on every build)
	static int					FtoiRound( float f ) { return (int)( f < 0.0f ? f - 0.5f : f + 0.5f ); }
	static bool					IsPowerOfTwo( int x ) { return ( x & ( x - 1 ) ) == 0 && x > 0; }

	static const float			PI;
	static const float			TWO_PI;
	static const float			HALF_PI;
	static const float			ONEFOURTH_PI;
	static const float			E;
	static const float			SQRT_TWO;
	static const float			SQRT_THREE;
	static const float			SQRT_1OVER2;
	static const float			SQRT_1OVER3;
	static const float			M_DEG2RAD;
	static const float			M_RAD2DEG;
	static const float			M_SEC2MS;
	static const float			M_MS2SEC;
	static const float			INFINITY;
	static const float			FLT_EPSILON;
};

class idAngles;

class idVec3 {
public:
	float			x;
	float			y;
	float			z;

					idVec3( void ) {}
					explicit idVec3( const float x, const float y, const float z ) { this->x = x; this->y = y; this->z = z; }

	void 			Set( const float x, const float y, const float z ) { this->x = x; this->y = y; this->z = z; }
	void			Zero( void ) { x = y = z = 0.0f; }

	float			operator[]( const int index ) const { return ( &x )[ index ]; }
	float &			operator[]( const int index ) { return ( &x )[ index ]; }
	idVec3			operator-() const { return idVec3( -x, -y, -z ); }
	float			operator*( const idVec3 &a ) const { return x * a.x + y * a.y + z * a.z; }
	idVec3			operator*( const float a ) const { return idVec3( x * a, y * a, z * a ); }
	idVec3			operator/( const float a ) const { float inva = 1.0f / a; return idVec3( x * inva, y * inva, z * inva ); }
	idVec3			operator+( const idVec3 &a ) const { return idVec3( x + a.x, y + a.y, z + a.z ); }
	idVec3			operator-( const idVec3 &a ) const { return idVec3( x - a.x, y - a.y, z - a.z ); }
	idVec3 &		operator+=( const idVec3 &a ) { x += a.x; y += a.y; z += a.z; return *this; }
	idVec3 &		operator-=( const idVec3 &a ) { x -= a.x; y -= a.y; z -= a.z; return *this; }
	idVec3 &		operator/=( const float a ) { float inva = 1.0f / a; x *= inva; y *= inva; z *= inva; return *this; }
	idVec3 &		operator*=( const float a ) { x *= a; y *= a; z *= a; return *this; }
	friend idVec3	operator*( const float a, const idVec3 b ) { return idVec3( b.x * a, b.y * a, b.z * a ); }

	bool			Compare( const idVec3 &a ) const { return ( ( x == a.x ) && ( y == a.y ) && ( z == a.z ) ); }
	bool			operator==( const idVec3 &a ) const { return Compare( a ); }
	bool			operator!=( const idVec3 &a ) const { return !Compare( a ); }

	idVec3			Cross( const idVec3 &a ) const { return idVec3( y * a.z - z * a.y, z * a.x - x * a.z, x * a.y - y * a.x ); }
	float			Length( void ) const { return idMath::Sqrt( x * x + y * y + z * z ); }
	float			LengthSqr( void ) const { return ( x * x + y * y + z * z ); }
	float			Normalize( void ) {
						float sqrLength = x * x + y * y + z * z;
						float invLength = idMath::InvSqrt( sqrLength );
						x *= invLength; y *= invLength; z *= invLength;
						return invLength * sqrLength;
					}

	idAngles		ToAngles( void ) const;
	float			ToYaw( void ) const;
	float			ToPitch( void ) const;
	const char *	ToString( int precision = 2 ) const;
	const float *	ToFloatPtr( void ) const { return &x; }
	float *			ToFloatPtr( void ) { return &x; }
};

// (renamed: ioquake3 has a C vec3_origin)
extern idVec3 vec3_origin_lite;
#define vec3_origin vec3_origin_lite
#define vec3_zero vec3_origin_lite

class idAngles {
public:
	float			pitch;
	float			yaw;
	float			roll;

					idAngles( void ) {}
					idAngles( float pitch, float yaw, float roll ) { this->pitch = pitch; this->yaw = yaw; this->roll = roll; }
					explicit idAngles( const idVec3 &v ) { pitch = v.x; yaw = v.y; roll = v.z; }

	void			Set( float pitch, float yaw, float roll ) { this->pitch = pitch; this->yaw = yaw; this->roll = roll; }
	void			ToVectors( idVec3 *forward, idVec3 *right = NULL, idVec3 *up = NULL ) const;
	idVec3			ToForward( void ) const;
	const idVec3 &	ToVec3( void ) const { return *reinterpret_cast<const idVec3 *>( &pitch ); }
};

#endif /* !__MATH_LITE_H__ */
