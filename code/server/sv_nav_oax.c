/*
===========================================================================
sv_nav_oax.c: the navigation mesh (Recast/Detour) and its game syscalls
(G_OAX_NAV_*, 1090-1099, see qcommon/oax.h); oax_features token "nav".

Bots path on AAS where a map has it. Heightmap terrain (phase 7) is not in
the BSP brushes bspc compiles, so terrain maps ship without AAS and the oax
game module's bots path on this navmesh instead. It is built when the
server loads a map, from the world's collision geometry (cm_navgeom.c:
brushes, terrain triangles, tree trunks):

  sv_navmesh -1 (default)  build for maps with OAX_TERRAIN or without an AAS file
              0            never
              1            always

The build and every query are functions of the map and the arguments, so
native and wasm servers agree exactly (debug values sv_nav_polys, sv_nav_hash).
===========================================================================
*/

#include "server.h"
#include "../qcommon/oax.h"
#include "../qcommon/cm_terrain.h"
#include "../qcommon/cm_navgeom.h"
#include "nav_oax.h"

static cvar_t *sv_navmesh;
static cvar_t *sv_navCellSize;

#define NAV_MAX_POINTS 64

static const float navHalfExtents[3] = { 32.0f, 32.0f, 96.0f };

/*
=================
SV_OAXNavMapLoaded

Called by SV_SpawnServer after the collision map loads.
=================
*/
void SV_OAXNavMapLoaded( const char *mapname ) {
	oaxNavGeometry_t g;
	oaxNavParams_t p;
	char err[128];
	int polys, want;

	OAXNav_Free();
	Com_DebugSetInt( "sv_nav_polys", 0 );
	Com_DebugSet( "sv_nav_hash", "none" );

	want = sv_navmesh->integer;
	if ( want < 0 ) {
		want = CM_OAXNumTerrains() > 0 || FS_ReadFile( va( "maps/%s.aas", mapname ), NULL ) <= 0;
	}
	if ( !want ) {
		return;
	}
	if ( !CM_OAXNavGeometry( &g ) ) {
		CM_OAXNavGeometryFree( &g );
		return;
	}
	Com_Memset( &p, 0, sizeof( p ) );
	p.cellSize = sv_navCellSize->value > 1.0f ? sv_navCellSize->value : 8.0f;
	p.cellHeight = 4.0f;
	p.agentHeight = 56.0f;		// standing player: DEFAULT_VIEWHEIGHT box, maxs z 32 - mins z -24
	p.agentRadius = 16.0f;		// player box half width 15, plus a unit
	p.agentClimb = 18.0f;		// STEPSIZE
	VectorCopy( g.mins, p.bmin );
	VectorCopy( g.maxs, p.bmax );
	polys = OAXNav_Build( g.verts, g.numVerts, g.tris, g.walkable, g.numTris, &p, err, sizeof( err ) );
	Com_Printf( "navmesh: %i triangles in, %i polygons, %i bytes, hash %08x%s%s\n", g.numTris, polys,
		OAXNav_DataSize(), OAXNav_Hash(), polys ? "" : ": ", polys ? "" : err );
	Com_DebugSetInt( "sv_nav_tris", g.numTris );
	Com_DebugSetInt( "sv_nav_polys", polys );
	Com_DebugSet( "sv_nav_hash", polys ? va( "%08x", OAXNav_Hash() ) : "none" );
	CM_OAXNavGeometryFree( &g );
}

static void SV_NavPrintPath( const float *pts, int n, int flags ) {
	char buf[1024];
	int i, len = 0;

	buf[0] = 0;
	Com_sprintf( buf, sizeof( buf ), "%i %i", n, flags );
	len = strlen( buf );
	for ( i = 0; i < n && len < (int)sizeof( buf ) - 48; i++ ) {
		Com_sprintf( buf + len, sizeof( buf ) - len, " %.1f %.1f %.1f", pts[i * 3], pts[i * 3 + 1], pts[i * 3 + 2] );
		len = strlen( buf );
	}
	Com_Printf( "nav_path: %s\n", buf );
	Com_DebugSet( "nav_path", buf );
}

