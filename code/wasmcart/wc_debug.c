/*
===========================================================================
wasmcart platform backend: named debug state (WC_FLAG_DEBUG).

The host reads this table only when a harness asks ("read player_origin"),
never per frame. Values are refreshed once per frame from the client's
latest snapshot and predicted state, so a test sees what the player sees.
===========================================================================
*/

#include <string.h>

#include "../client/client.h"
#include "wc_local.h"
#include "../qcommon/oax.h"

typedef struct {
	uint32_t name_ptr;
	uint32_t value_ptr;
	uint8_t  type;
	uint8_t  _pad[3];
	uint32_t len;
} wcDebugField_t;

#define WC_DBG_I32   5
#define WC_DBG_F32   6
#define WC_DBG_BYTES 8

static struct {
	float   origin[3];
	float   velocity[3];
	float   viewangles[3];
	int32_t groundEntity;       // -1 = in the air (ENTITYNUM_NONE)
	int32_t pmFlags;
	int32_t pmType;
	int32_t commandTime;
	int32_t serverTime;
	int32_t snapEntities;       // entities in the last snapshot
	int32_t serverEntities;     // entities the server has active
	int32_t connState;          // connstate_t
	int32_t frameMsec;
	int32_t frameCount;
	int32_t health;
	int32_t weapon;
	int32_t clRealtime;         // cls.realtime
	int32_t svsTime;            // svs.time (-1 = no local server)
	int32_t padStartTime;       // command time the last pad script started (-1 = none)
	char    mapname[64];
	char    command[256];       // written by a harness; executed at the next frame
	char    values[65536];      // named debug values, "name value\n" lines (Com_DebugSet)
} dbg;

// Per-frame movement trace: one row per rendered frame while a player state
// exists. A harness reads trace_count before and after a script and takes
// the rows in between (ring index = count % WC_TRACE_ROWS).
#define WC_TRACE_ROWS 4096
#define WC_TRACE_COLS 10   // frame, origin[3], velocity[3], ground, commandTime, yaw
static float   dbgTrace[WC_TRACE_ROWS * WC_TRACE_COLS];
static int32_t dbgTraceCount;

#define FIELD( n, v, t, l ) { (uint32_t)(uintptr_t)( n ), (uint32_t)(uintptr_t)( v ), ( t ), { 0, 0, 0 }, ( l ) }

static wcDebugField_t dbgTable[] = {
	FIELD( "player_origin", dbg.origin, WC_DBG_F32, 3 ),
	FIELD( "player_velocity", dbg.velocity, WC_DBG_F32, 3 ),
	FIELD( "player_viewangles", dbg.viewangles, WC_DBG_F32, 3 ),
	FIELD( "player_ground_entity", &dbg.groundEntity, WC_DBG_I32, 1 ),
	FIELD( "player_pm_flags", &dbg.pmFlags, WC_DBG_I32, 1 ),
	FIELD( "player_pm_type", &dbg.pmType, WC_DBG_I32, 1 ),
	FIELD( "player_health", &dbg.health, WC_DBG_I32, 1 ),
	FIELD( "player_weapon", &dbg.weapon, WC_DBG_I32, 1 ),
	FIELD( "command_time", &dbg.commandTime, WC_DBG_I32, 1 ),
	FIELD( "server_time", &dbg.serverTime, WC_DBG_I32, 1 ),
	FIELD( "snapshot_entities", &dbg.snapEntities, WC_DBG_I32, 1 ),
	FIELD( "server_entities", &dbg.serverEntities, WC_DBG_I32, 1 ),
	FIELD( "conn_state", &dbg.connState, WC_DBG_I32, 1 ),
	FIELD( "cl_realtime", &dbg.clRealtime, WC_DBG_I32, 1 ),
	FIELD( "svs_time", &dbg.svsTime, WC_DBG_I32, 1 ),
	FIELD( "pad_start_time", &dbg.padStartTime, WC_DBG_I32, 1 ),
	FIELD( "frame_msec", &dbg.frameMsec, WC_DBG_I32, 1 ),
	FIELD( "frame_count", &dbg.frameCount, WC_DBG_I32, 1 ),
	FIELD( "mapname", dbg.mapname, WC_DBG_BYTES, sizeof( dbg.mapname ) ),
	FIELD( "console_cmd", dbg.command, WC_DBG_BYTES, sizeof( dbg.command ) ),
	FIELD( "debug_values", dbg.values, WC_DBG_BYTES, sizeof( dbg.values ) ),
	FIELD( "trace_count", &dbgTraceCount, WC_DBG_I32, 1 ),
	FIELD( "trace", dbgTrace, WC_DBG_F32, WC_TRACE_ROWS * WC_TRACE_COLS ),
	{ 0, 0, 0, { 0, 0, 0 }, 0 }
};

