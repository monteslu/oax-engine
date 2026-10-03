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
cl_oax_hooks.c: client-side verification hooks (step 7.5 D, docs/test-hooks.md).

- cl_oaxFreezeTime <ms> (cheat, -1 = off): one global freeze. Every scene
  the cgame renders gets refdef.time = <ms>, so the renderer's shader
  clock (animated stages, deforms, procedural textures, material and light
  expressions, water), the particle and trail clocks and anything else
  the renderer derives from the scene time are pinned. The oax cgame
  reads the same cvar and pins its own clocks (sky portal rotation, light
  styles, light trajectories, mover interpolation, item bob and spin).
  The per-feature pins (r_fixedShaderTime, cg_oaxSkyPortalTime,
  cg_oaxLightTime) still work and win over the freeze for their feature.
- View read-back: the main world view actually rendered (after
  cl_overrideView) is published every frame as the debug value
  `cl_view` = "x y z pitch yaw roll" and `cl_view_time` (the scene time
  the renderer was given), so a test can assert where a shot was taken
  from instead of where it asked to be.
===========================================================================
*/

#include "client.h"
#include "../qcommon/oax.h"

static cvar_t *cl_oaxFreezeTime;

void CL_OAXHooksInit( void ) {
	cl_oaxFreezeTime = Cvar_Get( "cl_oaxFreezeTime", "-1", CVAR_CHEAT | CVAR_TEMP );
	Cvar_SetDescription( cl_oaxFreezeTime, "Freeze every animated thing the client draws at this scene time in ms (-1 = off): shaders, sky portals, light styles and effects, particles, mover interpolation. For reproducible screenshots." );
	OAX_AddFeature( "freeze" );
}

/* the frozen scene time in ms, or -1 */
int CL_OAXFreezeTime( void ) {
	if ( !cl_oaxFreezeTime || cl_oaxFreezeTime->integer < 0 ) {
		return -1;
	}
	return cl_oaxFreezeTime->integer;
}

/*
=================
CL_OAXSceneHooks

Applied to every scene the cgame renders, after cl_overrideView.
=================
*/
void CL_OAXSceneHooks( refdef_t *ref ) {
	int t = CL_OAXFreezeTime();

	if ( t >= 0 ) {
		ref->time = t;
	}
}

/*
=================
CL_OAXViewReadback

The main world view as rendered.
=================
*/
void CL_OAXViewReadback( const refdef_t *ref ) {
	vec3_t	angles, left;
	float	roll;

	vectoangles( ref->viewaxis[0], angles );
	// roll: the angle of the view's left axis about the forward axis,
	// relative to a level left axis
	{
		vec3_t	levelLeft, up;
		vec3_t	level;

		VectorSet( level, 0, angles[YAW], 0 );
		AngleVectors( level, NULL, levelLeft, NULL );
		VectorScale( levelLeft, -1, levelLeft );	// AngleVectors gives right
		VectorCopy( ref->viewaxis[1], left );
		CrossProduct( ref->viewaxis[0], levelLeft, up );
		roll = RAD2DEG( atan2( DotProduct( left, up ), DotProduct( left, levelLeft ) ) );
	}
	Com_DebugSet( "cl_view", va( "%.3f %.3f %.3f %.4f %.4f %.4f", ref->vieworg[0], ref->vieworg[1], ref->vieworg[2],
		AngleNormalize180( angles[PITCH] ), AngleNormalize180( angles[YAW] ), roll ) );
	Com_DebugSetInt( "cl_view_time", ref->time );
}
