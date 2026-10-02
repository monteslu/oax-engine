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

Adapted from DOOM-3 neo/game/script/Script_Thread.cpp. Changed (see
Script_Thread.h): no idClass, the engine's thread events dispatch through
ProcessEngineEvent, scheduling is a per-thread (time, post order) slot run
by the pump instead of the idEvent queue, game events are handed to the
game module, threads are deleted by the pump after they end, and a thread
that runs too many instructions in one frame is killed and reported.

===========================================================================
*/

#include "Script_Local.h"

idThread			*idThread::currentThread = NULL;
int					idThread::threadIndex = 0;
int					idThread::scheduleCounter = 0;
idList<idThread *>	idThread::threadList;

// oax: the one call that can be out to the game module at a time
static oaxScriptCall_t	gameCall;
static int				gameCallKind;
static scriptEntity_t	gameCallSelf;

typedef struct {
	int			id;
	const char	*name;
	const char	*format;
	char		returnType;
} engineEvent_t;

// the events the engine runs itself; the script declares them as
// scriptEvents in script/oax_events.script (D3: EV_Thread_* in this file)
static const engineEvent_t engineEvents[] = {
	{ TE_TERMINATE,			"terminate",		"d",	0 },
	{ TE_PAUSE,				"pause",			"",		0 },
	{ TE_WAIT,				"wait",				"f",	0 },
	{ TE_WAITFRAME,			"waitFrame",		"",		0 },
	{ TE_WAITFOR,			"waitFor",			"e",	0 },
	{ TE_WAITFORTHREAD,		"waitForThread",	"d",	0 },
	{ TE_PRINT,				"print",			"s",	0 },
	{ TE_PRINTLN,			"println",			"s",	0 },
	{ TE_ASSERT,			"assert",			"f",	0 },
	{ TE_RANDOM,			"random",			"f",	'f' },
	{ TE_GETTIME,			"getTime",			"",		'f' },
	{ TE_KILLTHREAD,		"killthread",		"s",	0 },
	{ TE_THREADNAME,		"threadname",		"s",	0 },
	{ TE_ANGTOFORWARD,		"angToForward",		"v",	'v' },
	{ TE_ANGTORIGHT,		"angToRight",		"v",	'v' },
	{ TE_ANGTOUP,			"angToUp",			"v",	'v' },
	{ TE_SIN,				"sin",				"f",	'f' },
	{ TE_COS,				"cos",				"f",	'f' },
	{ TE_SQRT,				"sqrt",				"f",	'f' },
	{ TE_VECNORMALIZE,		"vecNormalize",		"v",	'v' },
	{ TE_VECLENGTH,			"vecLength",		"v",	'f' },
	{ TE_DOTPRODUCT,		"DotProduct",		"vv",	'f' },
	{ TE_CROSSPRODUCT,		"CrossProduct",		"vv",	'v' },
	{ TE_VECTOANGLES,		"VecToAngles",		"v",	'v' },
	{ TE_WARNING,			"warning",			"s",	0 },
	{ TE_ERROR,				"error",			"s",	0 },
	{ TE_STRLEN,			"strLength",		"s",	'd' },
	{ TE_STRLEFT,			"strLeft",			"sd",	's' },
	{ TE_STRRIGHT,			"strRight",			"sd",	's' },
	{ TE_STRSKIP,			"strSkip",			"sd",	's' },
	{ TE_STRMID,			"strMid",			"sdd",	's' },
	{ TE_STRTOFLOAT,		"strToFloat",		"s",	'f' },
	{ TE_ISCLIENT,			"isClient",			"",		'f' },
	{ TE_ISMULTIPLAYER,		"isMultiplayer",	"",		'f' },
	{ TE_GETFRAMETIME,		"getFrameTime",		"",		'f' },
	{ TE_GETTICSPERSECOND,	"getTicsPerSecond",	"",		'f' },
	{ 0, NULL, NULL, 0 }
};

/*
================
idThread::RegisterEngineEvents
================
*/
void idThread::RegisterEngineEvents( void ) {
	const engineEvent_t *e;

	for ( e = engineEvents; e->name; e++ ) {
		idEventDef::Register( e->name, e->format, e->returnType, EVENT_ENGINE_THREAD, e->id );
	}
}

