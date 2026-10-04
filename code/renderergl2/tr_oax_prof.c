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
tr_oax_prof.c: the renderer's frame profiler (r_oaxProfile 1, or the
client's cl_oaxPerfHud / oaxprof, which turn it on).

- GPU time per zone: the backend marks where each kind of work starts
  (RB_OAXProfZone); every zone change ends one GL_TIME_ELAPSED query and
  begins the next, so the zones tile the frame with no nesting. Whole views
  of a kind count as one zone (sun and light shadow maps, the water
  reflection, sky portals and cubemaps). Results are read back
  OAX_PROF_FRAMES frames later without waiting; a frame whose queries are
  not ready yet is skipped, never stalled on.
- CPU time: the front end (RE_RenderScene: culling and sorting), the back
  end (command execution: GL calls), and the wait at the end of the frame
  (glFinish and the buffer swap: time the CPU spends waiting for the GPU
  or the display).
- Counts: draw calls and triangles through R_DrawElements, views rendered,
  terrain chunks and foliage instances.

Everything is averaged over the last OAX_PROF_AVG frames and handed to the
client (RE_OAXGetProfile); every 30 frames the averages are also published
as debug values r_prof_* for tests. GPU zones need timer queries (desktop
GL 3.3); without them only the CPU side is measured.
===========================================================================
*/

#include "tr_local.h"

cvar_t	*r_oaxProfile;
cvar_t	*r_oaxDirectPost;

#ifndef GL_TIME_ELAPSED
#define GL_TIME_ELAPSED 0x88BF
#endif

#define OAX_PROF_FRAMES		8		// frames of queries in flight
#define OAX_PROF_QUERIES	96		// zone changes recorded per frame
#define OAX_PROF_AVG		60		// frames averaged

static const char *zoneNames[OAX_PROF_ZONES] = {
	"shadow maps", "water reflection", "depth prepass", "shadow mask", "terrain",
	"foliage", "surfaces", "scene copy", "post: MSAA resolve", "post: fog, atmos", "post: bloom",
	"post: tone map", "post: grade, final", "2D / UI", "present", "other"
};

typedef struct {
	float	gpuMs[OAX_PROF_ZONES];
	float	frontMs, backMs, waitMs, frameMs;
	float	draws, tris, views, chunks, foliage;
} profFrame_t;

static struct {
	qboolean	inited, gpuOk;
	GLuint		queries[OAX_PROF_FRAMES][OAX_PROF_QUERIES];
	byte		zoneOf[OAX_PROF_FRAMES][OAX_PROF_QUERIES];
	int			numQueries[OAX_PROF_FRAMES];
	int			cpuFrameOf[OAX_PROF_FRAMES];	// the frame number each query frame's CPU numbers went to
	int			qFrame;					// query frame being recorded
	qboolean	recording;				// qFrame was free: this frame records queries
	int			zone, viewZone;
	qboolean	open;

	// this frame's CPU side
	unsigned	frontUs, backUs, waitUs, lastSwapUs;
	int			draws, tris, views;

	profFrame_t	history[OAX_PROF_AVG];
	qboolean	gpuValid[OAX_PROF_AVG];
	int			frameCount;
	int			published;
} prof;

static qboolean ProfOn( void ) {
	return r_oaxProfile && r_oaxProfile->integer && ri.Microseconds;
}

unsigned R_OAXProfNow( void ) {
	return ri.Microseconds ? ri.Microseconds() : 0;
}

static void ProfInit( void ) {
	prof.inited = qtrue;
	prof.gpuOk = !qglesMajorVersion && qglBeginQuery && qglGetQueryObjectuiv
		&& QGL_VERSION_ATLEAST( 3, 3 );
	if ( prof.gpuOk ) {
		int f;

		for ( f = 0; f < OAX_PROF_FRAMES; f++ ) {
			qglGenQueries( OAX_PROF_QUERIES, prof.queries[f] );
		}
	}
	prof.zone = prof.viewZone = -1;
	prof.recording = qtrue;
	ri.Printf( PRINT_ALL, "oax profiler: CPU timing on, GPU zone timing %s\n", prof.gpuOk ? "on" : "unavailable (no timer queries)" );
}

/*
=================
R_OAXProfShutdown

With the GL context (the queries go with it).
=================
*/
void R_OAXProfShutdown( void ) {
	if ( prof.inited && prof.gpuOk ) {
		int f;

		if ( prof.open ) {
			qglEndQuery( GL_TIME_ELAPSED );
		}
		for ( f = 0; f < OAX_PROF_FRAMES; f++ ) {
			qglDeleteQueries( OAX_PROF_QUERIES, prof.queries[f] );
		}
	}
	Com_Memset( &prof, 0, sizeof( prof ) );
}

