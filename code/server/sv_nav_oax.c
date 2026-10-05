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
sv_nav_oax.c: the navigation mesh (Recast/Detour) and its game syscalls
(G_OAX_NAV_*, 1090-1099, see qcommon/oax.h); oax_features token "nav".

Bots path on AAS where a map has it. Heightmap terrain (phase 7) is not in
the BSP brushes bspc compiles, so terrain maps ship without AAS and the oax
game module's bots path on this navmesh instead. It is built from the
world's collision geometry (cm_navgeom.c: brushes as faces and as solid
volumes, terrain triangles, tree trunks, collision meshes), once per map:
when the game module commits its authored links (G_OAX_NAV_COMMIT), or on
the first query if it never does. Tiles are built out from the map's spawn
points (nav_oax.cpp), so floors no one can reach cost nothing:

  sv_navmesh -1 (default)  build for maps with OAX_TERRAIN or without an AAS
                           file, and for Assault (g_gametype 14), which the oax
                           game's navmesh bots play even where AAS exists
              0            never
              1            always

The build and every query are functions of the map and the arguments, so
native and wasm servers agree exactly (debug values sv_nav_polys, sv_nav_hash).

Navigation from intent (step 7.5, docs/navigation.md): the game module
reads the map's authored entities (teleporters, jump pads, ladders,
info_oax_route pairs, hazard volumes) and hands them over as off-mesh links
(G_OAX_NAV_ADDLINK) and cost volumes (G_OAX_NAV_ADDAREA); G_OAX_NAV_COMMIT
rebuilds the navmesh with them. sv_navLinks 0 ignores them (a test
control). Debug values sv_nav_links ("connected/submitted"),
sv_nav_areas, sv_nav_tiles / sv_nav_grid_tiles, sv_nav_seeds,
sv_nav_volumes, sv_nav_closed_faces, sv_nav_buried_brushes,
sv_nav_opaque_tops, sv_nav_forbidden_tris, sv_nav_error.
===========================================================================
*/

#include "server.h"
#include "../qcommon/oax.h"
#include "../qcommon/cm_terrain.h"
#include "../qcommon/cm_navgeom.h"
#include "nav_oax.h"

static cvar_t *sv_navmesh;
static cvar_t *sv_navCellSize;
static cvar_t *sv_navLinks;

#define NAV_MAX_LINKS	256
#define NAV_MAX_AREAS	128

static int			navBuildPending;	// the map wants a navmesh that is not built yet
static oaxNavLink_t	navPendingLinks[NAV_MAX_LINKS];
static int			navNumPendingLinks;
static oaxNavArea_t	navPendingAreas[NAV_MAX_AREAS];
static int			navNumPendingAreas;

#define NAV_MAX_POINTS 64

static const float navHalfExtents[3] = { 32.0f, 32.0f, 96.0f };

/*
=================
SV_OAXNavMapLoaded

Called by SV_SpawnServer after the collision map loads.
=================
*/
static int SV_OAXNavBuild( const oaxNavLink_t *links, int numLinks, const oaxNavArea_t *areas, int numAreas );

void SV_OAXNavMapLoaded( const char *mapname ) {
	int want;

	OAXNav_Free();
	navBuildPending = 0;
	navNumPendingLinks = 0;
	navNumPendingAreas = 0;
	Com_DebugSetInt( "sv_nav_polys", 0 );
	Com_DebugSet( "sv_nav_hash", "none" );
	Com_DebugSet( "sv_nav_links", "0/0" );
	Com_DebugSetInt( "sv_nav_areas", 0 );

	want = sv_navmesh->integer;
	if ( want < 0 ) {
		want = CM_OAXNumTerrains() > 0 || FS_ReadFile( va( "maps/%s.aas", mapname ), NULL ) <= 0 ||
			Cvar_VariableIntegerValue( "g_gametype" ) == 14;
	}
	if ( !want ) {
		return;
	}
	// built on first use: an oax game module commits its links a moment
	// after the map starts, and one build of a large map is enough
	navBuildPending = 1;
}

// the deferred map-load build, before any query
static void SV_OAXNavEnsure( void ) {
	if ( navBuildPending ) {
		navBuildPending = 0;
		SV_OAXNavBuild( NULL, 0, NULL, 0 );
	}
}

/*
=================
SV_OAXNavBuild

Builds the navmesh from the world's collision geometry plus the given
links and cost volumes. Returns the polygon count.
=================
*/
#define NAV_MAX_SEEDS 256

/* the map's spawn points (info_player_*, team_CTF_*spawn / *player) from
   the entity string: walkers start there, so the navmesh is built out from
   them (nav_oax.h oaxNavSolids_t seeds) */
