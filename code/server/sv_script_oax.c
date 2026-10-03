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
sv_script_oax.c: the game syscalls of the map scripting block
(G_OAX_SCRIPT_*, 1010-1039 in qcommon/oax.h) and the script VM's cvars
and console commands. The VM itself is idscript/ (C++, id Tech 4).

  script_maxInstructions  instructions one thread may run per frame before
                          it is killed as a runaway (default 100000)
  g_debugScript, g_disasm D3's script debug switches
  scriptthreads           list the live threads
  scriptlog [file]        the event log (time event self thread rows), to
                          the console or a file in the home directory

New code for the oax engine (GPLv3, like the id Tech 4 code it drives).
===========================================================================
*/

#include "server.h"
#include "../qcommon/oax.h"
#include "../idscript/oax_script.h"

static cvar_t *script_maxInstructions;
static cvar_t *g_debugScript;
static cvar_t *g_disasm;

static void SV_OAXScriptSettings( void ) {
	OAXScript_SetMaxInstructions( script_maxInstructions->integer );
	OAXScript_SetDebug( g_debugScript->integer, g_disasm->integer, com_developer ? com_developer->integer : 0 );
}

static qboolean SV_OAXScriptCalls( intptr_t *args, intptr_t *ret ) {
	switch ( args[0] ) {
	case G_OAX_SCRIPT_INIT:
		*ret = OAXScript_Init( args[1] );
		SV_OAXScriptSettings();
		return qtrue;
	case G_OAX_SCRIPT_REGISTER_EVENT:
		*ret = OAXScript_RegisterEvent( VMA( 1 ), VMA( 2 ), args[3], args[4] );
		return qtrue;
	case G_OAX_SCRIPT_COMPILE_FILE:
		SV_OAXScriptSettings();
		*ret = OAXScript_CompileFile( VMA( 1 ) );
		return qtrue;
	case G_OAX_SCRIPT_SET_ENTITY:
		*ret = OAXScript_SetEntity( VMA( 1 ), args[2] );
		return qtrue;
	case G_OAX_SCRIPT_START_THREAD:
		*ret = OAXScript_StartThread( VMA( 1 ), args[2] );
		return qtrue;
	case G_OAX_SCRIPT_RUN:
		VM_CheckBlock( args[2], sizeof( oaxScriptCall_t ), "SCRIPTRUN" );
		SV_OAXScriptSettings();
		*ret = OAXScript_Run( args[1], VMA( 2 ) );
		return qtrue;
	case G_OAX_SCRIPT_RETURN:
		if ( args[1] ) {
			VM_CheckBlock( args[1], sizeof( oaxScriptValue_t ), "SCRIPTRET" );
		}
		OAXScript_Return( args[1] ? VMA( 1 ) : NULL, args[2] ? VMA( 2 ) : NULL );
		*ret = 0;
		return qtrue;
	case G_OAX_SCRIPT_OBJECT_DONE:
		OAXScript_ObjectDone( args[1], args[2] );
		*ret = 0;
		return qtrue;
	case G_OAX_SCRIPT_KILL_THREAD:
		OAXScript_KillThread( args[1] );
		*ret = 0;
		return qtrue;
	case G_OAX_SCRIPT_SHUTDOWN:
		OAXScript_Shutdown();
		*ret = 0;
		return qtrue;
	case G_OAX_SCRIPT_NUM_THREADS:
		*ret = OAXScript_NumThreads();
		return qtrue;
	}
	return qfalse;
}

static void SV_ScriptThreads_f( void ) {
	OAXScript_ListThreads();
}

static void SV_ScriptLog_f( void ) {
	static char text[64 * 1024];
	int len = OAXScript_LogText( text, sizeof( text ) );

	if ( Cmd_Argc() > 1 ) {
		fileHandle_t f = FS_FOpenFileWrite_HomeData( Cmd_Argv( 1 ) );
		if ( !f ) {
			Com_Printf( "scriptlog: couldn't write %s\n", Cmd_Argv( 1 ) );
			return;
		}
		FS_Write( text, len, f );
		FS_FCloseFile( f );
		return;
	}
	Com_Printf( "%s", text );
}

/*
=================
SV_OAXScriptInit

From SV_OAXInit: the "script" feature.
=================
*/
void SV_OAXScriptInit( void ) {
	script_maxInstructions = Cvar_Get( "script_maxInstructions", "100000", CVAR_CHEAT );
	Cvar_SetDescription( script_maxInstructions, "Script instructions one thread may run per frame before it is killed as a runaway." );
	g_debugScript = Cvar_Get( "g_debugScript", "0", CVAR_CHEAT );
	g_disasm = Cvar_Get( "g_disasm", "0", CVAR_CHEAT );
	Cmd_AddCommand( "scriptthreads", SV_ScriptThreads_f );
	Cmd_AddCommand( "scriptlog", SV_ScriptLog_f );
	SV_OAXRegisterGameHandler( SV_OAXScriptCalls );
	OAX_AddFeature( "script" );
}
