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