static int SV_OAXNavSeeds( float *seeds, int max ) {
	const char *p = CM_EntityString();
	char key[MAX_TOKEN_CHARS], classname[MAX_TOKEN_CHARS];
	float o[3];
	int n = 0, haveOrigin;

	while ( p && n < max ) {
		char *tok = COM_Parse( (char **)&p );
		if ( !tok[0] ) {
			break;
		}
		if ( tok[0] != '{' ) {
			continue;
		}
		classname[0] = 0;
		haveOrigin = 0;
		while ( 1 ) {
			tok = COM_Parse( (char **)&p );
			if ( !tok[0] || tok[0] == '}' ) {
				break;
			}
			Q_strncpyz( key, tok, sizeof( key ) );
			tok = COM_Parse( (char **)&p );
			if ( !Q_stricmp( key, "classname" ) ) {
				Q_strncpyz( classname, tok, sizeof( classname ) );
			} else if ( !Q_stricmp( key, "origin" ) && sscanf( tok, "%f %f %f", &o[0], &o[1], &o[2] ) == 3 ) {
				haveOrigin = 1;
			}
		}
		if ( haveOrigin && ( !Q_stricmpn( classname, "info_player_", 12 ) || !Q_stricmp( classname, "team_CTF_redspawn" ) ||
			!Q_stricmp( classname, "team_CTF_bluespawn" ) || !Q_stricmp( classname, "team_CTF_redplayer" ) ||
			!Q_stricmp( classname, "team_CTF_blueplayer" ) ) ) {
			VectorCopy( o, &seeds[n * 3] );
			n++;
		}
	}
	return n;
}

static int SV_OAXNavBuild( const oaxNavLink_t *links, int numLinks, const oaxNavArea_t *areas, int numAreas ) {
	float seeds[NAV_MAX_SEEDS * 3];
	oaxNavGeometry_t g;
	oaxNavParams_t p;
	oaxNavSolids_t solids;
	char err[128];
	int polys, tiles, gridTiles, solidColumns, opaqueTops;

	if ( !CM_OAXNavGeometry( &g ) ) {
		CM_OAXNavGeometryFree( &g );
		return 0;
	}
	Com_Memset( &p, 0, sizeof( p ) );
	p.cellSize = sv_navCellSize->value > 1.0f ? sv_navCellSize->value : 8.0f;
	p.cellHeight = 4.0f;
	p.agentHeight = 56.0f;		// standing player: DEFAULT_VIEWHEIGHT box, maxs z 32 - mins z -24
	p.agentRadius = 16.0f;		// player box half width 15, plus a unit
	p.agentClimb = 18.0f;		// STEPSIZE
	VectorCopy( g.mins, p.bmin );
	VectorCopy( g.maxs, p.bmax );
	Com_Memset( &solids, 0, sizeof( solids ) );
	solids.numVolumes = g.numVolumes;
	solids.volFirstPlane = g.volFirstPlane;
	solids.volNumPlanes = g.volNumPlanes;
	solids.volBounds = g.volBounds;
	solids.planes = g.planes;
	solids.planeTop = g.planeTop;
	solids.openAt = CM_OAXNavOpenAt;
	solids.seeds = seeds;
	solids.numSeeds = SV_OAXNavSeeds( seeds, NAV_MAX_SEEDS );
	polys = OAXNav_BuildEx( g.verts, g.numVerts, g.tris, g.walkable, g.numTris, &p, links, numLinks, areas, numAreas,
		&solids, err, sizeof( err ) );
	OAXNav_BuildStats( &tiles, &gridTiles, &solidColumns, &opaqueTops );
	Com_Printf( "navmesh: %i triangles and %i solid volumes in, %i polygons in %i of %i tiles, %i bytes, hash %08x, "
		"links %i/%i, cost volumes %i, %i solid columns, %i opaque tops dropped%s%s\n",
		g.numTris, g.numVolumes, polys, tiles, gridTiles, OAXNav_DataSize(), OAXNav_Hash(), OAXNav_LinkCount(), numLinks,
		numAreas, solidColumns, opaqueTops, polys ? "" : ": ", polys ? "" : err );
	{
		int r, fl, rest;
		float bb[6];
		OAXNav_BuildProfile( &r, &fl, &rest, bb );
		Com_Printf( "navmesh: walkable bounds %.0f %.0f %.0f .. %.0f %.0f %.0f; ms rasterize %i, solid fill %i, rest %i\n",
			bb[0], bb[1], bb[2], bb[3], bb[4], bb[5], r, fl, rest );
	}
	Com_DebugSetInt( "sv_nav_tiles", tiles );
	Com_DebugSetInt( "sv_nav_seeds", solids.numSeeds );
	Com_DebugSetInt( "sv_nav_grid_tiles", gridTiles );
	Com_DebugSetInt( "sv_nav_volumes", g.numVolumes );
	Com_DebugSetInt( "sv_nav_closed_faces", g.closedFaces );
	Com_DebugSetInt( "sv_nav_buried_brushes", g.buriedBrushes );
	Com_DebugSetInt( "sv_nav_opaque_tops", opaqueTops );
	Com_DebugSetInt( "sv_nav_forbidden_tris", OAXNav_ForbiddenTris() );
	Com_DebugSet( "sv_nav_error", polys ? "-" : err );
	Com_DebugSetInt( "sv_nav_tris", g.numTris );
	Com_DebugSetInt( "sv_nav_polys", polys );
	Com_DebugSet( "sv_nav_hash", polys ? va( "%08x", OAXNav_Hash() ) : "none" );
	Com_DebugSet( "sv_nav_links", va( "%i/%i", OAXNav_LinkCount(), numLinks ) );
	{
		// links that did not reach the walkable mesh at both ends (an end too far from a polygon)
		char open[256];
		int i, len = 0;
		open[0] = 0;
		for ( i = 0; i < numLinks && len < (int)sizeof( open ) - 8; i++ ) {
			if ( !OAXNav_LinkConnected( i ) ) {
				Com_sprintf( open + len, sizeof( open ) - len, "%s%i", len ? " " : "", i );
				len = strlen( open );
			}
		}
		Com_DebugSet( "sv_nav_links_open", len ? open : "-" );
		if ( len ) {
			Com_Printf( "navmesh: links not connected at both ends: %s\n", open );
		}
	}
	Com_DebugSetInt( "sv_nav_areas", numAreas );
	CM_OAXNavGeometryFree( &g );
	return polys;
}

