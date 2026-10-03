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
cl_cgame_oax.c: cgame syscalls from 1000 up (see qcommon/oax.h).

Each feature block registers one handler; CL_CgameSystemCalls hands any
number it does not know to CL_CgameSystemCallsOAX before it gives up.
===========================================================================
*/

#include "client.h"
#include "../qcommon/oax.h"

#define MAX_OAX_CGAME_HANDLERS 16

qboolean CL_OAXReverbCalls( intptr_t *args, intptr_t *ret );   // snd_reverb.c

static oaxSyscallHandler_t cgameHandlers[MAX_OAX_CGAME_HANDLERS];
static int                 numCgameHandlers;

void CL_OAXRegisterCgameHandler( oaxSyscallHandler_t h ) {
	int i;

	for ( i = 0; i < numCgameHandlers; i++ ) {
		if ( cgameHandlers[i] == h ) {
			return;
		}
	}
	if ( numCgameHandlers < MAX_OAX_CGAME_HANDLERS ) {
		cgameHandlers[numCgameHandlers++] = h;
	}
}

static qboolean CL_OAXInfraCalls( intptr_t *args, intptr_t *ret ) {
	switch ( args[0] ) {
	case CG_OAX_DEBUG_SET:
		Com_DebugSet( VMA( 1 ), VMA( 2 ) );
		*ret = 0;
		return qtrue;
	case CG_OAX_BSPX_READ:
		if ( args[3] > 0 ) {
			VM_CheckBlock( args[2], args[3], "BSPXREAD" );
		}
		*ret = BSPX_ReadCurrentMap( VMA( 1 ), args[3] > 0 ? VMA( 2 ) : NULL, args[3] );
		return qtrue;
	case CG_OAX_CM_TEMP_OBB: {
		// an oriented box (the server's G_OAX_ENT_SET_OBB): trace it with
		// CM_TransformedBoxTrace at the entity origin and zero angles
		const float *f;
		vec3_t axis[3];
		VM_CheckBlock( args[1], 15 * sizeof( float ), "CMOBB" );
		f = VMA( 1 );
		VectorCopy( f + 3, axis[0] );
		VectorCopy( f + 6, axis[1] );
		VectorCopy( f + 9, axis[2] );
		*ret = CM_OAXTempOBBModel( f, (const vec3_t *)axis, f + 12, args[2] );
		return qtrue;
	}
	}
	return qfalse;
}

/*
=================
CL_CgameSystemCallsOAX
=================
*/
qboolean CL_CgameSystemCallsOAX( intptr_t *args, intptr_t *ret ) {
	int i;

	if ( args[0] < 1000 || args[0] >= CG_OAX_END ) {
		return qfalse;
	}
	if ( CL_OAXInfraCalls( args, ret ) ) {
		return qtrue;
	}
	for ( i = 0; i < numCgameHandlers; i++ ) {
		if ( cgameHandlers[i]( args, ret ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

/*
=================
CL_OAXInit

Called once from CL_Init. Each feature block registers its handler (and
advertises its oax_features token) here, one line per feature.
=================
*/
void CL_OAXInit( void ) {
	CL_OAXRegisterCgameHandler( CL_OAXReverbCalls );
	OAX_AddFeature( "reverb" );
	CL_OAXGuiInit();
	CL_ULightInit();
	CL_OAXRenderInit();
	CL_PhysInit();		// "physics": cosmetic Box3D worlds, ragdoll skeletons
	CL_OAXFxInit();
	CL_OAXHooksInit();	// cl_oaxFreezeTime, view read-back
}