/*
================
idThread::ProcessEngineEvent

oax: D3 dispatched through idClass::ProcessEventArgPtr.
================
*/
void idThread::ProcessEngineEvent( const idEventDef *ev, intptr_t *data ) {
	switch ( ev->GetEngineId() ) {
	case TE_TERMINATE:			Event_TerminateThread( (int)data[0] ); break;
	case TE_PAUSE:				Event_Pause(); break;
	case TE_WAIT:				Event_Wait( *(float *)&data[0] ); break;
	case TE_WAITFRAME:			Event_WaitFrame(); break;
	case TE_WAITFOR:			Event_WaitFor( (scriptEntity_t)data[0] ); break;
	case TE_WAITFORTHREAD:		Event_WaitForThread( (int)data[0] ); break;
	case TE_PRINT:				Event_Print( (const char *)data[0] ); break;
	case TE_PRINTLN:			Event_PrintLn( (const char *)data[0] ); break;
	case TE_ASSERT:				Event_Assert( *(float *)&data[0] ); break;
	case TE_RANDOM:				Event_Random( *(float *)&data[0] ); break;
	case TE_GETTIME:			Event_GetTime(); break;
	case TE_KILLTHREAD:			Event_KillThread( (const char *)data[0] ); break;
	case TE_THREADNAME:			Event_SetThreadName( (const char *)data[0] ); break;
	case TE_ANGTOFORWARD:		{ idAngles a( *(idVec3 *)data[0] ); Event_AngToForward( a ); } break;
	case TE_ANGTORIGHT:			{ idAngles a( *(idVec3 *)data[0] ); Event_AngToRight( a ); } break;
	case TE_ANGTOUP:			{ idAngles a( *(idVec3 *)data[0] ); Event_AngToUp( a ); } break;
	case TE_SIN:				Event_GetSine( *(float *)&data[0] ); break;
	case TE_COS:				Event_GetCosine( *(float *)&data[0] ); break;
	case TE_SQRT:				Event_GetSquareRoot( *(float *)&data[0] ); break;
	case TE_VECNORMALIZE:		Event_VecNormalize( *(idVec3 *)data[0] ); break;
	case TE_VECLENGTH:			Event_VecLength( *(idVec3 *)data[0] ); break;
	case TE_DOTPRODUCT:			Event_VecDotProduct( *(idVec3 *)data[0], *(idVec3 *)data[1] ); break;
	case TE_CROSSPRODUCT:		Event_VecCrossProduct( *(idVec3 *)data[0], *(idVec3 *)data[1] ); break;
	case TE_VECTOANGLES:		Event_VecToAngles( *(idVec3 *)data[0] ); break;
	case TE_WARNING:			Event_Warning( (const char *)data[0] ); break;
	case TE_ERROR:				Event_Error( (const char *)data[0] ); break;
	case TE_STRLEN:				Event_StrLen( (const char *)data[0] ); break;
	case TE_STRLEFT:			Event_StrLeft( (const char *)data[0], (int)data[1] ); break;
	case TE_STRRIGHT:			Event_StrRight( (const char *)data[0], (int)data[1] ); break;
	case TE_STRSKIP:			Event_StrSkip( (const char *)data[0], (int)data[1] ); break;
	case TE_STRMID:				Event_StrMid( (const char *)data[0], (int)data[1], (int)data[2] ); break;
	case TE_STRTOFLOAT:			Event_StrToFloat( (const char *)data[0] ); break;
	case TE_ISCLIENT:			Event_IsClient(); break;
	case TE_ISMULTIPLAYER:		Event_IsMultiplayer(); break;
	case TE_GETFRAMETIME:		Event_GetFrameTime(); break;
	case TE_GETTICSPERSECOND:	Event_GetTicsPerSecond(); break;
	default:
		Error( "event '%s' has no engine implementation", ev->GetName() );
		break;
	}
}

/*
================
idThread::CurrentThread
================
*/
idThread *idThread::CurrentThread( void ) {
	return currentThread;
}

/*
================
idThread::CurrentThreadNum
================
*/
int idThread::CurrentThreadNum( void ) {
	if ( currentThread ) {
		return currentThread->GetThreadNum();
	} else {
		return 0;
	}
}

/*
================
idThread::idThread
================
*/
idThread::idThread() {
	Init();
	SetThreadName( va( "thread_%d", threadIndex ) );
	if ( gameLocal.debugScript ) {
		gameLocal.Printf( "%d: create thread (%d) '%s'\n", gameLocal.time, threadNum, threadName.c_str() );
	}
}

