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
oax_script.cpp: the id Tech 4 script VM's C entry points (oax_script.h)
and the pump that runs it for the game module.

New code for the oax engine (GPLv3, like the id Tech 4 code it drives).
It owns idScriptGame (Script_Local.h, what the VM sees of "the game"),
the error jumps that replace D3's exceptions, the event log tests compare
(time, event, self, thread rows; hashed), the runaway report and the
per-frame statistics published as debug values:

  g_script_time       level time of the last RUN
  g_script_threads    live threads after the frame's RUN
  g_script_instr      script instructions executed this frame
  g_script_calls      calls handed to the game module this frame
  g_script_us         time spent in RUN this frame, microseconds (0 on a
                      cart: it has no clock but level time)
  g_script_instr_max, g_script_calls_max, g_script_us_max: the worst frame
                      since INIT
  g_script_log_count  rows in the event log since INIT
  g_script_log_hash   FNV-1a over every row (8 hex digits)
  g_script_log_rhash  the same with times relative to the first row
  g_script_log_last   the last row
  g_script_runaway    "<thread> <name> ip <n> <file>:<line> after <count>"
  g_script_errors     compile + runtime errors since INIT
  g_script_error      the last error message
===========================================================================
*/

#include "Script_Local.h"

extern "C" {
void		idlib_DebugSet( const char *name, const char *value );
long long	idlib_Microseconds( void );
}

idScriptGame *			scriptGame = NULL;
scriptErrorJump_t *		scriptErrorJump = NULL;

// ---- idScriptGame -------------------------------------------------------------

idScriptGame::idScriptGame() {
	time = 0;
	previousTime = 0;
	msec = 0;
	randomState = 0x9e3779b9u;
	debugScript = false;
	disasm = false;
	developer = false;
	maxInstructions = 100000;
}

#define FORMAT_TEXT( text, fmt ) \
	char text[ 1024 ]; \
	va_list argptr; \
	va_start( argptr, fmt ); \
	idStr::vsnPrintf( text, sizeof( text ), fmt, argptr ); \
	va_end( argptr );

void idScriptGame::Printf( const char *fmt, ... ) const {
	FORMAT_TEXT( text, fmt );
	idlib_Print( text );
}

void idScriptGame::DPrintf( const char *fmt, ... ) const {
	FORMAT_TEXT( text, fmt );
	idlib_DPrint( text );
}

void idScriptGame::Warning( const char *fmt, ... ) const {
	FORMAT_TEXT( text, fmt );
	idlib_Print( "^3WARNING: " );
	idlib_Print( text );
	idlib_Print( "\n" );
}

void idScriptGame::Error( const char *fmt, ... ) const {
	FORMAT_TEXT( text, fmt );
	Script_ThreadError( text );
}

// xorshift32 (the design's seeded RNG; D3 used idRandom)
float idScriptGame::RandomFloat( void ) {
	unsigned int x = randomState;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	randomState = x;
	return ( x >> 8 ) * ( 1.0f / 16777216.0f );
}

void idFile::Printf( const char *fmt, ... ) {
	FORMAT_TEXT( text, fmt );
	idlib_Print( text );
}

// ---- errors ---------------------------------------------------------------------

static void Script_Jump( const char *text ) {
	if ( !scriptErrorJump ) {
		idlib_FatalError( text );
	}
	idStr::Copynz( scriptErrorJump->text, text, sizeof( scriptErrorJump->text ) );
	longjmp( scriptErrorJump->jb, 1 );
}

void Script_CompileError( const char *text ) {
	Script_Jump( text );
}

void Script_ThreadError( const char *text ) {
	Script_Jump( text );
}

// idLib (lexer, parser) errors while the VM runs
static void Script_LibError( const char *text ) {
	if ( scriptErrorJump ) {
		Script_Jump( text );
	}
}

// ---- event log and statistics ---------------------------------------------------

#define LOG_ROWS	1024

