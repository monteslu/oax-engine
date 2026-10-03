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
sv_game_oax.c: game (qagame) syscalls from 1000 up (see qcommon/oax.h).

Each feature block registers one handler; SV_GameSystemCalls hands any
number it does not know to SV_GameSystemCallsOAX before it gives up.
===========================================================================
*/

#include "server.h"
#include "../qcommon/oax.h"
#include "../physics/phys_public.h"

#define MAX_OAX_GAME_HANDLERS 16

static oaxSyscallHandler_t gameHandlers[MAX_OAX_GAME_HANDLERS];
static int                 numGameHandlers;

void SV_OAXRegisterGameHandler( oaxSyscallHandler_t h ) {
	int i;

	for ( i = 0; i < numGameHandlers; i++ ) {
		if ( gameHandlers[i] == h ) {
			return;
		}
	}
	if ( numGameHandlers < MAX_OAX_GAME_HANDLERS ) {
		gameHandlers[numGameHandlers++] = h;
	}
}

static qboolean SV_OAXInfraCalls( intptr_t *args, intptr_t *ret ) {
	switch ( args[0] ) {
	case G_OAX_DEBUG_SET:
		Com_DebugSet( VMA( 1 ), VMA( 2 ) );
		*ret = 0;
		return qtrue;
	case G_OAX_BSPX_READ:
		if ( args[3] > 0 ) {
			VM_CheckBlock( args[2], args[3], "BSPXREAD" );
		}
		*ret = BSPX_ReadCurrentMap( VMA( 1 ), args[3] > 0 ? VMA( 2 ) : NULL, args[3] );
		return qtrue;
	}
	return qfalse;
}

/*
==============================================================================
oriented entity boxes (token "ent_obb", G_OAX_ENT_SET_OBB): an entity that
is not a brush model can collide as a box with any orientation (vehicles)
instead of its axis-aligned mins/maxs. Traces, point contents and entity
contact all use it (SV_ClipHandleForEntity). The box is relative to
r.currentOrigin: center offset, axis, half extents.
==============================================================================
*/

typedef struct {
	qboolean	active;
	vec3_t		center;			// from r.currentOrigin
	vec3_t		axis[3];
	vec3_t		half;
} svEntOBB_t;

static svEntOBB_t svEntOBB[MAX_GENTITIES];

void SV_OAXClearEntityOBBs( void ) {
	Com_Memset( svEntOBB, 0, sizeof( svEntOBB ) );
}

qboolean SV_OAXEntityOBB( const sharedEntity_t *ent, clipHandle_t *handle ) {
	int num = ent->s.number;
	const svEntOBB_t *o;

	if ( num < 0 || num >= MAX_GENTITIES || ent->r.bmodel || !svEntOBB[num].active ) {
		return qfalse;
	}
	o = &svEntOBB[num];
	*handle = CM_OAXTempOBBModel( o->center, o->axis, o->half, ent->r.contents );
	return qtrue;
}

static qboolean SV_OAXEntCalls( intptr_t *args, intptr_t *ret ) {
	switch ( args[0] ) {
	case G_OAX_ENT_SET_OBB: {
		int num = args[1];
		if ( num < 0 || num >= MAX_GENTITIES ) {
			return qtrue;
		}
		if ( !args[2] ) {
			svEntOBB[num].active = qfalse;
		} else {
			const float *f;
			VM_CheckBlock( args[2], 15 * sizeof( float ), "ENTOBB" );
			f = VMA( 2 );
			VectorCopy( f, svEntOBB[num].center );
			VectorCopy( f + 3, svEntOBB[num].axis[0] );
			VectorCopy( f + 6, svEntOBB[num].axis[1] );
			VectorCopy( f + 9, svEntOBB[num].axis[2] );
			VectorCopy( f + 12, svEntOBB[num].half );
			svEntOBB[num].active = qtrue;
		}
		*ret = 0;
		return qtrue;
	}
	}
	return qfalse;
}

/*
=================
SV_GameSystemCallsOAX
=================
*/
qboolean SV_GameSystemCallsOAX( intptr_t *args, intptr_t *ret ) {
	int i;

	if ( args[0] < 1000 || args[0] >= G_OAX_END ) {
		return qfalse;
	}
	if ( SV_OAXInfraCalls( args, ret ) || SV_OAXEntCalls( args, ret ) ) {
		return qtrue;
	}
	for ( i = 0; i < numGameHandlers; i++ ) {
		if ( gameHandlers[i]( args, ret ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

/*
=================
SV_OAXInit

Called once from SV_Init. Each feature block registers its handler (and
advertises its oax_features token) here, one line per feature.
=================
*/
void SV_OAXInit( void ) {
	SV_OAXScriptInit();		// "script": id Tech 4 map scripting (sv_script_oax.c)
	OAX_AddFeature( "movers" );	// keyframed and spline movers (game and cgame only; trajectory types 32-35)
	OAX_AddFeature( "portal" );	// area portals: snd_occlusion, r_surfs_world (client/snd_occlusion.c)
	// zone volumes and seamless warps are QVM features (bg_oax_zone.c,
	// g_oax_warp.c); the tokens tell maps and tools they are supported
	OAX_AddFeature( "zones" );
	OAX_AddFeature( "warp" );
	SV_OAXGuiInit();
	// unified lighting: the game spawns ET_OAX_LIGHT entities for the lights
	// it controls (no game syscalls; the cgame drives the renderer)
	OAX_AddFeature( "ulight" );
	// Box3D worlds for the game (code/physics, block 1200-1249)
	Phys_Init();
	SV_OAXRegisterGameHandler( Phys_GameCalls );
	OAX_AddFeature( "physics" );
	OAX_AddFeature( "physics_vehicle" );
	OAX_AddFeature( "physics_vehicle_state" );	// PHYS_VEHICLE_SET_STATE (own-vehicle prediction)
	OAX_AddFeature( "ent_obb" );	// oriented entity boxes (G_OAX_ENT_SET_OBB, vehicles)
	SV_OAXNavInit();		// "nav": Recast/Detour navmesh for bots (sv_nav_oax.c)
}
