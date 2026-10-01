/*
===========================================================================
wasmcart platform backend: the server half of the debug state.

client.h and server.h cannot share a translation unit (both pull in the
game's public headers), so wc_debug.c asks for the server's view here.
===========================================================================
*/

#include "../server/server.h"

/*
===============
WC_Debug_ServerPlayerState

Client 0's authoritative player state when a local server is running a
game, else NULL.
===============
*/
const playerState_t *WC_Debug_ServerPlayerState( int *serverTime, int *activeEntities ) {
	int i, active = 0;

	if ( !com_sv_running || !com_sv_running->integer || sv.state != SS_GAME ||
		!svs.clients || svs.clients[0].state != CS_ACTIVE ) {
		return NULL;
	}

	for ( i = 0; i < sv.num_entities; i++ ) {
		if ( SV_GentityNum( i )->r.linked ) {
			active++;
		}
	}
	*serverTime = sv.time;
	*activeEntities = active;
	return SV_GameClientNum( 0 );
}

int WC_Debug_ServerStaticTime( void ) {
	if ( !com_sv_running || !com_sv_running->integer ) {
		return -1;
	}
	return svs.time;
}