typedef struct {
	int		time;
	char	event[ 32 ];
	int		self;
	int		thread;
} logRow_t;

static logRow_t		logRows[ LOG_ROWS ];
static int			logCount;
static unsigned int	logHash;

static int			frameInstructions;
static int			frameCalls;
static int			numErrors;
static long long	frameMicros;
static int			worstInstr, worstCalls, worstMicros;	// the worst frame since INIT

static idThread *	pendingThread;		// thread with a call out to the game module
static bool			callHanded;			// RUN gave it to the game; RETURN expected
static idThread *	resumeThread;		// RETURN came; continue this one first

// the same rows with times relative to the first row, so a log that starts
// on player input (whose absolute level time depends on the build's server
// frame phase) still compares across builds
static unsigned		logRelHash;
static int			logFirstTime;

static void Log_HashInto( unsigned *h, const void *data, int len ) {
	const byte *p = (const byte *)data;
	int i;

	for ( i = 0; i < len; i++ ) {
		*h = ( *h ^ p[ i ] ) * 16777619u;
	}
}

static void Log_Hash( const void *data, int len ) {
	Log_HashInto( &logHash, data, len );
}

static void Log_Add( const char *event, int self, int thread ) {
	logRow_t *row = &logRows[ logCount % LOG_ROWS ];
	int t = gameLocal.time;

	row->time = t;
	idStr::Copynz( row->event, event, sizeof( row->event ) );
	row->self = self;
	row->thread = thread;
	logCount++;

	Log_Hash( &t, sizeof( t ) );
	Log_Hash( event, idStr::Length( event ) + 1 );
	Log_Hash( &self, sizeof( self ) );
	Log_Hash( &thread, sizeof( thread ) );

	if ( logCount == 1 ) {
		logFirstTime = t;
	}
	{
		int rel = t - logFirstTime;
		Log_HashInto( &logRelHash, &rel, sizeof( rel ) );
		Log_HashInto( &logRelHash, event, idStr::Length( event ) + 1 );
		Log_HashInto( &logRelHash, &self, sizeof( self ) );
		Log_HashInto( &logRelHash, &thread, sizeof( thread ) );
	}
}

void Script_ReportError( const char *text ) {
	numErrors++;
	gameLocal.Warning( "%s", text );
	idlib_DebugSet( "g_script_error", text );
	idlib_DebugSet( "g_script_errors", va( "%d", numErrors ) );
}

void Script_CountInstruction( void ) {
	frameInstructions++;
}

void Script_ReportRunaway( int threadNum, const char *threadName, int instructionPointer, const char *file, int line, int count ) {
	const char *text = va( "%d %s ip %d %s:%d after %d", threadNum, threadName, instructionPointer, file, line, count );

	gameLocal.Warning( "runaway thread killed: %s", text );
	idlib_DebugSet( "g_script_runaway", text );
	Log_Add( "<runaway>", instructionPointer, threadNum );
}

void Script_GameCallPosted( idThread *thread, oaxScriptCall_t *call, const idEventDef *ev ) {
	pendingThread = thread;
	callHanded = false;
	frameCalls++;
	Log_Add( ev->GetName(), call->self, call->thread );
}

