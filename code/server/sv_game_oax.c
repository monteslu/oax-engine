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
=================
SV_GameSystemCallsOAX
=================
*/
qboolean SV_GameSystemCallsOAX( intptr_t *args, intptr_t *ret ) {
	int i;

	if ( args[0] < 1000 || args[0] >= G_OAX_END ) {
		return qfalse;
	}
	if ( SV_OAXInfraCalls( args, ret ) ) {
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
}