/*
================
idThread::idThread
================
*/
idThread::idThread( scriptEntity_t self, const function_t *func ) {
	assert( self );
	
	Init();
	SetThreadName( func->Name() );
	interpreter.EnterObjectFunction( self, func, false );
	if ( gameLocal.debugScript ) {
		gameLocal.Printf( "%d: create thread (%d) '%s'\n", gameLocal.time, threadNum, threadName.c_str() );
	}
}

/*
================
idThread::idThread
================
*/
idThread::idThread( const function_t *func ) {
	assert( func );

	Init();
	SetThreadName( func->Name() );
	interpreter.EnterFunction( func, false );
	if ( gameLocal.debugScript ) {
		gameLocal.Printf( "%d: create thread (%d) '%s'\n", gameLocal.time, threadNum, threadName.c_str() );
	}
}

/*
================
idThread::idThread
================
*/
idThread::idThread( idInterpreter *source, const function_t *func, int args ) {
	Init();
	interpreter.ThreadCall( source, func, args );
	if ( gameLocal.debugScript ) {
		gameLocal.Printf( "%d: create thread (%d) '%s'\n", gameLocal.time, threadNum, threadName.c_str() );
	}
}

/*
================
idThread::idThread
================
*/
idThread::idThread( idInterpreter *source, scriptEntity_t self, const function_t *func, int args ) {
	assert( self );

	Init();
	SetThreadName( func->Name() );
	interpreter.ThreadCall( source, func, args );
	if ( gameLocal.debugScript ) {
		gameLocal.Printf( "%d: create thread (%d) '%s'\n", gameLocal.time, threadNum, threadName.c_str() );
	}
}

/*
================
idThread::~idThread
================
*/
idThread::~idThread() {
	idThread	*thread;
	int			i;
	int			n;

	if ( gameLocal.debugScript ) {
		gameLocal.Printf( "%d: end thread (%d) '%s'\n", gameLocal.time, threadNum, threadName.c_str() );
	}
	threadList.Remove( this );
	n = threadList.Num();
	for( i = 0; i < n; i++ ) {
		thread = threadList[ i ];
		if ( thread->WaitingOnThread() == this ) {
			thread->ThreadCallback( this );
		}
	}

	if ( currentThread == this ) {
		currentThread = NULL;
	}
}

/*
================
idThread::Init
================
*/
void idThread::Init( void ) {
	// create a unique threadNum
	do {
		threadIndex++;
		if ( threadIndex == 0 ) {
			threadIndex = 1;
		}
	} while( GetThread( threadIndex ) );

	threadNum = threadIndex;
	threadList.Append( this );
	
	creationTime = gameLocal.time;
	lastExecuteTime = 0;

	scheduled = false;
	scheduleTime = 0;
	scheduleSeq = 0;
	removed = false;
	frameStamp = -1;
	frameInstructions = 0;

	ClearWaitFor();

	interpreter.SetThread( this );
}

/*
================
idThread::GetThread
================
*/
idThread *idThread::GetThread( int num ) {
	int			i;
	int			n;
	idThread	*thread;

	n = threadList.Num();
	for( i = 0; i < n; i++ ) {
		thread = threadList[ i ];
		if ( thread->GetThreadNum() == num ) {
			return thread;
		}
	}

	return NULL;
}

/*
================
idThread::DisplayInfo
================
*/
void idThread::DisplayInfo( void ) {
	gameLocal.Printf( 
		"%12i: '%s'\n"
		"        File: %s(%d)\n"
		"     Created: %d (%d ms ago)\n"
		"      Status: ", 
		threadNum, threadName.c_str(), 
		interpreter.CurrentFile(), interpreter.CurrentLine(), 
		creationTime, gameLocal.time - creationTime );

	if ( interpreter.threadDying ) {
		gameLocal.Printf( "Dying\n" );
	} else if ( interpreter.awaitingGame ) {
		gameLocal.Printf( "Waiting for the game module\n" );
	} else if ( interpreter.doneProcessing ) {
		gameLocal.Printf( 
			"Paused since %d (%d ms)\n"
			"      Reason: ",  lastExecuteTime, gameLocal.time - lastExecuteTime );
		if ( waitingForThread ) {
			gameLocal.Printf( "Waiting for thread #%3i '%s'\n", waitingForThread->GetThreadNum(), waitingForThread->GetThreadName() );
		} else if ( waitingFor != ENTITY_NONE ) {
			gameLocal.Printf( "Waiting for entity %d\n", waitingFor );
		} else if ( waitingUntil ) {
			gameLocal.Printf( "Waiting until %d (%d ms total wait time)\n", waitingUntil, waitingUntil - lastExecuteTime );
		} else {
			gameLocal.Printf( "None\n" );
		}
	} else {
		gameLocal.Printf( "Processing\n" );
	}

	interpreter.DisplayInfo();

	gameLocal.Printf( "\n" );
}