static void Script_PublishFrame( void ) {
	idlib_DebugSet( "g_script_time", va( "%d", gameLocal.time ) );
	idlib_DebugSet( "g_script_threads", va( "%d", idThread::GetThreads().Num() ) );
	idlib_DebugSet( "g_script_instr", va( "%d", frameInstructions ) );
	idlib_DebugSet( "g_script_calls", va( "%d", frameCalls ) );
	idlib_DebugSet( "g_script_us", va( "%d", (int)frameMicros ) );
	if ( frameInstructions > worstInstr ) worstInstr = frameInstructions;
	if ( frameCalls > worstCalls ) worstCalls = frameCalls;
	if ( frameMicros > worstMicros ) worstMicros = (int)frameMicros;
	idlib_DebugSet( "g_script_instr_max", va( "%d", worstInstr ) );
	idlib_DebugSet( "g_script_calls_max", va( "%d", worstCalls ) );
	idlib_DebugSet( "g_script_us_max", va( "%d", worstMicros ) );
	idlib_DebugSet( "g_script_log_count", va( "%d", logCount ) );
	idlib_DebugSet( "g_script_log_hash", va( "%08x", logHash ) );
	idlib_DebugSet( "g_script_log_rhash", va( "%08x", logRelHash ) );
	if ( logCount ) {
		const logRow_t *row = &logRows[ ( logCount - 1 ) % LOG_ROWS ];
		idlib_DebugSet( "g_script_log_last", va( "%d %s %d %d", row->time, row->event, row->self, row->thread ) );
	}
}

// ---- C entry points ---------------------------------------------------------------

