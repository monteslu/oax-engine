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
sv_gui_oax.c: in-world GUI syscalls for the game module (G_OAX_GUI_*,
1040-1059, see qcommon/oax.h) and the oax_features token "gui".

The server owns every GUI's state. The game QVM loads a headless GUI per
func_oax_gui, traces the player's eye with G_OAX_GUI_TRACE, and feeds
focus, cursor and button changes through G_OAX_GUI_HANDLE_EVENT, which
returns the commands the GUI issued plus `set "gui::<key>" "<value>"` for
every state variable the event changed. Everything here is a function of
the QVM's arguments, so native and wasm servers agree exactly.
===========================================================================
*/

#include "server.h"
#include "../qcommon/oax.h"
#include "../idgui/idgui_public.h"

// copy an event's command string into the QVM's buffer; returns its length
static int SV_GuiReturn( const char *cmds, intptr_t vmBuf, int size ) {
	int len = (int)strlen( cmds );

	if ( size <= 0 ) {
		return len;
	}
	VM_CheckBlock( vmBuf, size, "GUICMDS" );
	Q_strncpyz( VM_ArgPtr( vmBuf ), cmds, size );
	return len;
}

static qboolean SV_OAXGuiCalls( intptr_t *args, intptr_t *ret ) {
	switch ( args[0] ) {
	case G_OAX_GUI_LOAD:
		*ret = IDGUI_Load( IDGUI_SERVER, VMA( 1 ) );
		return qtrue;

	case G_OAX_GUI_FREE:
		IDGUI_Free( IDGUI_SERVER, args[1] );
		*ret = 0;
		return qtrue;

	case G_OAX_GUI_SETSTATE:
		IDGUI_SetState( IDGUI_SERVER, args[1], VMA( 2 ), VMA( 3 ) );
		IDGUI_StateChanged( IDGUI_SERVER, args[1], sv.time );
		*ret = 0;
		return qtrue;

	case G_OAX_GUI_GETSTATE:
		if ( args[4] > 0 ) {
			VM_CheckBlock( args[3], args[4], "GUIGETSTATE" );
		}
		*ret = IDGUI_GetState( IDGUI_SERVER, args[1], VMA( 2 ), args[4] > 0 ? VMA( 3 ) : NULL, args[4] );
		return qtrue;

	case G_OAX_GUI_HANDLE_EVENT:
		// ( handle, x, y, buttons, time, cmds, size )
		*ret = SV_GuiReturn( IDGUI_MouseEvent( IDGUI_SERVER, args[1], VMF( 2 ), VMF( 3 ), args[4], args[5] ), args[6], args[7] );
		return qtrue;

	case G_OAX_GUI_TRACE: {
		// ( entnum, start, end, float xyf[3] ) -> hit; xyf = x, y, fraction
		sharedEntity_t *ent;
		float xy[3];
		vec3_t start, end;

		*ret = 0;
		if ( args[1] < 0 || args[1] >= sv.num_entities ) {
			return qtrue;
		}
		VM_CheckBlock( args[4], sizeof( xy ), "GUITRACE" );
		ent = SV_GentityNum( args[1] );
		if ( !ent->r.bmodel ) {
			return qtrue;
		}
		VectorCopy( (const float *)VMA( 2 ), start );
		VectorCopy( (const float *)VMA( 3 ), end );
		if ( CM_GuiTrace( ent->s.modelindex, ent->r.currentOrigin, ent->r.currentAngles, start, end, &xy[0], &xy[1], &xy[2] ) ) {
			Com_Memcpy( VMA( 4 ), xy, sizeof( xy ) );
			*ret = 1;
		}
		return qtrue;
	}

	case G_OAX_GUI_ACTIVATE:
		// ( handle, activate, time, cmds, size )
		*ret = SV_GuiReturn( IDGUI_Activate( IDGUI_SERVER, args[1], args[2], args[3] ), args[4], args[5] );
		return qtrue;

	case G_OAX_GUI_NAMED_EVENT:
		// ( handle, name, time, cmds, size )
		*ret = SV_GuiReturn( IDGUI_NamedEvent( IDGUI_SERVER, args[1], VMA( 2 ), args[3] ), args[4], args[5] );
		return qtrue;

	case G_OAX_GUI_STATE_INFO:
		// ( handle, buf, size ) -> length of the full state info string
		if ( args[3] > 0 ) {
			VM_CheckBlock( args[2], args[3], "GUISTATEINFO" );
		}
		*ret = IDGUI_StateInfo( IDGUI_SERVER, args[1], args[3] > 0 ? VMA( 2 ) : NULL, args[3] );
		return qtrue;
	}
	return qfalse;
}

/*
=================
SV_OAXGuiReset

Every game VM start (map load or restart) begins with no server GUIs.
=================
*/
void SV_OAXGuiReset( void ) {
	IDGUI_FreeAll( IDGUI_SERVER );
}

static void SV_GuiPrint( const char *msg ) {
	Com_Printf( "%s", msg );
}

static void SV_GuiWarning( const char *msg ) {
	Com_Printf( S_COLOR_YELLOW "%s\n", msg );
}

static int SV_GuiReadFile( const char *path, void **buffer ) {
	return FS_ReadFile( path, buffer );
}

static void SV_GuiFreeFile( void *buffer ) {
	FS_FreeFile( buffer );
}

void SV_OAXGuiInit( void ) {
	idguiImport_t imp;

	Com_Memset( &imp, 0, sizeof( imp ) );
	imp.Print = SV_GuiPrint;
	imp.Warning = SV_GuiWarning;
	imp.ReadFile = SV_GuiReadFile;
	imp.FreeFile = SV_GuiFreeFile;
	IDGUI_Init( &imp );

	SV_OAXRegisterGameHandler( SV_OAXGuiCalls );
	OAX_AddFeature( "gui" );
}
