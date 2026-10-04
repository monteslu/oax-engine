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
wasmcart platform backend: cart entry points and the Sys_* layer.

Replaces code/sys/sys_main.c + sys_unix.c for the wasmcart build. The host
calls wc_render() once per frame; each call runs exactly one Com_Frame().

Determinism contract (WC_FLAG_DETERMINISTIC):
  - all time comes from wc_time_t (Sys_Milliseconds, time(), clock_gettime,
    gettimeofday are all served from it here; the host's WASI clock is never
    read),
  - all entropy comes from the seed the host passes to wc_set_seed().
===========================================================================
*/

#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "../sys/sys_local.h"
#include "../client/keycodes.h"

char *Key_GetBinding( int keynum );
#include "wc_local.h"

#ifndef WC_HOST_FLAG_DETERMINISTIC
#define WC_HOST_FLAG_DETERMINISTIC 0x01
#endif

#define WC_GPU_API_GLES3 1

wc_info_t       wc_info;
wc_host_info_t  wc_host_info;
wc_pad_t        wc_pads[4];
wc_time_t       wc_time;
uint8_t         wc_keys[32];
wc_pointer_t    wc_pointers[10];
wc_wheel_t      wc_wheel;

int16_t         wc_audio_ring[WC_AUDIO_CAP * 2];
uint32_t        wc_audio_write_cursor;

int             wc_width = WC_DEFAULT_WIDTH;
int             wc_height = WC_DEFAULT_HEIGHT;
int             wc_deterministic;

static int      wc_started;
static int      wc_halted;
static jmp_buf  wc_haltFrame;

/*
=================
RNG

xorshift32, seeded only by the host. A host that never calls wc_set_seed
gets a fixed default seed, which is still reproducible.
=================
*/
static uint32_t wc_rngState = 0x6f61656eu;

__attribute__((export_name("wc_set_seed")))
void wc_set_seed( uint32_t seed ) {
	wc_rngState = seed ? seed : 0x6f61656eu;
}

uint32_t WC_Random( void ) {
	uint32_t x = wc_rngState;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	wc_rngState = x;
	return x;
}

qboolean Sys_RandomBytes( byte *string, int len ) {
	int i;
	for ( i = 0; i < len; i++ ) {
		string[i] = (byte)( WC_Random() >> 24 );
	}
	return qtrue;
}

/*
=================
Clock

Engine time is milliseconds since the first rendered frame. Within one
wc_render() call it is constant, which is exactly what a host-paced frame
means: Com_Frame sees one step of delta_ms.
=================
*/
static double wc_timeBase = -1.0;

// profiling clock: cart time (a deterministic run never reads the host
// clock), so CPU timings on a cart only mean frame steps
unsigned int Sys_Microseconds( void ) {
	return (unsigned int)Sys_Milliseconds() * 1000u;
}

int Sys_Milliseconds( void ) {
	if ( wc_timeBase < 0.0 ) {
		wc_timeBase = wc_time.time_ms;
	}
	return (int)( wc_time.time_ms - wc_timeBase );
}

// Wall-clock reads inside libc and the engine (Com_RealTime, timestamps) are
// served from cart time so a deterministic run never touches the host clock.
// The epoch is fixed so file timestamps are stable across replays.
#define WC_EPOCH_SECONDS 1767225600   // 2026-01-01T00:00:00Z

time_t time( time_t *out ) {
	time_t t = (time_t)( WC_EPOCH_SECONDS + (int64_t)( wc_time.time_ms / 1000.0 ) );
	if ( out ) {
		*out = t;
	}
	return t;
}

int clock_gettime( clockid_t clk, struct timespec *ts ) {
	double ms = wc_time.time_ms;
	(void)clk;
	ts->tv_sec = (time_t)( WC_EPOCH_SECONDS + (int64_t)( ms / 1000.0 ) );
	ts->tv_nsec = (long)( ( ms - (double)(int64_t)( ms / 1000.0 ) * 1000.0 ) * 1000000.0 );
	return 0;
}

