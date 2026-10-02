/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// cl_testscript.c -- scripted gamepad input and movement traces
//
// For checking that every build simulates identically: a pad script plays
// the same controller states, frame by frame, through the same
// IN_GamepadFrame() path on every platform, and the movement trace records
// the server's player state per frame. Comparing two builds' traces by
// command time is the native-versus-wasm test.
//
// A pad script is a text file, one line per segment:
//   <frames> <lx> <ly> <rx> <ry> <lt> <rt> [button ...]
// sticks -32768..32767, triggers 0..32767, buttons by index in
// in_gamepad_t order (0 = A, 1 = B, ...). '#' starts a comment.

#include "client.h"

#define MAX_PADSCRIPT_STEPS 1024

typedef struct {
	int          frames;
	in_gamepad_t pad;
} padStep_t;

static padStep_t padSteps[MAX_PADSCRIPT_STEPS];
static int       numPadSteps;
static int       padStep;       // current step, -1 = not playing
static int       padStepFrame;  // frames played in the current step
static qboolean  padWaiting;    // waiting for the start phase
static char      padOnDone[MAX_STRING_CHARS];  // console command run when the script ends
static int       padLastCmdTime;
static qboolean  padClockHeld;
static qboolean  padAligned;    // clock aligned, the idle command before the start is out
int              cl_padScriptStartTime = -1;   // command time the script started at

static fileHandle_t traceFile;

const playerState_t *SV_TestPlayerState( void );

// The script starts on the client's command clock (cl.serverTime) crossing a
// multiple of this, and the time it started is recorded: two builds' clocks
// can settle at different millisecond phases, so traces are compared
// relative to the start, in command time.
#define PADSCRIPT_PHASE_MSEC 400
#define PADSCRIPT_ALIGN_MSEC 16

/*
===============
CL_PadScriptHoldsClock

From a script's start until the next script (or disconnect) the command
clock may not drift (CL_AdjustTimeDelta), or command times would follow
snapshot arrival, which differs between builds.
===============
*/
qboolean CL_PadScriptHoldsClock( void ) {
	if ( clc.state != CA_ACTIVE ) {
		padClockHeld = qfalse;
	}
	return padClockHeld;
}

/*
===============
CL_PadScript_f
===============
*/
static void CL_PadScript_f( void ) {
	char       *buf, *text, *line;
	const char *name;
	int         len;

	if ( Cmd_Argc() < 2 || Cmd_Argc() > 3 ) {
		Com_Printf( "usage: padscript <file> [command to run when it ends] (or 'stop')\n" );
		return;
	}
	// A command to run at the end, instead of queueing it behind 'wait's: the
	// command buffer runs in order, so anything after a wait would also hold
	// back the commands the scripted key presses bind to.
	Q_strncpyz( padOnDone, Cmd_Argc() == 3 ? Cmd_Argv( 2 ) : "", sizeof( padOnDone ) );
	name = Cmd_Argv( 1 );
	if ( !Q_stricmp( name, "stop" ) ) {
		padStep = -1;
		return;
	}

	len = FS_ReadFile( name, (void **)&buf );
	if ( len <= 0 ) {
		Com_Printf( "padscript: couldn't read %s\n", name );
		return;
	}

	numPadSteps = 0;
	text = buf;
	while ( text && *text && numPadSteps < MAX_PADSCRIPT_STEPS ) {
		padStep_t *st = &padSteps[numPadSteps];
		char      *next = strchr( text, '\n' ), *hash, *tok;
		int        n = 0, v[7];

		if ( next ) {
			*next++ = '\0';
		}
		line = text;
		text = next;
		if ( ( hash = strchr( line, '#' ) ) != NULL ) {
			*hash = '\0';
		}

		Com_Memset( st, 0, sizeof( *st ) );
		for ( tok = strtok( line, " \t\r" ); tok; tok = strtok( NULL, " \t\r" ) ) {
			if ( n < 7 ) {
				v[n++] = atoi( tok );
			} else {
				int b = atoi( tok );
				if ( b >= 0 && b < IN_GAMEPAD_BUTTONS ) {
					st->pad.buttons[b] = qtrue;
				}
			}
		}
		if ( n < 7 || v[0] <= 0 ) {
			continue;
		}
		st->frames = v[0];
		st->pad.axes[0] = v[1];
		st->pad.axes[1] = v[2];
		st->pad.axes[2] = v[3];
		st->pad.axes[3] = v[4];
		st->pad.axes[4] = v[5];
		st->pad.axes[5] = v[6];
		numPadSteps++;
	}
	FS_FreeFile( buf );

	padStep = numPadSteps ? 0 : -1;
	padStepFrame = 0;
	padWaiting = qtrue;
	padAligned = qfalse;
	padClockHeld = qfalse;
	padLastCmdTime = cl.serverTime;
	cl_padScriptStartTime = -1;
	Com_Printf( "padscript: %s, %d steps\n", name, numPadSteps );
}