static void SV_NavPrintPath( const float *pts, const int *links, int n, int flags ) {
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
	// the links the path takes, in order ("-" for none)
	buf[0] = 0;
	len = 0;
	for ( i = 0; i < n && len < (int)sizeof( buf ) - 16; i++ ) {
		if ( links[i] >= 0 ) {
			Com_sprintf( buf + len, sizeof( buf ) - len, "%s%i", len ? " " : "", links[i] );
			len = strlen( buf );
		}
	}
	Com_DebugSet( "nav_path_links", len ? buf : "-" );
}

/*
=================
SV_NavPath_f

nav_path sx sy sz gx gy gz [include [exclude]]: prints the straight path
and publishes it as the debug value "nav_path" ("count flags x y z ..."),
and the links it takes as "nav_path_links". include/exclude are polygon
and link flags (nav_oax.h OAXNAV_*), default: walking, hazards and every
link kind that needs no server rule.
=================
*/
static void SV_NavPath_f( void ) {
	float s[3], g[3], pts[NAV_MAX_POINTS * 3];
	int i, n, flags, links[NAV_MAX_POINTS];
	int include = OAXNAV_DEFAULT_INCLUDE, exclude = 0;

	if ( Cmd_Argc() != 7 && Cmd_Argc() != 8 && Cmd_Argc() != 9 ) {
		Com_Printf( "usage: nav_path sx sy sz gx gy gz [include [exclude]]\n" );
		return;
	}
	for ( i = 0; i < 3; i++ ) {
		s[i] = atof( Cmd_Argv( 1 + i ) );
		g[i] = atof( Cmd_Argv( 4 + i ) );
	}
	if ( Cmd_Argc() >= 8 ) {
		include = atoi( Cmd_Argv( 7 ) );
	}
	if ( Cmd_Argc() >= 9 ) {
		exclude = atoi( Cmd_Argv( 8 ) );
	}
	SV_OAXNavEnsure();
	n = OAXNav_FindPathEx( s, g, navHalfExtents, pts, links, NAV_MAX_POINTS, &flags, include, exclude );
	SV_NavPrintPath( pts, links, n, flags );
}