/*
================
idThread::ListThreads
================
*/
void idThread::ListThreads( void ) {
	int	i;
	int	n;

	n = threadList.Num();
	for( i = 0; i < n; i++ ) {
		gameLocal.Printf( "%3i: %-20s : %s(%d)\n", threadList[ i ]->threadNum, threadList[ i ]->threadName.c_str(), threadList[ i ]->interpreter.CurrentFile(), threadList[ i ]->interpreter.CurrentLine() );
	}
	gameLocal.Printf( "%d active threads\n\n", n );
}

/*
================
idThread::Restart
================
*/
void idThread::Restart( void ) {
	int	i;
	int	n;

	// reset the threadIndex
	threadIndex = 0;
	scheduleCounter = 0;

	currentThread = NULL;
	n = threadList.Num();
	for( i = n - 1; i >= 0; i-- ) {
		delete threadList[ i ];
	}
	threadList.Clear();
}

/*
================
idThread::PostExecute

oax: D3 PostEventMS( &EV_Thread_Execute, delay ). One slot per thread;
posting again replaces it.
================
*/
void idThread::PostExecute( int delay ) {
	scheduled = true;
	scheduleTime = gameLocal.time + delay;
	scheduleSeq = ++scheduleCounter;
}

/*
================
idThread::NextDue

oax: the thread to run next at time: the earliest scheduled time, then the
earliest posted, as D3's event queue ordered them.
================
*/
idThread *idThread::NextDue( int time ) {
	int			i;
	idThread	*best = NULL;

	for ( i = 0; i < threadList.Num(); i++ ) {
		idThread *t = threadList[ i ];
		if ( !t->scheduled || t->removed || t->scheduleTime > time ) {
			continue;
		}
		if ( !best || t->scheduleTime < best->scheduleTime || ( t->scheduleTime == best->scheduleTime && t->scheduleSeq < best->scheduleSeq ) ) {
			best = t;
		}
	}
	if ( best ) {
		best->scheduled = false;
	}
	return best;
}

/*
================
idThread::DeleteRemoved

oax: D3 removed ended threads with EV_Remove; the pump calls this between
threads.
================
*/
void idThread::DeleteRemoved( void ) {
	int i;

	for ( i = 0; i < threadList.Num(); ) {
		if ( threadList[ i ]->removed && !threadList[ i ]->interpreter.awaitingGame ) {
			delete threadList[ i ];		// removes itself from threadList
		} else {
			i++;
		}
	}
}

/*
================
idThread::DelayedStart
================
*/
void idThread::DelayedStart( int delay ) {
	CancelExecute();
	if ( gameLocal.time <= 0 ) {
		delay++;
	}
	PostExecute( delay );
}

/*
================
idThread::Start
================
*/
bool idThread::Start( void ) {
	bool result;

	CancelExecute();
	result = Execute();

	return result;
}

/*
================
idThread::SetThreadName
================
*/
void idThread::SetThreadName( const char *name ) {
	threadName = name;
}

/*
================
idThread::ObjectMoveDone
================
*/
void idThread::ObjectMoveDone( int threadnum, scriptEntity_t obj ) {
	idThread *thread;

	if ( !threadnum ) {
		return;
	}

	thread = GetThread( threadnum );
	if ( thread ) {
		thread->ObjectMoveDone( obj );
	}
}

/*
================
idThread::End
================
*/
void idThread::End( void ) {
	// Tell thread to die.  It will exit on its own.
	Pause();
	interpreter.threadDying	= true;
}

/*
================
idThread::KillThread
================
*/
void idThread::KillThread( const char *name ) {
	int			i;
	int			num;
	int			len;
	const char	*ptr;
	idThread	*thread;

	// see if the name uses a wild card
	ptr = strchr( name, '*' );
	if ( ptr ) {
		len = ptr - name;
	} else {
		len = strlen( name );
	}

	// kill only those threads whose name matches name
	num = threadList.Num();
	for( i = 0; i < num; i++ ) {
		thread = threadList[ i ];
		if ( !idStr::Cmpn( thread->GetThreadName(), name, len ) ) {
			thread->End();
			// oax: a thread that is not running ends now (D3 posted its
			// removal; a thread waiting on an entity would never run again)
			if ( thread != currentThread && !thread->interpreter.awaitingGame ) {
				thread->CancelExecute();
				thread->removed = true;
			}
		}
	}
}

