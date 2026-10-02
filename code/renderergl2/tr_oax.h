/*
===========================================================================
tr_oax.h: renderergl2 side of the oax map features (sky portals, light
styles, view fog, procedural textures and image programs).

Constants only; the prototypes are at the end of tr_local.h, after the
types they use.
===========================================================================
*/
#ifndef TR_OAX_H
#define TR_OAX_H

// refdef flags (mirrored in gamecode oax_public.h)
#define RDF_OAX_SKYPORTAL		0x0100	// this scene is the sky seen through a sky portal
#define RDF_OAX_UNDERSKY		0x0200	// main scene drawn over a sky portal scene: sky is not drawn

// light styles: rgbGen lightstyle <n> / alphaGen lightstyle <n>
#define OAX_MAX_LIGHTSTYLES		64

// procedural textures (tr_procedural.c): one GLSL program per pass type
typedef enum {
	OAX_PROC_PROG_FIRE_STEP,		// heat field step (ping-pong)
	OAX_PROC_PROG_FIRE_COLOR,		// heat field to palette
	OAX_PROC_PROG_WATER_STEP,		// wave equation step (ping-pong)
	OAX_PROC_PROG_WATER_COLOR,		// height field to distorted source
	OAX_PROC_PROG_WET,				// time-varying distortion of a source
	OAX_PROC_PROG_ICE,
	OAX_PROC_PROG_PLASMA,
	OAX_PROC_NUM_PROGRAMS
} oaxProcProgram_t;

#endif
