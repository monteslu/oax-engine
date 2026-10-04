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
cl_oax_prof.c: where a frame's time goes, on screen and in files.

  cl_oaxPerfHud 1   one line: fps, frame ms, CPU and GPU totals
  cl_oaxPerfHud 2   the full breakdown: CPU per stage, GPU per pass, counts
  oaxprof           print the breakdown to the console
  oaxprof csv <file> [frames]   one row per frame for the next frames
                    (default 600) into <homepath>/<game>/<file>

All of them turn the renderer's profiler on (r_oaxProfile, renderergl2
tr_oax_prof.c) while they need it. The CPU stages: server (SV_Frame),
cgame (its frame, less the scene it hands the renderer), the renderer's
front end (culling, sorting) and back end (issuing GL calls), and the wait
at the end of the frame (glFinish and the swap: the CPU idle while the GPU
or the display catches up). GPU times are per pass, from timer queries.
===========================================================================
*/

#include "client.h"


static cvar_t	*cl_oaxPerfHud;
static qboolean	turnedOn;		// r_oaxProfile was set by us

static fileHandle_t	csvFile;
static int			csvLeft;

// smoothed over the same frames as the renderer's averages
static float	serverMs, cgameMs;

static qboolean ProfileOn( void ) {
	return Cvar_VariableIntegerValue( "r_oaxProfile" ) != 0;
}

static qboolean Wanted( void ) {
	return cl_oaxPerfHud->integer || csvLeft > 0;
}

static void Get( oaxProfile_t *p ) {
	Com_Memset( p, 0, sizeof( *p ) );
	if ( re.OAXGetProfile ) {
		re.OAXGetProfile( p );
	}
}

static float CgameOnly( const oaxProfile_t *p ) {
	float c = cgameMs - p->cpuFrontMs;	// the cgame's frame includes the scene submission

	return c > 0 ? c : 0;
}

static void PrintTable( void ) {
	oaxProfile_t p;
	int i;

	Get( &p );
	if ( !p.frames ) {
		Com_Printf( "oaxprof: no frames profiled yet (the profiler turns on now; run oaxprof again in a second)\n" );
		return;
	}
	Com_Printf( "frame %.2f ms (%.0f fps), averaged over %d frames\n", p.frameMs, p.frameMs > 0 ? 1000.0f / p.frameMs : 0, p.frames );
	Com_Printf( "CPU   server %.2f  cgame %.2f  render front %.2f  render back %.2f  wait for GPU/swap %.2f ms\n",
		serverMs, CgameOnly( &p ), p.cpuFrontMs, p.cpuBackMs, p.cpuWaitMs );
	if ( p.gpuTiming ) {
		Com_Printf( "GPU   %.2f ms total\n", p.gpuTotalMs );
		for ( i = 0; i < OAX_PROF_ZONES; i++ ) {
			Com_Printf( "      %-17s %6.2f ms  %4.1f%%\n", p.zoneNames[i], p.gpuMs[i], p.gpuTotalMs > 0 ? 100.0f * p.gpuMs[i] / p.gpuTotalMs : 0 );
		}
	} else {
		Com_Printf( "GPU   per-pass timing unavailable (needs timer queries)\n" );
	}
	Com_Printf( "count %.0f draw calls, %.0fk triangles, %.1f views, %.0f terrain chunks, %.0f foliage instances\n",
		p.draws, p.tris / 1000.0f, p.views, p.terrainChunks, p.foliageInstances );
}

static void CsvHeader( void ) {
	oaxProfile_t p;
	char line[1024];
	int i;

	Get( &p );
	Q_strncpyz( line, "frame_ms,server_ms,cgame_ms,front_ms,back_ms,wait_ms,gpu_ms", sizeof( line ) );
	for ( i = 0; i < OAX_PROF_ZONES; i++ ) {
		Q_strcat( line, sizeof( line ), va( ",gpu_%d", i ) );
	}
	Q_strcat( line, sizeof( line ), ",draws,tris,views\n" );
	FS_Write( line, strlen( line ), csvFile );
	// the zone names, as a comment row
	Q_strncpyz( line, "#", sizeof( line ) );
	for ( i = 0; i < OAX_PROF_ZONES; i++ ) {
		Q_strcat( line, sizeof( line ), va( " gpu_%d=%s;", i, p.zoneNames[i] ? p.zoneNames[i] : "?" ) );
	}
	Q_strcat( line, sizeof( line ), "\n" );
	FS_Write( line, strlen( line ), csvFile );
}

