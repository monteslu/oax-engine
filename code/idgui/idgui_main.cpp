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
idgui_main.cpp: the C interface of the GUI module (idgui_public.h).

Written for oa-engine (GPLv3, as the DOOM-3 code it drives). The module's
classes are DOOM-3's (neo/ui); this file is the shared instance factory
that the server (headless: events, transitions, gui scripts, state) and the
client (drawing) both use, plus the error guard and the material table.

Server-side determinism: everything an event changes is computed from the
event's integer cursor position, its time and the GUI file, through float
math that is the same on every IEEE target (no host libm: sine and cosine
are the engine's musl ports, and the module builds with -ffp-contract=off).
===========================================================================
*/

#include "idgui_precompiled.h"

namespace idgui {

extern idUserInterfaceManagerLocal uiManagerLocal;

// referenced by idWindow::Redraw (DOOM-3 renderer cvar); always 0 here
idCVar r_skipGuiShaders( "r_skipGuiShaders", "0", CVAR_GUI | CVAR_INTEGER, "" );

static idCVarSystemLite	cvarSystemLocal;
idCVarSystemLite *		cvarSystem = &cvarSystemLocal;
static idDeclManagerLite	declManagerLocal;
idDeclManagerLite *		declManager = &declManagerLocal;

/*
===============================================================================
	materials: Q3 shader names, registered by the client on first draw
===============================================================================
*/

int idMaterial::Handle( void ) const {
	if ( !registered ) {
		registered = true;
		handle = gImport.RegisterShader ? gImport.RegisterShader( name.c_str() ) : 0;
	}
	return handle;
}

const idMaterial *idDeclManagerLite::FindMaterial( const char *name, bool makeDefault ) {
	idStr n = name;
	// DOOM-3 names images with extensions; Q3 shaders are found without one
	if ( n.Length() > 4 && ( !n.Right( 4 ).Icmp( ".tga" ) || !n.Right( 4 ).Icmp( ".jpg" ) ) ) {
		n.StripFileExtension();
	}
	for ( int i = 0; i < materials.Num(); i++ ) {
		if ( !idStr::Icmp( materials[i]->GetName(), n.c_str() ) ) {
			return materials[i];
		}
	}
	idMaterial *m = new idMaterial( n.c_str() );
	materials.Append( m );
	return m;
}

void idDeclManagerLite::PurgeMaterials( void ) {
	for ( int i = 0; i < materials.Num(); i++ ) {
		materials[i]->Purge();
	}
}

void idDeclManagerLite::FreeMaterials( void ) {
	materials.DeleteContents( true );
}

/*
===============================================================================
	instances
===============================================================================
*/

struct guiInstance_t {
	idUserInterfaceLocal *	ui;
	int						buttons;		// last mouse buttons seen (click edges)
	bool					haveCursor;		// a mouse position has been sent
	int						cursorX, cursorY;
};

static guiInstance_t	instances[2][IDGUI_MAX_GUIS];
static bool				initialized;
static idStr			result;			// the last event's command string

static guiInstance_t *Instance( int table, int handle ) {
	if ( table < 0 || table > 1 || handle < 1 || handle > IDGUI_MAX_GUIS ) {
		return NULL;
	}
	guiInstance_t *g = &instances[table][handle - 1];
	return g->ui ? g : NULL;
}

// "gui::name" and "name" are the same state variable
static const char *StateKey( const char *key ) {
	if ( !idStr::Icmpn( key, VAR_GUIPREFIX, VAR_GUIPREFIX_LEN ) ) {
		return key + VAR_GUIPREFIX_LEN;
	}
	return key;
}

// the window rectangles are computed while drawing; a headless GUI
// computes them before it hit-tests an event
static void PrepareForEvents( idUserInterfaceLocal *ui ) {
	if ( ui->GetDesktop() ) {
		ui->GetDesktop()->CalcRects( 0, 0 );
	}
}

static void AppendCommand( idStr &out, const char *cmd ) {
	if ( !cmd || !cmd[0] ) {
		return;
	}
	if ( out.Length() ) {
		out += " ; ";
	}
	out += cmd;
}

// `set "gui::<key>" "<value>"` for every state variable that differs from before
static void AppendStateChanges( idStr &out, const idDict &before, const idDict &after ) {
	for ( int i = 0; i < after.GetNumKeyVals(); i++ ) {
		const idKeyValue *kv = after.GetKeyVal( i );
		const idKeyValue *old = before.FindKey( kv->GetKey() );
		if ( old && !old->GetValue().Cmp( kv->GetValue() ) ) {
			continue;
		}
		AppendCommand( out, va( "set \"%s%s\" \"%s\"", VAR_GUIPREFIX, kv->GetKey().c_str(), kv->GetValue().c_str() ) );
	}
}

static void SendEvent( guiInstance_t *g, sysEventType_t type, int value, int value2, int time, idStr &cmds ) {
	sysEvent_t ev;
	memset( &ev, 0, sizeof( ev ) );
	ev.evType = type;
	ev.evValue = value;
	ev.evValue2 = value2;
	AppendCommand( cmds, g->ui->HandleEvent( &ev, time, NULL ) );
}

} // namespace idgui

using namespace idgui;

#define GUARD( failValue )							\
	idLibGuard guard;								\
	if ( setjmp( guard.jb ) ) {						\
		idLib_PopGuard( &guard );					\
		return failValue;							\
	}												\
	idLib_PushGuard( &guard )

#define UNGUARD()	idLib_PopGuard( &guard )

extern "C" {

void IDGUI_Init( const idguiImport_t *imp ) {
	// each side fills the imports it owns; NULL fields keep what is there
	const void **src = (const void **)imp;
	void **dst = (void **)&gImport;
	for ( size_t i = 0; i < sizeof( idguiImport_t ) / sizeof( void * ); i++ ) {
		if ( src[i] ) {
			dst[i] = (void *)src[i];
		}
	}
	if ( !initialized ) {
		GUARD();
		uiManagerLocal.Init();
		initialized = true;
		UNGUARD();
	}
}

void IDGUI_PurgeShaders( void ) {
	declManager->PurgeMaterials();
}

void IDGUI_Free( int table, int handle ) {
	guiInstance_t *g = Instance( table, handle );
	if ( !g ) {
		return;
	}
	GUARD();
	uiManagerLocal.DeAlloc( g->ui );
	UNGUARD();
	memset( g, 0, sizeof( *g ) );
}

void IDGUI_FreeAll( int table ) {
	for ( int i = 1; i <= IDGUI_MAX_GUIS; i++ ) {
		IDGUI_Free( table, i );
	}
}

int IDGUI_Load( int table, const char *path ) {
	if ( !initialized || table < 0 || table > 1 || !path || !path[0] ) {
		return 0;
	}
	int slot;
	for ( slot = 0; slot < IDGUI_MAX_GUIS; slot++ ) {
		if ( !instances[table][slot].ui ) {
			break;
		}
	}
	if ( slot == IDGUI_MAX_GUIS ) {
		common->Warning( "IDGUI_Load: too many GUIs (%d)", IDGUI_MAX_GUIS );
		return 0;
	}
	if ( fileSystem->ReadFile( path, NULL ) < 0 ) {
		common->Warning( "couldn't load gui '%s'", path );
		return 0;
	}

	idUserInterfaceLocal *ui = new idUserInterfaceLocal();
	{
		GUARD( 0 );
		// every instance is unique: world GUIs keep their own state
		ui->InitFromFile( path );
		ui->SetUniqued( true );
		UNGUARD();
	}
	guiInstance_t *g = &instances[table][slot];
	memset( g, 0, sizeof( *g ) );
	g->ui = ui;
	return slot + 1;
}

int IDGUI_IsInteractive( int table, int handle ) {
	guiInstance_t *g = Instance( table, handle );
	return g ? g->ui->IsInteractive() : 0;
}

void IDGUI_SetState( int table, int handle, const char *key, const char *value ) {
	guiInstance_t *g = Instance( table, handle );
	if ( !g || !key || !key[0] ) {
		return;
	}
	g->ui->SetStateString( StateKey( key ), value ? value : "" );
}

int IDGUI_GetState( int table, int handle, const char *key, char *buf, int size ) {
	guiInstance_t *g = Instance( table, handle );
	if ( size > 0 ) {
		buf[0] = '\0';
	}
	if ( !g || !key ) {
		return 0;
	}
	const idKeyValue *kv = g->ui->State().FindKey( StateKey( key ) );
	if ( !kv ) {
		return 0;
	}
	if ( size > 0 ) {
		idStr::Copynz( buf, kv->GetValue().c_str(), size );
	}
	return 1;
}

int IDGUI_StateInfo( int table, int handle, char *buf, int size ) {
	guiInstance_t *g = Instance( table, handle );
	idStr info;
	if ( size > 0 ) {
		buf[0] = '\0';
	}
	if ( !g ) {
		return 0;
	}
	const idDict &state = g->ui->State();
	for ( int i = 0; i < state.GetNumKeyVals(); i++ ) {
		const idKeyValue *kv = state.GetKeyVal( i );
		info += "\\";
		info += kv->GetKey();
		info += "\\";
		info += kv->GetValue();
	}
	if ( size > 0 ) {
		idStr::Copynz( buf, info.c_str(), size );
	}
	return info.Length();
}

void IDGUI_StateChanged( int table, int handle, int time ) {
	guiInstance_t *g = Instance( table, handle );
	if ( !g ) {
		return;
	}
	GUARD();
	g->ui->StateChanged( time, false );
	UNGUARD();
}

const char *IDGUI_MouseEvent( int table, int handle, float x, float y, int buttons, int time ) {
	guiInstance_t *g = Instance( table, handle );
	result = "";
	if ( !g ) {
		return "";
	}
	GUARD( "" );
	idDict before;
	before.Copy( g->ui->State() );
	idStr cmds;

	PrepareForEvents( g->ui );

	// DOOM-3's idPlayer::UpdateFocus: park the cursor in the corner, then
	// move it to the absolute position (whole virtual pixels, as SE_MOUSE)
	int cx = (int)( x * IDGUI_VIRTUAL_WIDTH );
	int cy = (int)( y * IDGUI_VIRTUAL_HEIGHT );
	if ( !g->haveCursor || cx != g->cursorX || cy != g->cursorY ) {
		SendEvent( g, SE_MOUSE, -2000, -2000, time, cmds );
		SendEvent( g, SE_MOUSE, cx, cy, time, cmds );
		g->haveCursor = true;
		g->cursorX = cx;
		g->cursorY = cy;
	}
	if ( ( buttons & IDGUI_BUTTON1 ) != ( g->buttons & IDGUI_BUTTON1 ) ) {
		SendEvent( g, SE_KEY, K_MOUSE1, ( buttons & IDGUI_BUTTON1 ) ? 1 : 0, time, cmds );
	}
	g->buttons = buttons;

	if ( table == IDGUI_SERVER ) {
		AppendStateChanges( cmds, before, g->ui->State() );
	}
	result = cmds;
	UNGUARD();
	return result.c_str();
}

const char *IDGUI_Activate( int table, int handle, int activate, int time ) {
	guiInstance_t *g = Instance( table, handle );
	result = "";
	if ( !g ) {
		return "";
	}
	GUARD( "" );
	idDict before;
	before.Copy( g->ui->State() );
	idStr cmds;
	PrepareForEvents( g->ui );
	AppendCommand( cmds, g->ui->Activate( activate != 0, time ) );
	if ( !activate ) {
		// leaving: the next focus starts from a parked cursor and released button
		g->haveCursor = false;
		if ( g->buttons & IDGUI_BUTTON1 ) {
			SendEvent( g, SE_KEY, K_MOUSE1, 0, time, cmds );
		}
		g->buttons = 0;
		SendEvent( g, SE_MOUSE, -2000, -2000, time, cmds );
	}
	if ( table == IDGUI_SERVER ) {
		AppendStateChanges( cmds, before, g->ui->State() );
	}
	result = cmds;
	UNGUARD();
	return result.c_str();
}

const char *IDGUI_NamedEvent( int table, int handle, const char *name, int time ) {
	guiInstance_t *g = Instance( table, handle );
	result = "";
	if ( !g || !name ) {
		return "";
	}
	GUARD( "" );
	idDict before;
	before.Copy( g->ui->State() );
	idStr cmds;
	g->ui->SetTime( time );
	g->ui->HandleNamedEvent( name );
	// the desktop hands back the commands the event added
	SendEvent( g, SE_NONE, 0, 0, time, cmds );
	if ( table == IDGUI_SERVER ) {
		AppendStateChanges( cmds, before, g->ui->State() );
	}
	result = cmds;
	UNGUARD();
	return result.c_str();
}

void IDGUI_Redraw( int table, int handle, int time, int width, int height ) {
	guiInstance_t *g = Instance( table, handle );
	if ( !g || !gImport.DrawStretchPic ) {
		return;
	}
	GUARD();
	uiManagerLocal.DC().SetTargetSize( (float)width, (float)height );
	g->ui->Redraw( time );
	UNGUARD();
}

} // extern "C"
