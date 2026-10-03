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
cl_cgame_ulight.c: unified lighting (phase 5) cgame syscalls, block
1050-1069 (qcommon/oax.h).

CG_OAX_R_UPDATELIGHTDEF lets the cgame drive a light of a unified or hybrid
map (keyed by its ordinal in the entity lump): origin, optional axis,
color, optional shader parms, on/off. Lights the cgame never touches stay
where the map put them, so a stock cgame still sees every light.
===========================================================================
*/

#include "client.h"
#include "../qcommon/oax.h"

static qboolean CL_ULightCgameCalls( intptr_t *args, intptr_t *ret ) {
	switch ( args[0] ) {
	case CG_OAX_R_UPDATELIGHTDEF:
		VM_CheckBlock( args[2], sizeof( vec3_t ), "ULIGHTORG" );
		if ( args[3] ) {
			VM_CheckBlock( args[3], 9 * sizeof( float ), "ULIGHTAXIS" );
		}
		VM_CheckBlock( args[4], sizeof( vec3_t ), "ULIGHTRGB" );
		if ( args[5] ) {
			VM_CheckBlock( args[5], 12 * sizeof( float ), "ULIGHTPARMS" );
		}
		if ( re.OAXUpdateLight ) {
			re.OAXUpdateLight( args[1], VMA( 2 ), args[3] ? VMA( 3 ) : NULL, VMA( 4 ),
				args[5] ? VMA( 5 ) : NULL, args[6] );
		}
		*ret = 0;
		return qtrue;
	}
	return qfalse;
}

void CL_ULightInit( void ) {
	CL_OAXRegisterCgameHandler( CL_ULightCgameCalls );
	OAX_AddFeature( "ulight" );
}