__attribute__((export_name("wc_debug_state")))
wcDebugField_t *wc_debug_state( void ) {
	return dbgTable;
}

const playerState_t *WC_Debug_ServerPlayerState( int *serverTime, int *activeEntities );
int WC_Debug_ServerStaticTime( void );
extern int cl_padScriptStartTime;

void WC_Debug_Init( void ) {
	memset( &dbg, 0, sizeof( dbg ) );
	dbg.groundEntity = -1;
}

/*
===============
WC_Debug_PollCommand

A test harness drives the engine by writing a console command into the
console_cmd field (NUL-terminated); it runs at the start of the next frame,
exactly as if typed, and the field is cleared to show it was taken.
===============
*/
void WC_Debug_PollCommand( void ) {
	if ( !dbg.command[0] ) {
		return;
	}
	dbg.command[sizeof( dbg.command ) - 1] = '\0';
	Cbuf_AddText( dbg.command );
	Cbuf_AddText( "\n" );
	memset( dbg.command, 0, sizeof( dbg.command ) );
}

/*
===============
WC_Debug_Frame

Called after each Com_Frame. Reads the server's copy of client 0's player
state when a local server is running (authoritative, and identical on every
build), and the client's snapshot otherwise.
===============
*/
void WC_Debug_Frame( int frameMsec ) {
	const playerState_t *ps = NULL;

	dbg.frameMsec = frameMsec;
	dbg.frameCount++;
	dbg.connState = clc.state;
	dbg.clRealtime = cls.realtime;
	dbg.svsTime = WC_Debug_ServerStaticTime();
	dbg.padStartTime = cl_padScriptStartTime;
	Com_DebugValuesText( dbg.values, sizeof( dbg.values ) );

	// the local server's copy is authoritative; fall back to the snapshot
	ps = WC_Debug_ServerPlayerState( &dbg.serverTime, &dbg.serverEntities );
	if ( !ps && cl.snap.valid ) {
		ps = &cl.snap.ps;
		dbg.serverTime = cl.snap.serverTime;
	}

	dbg.snapEntities = cl.snap.valid ? cl.snap.numEntities : 0;

	if ( ps ) {
		VectorCopy( ps->origin, dbg.origin );
		VectorCopy( ps->velocity, dbg.velocity );
		VectorCopy( ps->viewangles, dbg.viewangles );
		dbg.groundEntity = ps->groundEntityNum == ENTITYNUM_NONE ? -1 : ps->groundEntityNum;
		dbg.pmFlags = ps->pm_flags;
		dbg.pmType = ps->pm_type;
		dbg.commandTime = ps->commandTime;
		dbg.health = ps->stats[STAT_HEALTH];
		dbg.weapon = ps->weapon;

		{
			float *row = &dbgTrace[( dbgTraceCount % WC_TRACE_ROWS ) * WC_TRACE_COLS];
			row[0] = (float)dbg.frameCount;
			row[1] = ps->origin[0];
			row[2] = ps->origin[1];
			row[3] = ps->origin[2];
			row[4] = ps->velocity[0];
			row[5] = ps->velocity[1];
			row[6] = ps->velocity[2];
			row[7] = (float)dbg.groundEntity;
			row[8] = (float)ps->commandTime;
			row[9] = ps->viewangles[YAW];
			dbgTraceCount++;
		}
	}

	Q_strncpyz( dbg.mapname, Cvar_VariableString( "mapname" ), sizeof( dbg.mapname ) );
}