clock_t clock( void ) {
	return (clock_t)( wc_time.time_ms * ( CLOCKS_PER_SEC / 1000.0 ) );
}

int gettimeofday( struct timeval *tv, void *tz ) {
	double ms = wc_time.time_ms;
	(void)tz;
	if ( tv ) {
		tv->tv_sec = (time_t)( WC_EPOCH_SECONDS + (int64_t)( ms / 1000.0 ) );
		tv->tv_usec = (suseconds_t)( ( ms - (double)(int64_t)( ms / 1000.0 ) * 1000.0 ) * 1000.0 );
	}
	return 0;
}

/*
=================
Paths

Everything lives in the cart's virtual filesystem (wc_vfs.c): the bundled
assets are the install path, the SRAM-backed table is the home path.
=================
*/
static char binaryPath[MAX_OSPATH] = "";
static char installPath[MAX_OSPATH] = "";

void Sys_SetBinaryPath( const char *path ) { Q_strncpyz( binaryPath, path, sizeof( binaryPath ) ); }
char *Sys_BinaryPath( void ) { return binaryPath; }
void Sys_SetDefaultInstallPath( const char *path ) { Q_strncpyz( installPath, path, sizeof( installPath ) ); }
char *Sys_DefaultInstallPath( void ) { return installPath; }
char *Sys_DefaultAppPath( void ) { return binaryPath; }
char *Sys_BinaryPathRelative( const char *relative ) { (void)relative; return NULL; }

char *Sys_DefaultHomeConfigPath( void ) { return "home"; }
char *Sys_DefaultHomeDataPath( void ) { return "home"; }
char *Sys_DefaultHomeStatePath( void ) { return "home"; }
char *Sys_SteamPath( void ) { return ""; }
char *Sys_GogPath( void ) { return ""; }
char *Sys_MicrosoftStorePath( void ) { return ""; }
char *Sys_Cwd( void ) { return "."; }
char *Sys_GetCurrentUser( void ) { return "player"; }

const char *Sys_Basename( char *path ) {
	char *p = strrchr( path, '/' );
	return p ? p + 1 : path;
}

const char *Sys_Dirname( char *path ) {
	static char dir[MAX_OSPATH];
	char *p;
	Q_strncpyz( dir, path, sizeof( dir ) );
	p = strrchr( dir, '/' );
	if ( p ) {
		*p = '\0';
	} else {
		Q_strncpyz( dir, ".", sizeof( dir ) );
	}
	return dir;
}

qboolean Sys_Mkdir( const char *path ) { (void)path; return qtrue; }
FILE *Sys_Mkfifo( const char *ospath ) { (void)ospath; return NULL; }
qboolean Sys_OpenFolderInPlatformFileManager( const char *path ) { (void)path; return qfalse; }
qboolean Sys_OpenFolderInFileManager( const char *path, qboolean create ) { (void)path; (void)create; return qfalse; }
qboolean Sys_SetMaxFileLimit( void ) { return qtrue; }
int Sys_FileTime( char *path ) { (void)path; return -1; }

/*
=================
Process, dialogs, misc
=================
*/
qboolean Sys_LowPhysicalMemory( void ) { return qfalse; }
void Sys_Sleep( int msec ) { (void)msec; }
char *Sys_ConsoleInput( void ) { return NULL; }
char *Sys_GetClipboardData( void ) { return NULL; }
cpuFeatures_t Sys_GetProcessorFeatures( void ) { return (cpuFeatures_t)0; }
void Sys_SetFloatEnv( void ) { }
void Sys_GLimpSafeInit( void ) { }
void Sys_GLimpInit( void ) { }
void Sys_PlatformInit( void ) { }
void Sys_PlatformExit( void ) { }
void Sys_SetEnv( const char *name, const char *value ) { (void)name; (void)value; }
int Sys_PID( void ) { return 1; }
qboolean Sys_PIDIsRunning( int pid ) { return pid == 1; }
void Sys_InitPIDFile( const char *gamedir ) { (void)gamedir; }
void Sys_RemovePIDFile( const char *gamedir ) { (void)gamedir; }
qboolean Sys_DllExtension( const char *name ) { (void)name; return qfalse; }
void Sys_ErrorDialog( const char *error ) { (void)error; }
char *Sys_ParseProtocolUri( const char *uri ) { (void)uri; return NULL; }
void Sys_AnsiColorPrint( const char *msg ) { (void)msg; }

