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

Adapted from DOOM-3 neo/game/script/Script_Thread.h. Changed: idThread is
no longer an idClass. Its script events are a built-in table
(ProcessEngineEvent) instead of an idClass event list, and D3's event
queue (PostEventMS / ServiceEvents) is replaced by a schedule kept on the
threads themselves, run by the pump (OAXScript_Run): due threads run in
(time, post order) order, as D3's queue ran them. Events that need the
game (entity events, the game's sys events, waitFor's callback) are
handed to the game module as call records and the thread waits for the
result. Save games, signals, cameras, traces, fades and debug drawing are
gone (the game module implements what it needs as events). Each thread
counts its instructions per frame; a thread past the limit is killed.

===========================================================================
*/

#ifndef __SCRIPT_THREAD_H__
#define __SCRIPT_THREAD_H__

// what a call handed to the game module is for
#define GAMECALL_EVENT				0		// an event; its result is the call's return value
#define GAMECALL_WAITFOR			1		// sys.waitFor's "<script_setcallback>": nonzero means wait

// the engine's own thread events (sys.*)
enum {
	TE_TERMINATE = 1,
	TE_PAUSE,
	TE_WAIT,
	TE_WAITFRAME,
	TE_WAITFOR,
	TE_WAITFORTHREAD,
	TE_PRINT,
	TE_PRINTLN,
	TE_ASSERT,
	TE_RANDOM,
	TE_GETTIME,
	TE_KILLTHREAD,
	TE_THREADNAME,
	TE_ANGTOFORWARD,
	TE_ANGTORIGHT,
	TE_ANGTOUP,
	TE_SIN,
	TE_COS,
	TE_SQRT,
	TE_VECNORMALIZE,
	TE_VECLENGTH,
	TE_DOTPRODUCT,
	TE_CROSSPRODUCT,
	TE_VECTOANGLES,
	TE_WARNING,
	TE_ERROR,
	TE_STRLEN,
	TE_STRLEFT,
	TE_STRRIGHT,
	TE_STRSKIP,
	TE_STRMID,
	TE_STRTOFLOAT,
	TE_ISCLIENT,
	TE_ISMULTIPLAYER,
	TE_GETFRAMETIME,
	TE_GETTICSPERSECOND,
	TE_NUM
};

class idThread {
private:
	static idThread				*currentThread;

	idThread					*waitingForThread;
	int							waitingFor;			// entity handle, ENTITY_NONE when not waiting
	int							waitingUntil;
	idInterpreter				interpreter;

	int 						threadNum;
	idStr 						threadName;

	int							lastExecuteTime;
	int							creationTime;

	// oax: the schedule (D3: an EV_Thread_Execute event in the queue)
	bool						scheduled;
	int							scheduleTime;
	int							scheduleSeq;
	bool						removed;			// ended; deleted by the pump

	// oax: runaway check
	int							frameStamp;
	int							frameInstructions;

	static int					threadIndex;
	static int					scheduleCounter;
	static idList<idThread *>	threadList;

	void						Init( void );
	void						Pause( void );
	void						PostExecute( int delay );
	void						CancelExecute( void ) { scheduled = false; }

	// script callable events (the engine's own)
	void						Event_TerminateThread( int num );
	void						Event_Pause( void );
	void						Event_Wait( float time );
	void						Event_WaitFrame( void );
	void						Event_WaitFor( scriptEntity_t ent );
	void						Event_WaitForThread( int num );
	void						Event_Print( const char *text );
	void						Event_PrintLn( const char *text );
	void						Event_Assert( float value );
	void						Event_Random( float range ) const;
	void						Event_GetTime( void );
	void						Event_KillThread( const char *name );
	void						Event_SetThreadName( const char *name );
	void						Event_AngToForward( idAngles &ang );
	void						Event_AngToRight( idAngles &ang );
	void						Event_AngToUp( idAngles &ang );
	void						Event_GetSine( float angle );
	void						Event_GetCosine( float angle );
	void						Event_GetSquareRoot( float theSquare );
	void						Event_VecNormalize( idVec3 &vec );
	void						Event_VecLength( idVec3 &vec );
	void						Event_VecDotProduct( idVec3 &vec1, idVec3 &vec2 );
	void						Event_VecCrossProduct( idVec3 &vec1, idVec3 &vec2 );
	void						Event_VecToAngles( idVec3 &vec );
	void						Event_Warning( const char *text );
	void						Event_Error( const char *text );
	void 						Event_StrLen( const char *string );
	void 						Event_StrLeft( const char *string, int num );
	void 						Event_StrRight( const char *string, int num );
	void 						Event_StrSkip( const char *string, int num );
	void 						Event_StrMid( const char *string, int start, int num );
	void						Event_StrToFloat( const char *string );
	void						Event_IsClient( void );
	void 						Event_IsMultiplayer( void );
	void 						Event_GetFrameTime( void );
	void 						Event_GetTicsPerSecond( void );

public:
								idThread();
								idThread( scriptEntity_t self, const function_t *func );
								idThread( const function_t *func );
								idThread( idInterpreter *source, const function_t *func, int args );
								idThread( idInterpreter *source, scriptEntity_t self, const function_t *func, int args );

