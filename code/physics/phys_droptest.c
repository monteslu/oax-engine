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
phys_droptest.c: `phys_droptest`, a Box3D scene the engine builds by itself
on the loaded map, for tests of the map's static collision (step 7.5: a
surface-world map's hull, OAX_COLLISION meshes).

  phys_droptest <tag> <ticks> x y z [x y z ...]

makes an engine-owned world with the map's solid and player-clip brushes,
patches and collision meshes as static collision (PHYS_WORLD_ADD_BSP, the
same call gamecode makes), drops a 16-unit box at each point (each turned
a little differently, so they land on an edge and settle), steps it
`ticks` 16 ms ticks, and publishes debug values:

  phys_drop_<tag>_hash       the world hash after the last tick
  phys_drop_<tag>_asleep     bodies at rest (asleep)
  phys_drop_<tag>_<i>        body i: x y z (%.9g) and 1 asleep / 0 awake
  phys_drop_<tag>_done       1

then destroys the world. The result is a function of the map and the
arguments, so native and wasm builds must agree exactly.
===========================================================================
*/

#include "phys_local.h"

#define DROP_MAX_BODIES	64

static void Phys_DropTest_f( void ) {
	oaxPhysWorldDef_t wd;
	char tag[32];
	int world, ticks, n, i, bodies[DROP_MAX_BODIES], asleep = 0;

	if ( Cmd_Argc() < 6 || ( Cmd_Argc() - 3 ) % 3 ) {
		Com_Printf( "usage: phys_droptest <tag> <ticks> x y z [x y z ...]\n" );
		return;
	}
	if ( !com_sv_running || !com_sv_running->integer ) {
		Com_Printf( "phys_droptest: no map loaded\n" );
		return;
	}
	Q_strncpyz( tag, Cmd_Argv( 1 ), sizeof( tag ) );
	ticks = atoi( Cmd_Argv( 2 ) );
	if ( ticks < 1 ) ticks = 1;
	if ( ticks > 3000 ) ticks = 3000;
	n = ( Cmd_Argc() - 3 ) / 3;
	if ( n > DROP_MAX_BODIES ) n = DROP_MAX_BODIES;

	Com_Memset( &wd, 0, sizeof( wd ) );
	wd.gravity[2] = -800.0f;
	wd.workerCount = 1;
	wd.tickMsec = 16;
	wd.substeps = 4;
	world = Phys_WorldCreate( PHYS_OWNER_ENGINE, &wd );
	if ( !world ) {
		Com_Printf( "phys_droptest: no world\n" );
		return;
	}
	Phys_AddBSP( PHYS_OWNER_ENGINE, world, CONTENTS_SOLID | CONTENTS_PLAYERCLIP, PHYS_BSP_PATCHES, NULL );
	for ( i = 0; i < n; i++ ) {
		oaxPhysBodyDef_t bd;
		oaxPhysShapeDef_t sd;
		vec3_t angles, axis[3];
		float yaw, half;

		Com_Memset( &bd, 0, sizeof( bd ) );
		bd.type = PHYS_BODY_DYNAMIC;
		bd.origin[0] = atof( Cmd_Argv( 3 + i * 3 ) );
		bd.origin[1] = atof( Cmd_Argv( 4 + i * 3 ) );
		bd.origin[2] = atof( Cmd_Argv( 5 + i * 3 ) );
		bd.gravityScale = 1.0f;
		// a small tilt about two axes: quaternion from half angles (exact
		// float math: Q_detSin / Q_detCos)
		yaw = (float)( ( i * 37 ) % 90 );
		VectorSet( angles, 12.0f, yaw, 7.0f );
		AnglesToAxis( angles, axis );
		{
			float m00 = axis[0][0], m01 = axis[1][0], m02 = axis[2][0];
			float m10 = axis[0][1], m11 = axis[1][1], m12 = axis[2][1];
			float m20 = axis[0][2], m21 = axis[1][2], m22 = axis[2][2];
			float t = m00 + m11 + m22, s;

			if ( t > 0.0f ) {
				s = 0.5f / sqrtf( t + 1.0f );
				bd.quat[3] = 0.25f / s;
				bd.quat[0] = ( m21 - m12 ) * s;
				bd.quat[1] = ( m02 - m20 ) * s;
				bd.quat[2] = ( m10 - m01 ) * s;
			} else {
				bd.quat[3] = 1.0f;
			}
		}
		bodies[i] = Phys_BodyCreate( PHYS_OWNER_ENGINE, world, &bd );
		Com_Memset( &sd, 0, sizeof( sd ) );
		sd.type = PHYS_SHAPE_BOX;
		half = 8.0f;
		sd.params[0] = sd.params[1] = sd.params[2] = half;
		sd.quat[3] = 1.0f;
		sd.density = 400.0f;
		sd.friction = 0.6f;
		Phys_BodyAddShape( PHYS_OWNER_ENGINE, bodies[i], &sd, NULL, 0, NULL, 0 );
	}
	for ( i = 0; i < ticks; i++ ) {
		Phys_WorldStep( PHYS_OWNER_ENGINE, world, -1 );
	}
	for ( i = 0; i < n; i++ ) {
		oaxPhysBodyState_t st;
		qboolean sleeping;

		if ( !Phys_BodyState( PHYS_OWNER_ENGINE, bodies[i], &st ) ) {
			Com_DebugSet( va( "phys_drop_%s_%d", tag, i ), "none" );
			continue;
		}
		sleeping = !( st.flags & PHYS_STATE_AWAKE );
		asleep += sleeping;
		Com_DebugSet( va( "phys_drop_%s_%d", tag, i ), va( "%.9g %.9g %.9g %d", st.origin[0], st.origin[1], st.origin[2], sleeping ) );
	}
	Com_DebugSet( va( "phys_drop_%s_hash", tag ), va( "%08x", Phys_WorldHash( PHYS_OWNER_ENGINE, world ) ) );
	Com_DebugSetInt( va( "phys_drop_%s_asleep", tag ), asleep );
	Com_DebugSetInt( va( "phys_drop_%s_done", tag ), 1 );
	Com_Printf( "phys_droptest %s: %d bodies, %d ticks, %d asleep, hash %08x\n", tag, n, ticks, asleep, Phys_WorldHash( PHYS_OWNER_ENGINE, world ) );
	Phys_WorldDestroy( PHYS_OWNER_ENGINE, world );
}

void Phys_DropTestInit( void ) {
	static qboolean done;

	if ( !done ) {
		Cmd_AddCommand( "phys_droptest", Phys_DropTest_f );
		done = qtrue;
	}
}