dialogResult_t Sys_Dialog( dialogType_t type, const char *message, const char *title ) {
	(void)type; (void)title;
	Com_Printf( "Sys_Dialog: %s\n", message );
	return DR_OK;
}

// Game modules are always QVMs here: there is no dynamic loader in a cart.
void *Sys_LoadDll( const char *name, qboolean useSystemLib ) { (void)name; (void)useSystemLib; return NULL; }
void Sys_UnloadDll( void *dllHandle ) { (void)dllHandle; }
void *Sys_LoadGameDll( const char *name, vmMainProc *entryPoint, intptr_t (*systemcalls)(intptr_t, ...) ) {
	(void)name; (void)entryPoint; (void)systemcalls;
	return NULL;
}

void CON_Init( void ) { }
void CON_Shutdown( void ) { }
void CON_Print( const char *msg ) { (void)msg; }
char *CON_Input( void ) { return NULL; }

/*
=================
Print, error, quit

A cart cannot exit. Sys_Error and Sys_Quit flush the save region, log, and
unwind to wc_render(); every later frame is a no-op, so the host sees a
halted cart rather than a trap mid-frame.
=================
*/
void Sys_Print( const char *msg ) {
	wc_log( msg, (unsigned int)strlen( msg ) );
}

static void Sys_Halt( void ) Q_NO_RETURN;
static void Sys_Halt( void ) {
	WC_VFS_Flush();
	wc_halted = 1;
	longjmp( wc_haltFrame, 1 );
}

void WC_Debug_SetFatal( const char *text );

void Sys_Error( const char *error, ... ) {
	va_list argptr;
	char    string[1024];

	va_start( argptr, error );
	Q_vsnprintf( string, sizeof( string ), error, argptr );
	va_end( argptr );

	Sys_Print( "Sys_Error: " );
	Sys_Print( string );
	Sys_Print( "\n" );
	WC_Debug_SetFatal( string );	// the host reads it: a halted cart must not look idle
	Sys_Halt();
}

void Sys_Quit( void ) {
	Sys_Print( "Sys_Quit\n" );
	Sys_Halt();
}

void Sys_SigHandler( int signal ) {
	(void)signal;
	Sys_Halt();
}

static void Sys_In_Restart_f( void ) {
	IN_Restart();
}

void Sys_Init( void ) {
	Cmd_AddCommand( "in_restart", Sys_In_Restart_f );
	Cvar_Set( "arch", "wasm32-wasmcart" );
	Cvar_Set( "username", Sys_GetCurrentUser() );
}

/*
=================
wasmcart exports
=================
*/
extern void WC_Debug_Init( void );
extern void WC_Debug_Frame( int frameMsec );
extern void WC_Debug_PollCommand( void );

__attribute__((export_name("wc_get_info")))
wc_info_t *wc_get_info( void ) {
	memset( &wc_info, 0, sizeof( wc_info ) );
	wc_info.version = WC_ABI_VERSION;
	wc_info.width = wc_width;
	wc_info.height = wc_height;
	wc_info.fb_ptr = 0;
	wc_info.audio_ptr = (uint32_t)(uintptr_t)wc_audio_ring;
	wc_info.audio_cap = WC_AUDIO_CAP;
	wc_info.audio_write_ptr = (uint32_t)(uintptr_t)&wc_audio_write_cursor;
	wc_info.audio_sample_rate = WC_AUDIO_RATE;
	wc_info.input_ptr = (uint32_t)(uintptr_t)wc_pads;
	wc_info.save_ptr = (uint32_t)(uintptr_t)WC_VFS_SaveRegion();
	wc_info.save_size = WC_SAVE_SIZE;
	wc_info.time_ptr = (uint32_t)(uintptr_t)&wc_time;
	wc_info.host_info_ptr = (uint32_t)(uintptr_t)&wc_host_info;
	wc_info.pointer_ptr = (uint32_t)(uintptr_t)wc_pointers;
	wc_info.keys_ptr = (uint32_t)(uintptr_t)wc_keys;
	wc_info.wheel_ptr = (uint32_t)(uintptr_t)&wc_wheel;
	wc_info.gpu_api = WC_GPU_API_GLES3;
	wc_info.flags = WC_FLAG_POINTER | WC_FLAG_KEYBOARD | WC_FLAG_DETERMINISTIC | WC_FLAG_DEBUG;
	return &wc_info;
}