/*
================
idThread::KillThread
================
*/
void idThread::KillThread( int num ) {
	idThread *thread;

	thread = GetThread( num );
	if ( thread ) {
		// Tell thread to die.  It will delete itself on it's own.
		thread->End();
		// oax: a thread that is not running ends now (D3 posted its removal)
		if ( thread != currentThread && !thread->interpreter.awaitingGame ) {
			thread->CancelExecute();
			thread->removed = true;
		}
	}
}

/*
================
idThread::Execute

oax: errors inside the interpreter (D3: game-stopping common->Error) kill
just this thread; they longjmp back here.
================
*/
bool idThread::Execute( void ) {
	idThread * volatile	oldThread;
	volatile bool		done;
	scriptErrorJump_t	jump;

	oldThread = currentThread;
	currentThread = this;

	lastExecuteTime = gameLocal.time;
	if ( !interpreter.awaitingGame ) {
		ClearWaitFor();
	}

	jump.prev = scriptErrorJump;
	scriptErrorJump = &jump;
	if ( setjmp( jump.jb ) == 0 ) {
		done = interpreter.Execute();
		scriptErrorJump = jump.prev;
	} else {
		scriptErrorJump = jump.prev;
		Script_ReportError( jump.text );
		interpreter.awaitingGame = false;
		interpreter.threadDying = true;
		done = true;
	}

	if ( done ) {
		End();
		if ( interpreter.terminateOnExit ) {
			CancelExecute();
			removed = true;
		}
	} else if ( !interpreter.awaitingGame ) {
		if ( waitingUntil > lastExecuteTime ) {
			PostExecute( waitingUntil - lastExecuteTime );
		}
	}

	currentThread = oldThread;

	return done;
}

/*
================
idThread::IsWaiting

Checks if thread is still waiting for some event to occur.
================
*/
bool idThread::IsWaiting( void ) {
	if ( waitingForThread || ( waitingFor != ENTITY_NONE ) ) {
		return true;
	}

	if ( waitingUntil && ( waitingUntil > gameLocal.time ) ) {
		return true;
	}

	return false;
}

/*
================
idThread::CallFunction

NOTE: If this is called from within a event called by this thread, the function arguments will be invalid after calling this function.
================
*/
void idThread::CallFunction( const function_t *func, bool clearStack ) {
	ClearWaitFor();
	interpreter.EnterFunction( func, clearStack );
}

/*
================
idThread::CallFunction

NOTE: If this is called from within a event called by this thread, the function arguments will be invalid after calling this function.
================
*/
void idThread::CallFunction( scriptEntity_t self, const function_t *func, bool clearStack ) {
	assert( self );
	ClearWaitFor();
	interpreter.EnterObjectFunction( self, func, clearStack );
}

/*
================
idThread::ClearWaitFor
================
*/
void idThread::ClearWaitFor( void ) {
	waitingFor			= ENTITY_NONE;
	waitingForThread	= NULL;
	waitingUntil		= 0;
}

/*
================
idThread::IsWaitingFor
================
*/
bool idThread::IsWaitingFor( scriptEntity_t obj ) {
	assert( obj );
	return waitingFor == obj;
}

/*
================
idThread::ObjectMoveDone
================
*/
void idThread::ObjectMoveDone( scriptEntity_t obj ) {
	assert( obj );

	if ( IsWaitingFor( obj ) ) {
		ClearWaitFor();
		DelayedStart( 0 );
	}
}

/*
================
idThread::ThreadCallback
================
*/
void idThread::ThreadCallback( idThread *thread ) {
	if ( interpreter.threadDying ) {
		return;
	}

	if ( thread == waitingForThread ) {
		ClearWaitFor();
		DelayedStart( 0 );
	}
}

/*
================
idThread::Event_SetThreadName
================
*/
void idThread::Event_SetThreadName( const char *name ) {
	SetThreadName( name );
}

/*
================
idThread::Error
================
*/
void idThread::Error( const char *fmt, ... ) const {
	va_list	argptr;
	char	text[ 1024 ];

	va_start( argptr, fmt );
	idStr::vsnPrintf( text, sizeof( text ), fmt, argptr );
	va_end( argptr );

	interpreter.Error( "%s", text );
}

/*
================
idThread::Warning
================
*/
void idThread::Warning( const char *fmt, ... ) const {
	va_list	argptr;
	char	text[ 1024 ];

	va_start( argptr, fmt );
	idStr::vsnPrintf( text, sizeof( text ), fmt, argptr );
	va_end( argptr );

	interpreter.Warning( "%s", text );
}

