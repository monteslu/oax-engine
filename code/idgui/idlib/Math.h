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
Adapted from DOOM-3 neo/idlib/math/Math.h, Vector.h, Matrix.h and
Rotation.cpp for the oa-engine GUI module.
Changes: only what the GUI port uses (idMath scalars, idVec2/3/4, idMat3,
idRotation about an axis). The lookup-table and SIMD paths are gone; the
scalar functions call the C library directly, and sine and cosine
call the engine's deterministic musl ports. Lives in namespace idgui.
*/

#ifndef __IDGUI_MATH_H__
#define __IDGUI_MATH_H__

#ifdef INFINITY
#undef INFINITY
#endif

#ifdef FLT_EPSILON
#undef FLT_EPSILON
#endif

// the engine's deterministic trig (qcommon/q_detmath.c, musl): native and
// the cart compute the same transitions
// (declared in idgui_precompiled.h, outside the namespace)

#define DEG2RAD(a)				( (a) * idMath::M_DEG2RAD )
#define RAD2DEG(a)				( (a) * idMath::M_RAD2DEG )

#define SEC2MS(t)				( idMath::FtoiFast( (t) * idMath::M_SEC2MS ) )
#define MS2SEC(t)				( (t) * idMath::M_MS2SEC )

class idMath {
public:
	static float				Sqrt( float x ) { return sqrtf( x ); }
	static float				InvSqrt( float x ) { return ( x > 0.0f ) ? 1.0f / sqrtf( x ) : 1e30f; }
	static float				Sin( float a ) { return Q_detSinf( a ); }
	static float				Cos( float a ) { return Q_detCosf( a ); }
	static void					SinCos( float a, float &s, float &c ) { s = Q_detSinf( a ); c = Q_detCosf( a ); }
	static float				Fabs( float f ) { return fabsf( f ); }
	static float				Floor( float f ) { return floorf( f ); }
	static float				Ceil( float f ) { return ceilf( f ); }
	static int					Ftoi( float f ) { return (int) f; }
	static int					FtoiFast( float f ) { return (int) f; }
	static unsigned int		Ftol( float f ) { return (unsigned int) f; }
	static int					ClampInt( int min, int max, int value ) { return value < min ? min : value > max ? max : value; }
	static float				ClampFloat( float min, float max, float value ) { return value < min ? min : value > max ? max : value; }

	static const float			PI;
	static const float			TWO_PI;
	static const float			HALF_PI;
	static const float			SQRT_1OVER2;
	static const float			M_DEG2RAD;
	static const float			M_RAD2DEG;
	static const float			M_SEC2MS;
	static const float			M_MS2SEC;
	static const float			INFINITY;
	static const float			FLT_EPSILON;
};

//===============================================================
//	idVec2
//===============================================================

class idVec2 {
public:
	float			x;
	float			y;

					idVec2( void ) {}
					explicit idVec2( const float x, const float y ) { this->x = x; this->y = y; }

	void 			Set( const float x, const float y ) { this->x = x; this->y = y; }
	void			Zero( void ) { x = y = 0.0f; }

	float			operator[]( int index ) const { return ( &x )[ index ]; }
	float &			operator[]( int index ) { return ( &x )[ index ]; }
	idVec2			operator-() const { return idVec2( -x, -y ); }
	float			operator*( const idVec2 &a ) const { return x * a.x + y * a.y; }
	idVec2			operator*( const float a ) const { return idVec2( x * a, y * a ); }
	idVec2			operator/( const float a ) const { float inva = 1.0f / a; return idVec2( x * inva, y * inva ); }
	idVec2			operator+( const idVec2 &a ) const { return idVec2( x + a.x, y + a.y ); }
	idVec2			operator-( const idVec2 &a ) const { return idVec2( x - a.x, y - a.y ); }
	idVec2 &		operator+=( const idVec2 &a ) { x += a.x; y += a.y; return *this; }
	idVec2 &		operator-=( const idVec2 &a ) { x -= a.x; y -= a.y; return *this; }
	idVec2 &		operator*=( const float a ) { x *= a; y *= a; return *this; }
	bool			operator==( const idVec2 &a ) const { return x == a.x && y == a.y; }
	bool			operator!=( const idVec2 &a ) const { return !( *this == a ); }

	int				GetDimension( void ) const { return 2; }
	const float *	ToFloatPtr( void ) const { return &x; }
	float *			ToFloatPtr( void ) { return &x; }
	const char *	ToString( int precision = 2 ) const;
};

//===============================================================
//	idVec3
//===============================================================

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
	idVec3 &		operator*=( const float a ) { x *= a; y *= a; z *= a; return *this; }
	idVec3 &		operator*=( const class idMat3 &mat );
	bool			operator==( const idVec3 &a ) const { return x == a.x && y == a.y && z == a.z; }
	bool			operator!=( const idVec3 &a ) const { return !( *this == a ); }

	float			Length( void ) const { return idMath::Sqrt( x * x + y * y + z * z ); }
	float			LengthSqr( void ) const { return x * x + y * y + z * z; }
	float			Normalize( void ) {
						float sqrLength = x * x + y * y + z * z;
						float invLength = idMath::InvSqrt( sqrLength );
						x *= invLength; y *= invLength; z *= invLength;
						return invLength * sqrLength;
					}
	idVec3			Cross( const idVec3 &a ) const { return idVec3( y * a.z - z * a.y, z * a.x - x * a.z, x * a.y - y * a.x ); }

	int				GetDimension( void ) const { return 3; }
	const float *	ToFloatPtr( void ) const { return &x; }
	float *			ToFloatPtr( void ) { return &x; }
	const char *	ToString( int precision = 2 ) const;
};

//===============================================================
//	idVec4
//===============================================================