/*
=================
RB_OAXProfZone

The GPU work from here on belongs to zone (OAX_PZ_*); inside a view of a
kind that counts as a whole (RB_OAXProfBeginView), to that view's zone.
-1 stops timing until the next zone.
=================
*/
void RB_OAXProfZone( int zone ) {
	int *n;

	if ( !ProfOn() ) {
		return;
	}
	if ( !prof.inited ) {
		ProfInit();
	}
	if ( !prof.gpuOk ) {
		return;
	}
	if ( !prof.recording && !prof.open ) {
		return;
	}
	if ( zone >= 0 && prof.viewZone >= 0 ) {
		zone = prof.viewZone;
	}
	if ( prof.open && zone == prof.zone ) {
		return;
	}
	if ( prof.open ) {
		qglEndQuery( GL_TIME_ELAPSED );
		prof.open = qfalse;
	}
	prof.zone = zone;
	n = &prof.numQueries[prof.qFrame];
	if ( zone < 0 || !prof.recording || *n >= OAX_PROF_QUERIES ) {
		return;
	}
	qglBeginQuery( GL_TIME_ELAPSED, prof.queries[prof.qFrame][*n] );
	prof.zoneOf[prof.qFrame][*n] = zone;
	( *n )++;
	prof.open = qtrue;
}

/*
=================
RB_OAXProfBeginView / RB_OAXProfEndView

Around RB_DrawSurfs: views that are wholly one kind of work.
=================
*/
void RB_OAXProfBeginView( void ) {
	int z = -1;

	if ( !ProfOn() ) {
		return;
	}
	if ( backEnd.viewParms.flags & ( VPF_DEPTHSHADOW | VPF_SHADOWMAP ) ) {
		z = OAX_PZ_SHADOW;
	} else if ( backEnd.viewParms.oaxReflection ) {
		z = OAX_PZ_REFLECT;
	} else if ( ( backEnd.refdef.rdflags & RDF_OAX_SKYPORTAL ) || backEnd.viewParms.targetFbo ) {
		z = OAX_PZ_OTHER;
	}
	prof.viewZone = -1;
	RB_OAXProfZone( z >= 0 ? z : OAX_PZ_PREPASS );
	prof.viewZone = z;
	prof.views++;
}

void RB_OAXProfEndView( void ) {
	prof.viewZone = -1;
	if ( ProfOn() ) {
		RB_OAXProfZone( OAX_PZ_OTHER );
	}
}

/*
=================
R_OAXProfCount

From R_DrawElements.
=================
*/
void R_OAXProfCount( int numIndexes ) {
	prof.draws++;
	prof.tris += numIndexes / 3;
}

void R_OAXProfAddFront( unsigned us ) {
	prof.frontUs += us;
}

void R_OAXProfAddBack( unsigned us ) {
	prof.backUs += us;
}

void R_OAXProfAddWait( unsigned us ) {
	prof.waitUs += us;
}

static void Publish( void ) {
	oaxProfile_t p;
	int i;

	RE_OAXGetProfile( &p );
	if ( !ri.DebugSet || !p.frames ) {
		return;
	}
	ri.DebugSet( "r_prof_frame_ms", va( "%.3f", p.frameMs ) );
	ri.DebugSet( "r_prof_front_ms", va( "%.3f", p.cpuFrontMs ) );
	ri.DebugSet( "r_prof_back_ms", va( "%.3f", p.cpuBackMs ) );
	ri.DebugSet( "r_prof_wait_ms", va( "%.3f", p.cpuWaitMs ) );
	ri.DebugSet( "r_prof_gpu_ms", p.gpuTiming ? va( "%.3f", p.gpuTotalMs ) : "" );
	for ( i = 0; i < OAX_PROF_ZONES; i++ ) {
		ri.DebugSet( va( "r_prof_gpu%d_ms", i ), p.gpuTiming ? va( "%.3f", p.gpuMs[i] ) : "" );
	}
	ri.DebugSet( "r_prof_draws", va( "%.0f", p.draws ) );
	ri.DebugSet( "r_prof_tris", va( "%.0f", p.tris ) );
}