/*
================
idThread::ReturnString
================
*/
void idThread::ReturnString( const char *text ) {
	gameLocal.program.ReturnString( text );
}

/*
================
idThread::ReturnFloat
================
*/
void idThread::ReturnFloat( float value ) {
	gameLocal.program.ReturnFloat( value );
}

/*
================
idThread::ReturnInt
================
*/
void idThread::ReturnInt( int value ) {
	// true integers aren't supported in the compiler,
	// so int values are stored as floats
	gameLocal.program.ReturnFloat( value );
}

/*
================
idThread::ReturnVector
================
*/
void idThread::ReturnVector( idVec3 const &vec ) {
	gameLocal.program.ReturnVector( vec );
}

/*
================
idThread::ReturnEntity
================
*/
void idThread::ReturnEntity( scriptEntity_t ent ) {
	gameLocal.program.ReturnEntity( ent );
}

/*
================
idThread::Pause
================
*/
void idThread::Pause( void ) {
	ClearWaitFor();
	interpreter.doneProcessing = true;
}

/*
================
idThread::WaitMS
================
*/
void idThread::WaitMS( int time ) {
	Pause();
	waitingUntil = gameLocal.time + time;
}

/*
================
idThread::WaitSec
================
*/
void idThread::WaitSec( float time ) {
	WaitMS( SEC2MS( time ) );
}

/*
================
idThread::WaitFrame

oax: D3 waited gameLocal.msec; here the thread wakes on the next RUN with a
later level time, whatever the frame length.
================
*/
void idThread::WaitFrame( void ) {
	Pause();
	waitingUntil = gameLocal.time + 1;
}

/*
================
idThread::CountInstruction
================
*/
bool idThread::CountInstruction( void ) {
	if ( frameStamp != gameLocal.time ) {
		frameStamp = gameLocal.time;
		frameInstructions = 0;
	}
	frameInstructions++;
	Script_CountInstruction();
	return frameInstructions > gameLocal.maxInstructions;
}

/*
================
idThread::Runaway
================
*/
void idThread::Runaway( int instructionPointer ) {
	const char	*file = "";
	int			line = 0;

	if ( instructionPointer >= 0 && instructionPointer < gameLocal.program.NumStatements() ) {
		file = gameLocal.program.GetFilenameForStatement( instructionPointer );
		line = gameLocal.program.GetLineNumberForStatement( instructionPointer );
	}
	Script_ReportRunaway( threadNum, threadName.c_str(), instructionPointer, file, line, frameInstructions );
}

/*
================
idThread::GameCallRecord
================
*/
oaxScriptCall_t *idThread::GameCallRecord( void ) {
	return &gameCall;
}

/*
================
idThread::PostGameCall

oax: hands gameCall (its arguments already filled in) to the pump.
================
*/
void idThread::PostGameCall( const idEventDef *ev, scriptEntity_t self, int kind ) {
	gameCall.event = ev->GetEventNum();
	gameCall.self = self;
	gameCall.thread = threadNum;
	gameCallKind = kind;
	gameCallSelf = self;
	Script_GameCallPosted( this, &gameCall, ev );
}

/*
================
idThread::FinishGameCall

oax: the game module's result for the call this thread is waiting on.
================
*/
void idThread::FinishGameCall( const oaxScriptValue_t *value, const char *string ) {
	const idEventDef *ev;
	idVec3 v;

	if ( !interpreter.awaitingGame ) {
		return;
	}
	ev = idEventDef::GetEventCommand( gameCall.event );

	if ( gameCallKind == GAMECALL_WAITFOR ) {
		int busy = 0;

		if ( value ) {
			busy = ( value->type == OAX_SV_FLOAT ) ? ( value->f[0] != 0.0f ) : ( value->i != 0 );
		}
		interpreter.EndGameCall();
		// D3 idThread::Event_WaitFor
		if ( busy ) {
			Pause();
			waitingFor = gameCallSelf;
		}
		return;
	}

	switch ( ev ? ev->GetReturnType() : 0 ) {
	case D_EVENT_INTEGER:
		ReturnInt( !value ? 0 : value->type == OAX_SV_FLOAT ? (int)value->f[0] : value->i );
		break;
	case D_EVENT_FLOAT:
		ReturnFloat( !value ? 0.0f : value->type == OAX_SV_FLOAT ? value->f[0] : (float)value->i );
		break;
	case D_EVENT_VECTOR:
		if ( value && value->type == OAX_SV_VECTOR ) {
			v.Set( value->f[0], value->f[1], value->f[2] );
		} else {
			v.Zero();
		}
		ReturnVector( v );
		break;
	case D_EVENT_STRING:
		ReturnString( ( value && value->type == OAX_SV_STRING && string ) ? string : "" );
		break;
	case D_EVENT_ENTITY:
	case D_EVENT_ENTITY_NULL:
		ReturnEntity( ( value && value->type == OAX_SV_ENTITY ) ? value->i : ENTITY_NONE );
		break;
	default:
		break;
	}
	interpreter.EndGameCall();
}