static qboolean SV_OAXNavCalls( intptr_t *args, intptr_t *ret ) {
	switch ( args[0] ) {
	case G_OAX_NAV_STATUS:
		// a pending build answers 1: there will be a navmesh
		*ret = navBuildPending ? 1 : OAXNav_PolyCount();
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
		SV_OAXNavEnsure();
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
		SV_OAXNavEnsure();
		*ret = OAXNav_Nearest( VMA( 1 ), VMA( 2 ), VMA( 3 ) );
		return qtrue;

	case G_OAX_NAV_RANDOMPOINT:
		VM_CheckBlock( args[2], sizeof( vec3_t ), "NAVRANDOM" );
		SV_OAXNavEnsure();
		*ret = OAXNav_RandomPoint( (unsigned)args[1], VMA( 2 ) );
		return qtrue;

	case G_OAX_NAV_ADDLINK: {
		oaxNavLink_t *l;
		int kind = args[3];
		VM_CheckBlock( args[1], sizeof( vec3_t ), "NAVLINK" );
		VM_CheckBlock( args[2], sizeof( vec3_t ), "NAVLINK" );
		*ret = -1;
		if ( navNumPendingLinks >= NAV_MAX_LINKS || kind < OAXNAV_LINK_TELEPORT || kind > 0xffff ) {
			return qtrue;
		}
		l = &navPendingLinks[navNumPendingLinks];
		Com_Memset( l, 0, sizeof( *l ) );
		VectorCopy( (float *)VMA( 1 ), l->start );
		VectorCopy( (float *)VMA( 2 ), l->end );
		l->kind = (unsigned short)kind;
		l->radius = VMF( 4 );
		l->bidir = args[5] ? 1 : 0;
		*ret = navNumPendingLinks++;
		return qtrue;
	}

	case G_OAX_NAV_ADDAREA: {
		oaxNavArea_t *a;
		VM_CheckBlock( args[1], sizeof( vec3_t ), "NAVAREA" );
		VM_CheckBlock( args[2], sizeof( vec3_t ), "NAVAREA" );
		*ret = -1;
		if ( navNumPendingAreas >= NAV_MAX_AREAS ) {
			return qtrue;
		}
		a = &navPendingAreas[navNumPendingAreas];
		VectorCopy( (float *)VMA( 1 ), a->mins );
		VectorCopy( (float *)VMA( 2 ), a->maxs );
		a->cost = VMF( 3 );
		*ret = navNumPendingAreas++;
		return qtrue;
	}

	case G_OAX_NAV_COMMIT:
		// the one build of a map with authored intent; with none (or
		// sv_navLinks 0) the plain build, if it is still pending, or the
		// existing mesh. A built mesh is rebuilt only for new intent.
		if ( ( navNumPendingLinks || navNumPendingAreas ) && sv_navLinks->integer &&
			( navBuildPending || OAXNav_PolyCount() > 0 ) ) {
			navBuildPending = 0;
			*ret = SV_OAXNavBuild( navPendingLinks, navNumPendingLinks, navPendingAreas, navNumPendingAreas );
		} else {
			SV_OAXNavEnsure();
			*ret = OAXNav_PolyCount();
		}
		navNumPendingLinks = 0;
		navNumPendingAreas = 0;
		return qtrue;

	case G_OAX_NAV_FINDPATHEX: {
		int max = args[5], flags = 0, links[NAV_MAX_POINTS];
		float pts[NAV_MAX_POINTS * 3];
		if ( max > NAV_MAX_POINTS ) {
			max = NAV_MAX_POINTS;
		}
		if ( max <= 0 ) {
			*ret = 0;
			return qtrue;
		}
		VM_CheckBlock( args[1], sizeof( vec3_t ), "NAVPATHEX" );
		VM_CheckBlock( args[2], sizeof( vec3_t ), "NAVPATHEX" );
		VM_CheckBlock( args[3], max * 3 * sizeof( float ), "NAVPATHEX" );
		VM_CheckBlock( args[4], max * sizeof( int ), "NAVPATHEX" );
		SV_OAXNavEnsure();
		*ret = OAXNav_FindPathEx( VMA( 1 ), VMA( 2 ), navHalfExtents, pts, links, max, &flags, args[7], args[8] );
		Com_Memcpy( VMA( 3 ), pts, *ret * 3 * sizeof( float ) );
		Com_Memcpy( VMA( 4 ), links, *ret * sizeof( int ) );
		if ( args[6] ) {
			VM_CheckBlock( args[6], sizeof( int ), "NAVFLAGS" );
			*(int *)VMA( 6 ) = flags;
		}
		return qtrue;
	}
	}
	return qfalse;
}

void SV_OAXNavInit( void ) {
	sv_navmesh = Cvar_Get( "sv_navmesh", "-1", 0 );
	Cvar_SetDescription( sv_navmesh, "Navigation mesh for bots: -1 for maps with terrain or without AAS and for Assault (g_gametype 14), 0 never, 1 always." );
	sv_navCellSize = Cvar_Get( "sv_navCellSize", "8", 0 );
	Cvar_SetDescription( sv_navCellSize, "Navigation mesh voxel size across (world units)." );
	sv_navLinks = Cvar_Get( "sv_navLinks", "1", 0 );
	Cvar_SetDescription( sv_navLinks, "Build the navmesh with the map's authored links and hazard costs (teleporters, jump pads, ladders, routes). 0: plain walkable mesh (a test control)." );
	// the game's random seed for replayable matches (sv_game.c); -1 = the clock
	Cvar_SetDescription( Cvar_Get( "sv_gameSeed", "-1", 0 ), "Fixed random seed for the game module (-1: from the clock). For replayable test matches." );
	Cmd_AddCommand( "nav_path", SV_NavPath_f );
	SV_OAXRegisterGameHandler( SV_OAXNavCalls );
	OAX_AddFeature( "nav" );
}
