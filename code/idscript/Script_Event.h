/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

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

Adapted from DOOM-3 neo/game/gamesys/Event.h: the idEventDef part only
(name, argument format, return type, argument sizes). Changed: event
definitions are not static C++ objects bound to idClass methods. They are
a table filled at run time: the engine registers the thread events it
executes itself (wait, print, sin, ...) and the game module (QVM)
registers the entity and system events it executes, through the
G_OAX_SCRIPT_REGISTER_EVENT syscall. The idEvent queue is gone; thread
scheduling lives in Script_Thread.cpp.

===========================================================================
*/

#ifndef __SCRIPT_EVENT_H__
#define __SCRIPT_EVENT_H__

#define D_EVENT_MAXARGS				8

#define D_EVENT_VOID				( ( char )0 )
#define D_EVENT_INTEGER				'd'
#define D_EVENT_FLOAT				'f'
#define D_EVENT_VECTOR				'v'
#define D_EVENT_STRING				's'
#define D_EVENT_ENTITY				'e'
#define	D_EVENT_ENTITY_NULL			'E'			// event can handle NULL entity pointers
#define D_EVENT_TRACE				't'

#define MAX_EVENTS					512

// who executes an event, and how scripts may call it
#define EVENT_ENGINE_THREAD			1			// executed by idThread in the engine (sys.wait(), sys.print(), ...)
#define EVENT_QVM_ENTITY			2			// executed by the game module, called on an entity: $door.moveTo(...)
#define EVENT_QVM_SYS				4			// executed by the game module, called as sys.name(...)

class idEventDef {
private:
	char						name[ 64 ];
	char						formatspec[ D_EVENT_MAXARGS + 1 ];
	int							returnType;
	int							numargs;
	int							argsize;
	int							argOffset[ D_EVENT_MAXARGS ];
	int							eventnum;
	int							flags;
	int							engineId;		// EVENT_ENGINE_THREAD: which built-in

	static idEventDef			eventDefList[ MAX_EVENTS ];
	static int					numEventDefs;

public:
	const char					*GetName( void ) const { return name; }
	const char					*GetArgFormat( void ) const { return formatspec; }
	char						GetReturnType( void ) const { return (char)returnType; }
	int							GetEventNum( void ) const { return eventnum; }
	int							GetNumArgs( void ) const { return numargs; }
	int							GetArgSize( void ) const { return argsize; }
	int							GetArgOffset( int arg ) const { return argOffset[ arg ]; }
	int							GetFlags( void ) const { return flags; }
	int							GetEngineId( void ) const { return engineId; }
	bool						IsSysCallable( void ) const { return ( flags & ( EVENT_ENGINE_THREAD | EVENT_QVM_SYS ) ) != 0; }
	bool						IsEngineEvent( void ) const { return ( flags & EVENT_ENGINE_THREAD ) != 0; }

	// returns the event number, or -1 with a message when the definition is invalid
	static int					Register( const char *name, const char *format, char returnType, int flags, int engineId );
	static void					Clear( void );
	static int					NumEventCommands( void ) { return numEventDefs; }
	static const idEventDef		*GetEventCommand( int eventnum );
	static const idEventDef		*FindEvent( const char *name );
};

#endif /* !__SCRIPT_EVENT_H__ */