/***********************************************************************

  Script callable events  
	
***********************************************************************/

/*
================
idThread::Event_TerminateThread
================
*/
void idThread::Event_TerminateThread( int num ) {
	KillThread( num );
}

/*
================
idThread::Event_Pause
================
*/
void idThread::Event_Pause( void ) {
	Pause();
}

/*
================
idThread::Event_Wait
================
*/
void idThread::Event_Wait( float time ) {
	WaitSec( time );
}

/*
================
idThread::Event_WaitFrame
================
*/
void idThread::Event_WaitFrame( void ) {
	WaitFrame();
}

/*
================
idThread::Event_WaitFor

oax: D3 asked the entity directly (EV_Thread_SetCallback). Here the game
module answers "<script_setcallback>" on the entity: nonzero means it will
call OAXScript_ObjectDone for this thread when it is done, so the thread
waits until then.
================
*/
void idThread::Event_WaitFor( scriptEntity_t ent ) {
	const idEventDef *cb;

	cb = idEventDef::FindEvent( "<script_setcallback>" );
	if ( ent && cb ) {
		oaxScriptCall_t *call = GameCallRecord();
		call->argc = 0;
		PostGameCall( cb, ent, GAMECALL_WAITFOR );
		interpreter.awaitingGame = true;
		interpreter.doneProcessing = true;
	}
}

/*
================
idThread::Event_WaitForThread
================
*/
void idThread::Event_WaitForThread( int num ) {
	idThread *thread;

	thread = GetThread( num );
	if ( !thread || thread->removed ) {
		if ( gameLocal.debugScript ) {
			// just print a warning and continue executing
			Warning( "Thread %d not running", num );
		}
	} else {
		Pause();
		waitingForThread = thread;
	}
}

/*
================
idThread::Event_Print
================
*/
void idThread::Event_Print( const char *text ) {
	gameLocal.Printf( "%s", text );
}

/*
================
idThread::Event_PrintLn
================
*/
void idThread::Event_PrintLn( const char *text ) {
	gameLocal.Printf( "%s\n", text );
}

/*
================
idThread::Event_Assert
================
*/
void idThread::Event_Assert( float value ) {
	if ( !value ) {
		Error( "assertion failed" );
	}
}

/*
================
idThread::Event_Random

oax: the VM's own xorshift32, seeded by the game (D3: gameLocal.random)
================
*/
void idThread::Event_Random( float range ) const {
	float result;

	result = gameLocal.RandomFloat();
	ReturnFloat( range * result );
}

/*
================
idThread::Event_GetTime

oax: level time (D3: realClientTime)
================
*/
void idThread::Event_GetTime( void ) {
	ReturnFloat( MS2SEC( gameLocal.time ) );
}

/*
================
idThread::Event_KillThread
================
*/
void idThread::Event_KillThread( const char *name ) {
	KillThread( name );
}

/*
================
idThread::Event_AngToForward
================
*/
void idThread::Event_AngToForward( idAngles &ang ) {
	ReturnVector( ang.ToForward() );
}

/*
================
idThread::Event_AngToRight
================
*/
void idThread::Event_AngToRight( idAngles &ang ) {
	idVec3 vec;

	ang.ToVectors( NULL, &vec );
	ReturnVector( vec );
}

/*
================
idThread::Event_AngToUp
================
*/
void idThread::Event_AngToUp( idAngles &ang ) {
	idVec3 vec;

	ang.ToVectors( NULL, NULL, &vec );
	ReturnVector( vec );
}

/*
================
idThread::Event_GetSine
================
*/
void idThread::Event_GetSine( float angle ) {
	ReturnFloat( idMath::Sin( DEG2RAD( angle ) ) );
}

/*
================
idThread::Event_GetCosine
================
*/
void idThread::Event_GetCosine( float angle ) {
	ReturnFloat( idMath::Cos( DEG2RAD( angle ) ) );
}