/*
=================
SV_NavPath_f

nav_path sx sy sz gx gy gz: prints the straight path and publishes it as
the debug value "nav_path" ("count flags x y z ...").
=================
*/
static void SV_NavPath_f( void ) {
	float s[3], g[3], pts[NAV_MAX_POINTS * 3];
	int i, n, flags;

	if ( Cmd_Argc() != 7 ) {
		Com_Printf( "usage: nav_path sx sy sz gx gy gz\n" );
		return;
	}
	for ( i = 0; i < 3; i++ ) {
		s[i] = atof( Cmd_Argv( 1 + i ) );
		g[i] = atof( Cmd_Argv( 4 + i ) );
	}
	n = OAXNav_FindPath( s, g, navHalfExtents, pts, NAV_MAX_POINTS, &flags );
	SV_NavPrintPath( pts, n, flags );
}

static qboolean SV_OAXNavCalls( intptr_t *args, intptr_t *ret ) {
	switch ( args[0] ) {
	case G_OAX_NAV_STATUS:
		*ret = OAXNav_PolyCount();
		return qtrue;

	case G_OAX_NAV_FINDPATH: {
		int max = args[4], flags = 0;
		float pts[NAV_MAX_POINTS * 3];
		if ( max > NAV_MAX_POINTS ) {
			max = NAV_MAX_POINTS;
		}
		if ( max <= 0 ) {
			*ret = 0;
			return qtrue;
		}
		VM_CheckBlock( args[1], sizeof( vec3_t ), "NAVPATH" );
		VM_CheckBlock( args[2], sizeof( vec3_t ), "NAVPATH" );
		VM_CheckBlock( args[3], max * 3 * sizeof( float ), "NAVPATH" );
		*ret = OAXNav_FindPath( VMA( 1 ), VMA( 2 ), navHalfExtents, pts, max, &flags );
		Com_Memcpy( VMA( 3 ), pts, *ret * 3 * sizeof( float ) );
		if ( args[5] ) {
			VM_CheckBlock( args[5], sizeof( int ), "NAVFLAGS" );
			*(int *)VMA( 5 ) = flags;
		}
		return qtrue;
	}

	case G_OAX_NAV_NEAREST:
		VM_CheckBlock( args[1], sizeof( vec3_t ), "NAVNEAREST" );
		VM_CheckBlock( args[2], sizeof( vec3_t ), "NAVNEAREST" );
		VM_CheckBlock( args[3], sizeof( vec3_t ), "NAVNEAREST" );
		*ret = OAXNav_Nearest( VMA( 1 ), VMA( 2 ), VMA( 3 ) );
		return qtrue;

	case G_OAX_NAV_RANDOMPOINT:
		VM_CheckBlock( args[2], sizeof( vec3_t ), "NAVRANDOM" );
		*ret = OAXNav_RandomPoint( (unsigned)args[1], VMA( 2 ) );
		return qtrue;
	}
	return qfalse;
}

void SV_OAXNavInit( void ) {
	sv_navmesh = Cvar_Get( "sv_navmesh", "-1", 0 );
	Cvar_SetDescription( sv_navmesh, "Navigation mesh for bots: -1 for maps with terrain or without AAS, 0 never, 1 always." );
	sv_navCellSize = Cvar_Get( "sv_navCellSize", "8", 0 );
	Cvar_SetDescription( sv_navCellSize, "Navigation mesh voxel size across (world units)." );
	// the game's random seed for replayable matches (sv_game.c); -1 = the clock
	Cvar_SetDescription( Cvar_Get( "sv_gameSeed", "-1", 0 ), "Fixed random seed for the game module (-1: from the clock). For replayable test matches." );
	Cmd_AddCommand( "nav_path", SV_NavPath_f );
	SV_OAXRegisterGameHandler( SV_OAXNavCalls );
	OAX_AddFeature( "nav" );
}