/*
===============
CL_PadScriptFrame

Called by the platform's IN_Frame. While a script plays it supplies the
gamepad state and returns qtrue, and the platform skips its own pad.
===============
*/
qboolean CL_PadScriptFrame( int eventTime ) {
	static const in_gamepad_t idle;
	cvar_t *threshold, *analog;

	if ( padStep < 0 ) {
		return qfalse;
	}
	// The pad's key events are stamped with the frame clock, not the
	// platform's event time, so held-key fractions (CL_KeyState) come out the
	// same on every build.
	eventTime = com_frameTime;

	threshold = Cvar_Get( "joy_threshold", "0.15", CVAR_ARCHIVE );
	analog = Cvar_Get( "in_joystickUseAnalog", "0", CVAR_ARCHIVE );

	if ( padAligned ) {
		// One idle command went out on the aligned clock, so the command
		// before the script's first is the same on every build too.
		padAligned = qfalse;
		padWaiting = qfalse;
		cl_padScriptStartTime = cl.serverTime;
		if ( traceFile ) {
			char row[64];
			Com_sprintf( row, sizeof( row ), "# start %d\n", cl.serverTime );
			FS_Write( row, (int)strlen( row ), traceFile );
		}
	} else if ( padWaiting ) {
		int t = cl.serverTime;

		if ( clc.state == CA_ACTIVE && t / PADSCRIPT_PHASE_MSEC != padLastCmdTime / PADSCRIPT_PHASE_MSEC ) {
			// Put the command clock on a whole frame, so every build issues
			// its commands at the same times (pmove_fixed steps on 8 ms
			// boundaries of command time, so the phase changes the physics).
			// The clock is realtime + delta (cl_timeNudge aside), clamped so it
			// never runs backwards, so align that sum, not cl.serverTime.
			// The first frame after the boundary, not just any 16 ms frame:
			// the script then starts at the same phase of the server's 50 ms
			// frames and of 16 ms physics ticks on every build (both divide
			// 400), which server-simulated things (vehicles, movers) need.
			// t < boundary + 16 and base <= t, so this never runs backwards.
			int base = cls.realtime + cl.serverTimeDelta;
			int target = ( t / PADSCRIPT_PHASE_MSEC ) * PADSCRIPT_PHASE_MSEC + PADSCRIPT_ALIGN_MSEC;
			int adj = target - base;

			if ( adj < 0 || target < cl.serverTime ) {
				adj = ( PADSCRIPT_ALIGN_MSEC - ( ( base % PADSCRIPT_ALIGN_MSEC ) + PADSCRIPT_ALIGN_MSEC ) % PADSCRIPT_ALIGN_MSEC ) % PADSCRIPT_ALIGN_MSEC;
				if ( base + adj < cl.serverTime ) {
					adj += PADSCRIPT_ALIGN_MSEC * ( ( cl.serverTime - base - adj + PADSCRIPT_ALIGN_MSEC - 1 ) / PADSCRIPT_ALIGN_MSEC );
				}
			}
			cl.serverTimeDelta += adj;
			cl.serverTime = cl.oldServerTime = base + adj;
			t = cl.serverTime;
			padClockHeld = qtrue;
			padAligned = qtrue;
			IN_GamepadFrame( &idle, eventTime, threshold->value, analog->integer ? qtrue : qfalse );
			padLastCmdTime = t;
			return qtrue;
		}
		padLastCmdTime = t;
		return qfalse;
	}

	if ( padStep >= numPadSteps ) {
		// release everything once, then hand the pad back
		IN_GamepadFrame( &idle, eventTime, threshold->value, analog->integer ? qtrue : qfalse );
		padStep = -1;
		Com_Printf( "padscript: done\n" );
		if ( padOnDone[0] ) {
			Cbuf_AddText( padOnDone );
			Cbuf_AddText( "\n" );
			padOnDone[0] = '\0';
		}
		return qtrue;
	}

	IN_GamepadFrame( &padSteps[padStep].pad, eventTime, threshold->value, analog->integer ? qtrue : qfalse );
	if ( ++padStepFrame >= padSteps[padStep].frames ) {
		padStep++;
		padStepFrame = 0;
	}
	return qtrue;
}

/*
===============
CL_Trace_f

trace <file>: append one row per frame of the local server's player state
(commandTime, origin, velocity, ground entity) to <file> in the home
directory. "trace stop" closes it.
===============
*/
static void CL_Trace_f( void ) {
	if ( Cmd_Argc() != 2 ) {
		Com_Printf( "usage: trace <file> (or 'stop')\n" );
		return;
	}
	if ( traceFile ) {
		FS_FCloseFile( traceFile );
		traceFile = 0;
	}
	if ( Q_stricmp( Cmd_Argv( 1 ), "stop" ) ) {
		traceFile = FS_FOpenFileWrite_HomeData( Cmd_Argv( 1 ) );
		if ( !traceFile ) {
			Com_Printf( "trace: couldn't open %s\n", Cmd_Argv( 1 ) );
		}
	}
}

/*
===============
CL_TraceFrame
===============
*/
void CL_TraceFrame( void ) {
	const playerState_t *ps;
	char                 row[256];

	if ( !traceFile ) {
		return;
	}
	ps = SV_TestPlayerState();
	if ( !ps ) {
		return;
	}
	Com_sprintf( row, sizeof( row ), "%d %.9g %.9g %.9g %.9g %.9g %.9g %d %.9g\n",
		ps->commandTime, ps->origin[0], ps->origin[1], ps->origin[2],
		ps->velocity[0], ps->velocity[1], ps->velocity[2],
		ps->groundEntityNum == ENTITYNUM_NONE ? -1 : ps->groundEntityNum,
		ps->viewangles[YAW] );
	FS_Write( row, (int)strlen( row ), traceFile );
	FS_Flush( traceFile );
}

void CL_TestScriptInit( void ) {
	padStep = -1;
	Cmd_AddCommand( "padscript", CL_PadScript_f );
	Cmd_AddCommand( "trace", CL_Trace_f );
}

void CL_TestScriptShutdown( void ) {
	Cmd_RemoveCommand( "padscript" );
	Cmd_RemoveCommand( "trace" );
	if ( traceFile ) {
		FS_FCloseFile( traceFile );
		traceFile = 0;
	}
	padStep = -1;
}
