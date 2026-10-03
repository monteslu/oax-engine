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
cl_cgame_phys.c: physics for the cgame (cosmetic worlds) and the skeleton
calls ragdolls draw with (block 1200-1299, qcommon/oax.h; structs in
physics/oax_phys.h).

1200-1249 go to the shared physics module with the cgame as owner. The
skeleton calls (1250-1253) need a renderer that implements them; it says
so with the "physics_skel" token (renderergl2).
===========================================================================
*/

#include "client.h"
#include "../qcommon/oax.h"
#include "../physics/oax_phys.h"
#include "../physics/phys_public.h"

static qboolean CL_PhysSkelCalls( intptr_t *args, intptr_t *ret ) {
	switch ( args[0] ) {
	case PHYS_R_MODEL_SKELETON:
		*ret = 0;
		if ( args[3] > 0 ) {
			VM_CheckBlock( args[2], args[3] * sizeof( oaxSkelJoint_t ), "SKELJOINTS" );
		}
		if ( re.OAXModelSkeleton ) {
			*ret = re.OAXModelSkeleton( args[1], args[3] > 0 ? VMA( 2 ) : NULL, args[3] );
		}
		return qtrue;
	case PHYS_R_LERP_SKELETON:
		*ret = 0;
		if ( args[6] > 0 ) {
			VM_CheckBlock( args[5], args[6] * PHYS_SKEL_MAT_FLOATS * sizeof( float ), "SKELMATS" );
		}
		if ( re.OAXLerpSkeleton && args[6] > 0 ) {
			*ret = re.OAXLerpSkeleton( args[1], args[2], args[3], VMF( 4 ), VMA( 5 ), args[6] );
		}
		return qtrue;
	case PHYS_R_ADD_SKELETAL_ENTITY:
		*ret = 0;
		VM_CheckBlock( args[1], sizeof( refEntity_t ), "SKELENT" );
		if ( args[3] > 0 ) {
			VM_CheckBlock( args[2], args[3] * PHYS_SKEL_MAT_FLOATS * sizeof( float ), "SKELENTMATS" );
		}
		if ( re.OAXAddSkeletalEntity && args[3] > 0 ) {
			re.OAXAddSkeletalEntity( VMA( 1 ), VMA( 2 ), args[3] );
		} else {
			re.AddRefEntityToScene( VMA( 1 ) );
		}
		return qtrue;
	case PHYS_R_MODEL_FRAMES:
		*ret = re.OAXModelFrames ? re.OAXModelFrames( args[1] ) : 0;
		return qtrue;
	}
	return qfalse;
}

void CL_PhysInit( void ) {
	Phys_Init();
	CL_OAXRegisterCgameHandler( Phys_CgameCalls );
	CL_OAXRegisterCgameHandler( CL_PhysSkelCalls );
	OAX_AddFeature( "physics" );
	OAX_AddFeature( "physics_vehicle" );
}