class idVec4 {
public:
	float			x;
	float			y;
	float			z;
	float			w;

					idVec4( void ) {}
					explicit idVec4( const float x, const float y, const float z, const float w ) { this->x = x; this->y = y; this->z = z; this->w = w; }

	void 			Set( const float x, const float y, const float z, const float w ) { this->x = x; this->y = y; this->z = z; this->w = w; }
	void			Zero( void ) { x = y = z = w = 0.0f; }

	float			operator[]( const int index ) const { return ( &x )[ index ]; }
	float &			operator[]( const int index ) { return ( &x )[ index ]; }
	idVec4			operator-() const { return idVec4( -x, -y, -z, -w ); }
	float			operator*( const idVec4 &a ) const { return x * a.x + y * a.y + z * a.z + w * a.w; }
	idVec4			operator*( const float a ) const { return idVec4( x * a, y * a, z * a, w * a ); }
	idVec4			operator/( const float a ) const { float inva = 1.0f / a; return idVec4( x * inva, y * inva, z * inva, w * inva ); }
	idVec4			operator+( const idVec4 &a ) const { return idVec4( x + a.x, y + a.y, z + a.z, w + a.w ); }
	idVec4			operator-( const idVec4 &a ) const { return idVec4( x - a.x, y - a.y, z - a.z, w - a.w ); }
	idVec4 &		operator+=( const idVec4 &a ) { x += a.x; y += a.y; z += a.z; w += a.w; return *this; }
	idVec4 &		operator-=( const idVec4 &a ) { x -= a.x; y -= a.y; z -= a.z; w -= a.w; return *this; }
	idVec4 &		operator*=( const float a ) { x *= a; y *= a; z *= a; w *= a; return *this; }
	bool			operator==( const idVec4 &a ) const { return x == a.x && y == a.y && z == a.z && w == a.w; }
	bool			operator!=( const idVec4 &a ) const { return !( *this == a ); }
	friend idVec4	operator*( const float a, const idVec4 b ) { return idVec4( b.x * a, b.y * a, b.z * a, b.w * a ); }

	bool			Compare( const idVec4 &a ) const { return *this == a; }
	int				GetDimension( void ) const { return 4; }
	const idVec2 &	ToVec2( void ) const { return *reinterpret_cast<const idVec2 *>( this ); }
	idVec2 &		ToVec2( void ) { return *reinterpret_cast<idVec2 *>( this ); }
	const idVec3 &	ToVec3( void ) const { return *reinterpret_cast<const idVec3 *>( this ); }
	idVec3 &		ToVec3( void ) { return *reinterpret_cast<idVec3 *>( this ); }
	const float *	ToFloatPtr( void ) const { return &x; }
	float *			ToFloatPtr( void ) { return &x; }
	const char *	ToString( int precision = 2 ) const;
};

extern idVec3 vec3_origin;
#define vec3_zero vec3_origin

//===============================================================
//	idMat3 (row major, as in DOOM-3)
//===============================================================

class idMat3 {
public:
					idMat3( void ) {}
					explicit idMat3( const idVec3 &x, const idVec3 &y, const idVec3 &z ) { mat[0] = x; mat[1] = y; mat[2] = z; }

	const idVec3 &	operator[]( int index ) const { return mat[ index ]; }
	idVec3 &		operator[]( int index ) { return mat[ index ]; }

	idMat3			operator*( const idMat3 &a ) const {
						idMat3 dst;
						for ( int i = 0; i < 3; i++ ) {
							for ( int j = 0; j < 3; j++ ) {
								dst.mat[i][j] = mat[i][0] * a.mat[0][j] + mat[i][1] * a.mat[1][j] + mat[i][2] * a.mat[2][j];
							}
						}
						return dst;
					}
	idMat3 &		operator*=( const idMat3 &a ) { *this = *this * a; return *this; }
	friend idVec3	operator*( const idVec3 &vec, const idMat3 &mat ) {
						return idVec3(
							mat.mat[ 0 ].x * vec.x + mat.mat[ 1 ].x * vec.y + mat.mat[ 2 ].x * vec.z,
							mat.mat[ 0 ].y * vec.x + mat.mat[ 1 ].y * vec.y + mat.mat[ 2 ].y * vec.z,
							mat.mat[ 0 ].z * vec.x + mat.mat[ 1 ].z * vec.y + mat.mat[ 2 ].z * vec.z );
					}

	void			Zero( void ) { mat[0].Zero(); mat[1].Zero(); mat[2].Zero(); }
	void			Identity( void ) { mat[0].Set( 1, 0, 0 ); mat[1].Set( 0, 1, 0 ); mat[2].Set( 0, 0, 1 ); }
	bool			IsIdentity( void ) const {
						return mat[0] == idVec3( 1, 0, 0 ) && mat[1] == idVec3( 0, 1, 0 ) && mat[2] == idVec3( 0, 0, 1 );
					}

private:
	idVec3			mat[ 3 ];
};

ID_INLINE idVec3 &idVec3::operator*=( const idMat3 &mat ) {
	*this = *this * mat;
	return *this;
}

extern idMat3 mat3_identity;

//===============================================================
//	idRotation: angle in degrees about a vector through an origin
//===============================================================

class idRotation {
public:
					idRotation( void ) { angle = 0.0f; vec.Set( 0, 0, 1 ); origin.Zero(); }
	void			Set( const idVec3 &rotationOrigin, const idVec3 &rotationVec, const float rotationAngle ) {
						origin = rotationOrigin;
						vec = rotationVec;
						angle = rotationAngle;
					}
	idMat3			ToMat3( void ) const;

private:
	idVec3			origin;
	idVec3			vec;
	float			angle;
};

#endif