extern "C" {

int OAXScript_Init( int randomSeed ) {
	OAXScript_Shutdown();

	scriptGame = new idScriptGame;
	scriptErrorJump = NULL;
	idLib_SetErrorHandler( Script_LibError );
	gameLocal.randomState = randomSeed ? (unsigned int)randomSeed : 0x9e3779b9u;

	idEventDef::Clear();
	idThread::RegisterEngineEvents();
	gameLocal.program.Startup( NULL );

	memset( logRows, 0, sizeof( logRows ) );
	logCount = 0;
	logHash = 2166136261u;
	logRelHash = 2166136261u;
	logFirstTime = 0;
	frameInstructions = 0;
	frameCalls = 0;
	frameMicros = 0;
	worstInstr = worstCalls = worstMicros = 0;
	numErrors = 0;
	pendingThread = NULL;
	resumeThread = NULL;
	callHanded = false;
	idlib_DebugSet( "g_script_errors", "0" );
	idlib_DebugSet( "g_script_error", "" );
	idlib_DebugSet( "g_script_runaway", "" );
	Script_PublishFrame();
	return 1;
}

int OAXScript_RegisterEvent( const char *name, const char *argfmt, int ret, int flags ) {
	if ( !scriptGame ) {
		return -1;
	}
	flags &= ( OAX_EVENT_ENTITY | OAX_EVENT_SYS );
	if ( !flags ) {
		flags = OAX_EVENT_ENTITY;
	}
	return idEventDef::Register( name, argfmt, (char)ret, flags, 0 );
}

int OAXScript_CompileFile( const char *path ) {
	int result;

	if ( !scriptGame ) {
		return 0;
	}
	result = gameLocal.program.CompileFile( path );
	if ( result == 0 ) {
		numErrors++;
		idlib_DebugSet( "g_script_error", gameLocal.program.lastError );
		idlib_DebugSet( "g_script_errors", va( "%d", numErrors ) );
	}
	return result;
}

int OAXScript_SetEntity( const char *name, int handle ) {
	if ( !scriptGame ) {
		return 0;
	}
	return gameLocal.program.SetEntity( name, handle ) ? 1 : 0;
}

int OAXScript_StartThread( const char *funcName, int self ) {
	const function_t	*func;
	const idTypeDef		*type;
	idThread			*thread;

	if ( !scriptGame ) {
		return 0;
	}
	func = gameLocal.program.FindFunction( funcName );
	if ( !func || func->eventdef ) {
		gameLocal.Warning( "script function '%s' not found", funcName );
		return 0;
	}
	type = func->type;
	if ( type && type->NumParameters() == 0 ) {
		thread = new idThread( func );
	} else if ( type && type->NumParameters() == 1 && type->GetParmType( 0 )->Type() == ev_entity && self ) {
		// D3 called a trigger's function with the trigger as its object; a
		// function with one entity parameter gets self there
		thread = new idThread( self, func );
	} else {
		gameLocal.Warning( "script function '%s' must take no parameters or one entity", funcName );
		return 0;
	}
	thread->DelayedStart( 0 );
	return thread->GetThreadNum();
}

int OAXScript_Run( int levelTime, oaxScriptCall_t *call ) {
	long long	start;
	idThread	*thread;

	if ( !scriptGame ) {
		return 0;
	}

	start = idlib_Microseconds();
	if ( levelTime != gameLocal.time ) {
		gameLocal.previousTime = gameLocal.time;
		gameLocal.time = levelTime;
		gameLocal.msec = gameLocal.time - gameLocal.previousTime;
		frameInstructions = 0;
		frameCalls = 0;
		frameMicros = 0;
	}

	if ( pendingThread && callHanded ) {
		// RUN again without RETURN: the call returns a safe value
		OAXScript_Return( NULL, NULL );
	}

	while ( 1 ) {
		if ( resumeThread ) {
			thread = resumeThread;
			resumeThread = NULL;
			if ( thread->IsRemoved() || ( thread->IsDoneProcessing() && !thread->IsDying() && thread->IsWaiting() ) ) {
				continue;
			}
		} else {
			idThread::DeleteRemoved();
			thread = idThread::NextDue( gameLocal.time );
			if ( !thread ) {
				break;
			}
		}

		thread->Execute();

		if ( pendingThread == thread && thread->AwaitingGame() ) {
			*call = *idThread::GameCallRecord();
			callHanded = true;
			frameMicros += idlib_Microseconds() - start;
			return 1;
		}
	}

	idThread::DeleteRemoved();
	frameMicros += idlib_Microseconds() - start;
	Script_PublishFrame();
	return 0;
}

void OAXScript_Return( const oaxScriptValue_t *value, const char *string ) {
	idThread *thread;

	if ( !scriptGame || !pendingThread || !callHanded ) {
		return;
	}
	thread = pendingThread;
	pendingThread = NULL;
	callHanded = false;

	thread->FinishGameCall( value, string );
	resumeThread = thread;
}

void OAXScript_ObjectDone( int threadNum, int handle ) {
	if ( !scriptGame ) {
		return;
	}
	idThread::ObjectMoveDone( threadNum, handle );
}

void OAXScript_KillThread( int threadNum ) {
	if ( !scriptGame ) {
		return;
	}
	idThread::KillThread( threadNum );
}

int OAXScript_NumThreads( void ) {
	if ( !scriptGame ) {
		return 0;
	}
	return idThread::GetThreads().Num();
}

void OAXScript_Shutdown( void ) {
	if ( !scriptGame ) {
		return;
	}
	idThread::Restart();
	delete scriptGame;
	scriptGame = NULL;
	idEventDef::Clear();
	pendingThread = NULL;
	resumeThread = NULL;
	callHanded = false;
}

void OAXScript_SetMaxInstructions( int count ) {
	if ( scriptGame ) {
		gameLocal.maxInstructions = count > 0 ? count : 1;
	}
}

void OAXScript_SetDebug( int debugScript, int disasm, int developer ) {
	if ( scriptGame ) {
		gameLocal.debugScript = debugScript != 0;
		gameLocal.disasm = disasm != 0;
		gameLocal.developer = developer != 0;
	}
}

void OAXScript_ListThreads( void ) {
	if ( !scriptGame ) {
		idlib_Print( "no script VM\n" );
		return;
	}
	idThread::ListThreads();
}

int OAXScript_LogText( char *buf, int size ) {
	int i, first, len = 0;

	if ( size <= 0 ) {
		return 0;
	}
	buf[ 0 ] = '\0';
	first = logCount > LOG_ROWS ? logCount - LOG_ROWS : 0;
	for ( i = first; i < logCount; i++ ) {
		const logRow_t *row = &logRows[ i % LOG_ROWS ];
		int n = idStr::snPrintf( buf + len, size - len, "%d %s %d %d\n", row->time, row->event, row->self, row->thread );
		if ( n < 0 ) {
			break;
		}
		len += n;
	}
	return len;
}

}	// extern "C"
