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
cl_cgame_oax_fx.c: cgame syscalls of the effects block (phase 6, inside
CG_OAX_R_BASE 1013-1017): stateless GPU particles, projected decals and
ribbon trails. The renderer implements them through refexport_t's OAX*Fx
entries (NULL in renderers without them); the tokens "particles",
"decals" and "trails" are advertised only by a renderer that has them
(CL_OAXRendererFeatures).
===========================================================================
*/

#include "client.h"
#include "../qcommon/oax.h"

#define OAX_MAX_TRAIL_POINTS 256

static qboolean CL_OAXFxCalls( intptr_t *args, intptr_t *ret ) {
	switch ( args[0] ) {
	case CG_OAX_R_REGISTERFX:
		*ret = re.OAXRegisterFx ? re.OAXRegisterFx( VMA( 1 ) ) : 0;
		return qtrue;
	case CG_OAX_R_ADDFX:
		VM_CheckBlock( args[1], sizeof( oaxFx_t ), "ADDFX" );
		*ret = re.OAXAddFx ? re.OAXAddFx( VMA( 1 ) ) : 0;
		return qtrue;
	case CG_OAX_R_ADDDECAL:
		VM_CheckBlock( args[1], sizeof( oaxDecal_t ), "ADDDECAL" );
		*ret = re.OAXAddDecal ? re.OAXAddDecal( VMA( 1 ) ) : 0;
		return qtrue;
	case CG_OAX_R_ADDTRAIL: {
		const oaxTrail_t *t;

		VM_CheckBlock( args[1], sizeof( oaxTrail_t ), "ADDTRAIL" );
		t = VMA( 1 );
		*ret = 0;
		if ( t->numPoints < 2 || t->numPoints > OAX_MAX_TRAIL_POINTS ) {
			return qtrue;
		}
		VM_CheckBlock( args[2], t->numPoints * 4 * sizeof( float ), "ADDTRAIL" );
		if ( re.OAXAddTrail ) {
			re.OAXAddTrail( t, VMA( 2 ) );
		}
		return qtrue;
	}
	case CG_OAX_R_CLEARDECALS:
		if ( re.OAXClearDecals ) {
			re.OAXClearDecals();
		}
		*ret = 0;
		return qtrue;
	}
	return qfalse;
}

void CL_OAXFxInit( void ) {
	CL_OAXRegisterCgameHandler( CL_OAXFxCalls );
}
