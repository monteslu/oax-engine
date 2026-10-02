/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company. 

This file is part of the Doom 3 GPL Source Code (?Doom 3 Source Code?).  

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================

Adapted from DOOM-3 neo/game/gamesys/Event.cpp: idEventDef's constructor
checks (format characters, argument count, unique names) and FindEvent.
Changed: definitions are registered at run time into a fixed table
instead of being static objects; there is no idEvent queue.

===========================================================================
*/

#include "Script_Local.h"

idEventDef	idEventDef::eventDefList[ MAX_EVENTS ];
int			idEventDef::numEventDefs = 0;

/*
================
idEventDef::Clear
================
*/
void idEventDef::Clear( void ) {
	numEventDefs = 0;
}

/*
================
idEventDef::Register

oax: D3's idEventDef::idEventDef, run when the engine or the game module
registers an event.
================
*/
int idEventDef::Register( const char *command, const char *formatspec, char returnType, int flags, int engineId ) {
	idEventDef	*ev;
	int			i;
	int			argsize;

	if ( !command || !command[ 0 ] || strlen( command ) >= sizeof( ev->name ) ) {
		gameLocal.Warning( "idEventDef: bad event name" );
		return -1;
	}
	if ( !formatspec ) {
		formatspec = "";
	}
	if ( strlen( formatspec ) > D_EVENT_MAXARGS ) {
		gameLocal.Warning( "idEventDef: too many args for '%s' event", command );
		return -1;
	}
	if ( FindEvent( command ) ) {
		gameLocal.Warning( "idEventDef: event '%s' is already defined", command );
		return -1;
	}
	if ( numEventDefs >= MAX_EVENTS ) {
		gameLocal.Warning( "idEventDef: exceeded MAX_EVENTS" );
		return -1;
	}
	switch ( returnType ) {
	case D_EVENT_VOID: case D_EVENT_INTEGER: case D_EVENT_FLOAT: case D_EVENT_VECTOR:
	case D_EVENT_STRING: case D_EVENT_ENTITY: case D_EVENT_ENTITY_NULL:
		break;
	default:
		gameLocal.Warning( "idEventDef: invalid return type '%c' for '%s' event", returnType, command );
		return -1;
	}

	ev = &eventDefList[ numEventDefs ];
	memset( ev, 0, sizeof( *ev ) );
	idStr::Copynz( ev->name, command, sizeof( ev->name ) );
	idStr::Copynz( ev->formatspec, formatspec, sizeof( ev->formatspec ) );
	ev->returnType = returnType;
	ev->numargs = strlen( formatspec );
	ev->flags = flags;
	ev->engineId = engineId;

	// calculate the offsets for each arg (as D3 did, for an int data[] array)
	argsize = 0;
	for( i = 0; i < ev->numargs; i++ ) {
		ev->argOffset[ i ] = argsize;
		switch( formatspec[ i ] ) {
		case D_EVENT_FLOAT :
		case D_EVENT_INTEGER :
		case D_EVENT_VECTOR :
		case D_EVENT_STRING :
		case D_EVENT_ENTITY :
		case D_EVENT_ENTITY_NULL :
			argsize += sizeof( int );
			break;

		default :
			gameLocal.Warning( "idEventDef: invalid arg format '%s' string for '%s' event.", formatspec, command );
			return -1;
		}
	}
	ev->argsize = argsize;
	ev->eventnum = numEventDefs++;
	return ev->eventnum;
}

/*
================
idEventDef::GetEventCommand
================
*/
const idEventDef *idEventDef::GetEventCommand( int eventnum ) {
	if ( eventnum < 0 || eventnum >= numEventDefs ) {
		return NULL;
	}
	return &eventDefList[ eventnum ];
}

/*
================
idEventDef::FindEvent
================
*/
const idEventDef *idEventDef::FindEvent( const char *name ) {
	int i;

	for( i = 0; i < numEventDefs; i++ ) {
		if ( !strcmp( name, eventDefList[ i ].name ) ) {
			return &eventDefList[ i ];
		}
	}

	return NULL;
}
