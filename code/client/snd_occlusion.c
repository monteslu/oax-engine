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
snd_occlusion.c: sounds behind closed area portals (oax "portal" feature,
design 2.8).

New code for the oax engine. DOOM-3 propagated sound through its portal
graph and stopped it at closed portals; here a sound whose area is not
connected to the listener's (a closed door's area portal between them, as
the server's snapshot area mask says) plays at snd_occlusionScale of its
volume.

  snd_occlusion       -1 (default): on for oax maps (an OAX_MANIFEST BSPX
                      lump), off for stock maps; 0 off; 1 on
  snd_occlusionScale  volume factor for occluded sounds (default 0.2)
  soundocclusion x y z  publishes debug value s_occlusion: the factor a
                      sound at that point gets right now

The DMA mixer (every cart, native with s_useOpenAL 0) applies it.
===========================================================================
*/

#include "client.h"
#include "snd_local.h"
#include "../qcommon/oax.h"

static cvar_t	*snd_occlusion;
static cvar_t	*snd_occlusionScale;
static int		oaxMapChecked = -1;		// cl.serverId the check below is for
static qboolean	oaxMap;

static qboolean S_OcclusionOn( void ) {
	if ( !snd_occlusion || !snd_occlusion->integer ) {
		return qfalse;
	}
	if ( snd_occlusion->integer > 0 ) {
		return qtrue;
	}
	// auto: oax maps only, so stock maps sound as they always did
	if ( oaxMapChecked != cl.serverId ) {
		oaxMapChecked = cl.serverId;
		oaxMap = BSPX_ReadCurrentMap( "OAX_MANIFEST", NULL, 0 ) >= 0;
	}
	return oaxMap;
}

/*
=================
S_OcclusionScale

1 for a sound the listener's area reaches, snd_occlusionScale for one
behind a closed area portal.
=================
*/
float S_OcclusionScale( const vec3_t origin ) {
	int area;

	if ( clc.state != CA_ACTIVE || !cl.snap.valid || !S_OcclusionOn() ) {
		return 1.0f;
	}
	area = CM_LeafArea( CM_PointLeafnum( origin ) );
	if ( area < 0 || area >= MAX_MAP_AREA_BYTES * 8 ) {
		return 1.0f;
	}
	// the snapshot's area mask has a bit set for every area the client's
	// area is NOT connected to
	if ( cl.snap.areamask[area >> 3] & ( 1 << ( area & 7 ) ) ) {
		return snd_occlusionScale->value;
	}
	return 1.0f;
}

static void S_SoundOcclusion_f( void ) {
	vec3_t p;

	if ( Cmd_Argc() < 4 ) {
		Com_Printf( "usage: soundocclusion <x> <y> <z>\n" );
		return;
	}
	p[0] = atof( Cmd_Argv( 1 ) );
	p[1] = atof( Cmd_Argv( 2 ) );
	p[2] = atof( Cmd_Argv( 3 ) );
	Com_DebugSetFloat( "s_occlusion", S_OcclusionScale( p ) );
	Com_Printf( "occlusion at %g %g %g: %g\n", p[0], p[1], p[2], S_OcclusionScale( p ) );
}

void S_OcclusionInit( void ) {
	snd_occlusion = Cvar_Get( "snd_occlusion", "-1", CVAR_ARCHIVE );
	Cvar_SetDescription( snd_occlusion, "Sounds behind closed area portals play quieter: -1 on oax maps only, 0 off, 1 on." );
	snd_occlusionScale = Cvar_Get( "snd_occlusionScale", "0.2", CVAR_ARCHIVE );
	Cvar_SetDescription( snd_occlusionScale, "Volume factor for sounds behind closed area portals." );
	Cmd_AddCommand( "soundocclusion", S_SoundOcclusion_f );
}
