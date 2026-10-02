/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// q_detmath.c: the QVM math traps on deterministic math (qcommon/detmath),
// so gamecode computes the same bits on every build.

#include "q_shared.h"
#include "qcommon.h"

float Q_detSinf( float x ) {
	return (float)Q_detSin( x );
}

float Q_detCosf( float x ) {
	return (float)Q_detCos( x );
}

float Q_detAtan2f( float y, float x ) {
	return (float)Q_detAtan2( y, x );
}

// Q_acos with deterministic acos
float Q_detAcosf( float c ) {
	float angle = (float)Q_detAcos( c );

	if ( angle > M_PI ) {
		return (float)M_PI;
	}
	if ( angle < -M_PI ) {
		return (float)M_PI;
	}
	return angle;
}

// AngleVectors (q_math.c) with deterministic sin and cos
void Q_detAngleVectors( const vec3_t angles, vec3_t forward, vec3_t right, vec3_t up ) {
	float angle;
	float sr, sp, sy, cr, cp, cy;

	angle = angles[YAW] * ( M_PI * 2 / 360 );
	sy = Q_detSin( angle );
	cy = Q_detCos( angle );
	angle = angles[PITCH] * ( M_PI * 2 / 360 );
	sp = Q_detSin( angle );
	cp = Q_detCos( angle );
	angle = angles[ROLL] * ( M_PI * 2 / 360 );
	sr = Q_detSin( angle );
	cr = Q_detCos( angle );

	if ( forward ) {
		forward[0] = cp * cy;
		forward[1] = cp * sy;
		forward[2] = -sp;
	}
	if ( right ) {
		right[0] = ( -1 * sr * sp * cy + -1 * cr * -sy );
		right[1] = ( -1 * sr * sp * sy + -1 * cr * cy );
		right[2] = -1 * sr * cp;
	}
	if ( up ) {
		up[0] = ( cr * sp * cy + -sr * -sy );
		up[1] = ( cr * sp * sy + -sr * cy );
		up[2] = cr * cp;
	}
}