/*
=================
RB_OAXProfEndFrame

From RB_SwapBuffers, before the frame's end: closes the frame's queries,
reads back the oldest frame's if they are ready, and moves on.
=================
*/
void RB_OAXProfEndFrame( void ) {
	profFrame_t *h;
	unsigned now;
	int slot, f, i, chunks = 0, foliage = 0;

	if ( !ProfOn() ) {
		if ( prof.inited ) {
			R_OAXProfShutdown();
		}
		return;
	}
	if ( !prof.inited ) {
		ProfInit();
	}
	RB_OAXProfZone( -1 );

	// this frame's CPU side and counts
	now = R_OAXProfNow();
	slot = prof.frameCount % OAX_PROF_AVG;
	h = &prof.history[slot];
	Com_Memset( h, 0, sizeof( *h ) );
	h->frontMs = prof.frontUs * 0.001f;
	h->backMs = prof.backUs * 0.001f;
	h->waitMs = prof.waitUs * 0.001f;
	h->frameMs = prof.lastSwapUs ? ( now - prof.lastSwapUs ) * 0.001f : 0.0f;
	h->draws = prof.draws;
	h->tris = prof.tris;
	h->views = prof.views;
	R_OAXTerrainStats( &chunks, &foliage );
	h->chunks = chunks;
	h->foliage = foliage;
	prof.gpuValid[slot] = qfalse;
	if ( prof.recording ) {
		prof.cpuFrameOf[prof.qFrame] = prof.frameCount;
	}
	prof.lastSwapUs = now;
	prof.frontUs = prof.backUs = prof.waitUs = 0;
	prof.draws = prof.tris = prof.views = 0;
	prof.frameCount++;

	// every query frame whose results are in: its GPU times go to the
	// history slot its CPU numbers went to, if that slot is still this
	// frame's. Never waits: a GPU running behind just reports later.
	for ( f = 0; f < OAX_PROF_FRAMES && prof.gpuOk; f++ ) {
		GLuint ready = 1;
		int n = prof.numQueries[f];

		if ( !n ) {
			continue;
		}
		qglGetQueryObjectuiv( prof.queries[f][n - 1], GL_QUERY_RESULT_AVAILABLE, &ready );
		if ( !ready ) {
			continue;
		}
		if ( prof.frameCount - prof.cpuFrameOf[f] <= OAX_PROF_AVG ) {
			int oldSlot = prof.cpuFrameOf[f] % OAX_PROF_AVG;
			profFrame_t *old = &prof.history[oldSlot];

			for ( i = 0; i < n; i++ ) {
				GLuint ns = 0;

				qglGetQueryObjectuiv( prof.queries[f][i], GL_QUERY_RESULT, &ns );
				old->gpuMs[prof.zoneOf[f][i]] += ns * 1e-6f;
			}
			prof.gpuValid[oldSlot] = qtrue;
		}
		prof.numQueries[f] = 0;
	}
	// the next frame records into the next query frame only if it is free
	prof.qFrame = ( prof.qFrame + 1 ) % OAX_PROF_FRAMES;
	prof.recording = prof.numQueries[prof.qFrame] == 0;

	if ( ++prof.published >= 30 ) {
		prof.published = 0;
		Publish();
	}
}

/*
=================
RE_OAXGetProfile

The averages over the last OAX_PROF_AVG frames (frames 0: not profiling).
=================
*/
void RE_OAXGetProfile( oaxProfile_t *out ) {
	int n = prof.frameCount < OAX_PROF_AVG ? prof.frameCount : OAX_PROF_AVG;
	int i, z, gpuFrames = 0;

	Com_Memset( out, 0, sizeof( *out ) );
	for ( z = 0; z < OAX_PROF_ZONES; z++ ) {
		out->zoneNames[z] = zoneNames[z];
	}
	if ( !ProfOn() || !n ) {
		return;
	}
	out->frames = n;
	for ( i = 0; i < n; i++ ) {
		const profFrame_t *h = &prof.history[i];

		out->frameMs += h->frameMs;
		out->cpuFrontMs += h->frontMs;
		out->cpuBackMs += h->backMs;
		out->cpuWaitMs += h->waitMs;
		out->draws += h->draws;
		out->tris += h->tris;
		out->views += h->views;
		out->terrainChunks += h->chunks;
		out->foliageInstances += h->foliage;
		if ( prof.gpuValid[i] ) {
			gpuFrames++;
			for ( z = 0; z < OAX_PROF_ZONES; z++ ) {
				out->gpuMs[z] += h->gpuMs[z];
			}
		}
	}
	out->frameMs /= n;
	out->cpuFrontMs /= n;
	out->cpuBackMs /= n;
	out->cpuWaitMs /= n;
	out->draws /= n;
	out->tris /= n;
	out->views /= n;
	out->terrainChunks /= n;
	out->foliageInstances /= n;
	out->gpuTiming = prof.gpuOk && gpuFrames > 0;
	if ( gpuFrames ) {
		for ( z = 0; z < OAX_PROF_ZONES; z++ ) {
			out->gpuMs[z] /= gpuFrames;
			out->gpuTotalMs += out->gpuMs[z];
		}
	}
}
