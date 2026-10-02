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
//
#ifndef __TR_TYPES_H
#define __TR_TYPES_H


#define	MAX_DLIGHTS		32		// can't be increased, because bit flags are used on surfaces

#define	REFENTITYNUM_BITS	10		// can't be increased without changing drawsurf bit packing
#define	REFENTITYNUM_MASK	((1<<REFENTITYNUM_BITS) - 1)
// the last N-bit number (2^REFENTITYNUM_BITS - 1) is reserved for the special world refentity,
//  and this is reflected by the value of MAX_REFENTITIES (which therefore is not a power-of-2)
#define	MAX_REFENTITIES		((1<<REFENTITYNUM_BITS) - 1)
#define	REFENTITYNUM_WORLD	((1<<REFENTITYNUM_BITS) - 1)

// renderfx flags
#define	RF_MINLIGHT		0x0001		// allways have some light (viewmodel, some items)
#define	RF_THIRD_PERSON		0x0002		// don't draw through eyes, only mirrors (player bodies, chat sprites)
#define	RF_FIRST_PERSON		0x0004		// only draw through eyes (view weapon, damage blood blob)
#define	RF_DEPTHHACK		0x0008		// for view weapon Z crunching

#define RF_CROSSHAIR		0x0010		// This item is a cross hair and will draw over everything similar to
						// DEPTHHACK in stereo rendering mode, with the difference that the
						// projection matrix won't be hacked to reduce the stereo separation as
						// is done for the gun.

#define	RF_NOSHADOW		0x0040		// don't add stencil shadows

#define RF_LIGHTING_ORIGIN	0x0080		// use refEntity->lightingOrigin instead of refEntity->origin
						// for lighting.  This allows entities to sink into the floor
						// with their origin going solid, and allows all parts of a
						// player to get the same lighting

#define	RF_SHADOW_PLANE		0x0100		// use refEntity->shadowPlane
#define	RF_WRAP_FRAMES		0x0200		// mod the model frames by the maxframes to allow continuous
										// animation without needing to know the frame count

// refdef flags
#define RDF_NOWORLDMODEL	0x0001		// used for player configuration screen
#define RDF_HYPERSPACE		0x0004		// teleportation effect

typedef struct {
	vec3_t		xyz;
	float		st[2];
	byte		modulate[4];
} polyVert_t;

typedef struct poly_s {
	qhandle_t			hShader;
	int					numVerts;
	polyVert_t			*verts;
} poly_t;

typedef enum {
	RT_MODEL,
	RT_POLY,
	RT_SPRITE,
	RT_BEAM,
	RT_RAIL_CORE,
	RT_RAIL_RINGS,
	RT_LIGHTNING,
	RT_PORTALSURFACE,		// doesn't draw anything, just info for portals

	RT_MAX_REF_ENTITY_TYPE
} refEntityType_t;

typedef struct {
	refEntityType_t	reType;
	int			renderfx;

	qhandle_t	hModel;				// opaque type outside refresh

	// most recent data
	vec3_t		lightingOrigin;		// so multi-part models can be lit identically (RF_LIGHTING_ORIGIN)
	float		shadowPlane;		// projection shadows go here, stencils go slightly lower

	vec3_t		axis[3];			// rotation vectors
	qboolean	nonNormalizedAxes;	// axis are not normalized, i.e. they have scale
	float		origin[3];			// also used as MODEL_BEAM's "from"
	int			frame;				// also used as MODEL_BEAM's diameter

	// previous data for frame interpolation
	float		oldorigin[3];		// also used as MODEL_BEAM's "to"
	int			oldframe;
	float		backlerp;			// 0.0 = current, 1.0 = old

	// texturing
	int			skinNum;			// inline skin index
	qhandle_t	customSkin;			// NULL for default skin
	qhandle_t	customShader;		// use one image for the entire thing

	// misc
	byte		shaderRGBA[4];		// colors used by rgbgen entity shaders
	float		shaderTexCoord[2];	// texture coordinates used by tcMod entity modifiers
	float		shaderTime;			// subtracted from refdef time to control effect start times

	// extra sprite information
	float		radius;
	float		rotation;
} refEntity_t;

// oax extension (qcommon/oax.h): what a refEntity carries beyond the stock
// layout, passed with AddRefEntityToSceneExt. Plain ints and floats, so the
// cgame QVM can build it.
typedef struct {
	int			guiHandle;			// client GUI shown by this entity's "map $gui" stages, 0 = none
	float		shaderParms[12];	// unified lighting (phase 5)
	int			lightDefMask;		// unified lighting (phase 5)
} refEntityExt_t;

// oax effects (qcommon/oax.h CG_OAX_R_ADDFX .. CLEARDECALS). Plain ints and
// floats so the cgame QVM can build them; append-only once shipped.

// oaxFx_t.flags
#define OAXFX_SHADERTIME	0x0001	// startTime/stopTime are on the shader clock (r_fixedShaderTime
									// freezes the system); otherwise the refdef clock (cg.time)