/*
================
idThread::Event_GetSquareRoot
================
*/
void idThread::Event_GetSquareRoot( float theSquare ) {
	ReturnFloat( idMath::Sqrt( theSquare ) );
}

/*
================
idThread::Event_VecNormalize
================
*/
void idThread::Event_VecNormalize( idVec3 &vec ) {
	idVec3 n;

	n = vec;
	n.Normalize();
	ReturnVector( n );
}

/*
================
idThread::Event_VecLength
================
*/
void idThread::Event_VecLength( idVec3 &vec ) {
	ReturnFloat( vec.Length() );
}

/*
================
idThread::Event_VecDotProduct
================
*/
void idThread::Event_VecDotProduct( idVec3 &vec1, idVec3 &vec2 ) {
	ReturnFloat( vec1 * vec2 );
}

/*
================
idThread::Event_VecCrossProduct
================
*/
void idThread::Event_VecCrossProduct( idVec3 &vec1, idVec3 &vec2 ) {
	ReturnVector( vec1.Cross( vec2 ) );
}

/*
================
idThread::Event_VecToAngles
================
*/
void idThread::Event_VecToAngles( idVec3 &vec ) {
	idAngles ang = vec.ToAngles();
	ReturnVector( idVec3( ang.pitch, ang.yaw, ang.roll ) );
}

/*
================
idThread::Event_Warning
================
*/
void idThread::Event_Warning( const char *text ) {
	Warning( "%s", text );
}

/*
================
idThread::Event_Error
================
*/
void idThread::Event_Error( const char *text ) {
	Error( "%s", text );
}

/*
================
idThread::Event_StrLen
================
*/
void idThread::Event_StrLen( const char *string ) {
	int len;

	len = idStr::Length( string );
	idThread::ReturnInt( len );
}

/*
================
idThread::Event_StrLeft
================
*/
void idThread::Event_StrLeft( const char *string, int num ) {
	int len;

	if ( num < 0 ) {
		idThread::ReturnString( "" );
		return;
	}

	len = idStr::Length( string );
	if ( len < num ) {
		idThread::ReturnString( string );
		return;
	}

	idStr result( string, 0, num );
	idThread::ReturnString( result );
}

/*
================
idThread::Event_StrRight 
================
*/
void idThread::Event_StrRight( const char *string, int num ) {
	int len;

	if ( num < 0 ) {
		idThread::ReturnString( "" );
		return;
	}

	len = idStr::Length( string );
	if ( len < num ) {
		idThread::ReturnString( string );
		return;
	}

	idThread::ReturnString( string + len - num );
}

/*
================
idThread::Event_StrSkip
================
*/
void idThread::Event_StrSkip( const char *string, int num ) {
	int len;

	if ( num < 0 ) {
		idThread::ReturnString( string );
		return;
	}

	len = idStr::Length( string );
	if ( len < num ) {
		idThread::ReturnString( "" );
		return;
	}

	idThread::ReturnString( string + num );
}

/*
================
idThread::Event_StrMid
================
*/
void idThread::Event_StrMid( const char *string, int start, int num ) {
	int len;

	if ( num < 0 ) {
		idThread::ReturnString( "" );
		return;
	}

	if ( start < 0 ) {
		start = 0;
	}
	len = idStr::Length( string );
	if ( start > len ) {
		start = len;
	}

	if ( start + num > len ) {
		num = len - start;
	}

	idStr result( string, start, start + num );
	idThread::ReturnString( result );
}

/*
================
idThread::Event_StrToFloat( const char *string )
================
*/
void idThread::Event_StrToFloat( const char *string ) {
	float result;

	result = atof( string );
	idThread::ReturnFloat( result );
}

/*
================
idThread::Event_IsClient
================
*/
void idThread::Event_IsClient( void ) { 
	idThread::ReturnFloat( 0 );
}

/*
================
idThread::Event_IsMultiplayer
================
*/
void idThread::Event_IsMultiplayer( void ) { 
	idThread::ReturnFloat( 1 );
}

/*
================
idThread::Event_GetFrameTime
================
*/
void idThread::Event_GetFrameTime( void ) { 
	idThread::ReturnFloat( MS2SEC( gameLocal.msec ) );
}

/*
================
idThread::Event_GetTicsPerSecond
================
*/
void idThread::Event_GetTicsPerSecond( void ) { 
	idThread::ReturnFloat( gameLocal.msec > 0 ? 1000.0f / gameLocal.msec : 0.0f );
}