__attribute__((export_name("wc_init")))
void wc_init( void ) {
	int w = (int)wc_host_info.preferred_width;
	int h = (int)wc_host_info.preferred_height;

	wc_deterministic = ( wc_host_info.flags & WC_HOST_FLAG_DETERMINISTIC ) != 0;
	// a deterministic run (a test replay) always renders at the fixed
	// default size, whatever the host prefers: captures are comparable
	if ( w > 0 && h > 0 && !wc_deterministic ) {
		wc_width = w > WC_MAX_WIDTH ? WC_MAX_WIDTH : w;
		wc_height = h > WC_MAX_HEIGHT ? WC_MAX_HEIGHT : h;
	}
	wc_info.width = wc_width;
	wc_info.height = wc_height;
}

/*
The command line every cart boot starts from. Content and platform facts
live here; player preferences live in the saved q3config.cfg, which the
engine executes after this and therefore wins for anything archived.
*/
static void WC_BuildCommandLine( char *out, int size ) {
	Com_sprintf( out, size,
		"+set com_basegame baseoa"
		" +set fs_basepath ."
		" +set fs_homepath home"
		" +set com_hunkMegs 256"
		" +set com_zoneMegs 64"
		" +set r_mode -1 +set r_customwidth %d +set r_customheight %d"
		" +set r_fullscreen 0"
		" +set vm_game 1 +set vm_cgame 1 +set vm_ui 1"
		" +set sv_pure 0"
		" +set net_enabled 0"
		" +set s_useOpenAL 0"
		" +set in_joystick 1 +set in_joystickUseAnalog 1"
		" +set com_introplayed 1",
		wc_width, wc_height );

	// boot cvars a harness wrote into console_cmd before the first frame
	// ("set r_picmip 1;set x y"): applied before the renderer starts, so
	// latched cvars take effect at boot
	{
		char boot[256];
		char *p, *semi;

		if ( WC_Debug_TakeBootCommand( boot, sizeof( boot ) ) ) {
			for ( p = boot; p && *p; p = semi ) {
				semi = strchr( p, ';' );
				if ( semi ) {
					*semi++ = 0;
				}
				while ( *p == ' ' ) {
					p++;
				}
				if ( *p ) {
					Q_strcat( out, size, " +" );
					Q_strcat( out, size, p );
				}
			}
		}
	}
}

static void WC_Start( void ) {
	static char cmdline[MAX_STRING_CHARS];

	WC_VFS_Init();
	WC_Debug_Init();

	Sys_SetBinaryPath( "" );
	Sys_SetDefaultInstallPath( "" );

	WC_BuildCommandLine( cmdline, sizeof( cmdline ) );
	Com_Init( cmdline );
	NET_Init();
	// default gamepad bindings: CL_GamepadDefaults (cl_gamepad.c), from Com_Init
}

__attribute__((export_name("wc_render")))
void wc_render( void ) {
	if ( wc_halted ) {
		return;
	}
	if ( setjmp( wc_haltFrame ) ) {
		return;
	}

	if ( !wc_started ) {
		wc_started = 1;
		WC_Start();
		return;
	}

	WC_Debug_PollCommand();
	Com_Frame();
	WC_SND_Frame();
	WC_Debug_Frame( (int)wc_time.delta_ms );
}

__attribute__((export_name("wc_on_suspend")))
void wc_on_suspend( void ) {
	if ( wc_started && !wc_halted ) {
		WC_VFS_Flush();
	}
}