static void Prof_f( void ) {
	if ( !Q_stricmp( Cmd_Argv( 1 ), "csv" ) ) {
		const char *name = Cmd_Argv( 2 );

		if ( !name[0] ) {
			Com_Printf( "usage: oaxprof csv <file> [frames]\n" );
			return;
		}
		if ( csvFile ) {
			FS_FCloseFile( csvFile );
		}
		csvFile = FS_FOpenFileWrite_HomeData( name );
		if ( !csvFile ) {
			Com_Printf( "oaxprof: cannot write %s\n", name );
			csvLeft = 0;
			return;
		}
		csvLeft = Cmd_Argc() > 3 ? atoi( Cmd_Argv( 3 ) ) : 600;
		CsvHeader();
		Com_Printf( "oaxprof: recording %d frames to %s (averages over the last frames, one row a frame)\n", csvLeft, name );
		return;
	}
	if ( !ProfileOn() ) {
		Cvar_Set( "r_oaxProfile", "1" );
		turnedOn = qtrue;
	}
	PrintTable();
}

void CL_OAXProfInit( void ) {
	cl_oaxPerfHud = Cvar_Get( "cl_oaxPerfHud", "0", CVAR_ARCHIVE );
	Cvar_SetDescription( cl_oaxPerfHud, "Performance overlay: 1 one line (fps, frame, CPU, GPU), 2 the breakdown per stage and pass." );
	Cmd_AddCommand( "oaxprof", Prof_f );
}

/*
=================
CL_OAXProfFrame

Every client frame, after the screen is drawn: keeps the profiler on while
something wants it, smooths the engine-side times, writes CSV rows.
=================
*/
void CL_OAXProfFrame( void ) {
	oaxProfile_t p;

	if ( Wanted() && !ProfileOn() ) {
		Cvar_Set( "r_oaxProfile", "1" );
		turnedOn = qtrue;
	} else if ( !Wanted() && turnedOn ) {
		Cvar_Set( "r_oaxProfile", "0" );
		turnedOn = qfalse;
	}
	if ( !ProfileOn() ) {
		return;
	}
	serverMs += ( com_profServerUs * 0.001f - serverMs ) * 0.05f;
	cgameMs += ( com_profCgameUs * 0.001f - cgameMs ) * 0.05f;

	if ( csvLeft > 0 && csvFile ) {
		char line[1024];
		int i;

		Get( &p );
		Com_sprintf( line, sizeof( line ), "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f", p.frameMs, com_profServerUs * 0.001f,
			com_profCgameUs * 0.001f, p.cpuFrontMs, p.cpuBackMs, p.cpuWaitMs, p.gpuTiming ? p.gpuTotalMs : 0 );
		for ( i = 0; i < OAX_PROF_ZONES; i++ ) {
			Q_strcat( line, sizeof( line ), va( ",%.3f", p.gpuTiming ? p.gpuMs[i] : 0 ) );
		}
		Q_strcat( line, sizeof( line ), va( ",%.0f,%.0f,%.1f\n", p.draws, p.tris, p.views ) );
		FS_Write( line, strlen( line ), csvFile );
		if ( --csvLeft == 0 ) {
			FS_FCloseFile( csvFile );
			csvFile = 0;
			Com_Printf( "oaxprof: CSV done\n" );
		}
	}
}

/*
=================
CL_OAXProfDraw

The overlay, from SCR_DrawScreenField (on top of the game, under the
console).
=================
*/
void CL_OAXProfDraw( void ) {
	static float white[4] = { 1, 1, 1, 1 }, dim[4] = { 0.75f, 0.85f, 1, 1 };
	oaxProfile_t p;
	int y = 4 * SMALLCHAR_HEIGHT, i;

	if ( !cl_oaxPerfHud || !cl_oaxPerfHud->integer || !ProfileOn() ) {
		return;
	}
	Get( &p );
	if ( !p.frames ) {
		return;
	}
	SCR_DrawSmallStringExt( 8, y, va( "%3.0f fps  %5.2f ms   CPU front %.2f back %.2f wait %.2f   GPU %s",
		p.frameMs > 0 ? 1000.0f / p.frameMs : 0, p.frameMs, p.cpuFrontMs, p.cpuBackMs, p.cpuWaitMs,
		p.gpuTiming ? va( "%.2f ms", p.gpuTotalMs ) : "n/a" ), white, qtrue, qtrue );
	if ( cl_oaxPerfHud->integer < 2 ) {
		return;
	}
	y += SMALLCHAR_HEIGHT;
	SCR_DrawSmallStringExt( 8, y, va( "server %.2f  cgame %.2f ms   %.0f draws  %.0fk tris  %.1f views  %.0f chunks  %.0f foliage",
		serverMs, CgameOnly( &p ), p.draws, p.tris / 1000.0f, p.views, p.terrainChunks, p.foliageInstances ), dim, qtrue, qtrue );
	if ( !p.gpuTiming ) {
		return;
	}
	for ( i = 0; i < OAX_PROF_ZONES; i++ ) {
		y += SMALLCHAR_HEIGHT;
		SCR_DrawSmallStringExt( 8, y, va( "%-17s %6.2f ms %5.1f%%", p.zoneNames[i], p.gpuMs[i],
			p.gpuTotalMs > 0 ? 100.0f * p.gpuMs[i] / p.gpuTotalMs : 0 ), dim, qtrue, qtrue );
	}
}