// one particle system instance (a particle decl, particles/*.prt), added
// to the scene every frame it should draw. Particle positions are a pure
// function of these fields, the particle index and the time.
typedef struct {
	int			handle;				// CG_OAX_R_REGISTERFX
	int			startTime;			// ms
	int			stopTime;			// ms, 0 = never: particles born after it are not drawn
	int			seed;				// any int; same seed, same particles
	int			flags;				// OAXFX_*
	float		origin[3];
	float		axis[3][3];			// the decl's x y z; axis[2] is "up" for directions
	float		rgba[4];			// tint, multiplies the stage colours (0 0 0 0 = white)
	float		scale;				// distance and size scale, 0 = 1
} oaxFx_t;

// oaxDecal_t.flags
#define OAXDECAL_ALPHAFADE	0x0001	// fade alpha (blended decals); otherwise fade rgb (mark-style blends)

// a projected decal: the box origin +- halfSize along axis[0..2], projected
// along -axis[0] onto the world and brush-model surfaces inside it
typedef struct {
	int			shader;				// qhandle_t
	float		origin[3];
	float		axis[3][3];			// axis[0] points out of the surface; axis[1], axis[2] are texture s, t
	float		halfSize[3];
	float		rgba[4];
	int			startTime;			// ms (refdef clock)
	int			lifeMs;				// 0 = until the cap pushes it out
	int			fadeMs;				// fades out over the last fadeMs of its life
	int			flags;				// OAXDECAL_*
} oaxDecal_t;

// a ribbon trail through points (x y z ageMs), newest first, facing the viewer
typedef struct {
	int			shader;				// qhandle_t
	int			numPoints;
	int			lifeMs;				// points older than this are dropped; width and colour run head to lifeMs
	float		width[2];			// at age 0, at lifeMs
	float		rgba[2][4];			// at age 0, at lifeMs
	float		texLength;			// world units per texture repeat along the trail, 0 = once over the trail
	int			flags;
} oaxTrail_t;


#define	MAX_RENDER_STRINGS			8
#define	MAX_RENDER_STRING_LENGTH	32

typedef struct {
	int			x, y, width, height;
	float		fov_x, fov_y;
	vec3_t		vieworg;
	vec3_t		viewaxis[3];		// transformation matrix

	// time in milliseconds for shader effects and other time dependent rendering issues
	int			time;

	int			rdflags;			// RDF_NOWORLDMODEL, etc

	// 1 bits will prevent the associated area from rendering at all
	byte		areamask[MAX_MAP_AREA_BYTES];

	// text messages for deform text shaders
	char		text[MAX_RENDER_STRINGS][MAX_RENDER_STRING_LENGTH];
} refdef_t;


typedef enum {
	STEREO_CENTER,
	STEREO_LEFT,
	STEREO_RIGHT
} stereoFrame_t;


/*
** glconfig_t
**
** Contains variables specific to the OpenGL configuration
** being run right now.  These are constant once the OpenGL
** subsystem is initialized.
*/
typedef enum {
	TC_NONE,
	TC_S3TC,  // this is for the GL_S3_s3tc extension.
	TC_S3TC_ARB  // this is for the GL_EXT_texture_compression_s3tc extension.
} textureCompression_t;

typedef enum {
	GLDRV_ICD,					// driver is integrated with window system
								// WARNING: there are tests that check for
								// > GLDRV_ICD for minidriverness, so this
								// should always be the lowest value in this
								// enum set
	GLDRV_STANDALONE,			// driver is a non-3Dfx standalone driver
	GLDRV_VOODOO				// driver is a 3Dfx standalone driver
} glDriverType_t;

typedef enum {
	GLHW_GENERIC,			// where everything works the way it should
	GLHW_3DFX_2D3D,			// Voodoo Banshee or Voodoo3, relevant since if this is
							// the hardware type then there can NOT exist a secondary
							// display adapter
	GLHW_RIVA128,			// where you can't interpolate alpha
	GLHW_RAGEPRO,			// where you can't modulate alpha on alpha textures
	GLHW_PERMEDIA2			// where you don't have src*dst
} glHardwareType_t;

typedef struct {
	char					renderer_string[MAX_STRING_CHARS];
	char					vendor_string[MAX_STRING_CHARS];
	char					version_string[MAX_STRING_CHARS];
	char					extensions_string[BIG_INFO_STRING];

	int						maxTextureSize;			// queried from GL
	int						numTextureUnits;		// multitexture ability

	int						colorBits, depthBits, stencilBits;

	glDriverType_t			driverType;
	glHardwareType_t		hardwareType;

	qboolean				deviceSupportsGamma;
	textureCompression_t	textureCompression;
	qboolean				textureEnvAddAvailable;

	int						vidWidth, vidHeight;
	// aspect is the screen's physical width / height, which may be different
	// than scrWidth / scrHeight if the pixels are non-square
	// normal screens should be 4/3, but wide aspect monitors may be 16/9
	float					windowAspect;

	int						displayFrequency;

	// synonymous with "does rendering consume the entire screen?", therefore
	// a Voodoo or Voodoo2 will have this set to TRUE, as will a Win32 ICD that
	// used CDS.
	qboolean				isFullscreen;
	qboolean				stereoEnabled;
	qboolean				smpActive;		// UNUSED, present for compatibility
} glconfig_t;

#endif	// __TR_TYPES_H