								~idThread();

	void						EnableDebugInfo( void ) { interpreter.debug = true; };
	void						DisableDebugInfo( void ) { interpreter.debug = false; };

	void						WaitMS( int time );
	void						WaitSec( float time );
	void						WaitFrame( void );

								// NOTE: If this is called from within a event called by this thread, the function arguments will be invalid after calling this function.
	void						CallFunction( const function_t	*func, bool clearStack );

								// NOTE: If this is called from within a event called by this thread, the function arguments will be invalid after calling this function.
	void						CallFunction( scriptEntity_t obj, const function_t *func, bool clearStack );

	void						DisplayInfo();
	static idThread				*GetThread( int num );
	static void					ListThreads( void );
	static void					Restart( void );
	static void					ObjectMoveDone( int threadnum, scriptEntity_t obj );

	static idList<idThread*>&	GetThreads ( void );

	bool						IsDoneProcessing ( void );
	bool						IsDying			 ( void );

	void						End( void );
	static void					KillThread( const char *name );
	static void					KillThread( int num );
	bool						Execute( void );
	void						DoneProcessing( void ) { interpreter.doneProcessing = true; };
	void						ContinueProcessing( void ) { interpreter.doneProcessing = false; };
	bool						ThreadDying( void ) { return interpreter.threadDying; };
	void						EndThread( void ) { interpreter.threadDying = true; };
	bool						IsWaiting( void );
	void						ClearWaitFor( void );
	bool						IsWaitingFor( scriptEntity_t obj );
	void						ObjectMoveDone( scriptEntity_t obj );
	void						ThreadCallback( idThread *thread );
	void						DelayedStart( int delay );
	bool						Start( void );
	idThread					*WaitingOnThread( void );
	void						SetThreadNum( int num );
	int 						GetThreadNum( void );
	void						SetThreadName( const char *name );
	const char					*GetThreadName( void );

	void						Error( const char *fmt, ... ) const id_attribute((format(printf,2,3)));
	void						Warning( const char *fmt, ... ) const id_attribute((format(printf,2,3)));

	static idThread				*CurrentThread( void );
	static int					CurrentThreadNum( void );

	static void					ReturnString( const char *text );
	static void					ReturnFloat( float value );
	static void					ReturnInt( int value );
	static void					ReturnVector( idVec3 const &vec );
	static void					ReturnEntity( scriptEntity_t ent );

	// oax: the engine's thread events
	void						ProcessEngineEvent( const idEventDef *ev, intptr_t *data );
	static void					RegisterEngineEvents( void );

	// oax: calls to the game module (see oax_script.h)
	static oaxScriptCall_t *	GameCallRecord( void );
	void						PostGameCall( const idEventDef *ev, scriptEntity_t self, int kind );
	void						FinishGameCall( const oaxScriptValue_t *value, const char *string );
	bool						AwaitingGame( void ) const { return interpreter.awaitingGame; }

	// oax: the pump
	static idThread *			NextDue( int time );
	bool						IsRemoved( void ) const { return removed; }
	static void					DeleteRemoved( void );

	// oax: runaway check; true when this thread is over its budget
	bool						CountInstruction( void );
	void						Runaway( int instructionPointer );
};

/*
================
idThread::WaitingOnThread
================
*/
ID_INLINE idThread *idThread::WaitingOnThread( void ) {
	return waitingForThread;
}

/*
================
idThread::SetThreadNum
================
*/
ID_INLINE void idThread::SetThreadNum( int num ) {
	threadNum = num;
}

/*
================
idThread::GetThreadNum
================
*/
ID_INLINE int idThread::GetThreadNum( void ) {
	return threadNum;
}

/*
================
idThread::GetThreadName
================
*/
ID_INLINE const char *idThread::GetThreadName( void ) {
	return threadName.c_str();
}

/*
================
idThread::GetThreads
================
*/
ID_INLINE idList<idThread*>& idThread::GetThreads ( void ) {
	return threadList;
}

/*
================
idThread::IsDoneProcessing
================
*/
ID_INLINE bool idThread::IsDoneProcessing ( void ) {
	return interpreter.doneProcessing;
}

/*
================
idThread::IsDying
================
*/
ID_INLINE bool idThread::IsDying ( void ) {
	return interpreter.threadDying;
}

#endif /* !__SCRIPT_THREAD_H__ */
